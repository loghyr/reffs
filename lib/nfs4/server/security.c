/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * Export security enforcement — NFS4ERR_WRONGSEC and SECINFO helpers.
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

#include <stdlib.h>
#include <string.h>
#include <rpc/auth.h>

#include "nfsv42_xdr.h"
#include "reffs/log.h"
#include "reffs/io.h"
#include "reffs/rpc.h"
#include "reffs/server.h"
#include "reffs/settings.h"
#include "nfs4/compound.h"
#include "nfs4/ops.h"

/*
 * Kerberos 5 OID: 1.2.840.113554.1.2.2 (DER encoding).
 */
static const char krb5_oid_der[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7,
				     0x12, 0x01, 0x02, 0x02 };
#define KRB5_OID_LEN ((uint32_t)sizeof(krb5_oid_der))

/*
 * Check whether the next operation in the compound is SECINFO or
 * SECINFO_NO_NAME.  If so, the client is negotiating security and
 * the current FH-changing op must not return WRONGSEC.
 */
static bool next_op_is_secinfo(struct compound *compound)
{
	u_int next = compound->c_curr_op + 1;

	if (next >= compound->c_args->argarray.argarray_len)
		return false;

	nfs_opnum4 op = compound->c_args->argarray.argarray_val[next].argop;

	return op == OP_SECINFO || op == OP_SECINFO_NO_NAME;
}

/*
 * nfs4_check_wrongsec - enforce export flavor restrictions.
 *
 * Called from FH-changing ops (PUTFH, PUTROOTFH, LOOKUP, etc.).
 * Returns NFS4_OK if the client's auth flavor is allowed, or
 * NFS4ERR_WRONGSEC if not.
 */
nfsstat4 nfs4_check_wrongsec(struct compound *compound)
{
	/*
	 * Use per-sb flavors if the current sb has them configured;
	 * otherwise fall back to the global server_state flavors.
	 */
	const enum reffs_auth_flavor *flavors;
	unsigned int nflavors;

	if (compound->c_curr_sb && compound->c_curr_sb->sb_nflavors > 0) {
		flavors = compound->c_curr_sb->sb_flavors;
		nflavors = compound->c_curr_sb->sb_nflavors;
	} else {
		struct server_state *ss = compound->c_server_state;

		flavors = ss->ss_flavors;
		nflavors = ss->ss_nflavors;
	}

	if (nflavors == 0)
		return NFS4_OK;

	/* SECINFO lookahead: let the client discover flavors. */
	if (next_op_is_secinfo(compound))
		return NFS4_OK;

	uint32_t client_flavor = compound->c_rt->rt_info.ri_cred.rc_flavor;
	struct conn_info *ci = io_conn_get(compound->c_rt->rt_fd);
	bool client_tls = ci && ci->ci_tls_enabled;

	for (unsigned int i = 0; i < nflavors; i++) {
		switch (flavors[i]) {
		case REFFS_AUTH_TLS:
			/* TLS pseudo-flavor: AUTH_SYS over TLS transport. */
			if (client_tls && client_flavor == AUTH_SYS)
				return NFS4_OK;
			break;
		case REFFS_AUTH_KRB5:
			if (client_flavor == RPCSEC_GSS)
				return NFS4_OK;
			break;
		case REFFS_AUTH_KRB5I:
			if (client_flavor == RPCSEC_GSS &&
			    compound->c_rt->rt_info.ri_cred.rc_gss.gc_svc >=
				    RPC_GSS_SVC_INTEGRITY)
				return NFS4_OK;
			break;
		case REFFS_AUTH_KRB5P:
			if (client_flavor == RPCSEC_GSS &&
			    compound->c_rt->rt_info.ri_cred.rc_gss.gc_svc ==
				    RPC_GSS_SVC_PRIVACY)
				return NFS4_OK;
			break;
		default:
			if ((uint32_t)flavors[i] == client_flavor)
				return NFS4_OK;
			break;
		}
	}

	TRACE("WRONGSEC: client flavor %u tls=%d not in export flavor list",
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

	if (compound->c_curr_sb && compound->c_curr_sb->sb_nflavors > 0) {
		flavors = compound->c_curr_sb->sb_flavors;
		nflavors = compound->c_curr_sb->sb_nflavors;
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
			 * Advertise AUTH_SYS — the client negotiates
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
