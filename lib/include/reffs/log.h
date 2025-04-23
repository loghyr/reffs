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

#define reffs_fail(fmt, ...)                                                   \
	do {                                                                   \
		struct timespec ts;                                            \
		clock_gettime(CLOCK_REALTIME, &ts);                            \
		time_t now = ts.tv_sec;                                        \
		struct tm *tm_info = localtime(&now);                          \
		char time_str[32];                                             \
		strftime(time_str, 20, "%Y-%m-%d %H:%M:%S", tm_info);          \
		fprintf(stderr, "[%s.%06ld] %s() %d: " fmt "\n", time_str,     \
			ts.tv_nsec / 1000, __func__, __LINE__, ##__VA_ARGS__); \
		abort();                                                       \
	} while (0)

#define reffs_log(fmt, ...)                                                    \
	do {                                                                   \
		struct timespec ts;                                            \
		clock_gettime(CLOCK_REALTIME, &ts);                            \
		time_t now = ts.tv_sec;                                        \
		struct tm *tm_info = localtime(&now);                          \
		char time_str[32];                                             \
		strftime(time_str, 20, "%Y-%m-%d %H:%M:%S", tm_info);          \
		fprintf(stdout, "[%s.%06ld] %s() %d: " fmt "\n", time_str,     \
			ts.tv_nsec / 1000, __func__, __LINE__, ##__VA_ARGS__); \
	} while (0)

void reffs_trace(const char *function, int line, const char *msg, ...);

enum reffs_trace_level {
	REFFS_TRACE_LEVEL_DEBUG = 0,
	REFFS_TRACE_LEVEL_INFO = 1,
	REFFS_TRACE_LEVEL_NOTICE = 2,
	REFFS_TRACE_LEVEL_WARNING = 3,
	REFFS_TRACE_LEVEL_ERR = 4,
	REFFS_TRACE_LEVEL_DISABLED = 5
};

void reffs_tracing_set(enum reffs_trace_level level);
bool reffs_tracing_enabled(enum reffs_trace_level level);

#define FAIL(fmt, ...) reffs_fail(fmt, ##__VA_ARGS__)
#define LOG(fmt, ...) reffs_log(fmt, ##__VA_ARGS__)

#define WARN_ONCE(X, fmt, ...)                         \
	do {                                           \
		if (!atomic_flag_test_and_set((X)))    \
			reffs_log(fmt, ##__VA_ARGS__); \
	} while (0)

// FIXME: Expose it as a global to bypass a function call
#define TRACE(level, fmt, ...)                                \
	do {                                           \
		if (reffs_tracing_enabled(level))           \
			reffs_log(fmt, ##__VA_ARGS__); \
	} while (0)

#ifdef __clang__
#pragma clang diagnostic pop
#endif

#endif /* _REFFS_LOG_H */
