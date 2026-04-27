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
struct authunix_parms; /* <rpc/auth_unix.h> */

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
			     const struct authunix_parms *creds,
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
			    const struct authunix_parms *creds,
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
			  const struct authunix_parms *creds,
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

/* opentype4 mirror (RFC 8881 S18.16.1). */
#define PS_PROXY_OPEN_OPENTYPE_NOCREATE 0
#define PS_PROXY_OPEN_OPENTYPE_CREATE 1

/*
 * createmode4 mirror.  Only UNCHECKED and GUARDED are supported by
 * the primitive.  EXCLUSIVE4 / EXCLUSIVE4_1 are forwarded verbatim
 * to the upstream MDS, which owns the verifier-dedup state; the PS
 * does not attempt to mirror that state locally.
 *
 * The Linux NFSv4.2 kernel client uses EXCLUSIVE4_1 by default for
 * any OPEN(CREATE), so this primitive must accept it -- otherwise
 * basic file creation through a kernel-mounted PS fails with
 * NFS4ERR_NOTSUPP.
 */
#define PS_PROXY_OPEN_CREATEMODE_UNCHECKED 0
#define PS_PROXY_OPEN_CREATEMODE_GUARDED 1
#define PS_PROXY_OPEN_CREATEMODE_EXCLUSIVE 2 /* RFC 8881: verifier only */
#define PS_PROXY_OPEN_CREATEMODE_EXCLUSIVE_1 3 /* RFC 8881: verifier + attrs */

/* NFSv4 verifier4 is 8 bytes (RFC 8881 S3.1). */
#define PS_PROXY_OPEN_CREATEVERF_SIZE 8

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
 * Owner handling: `owner_clientid` is the end client's wire-supplied
 * clientid4 (forwarded verbatim; ignored by NFSv4.1+ MDSes).
 * `owner_data` is the OPEN-owner bytes that will hit the wire as
 * open_owner4.owner -- the caller is responsible for prefixing the
 * end-client identity tag (see ps_owner.h, ps_owner_wrap) so two
 * end clients on the same PS session do not collide on the MDS's
 * stateowner table.  The forwarder treats owner_data as opaque from
 * here.
 */
struct ps_proxy_open_request {
	uint32_t claim_type; /* PS_PROXY_OPEN_CLAIM_{NULL,FH} */
	/*
	 * opentype + createmode + createattrs are consulted only when
	 * the caller wants OPEN-with-CREATE (must be paired with
	 * CLAIM_NULL).  For NOCREATE leave opentype=NOCREATE and the
	 * createmode / createattrs fields are ignored.
	 */
	uint32_t opentype; /* PS_PROXY_OPEN_OPENTYPE_{NOCREATE,CREATE} */
	uint32_t createmode; /* PS_PROXY_OPEN_CREATEMODE_* */
	const uint32_t *createattrs_mask; /* bitmap4 words */
	uint32_t createattrs_mask_len;
	const uint8_t *createattrs_vals; /* attrlist4 bytes */
	uint32_t createattrs_vals_len;
	/*
	 * Verifier for EXCLUSIVE4 and EXCLUSIVE4_1.  Caller forwards
	 * the end client's verifier verbatim; the PS does not invent
	 * one.  Ignored for UNCHECKED / GUARDED.
	 */
	uint8_t createverf[PS_PROXY_OPEN_CREATEVERF_SIZE];
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
			  const struct authunix_parms *creds,
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
 * Credential forwarding is wired (slice 2e-iv-c-iii).  COMMIT
 * forwarding is implemented by ps_proxy_forward_commit() below
 * so UNSTABLE4 writes can be flushed by the client.
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
			   const struct authunix_parms *creds,
			   struct ps_proxy_write_reply *reply);

/*
 * Caller-owned result from ps_proxy_forward_commit.  Fully copied
 * out of the compound -- the only durable wire field is the
 * write verifier the upstream returns so the client can detect
 * server restarts that would have invalidated previously-acked
 * unstable writes.
 */
struct ps_proxy_commit_reply {
	uint8_t verifier[PS_PROXY_VERIFIER_SIZE];
};

