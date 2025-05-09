/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifndef _REFFS_LOG_H
#define _REFFS_LOG_H

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <sys/syscall.h>

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-zero-variadic-macro-arguments"
#endif

#define reffs_fail(fmt, ...)                                             \
	do {                                                             \
		struct timespec ts;                                      \
		clock_gettime(CLOCK_REALTIME, &ts);                      \
		struct tm *tm_info = localtime(&ts.tv_sec);              \
		char time_str[32];                                       \
		pid_t tid = syscall(SYS_gettid);                         \
		strftime(time_str, 20, "%Y-%m-%d %H:%M:%S", tm_info);    \
		fprintf(stderr, "[%s.%09ld] [%d:%d] (%s:%d): " fmt "\n", \
			time_str, ts.tv_nsec, getpid(), tid, __func__,   \
			__LINE__, ##__VA_ARGS__);                        \
		abort();                                                 \
	} while (0)

#define reffs_log(fmt, ...)                                              \
	do {                                                             \
		struct timespec ts;                                      \
		clock_gettime(CLOCK_REALTIME, &ts);                      \
		struct tm *tm_info = localtime(&ts.tv_sec);              \
		char time_str[32];                                       \
		pid_t tid = syscall(SYS_gettid);                         \
		strftime(time_str, 20, "%Y-%m-%d %H:%M:%S", tm_info);    \
		fprintf(stdout, "[%s.%09ld] [%d:%d] (%s:%d): " fmt "\n", \
			time_str, ts.tv_nsec, getpid(), tid, __func__,   \
			__LINE__, ##__VA_ARGS__);                        \
	} while (0)

void reffs_trace(const char *function, int line, const char *msg, ...);

#define FAIL(fmt, ...) reffs_fail(fmt, ##__VA_ARGS__)
#define LOG(fmt, ...) reffs_log(fmt, ##__VA_ARGS__)

#define WARN_ONCE(X, fmt, ...)                         \
	do {                                           \
		if (!atomic_flag_test_and_set((X)))    \
			reffs_log(fmt, ##__VA_ARGS__); \
	} while (0)

// FIXME: Expose it as a global to bypass a function call
#define TRACE(fmt, ...)                        \
	do {                                   \
		reffs_log(fmt, ##__VA_ARGS__); \
	} while (0)

#ifdef __clang__
#pragma clang diagnostic pop
#endif

#endif /* _REFFS_LOG_H */
