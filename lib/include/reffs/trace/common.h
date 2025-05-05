/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifndef _REFFS_TRACE_H
#define _REFFS_TRACE_H

#include "reffs/trace/types.h"

/* Initialization and configuration */
void reffs_trace_init(const char *filename);
void reffs_trace_close(void);

/* Enabling/disabling trace categories */
void reffs_trace_enable_category(enum reffs_trace_category category);
void reffs_trace_disable_category(enum reffs_trace_category category);
bool reffs_trace_is_category_enabled(enum reffs_trace_category category);

/* Trace filtering */
bool reffs_should_trace(enum reffs_trace_category category);

/* General trace functions */
void reffs_trace_event(enum reffs_trace_category category, const char *name,
		       const int line, const char *format, ...)
	__attribute__((format(printf, 4, 5)));

#endif /* _REFFS_TRACE_H */
