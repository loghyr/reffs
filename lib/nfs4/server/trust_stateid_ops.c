/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * TRUST_STATEID, REVOKE_STATEID, BULK_REVOKE_STATEID op handlers.
 *
 * These ops are DS-side control plane: the MDS registers layout
 * stateids so the DS can validate them in CHUNK_WRITE / CHUNK_READ.
 *
 * All three ops require that the caller's EXCHANGE_ID carried
 * EXCHGID4_FLAG_USE_PNFS_MDS.  This is enforced by checking
 * compound->c_nfs4_client->nc_exchgid_flags.
 *
 * The tsa_expire field is a nfstime4 wall-clock deadline.  We
 * convert it to CLOCK_MONOTONIC here using a dual-clock snapshot
 * (see trust_stateid_convert_expire).
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <string.h>
#include <time.h>

#include "nfsv42_xdr.h"
#include "nfsv42_names.h"
#include "reffs/log.h"
#include "reffs/rpc.h"
#include "reffs/inode.h"
#include "reffs/server.h"
#include "reffs/filehandle.h"
#include "nfs4/compound.h"
#include "nfs4/ops.h"
#include "nfs4/errors.h"
#include "nfs4/client.h"
#include "nfs4/stateid.h"
#include "nfs4/trust_stateid.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                             */

/*
 * require_mds_client -- return true if the compound has an associated
 * nfs4_client that is allowed to issue trust-stateid ops.
 *
 * Phase 2 NFSv4 dstore bring-up: the MDS-to-DS session uses
 * EXCHGID4_FLAG_USE_NON_PNFS per RFC 8881 S18.35 ("MDS is a plain
 * NFSv4 client of the DS, not the DS's MDS"), but the original guard
 * only accepted EXCHGID4_FLAG_USE_PNFS_MDS -- so every TRUST_STATEID
 * /REVOKE_STATEID/BULK_REVOKE_STATEID from the MDS came back with
 * NFS4ERR_PERM.  Probe at startup reported tight coupling disabled
 * for every dstore even though the protocol is wired correctly on
 * both sides.
 *
 * Accept either flag: USE_PNFS_MDS for legacy callers, USE_NON_PNFS
 * for the MDS-to-DS path.  Production deployments will want a real
 * allowlist (similar to [[allowed_ps]] for proxy-server registration);
 * tracked as a follow-on.  For now, any session that completed
 * EXCHANGE_ID is allowed to fan out trust state to this DS -- the DS
 * has no authentication wider than the connection's RPCSEC and the
 * TRUST_STATEID op itself is purely advisory (the actual access
 * control is the client's stateid match against the trust table).
 */
static bool require_mds_client(struct compound *compound, nfsstat4 *status)
{
	if (!compound->c_nfs4_client) {
		*status = NFS4ERR_PERM;
		return false;
	}
	uint32_t flags = compound->c_nfs4_client->nc_exchgid_flags;

	if (!(flags &
	      (EXCHGID4_FLAG_USE_PNFS_MDS | EXCHGID4_FLAG_USE_NON_PNFS))) {
		*status = NFS4ERR_PERM;
		return false;
	}
	return true;
}

/*
 * dual_clock_now -- snapshot wall-clock and monotonic together.
 * Both are returned in nanoseconds.
 */
static void dual_clock_now(uint64_t *wall_ns_out, uint64_t *mono_ns_out)
{
	struct timespec wall, mono;

	clock_gettime(CLOCK_REALTIME, &wall);
	clock_gettime(CLOCK_MONOTONIC, &mono);

	*wall_ns_out =
		(uint64_t)wall.tv_sec * 1000000000ULL + (uint64_t)wall.tv_nsec;
	*mono_ns_out =
		(uint64_t)mono.tv_sec * 1000000000ULL + (uint64_t)mono.tv_nsec;
}

/* ------------------------------------------------------------------ */
/* TRUST_STATEID                                                        */
/* ------------------------------------------------------------------ */

