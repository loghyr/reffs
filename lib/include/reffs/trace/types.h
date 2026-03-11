/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_TRACE_TYPES_H
#define _REFFS_TRACE_TYPES_H

#include <stdbool.h>
#include <stdint.h>

/* Trace levels */
enum reffs_trace_level {
	REFFS_TRACE_LEVEL_OFF = 0,
	REFFS_TRACE_LEVEL_ERROR = 1,
	REFFS_TRACE_LEVEL_WARN = 2,
	REFFS_TRACE_LEVEL_NOTICE = 3,
	REFFS_TRACE_LEVEL_INFO = 4,
	REFFS_TRACE_LEVEL_DEBUG = 5,
	REFFS_TRACE_LEVEL_TRACE = 6
};

/* Trace categories */
enum reffs_trace_category {
	REFFS_TRACE_CAT_GENERAL = 0,
	REFFS_TRACE_CAT_IO = 1,
	REFFS_TRACE_CAT_RPC = 2,
	REFFS_TRACE_CAT_NFS = 3,
	REFFS_TRACE_CAT_NLM = 4,
	REFFS_TRACE_CAT_FS = 5,
	REFFS_TRACE_CAT_LOG = 6,
	REFFS_TRACE_CAT_ALL
};

/* Trace event information structure */
struct reffs_trace_event_info {
	const char *event_name;
	enum reffs_trace_category category;
	enum reffs_trace_level level;
	const char *file;
	int line;
	const char *function;
	uint64_t timestamp;
	pid_t pid;
	pthread_t tid;
};

#endif /* _REFFS_TRACE_TYPES_H */
