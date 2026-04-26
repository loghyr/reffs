/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * Export security enforcement -- NFS4ERR_WRONGSEC and SECINFO helpers.
 *
 * RFC 8881 section 2.6.3.1: filehandle-changing operations (PUTFH,
 * PUTROOTFH, PUTPUBFH, LOOKUP, LOOKUPP, OPEN) must return
 * NFS4ERR_WRONGSEC when the client's RPC auth flavor does not match
 * the export's allowed flavor list.
 *
 * Exception: if the next operation in the compound is SECINFO or
 * SECINFO_NO_NAME, the put-FH operation must succeed so the client
 * can discover the required flavors.
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <rpc/auth.h>

#include "nfsv42_xdr.h"
#include "reffs/client_match.h"
#include "reffs/log.h"
#include "reffs/io.h"
#include "reffs/rpc.h"
#include "reffs/server.h"
#include "reffs/settings.h"
#include "nfs4/client.h"
#include "nfs4/compound.h"
#include "nfs4/ops.h"

/*
 * Returns true if `op` is a namespace-discovery op that bypasses
 * export-rule filtering for a registered Proxy Server (slice 6b-ii).
 *
 * Discovery ops let a registered PS walk the MDS namespace to
 * resolve client-driven LOOKUP / GETFH paths into FHs the PS can
 * cache and re-present on its :4098 listener.  Data-access ops
 * (OPEN, READ, WRITE, etc.) are NOT in this set: they apply normal
 * authorization against the forwarded client credentials.
 *
 * GETFH and SEQUENCE are listed in the design (proxy-server.md
 * "Privilege model") but do not call nfs4_check_wrongsec(), so they
 * are not enumerated here.  RESTOREFH is included because it is a
 * put-FH op that can land on a flavor-restricted export, same as
 * PUTFH.
 */
static bool op_is_namespace_discovery(uint32_t opnum)
{
	switch (opnum) {
	case OP_PUTFH:
	case OP_PUTPUBFH:
	case OP_PUTROOTFH:
	case OP_RESTOREFH:
	case OP_LOOKUP:
	case OP_LOOKUPP:
		return true;
	default:
		return false;
	}
}

/*
 * Kerberos 5 OID: 1.2.840.113554.1.2.2 (DER encoding).
 */
static const char krb5_oid_der[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7,
				     0x12, 0x01, 0x02, 0x02 };
#define KRB5_OID_LEN ((uint32_t)sizeof(krb5_oid_der))

/*
 * Find the "next real op" after the current op, skipping SAVEFH.
 *
 * RFC 8881 S2.6.3.1.1.1: when determining whether WRONGSEC applies,
 * the server "pretends SAVEFH is not in the series of operations"
 * and looks past it to the real next operation.
 *
 * Returns the opnum of the next real op, or -1 if the current op
 * is effectively the last one (S2.6.3.1.1.6: put-FH + nothing).
 */
static int next_real_op(struct compound *compound)
{
	u_int next = compound->c_curr_op + 1;
	u_int len = compound->c_args->argarray.argarray_len;

	/* Skip over any intervening SAVEFH ops. */
	while (next < len &&
	       compound->c_args->argarray.argarray_val[next].argop == OP_SAVEFH)
		next++;

	if (next >= len)
		return -1;

	return (int)compound->c_args->argarray.argarray_val[next].argop;
}

static bool op_is_putfh(nfs_opnum4 op)
{
	return op == OP_PUTFH || op == OP_PUTROOTFH || op == OP_PUTPUBFH ||
	       op == OP_RESTOREFH;
}

/*
 * nfs4_putfh_should_check_wrongsec -- determine whether a put-FH op
 * should enforce WRONGSEC against the target export's flavor list.
 *
 * RFC 8881 S2.6.3.1 rules (in priority order, after skipping SAVEFH):
 *  1. Next real op is SECINFO/SECINFO_NO_NAME: false  (S2.6.3.1.1.5)
 *  2. Next real op is LOOKUP or LOOKUPP: false         (S2.6.3.1.1.3/4)
 *  3. Next real op is OPEN with CLAIM_NULL or
 *     CLAIM_DELEGATE_CUR: false                        (S2.6.3.1.1.3)
 *  4. Next real op is another put-FH: false            (S2.6.3.1.1.2)
 *  5. No next op (end of compound): false              (S2.6.3.1.1.6)
 *  6. Otherwise (including OPEN by FH): true           (S2.6.3.1.1.7)
 */