/*
 * Build and send SEQUENCE + PUTFH(upstream_fh) + COMMIT on `ms`,
 * copy the MDS's writeverf into `reply`.
 *
 * `offset` and `count` carry the byte range the client is asking
 * to flush -- forwarded verbatim to the upstream MDS, which
 * decides whether the range is meaningful (RFC 8881 S18.3 lets
 * a server commit more than the requested range).
 *
 * On any failure `reply` is left zero-initialised and no durable
 * state ran on the upstream.
 *
 * Returns:
 *   0        success; reply->verifier populated
 *   -EINVAL  ms / upstream_fh / reply NULL, or upstream_fh_len == 0
 *   -E2BIG   upstream_fh_len > PS_MAX_FH_SIZE
 *   -errno   RPC / compound failure, or a non-OK per-op status
 */
int ps_proxy_forward_commit(struct mds_session *ms, const uint8_t *upstream_fh,
			    uint32_t upstream_fh_len, uint64_t offset,
			    uint32_t count, const struct authunix_parms *creds,
			    struct ps_proxy_commit_reply *reply);

/*
 * Caller-owned result from ps_proxy_forward_remove.  Carries the
 * upstream MDS's change_info4 verbatim so the end client can detect
 * a concurrent mutation that raced with the REMOVE; the
 * before/after pair is what the server side of REMOVE4resok holds.
 */
struct ps_proxy_remove_reply {
	bool atomic; /* TRUE if before/after are an atomic pair */
	uint64_t before;
	uint64_t after;
};

/*
 * Build and send SEQUENCE + PUTFH(parent_upstream_fh) + REMOVE(name)
 * on `ms`, copy the MDS's change_info4 into `reply`.
 *
 * `name` is a bare component (no '/') of `name_len` bytes.  The
 * primitive does no local cache invalidation -- the client's
 * dcache invalidates on cinfo mismatch the next time it issues
 * a LOOKUP / READDIR through the PS.
 *
 * On any failure `reply` is left zero-initialised and no durable
 * state ran on the upstream.
 *
 * Returns:
 *   0        success; reply->{atomic,before,after} populated
 *   -EINVAL  ms / parent_fh / name / reply NULL, or
 *            parent_fh_len / name_len == 0
 *   -E2BIG   parent_fh_len > PS_MAX_FH_SIZE
 *   -ENOENT  upstream returned NFS4ERR_NOENT (no such name)
 *   -errno   RPC / compound failure, or a non-OK per-op status
 */
int ps_proxy_forward_remove(struct mds_session *ms, const uint8_t *parent_fh,
			    uint32_t parent_fh_len, const char *name,
			    uint32_t name_len,
			    const struct authunix_parms *creds,
			    struct ps_proxy_remove_reply *reply);

/*
 * Caller-owned result from ps_proxy_forward_rename.  Carries both
 * change_info4 values the upstream MDS returns -- source dir +
 * target dir -- so the end client can detect concurrent mutation
 * on either side of the rename.
 */
struct ps_proxy_rename_reply {
	struct {
		bool atomic;
		uint64_t before;
		uint64_t after;
	} source_cinfo;
	struct {
		bool atomic;
		uint64_t before;
		uint64_t after;
	} target_cinfo;
};

/*
 * Build and send SEQUENCE + PUTFH(src) + SAVEFH + PUTFH(dst) +
 * RENAME(oldname, newname) on `ms`, copy both change_info4
 * results into `reply`.  Both directories MUST be on the same
 * upstream MDS (the caller's hook in nfs4_op_rename enforces
 * this by checking both inodes live on the same proxy SB before
 * forwarding).
 *
 * Both names are bare components (no '/') of the indicated
 * lengths.  Like REMOVE, the primitive does no local cache
 * invalidation.
 *
 * On any failure `reply` is left zero-initialised and no durable
 * state ran on the upstream.
 *
 * Returns:
 *   0        success; both cinfo blocks populated
 *   -EINVAL  any pointer NULL, any length 0
 *   -E2BIG   either FH > PS_MAX_FH_SIZE
 *   -ENOENT  upstream returned NFS4ERR_NOENT (oldname missing)
 *   -EEXIST  upstream returned NFS4ERR_EXIST (newname collision)
 *   -errno   RPC / compound failure, or a non-OK per-op status
 */
