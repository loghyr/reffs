/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "reffs/cmp.h"
#include "reffs/dirent.h"
#include "reffs/identity.h"
#include "reffs/inode.h"
#include "reffs/super_block.h"
#include "reffs/types.h"

#include "ps_inode.h"
#include "ps_proxy_ops.h"
#include "ps_sb.h"
#include "ps_state.h"

/*
 * Is this inode backed by a proxy SB?  Only those SBs follow the
 * convention that i_storage_private is a ps_inode_proxy_data; any
 * other caller has no contract for the field today.
 */
static bool ps_inode_is_proxy(const struct inode *inode)
{
	return inode && inode->i_sb && inode->i_sb->sb_proxy_binding;
}

int ps_inode_set_upstream_fh(struct inode *inode, const uint8_t *fh,
			     uint32_t fh_len)
{
	if (!inode || !fh || fh_len == 0)
		return -EINVAL;
	if (fh_len > PS_MAX_FH_SIZE)
		return -E2BIG;
	if (!ps_inode_is_proxy(inode))
		return -EINVAL;

	struct ps_inode_proxy_data *pid = inode->i_storage_private;

	if (!pid) {
		pid = calloc(1, sizeof(*pid));
		if (!pid)
			return -ENOMEM;
		/*
		 * First assignment under the inode's attr mutex would
		 * be ideal, but this path is only called from the
		 * single-writer LOOKUP hook (slice 2e-iv-f) which holds
		 * no races with readers yet: the inode was just
		 * allocated on behalf of this one compound and has not
		 * been published anywhere concurrent readers can reach.
		 * Plain store is sufficient at the current integration
		 * point; revisit if we ever allow concurrent publishers.
		 */
		inode->i_storage_private = pid;
	}

	memcpy(pid->upstream_fh, fh, fh_len);
	pid->upstream_fh_len = fh_len;
	return 0;
}

int ps_inode_get_upstream_fh(const struct inode *inode, uint8_t *buf,
			     uint32_t buf_len, uint32_t *len_out)
{
	if (!inode || !buf || !len_out)
		return -EINVAL;
	if (!ps_inode_is_proxy(inode))
		return -EINVAL;

	const struct ps_inode_proxy_data *pid = inode->i_storage_private;

	/*
	 * Fallback for the SB root: discovery cached the root's
	 * upstream FH on the SB binding (ps_sb_binding.psb_mds_fh)
	 * rather than on the inode, because the inode didn't exist
	 * yet when discovery ran.  Let the accessor hide that detail
	 * so op handlers treat root and non-root inodes uniformly.
	 */
	if (!pid && inode == inode->i_sb->sb_root_inode) {
		const struct ps_sb_binding *binding =
			inode->i_sb->sb_proxy_binding;

		if (!binding || binding->psb_mds_fh_len == 0)
			return -ENOENT;

		if (binding->psb_mds_fh_len > buf_len)
			return -ENOSPC;
		memcpy(buf, binding->psb_mds_fh, binding->psb_mds_fh_len);
		*len_out = binding->psb_mds_fh_len;
		return 0;
	}

	if (!pid || pid->upstream_fh_len == 0)
		return -ENOENT;

	if (pid->upstream_fh_len > buf_len)
		return -ENOSPC;
	memcpy(buf, pid->upstream_fh, pid->upstream_fh_len);
	*len_out = pid->upstream_fh_len;
	return 0;
}

int ps_proxy_lookup_forward_for_inode(const struct inode *parent,
				      const char *name, uint32_t name_len,
				      uint8_t *child_fh_buf,
				      uint32_t child_fh_buf_len,
				      uint32_t *child_fh_len_out)
{
	if (!parent || !name || name_len == 0 || !child_fh_buf ||
	    !child_fh_len_out)
		return -EINVAL;
	if (!ps_inode_is_proxy(parent))
		return -EINVAL;

	/*
	 * Resolve the parent's upstream anchor.  ps_inode_get_upstream_fh
	 * handles both the per-inode-override case (i_storage_private
	 * was set by a prior LOOKUP hook on a deeper inode) and the
	 * SB-root fallback case (the binding FH from discovery).
	 */
	uint8_t parent_fh[PS_MAX_FH_SIZE];
	uint32_t parent_fh_len = 0;
	int ret = ps_inode_get_upstream_fh(parent, parent_fh, sizeof(parent_fh),
					   &parent_fh_len);

	if (ret < 0)
		return ret;

	const struct ps_sb_binding *binding = parent->i_sb->sb_proxy_binding;
	const struct ps_listener_state *pls =
		ps_state_find(binding->psb_listener_id);

	/*
	 * No session -> transient proxy-side unavailability.  -ENOTCONN
	 * is a clean signal for the op handler (slice 2e-iv-g) to
	 * translate to NFS4ERR_DELAY without having to distinguish
	 * session-down from generic I/O errors.
	 */
	if (!pls || !pls->pls_session)
		return -ENOTCONN;

	return ps_proxy_forward_lookup(pls->pls_session, parent_fh,
				       parent_fh_len, name, name_len,
				       child_fh_buf, child_fh_buf_len,
				       child_fh_len_out);
}

