/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
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
#include <unistd.h>
#include "reffs/rcu.h"
#include "reffs/backend.h"
#include "reffs/dirent.h"
#include "reffs/inode.h"
#include "reffs/log.h"
#include "reffs/super_block.h"
#include "reffs/types.h"

struct rcu_head;

CDS_LIST_HEAD(super_block_list);

struct cds_list_head *super_block_list_head(void)
{
	return &super_block_list;
}

static void super_block_remove_all_inodes(struct super_block *sb)
{
	struct cds_lfht_iter iter;
	struct inode *inode;

	while (__atomic_load_n(&sb->sb_delayed_count, __ATOMIC_RELAXED) > 0) {
		LOG("Waiting for delayed releases to drain (%lu remaining)",
		    sb->sb_delayed_count);
		sleep(1);
	}

	rcu_barrier();

	rcu_read_lock();
	cds_lfht_for_each_entry(sb->sb_inodes, &iter, inode, i_node) {
		inode_unhash(inode);
	}
	rcu_read_unlock();

	rcu_barrier();

	/*
	 * Now the hash table should be empty. If there are still entries,
	 * it's because of a race or leak, but we must not hang here.
	 */
	unsigned long count = 0;
	rcu_read_lock();
	cds_lfht_for_each_entry(sb->sb_inodes, &iter, inode, i_node) {
		count++;
	}
	rcu_read_unlock();

	if (count > 0) {
		LOG("WARNING: %lu inodes still in hash table after removal",
		    count);
	}
}

static void super_block_free(struct super_block *sb)
{
	if (!sb)
		return;

	if (sb->sb_ops && sb->sb_ops->sb_free)
		sb->sb_ops->sb_free(sb);

	int ret = cds_lfht_destroy(sb->sb_inodes, NULL);
	if (ret < 0) {
		LOG("Could not delete a hash table: %m");
	}

	free(sb->sb_path);
	free(sb->sb_backend_path);
	free(sb);
}

static void super_block_free_rcu(struct rcu_head *rcu)
{
	struct super_block *sb =
		caa_container_of(rcu, struct super_block, sb_rcu);

	super_block_free(sb);
}

static void super_block_release(struct urcu_ref *ref)
{
	struct super_block *sb =
		caa_container_of(ref, struct super_block, sb_ref);

	uint64_t flags = __atomic_fetch_and(&sb->sb_state, ~SB_IN_LIST,
					    __ATOMIC_ACQUIRE);
	if (flags & SB_IN_LIST)
		cds_list_del_init(&sb->sb_link);

	super_block_remove_all_inodes(sb);

	call_rcu(&sb->sb_rcu, super_block_free_rcu);
}

int super_block_dirent_create(struct super_block *sb, struct reffs_dirent *rd,
			      enum reffs_life_action rla)
{
	sb->sb_dirent = dirent_alloc(rd, "/", rla, true);
	if (!sb->sb_dirent)
		return ENOMEM;

	sb->sb_dirent->rd_inode = inode_alloc(
		sb, __atomic_add_fetch(&sb->sb_next_ino, 1, __ATOMIC_RELAXED));
	if (!sb->sb_dirent->rd_inode) {
		dirent_put(sb->sb_dirent);
		return ENOMEM;
	}

	if (rla == reffs_life_action_birth) {
		sb->sb_dirent->rd_inode->i_nlink = 2;
		sb->sb_dirent->rd_inode->i_mode = S_IFDIR | 0777;
	}
	sb->sb_dirent->rd_inode->i_parent = sb->sb_dirent;

	return 0;
}

void super_block_dirent_release(struct super_block *sb,
				enum reffs_life_action rla)
{
	struct reffs_dirent *rd;

	rcu_read_lock();
	rd = rcu_xchg_pointer(&sb->sb_dirent, NULL);
	if (rd) {
		dirent_parent_release(rd, rla);
		dirent_put(rd);
	}
	rcu_read_unlock();
}

struct super_block *super_block_alloc(uint64_t id, char *path,
				      enum reffs_storage_type storage_type,
				      const char *backend_path)
{
	struct super_block *sb;

	sb = calloc(1, sizeof(*sb));
	if (!sb) {
		LOG("Could not alloc a sb");
		return NULL;
	}

	sb->sb_id = id;
	sb->sb_path = strdup(path);
	sb->sb_storage_type = storage_type;
	if (backend_path)
		sb->sb_backend_path = strdup(backend_path);

	sb->sb_ops = reffs_backend_get_ops(storage_type);
	if (!sb->sb_ops) {
		free(sb->sb_path);
		free(sb->sb_backend_path);
		free(sb);
		return NULL;
	}

	CDS_INIT_LIST_HEAD(&sb->sb_link);

	sb->sb_inodes = cds_lfht_new(
		8, 8, 0, CDS_LFHT_AUTO_RESIZE | CDS_LFHT_ACCOUNTING, NULL);
	if (!sb->sb_inodes) {
		LOG("Could not create a new hash table");
		super_block_free(sb);
		return NULL;
	}

	urcu_ref_init(&sb->sb_ref);

	uuid_generate(sb->sb_uuid);

	sb->sb_bytes_max = SIZE_MAX;
	sb->sb_inodes_max = SIZE_MAX;

	if (sb->sb_ops->sb_alloc) {
		int ret = sb->sb_ops->sb_alloc(sb, backend_path);
		if (ret != 0) {
			/* Not in list yet, can just free */
			super_block_free(sb);
			return NULL;
		}
	}

	__atomic_fetch_or(&sb->sb_state, SB_IN_LIST, __ATOMIC_RELEASE);
	cds_list_add_rcu(&sb->sb_link, &super_block_list);

	return sb;
}

struct super_block *super_block_find(uint64_t id)
{
	struct super_block *sb = NULL;
	struct super_block *tmp;

	rcu_read_lock();
	cds_list_for_each_entry_rcu(tmp, &super_block_list, sb_link)
		if (id == tmp->sb_id) {
			sb = super_block_get(tmp);
			break;
		}
	rcu_read_unlock();

	return sb;
}

struct super_block *super_block_get(struct super_block *sb)
{
	if (!sb)
		return NULL;

	if (!urcu_ref_get_unless_zero(&sb->sb_ref))
		return NULL;

	return sb;
}

void super_block_put(struct super_block *sb)
{
	if (!sb)
		return;

	urcu_ref_put(&sb->sb_ref, super_block_release);
}
