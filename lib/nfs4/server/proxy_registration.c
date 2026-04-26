/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * PROXY_REGISTRATION + PROXY_PROGRESS op handlers.
 *
 * Slice 6a (mirror of design phase 6 in
 * .claude/design/proxy-server.md):
 *
 *   - PROXY_REGISTRATION wires the bare flag-bit and
 *     session-context validation, sets nc_is_registered_ps on the
 *     calling client.  ALLOWLIST IDENTITY CHECK + SQUAT-GUARD +
 *     TLS-vs-AUTH_SYS distinction land in slice 6b along with
 *     audit logging.
 *
 *   - PROXY_PROGRESS is wire-allocated (op number 94) but its
 *     handler is a NFS4ERR_NOTSUPP stub.  No MDS-initiated CB
 *     ops exist yet -- the receive path arrives in slice 6c when
 *     CB_PROXY_MOVE / CB_PROXY_REPAIR land.  Stubbing the
 *     fore-channel op now is purely for op-number stability.
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <stddef.h>

#include "nfsv42_xdr.h"
#include "nfsv42_names.h"
#include "reffs/log.h"
#include "nfs4/compound.h"
#include "nfs4/ops.h"
#include "nfs4/client.h"

uint32_t nfs4_op_proxy_registration(struct compound *compound)
{
	PROXY_REGISTRATION4args *args =
		NFS4_OP_ARG_SETUP(compound, opproxy_registration);
	PROXY_REGISTRATION4res *res =
		NFS4_OP_RES_SETUP(compound, opproxy_registration);
	nfsstat4 *status = &res->prrr_status;

	/*
	 * RFC 8178 S4.4.3 -- prr_flags is reserved.  Non-zero bits MUST
	 * be rejected with NFS4ERR_INVAL until they are assigned by a
	 * future draft revision.  This is the flag-bit-not-known
	 * discovery rule applied to a numbered slot.
	 */
	if (args->prr_flags != 0) {
		*status = NFS4ERR_INVAL;
		return 0;
	}

	/*
	 * PROXY_REGISTRATION is a fore-channel op that needs a session-
	 * established client to record the privilege against.  No
	 * c_nfs4_client means SEQUENCE didn't run earlier in the
	 * compound (or this is a non-session compound, which the spec
	 * doesn't allow for this op).
	 */
	if (!compound->c_nfs4_client) {
		*status = NFS4ERR_PERM;
		return 0;
	}

	/*
	 * Per the data-mover draft (sec-PROXY_REGISTRATION) the PS's
	 * MDS-facing session is created with EXCHGID4_FLAG_USE_NON_PNFS
	 * -- the PS is a regular NFSv4 client of the MDS, NOT a peer
	 * MDS or DS.  A session whose EXCHANGE_ID set USE_PNFS_MDS or
	 * USE_PNFS_DS is not eligible for the proxy privilege.
	 */
	if (!(compound->c_nfs4_client->nc_exchgid_flags &
	      EXCHGID4_FLAG_USE_NON_PNFS)) {
		*status = NFS4ERR_PERM;
		return 0;
	}

	/*
	 * The data-mover draft (sec-security) mandates that the
	 * MDS<->PS session use RPCSEC_GSS or RPC-over-TLS with mutual
	 * authentication; AUTH_SYS is forbidden.  c_gss_principal is
	 * the only signal currently available -- a non-NULL value
	 * proves RPCSEC_GSS authentication.
	 *
	 * NOT_NOW_BROWN_COW: distinguish AUTH_SYS from TLS-only
	 * sessions.  TLS-only sessions have no GSS principal so they
	 * are also rejected here, which is too strict for a future
	 * mTLS-PS deployment.  Slice 6b adds the TLS auth-context
	 * check (and the [[allowed_ps]] allowlist that consumes the
	 * resulting identity).
	 */
	if (compound->c_gss_principal == NULL) {
		*status = NFS4ERR_PERM;
		return 0;
	}

	/*
	 * Record the privilege on the client.  Future namespace-
	 * discovery ops (LOOKUP / LOOKUPP / PUTFH / PUTROOTFH / GETFH /
	 * SEQUENCE) on any session belonging to this client will
	 * bypass export-rule filtering -- see
	 * .claude/design/proxy-server.md "Privilege model".  Audit
	 * logging of the bypassed ops lands in slice 6b.
	 */
	compound->c_nfs4_client->nc_is_registered_ps = true;

	LOG("PROXY_REGISTRATION: client granted PS privilege "
	    "(principal=%s exchgid_flags=0x%x)",
	    compound->c_gss_principal,
	    compound->c_nfs4_client->nc_exchgid_flags);

	/* status stays NFS4_OK (zero from calloc'd resarray). */
	return 0;
}

uint32_t nfs4_op_proxy_progress(struct compound *compound)
{
	PROXY_PROGRESS4res *res = NFS4_OP_RES_SETUP(compound, opproxy_progress);

	/*
	 * Slice 6a: stub.  PROXY_PROGRESS reports interim/terminal
	 * status of MDS-initiated CB_PROXY_MOVE / CB_PROXY_REPAIR.
	 * Until slice 6c lands the CB receive path there is nothing
	 * for a PS to report progress on.
	 */
	res->prar_status = NFS4ERR_NOTSUPP;
	return 0;
}
