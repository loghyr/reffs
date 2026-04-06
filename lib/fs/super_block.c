/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
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
#include <unistd.h>
#include "reffs/rcu.h"
#include "reffs/backend.h"
#include "reffs/dirent.h"
#include "reffs/fs.h"
#include "reffs/inode.h"
#include "reffs/log.h"
#include "reffs/super_block.h"
#include "reffs/types.h"
#include "reffs/trace/fs.h"

struct rcu_head;

CDS_LIST_HEAD(super_block_list);

struct cds_list_head *super_block_list_head(void)
{
	return &super_block_list;
}

/* ------------------------------------------------------------------ */
/* LRU eviction                                                         */
/* ------------------------------------------------------------------ */

/*
 * Evict up to 'count' idle inodes from the LRU.
 *
 * An inode is evicted by:
 *   1. Verifying i_active is still 0 (re-check under the lock).
 *   2. Removing it from the LRU list.
 *   3. Dropping the memory-lifetime ref that the hash table implicitly
 *      represented.  inode_unhash + inode_put handle the rest.
 *
 * We do NOT call inode_free directly; inode_put + inode_release do that
 * via the normal urcu_ref path, which handles the nlink==0 storage cleanup.
 */
void super_block_evict_inodes(struct super_block *sb, size_t count)
{
	struct inode *inode, *tmp;
	size_t evicted = 0;
	CDS_LIST_HEAD(evictees);

	/*
	 * Phase 1: collect eviction candidates under the LRU lock.
	 * Tombstone i_active = -1 prevents inode_active_get from
	 * racing.  Remove from LRU and transfer to the local list.
	 */
	pthread_mutex_lock(&sb->sb_inode_lru_lock);
	cds_list_for_each_entry_safe(inode, tmp, &sb->sb_inode_lru, i_lru) {
		if (evicted >= count)
			break;

		/* Re-check: someone may have re-activated it. */
		int64_t active =
			__atomic_load_n(&inode->i_active, __ATOMIC_ACQUIRE);
		if (active != 0)
			continue;

		/* Mark as being evicted so inode_active_get backs off. */
		__atomic_store_n(&inode->i_active, -1, __ATOMIC_RELEASE);

		cds_list_del_init(&inode->i_lru);
		__atomic_fetch_and(&inode->i_state, ~INODE_IS_ON_LRU,
				   __ATOMIC_RELAXED);
		sb->sb_inode_lru_count--;
		evicted++;

		/* Move to local list for phase 2 processing. */
		cds_list_add_tail(&inode->i_lru, &evictees);
	}
	pthread_mutex_unlock(&sb->sb_inode_lru_lock);

	/*
	 * Phase 2: sync and release WITHOUT holding the LRU lock.
	 * i_active = -1 (tombstone) prevents concurrent reactivation.
	 * This avoids blocking inode_lru_add on other worker threads
	 * while we do disk I/O.
	 */
	cds_list_for_each_entry_safe(inode, tmp, &evictees, i_lru) {
		cds_list_del_init(&inode->i_lru);

		if (sb->sb_ops && sb->sb_ops->inode_sync)
			sb->sb_ops->inode_sync(inode);

		inode_unhash(inode);
		inode_put(inode);
	}
}

/*
 * Evict up to 'count' leaf dirents from the dirent LRU.
 *
 * A dirent is evicted by:
 *   1. Verifying rd_active is still 0.
 *   2. Verifying its rd_children list is still empty (still a leaf).
 *   3. Removing from parent's children list via dirent_parent_release.
 *   4. Dropping the list-held ref so dirent_release fires via RCU.
 *
 * The inode weak pointer (rd_inode) is NULLed in dirent_release, so there
 * is no dangling pointer to worry about.
 */
