/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifndef _REFFS_LOG_H
#define _REFFS_LOG_H

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdatomic.h>

static inline void reffs_fail(const char *function, int line, const char *msg,
			      ...)
{
	va_list ap;
	va_start(ap, msg);
	fprintf(stderr, "%s:%d ", function, line);
	vfprintf(stderr, msg, ap);
	fprintf(stderr, "\n");
	va_end(ap);
	abort();
}

static inline void reffs_log(const char *function, int line, const char *msg,
			     ...)
{
	va_list ap;
	va_start(ap, msg);
	fprintf(stdout, "%s:%d ", function, line);
	vfprintf(stdout, msg, ap);
	fprintf(stdout, "\n");
	va_end(ap);
}

#define FAIL(...) reffs_fail(__func__, __LINE__, __VA_ARGS__)
#define LOG(...) reffs_log(__func__, __LINE__, __VA_ARGS__)

#define WARN_ONCE(X, ...)                                           \
	do {                                                        \
		if (!atomic_flag_test_and_set((X)))                 \
			reffs_log(__func__, __LINE__, __VA_ARGS__); \
	} while (0)

#endif /* _REFFS_LOG_H */
