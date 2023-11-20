/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdbool.h>
#include "reffs/log.h"

void reffs_trace(const char *msg, ...)
{
	va_list ap;
	va_start(ap, msg);
	vfprintf(stdout, msg, ap);
	fprintf(stdout, "\n");
	va_end(ap);
}

static bool reffs_tracing_state = false;

void reffs_tracing_set(void)
{
	reffs_tracing_state = true;
}

void reffs_tracing_clear(void)
{
	reffs_tracing_state = false;
}

bool reffs_tracing_enabled(void)
{
	return reffs_tracing_state;
}
