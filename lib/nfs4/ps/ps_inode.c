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

#include "reffs/inode.h"
#include "reffs/super_block.h"

#include "ps_inode.h"
#include "ps_sb.h"

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