int ps_proxy_forward_rename(struct mds_session *ms, const uint8_t *src_fh,
			    uint32_t src_fh_len, const char *oldname,
			    uint32_t oldname_len, const uint8_t *dst_fh,
			    uint32_t dst_fh_len, const char *newname,
			    uint32_t newname_len,
			    const struct authunix_parms *creds,
			    struct ps_proxy_rename_reply *reply);

/*
 * Caller-owned result from ps_proxy_forward_mkdir.  Carries enough
 * to populate the wire CREATE4resok the client expects (cinfo +
 * attrset) plus the new directory's upstream FH and minimum attrs
 * the caller's hook needs to materialise a local proxy-SB inode
 * for the new object so subsequent ops in the same compound can
 * see it.
 *
 * `attrset_mask` is heap-allocated by the forwarder (or NULL if
 * the upstream returned an empty bitmap); release with
 * ps_proxy_mkdir_reply_free() exactly once.
 */
struct ps_proxy_mkdir_reply {
	struct {
		bool atomic;
		uint64_t before;
		uint64_t after;
	} cinfo;
	uint32_t *attrset_mask;
	uint32_t attrset_mask_len;
	uint8_t child_fh[PS_MAX_FH_SIZE];
	uint32_t child_fh_len;
	struct ps_proxy_attrs_min child_attrs;
};

/*
 * Build and send SEQUENCE + PUTFH(parent) + CREATE(NF4DIR, name,
 * createattrs) + GETFH + GETATTR(TYPE|MODE) on `ms`, copy the
 * upstream's results into `reply`.  After this returns 0 the
 * caller has everything it needs to materialise a local proxy-SB
 * inode for the new directory and switch the compound's CURRENT_FH
 * to it (matching the on-wire CREATE semantics from RFC 8881
 * S18.4 -- CURRENT_FH transitions to the new object).
 *
 * Per-op error fidelity: NFS4ERR_EXIST -> -EEXIST, NFS4ERR_ACCESS
 * -> -EACCES, etc.  via nfs4_to_errno.
 *
 * On any failure `reply` is left zero-initialised and no durable
 * state ran on the upstream.
 *
 * Returns:
 *   0        success; reply populated
 *   -EINVAL  any pointer NULL, parent_fh / name length 0
 *   -E2BIG   parent_fh > PS_MAX_FH_SIZE
 *   -ENOSPC  child FH > PS_MAX_FH_SIZE (MDS misbehaving)
 *   -EEXIST  upstream returned NFS4ERR_EXIST (name already taken)
 *   -ENOMEM  attrset_mask alloc failure
 *   -errno   RPC / compound failure, or a non-OK per-op status
 */
int ps_proxy_forward_mkdir(struct mds_session *ms, const uint8_t *parent_fh,
			   uint32_t parent_fh_len, const char *name,
			   uint32_t name_len, const uint32_t *createattrs_mask,
			   uint32_t createattrs_mask_len,
			   const uint8_t *createattrs_vals,
			   uint32_t createattrs_vals_len,
			   const struct authunix_parms *creds,
			   struct ps_proxy_mkdir_reply *reply);

/*
 * Release any heap allocated by ps_proxy_forward_mkdir into
 * `reply` and zero the struct.  NULL-safe.
 */
void ps_proxy_mkdir_reply_free(struct ps_proxy_mkdir_reply *reply);

/*
 * Caller-owned result from ps_proxy_forward_symlink.  Same shape
 * as ps_proxy_mkdir_reply -- the only wire difference between
 * CREATE(NF4DIR) and CREATE(NF4LNK) is the createtype4 union
 * payload, not the response.  Free with
 * ps_proxy_symlink_reply_free() exactly once.
 */
struct ps_proxy_symlink_reply {
	struct {
		bool atomic;
		uint64_t before;
		uint64_t after;
	} cinfo;
	uint32_t *attrset_mask;
	uint32_t attrset_mask_len;
	uint8_t child_fh[PS_MAX_FH_SIZE];
	uint32_t child_fh_len;
	struct ps_proxy_attrs_min child_attrs;
};

