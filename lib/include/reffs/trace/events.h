/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifndef _REFFS_TRACE_EVENTS_H
#define _REFFS_TRACE_EVENTS_H

/* 
 * This file defines trace events in a format inspired by Linux kernel tracepoints.
 * These definitions can be used as documentation or processed by tools.
 */

#include "reffs/trace/types.h"

/* 
 * Trace event definition macro
 * 
 * Format:
 * TRACE_EVENT_DEF(name, category, level, args_struct, format_string)
 */

#define TRACE_EVENT_DEF(name, category, level, args_struct, format_string) \
	extern void trace_##name args_struct;

/* IO Subsystem Events */
TRACE_EVENT_DEF(io_read_submit, REFFS_TRACE_CAT_IO, REFFS_TRACE_LEVEL_DEBUG,
		(struct io_context * ic), "fd=%d, op=%s, id=%u, len=%zu");

TRACE_EVENT_DEF(io_record_marker, REFFS_TRACE_CAT_IO, REFFS_TRACE_LEVEL_DEBUG,
		(struct buffer_state * bs, uint32_t marker, bool last_fragment,
		 uint32_t fragment_len),
		"fd=%d, marker=0x%08x, last=%d, len=%u, filled=%zu");

TRACE_EVENT_DEF(io_message_complete, REFFS_TRACE_CAT_IO, REFFS_TRACE_LEVEL_INFO,
		(int fd, uint32_t xid, size_t size),
		"fd=%d, xid=0x%08x, size=%zu");

/* RPC Subsystem Events */
TRACE_EVENT_DEF(rpc_call_start, REFFS_TRACE_CAT_RPC, REFFS_TRACE_LEVEL_INFO,
		(uint32_t xid, int program, int version, int procedure),
		"xid=0x%08x, prog=%d, vers=%d, proc=%d");

TRACE_EVENT_DEF(rpc_call_complete, REFFS_TRACE_CAT_RPC, REFFS_TRACE_LEVEL_INFO,
		(uint32_t xid, int status, uint64_t duration_us),
		"xid=0x%08x, status=%d, duration=%llu us");

#endif /* _REFFS_TRACE_EVENTS_H */
