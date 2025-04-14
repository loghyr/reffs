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

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-zero-variadic-macro-arguments"
#endif

#define reffs_fail(fmt, ...)                                              \
	do {                                                              \
		fprintf(stderr, "%s() %d: " fmt "\n", __func__, __LINE__, \
			##__VA_ARGS__);                                   \
		abort();                                                  \
	} while (0)

#define reffs_log(fmt, ...)                                               \
	do {                                                              \
		fprintf(stdout, "%s() %d: " fmt "\n", __func__, __LINE__, \
			##__VA_ARGS__);                                   \
	} while (0)

void reffs_trace(const char *function, int line, const char *msg, ...);

// FIXME: Maybe a range of tracing state?
#define REFFS_TRACE_STATE_ENABLED (true)
#define REFFS_TRACE_STATE_DISABLED (false)
void reffs_tracing_set(bool state);
bool reffs_tracing_enabled(void);

#define FAIL(fmt, ...) reffs_fail(fmt, ##__VA_ARGS__)
#define LOG(fmt, ...) reffs_log(fmt, ##__VA_ARGS__)

#define WARN_ONCE(X, fmt, ...)                         \
	do {                                           \
		if (!atomic_flag_test_and_set((X)))    \
			reffs_log(fmt, ##__VA_ARGS__); \
	} while (0)

// FIXME: Expose it as a global to bypass a function call
#define TRACE(fmt, ...)                                \
	do {                                           \
		if (reffs_tracing_enabled())           \
			reffs_log(fmt, ##__VA_ARGS__); \
	} while (0)

#ifdef __clang__
#pragma clang diagnostic pop
#endif

#endif /* _REFFS_LOG_H */