bool nfs4_putfh_should_check_wrongsec(struct compound *compound)
{
	int op = next_real_op(compound);

	/* S2.6.3.1.1.6: put-FH + nothing */
	if (op < 0)
		return false;

	/* S2.6.3.1.1.5: put-FH + SECINFO/SECINFO_NO_NAME */
	if (op == OP_SECINFO || op == OP_SECINFO_NO_NAME)
		return false;

	/* S2.6.3.1.1.3: put-FH + LOOKUP */
	/* S2.6.3.1.1.4: put-FH + LOOKUPP */
	if (op == OP_LOOKUP || op == OP_LOOKUPP)
		return false;

	/* S2.6.3.1.1.3: put-FH + OPEN by component name */
	if (op == OP_OPEN) {
		u_int next = compound->c_curr_op + 1;
		u_int len = compound->c_args->argarray.argarray_len;

		while (next < len &&
		       compound->c_args->argarray.argarray_val[next].argop ==
			       OP_SAVEFH)
			next++;
		if (next < len) {
			nfs_argop4 *argop =
				&compound->c_args->argarray.argarray_val[next];
			open_claim_type4 claim =
				argop->nfs_argop4_u.opopen.claim.claim;
			if (claim == CLAIM_NULL || claim == CLAIM_DELEGATE_CUR)
				return false;
		}
	}

	/* S2.6.3.1.1.2: put-FH + another put-FH */
	if (op_is_putfh((nfs_opnum4)op))
		return false;

	/* S2.6.3.1.1.7: put-FH + anything else */
	return true;
}

/*
 * flavor_matches - check whether a single export flavor entry matches
 * the client's RPC auth flavor (and, for GSS, service level).
 */
static bool flavor_matches(enum reffs_auth_flavor f, uint32_t client_flavor,
			   bool client_tls, const struct rpc_cred *cred)
{
	switch (f) {
	case REFFS_AUTH_TLS:
		/* TLS pseudo-flavor: AUTH_SYS over TLS transport. */
		return client_tls && client_flavor == AUTH_SYS;
	case REFFS_AUTH_KRB5:
		return client_flavor == RPCSEC_GSS;
	case REFFS_AUTH_KRB5I:
		return client_flavor == RPCSEC_GSS &&
		       cred->rc_gss.gc_svc >= RPC_GSS_SVC_INTEGRITY;
	case REFFS_AUTH_KRB5P:
		return client_flavor == RPCSEC_GSS &&
		       cred->rc_gss.gc_svc == RPC_GSS_SVC_PRIVACY;
	default:
		return (uint32_t)f == client_flavor;
	}
}

/*
 * nfs4_check_wrongsec - enforce per-client export flavor restrictions.
 *
 * Called from FH-changing ops (PUTFH, PUTROOTFH, LOOKUP, etc.).
 *
 * Algorithm:
 *  1. If the sb has client rules, look up the matched rule for this
 *     connection's peer address via client_rule_match().
 *     - No matching rule: return NFS4ERR_ACCESS (client not allowed).
 *     - Matching rule found: apply root_squash / all_squash to c_ap,
 *       then check the rule's scr_flavors[].
 *  2. If the sb has no client rules, fall back to the global
 *     server_state flavor list (legacy path).
 *
 * SECINFO uses sb_all_flavors (union of all rule flavors) via
 * nfs4_build_secinfo(); this function is a pure flavor-vs-rule check.
 */
