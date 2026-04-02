/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
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
#include <zstd.h>
#include "reffs/trace/common.h"
#include "reffs/trace/types.h"

#define MAX_TRACE_SIZE (512 * 1024 * 1024)
#define MAX_TRACE_QUEUE 64

static const char *reffs_trace_name;

static off_t trace_bytes_written = 0;

/* Global trace state */
static pthread_mutex_t trace_mutex = PTHREAD_MUTEX_INITIALIZER;
static FILE *trace_fp = NULL;
#ifdef ENABLE_ALL_TRACE_CATEGORIES
static bool category_enabled[REFFS_TRACE_CAT_ALL] = { true, true, true,
						      true, true, true,
						      true, true, true };
#else
static bool category_enabled[REFFS_TRACE_CAT_ALL] = { false, true,  true,
						      true,  true,  false,
						      true,  false, false };
#endif

static const char *category_names[] = { "GENERAL", "IO",       "RPC",
					"NFS",	   "NLM",      "FS",
					"LOG",	   "SECURITY", "LIFECYCLE" };

static char *trace_compress_queue[MAX_TRACE_QUEUE];
static int trace_compress_head = 0;
static int trace_compress_tail = 0;
static pthread_mutex_t trace_compress_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t trace_compress_cond = PTHREAD_COND_INITIALIZER;
static pthread_t trace_compress_tid;

static void compress_trace_file_inline(char *input_path);

static void format_timestamped_name(char *buf, size_t len)
{
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	strftime(buf, len, "trace-%Y%m%d-%H%M%S.log", &tm);
}

static void *trace_compress_thread(void __attribute__((unused)) * arg)
{
	while (1) {
		pthread_mutex_lock(&trace_compress_mutex);
		while (trace_compress_head == trace_compress_tail) {
			pthread_cond_wait(&trace_compress_cond,
					  &trace_compress_mutex);
		}

		char *input_path = trace_compress_queue[trace_compress_tail];
		trace_compress_tail =
			(trace_compress_tail + 1) % MAX_TRACE_QUEUE;
		pthread_mutex_unlock(&trace_compress_mutex);

		if (input_path) {
			compress_trace_file_inline(input_path);
			free(input_path);
		}
	}
	return NULL;
}

static void compress_trace_file_inline(char *input_path)
{
	char output_path[512];
	snprintf(output_path, sizeof(output_path), "%s.zst", input_path);

	FILE *in = fopen(input_path, "rb");
	if (!in) {
		fprintf(stderr, "Failed to open %s: %s\n", input_path,
			strerror(errno));
		return;
	}

	FILE *out = fopen(output_path, "wb");
	if (!out) {
		fprintf(stderr, "Failed to open %s: %s\n", output_path,
			strerror(errno));
		fclose(in);
		return;
	}

	size_t const buf_size = ZSTD_CStreamOutSize();
	void *in_buf = malloc(buf_size);
	void *out_buf = malloc(buf_size);
	ZSTD_CStream *cstream = ZSTD_createCStream();
	ZSTD_initCStream(cstream, 3);

	ZSTD_outBuffer out_buf_wrap = { out_buf, buf_size, 0 };

	while (1) {
		size_t read_size = fread(in_buf, 1, buf_size, in);
		ZSTD_inBuffer in_buf_wrap = { in_buf, read_size, 0 };

		int finished = feof(in);

		ZSTD_EndDirective mode = finished ? ZSTD_e_end :
						    ZSTD_e_continue;

		do {
			out_buf_wrap.pos = 0;
			size_t ret = ZSTD_compressStream2(
				cstream, &out_buf_wrap, &in_buf_wrap, mode);
			if (ZSTD_isError(ret)) {
				fprintf(stderr,
					"ZSTD_compressStream2 error: %s\n",
					ZSTD_getErrorName(ret));
				break;
			}
			fwrite(out_buf, 1, out_buf_wrap.pos, out);
			if (finished && ret == 0)
				break;
		} while (in_buf_wrap.pos < in_buf_wrap.size ||
			 (finished && out_buf_wrap.pos > 0));

		if (finished)
			break;
	}

	ZSTD_freeCStream(cstream);
	free(in_buf);
	free(out_buf);
	fclose(in);
	fclose(out);
	remove(input_path);
}

