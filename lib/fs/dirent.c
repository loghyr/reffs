/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "reffs/rcu.h"
#include "reffs/backend.h"
#include "reffs/cmp.h"
#include "reffs/dirent.h"
#include "reffs/inode.h"
#include "reffs/log.h"
#include "reffs/super_block.h"
#include "reffs/test.h"
#include "reffs/trace/fs.h"
#include "reffs/types.h"

struct rcu_head;
#include "reffs/types.h"
#include "reffs/cmp.h"
#include "reffs/trace/fs.h"

CDS_LIST_HEAD(dirent_list);

/* ------------------------------------------------------------------ */
/* LRU helpers                                                          */
/* ------------------------------------------------------------------ */

/*
 * Add a dirent to the tail of the sb dirent LRU.
 * Called when rd_active drops to zero.
 * A dirent is only eligible if it has no in-memory children (leaf node).
 */
static void dirent_lru_add(struct reffs_dirent *rd)
{
	struct super_block *sb;

	if (!rd->rd_inode || !rd->rd_inode->i_sb)
		return;

	/* Only leaf dirents can be evicted; skip directories with children. */
	if (!cds_list_empty(&rd->rd_children))
		return;

	sb = rd->rd_inode->i_sb;

	pthread_mutex_lock(&sb->sb_dirent_lru_lock);
	if (!(__atomic_fetch_or(&rd->rd_state, DIRENT_IS_ON_LRU,
				__ATOMIC_ACQ_REL) &
	      DIRENT_IS_ON_LRU)) {
		cds_list_add_tail(&rd->rd_lru, &sb->sb_dirent_lru);
		sb->sb_dirent_lru_count++;
	}
	pthread_mutex_unlock(&sb->sb_dirent_lru_lock);

	if (sb->sb_dirent_lru_count > sb->sb_dirent_lru_max)
		super_block_evict_dirents(sb, sb->sb_dirent_lru_count -
						      sb->sb_dirent_lru_max);
}

/*
 * Remove a dirent from the LRU (re-activated).
 * Caller must hold sb_dirent_lru_lock.
 */
static void dirent_lru_del_locked(struct reffs_dirent *rd)
{
	uint64_t old = __atomic_fetch_and(&rd->rd_state, ~DIRENT_IS_ON_LRU,
					  __ATOMIC_ACQ_REL);
	if (old & DIRENT_IS_ON_LRU) {
		struct super_block *sb = rd->rd_inode->i_sb;
		cds_list_del_init(&rd->rd_lru);
		sb->sb_dirent_lru_count--;
	}
}

/* ------------------------------------------------------------------ */
/* Memory-lifetime ref                                                  */
/* ------------------------------------------------------------------ */

static void dirent_free_rcu(struct rcu_head *rcu)
{
	struct reffs_dirent *rd =
		caa_container_of(rcu, struct reffs_dirent, rd_rcu);

	trace_fs_dirent(rd, __func__, __LINE__);

	pthread_rwlock_destroy(&rd->rd_rwlock);

	free(rd->rd_name);
	free(rd);
}

static void dirent_release(struct urcu_ref *ref)
{
	struct reffs_dirent *rd =
		caa_container_of(ref, struct reffs_dirent, rd_ref);
	struct reffs_dirent *parent;

	trace_fs_dirent(rd, __func__, __LINE__);

	rd->rd_inode = NULL; /* prevent stale access after RCU free */

	/*
	 * If rd_parent is still set, detach from the parent's rd_children list
	 * and drop the parent's ref.  This is the "unload" cleanup path for
	 * dirents that reached ref=0 without an explicit dirent_parent_release
	 * call (e.g. error paths).
	 *
	 * We must NOT call dirent_parent_release() here because that function
	 * ends with dirent_put(rd), which would decrement rd_ref from 0 to -1
	 * and re-enter dirent_release — a double-free.
	 *
	 * Instead we do the minimal inline cleanup: remove from the siblings
	 * list and drop the parent ref.  We do NOT call dirent_put(rd) because
	 * the caller already consumed the last ref to get here.
	 */
	rcu_read_lock();
	parent = rcu_xchg_pointer(&rd->rd_parent, NULL);
	if (parent) {
		cds_list_del_rcu(&rd->rd_siblings);
		dirent_put(parent);
		/* NOTE: dirent_put(rd) is deliberately omitted — rd_ref is already 0. */
	}
	rcu_read_unlock();

	call_rcu(&rd->rd_rcu, dirent_free_rcu);
}

