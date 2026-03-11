/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_LOG_H
#define _REFFS_LOG_H

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <time.h>
#include <sys/syscall.h>

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-zero-variadic-macro-arguments"
#endif

extern FILE *reffs_log_file;

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

#define reffs_log(fmt, ...)                                                \
	do {                                                               \
		struct timespec ts;                                        \
		clock_gettime(CLOCK_REALTIME, &ts);                        \
		struct tm *tm_info = localtime(&ts.tv_sec);                \
		char time_str[32];                                         \
		pid_t tid = syscall(SYS_gettid);                           \
		strftime(time_str, 20, "%Y-%m-%d %H:%M:%S", tm_info);      \
		fprintf(reffs_log_file ? reffs_log_file : stdout,          \
			"[%s.%09ld] [%d:%d] (%s:%d): " fmt "\n", time_str, \
			ts.tv_nsec, getpid(), tid, __func__, __LINE__,     \
			##__VA_ARGS__);                                    \
		reffs_trace_event(REFFS_TRACE_CAT_LOG, __func__, __LINE__, \
				  fmt, ##__VA_ARGS__);                     \
	} while (0)

void reffs_trace(const char *function, int line, const char *msg, ...);

#define FAIL(fmt, ...) reffs_fail(fmt, ##__VA_ARGS__)
#define LOG(fmt, ...) reffs_log(fmt, ##__VA_ARGS__)

#define WARN_ONCE(X, fmt, ...)                         \
	do {                                           \
		if (!atomic_flag_test_and_set((X)))    \
			reffs_log(fmt, ##__VA_ARGS__); \
	} while (0)

// Use reffs_trace_event for high-volume events to avoid console spam
#include "reffs/trace/common.h"
#define TRACE(fmt, ...)                                                \
	reffs_trace_event(REFFS_TRACE_CAT_FS, __func__, __LINE__, fmt, \
			  ##__VA_ARGS__)

// TRC is for high-priority trace events that should always be emitted to the trace file
#define TRC(fmt, ...)                                                  \
	reffs_trace_event(REFFS_TRACE_CAT_FS, __func__, __LINE__, fmt, \
			  ##__VA_ARGS__)

#ifdef __clang__
#pragma clang diagnostic pop
#endif

#endif /* _REFFS_LOG_H */
