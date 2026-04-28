/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * PROXY_REGISTRATION + PROXY_PROGRESS op handlers.
 *
 * Slice 6a + 6b-i (mirror of design phase 6 in
 * .claude/design/proxy-server.md and slice plan in
 * .claude/design/proxy-server-phase6b.md):
 *
 *   - PROXY_REGISTRATION wires the bare flag-bit and
 *     session-context validation, sets nc_is_registered_ps on the
 *     calling client (6a), and rejects any GSS principal absent
 *     from the [[allowed_ps]] allowlist (6b-i).  SQUAT-GUARD +
 *     RENEWAL + TLS-vs-AUTH_SYS distinction land in slices
 *     6b-iii / 6b-iv along with the bypass-wiring + audit logs
 *     (6b-ii) that consume nc_is_registered_ps.
 *
 *   - PROXY_PROGRESS is wire-allocated (op number 94) but its
 *     handler is a NFS4ERR_NOTSUPP stub.  Slice 6c-w (the
 *     2026-04-26 architecture revision) walked back the original
 *     CB_PROXY_* design and re-shaped PROXY_PROGRESS as a fore-
 *     channel poll whose reply carries work assignments inline;
 *     slice 6c-y populates the assignment list from the autopilot
 *     queue.  Until then the handler stays as a NFS4ERR_NOTSUPP
 *     stub.
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "nfsv42_xdr.h"
#include "nfsv42_names.h"
#include "reffs/log.h"
#include "reffs/server.h"
#include "reffs/time.h"
#include "nfs4/compound.h"
#include "nfs4/ops.h"
#include "nfs4/client.h"
#include "nfs4/migration_record.h"
#include "nfs4/proxy_stateid.h"

/*
 * Returns true if either identity context (GSS principal OR TLS
 * fingerprint) exactly matches any entry in the server-state
 * allowlist.  Empty allowlist -> always false (deny).  Realm fuzz /
 * DNS canonicalization / glob are intentionally NOT supported on
 * either column -- an entry binds to one identity, full stop.  See
 * proxy-server-phase6b.md "Security model".
 *
 * Slice 6b-i seeded the principal column; slice 6b-iv added the
 * tls_fingerprint column.  Either context is sufficient -- the
 * matched identity (principal OR fingerprint) is the privilege
 * grant, and a registration only needs ONE recognised identity.
 */