struct reffs_dirent *dirent_get(struct reffs_dirent *rd)
{
	if (!rd)
		return NULL;

	if (!urcu_ref_get_unless_zero(&rd->rd_ref))
		return NULL;

	trace_fs_dirent(rd, __func__, __LINE__);

	return rd;
}

void dirent_put(struct reffs_dirent *rd)
{
	if (!rd)
		return;

	trace_fs_dirent(rd, __func__, __LINE__);
	urcu_ref_put(&rd->rd_ref, dirent_release);
}

/* ------------------------------------------------------------------ */
/* Active-use ref                                                       */
/* ------------------------------------------------------------------ */

struct reffs_dirent *dirent_active_get(struct reffs_dirent *rd)
{
	if (!rd)
		return NULL;

	if (!dirent_get(rd))
		return NULL;

	int64_t prev = __atomic_fetch_add(&rd->rd_active, 1, __ATOMIC_ACQ_REL);
	if (prev < 0) {
		__atomic_fetch_sub(&rd->rd_active, 1, __ATOMIC_RELAXED);
		dirent_put(rd);
		return NULL;
	}

	/* Pull off the LRU if sitting there. */
	if (rd->rd_inode && rd->rd_inode->i_sb) {
		struct super_block *sb = rd->rd_inode->i_sb;
		pthread_mutex_lock(&sb->sb_dirent_lru_lock);
		dirent_lru_del_locked(rd);
		pthread_mutex_unlock(&sb->sb_dirent_lru_lock);
	}

	return rd;
}

void dirent_active_put(struct reffs_dirent *rd)
{
	if (!rd)
		return;

	trace_fs_dirent(rd, __func__, __LINE__);
	int64_t remaining =
		__atomic_sub_fetch(&rd->rd_active, 1, __ATOMIC_ACQ_REL);
	if (remaining == 0)
		dirent_lru_add(rd);

	dirent_put(rd);
}

/* ------------------------------------------------------------------ */
/* Ensure inode is loaded for a dirent                                  */
/* ------------------------------------------------------------------ */

/*
 * dirent_ensure_inode -- return an active-ref-held inode for this dirent,
 * loading from disk if it was evicted.  Returns NULL on failure.
 * Caller must call inode_active_put() when done.
 */
struct inode *dirent_ensure_inode(struct reffs_dirent *rd)
{
	struct inode *inode;

	if (!rd)
		return NULL;

	/*
	 * Fast path: rd_inode is still in memory.  Grab an active ref;
	 */
	rcu_read_lock();
	inode = rcu_dereference(rd->rd_inode);
	if (inode)
		inode = inode_active_get_rcu(inode);
	rcu_read_unlock();

	if (inode) {
		inode_active_lru_pull(inode);
		return inode;
	}

	/* Miss: rd_inode was evicted.  Reload by ino number. */
	if (!rd->rd_ino) {
		/*
		 * rd_ino == 0 means the dirent was never fully attached
		 * (e.g. mid-construction).
		 */
		return NULL;
	}

	/*
	 * rd_sb is set at alloc time (inherited from parent, or set directly
	 * on the root dirent by super_block_dirent_create).  It is always
	 * valid for the lifetime of the dirent, so no parent-chain walk needed.
	 */
	struct super_block *sb = rd->rd_sb;
	if (!sb)
		return NULL;

	/* inode_alloc will find it in the hash or load from disk. */
	inode = inode_alloc(sb, rd->rd_ino);
	if (inode) {
		/*
		 * Re-attach via dirent_attach_inode so i_dirent is set on the
		 * reloaded inode.  Without this, a subsequent eviction would
		 * find i_dirent == NULL and skip nulling rd_inode, re-opening
		 * the UAF window.
		 *
		 * Benign race: two threads may both call dirent_attach_inode
		 * concurrently and set the same values.
		 */
		dirent_attach_inode(rd, inode);
	}

	return inode;
}

/* ------------------------------------------------------------------ */
/* Inode attach                                                         */
/* ------------------------------------------------------------------ */