void super_block_evict_dirents(struct super_block *sb, size_t count)
{
	struct reffs_dirent *rd, *tmp;
	size_t evicted = 0;
	CDS_LIST_HEAD(evictees);

	/*
	 * Phase 1: collect eviction candidates under the LRU lock.
	 * Tombstone rd_active = -1 prevents dirent_active_get from
	 * racing (it checks prev < 0 and backs off).
	 */
	pthread_mutex_lock(&sb->sb_dirent_lru_lock);
	cds_list_for_each_entry_safe(rd, tmp, &sb->sb_dirent_lru, rd_lru) {
		if (evicted >= count)
			break;

		int64_t active =
			__atomic_load_n(&rd->rd_active, __ATOMIC_ACQUIRE);
		if (active != 0)
			continue;

		/* Must still be a leaf. */
		if (!cds_list_empty(&rd->rd_children))
			continue;

		/* Tombstone: prevents concurrent reactivation. */
		__atomic_store_n(&rd->rd_active, -1, __ATOMIC_RELEASE);

		cds_list_del_init(&rd->rd_lru);
		__atomic_fetch_and(&rd->rd_state, ~DIRENT_IS_ON_LRU,
				   __ATOMIC_RELAXED);
		sb->sb_dirent_lru_count--;
		evicted++;

		cds_list_add_tail(&rd->rd_lru, &evictees);
	}
	pthread_mutex_unlock(&sb->sb_dirent_lru_lock);

	/*
	 * Phase 2: sync and release WITHOUT holding the LRU lock.
	 */
	cds_list_for_each_entry_safe(rd, tmp, &evictees, rd_lru) {
		cds_list_del_init(&rd->rd_lru);

		if (rd->rd_parent)
			dirent_sync_to_disk(rd->rd_parent);

		/*
		 * Release from parent.  reffs_life_action_unload means:
		 * unlink from tree, do not decrement nlink, do not write disk.
		 */
		dirent_parent_release(rd, reffs_life_action_unload);

		/* Drop our LRU-held ref. */
		dirent_put(rd);
	}
}

/* ------------------------------------------------------------------ */
/* Teardown helpers                                                     */
/* ------------------------------------------------------------------ */

void super_block_release_dirents(struct super_block *sb)
{
	if (!sb)
		return;

	/* Drop permanent active ref on root inode before tearing down tree. */
	struct inode *root_inode = sb->sb_root_inode;
	sb->sb_root_inode = NULL;
	if (root_inode)
		inode_active_put(root_inode);

	struct reffs_dirent *rd;

	rd = rcu_xchg_pointer(&sb->sb_dirent, NULL);
	if (rd) {
		dirent_get(rd);
		dirent_children_release(rd, reffs_life_action_shutdown);
		dirent_parent_release(rd, reffs_life_action_shutdown);
		dirent_put(rd); /* walk ref */
		dirent_put(rd); /* alloc ref */
	}

	super_block_drain(sb);
	/* rcu_barrier is called inside super_block_drain; no second one needed */
}

static void super_block_remove_all_inodes(struct super_block *sb)
{
	struct cds_lfht_iter iter;
	struct inode *inode;

	for (int i = 0;
	     __atomic_load_n(&sb->sb_delayed_count, __ATOMIC_RELAXED) > 0;
	     i++) {
		if (i % 20 == 0)
			LOG("Waiting for delayed releases to drain (%lu remaining)",
			    sb->sb_delayed_count);
		usleep(50000); /* 50ms */
	}

	/*
	 * Unhash each inode and pull it off the LRU atomically under the
	 * LRU lock.  We do this inode-by-inode so that we hold a valid
	 * reference to the inode struct (via the hash iteration) when we
	 * touch i_lru and i_state.
	 *
	 * We must NOT walk sb_inode_lru as a separate list pass: any
	 * intervening rcu_barrier() (from another subsystem or a concurrent
	 * call_rcu flush) could have already run inode_free_rcu on inodes
	 * that were queued for RCU free before we got here, making their
	 * i_lru fields freed memory.
	 *
	 * inode_release / inode_free_rcu never touch sb_inode_lru_count, so
	 * we are responsible for decrementing it here for each LRU inode we
	 * remove.
	 */
	pthread_mutex_lock(&sb->sb_inode_lru_lock);
	if (!sb->sb_inodes) {
		pthread_mutex_unlock(&sb->sb_inode_lru_lock);
		return;
	}
	rcu_read_lock();
	cds_lfht_first(sb->sb_inodes, &iter);
	while (cds_lfht_iter_get_node(&iter) != NULL) {
		inode = caa_container_of(cds_lfht_iter_get_node(&iter),
					 struct inode, i_node);
		cds_lfht_next(sb->sb_inodes, &iter);

		/* Pull off LRU if present, decrement count. */
		uint64_t old = __atomic_fetch_and(
			&inode->i_state, ~INODE_IS_ON_LRU, __ATOMIC_ACQ_REL);
		if (old & INODE_IS_ON_LRU) {
			cds_list_del_init(&inode->i_lru);
			sb->sb_inode_lru_count--;
		}

		__atomic_fetch_or(&inode->i_state, INODE_IS_SHUTTING_DOWN,
				  __ATOMIC_ACQUIRE);

		inode_remove_all_stateids(inode);

		if (inode_unhash(inode))
			inode_put(inode);
	}
	rcu_read_unlock();
	pthread_mutex_unlock(&sb->sb_inode_lru_lock);

	rcu_barrier();
}

