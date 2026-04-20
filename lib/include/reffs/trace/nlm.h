/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_TRACE_NLM_H
#define _REFFS_TRACE_NLM_H

#include <inttypes.h>
#include "reffs/trace/common.h"
#include "reffs/rpc.h"

static inline void trace_nlm4_lock(struct inode *inode, uint32_t svid,
				   uint64_t offset, uint64_t len,
				   bool exclusive)
{
	reffs_trace_event(REFFS_TRACE_CAT_NLM, "nlm4_lock", __LINE__,
			  "ino=%" PRIu64 " svid=%u off=%" PRIu64 " len=%" PRIu64
			  " excl=%d",
			  inode->i_ino, svid, offset, len, exclusive);
}

static inline void trace_nlm4_unlock(struct inode *inode, uint32_t svid,
				     uint64_t offset, uint64_t len)
{
	reffs_trace_event(REFFS_TRACE_CAT_NLM, "nlm4_unlock", __LINE__,
			  "ino=%" PRIu64 " svid=%u off=%" PRIu64
			  " len=%" PRIu64,
			  inode->i_ino, svid, offset, len);
}

static inline void trace_nlm4_test(struct inode *inode, uint32_t svid,
				   uint64_t offset, uint64_t len,
				   bool exclusive)
{
	reffs_trace_event(REFFS_TRACE_CAT_NLM, "nlm4_test", __LINE__,
			  "ino=%" PRIu64 " svid=%u off=%" PRIu64 " len=%" PRIu64
			  " excl=%d",
			  inode->i_ino, svid, offset, len, exclusive);
}

#endif /* _REFFS_TRACE_NLM_H */
