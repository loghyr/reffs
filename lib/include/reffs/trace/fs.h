/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifndef _REFFS_TRACE_FS_H
#define _REFFS_TRACE_FS_H

#include <stdint.h>
#include "reffs/dirent.h"
#include "reffs/trace/common.h"

static inline void trace_fs_dirent(struct reffs_dirent *rd, const char *event,
				   int line)
{
	reffs_trace_event(REFFS_TRACE_CAT_FS, event, line, "rd=%p ref=%ld",
			  (void *)rd, rd->rd_ref.refcount);
}

static inline void trace_fs_inode(struct inode *inode, const char *event,
				  int line)
{
	reffs_trace_event(REFFS_TRACE_CAT_FS, event, line, "inode=%p ref=%ld",
			  (void *)inode, inode->i_ref.refcount);
}

#endif /* _REFFS_TRACE_FS_H */