static void super_block_free(struct super_block *sb)
{
	if (!sb)
		return;

	trace_fs_super_block(sb, __func__, __LINE__);

	if (sb->sb_ops && sb->sb_ops->sb_free)
		sb->sb_ops->sb_free(sb);

	reffs_backend_free_ops(sb->sb_ops);
	sb->sb_ops = NULL;

	int ret = cds_lfht_destroy(sb->sb_inodes, NULL);
	if (ret < 0) {
		LOG("Could not delete a hash table: %m");
	}

	pthread_mutex_destroy(&sb->sb_inode_lru_lock);
	pthread_mutex_destroy(&sb->sb_dirent_lru_lock);

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

	/*
	 * Do NOT call super_block_remove_all_inodes() here.
	 *
	 * Each inode holds a super_block_get() ref.  inode_release() calls
	 * super_block_put(), which would re-enter super_block_release() and
	 * call super_block_remove_all_inodes() again while we are still
	 * iterating the hash table -- causing double inode_put() and a
	 * urcu_ref underflow.
	 *
	 * Callers must drain inodes explicitly (via super_block_put_all())
	 * before dropping the last sb ref.
	 */

	trace_fs_super_block(sb, __func__, __LINE__);
	call_rcu(&sb->sb_rcu, super_block_free_rcu);
}

/* ------------------------------------------------------------------ */
/* Explicit inode drain (must be called before last super_block_put)   */
/* ------------------------------------------------------------------ */

/*
 * super_block_drain -- flush all inodes out of the hash table.
 *
 * Must be called while the caller still holds a ref on sb (i.e. before
 * the final super_block_put).  This prevents the re-entrancy problem
 * where inode_release -> super_block_put -> super_block_release would
 * call super_block_remove_all_inodes a second time.
 */
void super_block_drain(struct super_block *sb)
{
	if (!sb)
		return;
	trace_fs_super_block(sb, __func__, __LINE__);
	super_block_remove_all_inodes(sb);
}

/* ------------------------------------------------------------------ */
/* Dirent create / release                                             */
/* ------------------------------------------------------------------ */

int super_block_dirent_create(struct super_block *sb, struct reffs_dirent *rd,
			      enum reffs_life_action rla)
{
	sb->sb_dirent = dirent_alloc(rd, "/", rla, true);
	if (!sb->sb_dirent)
		return -ENOMEM;

	uint64_t new_ino =
		__atomic_add_fetch(&sb->sb_next_ino, 1, __ATOMIC_RELAXED);
	/*
	 * inode_alloc returns with i_active == 1.  rd_inode is a weak pointer
	 * and does not own an active ref, so drop it immediately.
	 */
	struct inode *root_inode = inode_alloc(sb, new_ino);
	if (!root_inode) {
		dirent_put(sb->sb_dirent);
		return -ENOMEM;
	}
	sb->sb_dirent->rd_ino = new_ino;
	sb->sb_dirent->rd_sb =
		sb; /* root dirent has no parent to inherit from */

	/*
	 * Set defaults only if the inode was NOT loaded from persistence.
	 * The inode_alloc hook (posix/rocksdb) loads persisted fields;
	 * if i_mode is non-zero, the inode was loaded and we must not
	 * overwrite its persisted attributes.
	 */
	if (rla == reffs_life_action_birth && root_inode->i_mode == 0) {
		struct timespec now;

		clock_gettime(CLOCK_REALTIME, &now);
		root_inode->i_nlink = 2;
		root_inode->i_mode = S_IFDIR | 0777;
		root_inode->i_atime = now;
		root_inode->i_mtime = now;
		root_inode->i_ctime = now;
		root_inode->i_btime = now;
		inode_set_default_sec_label(root_inode);
	}
	if (root_inode->i_parent_ino == 0)
		root_inode->i_parent_ino = new_ino; /* root: self */

	/*
	 * Wire up rd_inode / i_dirent via the helper so
	 * inode_release can null rd_inode before call_rcu.
	 */
	dirent_attach_inode(sb->sb_dirent, root_inode);

