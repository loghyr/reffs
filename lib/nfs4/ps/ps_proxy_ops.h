/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_PS_PROXY_OPS_H
#define _REFFS_PS_PROXY_OPS_H

#include <stdint.h>

struct mds_session; /* lib/nfs4/client/ec_client.h */

/*
 * Forwarded-op plumbing for the proxy-server listeners.
 *
 * The NFSv4.2 server path on a proxy SB needs to forward certain
 * ops (GETATTR first, OPEN / LOOKUP later) to the upstream MDS
 * across the listener's cached mds_session.  These helpers
 * encapsulate the compound construction + response extraction so
 * op handlers stay readable and the XDR types do not leak into
 * the dispatch code.
 *
 * See .claude/design/proxy-server.md phase 2 "Client GETATTR on a
 * proxied file".
 */

/*
 * Caller-owned result from ps_proxy_forward_getattr.  Both buffers
 * are heap-allocated inside the forward call (or NULL if the MDS
 * returned empty-length fields) and must be released via
 * ps_proxy_getattr_reply_free() exactly once.
 *
 * attrmask is the NFSv4 bitmap4: an array of uint32_t words;
 * attrmask_len is the count of words (matches bitmap4.bitmap4_len
 * on the wire).  attr_vals is the XDR-packed attribute payload
 * matching the bits set in attrmask; attr_vals_len is the byte
 * count.  Keeping the reply in raw-bytes form avoids pulling the
 * generated nfsv42_xdr.h into this header -- callers that need
 * typed access unpack into their own fattr4 struct.
 */
struct ps_proxy_getattr_reply {
	uint32_t *attrmask;
	uint32_t attrmask_len;
	uint8_t *attr_vals;
	uint32_t attr_vals_len;
};

/*
 * Build and send SEQUENCE + PUTFH(upstream_fh) + GETATTR(requested)
 * on `ms`, then copy the MDS's reply into `reply` (allocating the
 * two heap buffers described above).  On any failure, `reply`
 * fields are left zero-initialised and no buffers are allocated.
 *
 * NOT_NOW_BROWN_COW (both deferred to slice 2e-iv-c):
 *
 *   1. Credential forwarding.  Today the compound carries whatever
 *      credentials the listener's mds_session was opened with
 *      (typically the PS's service uid/gid).  The proxy-server
 *      design requires the END CLIENT'S AUTH_SYS credentials to
 *      ride on the forwarded op so the MDS applies its own export
 *      policy to the real caller.  That needs a per-compound creds
 *      override on the mds_session's TIRPC handle, which
 *      mds_compound_* does not expose today.
 *
 *   2. FSID remap.  If the requested_mask includes FATTR4_FSID the
 *      MDS's fsid gets forwarded verbatim; a client would then see
 *      the MDS's fsid crossing into the proxy namespace instead of
 *      the proxy SB's own fsid.  For BAT this is acceptable
 *      because the client does not cross filesystem boundaries on
 *      the proxy mount; a follow-up will rewrite FATTR4_FSID out
 *      of attr_vals when present.
 *
 * Returns:
 *   0        success
 *   -EINVAL  ms / upstream_fh / requested_mask / reply is NULL,
 *            upstream_fh_len is 0, or requested_mask_len is 0
 *   -E2BIG   upstream_fh_len > NFS4_FHSIZE (128 bytes, RFC 8881)
 *   -ENOMEM  heap allocation failure
 *   -errno   RPC / compound failure, or a non-OK per-op status
 */
int ps_proxy_forward_getattr(struct mds_session *ms, const uint8_t *upstream_fh,
			     uint32_t upstream_fh_len,
			     const uint32_t *requested_mask,
			     uint32_t requested_mask_len,
			     struct ps_proxy_getattr_reply *reply);

/*
 * Release any buffers allocated by ps_proxy_forward_getattr() into
 * `reply` and zero the struct.  NULL-safe for both the struct
 * itself and the inner pointers -- safe to call on a reply that
 * never saw a successful forward.
 */
void ps_proxy_getattr_reply_free(struct ps_proxy_getattr_reply *reply);

#endif /* _REFFS_PS_PROXY_OPS_H */
