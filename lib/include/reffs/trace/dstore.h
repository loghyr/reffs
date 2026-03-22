/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifndef _REFFS_TRACE_DSTORE_H
#define _REFFS_TRACE_DSTORE_H

#include "reffs/dstore.h"
#include "reffs/trace/common.h"

static inline void trace_dstore(const struct dstore *ds, const char *event,
				int line)
{
	if (!ds) {
		reffs_trace_event(REFFS_TRACE_CAT_NFS, event, line,
				  "dstore=NULL");
		return;
	}

	reffs_trace_event(
		REFFS_TRACE_CAT_NFS, event, line,
		"dstore=%p id=%u ref=%ld state=0x%lx addr=%s path=%s fh_len=%u",
		(const void *)ds, ds->ds_id, ds->ds_ref.refcount,
		(unsigned long)ds->ds_state, ds->ds_address, ds->ds_path,
		ds->ds_root_fh_len);
}

#endif /* _REFFS_TRACE_DSTORE_H */
