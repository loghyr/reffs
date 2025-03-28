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

static bool reffs_tracing_state = false;

void reffs_tracing_set(bool state)
{
	reffs_tracing_state = state;
}

bool reffs_tracing_enabled(void)
{
	return reffs_tracing_state;
}