/*
 * Build and send SEQUENCE + PUTFH(parent) + CREATE(NF4LNK,
 * name, linkdata, createattrs) + GETFH + GETATTR(TYPE|MODE)
 * on `ms`.  Same response shape as the mkdir forwarder; the
 * caller's hook materialises a local proxy-SB inode for the new
 * symlink so subsequent ops in the same compound see it.
 *
 * `linkdata` is the symlink target string of `linkdata_len`
 * bytes; the upstream MDS stores it verbatim.  Must be non-NULL
 * and non-zero.
 *
 * Returns:
 *   0        success; reply populated
 *   -EINVAL  any pointer NULL, parent_fh / name / linkdata length 0
 *   -E2BIG   parent_fh > PS_MAX_FH_SIZE
 *   -ENOSPC  child FH > PS_MAX_FH_SIZE (MDS misbehaving)
 *   -EEXIST  upstream returned NFS4ERR_EXIST
 *   -ENOMEM  attrset_mask alloc failure
 *   -errno   RPC / compound failure, or a non-OK per-op status
 */
int ps_proxy_forward_symlink(struct mds_session *ms, const uint8_t *parent_fh,
			     uint32_t parent_fh_len, const char *name,
			     uint32_t name_len, const char *linkdata,
			     uint32_t linkdata_len,
			     const uint32_t *createattrs_mask,
			     uint32_t createattrs_mask_len,
			     const uint8_t *createattrs_vals,
			     uint32_t createattrs_vals_len,
			     const struct authunix_parms *creds,
			     struct ps_proxy_symlink_reply *reply);

void ps_proxy_symlink_reply_free(struct ps_proxy_symlink_reply *reply);

/*
 * Caller-owned result from ps_proxy_forward_mknod.  Same shape as
 * the mkdir/symlink replies -- the only wire difference between
 * CREATE(NF4DIR), CREATE(NF4LNK), and CREATE(NF4{BLK,CHR,SOCK,FIFO})
 * is the createtype4 union payload, not the response.  Free with
 * ps_proxy_mknod_reply_free() exactly once.
 */
struct ps_proxy_mknod_reply {
	struct {
		bool atomic;
		uint64_t before;
		uint64_t after;
	} cinfo;
	uint32_t *attrset_mask;
	uint32_t attrset_mask_len;
	uint8_t child_fh[PS_MAX_FH_SIZE];
	uint32_t child_fh_len;
	struct ps_proxy_attrs_min child_attrs;
};

/*
 * Build and send SEQUENCE + PUTFH(parent) + CREATE(`type`, name,
 * [devdata], createattrs) + GETFH + GETATTR(TYPE|MODE) on `ms`.
 * Used for the four "special file" object types that have no
 * dedicated forwarder: NF4BLK, NF4CHR, NF4SOCK, NF4FIFO.  Other
 * types (NF4DIR, NF4LNK, NF4REG) have their own forwarders.
 *
 * `specdata1` / `specdata2` carry the major / minor device numbers
 * for NF4BLK and NF4CHR (they fill the createtype4_u.devdata
 * specdata4 union member).  For NF4SOCK and NF4FIFO they are
 * ignored; pass 0.
 *
 * Returns:
 *   0        success; reply populated
 *   -EINVAL  any pointer NULL, parent_fh / name length 0, or
 *            `type` not one of NF4BLK / NF4CHR / NF4SOCK / NF4FIFO
 *   -E2BIG   parent_fh > PS_MAX_FH_SIZE
 *   -ENOSPC  child FH > PS_MAX_FH_SIZE (MDS misbehaving)
 *   -EEXIST  upstream returned NFS4ERR_EXIST (name already taken)
 *   -ENOMEM  attrset_mask alloc failure
 *   -errno   RPC / compound failure, or a non-OK per-op status
 */