	/*
	 * Keep the active ref on root_inode alive for the lifetime of the
	 * mount.  The root inode must never be evicted by the LRU (i_active
	 * must stay > 0).  super_block_dirent_release drops this ref at
	 * unmount time, after which the normal teardown path frees the inode.
	 */
	sb->sb_root_inode =
		root_inode; /* strong active ref, dropped at release */

	return 0;
}

void super_block_dirent_release(struct super_block *sb,
				enum reffs_life_action rla)
{
	struct reffs_dirent *rd;
	struct inode *root_inode;

	/*
	 * Drop the permanent active ref on the root inode before releasing
	 * the dirent tree, so inode_release can proceed normally during
	 * teardown.
	 */
	root_inode = sb->sb_root_inode;
	sb->sb_root_inode = NULL;
	if (root_inode)
		inode_active_put(root_inode);

	rcu_read_lock();
	rd = rcu_xchg_pointer(&sb->sb_dirent, NULL);
	if (rd) {
		dirent_parent_release(rd, rla);
		dirent_put(rd);
	}
	rcu_read_unlock();
}

/* ------------------------------------------------------------------ */
/* Alloc / find                                                         */
/* ------------------------------------------------------------------ */

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
	if (!sb->sb_path) {
		free(sb);
		return NULL;
	}

	sb->sb_storage_type = storage_type;
	if (backend_path) {
		sb->sb_backend_path = strdup(backend_path);
		if (!sb->sb_backend_path) {
			free(sb->sb_path);
			free(sb);
			return NULL;
		}
	}

	/* Map storage_type to (md, data) pair and compose */
	enum reffs_md_type md;
	enum reffs_data_type data;

	switch (storage_type) {
	case REFFS_STORAGE_RAM:
		md = REFFS_MD_RAM;
		data = REFFS_DATA_RAM;
		break;
	case REFFS_STORAGE_POSIX:
		md = REFFS_MD_POSIX;
		data = REFFS_DATA_POSIX;
		break;
	case REFFS_STORAGE_ROCKSDB:
		md = REFFS_MD_ROCKSDB;
		data = REFFS_DATA_POSIX;
		break;
	default:
		free(sb->sb_path);
		free(sb->sb_backend_path);
		free(sb);
		return NULL;
	}

	sb->sb_ops = reffs_backend_compose(md, data);
	if (!sb->sb_ops) {
		free(sb->sb_path);
		free(sb->sb_backend_path);
		free(sb);
		return NULL;
	}

	CDS_INIT_LIST_HEAD(&sb->sb_link);

	CDS_INIT_LIST_HEAD(&sb->sb_inode_lru);
	pthread_mutex_init(&sb->sb_inode_lru_lock, NULL);
	sb->sb_inode_lru_max = SB_INODE_LRU_MAX_DEFAULT;

	CDS_INIT_LIST_HEAD(&sb->sb_dirent_lru);
	pthread_mutex_init(&sb->sb_dirent_lru_lock, NULL);
	sb->sb_dirent_lru_max = SB_DIRENT_LRU_MAX_DEFAULT;

	sb->sb_inodes = cds_lfht_new(
		8, 8, 0, CDS_LFHT_AUTO_RESIZE | CDS_LFHT_ACCOUNTING, NULL);
	if (!sb->sb_inodes) {
		LOG("Could not create a new hash table");
		super_block_free(sb);
		return NULL;
	}

	urcu_ref_init(&sb->sb_ref);

	/*
	 * sb_uuid is NOT generated here.  Callers are responsible:
	 * - New sbs: uuid_generate(sb->sb_uuid) after alloc
	 * - Loaded sbs: uuid_copy(sb->sb_uuid, persisted) after alloc
	 * This ensures UUIDs are stable across restarts (reviewer rule 8).
	 */

	sb->sb_bytes_max = SIZE_MAX;
	sb->sb_inodes_max = SIZE_MAX;

	if (sb->sb_ops->sb_alloc) {
		int ret = sb->sb_ops->sb_alloc(sb, backend_path);
		if (ret != 0) {
			super_block_free(sb);
			return NULL;
		}
	}

	__atomic_fetch_or(&sb->sb_state, SB_IN_LIST, __ATOMIC_RELEASE);
	cds_list_add_rcu(&sb->sb_link, &super_block_list);

	trace_fs_super_block(sb, __func__, __LINE__);

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

	if (sb)
		trace_fs_super_block(sb, __func__, __LINE__);

	return sb;
}