nfsstat4 nfs4_check_wrongsec(struct compound *compound)
{
	uint32_t client_flavor = compound->c_rt->rt_info.ri_cred.rc_flavor;
	const struct rpc_cred *cred = &compound->c_rt->rt_info.ri_cred;
	bool client_tls = compound->c_rt->rt_fd >= 0 &&
			  io_conn_is_tls_enabled(compound->c_rt->rt_fd);
	const enum reffs_auth_flavor *flavors;
	unsigned int nflavors;
	uint32_t curr_opnum =
		compound->c_args->argarray.argarray_val[compound->c_curr_op]
			.argop;

	/*
	 * Slice 6b-ii: registered Proxy Server bypass.  A client whose
	 * PROXY_REGISTRATION succeeded (slice 6b-i) gets nc_is_registered_ps
	 * set; on namespace-discovery ops only, we skip both the
	 * client-rule peer match and the flavor check, granting the PS
	 * the narrow privilege it needs to walk the MDS namespace.
	 *
	 * Data-access ops (OPEN, READ, etc.) still hit the normal path.
	 * The forwarded-credentials story for those ops is unchanged --
	 * see proxy-server.md "Privilege model".
	 *
	 * Audit at TRACE: every bypassed compound emits one line so an
	 * operator with the trace category enabled can review the
	 * namespace-shape disclosure after the fact.  LOG would flood
	 * (a healthy PS triggers this on every traversal).
	 */
	if (compound->c_nfs4_client &&
	    atomic_load_explicit(&compound->c_nfs4_client->nc_is_registered_ps,
				 memory_order_acquire) &&
	    op_is_namespace_discovery(curr_opnum)) {
		TRACE("PS-bypass: op=%u client_flavor=%u tls=%d sb_id=%lu",
		      curr_opnum, client_flavor, client_tls,
		      compound->c_curr_sb ?
			      (unsigned long)compound->c_curr_sb->sb_id :
			      0UL);
		return NFS4_OK;
	}

	/*
	 * Per-export client rule path: match this connection's peer
	 * against the sb's ordered rule list.
	 */
	if (compound->c_curr_sb && compound->c_curr_sb->sb_nclient_rules > 0) {
		const struct sockaddr_storage *peer =
			&compound->c_rt->rt_info.ri_ci.ci_peer;
		const struct sb_client_rule *rule = client_rule_match(
			compound->c_curr_sb->sb_client_rules,
			compound->c_curr_sb->sb_nclient_rules, peer);

		if (!rule) {
			TRACE("ACCESS denied: no matching client rule for peer");
			return NFS4ERR_ACCESS;
		}

		/* Apply squash to the compound's working credential. */
		rpc_cred_squash(&compound->c_ap, rule);

		flavors = rule->scr_flavors;
		nflavors = rule->scr_nflavors;
	} else {
		/* Legacy path: no per-client rules -- use global flavors. */
		struct server_state *ss = compound->c_server_state;

		flavors = ss->ss_flavors;
		nflavors = ss->ss_nflavors;
	}

	if (nflavors == 0)
		return NFS4_OK;

	for (unsigned int i = 0; i < nflavors; i++) {
		if (flavor_matches(flavors[i], client_flavor, client_tls, cred))
			return NFS4_OK;
	}

	TRACE("WRONGSEC: client flavor %u tls=%d not in export rule flavors",
	      client_flavor, client_tls);
	return NFS4ERR_WRONGSEC;
}

/*
 * nfs4_build_secinfo - populate a SECINFO4resok with export flavors.
 *
 * Used by both SECINFO and SECINFO_NO_NAME.  Returns NFS4_OK on
 * success, NFS4ERR_DELAY on allocation failure.
 */
