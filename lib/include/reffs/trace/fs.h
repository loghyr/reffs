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
	reffs_trace_event(
		REFFS_TRACE_CAT_FS, event, line,
		"inode=%p ref=%ld sb=%lu ino=%lu nlink=%u size=%lu mode=%u",
		(void *)inode, inode->i_ref.refcount,
		inode->i_sb ? inode->i_sb->sb_id : 0, inode->i_ino,
		inode->i_nlink, inode->i_size, inode->i_mode);
}

static inline void trace_fs_super_block(struct super_block *sb,
					const char *event, int line)
{
	reffs_trace_event(REFFS_TRACE_CAT_FS, event, line,
			  "sb=%p ref=%ld id=%lu", (void *)sb,
			  sb->sb_ref.refcount, sb->sb_id);
}

#endif /* _REFFS_TRACE_FS_H */