static bool ps_identity_allowed(const struct server_state *ss,
				const char *principal,
				const char *tls_fingerprint)
{
	bool have_principal = principal && principal[0] != '\0';
	bool have_fingerprint = tls_fingerprint && tls_fingerprint[0] != '\0';

	if (!have_principal && !have_fingerprint)
		return false;
	for (unsigned int i = 0; i < ss->ss_nallowed_ps; i++) {
		if (have_principal && ss->ss_allowed_ps[i][0] != '\0' &&
		    strcmp(ss->ss_allowed_ps[i], principal) == 0)
			return true;
		if (have_fingerprint &&
		    ss->ss_allowed_ps_tls_fingerprint[i][0] != '\0' &&
		    strcmp(ss->ss_allowed_ps_tls_fingerprint[i],
			   tls_fingerprint) == 0)
			return true;
	}
	return false;
}

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
	 * authentication; AUTH_SYS over plain TCP is forbidden.  Slice
	 * 6b-iv broadens the slice-6a "no GSS principal -> reject"
	 * check to "neither GSS principal NOR TLS fingerprint -> reject":
	 * a TLS-authenticated session with an allowlisted client cert
	 * is now a valid identity context, even with no GSS principal.
	 *
	 * Production wiring of c_gss_principal and c_tls_fingerprint
	 * remains NOT_NOW_BROWN_COW -- both fields are populated only
	 * via test mocks today.  See compound.h.
	 */
	if (compound->c_gss_principal == NULL &&
	    compound->c_tls_fingerprint == NULL) {
		*status = NFS4ERR_PERM;
		return 0;
	}

	/*
	 * Slice 6b-i + 6b-iv: identity check.  Either the GSS principal
	 * OR the TLS fingerprint must match an entry on the operator-
	 * curated [[allowed_ps]] allowlist.  Default-deny: an absent or
	 * empty list rejects every PROXY_REGISTRATION, which is the
	 * correct posture for a security-sensitive privilege grant.
	 * LOG (not TRACE) every reject -- a rejected registration is
	 * operator-actionable (misconfig or attack).
	 *
	 * Use compound->c_server_state (grabbed once per compound by
	 * dispatch_compound) rather than server_state_find() -- avoids
	 * the GRACE_STARTED -> IN_GRACE side-effect transition and a
	 * redundant ref bump.  c_server_state is provably non-NULL
	 * here because dispatch returned NFS4ERR_DELAY before this
	 * handler can run if the lookup failed.
	 */
	if (!ps_identity_allowed(compound->c_server_state,
				 compound->c_gss_principal,
				 compound->c_tls_fingerprint)) {
		LOG("PROXY_REGISTRATION: identity not on allowlist "
		    "(principal=%s tls_fingerprint=%s)",
		    compound->c_gss_principal ? compound->c_gss_principal :
						"(null)",
		    compound->c_tls_fingerprint ? compound->c_tls_fingerprint :
						  "(null)");
		*status = NFS4ERR_PERM;
		return 0;
	}

	/*
	 * Slice 6b-iii: squat-guard.  If another in-memory client is
	 * already registered with this same GSS principal AND its
	 * lease is still valid AND the incoming prr_registration_id
	 * does not match -- this is a different peer trying to displace
	 * an active PS.  Refuse with NFS4ERR_DELAY so the legit PS keeps
	 * its grant; the attacker (or the legit PS after a state-loss
	 * restart) must wait one lease period.  A matching id is a
	 * renewal: refresh the prior client's lease and proceed.
	 */
	struct nfs4_client *self = compound->c_nfs4_client;
	struct nfs4_client *other = nfs4_client_find_other_registered_ps(
		compound->c_server_state, self, compound->c_gss_principal,
		compound->c_tls_fingerprint);

	uint64_t lease_period_ns =
		(uint64_t)server_lease_time(compound->c_server_state) *
		1000000000ULL;

	if (other) {
		uint64_t now_ns = reffs_now_ns();
		uint64_t expire_ns = atomic_load_explicit(
			&other->nc_ps_lease_expire_ns, memory_order_acquire);
		bool same_id = other->nc_ps_registration_id_len ==
				       args->prr_registration_id
					       .prr_registration_id_len &&
			       (other->nc_ps_registration_id_len == 0 ||
				memcmp(other->nc_ps_registration_id,
				       args->prr_registration_id
					       .prr_registration_id_val,
				       other->nc_ps_registration_id_len) == 0);

		if (expire_ns > now_ns && !same_id) {
			LOG("PROXY_REGISTRATION: squat blocked -- identity "
			    "already registered with different "
			    "registration_id (principal=%s tls_fingerprint=%s)",
			    compound->c_gss_principal ?
				    compound->c_gss_principal :
				    "(null)",
			    compound->c_tls_fingerprint ?
				    compound->c_tls_fingerprint :
				    "(null)");
			nfs4_client_put(other);
			*status = NFS4ERR_DELAY;
			return 0;
		}
		if (expire_ns > now_ns && same_id) {
			/* Renewal: bump prior client's lease so the still-
			 * connected old session does not lose its privilege
			 * between "renewed on new session" and "old session
			 * times out on its own". */
			atomic_store_explicit(&other->nc_ps_lease_expire_ns,
					      now_ns + lease_period_ns,
					      memory_order_release);
		}
		/* Else expired: treat as fresh, fall through. */
		nfs4_client_put(other);
	}

	/*
	 * Capture identity + lease on self, then record the privilege.
	 * Future namespace-discovery ops on any session belonging to
	 * this client will bypass export-rule filtering -- see
	 * .claude/design/proxy-server.md "Privilege model".  Audit
	 * logging of the bypassed ops landed in slice 6b-ii.
	 */
	if (compound->c_gss_principal) {
		strncpy(self->nc_ps_principal, compound->c_gss_principal,
			REFFS_CONFIG_MAX_PRINCIPAL - 1);
		self->nc_ps_principal[REFFS_CONFIG_MAX_PRINCIPAL - 1] = '\0';
	} else {
		self->nc_ps_principal[0] = '\0';
	}
	if (compound->c_tls_fingerprint) {
		strncpy(self->nc_ps_tls_fingerprint,
			compound->c_tls_fingerprint,
			REFFS_CONFIG_MAX_TLS_FINGERPRINT - 1);
		self->nc_ps_tls_fingerprint[REFFS_CONFIG_MAX_TLS_FINGERPRINT -
					    1] = '\0';
	} else {
		self->nc_ps_tls_fingerprint[0] = '\0';
	}
	if (args->prr_registration_id.prr_registration_id_len > 0) {
		memcpy(self->nc_ps_registration_id,
		       args->prr_registration_id.prr_registration_id_val,
		       args->prr_registration_id.prr_registration_id_len);
	}
	self->nc_ps_registration_id_len =
		args->prr_registration_id.prr_registration_id_len;
	atomic_store_explicit(&self->nc_ps_lease_expire_ns,
			      reffs_now_ns() + lease_period_ns,
			      memory_order_release);
	/*
	 * Publication: this release-store is the synchronisation point
	 * for the squat-guard scanner running on another session.  All
	 * stores above (principal / registration_id / lease) MUST be
	 * visible to a scanner that observes nc_is_registered_ps == true
	 * via acquire-load -- see client.c's
	 * nfs4_client_find_other_registered_ps and security.c's
	 * registered-PS bypass check.
	 */
	atomic_store_explicit(&self->nc_is_registered_ps, true,
			      memory_order_release);

	LOG("PROXY_REGISTRATION: client granted PS privilege "
	    "(principal=%s tls_fingerprint=%s exchgid_flags=0x%x)",
	    compound->c_gss_principal ? compound->c_gss_principal : "(null)",
	    compound->c_tls_fingerprint ? compound->c_tls_fingerprint :
					  "(null)",
	    compound->c_nfs4_client->nc_exchgid_flags);

	/* status stays NFS4_OK (zero from calloc'd resarray). */
	return 0;
}

