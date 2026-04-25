/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_PS_SECURITY_H
#define _REFFS_PS_SECURITY_H

#include <stdbool.h>

struct compound; /* lib/nfs4/include/nfs4/compound.h */

/*
 * Security guards for proxy-server fast paths.
 *
 * Today the PS forwards every upstream compound under its own
 * AUTH_SYS service credentials (slice 2e-iv-c -- credential
 * forwarding -- threads the END CLIENT's AUTH_SYS uid/gid through;
 * see ps_proxy_ops.h).  RPCSEC_GSS-authed end-client compounds
 * cannot be forwarded under that model: the upstream MDS would
 * see the PS's service principal in place of the end client's,
 * which is a real security finding (the MDS would apply the wrong
 * principal to ACL / krb5p decisions).
 *
 * Full RPCSEC_GSSv3 forwarding (RFC 7861 structured privilege
 * assertion -- "I am acting on behalf of principal X") is the
 * documented path forward.  See proxy-server.md "Action Items"
 * item 3.  Until that lands, refuse GSS-authed compounds at the
 * proxy fast-path entry rather than silently downgrading.
 */

/*
 * Returns true if the inbound compound was authenticated with
 * RPCSEC_GSS.  Caller -- the per-op proxy fast-path -- handles
 * the refusal:
 *
 *   if (ps_proxy_compound_is_gss(compound)) {
 *           TRACE("proxy <op>: refused -- RPCSEC_GSS forwarding "
 *                 "not yet implemented");
 *           *status = NFS4ERR_WRONGSEC;
 *           goto out;
 *   }
 *
 * NFS4ERR_WRONGSEC tells the client "wrong mechanism for this
 * object."  Clients fall through to SECINFO; SECINFO returns the
 * configured flavor list (which still advertises krb5 as the
 * export's policy), so this surfaces to userspace as an EACCES /
 * permission denied rather than an infinite renegotiation loop.
 *
 * Loop-safe only when the proxy export's client rule list does
 * NOT include a GSS flavor.  An admin who adds krb5 to a proxy
 * SB's rule list before RPCSEC_GSSv3 forwarding lands will see
 * the client SECINFO-renegotiate-to-krb5-and-retry cycle (Linux
 * kernel client surfaces the second WRONGSEC as EACCES at mount,
 * but the wire pattern is a loop).  Today's PS forwarding only
 * supports AUTH_SYS, so a krb5 proxy rule is a deployment-time
 * misconfiguration; this slice does not introduce the hazard,
 * it surfaces an existing one.
 *
 * NULL-safe: returns false if compound or compound->c_rt is NULL
 * (no inbound credential to inspect).
 */
bool ps_proxy_compound_is_gss(const struct compound *compound);

#endif /* _REFFS_PS_SECURITY_H */