int ps_proxy_forward_mknod(
	struct mds_session *ms, const uint8_t *parent_fh,
	uint32_t parent_fh_len, const char *name, uint32_t name_len,
	uint32_t type, /* nfs_ftype4 value */
	uint32_t specdata1, uint32_t specdata2,
	const uint32_t *createattrs_mask, uint32_t createattrs_mask_len,
	const uint8_t *createattrs_vals, uint32_t createattrs_vals_len,
	const struct authunix_parms *creds, struct ps_proxy_mknod_reply *reply);

void ps_proxy_mknod_reply_free(struct ps_proxy_mknod_reply *reply);

/*
 * Caller-owned result from ps_proxy_forward_link.  The wire LINK
 * op (RFC 8881 S18.9) returns only the target-dir change_info4 --
 * a hardlink doesn't mint a new inode, so there's no GETFH /
 * GETATTR pair to bring back.  The local hook just bumps the
 * source inode's nlink and reports the upstream cinfo verbatim.
 */
struct ps_proxy_link_reply {
	bool atomic;
	uint64_t before;
	uint64_t after;
};

/*
 * Build and send SEQUENCE + PUTFH(src_fh) + SAVEFH + PUTFH(dst_dir_fh)
 * + LINK(newname) on `ms`.  Mirrors how the wire LINK op identifies
 * the source file (SAVED_FH) and the target directory (CURRENT_FH).
 * Both must be on the same upstream MDS (caller's hook checks
 * same-proxy-SB before forwarding; cross-SB hits NFS4ERR_XDEV
 * before this point).
 *
 * Returns:
 *   0        success; reply->{atomic,before,after} populated
 *   -EINVAL  any pointer NULL, any length 0
 *   -E2BIG   either FH > PS_MAX_FH_SIZE
 *   -EEXIST  upstream returned NFS4ERR_EXIST (newname collision)
 *   -errno   RPC / compound failure, or a non-OK per-op status
 */
int ps_proxy_forward_link(struct mds_session *ms, const uint8_t *src_fh,
			  uint32_t src_fh_len, const uint8_t *dst_dir_fh,
			  uint32_t dst_dir_fh_len, const char *newname,
			  uint32_t newname_len,
			  const struct authunix_parms *creds,
			  struct ps_proxy_link_reply *reply);

/*
 * Caller-owned result from ps_proxy_forward_close.  CLOSE returns an
 * updated stateid4 (same `other`, bumped `seqid`) that the end
 * client treats as the canonical stateid going forward.  We copy
 * both fields so the caller's hook can plug them into CLOSE4res
 * verbatim without re-deriving.
 */
struct ps_proxy_close_reply {
	uint32_t stateid_seqid;
	uint8_t stateid_other[PS_STATEID_OTHER_SIZE];
};

/*
 * Build and send SEQUENCE + PUTFH(upstream_fh) + CLOSE on `ms`,
 * copy the MDS's updated stateid into `reply`.
 *
 * The stateid passed in is the end client's open stateid -- which
 * for a proxy file is the MDS's own stateid from a prior OPEN
 * forward (see slice 2e-iv-j).  No translation is needed.
 *
 * On any failure `reply` is left zero-initialised and no durable
 * state ran on the upstream.
 *
 * NOT_NOW_BROWN_COW (slice 2e-iv-c): credential forwarding.
 *
 * Returns:
 *   0        success; reply populated
 *   -EINVAL  ms / upstream_fh / stateid_other / reply NULL, or
 *            upstream_fh_len == 0
 *   -E2BIG   upstream_fh_len > PS_MAX_FH_SIZE
 *   -errno   RPC / compound failure, or non-OK per-op status
 */
int ps_proxy_forward_close(struct mds_session *ms, const uint8_t *upstream_fh,
			   uint32_t upstream_fh_len, uint32_t close_seqid,
			   uint32_t stateid_seqid,
			   const uint8_t stateid_other[PS_STATEID_OTHER_SIZE],
			   const struct authunix_parms *creds,
			   struct ps_proxy_close_reply *reply);

/*
 * A single READDIR entry as returned by the MDS, deep-copied into
 * PS-owned heap storage so the caller can release the compound
 * without losing the reply.  `name`, `attrmask`, and `attr_vals`
 * each own a separate malloc; walk via ->next and release with
 * ps_proxy_readdir_reply_free().
 */