uint32_t nfs4_op_proxy_progress(struct compound *compound)
{
	PROXY_PROGRESS4res *res = NFS4_OP_RES_SETUP(compound, opproxy_progress);

	/*
	 * Slice 6c-w: response shape now carries assignments on
	 * NFS4_OK; populating that list is slice 6c-y's job (consumes
	 * the autopilot queue).  Until then the stub returns
	 * NFS4ERR_NOTSUPP, which selects the union's `default` arm
	 * (no resok body emitted on the wire).
	 */
	res->ppr_status = NFS4ERR_NOTSUPP;
	return 0;
}

/*
 * Priority-ordered authorization rule for PROXY_DONE / PROXY_CANCEL,
 * per draft-haynes-nfsv4-flexfiles-v2-data-mover sec-PROXY_DONE:
 *
 *   1. Caller's session is registered-PS  -> NFS4ERR_PERM
 *   2. stateid.other matches current boot_seq  -> NFS4ERR_STALE_STATEID
 *   3. Migration record exists for this stateid  -> NFS4ERR_BAD_STATEID
 *   4. Caller's registered-PS identity matches record.owner_reg
 *      -> NFS4ERR_PERM
 *   5. Current FH matches record.file_FH  -> NFS4ERR_BAD_STATEID
 *   6. seqid matches the record's most recently issued seqid
 *      -> NFS4ERR_OLD_STATEID
 *
 * On all-checks-pass returns NFS4_OK and *out_mr is the
 * ref-bumped migration record (caller drops via
 * migration_record_put after applying side effects).  On any
 * failure returns the matching nfsstat4 and *out_mr is NULL.
 */
static nfsstat4 proxy_record_validate(struct compound *compound,
				      const stateid4 *stid,
				      struct migration_record **out_mr)
{
	*out_mr = NULL;

	struct nfs4_client *nc = compound->c_nfs4_client;

