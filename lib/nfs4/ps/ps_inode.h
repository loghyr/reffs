/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_PS_INODE_H
#define _REFFS_PS_INODE_H

#include <stdint.h>

#include "ps_state.h" /* PS_MAX_FH_SIZE */

struct inode; /* lib/include/reffs/inode.h */
struct reffs_dirent; /* lib/include/reffs/dirent.h */
struct ps_proxy_attrs_min; /* lib/nfs4/ps/ps_proxy_ops.h */
struct authunix_parms; /* <rpc/auth_unix.h> */

/*
 * Per-inode proxy data.
 *
 * Stored in the existing `void *i_storage_private` slot on struct
 * inode when the inode lives in a proxy SB.  Native-SB inodes leave
 * that slot NULL (no backend uses it today), so the convention is
 * PS-local: "if sb->sb_proxy_binding is non-NULL and
 * inode->i_storage_private is non-NULL, it is a ps_inode_proxy_data".
 *
 * The field is released by fs/inode.c's inode_free_rcu via a guarded
 * plain free() -- that lets the upstream FH live exactly as long as
 * the inode itself (client closes the file -> LRU evicts -> i_ref
 * drops -> RCU callback fires -> free).  No separate bookkeeping.
 *
 * POD only: if this struct ever grows internal allocations the
 * inode_free_rcu guard in lib/fs/inode.c needs a matching free hook.
 * Today it's bytes in + 4-byte length, so free() is enough.
 */
struct ps_inode_proxy_data {
	uint32_t upstream_fh_len;
	uint8_t upstream_fh[PS_MAX_FH_SIZE];
};

/*
 * Stash `fh` as the upstream FH for this proxy-SB inode.  Allocates
 * an i_storage_private on first call, replaces in place on later
 * calls (e.g. an on-demand re-discovery refresh).
 *
 * Returns:
 *   0        success
 *   -EINVAL  inode / fh NULL, fh_len 0, or inode's SB is not a proxy
 *   -E2BIG   fh_len > PS_MAX_FH_SIZE
 *   -ENOMEM  first-time allocation failed
 */
int ps_inode_set_upstream_fh(struct inode *inode, const uint8_t *fh,
			     uint32_t fh_len);

/*
 * Retrieve the upstream FH for a proxy-SB inode.  Falls back to the
 * SB binding's root FH when the inode is the SB root and has no
 * per-inode override set -- that's the one case where discovery
 * cached the FH on the SB itself (see ps_sb_binding) rather than
 * on an inode we hadn't allocated yet.
 *
 * Copies up to `buf_len` bytes into `buf`; writes the actual length
 * to `*len_out`.  If the stored FH is larger than buf_len, returns
 * -ENOSPC and leaves `*len_out` untouched.
 *
 * Returns:
 *   0        success
 *   -EINVAL  inode / buf / len_out NULL, or inode's SB is not a proxy
 *   -ENOENT  no upstream FH recorded for this inode and it is not
 *            the SB root (caller translates to NFS4ERR_NOENT or
 *            NFS4ERR_NOTSUPP depending on context)
 *   -ENOSPC  buf_len smaller than the stored FH
 */
int ps_inode_get_upstream_fh(const struct inode *inode, uint8_t *buf,
			     uint32_t buf_len, uint32_t *len_out);

/*
 * Forward a single-component LOOKUP against the upstream MDS,
 * anchored at `parent`'s upstream FH.  Thin composition of
 * ps_inode_get_upstream_fh (parent FH) + ps_state_find (listener
 * session) + ps_proxy_forward_lookup (the RPC).  When the caller
 * supplies `attr_request` + `attrs_out`, a GETATTR rides on the
 * same compound and the result is parsed into *attrs_out; pass
 * all three NULL/zero to skip the GETATTR.
 *
 * The op-handler hook in nfs4_op_lookup takes the child FH this
 * returns, feeds it plus the parsed attrs to ps_lookup_materialize,
 * and returns the result to the client.
 *
 * Returns:
 *   0         success -- child FH copied, length in *child_fh_len_out;
 *             attrs_out populated if requested
 *   -EINVAL   bad args, parent is not on a proxy SB, or attr-request /
 *             attrs_out consistency violated
 *   -ENOTCONN listener has no MDS session (transient; caller should
 *             surface NFS4ERR_DELAY)
 *   -ENOENT   upstream says the child doesn't exist
 *   -ENOSPC   child_fh_buf_len is smaller than the returned FH
 *   -ENOTSUP  forwarded GETATTR reply contains an attr this parser
 *             does not recognise
 *   -errno    transport / protocol failure
 */
