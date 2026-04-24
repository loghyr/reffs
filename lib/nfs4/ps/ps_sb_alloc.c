/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <uuid/uuid.h>

#include "reffs/fs.h"
#include "reffs/sb_registry.h"
#include "reffs/super_block.h"

#include "ps_sb.h"
#include "ps_state.h"

int ps_sb_alloc_for_export(const struct ps_listener_state *pls,
			   const char *path, const uint8_t *mds_fh,
			   uint32_t mds_fh_len)
{
	struct super_block *sb = NULL;
	struct ps_sb_binding *binding = NULL;
	uint64_t sb_id;
	int ret;

	if (!pls || !path || path[0] != '/' || !mds_fh || mds_fh_len == 0)
		return -EINVAL;
	if (mds_fh_len > PS_MAX_FH_SIZE)
		return -E2BIG;
	/*
	 * The listener's root SB already owns "/".  Mounting a proxy
	 * SB at the same dirent would try to set RD_MOUNTED_ON on the
	 * root and leave the namespace in an inconsistent state.
	 */
	if (strcmp(path, "/") == 0)
		return -EINVAL;

	/*
	 * Make sure the mount-point directory exists in the listener's
	 * own namespace.  mkdir_p swallows EEXIST per component so a
	 * re-discovery call on an already-mounted path is a no-op here.
	 */
	ret = reffs_fs_mkdir_p_for_listener(pls->pls_listener_id, path, 0755);
	if (ret)
		return ret;

	/*
	 * Refuse to mount if another SB already owns this path (exact
	 * match) or a parent path (would shadow the existing mount).
	 * Child paths are fine -- we're establishing a new inner mount.
	 */
	ret = super_block_check_path_conflict(pls->pls_listener_id, path);
	if (ret)
		return ret;

	/*
	 * In-core id allocation only -- pass NULL for state_dir because
	 * proxy SBs are RAM-backed and regenerated on every reffsd boot
	 * (the upstream's MOUNT3 enumeration runs fresh each startup).
	 * A persistent counter bump here would grow the on-disk registry
	 * with entries we never load.
	 */
	sb_id = sb_registry_alloc_id(NULL);
	if (sb_id == 0)
		return -EAGAIN;

	sb = super_block_alloc(sb_id, (char *)path, REFFS_STORAGE_RAM, NULL);
	if (!sb)
		return -ENOMEM;

	/*
	 * Stamp listener id before anything can consult
	 * super_block_find_for_listener() on this sb.  We are still on
	 * the caller's single-writer thread; there is no visibility
	 * window where another worker could find an id=0 SB.
	 */
	sb->sb_listener_id = pls->pls_listener_id;
	uuid_generate(sb->sb_uuid);

	ret = super_block_dirent_create(sb, NULL, reffs_life_action_birth);
	if (ret)
		goto err_put;

	binding = ps_sb_binding_alloc(pls->pls_listener_id, mds_fh, mds_fh_len);
	if (!binding) {
		ret = -ENOMEM;
		goto err_release_dirents;
	}

	/*
	 * Attach before mount: super_block_set_proxy_binding transfers
	 * ownership to the SB, so if super_block_mount below fails the
	 * binding is released automatically when the SB is freed.  No
	 * separate cleanup branch needed.
	 */
	super_block_set_proxy_binding(sb, binding, ps_sb_binding_free_cb);
	binding = NULL;

	ret = super_block_mount(sb, path);
	if (ret)
		goto err_release_dirents;

	return 0;

err_release_dirents:
	super_block_destroy(sb);
	super_block_release_dirents(sb);
err_put:
	super_block_put(sb);
	return ret;
}
