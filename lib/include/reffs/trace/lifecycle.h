/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifndef _REFFS_TRACE_LIFECYCLE_H
#define _REFFS_TRACE_LIFECYCLE_H

#include "reffs/trace/common.h"

static inline void trace_lifecycle_startup(const char *event, int line,
					   int backend, uint32_t boot_seq,
					   uint32_t clean_shutdown)
{
	reffs_trace_event(REFFS_TRACE_CAT_LIFECYCLE, event, line,
			  "backend=%d boot_seq=%u clean_shutdown=%u", backend,
			  boot_seq, clean_shutdown);
}

static inline void trace_lifecycle_recovery(const char *event, int line,
					    unsigned long sb_id,
					    unsigned long next_ino)
{
	reffs_trace_event(REFFS_TRACE_CAT_LIFECYCLE, event, line,
			  "sb=%lu next_ino=%lu", sb_id, next_ino);
}

static inline void trace_lifecycle_shutdown(const char *event, int line,
					    const char *phase)
{
	reffs_trace_event(REFFS_TRACE_CAT_LIFECYCLE, event, line, "%s", phase);
}

static inline void trace_lifecycle_worker(const char *event, int line,
					  int queued, int discarded)
{
	reffs_trace_event(REFFS_TRACE_CAT_LIFECYCLE, event, line,
			  "queued=%d discarded=%d", queued, discarded);
}

#endif /* _REFFS_TRACE_LIFECYCLE_H */
