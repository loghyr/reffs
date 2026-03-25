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
#include "reffs/rpc.h"
#include "reffs/server.h"
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
	struct server_state *ss = server_state_find();

	if (!ss || ss->ss_nflavors == 0) {
		server_state_put(ss);
		return NFS4_OK;
	}

	/* SECINFO lookahead: let the client discover flavors. */
	if (next_op_is_secinfo(compound)) {
		server_state_put(ss);
		return NFS4_OK;
	}

	uint32_t client_flavor = compound->c_rt->rt_info.ri_cred.rc_flavor;

	for (unsigned int i = 0; i < ss->ss_nflavors; i++) {
		if ((uint32_t)ss->ss_flavors[i] == client_flavor) {
			server_state_put(ss);
			return NFS4_OK;
		}
	}

	server_state_put(ss);
	TRACE("WRONGSEC: client flavor %u not in export flavor list",
	      client_flavor);
	return NFS4ERR_WRONGSEC;
}

/*
 * nfs4_build_secinfo - populate a SECINFO4resok with export flavors.
 *
 * Used by both SECINFO and SECINFO_NO_NAME.  Returns NFS4_OK on
 * success, NFS4ERR_DELAY on allocation failure.
 */
nfsstat4 nfs4_build_secinfo(SECINFO4resok *resok)
{
	struct server_state *ss = server_state_find();

	if (!ss || ss->ss_nflavors == 0) {
		server_state_put(ss);
		/* Fallback: advertise AUTH_SYS. */
		resok->SECINFO4resok_val = calloc(1, sizeof(secinfo4));
		if (!resok->SECINFO4resok_val)
			return NFS4ERR_DELAY;
		resok->SECINFO4resok_len = 1;
		resok->SECINFO4resok_val[0].flavor = AUTH_SYS;
		return NFS4_OK;
	}

	unsigned int n = ss->ss_nflavors;

	resok->SECINFO4resok_val = calloc(n, sizeof(secinfo4));
	if (!resok->SECINFO4resok_val) {
		server_state_put(ss);
		return NFS4ERR_DELAY;
	}
	resok->SECINFO4resok_len = n;

	for (unsigned int i = 0; i < n; i++) {
		secinfo4 *si = &resok->SECINFO4resok_val[i];
		rpc_Gss_Svc_t svc;

		switch (ss->ss_flavors[i]) {
		case REFFS_AUTH_SYS:
			si->flavor = AUTH_SYS;
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
				server_state_put(ss);
				return NFS4ERR_DELAY;
			}
			memcpy(si->secinfo4_u.flavor_info.oid.sec_oid4_val,
			       krb5_oid_der, KRB5_OID_LEN);
			si->secinfo4_u.flavor_info.oid.sec_oid4_len =
				KRB5_OID_LEN;
			si->secinfo4_u.flavor_info.qop = 0;
			si->secinfo4_u.flavor_info.service = svc;
			break;
		default:
			si->flavor = AUTH_NONE;
			break;
		}
	}

	server_state_put(ss);
	return NFS4_OK;
}
