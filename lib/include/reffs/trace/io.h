/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifndef _REFFS_TRACE_IO_H
#define _REFFS_TRACE_IO_H

#include <stdbool.h>
#include <stdint.h>
#include "reffs/io.h"
#include "reffs/trace/trace.h"

/* IO specific trace functions */
static inline void trace_io_accept_submit(struct io_context *ic)
{
	reffs_trace_event(REFFS_TRACE_CAT_IO, "io_accept_submit",
			  "fd=%d, op=%s, id=%u, len=%zu", ic->ic_fd,
			  io_op_type_to_str(ic->ic_op_type), ic->ic_id,
			  ic->ic_buffer_len);
}

static inline void trace_io_connect_submit(struct io_context *ic)
{
	reffs_trace_event(REFFS_TRACE_CAT_IO, "io_connect_submit",
			  "fd=%d, op=%s, id=%u, len=%zu", ic->ic_fd,
			  io_op_type_to_str(ic->ic_op_type), ic->ic_id,
			  ic->ic_buffer_len);
}

static inline void trace_io_read_submit(struct io_context *ic)
{
	reffs_trace_event(REFFS_TRACE_CAT_IO, "io_read_submit",
			  "fd=%d, op=%s, id=%u, len=%zu", ic->ic_fd,
			  io_op_type_to_str(ic->ic_op_type), ic->ic_id,
			  ic->ic_buffer_len);
}

static inline void trace_io_write_submit(struct io_context *ic)
{
	reffs_trace_event(REFFS_TRACE_CAT_IO, "io_write_submit",
			  "fd=%d, op=%s, id=%u, len=%zu", ic->ic_fd,
			  io_op_type_to_str(ic->ic_op_type), ic->ic_id,
			  ic->ic_buffer_len);
}

static inline void trace_io_record_marker(struct buffer_state *bs,
					  uint32_t marker, bool last_fragment,
					  uint32_t fragment_len)
{
	reffs_trace_event(REFFS_TRACE_CAT_IO, "io_record_marker",
			  "fd=%d, marker=0x%08x, last=%d, len=%u, filled=%zu",
			  bs->bs_fd, marker, last_fragment, fragment_len,
			  bs->bs_filled);
}

static inline void trace_io_message_complete(int fd, uint32_t xid, size_t size)
{
	reffs_trace_event(REFFS_TRACE_CAT_IO, "io_message_complete",
			  "fd=%d, xid=0x%08x, size=%zu", fd, xid, size);
}

static inline void trace_io_context(struct io_context *ic, const char *action)
{
	if (reffs_trace_is_category_enabled(REFFS_TRACE_CAT_IO)) {
		time_t now = time(NULL);
		time_t age = now - ic->ic_creation_time;

		reffs_trace_event(REFFS_TRACE_CAT_IO, action,
				  "ic=%p ref=%ld op=%s fd=%d age=%ld id=%u",
				  (void *)ic, ic->ic_ref.refcount,
				  io_op_type_to_str(ic->ic_op_type), ic->ic_fd,
				  age, ic->ic_id);
	}
}
#endif /* _REFFS_TRACE_IO_H */
