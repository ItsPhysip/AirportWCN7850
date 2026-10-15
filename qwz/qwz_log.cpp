/*
 * qwz_log - the driver's log ring, published in the I/O Registry.
 * See qwz_log.h.
 *
 * Copyright (c) 2026 razvanccr. GPL-2.0-or-later (see LICENSE).
 */
#include "qwz_log.h"

#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSString.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <sys/vnode.h>
#include <sys/uio.h>
#include <sys/fcntl.h>
#include <sys/mount.h>

#define QWZ_LOG_SIZE	(512 * 1024)

static char qwz_log_ring[QWZ_LOG_SIZE];
static volatile uint64_t qwz_log_head;	/* bytes ever written */
static IOService *qwz_log_target;

static vnode_t qwz_log_vp;
static vfs_context_t qwz_log_ctx;
static thread_t qwz_log_file_thread;

static void
qwz_log_file_write(const char *line, int len)
{
	uio_t uio;

	if (qwz_log_vp == NULL || current_thread() != qwz_log_file_thread)
		return;
	uio = uio_create(1, 0, UIO_SYSSPACE, UIO_WRITE);
	if (uio == NULL)
		return;
	uio_addiov(uio, CAST_USER_ADDR_T(line), len);
	VNOP_WRITE(qwz_log_vp, uio, IO_APPEND | IO_SYNC, qwz_log_ctx);
	uio_free(uio);
	VNOP_FSYNC(qwz_log_vp, MNT_WAIT, qwz_log_ctx);
	/* As fcntl(F_FULLFSYNC): make the drive flush its own write cache too. */
	VNOP_IOCTL(qwz_log_vp, F_FULLFSYNC, (caddr_t)NULL, 0, qwz_log_ctx);
}

void
qwz_log_file_begin(void)
{
	if (qwz_log_vp != NULL)
		return;
	qwz_log_ctx = vfs_context_create(NULL);
	if (qwz_log_ctx == NULL)
		return;
	if (vnode_open(QWZ_LOG_FILE, O_CREAT | O_APPEND | FWRITE, 0644, 0,
	    &qwz_log_vp, qwz_log_ctx) != 0) {
		qwz_log_vp = NULL;
		vfs_context_rele(qwz_log_ctx);
		qwz_log_ctx = NULL;
		IOLog("qwz: cannot open %s\n", QWZ_LOG_FILE);
		return;
	}
	qwz_log_file_thread = current_thread();
}

void
qwz_log_file_end(void)
{
	if (qwz_log_vp == NULL)
		return;
	qwz_log_file_thread = NULL;
	vnode_close(qwz_log_vp, FWRITE, qwz_log_ctx);
	qwz_log_vp = NULL;
	vfs_context_rele(qwz_log_ctx);
	qwz_log_ctx = NULL;
}

int
qwz_vlog(const char *fmt, va_list ap)
{
	char line[384];
	uint64_t ns, start;
	int head, len, i;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	head = snprintf(line, sizeof(line), "[%5llu.%03llu] ",
	    (unsigned long long)(ns / 1000000000ULL),
	    (unsigned long long)(ns / 1000000ULL % 1000));
	if (head < 0 || (size_t)head >= sizeof(line))
		head = 0;
	len = vsnprintf(line + head, sizeof(line) - head, fmt, ap);
	if (len < 0)
		return len;
	len += head;
	if ((size_t)len >= sizeof(line))
		len = sizeof(line) - 1;
	IOLog("%s", line);
	qwz_log_file_write(line, len);

	/* Concurrent writers reserve disjoint ranges, so lines never interleave. */
	start = __atomic_fetch_add(&qwz_log_head, (uint64_t)len, __ATOMIC_RELAXED);
	for (i = 0; i < len; i++)
		qwz_log_ring[(start + i) % QWZ_LOG_SIZE] = line[i];
	return len;
}

int
qwz_log(const char *fmt, ...)
{
	va_list ap;
	int r;

	va_start(ap, fmt);
	r = qwz_vlog(fmt, ap);
	va_end(ap);
	return r;
}

void
qwz_log_set_target(IOService *target)
{
	qwz_log_target = target;
	if (target != NULL)
		target->setProperty("QwzMacVersion", QWZMAC_VERSION);
}

void
qwz_log_publish(void)
{
	uint64_t head = __atomic_load_n(&qwz_log_head, __ATOMIC_RELAXED);
	size_t avail = head < QWZ_LOG_SIZE ? (size_t)head : QWZ_LOG_SIZE, i, skip = 0;
	uint64_t first = head - avail;
	char *buf;
	OSString *str;

	if (qwz_log_target == NULL)
		return;
	buf = (char *)IOMalloc(avail + 1);
	if (buf == NULL)
		return;
	for (i = 0; i < avail; i++)
		buf[i] = qwz_log_ring[(first + i) % QWZ_LOG_SIZE];
	buf[avail] = 0;
	if (head > QWZ_LOG_SIZE) {	/* drop the partial first line after a wrap */
		while (skip < avail && buf[skip] != '\n')
			skip++;
		if (skip < avail)
			skip++;
	}
	str = OSString::withCString(buf + skip);
	if (str != NULL) {
		qwz_log_target->setProperty("QwzMacLog", str);
		str->release();
	}
	IOFree(buf, avail + 1);
}

void
qwz_log_stage(const char *stage)
{
	if (qwz_log_target != NULL)
		qwz_log_target->setProperty("QwzMacStage", stage);
	qwz_log("qwz: stage: %s\n", stage);
	qwz_log_publish();
}