/*
 * dirent_attach_inode - canonical write site for rd->rd_inode / inode->i_dirent.
 *
 * Every place that wires up a new rd_inode must go through here so that
 * inode_release() can null rd_inode (via i_dirent) before call_rcu, closing
 * the UAF window in dirent_ensure_inode's fast path.
 *
 * Caller must hold a reference that keeps inode alive for the duration of
 * this call (e.g. an active ref or the inode hash ref).
 */
void dirent_attach_inode(struct reffs_dirent *rd, struct inode *inode)
{
	rcu_assign_pointer(rd->rd_inode, inode);
	inode->i_dirent = rd;
}

/* ------------------------------------------------------------------ */
/* Parent attach / detach                                               */
/* ------------------------------------------------------------------ */

void dirent_parent_attach(struct reffs_dirent *rd, struct reffs_dirent *parent,
			  enum reffs_life_action rla, bool is_dir)
{
	if (!rd || !parent)
		return;

	rcu_read_lock();
	rd->rd_parent = dirent_get(parent);
	rd->rd_sb = parent->rd_sb; /* inherit sb pointer from parent */
	verify(S_ISDIR(parent->rd_inode->i_mode));
	if (rla != reffs_life_action_load && is_dir) {
		__atomic_fetch_add(&parent->rd_inode->i_nlink, 1,
				   __ATOMIC_RELAXED);
	}
	if (rla != reffs_life_action_load) {
		rd->rd_cookie = __atomic_add_fetch(&parent->rd_cookie_next, 1,
						   __ATOMIC_RELAXED);
	}
	cds_list_add_tail_rcu(&rd->rd_siblings, &parent->rd_children);
	dirent_get(rd); /* One for the linked list */

	if (rd->rd_inode) {
		if (rla != reffs_life_action_load &&
		    rla != reffs_life_action_unload)
			inode_sync_to_disk(rd->rd_inode);
	}

	if (rla != reffs_life_action_load && rla != reffs_life_action_unload)
		dirent_sync_to_disk(parent);
	rcu_read_unlock();
}

void dirent_parent_release(struct reffs_dirent *rd, enum reffs_life_action rla)
{
	struct reffs_dirent *parent;

	if (!rd)
		return;

	/*
	 * Shutdown path: the filesystem is being torn down.  On-disk state is
	 * already consistent so no accounting, nlink updates, or disk writes
	 * are needed.  We must NOT dereference rd_inode or parent->rd_inode
	 * because those inodes may have already been freed by
	 * super_block_drain.  Just detach from the sibling list and drop the
	 * dirent refs.
	 */
	if (rla == reffs_life_action_shutdown) {
		rcu_read_lock();
		parent = rcu_xchg_pointer(&rd->rd_parent, NULL);
		if (parent) {
			cds_list_del_rcu(&rd->rd_siblings);
			dirent_put(parent);
			dirent_put(rd);
		}
		rcu_read_unlock();
		return;
	}

	rcu_read_lock();
	parent = rcu_xchg_pointer(&rd->rd_parent, NULL);
	if (parent) {
		if (rd->rd_inode && S_ISDIR(rd->rd_inode->i_mode) &&
		    (rla == reffs_life_action_death ||
		     rla == reffs_life_action_move ||
		     rla == reffs_life_action_delayed_death)) {
			uint32_t old_nlink =
				__atomic_fetch_sub(&parent->rd_inode->i_nlink,
						   1, __ATOMIC_RELAXED);
			if (old_nlink <= 2) {
				LOG("WARNING: nlink for directory (ino %lu) dropped to %u! Resetting to 2 to prevent corruption.",
				    parent->rd_inode->i_ino, old_nlink - 1);
				__atomic_store_n(&parent->rd_inode->i_nlink, 2,
						 __ATOMIC_RELAXED);
			}
		}
		cds_list_del_rcu(&rd->rd_siblings);

		if (rd->rd_inode && (rla == reffs_life_action_death ||
				     rla == reffs_life_action_delayed_death)) {
			int n = S_ISDIR(rd->rd_inode->i_mode) ? 2 : 1;
			uint32_t old_nlink = __atomic_fetch_sub(
				&rd->rd_inode->i_nlink, n, __ATOMIC_RELAXED);
			TRACE("ino=%lu old_nlink=%u sub=%d new_nlink=%u",
			      rd->rd_inode->i_ino, old_nlink, n, old_nlink - n);

			if (old_nlink - n == 0) {
				size_t size = rd->rd_inode->i_size;
				size_t old_used;
				size_t new_used;

				__atomic_fetch_sub(
					&rd->rd_inode->i_sb->sb_inodes_used, 1,
					__ATOMIC_RELAXED);

				do {
					__atomic_load(&rd->rd_inode->i_sb
							       ->sb_bytes_used,
						      &old_used,
						      __ATOMIC_RELAXED);
					if (old_used >= size)
						new_used = old_used - size;
					else
						new_used = 0;
				} while (!__atomic_compare_exchange(
					&rd->rd_inode->i_sb->sb_bytes_used,
					&old_used, &new_used, false,
					__ATOMIC_SEQ_CST, __ATOMIC_RELAXED));
			}

			if (rla != reffs_life_action_load &&
			    rla != reffs_life_action_unload)
				inode_sync_to_disk(rd->rd_inode);

			if (rla == reffs_life_action_delayed_death)
				inode_schedule_delayed_release(
					rd->rd_inode, INODE_RELEASE_HARVEST);
		}

		if (rla != reffs_life_action_load &&
		    rla != reffs_life_action_unload)
			dirent_sync_to_disk(parent);
		dirent_put(parent);
		dirent_put(rd);
	}
	rcu_read_unlock();
}

