/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <sys/syscall.h>
#include <inttypes.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdlib.h>
#include <errno.h>
#include "reffs/trace/common.h"
#include "reffs/trace/types.h"

/* Global trace state */
static pthread_mutex_t trace_mutex = PTHREAD_MUTEX_INITIALIZER;
static FILE *trace_fp = NULL;
static bool category_enabled[REFFS_TRACE_CAT_ALL] = { false, false, true, true,
						      false };

/* Initialize tracing */
void reffs_trace_init(const char *filename)
{
	pthread_mutex_lock(&trace_mutex);
	if (trace_fp == NULL && filename != NULL) {
		trace_fp =
			fopen(filename,
			      "w"); // For right now, with a RAM backed storage
	}
	if (trace_fp == NULL) {
		trace_fp = stderr;
	}
	pthread_mutex_unlock(&trace_mutex);
}

/* Close trace file */
void reffs_trace_close(void)
{
	pthread_mutex_lock(&trace_mutex);
	if (trace_fp != NULL && trace_fp != stderr) {
		fclose(trace_fp);
		trace_fp = NULL;
	}
	pthread_mutex_unlock(&trace_mutex);
}

/* Enable a trace category */
void reffs_trace_enable_category(enum reffs_trace_category category)
{
	if (category < REFFS_TRACE_CAT_ALL) {
		pthread_mutex_lock(&trace_mutex);
		category_enabled[category] = true;
		pthread_mutex_unlock(&trace_mutex);
	}
}

/* Disable a trace category */
void reffs_trace_disable_category(enum reffs_trace_category category)
{
	if (category < REFFS_TRACE_CAT_ALL) {
		pthread_mutex_lock(&trace_mutex);
		category_enabled[category] = false;
		pthread_mutex_unlock(&trace_mutex);
	}
}

/* Check if a category is enabled */
bool reffs_trace_is_category_enabled(enum reffs_trace_category category)
{
	if (category < REFFS_TRACE_CAT_ALL) {
		return category_enabled[category];
	}
	return false;
}

/* Check if trace should be shown */
bool reffs_should_trace(enum reffs_trace_category category)
{
	return (category < REFFS_TRACE_CAT_ALL && category_enabled[category]);
}

#define MAX_TRACE_SIZE (512 * 1024 * 1024)
#define BASE_TRACE_NAME "trace.log"

static off_t trace_bytes_written = 0;
static int trace_file_index = 0;

static void rotate_trace_if_needed_locked(void)
{
	if (trace_fp == NULL || trace_fp == stderr)
		return;

	// Check size
	if (trace_bytes_written < MAX_TRACE_SIZE)
		return;

	char rotated_name[256];
	snprintf(rotated_name, sizeof(rotated_name), "trace-%d.log",
		 trace_file_index++);

	fclose(trace_fp);
	trace_fp = NULL;

	// Rename the old trace
	if (rename(BASE_TRACE_NAME, rotated_name) != 0) {
		fprintf(stderr, "Failed to rotate trace log: %s\n",
			strerror(errno));
		return;
	}

	// Launch zstd in the background
	pid_t pid = fork();
	if (pid == 0) {
		// Child process
		execlp("zstd", "zstd", rotated_name, NULL);
		_exit(1); // If execlp fails
	}

	// Reopen a new trace.log
	trace_fp = fopen(BASE_TRACE_NAME, "w");
	if (!trace_fp) {
		trace_fp = stderr;
	}

	trace_bytes_written = 0;
}

__thread struct timespec last_event_ts = {0};

void reffs_trace_event(enum reffs_trace_category category, const char *name,
		       const int line, const char *format, ...)
{
	if (!reffs_should_trace(category)) {
		return;
	}

	struct timespec ts;
	char time_str[32];
	va_list args;

	clock_gettime(CLOCK_REALTIME, &ts);
	struct tm *tm_info = localtime(&ts.tv_sec);
	strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);

	uint64_t epoch_ns = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;

	// Compute time delta in microseconds
	uint64_t delta_us = 0;
	if (last_event_ts.tv_sec != 0) {
		delta_us = (ts.tv_sec - last_event_ts.tv_sec) * 1000000ULL +
			   (ts.tv_nsec - last_event_ts.tv_nsec) / 1000ULL;
	}
	last_event_ts = ts;

	pid_t tid = syscall(SYS_gettid);

	pthread_mutex_lock(&trace_mutex);
	if (trace_fp != NULL) {
		int n = fprintf(trace_fp,
				"[%s.%09ld] [epoch_ns=%" PRIu64 "] [Δ+%6" PRIu64
				"us] [%d:%d] (%s:%d): ",
				time_str, ts.tv_nsec, epoch_ns, delta_us,
				getpid(), tid, name, line);

		va_start(args, format);
		n += vfprintf(trace_fp, format, args);
		va_end(args);

		n += fprintf(trace_fp, "\n");
		fflush(trace_fp);

		trace_bytes_written += n;
		rotate_trace_if_needed_locked();
	}
	pthread_mutex_unlock(&trace_mutex);
}

