/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_NFS4_COMPOUND_H
#define _REFFS_NFS4_COMPOUND_H

#include <rpc/auth_unix.h>
#include <stdbool.h>
#include <sys/types.h>

#include "reffs/filehandle.h"
#include "reffs/rpc.h"
#include "reffs/super_block.h"
#include "reffs/inode.h"
#include "reffs/stateid.h"
#include "nfs4/client.h"
#include "nfs4/session.h"
#include "nfsv42_xdr.h"

struct compound {
	struct rpc_trans *c_rt;
	u_int c_curr_op;
	uint64_t c_op_start_ns; /* wall-clock start of the current op (ns) */
	struct authunix_parms c_ap;
	struct network_file_handle c_curr_nfh;
	struct network_file_handle c_saved_nfh;
	struct super_block *c_curr_sb;
	struct super_block *c_saved_sb;
	struct stateid *c_curr_stid;
	struct stateid *c_saved_stid;
	struct inode *c_inode;
	/*
	 * c_session is set by SEQUENCE; c_slot points into c_session->ns_slots.
	 * c_nfs4_client is set directly by EXCHANGE_ID and also by SEQUENCE
	 * (from c_session->ns_client) so all op handlers have a consistent path
	 * to the client regardless of which op established it.
	 */
	struct nfs4_session *c_session;
	struct nfs4_slot *c_slot;
	struct nfs4_client *c_nfs4_client;

	/*
	 * For convience.
	 */
	COMPOUND4args *c_args;
	COMPOUND4res *c_res;

	/* Server-wide state — grabbed once at compound entry. */
	struct server_state *c_server_state;

	/*
	 * Monotonic allocation sequence number — detects freed+recycled
	 * compounds (a different compound calloc'd at the same address).
	 */
	uint64_t c_alloc_seq;

	/* Compound-level state flags. */
#define COMPOUND_DS_ATTRS_REFRESHED (1u << 0)
	uint32_t c_flags;
};

int nfs4_proc_compound(struct rpc_trans *rt);

/*
 * dispatch_compound -- run compound ops until all are done or one goes async.
 *
 * Returns true if the compound yielded (an op called task_pause); the caller
 * must not finalize the compound.  Returns false when all ops completed
 * (or an error stopped the loop).
 */
bool dispatch_compound(struct compound *compound);

#endif
