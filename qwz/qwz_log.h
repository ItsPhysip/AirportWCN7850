/*
 * qwz_log - the driver's log ring, published in the I/O Registry.
 *
 * Messages from an OpenCore-injected kext do not reach the unified log, and
 * the kernel message buffer wraps quickly on a verbose boot, so every line
 * also goes into a ring that is published on the PCI device together with
 * the current bring-up stage (the approach SimpleMtkWlan uses):
 *
 *   ioreg -l -w0 | grep -E '"QwzMac(Stage|Log|Version)"'
 *
 * Copyright (c) 2026 razvanccr. GPL-2.0-or-later (see LICENSE).
 */
#ifndef QWZ_LOG_H
#define QWZ_LOG_H

#include <stdarg.h>

class IOService;

int	qwz_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int	qwz_vlog(const char *fmt, va_list ap) __attribute__((format(printf, 1, 0)));

/* Where QwzMacLog/QwzMacStage are published: the WCN7850's IOPCIDevice. */
void	qwz_log_set_target(IOService *);
/* Records the current stage and republishes the log. */
void	qwz_log_stage(const char *stage);
int	qwz_stage_hit_limit(void);	/* qwz_compat.cpp: a checkpoint was refused by qwz_stage */
/* Republishes the log ring as QwzMacLog. */
void	qwz_log_publish(void);

#endif /* QWZ_LOG_H */