nfsstat4 nfs4_build_secinfo(struct compound *compound, SECINFO4resok *resok)
{
	const enum reffs_auth_flavor *flavors;
	unsigned int nflavors;

	if (compound->c_curr_sb && compound->c_curr_sb->sb_nall_flavors > 0) {
		flavors = compound->c_curr_sb->sb_all_flavors;
		nflavors = compound->c_curr_sb->sb_nall_flavors;
	} else {
		struct server_state *ss = compound->c_server_state;

		flavors = ss->ss_flavors;
		nflavors = ss->ss_nflavors;
	}

	if (nflavors == 0) {
		/* Fallback: advertise AUTH_SYS. */
		resok->SECINFO4resok_val = calloc(1, sizeof(secinfo4));
		if (!resok->SECINFO4resok_val)
			return NFS4ERR_DELAY;
		resok->SECINFO4resok_len = 1;
		resok->SECINFO4resok_val[0].flavor = AUTH_SYS;
		return NFS4_OK;
	}

	/*
	 * Build a deduped flavor list.  TLS and SYS both map to
	 * AUTH_SYS on the wire, so ["sys", "tls"] produces one entry.
	 * Allocate worst-case (nflavors) and trim.
	 */
	unsigned int max = nflavors;

	resok->SECINFO4resok_val = calloc(max, sizeof(secinfo4));
	if (!resok->SECINFO4resok_val)
		return NFS4ERR_DELAY;

	unsigned int out = 0;
	bool have_auth_sys = false;

	for (unsigned int i = 0; i < max; i++) {
		secinfo4 *si = &resok->SECINFO4resok_val[out];
		rpc_Gss_Svc_t svc;

		switch (flavors[i]) {
		case REFFS_AUTH_SYS:
		case REFFS_AUTH_TLS:
			/*
			 * TLS is transport-level, not an RPC flavor.
			 * Advertise AUTH_SYS -- the client negotiates
			 * TLS separately via AUTH_TLS NULL RPC.
			 */
			if (have_auth_sys)
				continue;
			have_auth_sys = true;
			si->flavor = AUTH_SYS;
			out++;
			break;
		case REFFS_AUTH_KRB5:
			svc = RPC_GSS_SVC_NONE;
			goto fill_gss;
		case REFFS_AUTH_KRB5I:
			svc = RPC_GSS_SVC_INTEGRITY;
			goto fill_gss;
		case REFFS_AUTH_KRB5P:
			svc = RPC_GSS_SVC_PRIVACY;
fill_gss:
			si->flavor = RPCSEC_GSS;
			si->secinfo4_u.flavor_info.oid.sec_oid4_val =
				malloc(KRB5_OID_LEN);
			if (!si->secinfo4_u.flavor_info.oid.sec_oid4_val) {
				/* Free OIDs from earlier entries. */
				for (unsigned int j = 0; j < out; j++) {
					secinfo4 *prev =
						&resok->SECINFO4resok_val[j];
					free(prev->secinfo4_u.flavor_info.oid
						     .sec_oid4_val);
				}
				free(resok->SECINFO4resok_val);
				resok->SECINFO4resok_val = NULL;
				return NFS4ERR_DELAY;
			}
			memcpy(si->secinfo4_u.flavor_info.oid.sec_oid4_val,
			       krb5_oid_der, KRB5_OID_LEN);
			si->secinfo4_u.flavor_info.oid.sec_oid4_len =
				KRB5_OID_LEN;
			si->secinfo4_u.flavor_info.qop = 0;
			si->secinfo4_u.flavor_info.service = svc;
			out++;
			break;
		default:
			si->flavor = AUTH_NONE;
			out++;
			break;
		}
	}

	resok->SECINFO4resok_len = out;
	return NFS4_OK;
}

/*
 * nfs4_check_rofs - enforce per-client read-only export restriction.
 *
 * Returns NFS4ERR_ROFS when the matched client rule has scr_rw=false.
 * Returns NFS4_OK when the export allows writes from this client.
 *
 * Called from mutating ops (WRITE, CREATE, REMOVE, RENAME, SETATTR,
 * LINK, etc.) when the current filehandle belongs to a per-rule sb.
 *
 * NOT_NOW_BROWN_COW: wire into every write op handler.  The function
 * and matching logic are ready; the call sites are deferred to keep
 * this step focused on the security enforcement framework.
 */
nfsstat4 nfs4_check_rofs(struct compound *compound)
{
	if (!compound->c_curr_sb || compound->c_curr_sb->sb_nclient_rules == 0)
		return NFS4_OK;

	const struct sockaddr_storage *peer =
		&compound->c_rt->rt_info.ri_ci.ci_peer;
	const struct sb_client_rule *rule =
		client_rule_match(compound->c_curr_sb->sb_client_rules,
				  compound->c_curr_sb->sb_nclient_rules, peer);

	if (!rule)
		return NFS4ERR_ACCESS;

	if (!rule->scr_rw) {
		TRACE("ROFS: client %s attempted write on read-only export",
		      compound->c_rt->rt_addr_str ?
			      compound->c_rt->rt_addr_str :
			      "(unknown)");
		return NFS4ERR_ROFS;
	}

	return NFS4_OK;
}