int ps_lookup_materialize(struct inode *parent, const char *name,
			  uint32_t name_len, const uint8_t *child_fh,
			  uint32_t child_fh_len, struct reffs_dirent **out_de,
			  struct inode **out_inode)
{
	if (!parent || !name || name_len == 0 || !child_fh ||
	    child_fh_len == 0 || !out_de || !out_inode)
		return -EINVAL;
	if (child_fh_len > PS_MAX_FH_SIZE)
		return -E2BIG;
	if (!ps_inode_is_proxy(parent))
		return -EINVAL;
	if (!S_ISDIR(parent->i_mode))
		return -EINVAL;
	if (!parent->i_dirent)
		return -EINVAL;

	*out_de = NULL;
	*out_inode = NULL;

	/*
	 * strndup so reffs_dirent owns a NUL-terminated copy of the name.
	 * dirent_alloc takes `char *` and keeps its own duplicate, but
	 * its callers uniformly pass NUL-terminated strings and dirent_find
	 * uses strcmp, so we need a terminator here too.
	 */
	char *name_dup = strndup(name, name_len);

	if (!name_dup)
		return -ENOMEM;

	/*
	 * Fast-path duplicate check: if another thread already materialized
	 * the same child (concurrent LOOKUP from a second client), surface
	 * -EEXIST so the hook can fall back to the in-memory dirent without
	 * leaking a second inode allocation.
	 *
	 * This is a best-effort pre-check, not a race-free guard.  Two
	 * concurrent calls for the same (parent, name) can both observe
	 * existing==NULL and both proceed to dirent_alloc.  The LOOKUP
	 * hook in slice 2e-iv-g-ii is the caller's serialization point:
	 * it holds the compound's single-dispatch discipline per parent
	 * inode, so two compounds racing on the same parent are
	 * interleaved one-at-a-time at the op-handler layer.  vfs_create
	 * has the same pre-check but additionally holds parent->rd_rwlock
	 * via vfs_lock_dirs -- if the LOOKUP hook ever parallelises
	 * per-parent, the rwlock must move into this helper too.
	 */
	struct reffs_dirent *existing =
		dirent_find(parent->i_dirent, reffs_case_get(), name_dup);

	if (existing) {
		dirent_put(existing);
		free(name_dup);
		return -EEXIST;
	}

	struct super_block *sb = parent->i_sb;
	struct reffs_dirent *rd = dirent_alloc(parent->i_dirent, name_dup,
					       reffs_life_action_load,
					       false /* see type-fix note */);

	if (!rd) {
		free(name_dup);
		return -ENOMEM;
	}

	/* dirent_alloc strdup'd name_dup into rd->rd_name; ours is done. */
	free(name_dup);

	uint64_t ino =
		__atomic_add_fetch(&sb->sb_next_ino, 1, __ATOMIC_RELAXED);
	struct inode *inode = inode_alloc(sb, ino);

	if (!inode) {
		dirent_parent_release(rd, reffs_life_action_unload);
		dirent_put(rd);
		return -ENOMEM;
	}

	/*
	 * Placeholder attributes.  The object type, size, timestamps, and
	 * identity fields will be overwritten by the first forwarded
	 * GETATTR on this inode -- we don't know them yet from LOOKUP
	 * alone.  Mode 0644 is the safe default that makes the inode
	 * immediately readable without access-check failures against
	 * the end client before the first GETATTR lands.
	 *
	 * NOT_NOW_BROWN_COW: promote the object type (dir / symlink /
	 * special) and fill real attrs by piggy-backing GETATTR on the
	 * LOOKUP compound, or running a follow-up GETATTR before the
	 * LOOKUP reply is returned.  Deferred to a later slice.
	 */
	inode->i_uid = REFFS_ID_MAKE(REFFS_ID_UNIX, 0, 0);
	inode->i_gid = REFFS_ID_MAKE(REFFS_ID_UNIX, 0, 0);
	inode->i_mode = S_IFREG | 0644;
	inode->i_nlink = 1;
	inode->i_size = 0;
	inode->i_used = 0;
	inode->i_parent_ino = parent->i_ino;
	clock_gettime(CLOCK_REALTIME, &inode->i_mtime);
	inode->i_atime = inode->i_mtime;
	inode->i_ctime = inode->i_mtime;
	inode->i_btime = inode->i_mtime;

	int ret = ps_inode_set_upstream_fh(inode, child_fh, child_fh_len);

	if (ret < 0) {
		inode_active_put(inode);
		dirent_parent_release(rd, reffs_life_action_unload);
		dirent_put(rd);
		return ret;
	}

	dirent_attach_inode(rd, inode);
	rd->rd_ino = inode->i_ino;

	*out_de = rd;
	*out_inode = inode; /* transfers active ref from inode_alloc */
	return 0;
}