struct ps_proxy_readdir_entry {
	struct ps_proxy_readdir_entry *next;
	uint64_t cookie;
	char *name; /* NUL-terminated UTF-8 */
	uint32_t *attrmask; /* bitmap4 words */
	uint32_t attrmask_len;
	uint8_t *attr_vals; /* attrlist4 bytes */
	uint32_t attr_vals_len;
};

struct ps_proxy_readdir_reply {
	uint8_t cookieverf[PS_PROXY_VERIFIER_SIZE];
	struct ps_proxy_readdir_entry *entries; /* may be NULL (empty dir) */
	bool eof;
};

/*
 * Build and send SEQUENCE + PUTFH(upstream_fh) + READDIR on `ms`;
 * deep-copy the MDS's dirlist4 into the PS-owned linked list on
 * `reply`.  Client-supplied cookie / cookieverf / counts /
 * attr_request are forwarded verbatim.
 *
 * attr_request is the bitmap4 the client supplied on its wire
 * READDIR; the PS does not interpret it and the MDS returns
 * whatever attrs it would normally encode.  A zero-length
 * attr_request is legal (the MDS returns entries with empty
 * attrs).
 *
 * On any failure `reply` is left zero-initialised and no heap
 * allocations leak.
 *
 * NOT_NOW_BROWN_COW (slice 2e-iv-c): credential forwarding.
 *
 * Returns:
 *   0        success; reply populated (entries may be NULL)
 *   -EINVAL  ms / upstream_fh / reply / cookieverf NULL, or
 *            upstream_fh_len == 0
 *   -E2BIG   upstream_fh_len > PS_MAX_FH_SIZE
 *   -ENOMEM  heap allocation failure during deep copy
 *   -errno   RPC / compound failure, or a non-OK per-op status
 */
int ps_proxy_forward_readdir(struct mds_session *ms, const uint8_t *upstream_fh,
			     uint32_t upstream_fh_len, uint64_t cookie,
			     const uint8_t cookieverf[PS_PROXY_VERIFIER_SIZE],
			     uint32_t dircount, uint32_t maxcount,
			     const uint32_t *attr_request,
			     uint32_t attr_request_len,
			     const struct authunix_parms *creds,
			     struct ps_proxy_readdir_reply *reply);

/*
 * Release all heap owned by `reply` (entries list + each entry's
 * name / attrmask / attr_vals) and zero the struct.  NULL-safe.
 */
void ps_proxy_readdir_reply_free(struct ps_proxy_readdir_reply *reply);

/* ------------------------------------------------------------------ */
/* Layout passthrough -- task #150 (codec demos through PS)           */
/* ------------------------------------------------------------------ */
/*
 * The PS forwards layout-related ops verbatim to the upstream MDS
 * and returns the response unchanged.  Clients (e.g. ec_demo) then
 * dial the upstream DSes directly using the deviceids from the
 * forwarded layout body -- the PS is not in the data path.
 *
 * Requires the client to have IP reachability to the DSes (true on
 * deploy/sanity's docker bridge).  For NAT'd / firewalled
 * deployments where the PS is the only network path, a "translation"
 * variant that rewrites deviceids to point at the PS itself is the
 * follow-up.
 */

/*
 * One layout segment in a PS-owned heap copy of LAYOUTGET4resok.
 * Mirrors layout4 from RFC 8881 S3.3.13: offset/length/iomode + an
 * opaque body whose interpretation depends on lo_content_type.
 */
struct ps_proxy_layout_segment {
	uint64_t lo_offset;
	uint64_t lo_length;
	uint32_t lo_iomode;
	uint32_t lo_content_type; /* layouttype4 */
	uint8_t *lo_content_body; /* heap, layout body bytes */
	uint32_t lo_content_body_len;
};

/*
 * PS-owned heap copy of LAYOUTGET4resok.  layouts[] and each
 * segment's lo_content_body are heap-allocated; release exactly
 * once via ps_proxy_layoutget_reply_free().  The MDS-issued layout
 * stateid passes through verbatim so the client's subsequent
 * LAYOUTRETURN / LAYOUTCOMMIT can ride with a stateid the MDS will
 * recognise.
 */