/* ------------------------------------------------------------------ */
/* Alloc / find                                                         */
/* ------------------------------------------------------------------ */

/* name should be utf8 */
struct reffs_dirent *dirent_alloc(struct reffs_dirent *parent, char *name,
				  enum reffs_life_action rla, bool is_dir)
{
	struct reffs_dirent *rd;

	rd = calloc(1, sizeof(*rd));
	if (!rd) {
		LOG("Could not alloc a rd");
		return NULL;
	}

	rd->rd_name = strdup(name);
	if (!rd->rd_name) {
		LOG("Could not alloc a rd->rd_name");
		free(rd);
		return NULL;
	}

	urcu_ref_init(&rd->rd_ref);
	CDS_INIT_LIST_HEAD(&rd->rd_lru);
	rd->rd_cookie_next = 3;

	trace_fs_dirent(rd, __func__, __LINE__);

	pthread_rwlock_init(&rd->rd_rwlock, NULL);

	CDS_INIT_LIST_HEAD(&rd->rd_siblings);
	CDS_INIT_LIST_HEAD(&rd->rd_children);
	if (parent)
		dirent_parent_attach(rd, parent, rla, is_dir);

	return rd;
}

struct reffs_dirent *dirent_find(struct reffs_dirent *parent,
				 enum reffs_text_case rtc, char *name)
{
	struct reffs_dirent *rd = NULL;
	struct reffs_dirent *tmp;
	reffs_strng_compare cmp = reffs_text_case_cmp_of(rtc);

	assert(parent);
	assert(name);

	if (!name)
		return rd;

	/*
	 * Walk parent->rd_children directly.  The child list lives on the
	 * stable dirent (not the evictable inode), so no inode fault-in is
	 * needed here.  dirent_load_child_by_name handles the disk-miss path.
	 */
	rcu_read_lock();
	cds_list_for_each_entry_rcu(tmp, &parent->rd_children, rd_siblings) {
		if (!cmp(tmp->rd_name, name)) {
			rd = dirent_get(tmp);
			break;
		}
	}
	rcu_read_unlock();

	return rd;
}

void dirent_children_release(struct reffs_dirent *parent,
			     enum reffs_life_action rla)
{
	struct reffs_dirent *rd;

	/*
	 * Walk parent->rd_children directly.  The child list lives on the
	 * stable dirent, so no inode fault-in is needed and no concern about
	 * the inode having been evicted.
	 *
	 * Recurse before release so that children are detached bottom-up,
	 * keeping rd_children empty by the time the parent's
	 * dirent_parent_release runs.
	 *
	 * Shutdown path: avoid disk writes; just detach in-memory state.
	 * Non-shutdown path: full accounting (nlink, disk sync).
	 */
	if (rla == reffs_life_action_shutdown) {
		rcu_read_lock();
		while (!cds_list_empty(&parent->rd_children)) {
			rd = cds_list_first_entry(&parent->rd_children,
						  struct reffs_dirent,
						  rd_siblings);
			dirent_get(rd);
			dirent_children_release(rd, rla);
			dirent_parent_release(rd, rla);
			dirent_put(rd);
		}
		rcu_read_unlock();
		return;
	}

