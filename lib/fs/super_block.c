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

		/*
		 * Sync to disk before eviction so data is durable.
		 * This is done under the LRU lock to prevent a concurrent
		 * inode_active_get from seeing a partially-written state.
		 */
		if (sb->sb_ops && sb->sb_ops->inode_sync)
			sb->sb_ops->inode_sync(inode);

		/* Drop the hash-table ref.  This may call inode_release. */
		inode_unhash(inode);
		inode_put(inode);
	}
	pthread_mutex_unlock(&sb->sb_inode_lru_lock);
}

/*
 * Evict up to 'count' leaf dirents from the dirent LRU.
 *
 * A dirent is evicted by:
 *   1. Verifying rd_active is still 0.
 *   2. Verifying its inode's i_children list is still empty (still a leaf).
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

	pthread_mutex_lock(&sb->sb_dirent_lru_lock);
	cds_list_for_each_entry_safe(rd, tmp, &sb->sb_dirent_lru, rd_lru) {
		if (evicted >= count)
			break;

		int64_t active =
			__atomic_load_n(&rd->rd_active, __ATOMIC_ACQUIRE);
		if (active != 0)
			continue;

		/* Must still be a leaf. */
		if (rd->rd_inode && !cds_list_empty(&rd->rd_inode->i_children))
			continue;

		/* Sync directory state before eviction. */
		if (rd->rd_parent)
			dirent_sync_to_disk(rd->rd_parent);

		cds_list_del_init(&rd->rd_lru);
		__atomic_fetch_and(&rd->rd_state, ~DIRENT_IS_ON_LRU,
				   __ATOMIC_RELAXED);
		sb->sb_dirent_lru_count--;
		evicted++;

		/*
		 * Release from parent.  reffs_life_action_unload means:
		 * unlink from tree, do not decrement nlink, do not write disk.
		 */
		dirent_parent_release(rd, reffs_life_action_unload);

		/* Drop our LRU-held ref. */
		dirent_put(rd);
	}
	pthread_mutex_unlock(&sb->sb_dirent_lru_lock);
}

/* ------------------------------------------------------------------ */
/* Teardown helpers                                                     */
/* ------------------------------------------------------------------ */

/*
 * release_dirents_recursive -- recursively release a dirent subtree.
 *
 * PRECONDITION: rd_parent->rd_inode must not be a freed pointer.
 *
 * rd_inode is a weak pointer that is never nulled by the LRU eviction path
 * (super_block_evict_inodes sets i_active=-1 and drops the hash ref, but
 * does not null rd_inode on the dirent).  After inode_release calls
 * call_rcu(inode_free_rcu) and a grace period elapses, the inode struct is
 * freed while rd_inode still points at it.
 *
 * This function is safe to call only when no inode reachable via rd_inode
 * in the subtree has been freed.  That invariant is guaranteed by the normal
 * operational path (no test calls drain_lru before teardown).  If the LRU
 * eviction path is exercised before teardown, callers must ensure all evicted
 * inodes have their rd_inode nulled first.
 *
 * TODO: the correct long-term fix is to add an i_dirent weak back-pointer
 * to struct inode and null rd_inode from inode_release() before call_rcu,
 * making the invariant self-enforcing.  Until then, callers must not allow
 * evicted-inode dirents to remain in the tree at teardown time.
 *
 * Observed crashes (2026-03-08) from violating this invariant:
 *   READ of size 8 ... super_block.c:159  direct rd_inode dereference
 *   READ of size 8 ... urcu/ref.h:83      after dirent_ensure_inode attempt
 *     (dirent_ensure_inode's fast path calls inode_active_get on rd_inode
 *      without first verifying the struct is still allocated; same UAF,
 *      one frame deeper)
 */
static void release_dirents_recursive(struct reffs_dirent *rd_parent)
{
	struct reffs_dirent *rd, *tmp;

	if (!rd_parent || !rd_parent->rd_inode)
		return;

	/*
	 * Use _safe variant: dirent_parent_release removes the entry from
	 * i_children so we must not hold a pointer to rd_siblings across it.
	 * Collect the next pointer before the list is mutated.
	 */
	cds_list_for_each_entry_safe(rd, tmp, &rd_parent->rd_inode->i_children,
				     rd_siblings) {
		release_dirents_recursive(rd);
		dirent_parent_release(rd, reffs_life_action_death);
	}
}

void super_block_release_dirents(struct super_block *sb)
{
	if (!sb)
		return;

	struct reffs_dirent *rd;

	rcu_barrier();

	/* Phase 1: walk and release the full dirent tree */
	rd = rcu_xchg_pointer(&sb->sb_dirent, NULL);
	if (rd) {
		dirent_get(rd);
		release_dirents_recursive(rd);
		dirent_parent_release(rd, reffs_life_action_death);
		dirent_put(rd); /* walk ref */
		dirent_put(rd); /* alloc ref */
	}

	/* Phase 2: drain inodes */
	super_block_drain(sb);
	rcu_barrier();
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
		if (inode_unhash(inode))
			inode_put(inode);
	}
	rcu_read_unlock();

	rcu_barrier();

#ifdef NOT_NOW_BROWN_COW
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
#endif
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
	 * iterating the hash table — causing double inode_put() and a
	 * urcu_ref underflow.
	 *
	 * Callers must drain inodes explicitly (via super_block_put_all())
	 * before dropping the last sb ref.
	 */

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
	sb->sb_dirent->rd_inode = root_inode;
	sb->sb_dirent->rd_ino = new_ino;

	if (rla == reffs_life_action_birth) {
		root_inode->i_nlink = 2;
		root_inode->i_mode = S_IFDIR | 0777;
	}
	root_inode->i_parent = sb->sb_dirent;
	root_inode->i_parent_ino = new_ino; /* root: self */

	inode_active_put(root_inode); /* drop active ref; weak ptr needs none */

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

	uuid_generate(sb->sb_uuid);

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
