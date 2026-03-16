/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_TRACE_NFS4_SERVER_H
#define _REFFS_TRACE_NFS4_SERVER_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "reffs/trace/common.h"
#include "reffs/rpc.h"
#include "reffs/stateid.h"
#include "nfsv42_xdr.h"

/* Forward declaration from ops.h */
const char *nfs4_op_name(nfs_opnum4 op);

static inline void trace_nfs4_stateid(struct stateid *stid, const char *event,
				      int line)
{
	reffs_trace_event(REFFS_TRACE_CAT_NFS, event, line,
			  "stid=%u seqid=%u type=%u", stid->s_id, stid->s_seqid,
			  stid->s_type);
}

/* NFS4 operation trace functions */
static inline void trace_nfs4_srv_null(struct rpc_trans *rt)
{
	reffs_trace_event(REFFS_TRACE_CAT_NFS, "nfs4_null", __LINE__,
			  "xid=0x%08x", rt->rt_info.ri_xid);
}

static inline void trace_nfs4_srv_compound(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	COMPOUND4args *args = (COMPOUND4args *)ph->ph_args;
	char ops_buf[1024];
	int pos = 0;

	pos += snprintf(ops_buf + pos, sizeof(ops_buf) - pos, "[ ");
	for (uint32_t i = 0; i < args->argarray.argarray_len; i++) {
		const char *name =
			nfs4_op_name(args->argarray.argarray_val[i].argop);
		pos += snprintf(
			ops_buf + pos, sizeof(ops_buf) - pos, "%s%s", name,
			(i == args->argarray.argarray_len - 1) ? "" : ", ");
		if (pos >= (int)sizeof(ops_buf) - 32)
			break;
	}
	snprintf(ops_buf + pos, sizeof(ops_buf) - pos, " ]");

	reffs_trace_event(REFFS_TRACE_CAT_NFS, "nfs4_compound", __LINE__,
			  "xid=0x%08x ops = %s", rt->rt_info.ri_xid, ops_buf);
}

#endif /* _REFFS_TRACE_NFS4_SERVER_H */