	rcu_read_lock();
	while (!cds_list_empty(&parent->rd_children)) {
		rd = cds_list_first_entry(&parent->rd_children,
					  struct reffs_dirent, rd_siblings);
		dirent_get(rd);
		dirent_parent_release(rd, rla);
		dirent_children_release(rd, rla);
		dirent_put(rd);
	}
	rcu_read_unlock();
}

/* ------------------------------------------------------------------ */
/* Load a single named child from disk on LOOKUP miss                  */
/* ------------------------------------------------------------------ */

/*
 * dirent_load_child_by_name -- LOOKUP helper for partially-resident dirs.
 *
 * Fast path: dirent_find() returns an in-memory hit.
 * Slow path: call sb_ops->dir_find_entry_by_name to read the .dir file,
 *   then allocate and attach a single dirent so future lookups hit cache.
 *
 * The returned dirent has one ref held (via dirent_get inside dirent_alloc
 * or dirent_find); caller must dirent_put() it.
 */
struct reffs_dirent *dirent_load_child_by_name(struct reffs_dirent *parent_de,
					       const char *name)
{
	struct reffs_dirent *rd;
	struct inode *parent_inode;
	struct super_block *sb;
	uint64_t child_ino, cookie;
	int ret;

	if (!parent_de || !name)
		return NULL;

	/* Fast path: already in memory. */
	rd = dirent_find(parent_de, reffs_text_case_sensitive, (char *)name);
	if (rd)
		return rd;

	/*
	 * Slow path: need to hit the disk.
	 * We need the parent inode and its superblock.
	 */
	parent_inode = dirent_ensure_inode(parent_de);
	if (!parent_inode)
		return NULL;

	sb = parent_inode->i_sb;
	if (!sb || !sb->sb_ops || !sb->sb_ops->dir_find_entry_by_name) {
		inode_active_put(parent_inode);
		return NULL;
	}

	ret = sb->sb_ops->dir_find_entry_by_name(sb, parent_inode->i_ino, name,
						 &child_ino, &cookie);
	inode_active_put(parent_inode);

	if (ret == -ENOENT)
		return NULL; /* entry genuinely does not exist */
	if (ret != 0) {
		LOG("dir_find_entry_by_name(%s) failed: %d", name, ret);
		return NULL;
	}

	/*
	 * Check again under the in-memory list in case a concurrent thread
	 * loaded it while we were reading disk.
	 */
	rd = dirent_find(parent_de, reffs_text_case_sensitive, (char *)name);
	if (rd)
		return rd;

	/*
	 * Allocate the dirent.  dirent_alloc calls dirent_parent_attach
	 * with reffs_life_action_load which:
	 *   - links into parent->rd_children
	 *   - does NOT bump nlink (already persisted on disk)
	 *   - does NOT write to disk
	 *   - does NOT bump rd_cookie_next
	 */
	rd = dirent_alloc(parent_de, (char *)name, reffs_life_action_load,
			  false /* is_dir unknown here; set below */);
	if (!rd)
		return NULL;

	rd->rd_ino = child_ino;
	rd->rd_cookie = cookie;

	/*
	 * rd_inode starts NULL (weak pointer).  dirent_ensure_inode() will
	 * populate it on first access.  We do a speculative inode_find()
	 * here so that if the inode happens to be cached we wire it up now.
	 */
	rd->rd_inode = inode_find(sb, child_ino);
	if (rd->rd_inode) {
		/* inode_find returned with active ref; we only want weak ref */
		inode_active_put(rd->rd_inode);
	}

	return rd;
}

/* ------------------------------------------------------------------ */
/* Disk sync                                                            */
/* ------------------------------------------------------------------ */

void dirent_sync_to_disk(struct reffs_dirent *parent)
{
	if (!parent || !parent->rd_inode)
		return;

	struct inode *inode = parent->rd_inode;
	struct super_block *sb = inode->i_sb;

	if (sb && sb->sb_ops && sb->sb_ops->dir_sync) {
		sb->sb_ops->dir_sync(inode);
	}
}