struct ps_proxy_layoutget_reply {
	bool return_on_close;
	uint32_t stateid_seqid;
	uint8_t stateid_other[PS_STATEID_OTHER_SIZE];
	uint32_t nlayouts;
	struct ps_proxy_layout_segment *layouts; /* heap array */
};

/*
 * Build and send SEQUENCE + PUTFH(upstream_fh) + LAYOUTGET on `ms`,
 * then deep-copy the LAYOUTGET4resok into `reply`.  All variable-
 * length fields (layout4 array + each lo_content body) move to
 * PS-owned heap so the caller can hold the reply past
 * mds_compound_fini.
 *
 * Returns:
 *   0        success; reply populated (nlayouts may be 0)
 *   -EINVAL  ms / upstream_fh / stateid_other / reply NULL, or
 *            upstream_fh_len == 0
 *   -E2BIG   upstream_fh_len > PS_MAX_FH_SIZE
 *   -ENOMEM  heap allocation failure during deep copy
 *   -ENOSYS  forwarder is the foundation stub -- impl pending in
 *            the real-implementation commit (DELETE this branch
 *            once the deep-copy is wired)
 *   -errno   RPC / compound failure, or non-OK per-op status
 */
int ps_proxy_forward_layoutget(
	struct mds_session *ms, const uint8_t *upstream_fh,
	uint32_t upstream_fh_len, bool signal_layout_avail,
	uint32_t layout_type, uint32_t iomode, uint64_t offset, uint64_t length,
	uint64_t minlength, uint32_t stateid_seqid,
	const uint8_t stateid_other[PS_STATEID_OTHER_SIZE], uint32_t maxcount,
	const struct authunix_parms *creds,
	struct ps_proxy_layoutget_reply *reply);

void ps_proxy_layoutget_reply_free(struct ps_proxy_layoutget_reply *reply);

/*
 * GETDEVICEINFO forward.  Returns a PS-owned deep copy of the MDS's
 * device_addr4 (layouttype + opaque addr_body).  ec_demo calls this
 * once per unique deviceid in a freshly-acquired layout to learn
 * how to dial each DS.
 */
struct ps_proxy_getdeviceinfo_reply {
	uint32_t da_layout_type;
	uint8_t *da_addr_body; /* heap, addr body bytes */
	uint32_t da_addr_body_len;
	uint32_t notification_mask; /* bitmap4: notification bits */
};

int ps_proxy_forward_getdeviceinfo(struct mds_session *ms,
				   const uint8_t deviceid[16],
				   uint32_t layout_type, uint32_t maxcount,
				   const struct authunix_parms *creds,
				   struct ps_proxy_getdeviceinfo_reply *reply);

void ps_proxy_getdeviceinfo_reply_free(
	struct ps_proxy_getdeviceinfo_reply *reply);

/*
 * LAYOUTRETURN forward.  Most LAYOUTRETURN responses are simple
 * (status + stateid for non-RETURN_ALL cases) so we just copy the
 * stateid out.  return_all skips the per-file path and returns no
 * stateid.
 *
 * lr_body / lr_body_len are the opaque layoutreturn_file4 body
 * (kind-specific; ec_demo passes empty for plain returns).
 */
struct ps_proxy_layoutreturn_reply {
	bool stateid_present;
	uint32_t stateid_seqid;
	uint8_t stateid_other[PS_STATEID_OTHER_SIZE];
};

int ps_proxy_forward_layoutreturn(
	struct mds_session *ms, const uint8_t *upstream_fh,
	uint32_t upstream_fh_len, bool reclaim, uint32_t layout_type,
	uint32_t iomode, uint32_t return_type, uint64_t offset, uint64_t length,
	uint32_t stateid_seqid,
	const uint8_t stateid_other[PS_STATEID_OTHER_SIZE],
	const uint8_t *lr_body, uint32_t lr_body_len,
	const struct authunix_parms *creds,
	struct ps_proxy_layoutreturn_reply *reply);

#endif /* _REFFS_PS_PROXY_OPS_H */
