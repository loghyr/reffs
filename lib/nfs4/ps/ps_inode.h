/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_PS_INODE_H
#define _REFFS_PS_INODE_H

#include <stdint.h>

#include "ps_state.h" /* PS_MAX_FH_SIZE */

struct inode; /* lib/include/reffs/inode.h */

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

#endif /* _REFFS_PS_INODE_H */