int ps_proxy_lookup_forward_for_inode(
	const struct inode *parent, const char *name, uint32_t name_len,
	uint8_t *child_fh_buf, uint32_t child_fh_buf_len,
	uint32_t *child_fh_len_out, const uint32_t *attr_request,
	uint32_t attr_request_len, const struct authunix_parms *creds,
	struct ps_proxy_attrs_min *attrs_out);

/*
 * Materialize a local dirent + inode on a proxy SB for an upstream
 * child that has just been located via ps_proxy_lookup_forward_for_inode.
 *
 * `parent` MUST be a loaded directory inode on a proxy SB (its SB has
 * sb_proxy_binding != NULL) whose dirent chain is resident; the LOOKUP
 * hook in slice 2e-iv-g-ii runs inode_reconstruct_path_to_root before
 * reaching here, so parent->i_dirent is non-NULL by contract.
 *
 * `child_fh` is the upstream MDS FH the caller just obtained.  It is
 * stashed on the new inode so subsequent ops on this child can forward
 * to the upstream without a fresh LOOKUP round-trip.
 *
 * `attrs` is an optional type + mode hint harvested from the forwarded
 * GETATTR that rides on the same LOOKUP compound (slice 2e-iv-h).  When
 * `attrs->have_type` is set, the new inode's mode gets the matching
 * S_IFDIR / S_IFLNK / etc. (and i_nlink starts at 2 for directories).
 * When `attrs->have_mode` is set, the permission bits come from there
 * (masked to 07777).  Pass NULL to fall back to the S_IFREG | 0644
 * placeholder -- subsequent GETATTRs on the new inode forward upstream
 * and patch the attrs in full.
 *
 * On success, the out parameters carry live refs that the caller MUST
 * release:
 *   *out_de     -- the ref-held dirent (release via dirent_put)
 *   *out_inode  -- active ref on the inode (release via inode_active_put)
 *
 * Returns:
 *   0         success
 *   -EINVAL   bad args, parent not on a proxy SB, or parent not a dir
 *   -E2BIG    child_fh_len > PS_MAX_FH_SIZE
 *   -EEXIST   a dirent with this name already exists on the parent
 *             (caller treats this as a concurrent-LOOKUP race)
 *   -ENOMEM   dirent/inode allocation failed
 */
int ps_lookup_materialize(struct inode *parent, const char *name,
			  uint32_t name_len, const uint8_t *child_fh,
			  uint32_t child_fh_len,
			  const struct ps_proxy_attrs_min *attrs,
			  struct reffs_dirent **out_de,
			  struct inode **out_inode);

/*
 * Evict the local cached dirent for `name` under `parent` (a proxy-SB
 * directory), if one is resident in memory.  Used after a forwarded
 * REMOVE / RENAME against the upstream MDS succeeds, so a follow-up
 * LOOKUP through the PS does not hit a stale warm dirent that says
 * the name still exists (REMOVE) or still resolves to the old inode
 * (RENAME source).
 *
 * Memory-only: looks via dirent_find (no disk fault-in), so a name
 * that was never resident is a no-op without a slow-path round-trip.
 * Uses reffs_life_action_unload semantics: detach + drop refs, no
 * nlink decrement, no on-disk teardown.  Proxy-SB inodes have no
 * .meta / .dat backing for forwarded-from-upstream entries; the
 * upstream MDS owns the authoritative state.
 *
 * No-op when:
 *   - parent / name is NULL or name_len is 0
 *   - parent's SB has no sb_proxy_binding (caller is on a native SB)
 *   - parent has no resident dirent chain
 *   - no child by that name is currently cached
 *
 * Always safe to call -- returns void.
 */
void ps_invalidate_local_dirent(struct inode *parent, const char *name,
				uint32_t name_len);

#endif /* _REFFS_PS_INODE_H */
