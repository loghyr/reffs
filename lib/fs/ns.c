/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "reffs/rcu.h"
#include "reffs/log.h"
#include "reffs/test.h"
#include "reffs/inode.h"
#include "reffs/super_block.h"
#include "reffs/data_block.h"
#include "reffs/server.h"
#include "reffs/ns.h"
#include "reffs/filehandle.h"

volatile sig_atomic_t reffs_namespace_initialized = 0;
static struct super_block *reffs_root_sb = NULL;
static struct dirent *reffs_root_de = NULL;

int reffs_ns_init(void)
{
	struct inode *inode = NULL;
	int ret = 0;

	if (reffs_namespace_initialized)
		return EALREADY;

	reffs_namespace_initialized = 1;

	reffs_root_de = dirent_alloc(NULL, "/", reffs_life_action_birth);
	if (!reffs_root_de) {
		ret = ENOMEM;
		goto out;
	}

	reffs_root_de->d_inode = inode_alloc(NULL, 1);
	if (!reffs_root_de->d_inode) {
		ret = ENOMEM;
		goto out;
	}

	inode = inode_get(reffs_root_de->d_inode);
	assert(inode);
	if (!inode) {
		ret = ENOENT;
		LOG("No root inode on root sb");
		goto out;
	}

	inode->i_uid = 0;
	inode->i_gid = 0;
	clock_gettime(CLOCK_REALTIME, &inode->i_mtime);
	inode->i_atime = inode->i_mtime;
	inode->i_btime = inode->i_mtime;
	inode->i_ctime = inode->i_mtime;
	inode->i_mode = S_IFDIR | 0755;
	inode->i_size = 4096;
	inode->i_used = 4096;
	inode->i_nlink = 2;

	inode_put(inode);
	reffs_root_sb = super_block_alloc(1, "/");
	if (!reffs_root_sb) {
		ret = ENOMEM;
		goto out;
	}

	ret = super_block_dirent_create(reffs_root_sb, reffs_root_de,
					reffs_life_action_birth);
	if (ret)
		goto out;

	inode = inode_get(reffs_root_sb->sb_dirent->d_inode);
	assert(inode);
	if (!inode) {
		ret = ENOENT;
		LOG("No root inode on root sb");
		goto out;
	}

	inode->i_uid = 0;
	inode->i_gid = 0;
	clock_gettime(CLOCK_REALTIME, &inode->i_mtime);
	inode->i_atime = inode->i_mtime;
	inode->i_btime = inode->i_mtime;
	inode->i_ctime = inode->i_mtime;
	inode->i_mode = S_IFDIR | 0777;
	inode->i_size = 4096;
	inode->i_used = 4096;
	inode->i_nlink = 2;

	inode->i_parent = reffs_root_sb->sb_dirent;

	inode_put(inode);

	network_file_handle_init();

out:
	if (ret)
		reffs_ns_fini();

	return ret;
}

static void release_dirents_recursive(struct dirent *de_parent)
{
	struct dirent *de;
	int count = 0;

	if (!de_parent || !de_parent->d_inode)
		return;

	cds_list_for_each_entry_rcu(de, &de_parent->d_inode->i_children,
				    d_siblings) {
		release_dirents_recursive(de);
		dirent_put(de);
		count++;
	}

	TRACE(REFFS_TRACE_LEVEL_INFO, "Unloaded %d entries for %s", count,
	      de_parent->d_name);
}

static void release_all_fs_dirents(void)
{
	struct super_block *sb, *tmp;
	struct dirent *de_parent;
	char uuid_str[UUID_STR_LEN];

	struct cds_list_head *sb_list = super_block_list_head();

	cds_list_for_each_entry_safe(sb, tmp, sb_list, sb_link) {
		uuid_unparse(sb->sb_uuid, uuid_str);
		TRACE(REFFS_TRACE_LEVEL_WARNING, "Unloading \"%s\" (uuid %s)",
		      sb->sb_path, uuid_str);
		de_parent = dirent_get(sb->sb_dirent);
		if (de_parent) {
			release_dirents_recursive(de_parent);

			dirent_parent_release(de_parent,
					      reffs_life_action_death);
			dirent_put(de_parent);
		}

		super_block_put(sb);
	}
}

int reffs_ns_fini(void)
{
	if (!reffs_namespace_initialized)
		return EALREADY;

	reffs_namespace_initialized = 0;

	if (reffs_root_sb) {
		super_block_dirent_release(reffs_root_sb,
					   reffs_life_action_death);
		release_all_fs_dirents();
		/*
		 * Normally we would still have a reference to the sb
		 * here, but as we strong armed all sbs to be
		 * put in release_all_fs_dirents(), we don't!
		 */
		//super_block_put(reffs_root_sb);
		reffs_root_sb = NULL;
	}

	if (reffs_root_de) {
		dirent_parent_release(reffs_root_de, reffs_life_action_death);
		dirent_put(reffs_root_de);
		reffs_root_de = NULL;
	}

	return 0;
}