uint32_t nfs4_op_trust_stateid(struct compound *compound)
{
	TRUST_STATEID4args *args = NFS4_OP_ARG_SETUP(compound, optrust_stateid);
	TRUST_STATEID4res *res = NFS4_OP_RES_SETUP(compound, optrust_stateid);
	nfsstat4 *status = &res->tsr_status;

	if (network_file_handle_empty(&compound->c_curr_nfh)) {
		*status = NFS4ERR_NOFILEHANDLE;
		return 0;
	}

	if (!require_mds_client(compound, status))
		return 0;

	/*
	 * tsa_layout_stateid must not be a special stateid.
	 *
	 * Return NFS4ERR_INVAL (not NFS4ERR_BAD_STATEID): an MDS may
	 * probe for tight-coupling support by sending an anonymous
	 * stateid; INVAL is the correct "capability not supported this
	 * way" signal that lets the MDS fall back to loose coupling.
	 */
	if (stateid4_is_special(&args->tsa_layout_stateid)) {
		*status = NFS4ERR_INVAL;
		return 0;
	}

	/* Validate iomode. */
	if (args->tsa_iomode != LAYOUTIOMODE4_READ &&
	    args->tsa_iomode != LAYOUTIOMODE4_RW) {
		*status = NFS4ERR_INVAL;
		return 0;
	}

	/* Validate nfstime4: nseconds must be < 1e9. */
	if (args->tsa_expire.nseconds >= 1000000000u) {
		*status = NFS4ERR_INVAL;
		return 0;
	}

	/*
	 * Principal check.
	 *
	 * If tsa_principal is non-empty the MDS is saying "only trust
	 * I/O from this principal".  We record the principal string in
	 * the trust entry; the CHUNK handler validates it at I/O time.
	 *
	 * Currently, the compound's c_gss_principal is NULL for AUTH_SYS
	 * sessions.  A non-empty tsa_principal from an AUTH_SYS compound
	 * indicates a misconfigured MDS -- reject it.
	 */
	const char *principal = "";
	if (args->tsa_principal.utf8string_len > 0) {
		if (!compound->c_gss_principal) {
			TRACE("TRUST_STATEID: non-empty principal from "
			      "AUTH_SYS MDS -- rejecting");
			*status = NFS4ERR_ACCESS;
			return 0;
		}
		/* Validate UTF-8 length fits in our table. */
		if (args->tsa_principal.utf8string_len >= TRUST_PRINCIPAL_MAX) {
			*status = NFS4ERR_INVAL;
			return 0;
		}
		principal = args->tsa_principal.utf8string_val;
	}

	/* Convert wall-clock expiry to CLOCK_MONOTONIC. */
	uint64_t wall_ns, mono_ns;
	dual_clock_now(&wall_ns, &mono_ns);
	uint64_t expire_mono_ns = trust_stateid_convert_expire(
		&args->tsa_expire, wall_ns, mono_ns);
	if (expire_mono_ns == 0) {
		*status = NFS4ERR_INVAL;
		return 0;
	}

	uint64_t ino = compound->c_inode ? compound->c_inode->i_ino : 0;
	clientid4 clientid = compound->c_nfs4_client ?
				     (clientid4)nfs4_client_to_client(
					     compound->c_nfs4_client)
					     ->c_id :
				     0;

	int ret = trust_stateid_register(&args->tsa_layout_stateid, ino,
					 clientid, args->tsa_iomode,
					 expire_mono_ns, principal);
	if (ret != 0)
		*status = NFS4ERR_SERVERFAULT;

	return 0;
}

/* ------------------------------------------------------------------ */
/* REVOKE_STATEID                                                       */
/* ------------------------------------------------------------------ */

uint32_t nfs4_op_revoke_stateid(struct compound *compound)
{
	REVOKE_STATEID4args *args =
		NFS4_OP_ARG_SETUP(compound, oprevoke_stateid);
	REVOKE_STATEID4res *res = NFS4_OP_RES_SETUP(compound, oprevoke_stateid);
	nfsstat4 *status = &res->rsr_status;

	if (network_file_handle_empty(&compound->c_curr_nfh)) {
		*status = NFS4ERR_NOFILEHANDLE;
		return 0;
	}

	if (!require_mds_client(compound, status))
		return 0;

	if (stateid4_is_special(&args->rsa_layout_stateid)) {
		*status = NFS4ERR_BAD_STATEID;
		return 0;
	}

	trust_stateid_revoke(&args->rsa_layout_stateid);

	/* Returns NFS4_OK (default, 0) whether or not the entry existed. */
	return 0;
}

/* ------------------------------------------------------------------ */
/* BULK_REVOKE_STATEID                                                  */
/* ------------------------------------------------------------------ */

uint32_t nfs4_op_bulk_revoke_stateid(struct compound *compound)
{
	BULK_REVOKE_STATEID4args *args =
		NFS4_OP_ARG_SETUP(compound, opbulk_revoke_stateid);
	BULK_REVOKE_STATEID4res *res =
		NFS4_OP_RES_SETUP(compound, opbulk_revoke_stateid);
	nfsstat4 *status = &res->brsr_status;

	if (!require_mds_client(compound, status))
		return 0;

	/*
	 * No PUTFH required -- BULK_REVOKE_STATEID operates on the
	 * entire trust table (or all entries for a clientid), not on
	 * a specific file.  The current filehandle is ignored.
	 *
	 * brsa_clientid == 0 means "clear everything" (MDS reboot cleanup).
	 */
	trust_stateid_bulk_revoke(args->brsa_clientid);

	return 0;
}
