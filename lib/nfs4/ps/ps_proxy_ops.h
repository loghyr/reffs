/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_PS_PROXY_OPS_H
#define _REFFS_PS_PROXY_OPS_H

#include <stdbool.h>
#include <stdint.h>

#include "ps_state.h" /* PS_MAX_FH_SIZE */

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
 * Minimum attrs extracted from a forwarded GETATTR reply.  Exactly
 * what ps_lookup_materialize needs to distinguish a directory from
 * a regular file and establish an initial mode on the new inode.
 * Other attrs (size, times, identity) are lazily populated by the
 * next real GETATTR the client issues, which the forwarding path
 * already handles in full via ps_proxy_forward_getattr.
 */
struct ps_proxy_attrs_min {
	bool have_type; /* FATTR4_TYPE (bit 1) present in reply */
	uint32_t type; /* nfs_ftype4 value, valid only if have_type */
	bool have_mode; /* FATTR4_MODE (bit 33) present in reply */
	uint32_t mode; /* mode4 payload, valid only if have_mode */
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
 * caller-supplied buffer.  When `attr_request` is non-NULL with
 * `attr_request_len > 0` and `attrs_out` is non-NULL, a GETATTR
 * for `attr_request` is appended to the same compound and the
 * reply is parsed into `attrs_out` via ps_proxy_parse_attrs_min
 * -- so the TYPE/MODE needed to materialise the child arrive in
 * the same round-trip.  Pass NULL for `attr_request` or
 * `attrs_out` to skip the GETATTR entirely.
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
 * NOT_NOW_BROWN_COW: credential forwarding + FSID remap are slice
 * 2e-iv-c concerns and apply identically to LOOKUP-forwarding.
 *
 * Returns:
 *   0        success -- child FH copied, length in *child_fh_len_out;
 *            if attrs were requested, *attrs_out populated (have_*
 *            reflect which attrs the MDS returned)
 *   -EINVAL  ms / parent_fh / name / child_fh_buf / child_fh_len_out
 *            is NULL, parent_fh_len / name_len is 0, or the GETATTR
 *            fattr4 reply is malformed
 *   -E2BIG   parent_fh_len > PS_MAX_FH_SIZE
 *   -ENOSPC  child_fh_buf_len is smaller than the returned FH
 *   -ENOENT  upstream returned NFS4ERR_NOENT (missing child)
 *   -ENOTSUP the GETATTR reply contains an attr this parser does
 *            not recognise (see ps_proxy_parse_attrs_min)
 *   -errno   RPC / compound failure, or any other per-op status
 */
int ps_proxy_forward_lookup(struct mds_session *ms, const uint8_t *parent_fh,
			    uint32_t parent_fh_len, const char *name,
			    uint32_t name_len, uint8_t *child_fh_buf,
			    uint32_t child_fh_buf_len,
			    uint32_t *child_fh_len_out,
			    const uint32_t *attr_request,
			    uint32_t attr_request_len,
			    struct ps_proxy_attrs_min *attrs_out);

/*
 * Parse a fattr4 reply (as returned by ps_proxy_forward_getattr in
 * `attrmask` + `attr_vals`) into ps_proxy_attrs_min.  Only FATTR4_TYPE
 * (RFC 8881 S5.8.1 bit 1) and FATTR4_MODE (RFC 8881 S5.8.2.15 bit 33)
 * are recognised; any other bit set in attrmask returns -ENOTSUP
 * because advancing past an unknown attribute would require the full
 * size table and this parser is deliberately minimal.
 *
 * Framing is strict: attribute values appear in ascending bit order,
 * TYPE and MODE each occupy 4 bytes big-endian (RFC 8881 S3.1),
 * and the cursor must reach attr_vals_len exactly at end.  A zero-
 * length attrmask paired with a zero-length attr_vals is legal and
 * returns an empty ps_proxy_attrs_min with both have_* false.
 *
 * Returns:
 *   0        success -- `out` populated; have_* reflect which bits
 *            were present in the reply
 *   -EINVAL  NULL args, truncated buffer, trailing bytes, or mask
 *            non-empty with attr_vals empty (framing mismatch)
 *   -ENOTSUP attrmask contains a bit this parser does not handle
 */
int ps_proxy_parse_attrs_min(const uint32_t *attrmask, uint32_t attrmask_len,
			     const uint8_t *attr_vals, uint32_t attr_vals_len,
			     struct ps_proxy_attrs_min *out);

/*
 * RFC 8881 stateid4.other is 12 bytes.  Mirrored here so callers can
 * pass stateids without pulling nfsv42_xdr.h into headers that don't
 * already include it.
 */
#define PS_STATEID_OTHER_SIZE 12

/*
 * Caller-owned result from ps_proxy_forward_read.  `data` is a heap
 * buffer allocated inside the forward call (NULL when the MDS
 * returned zero bytes); release via ps_proxy_read_reply_free()
 * exactly once.  `eof` is the upstream's signal that the read
 * reached end-of-file -- propagate verbatim to the client.
 */
struct ps_proxy_read_reply {
	uint8_t *data;
	uint32_t data_len;
	bool eof;
};

/*
 * Build and send SEQUENCE + PUTFH(upstream_fh) + READ on `ms`, then
 * copy the MDS's payload into `reply`.  `stateid_seqid` + the 12-byte
 * `stateid_other` make up a stateid4 on the wire; today the only
 * caller passes the anonymous stateid (all-zeros) because OPEN
 * forwarding has not landed yet and the anonymous stateid is the
 * only one a client can legally use without a prior OPEN
 * (RFC 8881 S8.2.3).
 *
 * On any failure `reply` fields are left zero-initialised and no
 * buffer is allocated.
 *
 * NOT_NOW_BROWN_COW (slice 2e-iv-c): credential forwarding.  Today
 * the compound rides on the PS session's credentials, not the end
 * client's -- same caveat as the GETATTR / LOOKUP forwarders.
 *
 * Returns:
 *   0        success; reply->data / data_len / eof populated
 *   -EINVAL  ms / upstream_fh / stateid_other / reply NULL, or
 *            upstream_fh_len / count is 0
 *   -E2BIG   upstream_fh_len > PS_MAX_FH_SIZE
 *   -ENOMEM  heap allocation failure
 *   -errno   RPC / compound failure, or a non-OK per-op status
 */
int ps_proxy_forward_read(struct mds_session *ms, const uint8_t *upstream_fh,
			  uint32_t upstream_fh_len, uint32_t stateid_seqid,
			  const uint8_t stateid_other[PS_STATEID_OTHER_SIZE],
			  uint64_t offset, uint32_t count,
			  struct ps_proxy_read_reply *reply);

/*
 * Release any buffer allocated by ps_proxy_forward_read into
 * `reply` and zero the struct.  NULL-safe for the struct and the
 * inner pointer.
 */
void ps_proxy_read_reply_free(struct ps_proxy_read_reply *reply);

/*
 * Recognised open_claim_type4 values for the OPEN forwarder.
 * Mirroring just the two we support keeps callers off the
 * nfsv42_xdr.h dependency.
 *
 *   PS_PROXY_OPEN_CLAIM_NULL -- CURRENT_FH is the parent directory;
 *     primitive sends OPEN with claim={CLAIM_NULL, name}.  Linux
 *     NFSv4 client uses this shape.
 *   PS_PROXY_OPEN_CLAIM_FH   -- CURRENT_FH is the target file;
 *     primitive sends OPEN with claim={CLAIM_FH}.  FreeBSD NFSv4
 *     client uses this shape (open after LOOKUP-supplied FH).
 */
#define PS_PROXY_OPEN_CLAIM_NULL 0
#define PS_PROXY_OPEN_CLAIM_FH 4

/*
 * OPEN forwarder input.  Scope is deliberately narrow:
 *   - CLAIM_NULL (with name) + NOCREATE
 *   - CLAIM_FH   (no name)   + NOCREATE
 * Together these cover the open shapes a Linux or FreeBSD NFSv4
 * client uses before the first READ of an existing file.  CREATE-
 * mode, CLAIM_PREVIOUS, delegation claims, and attr-carrying
 * opens are separate slices; the hook in nfs4_op_open rejects
 * them with NFS4ERR_NOTSUPP.
 *
 * Owner handling: `owner_clientid` is the client's clientid4 and
 * `owner_data` is the client's opaque open-owner bytes.  Both get
 * forwarded verbatim as the open_owner4 on the MDS-facing compound.
 * An end client that reuses the same (clientid, owner) tuple sees
 * the MDS's idempotent OPEN semantics -- no local open-owner table
 * on the PS.  Future work (credential forwarding slice 2e-iv-c) may
 * wrap owner_data to disambiguate multiple end-clients that collide.
 */
struct ps_proxy_open_request {
	uint32_t claim_type; /* PS_PROXY_OPEN_CLAIM_{NULL,FH} */
	uint32_t seqid;
	uint32_t share_access;
	uint32_t share_deny;
	uint64_t owner_clientid;
	const uint8_t *owner_data;
	uint32_t owner_data_len;
};

/*
 * OPEN forwarder result.  Caller-owned and self-contained (no heap
 * allocations) -- copies out of the compound's XDR buffers so the
 * caller does not have to track compound lifetime.
 *
 *   stateid_seqid / stateid_other -- the MDS's open stateid4.  The
 *     hook returns this verbatim to the end client so the client's
 *     next READ / WRITE / CLOSE rides with a stateid the MDS will
 *     recognise.
 *   rflags -- MDS's OPEN4resok.rflags, forwarded so the client gets
 *     OPEN4_RESULT_LOCKTYPE_POSIX etc. straight from the MDS.
 *   child_fh / child_fh_len -- MDS's GETFH for the opened object.
 *
 * Fields we deliberately drop for this slice: change_info4,
 * attrset (CREATE-only), delegation.  The hook synthesises
 * defaults for those (no delegation, zero cinfo, empty attrset).
 */
struct ps_proxy_open_reply {
	uint32_t stateid_seqid;
	uint8_t stateid_other[PS_STATEID_OTHER_SIZE];
	uint32_t rflags;
	uint8_t child_fh[PS_MAX_FH_SIZE];
	uint32_t child_fh_len;
};

/*
 * Build and send SEQUENCE + PUTFH(current_fh) + OPEN + GETFH on
 * `ms`, copy the MDS's reply into `reply`.
 *
 *   For req->claim_type == PS_PROXY_OPEN_CLAIM_NULL: `current_fh`
 *     is the parent directory and `name` / `name_len` is the child
 *     component to open (CLAIM_NULL.file on the wire).
 *
 *   For req->claim_type == PS_PROXY_OPEN_CLAIM_FH: `current_fh` is
 *     the target file already known to the client and `name` /
 *     `name_len` are ignored (CLAIM_FH carries no filename).
 *
 * On any failure `reply` is left zero-initialised and nothing
 * durable ran on the upstream.
 *
 * NOT_NOW_BROWN_COW: credential forwarding (2e-iv-c) and the OPEN
 * shapes listed above.
 *
 * Returns:
 *   0         success; reply populated
 *   -EINVAL   NULL args, zero lengths where required, unsupported
 *             claim_type, or owner_data_len > internal cap
 *   -E2BIG    current_fh_len > PS_MAX_FH_SIZE
 *   -ENOSPC   child FH larger than PS_MAX_FH_SIZE (MDS misbehaving)
 *   -ENOENT   upstream returned NFS4ERR_NOENT
 *   -errno    RPC / compound failure, or any other per-op status
 */
int ps_proxy_forward_open(struct mds_session *ms, const uint8_t *current_fh,
			  uint32_t current_fh_len, const char *name,
			  uint32_t name_len,
			  const struct ps_proxy_open_request *req,
			  struct ps_proxy_open_reply *reply);

/*
 * NFSv4 verifier4 is 8 bytes (RFC 8881 S3.1).  Mirrored locally so
 * callers can receive WRITE replies without pulling nfsv42_xdr.h.
 */
#define PS_PROXY_VERIFIER_SIZE 8

/*
 * Caller-owned result from ps_proxy_forward_write.  Fully copied
 * out of the compound so no heap lives across the call.
 *
 *   count     -- bytes the MDS committed (may be < data_len).
 *   committed -- stable_how4: UNSTABLE4=0, DATA_SYNC4=1, FILE_SYNC4=2.
 *                Forwarded verbatim so the end client can decide
 *                whether a follow-up COMMIT is required.
 *   verifier  -- write verifier to correlate with a later COMMIT.
 */
struct ps_proxy_write_reply {
	uint32_t count;
	uint32_t committed;
	uint8_t verifier[PS_PROXY_VERIFIER_SIZE];
};

/*
 * Build and send SEQUENCE + PUTFH(upstream_fh) + WRITE on `ms`,
 * copy the MDS's reply into `reply`.
 *
 * stateid_seqid + stateid_other make up the stateid4 on the wire.
 * Today the only expected use is with the MDS's own open stateid
 * (from ps_proxy_forward_open in slice 2e-iv-j), which the proxy
 * pass-through model means the PS returned to the end client
 * verbatim.  The primitive does no local validation; the MDS
 * checks the stateid on its side.
 *
 * On any failure `reply` is left zero-initialised and no durable
 * state ran on the upstream.
 *
 * NOT_NOW_BROWN_COW: credential forwarding (slice 2e-iv-c) and
 * COMMIT forwarding (a follow-up; until then UNSTABLE4 writes
 * cannot be flushed by the client).
 *
 * Returns:
 *   0        success; reply populated
 *   -EINVAL  ms / upstream_fh / stateid_other / data / reply NULL,
 *            or upstream_fh_len / data_len == 0, or unknown stable
 *   -E2BIG   upstream_fh_len > PS_MAX_FH_SIZE
 *   -errno   RPC / compound failure, or a non-OK per-op status
 */
int ps_proxy_forward_write(struct mds_session *ms, const uint8_t *upstream_fh,
			   uint32_t upstream_fh_len, uint32_t stateid_seqid,
			   const uint8_t stateid_other[PS_STATEID_OTHER_SIZE],
			   uint64_t offset, uint32_t stable,
			   const uint8_t *data, uint32_t data_len,
			   struct ps_proxy_write_reply *reply);

#endif /* _REFFS_PS_PROXY_OPS_H */