	if (!nc || !atomic_load_explicit(&nc->nc_is_registered_ps,
					 memory_order_acquire))
		return NFS4ERR_PERM;

	struct server_state *ss = compound->c_server_state;
	uint16_t boot_seq = ss ? server_boot_seq(ss) : 0;

	if (proxy_stateid_is_stale(stid, boot_seq))
		return NFS4ERR_STALE_STATEID;

	struct migration_record *mr = migration_record_find_by_stateid(stid);

	if (!mr)
		return NFS4ERR_BAD_STATEID;

	uint32_t caller_len = 0;
	const char *caller_id =
		nfs4_client_registered_ps_identity(nc, &caller_len);

	if (!caller_id || caller_len != mr->mr_owner_reg_len ||
	    memcmp(caller_id, mr->mr_owner_reg, caller_len) != 0) {
		migration_record_put(mr);
		return NFS4ERR_PERM;
	}

	/*
	 * File-FH match: PUTFH precondition.  The compound's current
	 * NFH carries (sb_id, ino); the migration record holds (sb,
	 * ino) -- compare both to fully identify the file.  An empty
	 * NFH (no PUTFH preceded) reads as nfh_ino == 0, which can't
	 * match a real migration's mr_ino.
	 */
	if (compound->c_curr_nfh.nfh_ino != mr->mr_ino ||
	    (compound->c_curr_sb && compound->c_curr_sb != mr->mr_sb)) {
		migration_record_put(mr);
		return NFS4ERR_BAD_STATEID;
	}

	uint32_t cur_seqid =
		atomic_load_explicit(&mr->mr_seqid, memory_order_acquire);

	if (stid->seqid != cur_seqid) {
		migration_record_put(mr);
		return NFS4ERR_OLD_STATEID;
	}

	*out_mr = mr;
	return NFS4_OK;
}

uint32_t nfs4_op_proxy_done(struct compound *compound)
{
	PROXY_DONE4args *args = NFS4_OP_ARG_SETUP(compound, opproxy_done);
	PROXY_DONE4res *res = NFS4_OP_RES_SETUP(compound, opproxy_done);
	struct migration_record *mr = NULL;

	res->pdr_status =
		proxy_record_validate(compound, &args->pd_stateid, &mr);
	if (res->pdr_status != NFS4_OK)
		return 0;

	/*
	 * Side effects keyed off pd_status:
	 *
	 *   NFS4_OK    -> commit the migration (transitions phase to
	 *                 COMMITTED and unhashes the record).  Slice
	 *                 6c-x.4 wires the actual per-instance delta
	 *                 application onto i_layout_segments; slice
	 *                 6c-x.5 issues CB_LAYOUTRECALL on DRAINING
	 *                 slot removal.  This slice ships the protocol
	 *                 surface + record-state mutation only.
	 *   non-OK     -> abandon the migration (rollback).  No CB
	 *                 needed: external clients never saw the
	 *                 post-image (omit-and-replace policy delays
	 *                 the layout flip until after the recall).
	 *
	 * commit / abandon return -EALREADY if the record is already
	 * in a terminal phase.  That is a benign double-call (e.g., the
	 * reaper raced ahead of the PS's PROXY_DONE); we still return
	 * NFS4_OK to the PS so its state machine ticks forward.
	 */
	if (args->pd_status == NFS4_OK)
		(void)migration_record_commit(mr);
	else
		(void)migration_record_abandon(mr);

	migration_record_put(mr);
	res->pdr_status = NFS4_OK;
	return 0;
}

uint32_t nfs4_op_proxy_cancel(struct compound *compound)
{
	PROXY_CANCEL4args *args = NFS4_OP_ARG_SETUP(compound, opproxy_cancel);
	PROXY_CANCEL4res *res = NFS4_OP_RES_SETUP(compound, opproxy_cancel);
	struct migration_record *mr = NULL;

	res->pcr_status =
		proxy_record_validate(compound, &args->pc_stateid, &mr);
	if (res->pcr_status != NFS4_OK)
		return 0;

	(void)migration_record_abandon(mr);
	migration_record_put(mr);
	res->pcr_status = NFS4_OK;
	return 0;
}
