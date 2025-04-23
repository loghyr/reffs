/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdbool.h>
#include "reffs/log.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

#define REFFS_OUTPUT_BUFFER 1024

static enum reffs_trace_level reffs_trace_level = REFFS_TRACE_LEVEL_DISABLED;

void reffs_tracing_set(enum reffs_trace_level level)
{
	reffs_trace_level = level;
}

bool reffs_tracing_enabled(enum reffs_trace_level level)
{
	return level >= reffs_trace_level;
}
