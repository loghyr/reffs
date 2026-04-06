/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <urcu/list.h>
#include <uuid/uuid.h>

#include "reffs/backend.h"
#include "reffs/rcu.h"
#include "reffs/dirent.h"
#include "reffs/filehandle.h"
#include "reffs/fs.h"
#include "reffs/inode.h"
#include "reffs/log.h"
#include "reffs/ns.h"
#include "reffs/evictor.h"
#include "reffs/super_block.h"
#include "reffs/types.h"
#include "reffs/trace/fs.h"

volatile sig_atomic_t reffs_namespace_initialized = 0;
static struct super_block *reffs_root_sb = NULL;
static struct reffs_dirent *reffs_root_de = NULL;

int reffs_ns_init(void)
{
	struct inode *inode = NULL;
	int ret = 0;

	if (reffs_namespace_initialized)
		return -EALREADY;

	reffs_namespace_initialized = 1;

	reffs_backend_init();

	reffs_root_sb = super_block_alloc(1, "/", reffs_fs_get_storage_type(),
					  reffs_fs_get_backend_path());
	if (!reffs_root_sb) {
		ret = -ENOMEM;
		goto out;
	}
	uuid_generate(reffs_root_sb->sb_uuid);

	ret = super_block_dirent_create(reffs_root_sb, NULL,
					reffs_life_action_birth);
	if (ret)
		goto out;

	reffs_root_de = dirent_get(reffs_root_sb->sb_dirent);

	inode = inode_active_get(reffs_root_sb->sb_dirent->rd_inode);
	if (!inode) {
		ret = ENOENT;
		LOG("No root inode on root sb");
		goto out;
	}

	trace_fs_inode(inode, __func__, __LINE__);

	inode->i_uid = REFFS_ID_ROOT_VAL;
	inode->i_gid = REFFS_ID_ROOT_VAL;
	clock_gettime(CLOCK_REALTIME, &inode->i_mtime);
	inode->i_atime = inode->i_mtime;
	inode->i_btime = inode->i_mtime;
	inode->i_ctime = inode->i_mtime;
	inode->i_mode = S_IFDIR | 0777;

	inode->i_size = inode->i_sb->sb_block_size;
	inode->i_used = 1;
	inode->i_nlink = 2;

	inode_active_put(inode);

	evictor_init();

out:
	if (ret)
		reffs_ns_fini();

	return ret;
}

void release_all_fs_dirents(void)
{
	struct super_block *sb, *tmp;
	char uuid_str[UUID_STR_LEN];

	struct cds_list_head *sb_list = super_block_list_head();

	/*
         * Do NOT call rcu_barrier() here.  Evicted inodes may have
         * inode_free_rcu callbacks queued; completing them before the
         * dirent walk would free inode structs while rd_inode still points
         * at them.  release_dirents_recursive uses inode_active_get which
         * reads the urcu_ref inside the struct -- UAF if already freed.
         * Each super_block_release_dirents call ends with super_block_drain
         * + rcu_barrier(), which cleans up after the walk completes.
         */
	cds_list_for_each_entry_safe(sb, tmp, sb_list, sb_link) {
		uuid_unparse(sb->sb_uuid, uuid_str);
		super_block_release_dirents(sb);
		super_block_put(sb);
	}
}

int reffs_ns_fini(void)
{
	if (!reffs_namespace_initialized)
		return -EALREADY;

	/* Stop evictor BEFORE draining superblocks. */
	evictor_fini();

	reffs_namespace_initialized = 0;

	if (reffs_root_sb) {
		/*
		 * release_all_fs_dirents() walks children, then calls
		 * dirent_parent_release + dirent_put on sb->sb_dirent (the
		 * root dirent), then super_block_put() for the sb's initial
		 * urcu_ref.  Do NOT call super_block_dirent_release() after
		 * this -- it would double-release the root dirent.
		 */
		release_all_fs_dirents();
		reffs_root_sb = NULL;
	}

	if (reffs_root_de) {
		/*
		 * reffs_root_de is the dirent_get() ref taken in ns_init.
		 * The dirent itself was already fully released by
		 * release_all_fs_dirents(); just drop our cached ref.
		 */
		dirent_put(reffs_root_de);
		reffs_root_de = NULL;
	}

	return 0;
}
