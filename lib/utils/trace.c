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

	char formatted_msg[REFFS_OUTPUT_BUFFER];
	snprintf(formatted_msg, sizeof(formatted_msg), "%s:%d %s\n", function,
		 line, msg);

	va_list ap_copy;
	va_copy(ap_copy, ap);

	fprintf(stdout, "%s", formatted_msg);

	va_end(ap_copy);
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
