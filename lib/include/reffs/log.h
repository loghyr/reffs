/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifndef _REFFS_LOG_H
#define _REFFS_LOG_H

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdatomic.h>

#define REFFS_OUTPUT_BUFFER (1024)

static inline void reffs_log(const char *function, int line, const char *msg,
			     ...)
{
	va_list ap;
	va_start(ap, msg);

	char formatted_msg[REFFS_OUTPUT_BUFFER];
	snprintf(formatted_msg, sizeof(formatted_msg), "%s:%d %s\n", function,
		 line, msg);

	va_list ap_copy;
	va_copy(ap_copy, ap);

	fprintf(stderr, "%s", formatted_msg);

	va_end(ap_copy);
	va_end(ap);
}

static inline void reffs_fail(const char *function, int line, const char *msg,
			      ...)
{
	va_list ap;
	va_start(ap, msg);

	reffs_log(function, line, msg, ap);

	va_end(ap);
	abort();
}

void reffs_trace(const char *function, int line, const char *msg, ...);

// FIXME: Maybe a range of tracing state?
#define REFFS_TRACE_STATE_ENABLED (true)
#define REFFS_TRACE_STATE_DISABLED (false)
void reffs_tracing_set(bool state);
bool reffs_tracing_enabled(void);

#define FAIL(...) reffs_fail(__func__, __LINE__, __VA_ARGS__)
#define LOG(...) reffs_log(__func__, __LINE__, __VA_ARGS__)

#define WARN_ONCE(X, ...)                                           \
	do {                                                        \
		if (!atomic_flag_test_and_set((X)))                 \
			reffs_log(__func__, __LINE__, __VA_ARGS__); \
	} while (0)

// FIXME: Expose it as a global to bypass a function call
#define TRACE(...)                                                    \
	do {                                                          \
		if (reffs_tracing_enabled())                          \
			reffs_trace(__func__, __LINE__, __VA_ARGS__); \
	} while (0)

#endif /* _REFFS_LOG_H */
