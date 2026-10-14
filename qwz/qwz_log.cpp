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

#define QWZ_LOG_SIZE	(128 * 1024)

static char qwz_log_ring[QWZ_LOG_SIZE];
static volatile uint64_t qwz_log_head;	/* bytes ever written */
static IOService *qwz_log_target;

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
