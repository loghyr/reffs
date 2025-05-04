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
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <string.h>
#include "reffs/trace/trace.h"
#include "reffs/trace/types.h"

/* Global trace state */
static pthread_mutex_t trace_mutex = PTHREAD_MUTEX_INITIALIZER;
static FILE *trace_fp = NULL;
static bool category_enabled[REFFS_TRACE_CAT_MAX] = { false, false, true, true,
						      false };

/* Initialize tracing */
void reffs_trace_init(const char *filename)
{
	pthread_mutex_lock(&trace_mutex);
	if (trace_fp == NULL && filename != NULL) {
		trace_fp = fopen(filename, "a");
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
	if (category < REFFS_TRACE_CAT_MAX) {
		pthread_mutex_lock(&trace_mutex);
		category_enabled[category] = true;
		pthread_mutex_unlock(&trace_mutex);
	}
}

/* Disable a trace category */
void reffs_trace_disable_category(enum reffs_trace_category category)
{
	if (category < REFFS_TRACE_CAT_MAX) {
		pthread_mutex_lock(&trace_mutex);
		category_enabled[category] = false;
		pthread_mutex_unlock(&trace_mutex);
	}
}

/* Check if a category is enabled */
bool reffs_trace_is_category_enabled(enum reffs_trace_category category)
{
	if (category < REFFS_TRACE_CAT_MAX) {
		return category_enabled[category];
	}
	return false;
}

/* Check if trace should be shown */
bool reffs_should_trace(enum reffs_trace_category category)
{
	return (category < REFFS_TRACE_CAT_MAX && category_enabled[category]);
}

/* Write trace event */
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

	pthread_mutex_lock(&trace_mutex);
	if (trace_fp != NULL) {
		fprintf(trace_fp, "[%s.%09ld] [%d] (%s:%d): ", time_str,
			ts.tv_nsec, getpid(), name, line);

		va_start(args, format);
		vfprintf(trace_fp, format, args);
		va_end(args);

		fprintf(trace_fp, "\n");
		fflush(trace_fp);
	}
	pthread_mutex_unlock(&trace_mutex);
}