struct super_block *super_block_get(struct super_block *sb)
{
	if (!sb)
		return NULL;

	trace_fs_super_block(sb, __func__, __LINE__);

	if (!urcu_ref_get_unless_zero(&sb->sb_ref))
		return NULL;

	return sb;
}

void super_block_put(struct super_block *sb)
{
	if (!sb)
		return;

	trace_fs_super_block(sb, __func__, __LINE__);

	urcu_ref_put(&sb->sb_ref, super_block_release);
}

/* ------------------------------------------------------------------ */
/* Lifecycle state machine                                             */
/* ------------------------------------------------------------------ */

int super_block_mount(struct super_block *sb, const char *path)
{
	struct name_match *nm = NULL;
	int ret;

	if (!sb || !path)
		return -EINVAL;

	/* Only CREATED or UNMOUNTED can transition to MOUNTED. */
	if (sb->sb_lifecycle == SB_MOUNTED)
		return -EBUSY;
	if (sb->sb_lifecycle == SB_DESTROYED)
		return -EINVAL;

	/* Validate that 'path' exists as a directory. */
	struct stat st;

	if (reffs_fs_getattr(path, &st))
		return -ENOENT;
	if (!S_ISDIR(st.st_mode))
		return -ENOTDIR;

	/*
	 * Resolve path to a dirent and set RD_MOUNTED_ON.
	 * This flag tells LOOKUP to cross into the child sb.
	 */
	ret = find_matching_directory_entry(&nm, path, LAST_COMPONENT_IS_MATCH);
	if (ret)
		return ret;

	__atomic_fetch_or(&nm->nm_dirent->rd_state, RD_MOUNTED_ON,
			  __ATOMIC_RELEASE);

	/* Store mount-point references on the child sb. */
	sb->sb_mount_dirent = dirent_get(nm->nm_dirent);
	sb->sb_parent_sb = super_block_find(SUPER_BLOCK_ROOT_ID);
	if (!sb->sb_parent_sb) {
		__atomic_fetch_and(&nm->nm_dirent->rd_state, ~RD_MOUNTED_ON,
				   __ATOMIC_RELEASE);
		dirent_put(sb->sb_mount_dirent);
		sb->sb_mount_dirent = NULL;
		name_match_free(nm);
		return -ENOENT;
	}

	name_match_free(nm);

	sb->sb_lifecycle = SB_MOUNTED;
	return 0;
}

int super_block_unmount(struct super_block *sb)
{
	if (!sb)
		return -EINVAL;

	/* Root sb cannot be unmounted. */
	if (sb->sb_id == SUPER_BLOCK_ROOT_ID)
		return -EPERM;

	if (sb->sb_lifecycle != SB_MOUNTED)
		return -EINVAL;

	/* Check for child sbs mounted within this sb's namespace. */
	{
		struct super_block *tmp;

		rcu_read_lock();
		cds_list_for_each_entry_rcu(tmp, &super_block_list, sb_link) {
			if (tmp->sb_parent_sb == sb &&
			    tmp->sb_lifecycle == SB_MOUNTED) {
				rcu_read_unlock();
				return -EBUSY;
			}
		}
		rcu_read_unlock();
	}

	/* Clear RD_MOUNTED_ON on the mounted-on dirent. */
	if (sb->sb_mount_dirent) {
		__atomic_fetch_and(&sb->sb_mount_dirent->rd_state,
				   ~RD_MOUNTED_ON, __ATOMIC_RELEASE);
		dirent_put(sb->sb_mount_dirent);
		sb->sb_mount_dirent = NULL;
	}

	super_block_put(sb->sb_parent_sb);
	sb->sb_parent_sb = NULL;

	sb->sb_lifecycle = SB_UNMOUNTED;
	return 0;
}

int super_block_destroy(struct super_block *sb)
{
	if (!sb)
		return -EINVAL;

	/* Root sb cannot be destroyed. */
	if (sb->sb_id == SUPER_BLOCK_ROOT_ID)
		return -EPERM;

	/* Must unmount before destroying. */
	if (sb->sb_lifecycle == SB_MOUNTED)
		return -EBUSY;

	/* Already destroyed. */
	if (sb->sb_lifecycle == SB_DESTROYED)
		return -EINVAL;

	/*
	 * NOT_NOW_BROWN_COW: check for active open files.
	 * Return -EBUSY if any stateid references this sb.
	 */

	sb->sb_lifecycle = SB_DESTROYED;
	return 0;
}

