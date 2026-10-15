//
//  _task.cpp
//  itlwm
//
//  Created by qcwap on 2020/3/1.
//  Copyright © 2020 钟先耀. All rights reserved.
//

/*
* Copyright (C) 2020  钟先耀
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*/

#include <sys/_task.h>
#include <sys/proc.h>

#include <IOKit/IOLib.h>
#include <IOKit/IOCommandGate.h>
#include <kern/thread_call.h>
#include "qwz_log.h"

/*
 * QwzMac: the task queues run on thread calls instead of dedicated kernel
 * threads. On the test machine, starting itl80211's original worker thread
 * (parked in IORecursiveLockSleep) reset the computer, while thread calls
 * work. A queue has one thread call; only one invocation is ever scheduled
 * or running, so tasks on a queue still run one at a time, in order. Each
 * task runs holding the controller's gate, as OpenBSD runs tasks holding the
 * kernel lock; a task may tsleep(), which releases the gate while it waits.
 */

extern IOCommandGate *_fCommandGate;

struct taskq {
    const char       *tq_name;
    IOLock           *tq_mtx;
    thread_call_t     tq_call;
    bool              tq_scheduled;   /* tq_call entered or running */
    bool              tq_dying;
    struct task_list  tq_worklist;
};

static struct taskq taskq_sys;
struct taskq *const systq = &taskq_sys;

static IOReturn
taskq_run_gated(OSObject *, void *arg0, void *, void *, void *)
{
    struct task *work = (struct task *)arg0;

    (*work->t_func)(work->t_arg);
    return kIOReturnSuccess;
}

static void
taskq_run(thread_call_param_t p0, thread_call_param_t)
{
    struct taskq *tq = (struct taskq *)p0;
    struct task *next, work;

    for (;;) {
        IOLockLock(tq->tq_mtx);
        next = TAILQ_FIRST(&tq->tq_worklist);
        if (next == NULL || tq->tq_dying) {
            tq->tq_scheduled = false;
            IOLockWakeup(tq->tq_mtx, tq, false);
            IOLockUnlock(tq->tq_mtx);
            return;
        }
        TAILQ_REMOVE(&tq->tq_worklist, next, t_entry);
        CLR(next->t_flags, TASK_ONQUEUE);
        work = *next;   /* copy: the owner may re-add or free it while it runs */
        IOLockUnlock(tq->tq_mtx);

        if (_fCommandGate != NULL)
            _fCommandGate->runAction(taskq_run_gated, &work);
        else
            (*work.t_func)(work.t_arg);
    }
}

static bool
taskq_setup(struct taskq *tq, const char *name)
{
    tq->tq_name = name;
    tq->tq_scheduled = false;
    tq->tq_dying = false;
    TAILQ_INIT(&tq->tq_worklist);
    tq->tq_mtx = IOLockAlloc();
    if (tq->tq_mtx == NULL)
        return false;
    tq->tq_call = thread_call_allocate(taskq_run, tq);
    if (tq->tq_call == NULL) {
        IOLockFree(tq->tq_mtx);
        tq->tq_mtx = NULL;
        return false;
    }
    return true;
}

void
taskq_init(void)
{
    if (systq->tq_mtx == NULL && !taskq_setup(systq, "systq"))
        IOLog("itlwm: taskq: cannot set up systq\n");
}

struct taskq *
taskq_create(const char *name, unsigned int nthreads, int ipl,
             unsigned int flags)
{
    struct taskq *tq = (struct taskq *)IOMalloc(sizeof(*tq));

    if (tq == NULL)
        return NULL;
    bzero(tq, sizeof(*tq));
    if (!taskq_setup(tq, name)) {
        IOFree(tq, sizeof(*tq));
        return NULL;
    }
    return tq;
}

/* Wait until nothing is scheduled or running on the queue. */
void
taskq_barrier(struct taskq *tq)
{
    if (tq == NULL || tq->tq_mtx == NULL)
        return;
    IOLockLock(tq->tq_mtx);
    while (tq->tq_scheduled)
        IOLockSleep(tq->tq_mtx, tq, THREAD_UNINT);
    IOLockUnlock(tq->tq_mtx);
}

void
taskq_destroy(struct taskq *tq)
{
    if (tq == NULL || tq->tq_mtx == NULL)
        return;
    IOLockLock(tq->tq_mtx);
    tq->tq_dying = true;
    IOLockUnlock(tq->tq_mtx);
    if (thread_call_cancel(tq->tq_call)) {
        IOLockLock(tq->tq_mtx);
        tq->tq_scheduled = false;
        IOLockUnlock(tq->tq_mtx);
    }
    taskq_barrier(tq);
    thread_call_free(tq->tq_call);
    IOLockFree(tq->tq_mtx);
    if (tq != systq)
        IOFree(tq, sizeof(*tq));
    else
        tq->tq_mtx = NULL;
}

void
taskq_del_barrier(struct taskq *tq, struct task *t)
{
    task_del(tq, t);
    taskq_barrier(tq);
}

void
task_set(struct task *t, void (*fn)(void *), void *arg, const char *name)
{
    t->t_func = fn;
    t->t_arg = arg;
    t->t_flags = 0;
    strlcpy(t->name, name, sizeof(t->name));
}

int
task_add(struct taskq *tq, struct task *w)
{
    int rv = 0;
    bool enter = false;

    if (tq == NULL || tq->tq_mtx == NULL || ISSET(w->t_flags, TASK_ONQUEUE))
        return 0;
    IOLockLock(tq->tq_mtx);
    if (!tq->tq_dying && !ISSET(w->t_flags, TASK_ONQUEUE)) {
        rv = 1;
        SET(w->t_flags, TASK_ONQUEUE);
        TAILQ_INSERT_TAIL(&tq->tq_worklist, w, t_entry);
        if (!tq->tq_scheduled) {
            tq->tq_scheduled = true;
            enter = true;
        }
    }
    IOLockUnlock(tq->tq_mtx);
    if (enter)
        thread_call_enter(tq->tq_call);
    return rv;
}

int
task_del(struct taskq *tq, struct task *w)
{
    int rv = 0;

    if (tq == NULL || tq->tq_mtx == NULL || !ISSET(w->t_flags, TASK_ONQUEUE))
        return 0;
    IOLockLock(tq->tq_mtx);
    if (ISSET(w->t_flags, TASK_ONQUEUE)) {
        rv = 1;
        TAILQ_REMOVE(&tq->tq_worklist, w, t_entry);
        CLR(w->t_flags, TASK_ONQUEUE);
    }
    IOLockUnlock(tq->tq_mtx);
    return rv;
}
