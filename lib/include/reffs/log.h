/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_LOG_H
#define _REFFS_LOG_H

#include <errno.h>

/*
 * Portability shim: Linux-only errno codes used throughout reffs.
 *
 * EREMOTEIO and ENODATA are glibc-specific.  FreeBSD's ELAST is 97
 * (EINTEGRITY), so we assign reffs-internal values well above that
 * range — distinct from every POSIX/FreeBSD errno — so internal
 * comparisons like `if (ret == -EREMOTEIO)` continue to work.
 *
 * These values are reffs-internal only: if an error ever needs to
 * cross a boundary (syscall return, NFS wire, kernel), coerce it to
 * a POSIX errno (EIO for EREMOTEIO, ENOATTR for ENODATA).
 */
#ifndef EREMOTEIO
#define EREMOTEIO 198
#endif
#ifndef ENODATA
#define ENODATA   199
#endif

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <time.h>

#if defined(__linux__)
#include <sys/syscall.h>
#elif defined(__FreeBSD__)
#include <pthread_np.h>
#endif

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-zero-variadic-macro-arguments"
#endif

/*
 * Portable kernel thread id accessor.  Linux: syscall(SYS_gettid).
 * FreeBSD: pthread_getthreadid_np(), a non-portable extension.  Both
 * return a small integer that uniquely identifies the kernel thread
 * within the process; suitable for log/trace prefixes.
 */
static inline pid_t reffs_gettid(void)
{
#if defined(__linux__)
	return (pid_t)syscall(SYS_gettid);
#elif defined(__FreeBSD__)
	return (pid_t)pthread_getthreadid_np();
#else
	return (pid_t)getpid();
#endif
}

extern FILE *reffs_log_file;

#define reffs_fail(fmt, ...)                                             \
	do {                                                             \
		struct timespec ts;                                      \
		clock_gettime(CLOCK_REALTIME, &ts);                      \
		struct tm *tm_info = localtime(&ts.tv_sec);              \
		char time_str[32];                                       \
		pid_t tid = reffs_gettid();                         \
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
		pid_t tid = reffs_gettid();                           \
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
