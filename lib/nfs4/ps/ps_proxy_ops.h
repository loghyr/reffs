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

/*
 * Forward a LOOKUP of a single pathname component against the
 * upstream MDS.  Sends SEQUENCE + PUTFH(parent_fh) + LOOKUP(name)
 * + GETFH on `ms` and copies the child's upstream FH into the
 * caller-supplied buffer.
 *
 * `name` is a bare component (no '/') UTF-8 string of `name_len`
 * bytes; callers that need multi-component resolution either walk
 * components themselves or use ps_discovery_walk_path (already
 * batched for the discovery path).  Single-component here matches
 * what nfs4_op_lookup sees from a client PUTFH + LOOKUP compound.
 *
 * On success, copies up to `child_fh_buf_len` bytes into
 * `child_fh_buf` and writes the actual length to
 * `*child_fh_len_out`.  If the upstream returns a larger FH than
 * the buffer, returns -ENOSPC and leaves `*child_fh_len_out`
 * untouched -- no partial copy.
 *
 * A missing child component on the upstream returns -ENOENT so
 * the caller can surface NFS4ERR_NOENT to the client without
 * having to re-parse a generic -EREMOTEIO.  Other NFS4ERR_*
 * statuses collapse to -EREMOTEIO.
 *
 * NOT_NOW_BROWN_COW (slice 2e-iv-e integration):
 *
 *   - No caller wiring yet.  The nfs4_op_lookup hook that drives
 *     this primitive lands in the next slice, because it also
 *     needs to decide where the per-child upstream FH gets
 *     stashed on the resulting local inode (sidecar map on the
 *     SB vs. new pointer on struct inode).  That design decision
 *     is the bulk of slice 2e-iv-e.
 *
 *   - Credential forwarding + FSID remap are still 2e-iv-c
 *     concerns and apply identically to LOOKUP-forwarding.
 *
 * Returns:
 *   0        success -- child FH copied, length in *child_fh_len_out
 *   -EINVAL  ms / parent_fh / name / child_fh_buf / child_fh_len_out
 *            is NULL, or parent_fh_len / name_len is 0
 *   -E2BIG   parent_fh_len > PS_MAX_FH_SIZE
 *   -ENOSPC  child_fh_buf_len is smaller than the returned FH
 *   -ENOENT  upstream returned NFS4ERR_NOENT (missing child)
 *   -errno   RPC / compound failure, or any other per-op status
 */
int ps_proxy_forward_lookup(struct mds_session *ms, const uint8_t *parent_fh,
			    uint32_t parent_fh_len, const char *name,
			    uint32_t name_len, uint8_t *child_fh_buf,
			    uint32_t child_fh_buf_len,
			    uint32_t *child_fh_len_out);

#endif /* _REFFS_PS_PROXY_OPS_H */
