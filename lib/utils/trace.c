/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdbool.h>
#include "reffs/log.h"

void reffs_trace(const char *function, int line, const char *msg, ...)
{
	va_list ap;
	va_start(ap, msg);
	fprintf(stdout, "%s:%d ", function, line);
	vfprintf(stdout, msg, ap);
	fprintf(stdout, "\n");
	va_end(ap);
}

static bool reffs_tracing_state = false;

void reffs_tracing_set(bool state)
{
	reffs_tracing_state = state;
}

bool reffs_tracing_enabled(void)
{
	return reffs_tracing_state;
}