enum sb_lifecycle super_block_lifecycle(const struct super_block *sb)
{
	return sb->sb_lifecycle;
}

static const char *lifecycle_names[] = { "CREATED", "MOUNTED", "UNMOUNTED",
					 "DESTROYED" };

const char *super_block_lifecycle_name(enum sb_lifecycle state)
{
	if (state <= SB_DESTROYED)
		return lifecycle_names[state];
	return "UNKNOWN";
}

struct super_block *super_block_find_mounted_on(struct reffs_dirent *de)
{
	struct super_block *sb = NULL;
	struct super_block *tmp;

	if (!de)
		return NULL;

	rcu_read_lock();
	cds_list_for_each_entry_rcu(tmp, &super_block_list, sb_link) {
		if (tmp->sb_mount_dirent == de &&
		    tmp->sb_lifecycle == SB_MOUNTED) {
			sb = super_block_get(tmp);
			break;
		}
	}
	rcu_read_unlock();

	return sb;
}

void super_block_set_flavors(struct super_block *sb,
			     const enum reffs_auth_flavor *flavors,
			     unsigned int nflavors)
{
	if (!sb)
		return;

	unsigned int n = nflavors;

	if (n > REFFS_CONFIG_MAX_FLAVORS)
		n = REFFS_CONFIG_MAX_FLAVORS;

	if (n > 0 && flavors)
		memcpy(sb->sb_flavors, flavors, n * sizeof(sb->sb_flavors[0]));
	sb->sb_nflavors = n;
}

int super_block_lint_flavors(void)
{
	struct super_block *sb;
	int warnings = 0;

	rcu_read_lock();
	cds_list_for_each_entry_rcu(sb, &super_block_list, sb_link) {
		if (sb->sb_id == SUPER_BLOCK_ROOT_ID)
			continue;
		if (sb->sb_lifecycle != SB_MOUNTED)
			continue;
		if (!sb->sb_parent_sb)
			continue;

		/*
		 * Check that every flavor in this child sb is also
		 * present in the parent sb's flavor list.
		 */
		struct super_block *parent = sb->sb_parent_sb;

		for (unsigned int i = 0; i < sb->sb_nflavors; i++) {
			int found = 0;

			for (unsigned int j = 0; j < parent->sb_nflavors; j++) {
				if (sb->sb_flavors[i] ==
				    parent->sb_flavors[j]) {
					found = 1;
					break;
				}
			}
			if (!found) {
				LOG("lint-flavors: sb %lu requires flavor %u "
				    "not in parent sb %lu",
				    (unsigned long)sb->sb_id,
				    (unsigned)sb->sb_flavors[i],
				    (unsigned long)parent->sb_id);
				warnings++;
			}
		}
	}
	rcu_read_unlock();

	return warnings;
}

/*
 * Check whether a path/prefix is a proper parent of child.
 * "/foo" is a prefix of "/foo/bar" (next char is '/').
 * "/foo" is NOT a prefix of "/foobar" (next char is 'b').
 * "/" is a prefix of everything but we exempt root from checks.
 */
static bool is_path_prefix(const char *parent, const char *child)
{
	size_t len = strlen(parent);

	if (strcmp(parent, "/") == 0)
		return true;
	return strncmp(parent, child, len) == 0 &&
	       (child[len] == '/' || child[len] == '\0');
}

int super_block_check_path_conflict(const char *path)
{
	struct super_block *sb;

	if (!path)
		return -EINVAL;

	rcu_read_lock();
	cds_list_for_each_entry_rcu(sb, &super_block_list, sb_link) {
		/* Root is the pseudo-root -- prefix of everything by design. */
		if (sb->sb_id == SUPER_BLOCK_ROOT_ID)
			continue;
		if (sb->sb_lifecycle != SB_MOUNTED)
			continue;
		if (!sb->sb_path)
			continue;

		/* Exact match: same path already mounted. */
		if (strcmp(sb->sb_path, path) == 0) {
			rcu_read_unlock();
			return -EEXIST;
		}

		/*
		 * New path is a parent of an existing mount.
		 * This would change namespace traversal for the child.
		 */
		if (is_path_prefix(path, sb->sb_path)) {
			rcu_read_unlock();
			return -EBUSY;
		}
	}
	rcu_read_unlock();

	return 0;
}
