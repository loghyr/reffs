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
#include "reffs/settings.h"
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

	/* Server-wide state -- grabbed once at compound entry. */
	struct server_state *c_server_state;

	/*
	 * Monotonic allocation sequence number -- detects freed+recycled
	 * compounds (a different compound calloc'd at the same address).
	 */
	uint64_t c_alloc_seq;

	/*
	 * GSS principal for this compound (non-NULL only when the RPC
	 * credential is RPCSEC_GSS -- currently always NULL on the DS
	 * path since the DS accepts AUTH_SYS from the MDS).
	 *
	 * TRUST_STATEID checks tsa_principal against this field.  When
	 * tsa_principal is non-empty but c_gss_principal is NULL (AUTH_SYS
	 * caller), the op returns NFS4ERR_ACCESS -- a trusted MDS never
	 * sends a non-empty principal over AUTH_SYS.
	 */
	const char *c_gss_principal; /* NULL for AUTH_SYS */
	/*
	 * Backing storage for c_gss_principal in the production path
	 * (slice plan-A.i).  compound_alloc() calls
	 * rpc_cred_get_gss_principal(); on success it copies the
	 * principal into c_gss_principal_buf and points
	 * c_gss_principal at the buffer.  Test mocks may bypass the
	 * buffer and point c_gss_principal at a string literal --
	 * compound_free() does not free or unwind this field.
	 *
	 * NOT_NOW_BROWN_COW (was deferred until plan-A.i): production
	 * wiring of c_gss_principal from the GSS context now lives in
	 * compound_alloc().  Unit tests retain the bypass-the-buffer
	 * pattern.
	 */
	char c_gss_principal_buf[REFFS_CONFIG_MAX_PRINCIPAL];

	/*
	 * Slice 6b-iv: TLS peer certificate identity context.  SHA-256
	 * of the peer cert's DER encoding, formatted as colon-separated
	 * hex.  NULL when the session is not over TLS or the peer did
	 * not present a cert.  PROXY_REGISTRATION matches against
	 * either this OR c_gss_principal (slice 6b-i allowlist).
	 *
	 * NOT_NOW_BROWN_COW: populate from
	 * tls_get_peer_cert_fingerprint() once the dispatch path wires
	 * SSL session -> compound (deferred alongside the c_gss_principal
	 * production wiring; both are mockable in unit tests today).
	 */
	const char *c_tls_fingerprint; /* NULL for non-TLS or no peer cert */
	/*
	 * Backing storage for c_tls_fingerprint in the production
	 * path (slice plan-A.ii).  compound_alloc() calls
	 * io_conn_get_peer_cert_fingerprint(); on success it copies
	 * the formatted hex into c_tls_fingerprint_buf and points
	 * c_tls_fingerprint at the buffer.  Test mocks may bypass
	 * the buffer and point c_tls_fingerprint at a string literal.
	 */
	char c_tls_fingerprint_buf[REFFS_CONFIG_MAX_TLS_FINGERPRINT];

	/*
	 * Plan A follow-up #2: cache the connection's TLS state at
	 * compound_alloc() time.  The TLS bit on a conn_info never
	 * mid-compound flips (STARTTLS only fires before any compound
	 * dispatch), so the per-op nfs4_check_wrongsec() callers can
	 * read this scalar instead of taking conn_mutex via
	 * io_conn_is_tls_enabled().  A 16-op compound saves 16 mutex
	 * lock/unlock cycles.
	 */
	bool c_is_tls;

	/* Compound-level state flags. */
#define COMPOUND_DS_ATTRS_REFRESHED (1u << 0)
	uint32_t c_flags;

	/*
	 * Listener scope.  0 = native namespace; 1+ = a proxy-server
	 * namespace defined by [[proxy_mds]].  Set at compound_alloc()
	 * from the accepting fd's conn_info.  PUTFH / PUTROOTFH /
	 * PUTPUBFH pass this to super_block_find_for_listener() so that
	 * an FH minted on one listener and presented on another misses
	 * the lookup and the client sees NFS4ERR_STALE.
	 */
	uint32_t c_listener_id;
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
