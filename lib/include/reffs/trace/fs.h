/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_TRACE_FS_H
#define _REFFS_TRACE_FS_H

#include <stdint.h>
#include "reffs/dirent.h"
#include "reffs/super_block.h"
#include "reffs/inode.h"
#include "reffs/stateid.h"
#include "reffs/trace/common.h"

static inline void trace_fs_stateid(struct stateid *stid, const char *event,
				    int line)
{
	reffs_trace_event(REFFS_TRACE_CAT_FS, event, line,
			  "stid=%u seqid=%u type=%u", stid->s_id, stid->s_seqid,
			  stid->s_tag);
}

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
		(void *)inode, inode ? inode->i_ref.refcount : 0,
		inode ? (inode->i_sb ? inode->i_sb->sb_id : 0) : 0,
		inode ? inode->i_ino : 0, inode ? inode->i_nlink : 0,
		inode ? inode->i_size : 0, inode ? inode->i_mode : 0);
}

static inline void trace_fs_super_block(struct super_block *sb,
					const char *event, int line)
{
	reffs_trace_event(REFFS_TRACE_CAT_FS, event, line,
			  "sb=%p ref=%ld id=%lu", (void *)sb,
			  sb ? sb->sb_ref.refcount : 0, sb ? sb->sb_id : 0);
}

#endif /* _REFFS_TRACE_FS_H */