static void rotate_trace_if_needed_locked(void)
{
	if (trace_fp == NULL || trace_fp == stderr)
		return;

	if (trace_bytes_written < MAX_TRACE_SIZE)
		return;

	char rotated_name[256];
	format_timestamped_name(rotated_name, sizeof(rotated_name));

	fclose(trace_fp);
	trace_fp = NULL;

	if (rename(reffs_trace_name, rotated_name) != 0) {
		fprintf(stderr,
			"Failed to rename trace log: %s (from %s to %s)\n",
			strerror(errno), reffs_trace_name, rotated_name);
		// Re-open original to continue tracing if rename fails
		goto reopen;
	}

	char *queued = strdup(rotated_name);
	if (queued) {
		pthread_mutex_lock(&trace_compress_mutex);
		int next_head = (trace_compress_head + 1) % MAX_TRACE_QUEUE;
		if (next_head != trace_compress_tail) {
			trace_compress_queue[trace_compress_head] = queued;
			trace_compress_head = next_head;
			pthread_cond_signal(&trace_compress_cond);
		} else {
			pthread_mutex_unlock(&trace_compress_mutex);
			fprintf(stderr,
				"Compress queue full, compressing %s inline\n",
				rotated_name);
			compress_trace_file_inline(queued);
			free(queued);
			goto reopen;
		}
		pthread_mutex_unlock(&trace_compress_mutex);
	}

reopen:
	trace_fp = fopen(reffs_trace_name, "w");
	if (!trace_fp) {
		fprintf(stderr, "Failed to reopen trace.log: %s\n",
			strerror(errno));
		trace_fp = stderr;
	}
	trace_bytes_written = 0;
}

/* Initialize tracing */
void reffs_trace_init(const char *filename)
{
	reffs_trace_name = filename;
	pthread_create(&trace_compress_tid, NULL, trace_compress_thread, NULL);
	if (reffs_trace_name)
		trace_fp = fopen(reffs_trace_name, "w");
	if (!trace_fp)
		trace_fp = stderr;
	trace_bytes_written = 0;
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

/* Enable all trace categories */
void reffs_trace_enable_all_categories(void)
{
	pthread_mutex_lock(&trace_mutex);
	for (int i = 0; i < REFFS_TRACE_CAT_ALL; i++)
		category_enabled[i] = true;
	pthread_mutex_unlock(&trace_mutex);
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

__thread struct timespec last_event_ts = { 0 };

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
	struct tm tm_buf;
	localtime_r(&ts.tv_sec, &tm_buf);
	strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm_buf);

	uint64_t epoch_ns = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;

	// Compute time delta in microseconds
	uint64_t delta_us = 0;
	if (last_event_ts.tv_sec != 0) {
		int64_t sec_diff = (int64_t)ts.tv_sec - last_event_ts.tv_sec;
		int64_t nsec_diff = (int64_t)ts.tv_nsec - last_event_ts.tv_nsec;
		int64_t total_us =
			(sec_diff * 1000000LL) + (nsec_diff / 1000LL);
		delta_us = (total_us > 0) ? (uint64_t)total_us : 0;
	}
	last_event_ts = ts;

	pid_t tid = syscall(SYS_gettid);

	pthread_mutex_lock(&trace_mutex);
	if (trace_fp != NULL) {
		int n = fprintf(trace_fp,
				"[%s.%09ld] [%s] [epoch_ns=%" PRIu64
				"] [Δ+%6" PRIu64 "us] [%d:%d] (%s:%d): ",
				time_str, ts.tv_nsec, category_names[category],
				epoch_ns, delta_us, getpid(), tid, name, line);

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
