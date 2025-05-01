/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifndef _REFFS_TRACE_FS_H
#define _REFFS_TRACE_FS_H

#include <stdint.h>
#include "reffs/dirent.h"
#include "reffs/trace/trace.h"

static inline void trace_fs_dirent(struct dirent *de, const char *action)
{
	reffs_trace_event(REFFS_TRACE_CAT_FS, action, "de=%p ref=%ld",
			  (void *)de, de->d_ref.refcount);
}

#endif /* _REFFS_TRACE_FS_H */
