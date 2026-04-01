/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <xxhash.h>

#include "reffs/rcu.h"
#include "reffs/backend.h"
#include "reffs/cmp.h"
#include "reffs/data_block.h"
#include "reffs/dirent.h"
#include "reffs/inode.h"
#include "reffs/layout_segment.h"
#include "reffs/log.h"
#include "reffs/super_block.h"
#include "reffs/evictor.h"
#include "reffs/trace/fs.h"
#include "reffs/test.h"
#include "reffs/stateid.h"

struct rcu_head;

/* ------------------------------------------------------------------ */
/* Hash table helpers                                                   */
/* ------------------------------------------------------------------ */

static int inode_match(struct cds_lfht_node *ht_node, const void *vkey)
{
	struct inode *inode = caa_container_of(ht_node, struct inode, i_node);
	const uint64_t *key = vkey;

	return *key == inode->i_ino;
}

/*
 * Weak stub: overridden by the real chunk_store_destroy() in
 * lib/nfs4/server/chunk_store.c when libreffs_nfs4_server is linked.
 */
__attribute__((weak)) void chunk_store_destroy(struct chunk_store *cs
					       __attribute__((unused)))
{
}

static void inode_free_rcu(struct rcu_head *rcu)
{
	struct inode *inode = caa_container_of(rcu, struct inode, i_rcu);

	/*
	 * NULL i_sb before calling trace so that trace_fs_inode cannot
	 * dereference a superblock that may be freed concurrently.
	 * The ref taken in inode_alloc (stored in i_sb) keeps the sb memory
	 * alive until super_block_put() below.
	 */
	struct super_block *sb = inode->i_sb;
	inode->i_sb = NULL;

	trace_fs_inode(inode, __func__, __LINE__);

	if (inode->i_db)
		data_block_put(inode->i_db);

	pthread_rwlock_destroy(&inode->i_db_rwlock);
	pthread_mutex_destroy(&inode->i_attr_mutex);
	pthread_mutex_destroy(&inode->i_lock_mutex);

	free(inode->i_symlink);
	layout_segments_free(inode->i_layout_segments);
	chunk_store_destroy(inode->i_chunk_store);
	free(inode);

	/* Drop the ref taken in inode_alloc (stored in i_sb); may free the superblock. */
	super_block_put(sb);
}

bool inode_unhash(struct inode *inode)
{
	int ret;
	bool b;
	uint64_t state;

	state = __atomic_fetch_and(&inode->i_state, ~INODE_IS_HASHED,
				   __ATOMIC_ACQUIRE);
	b = state & INODE_IS_HASHED;
	if (b) {
		ret = cds_lfht_del(inode->i_sb->sb_inodes, &inode->i_node);
		if (ret)
			LOG("ret = %d", ret);
		assert(!ret);
		return true;
	}

	return false;
}

/* ------------------------------------------------------------------ */
/* LRU helpers                                                          */
/* ------------------------------------------------------------------ */

/*
 * Add inode to the tail of the sb inode LRU.
 * Called when i_active drops to zero (inode_active_put).
 * Must NOT be called while the sb_inode_lru_lock is held by the caller.
 */
static void inode_lru_add(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	if (!sb)
		return;

	/*
	 * Release the data block FD when the inode goes idle.
	 * The FD will be re-opened on demand if the inode is
	 * reactivated.  Without this, every cached inode holds
	 * an open FD until eviction — with a 64K LRU limit, that
	 * means up to 64K open FDs for idle inodes.
	 *
	 * Safety: i_active == 0 guarantees no NFS operation is
	 * in-flight on this inode, so no thread holds i_db_rwlock
	 * or has a live pread/pwrite on the FD.  The reopen path
	 * in posix_data_db_reopen() uses pd_reopen_mutex to
	 * serialize concurrent reactivations.
	 */
	if (inode->i_db && sb->sb_ops && sb->sb_ops->db_release_resources)
		sb->sb_ops->db_release_resources(inode->i_db);

	pthread_mutex_lock(&sb->sb_inode_lru_lock);
	if (!(__atomic_fetch_or(&inode->i_state, INODE_IS_ON_LRU,
				__ATOMIC_ACQ_REL) &
	      INODE_IS_ON_LRU)) {
		cds_list_add_tail(&inode->i_lru, &sb->sb_inode_lru);
		sb->sb_inode_lru_count++;
	}
	pthread_mutex_unlock(&sb->sb_inode_lru_lock);

	/* Pressure-evict if over the limit */
	if (sb->sb_inode_lru_count > sb->sb_inode_lru_max) {
		if (evictor_get_mode() == EVICTOR_ASYNC) {
			evictor_signal();
			/*
			 * Backpressure: if way over the limit, fall back
			 * to synchronous eviction on this worker thread.
			 */
			if (sb->sb_inode_lru_count > 2 * sb->sb_inode_lru_max)
				super_block_evict_inodes(
					sb, sb->sb_inode_lru_count -
						    sb->sb_inode_lru_max);
		} else {
			super_block_evict_inodes(sb,
						 sb->sb_inode_lru_count -
							 sb->sb_inode_lru_max);
		}
	}
}

/*
 * Remove inode from the LRU (e.g. because it was re-activated).
 * Caller must hold sb_inode_lru_lock.
 */
static void inode_lru_del_locked(struct inode *inode)
{
	uint64_t old = __atomic_fetch_and(&inode->i_state, ~INODE_IS_ON_LRU,
					  __ATOMIC_ACQ_REL);
	if (old & INODE_IS_ON_LRU) {
		cds_list_del_init(&inode->i_lru);
		inode->i_sb->sb_inode_lru_count--;
	}
}

/* ------------------------------------------------------------------ */
/* Memory-lifetime ref                                                  */
/* ------------------------------------------------------------------ */

static void inode_release(struct urcu_ref *ref)
{
	struct inode *inode = caa_container_of(ref, struct inode, i_ref);

	trace_fs_inode(inode, __func__, __LINE__);

	inode_unhash(inode);
	if (inode->i_sb) {
		atomic_fetch_sub_explicit(&inode->i_sb->sb_inodes_used, 1,
					  memory_order_relaxed);

		if (atomic_load_explicit(&inode->i_nlink,
					 memory_order_relaxed) == 0) {
			size_t size = inode->i_size;
			size_t old_used;
			size_t new_used;

			old_used = atomic_load_explicit(
				&inode->i_sb->sb_bytes_used,
				memory_order_relaxed);
			do {
				if (old_used >= size)
					new_used = old_used - size;
				else
					new_used = 0;
			} while (!atomic_compare_exchange_strong_explicit(
				&inode->i_sb->sb_bytes_used, &old_used,
				new_used, memory_order_seq_cst,
				memory_order_relaxed));

			if (inode->i_sb->sb_ops &&
			    inode->i_sb->sb_ops->inode_free) {
				inode->i_sb->sb_ops->inode_free(inode);
			}
		}
	}

	int ret = cds_lfht_destroy(inode->i_stateids, NULL);
	if (ret < 0) {
		LOG("Could not delete a hash table: %m");
	}

	/*
	 * Null rd_inode on our dirent BEFORE scheduling the RCU free.
	 *
	 * This is the key invariant that closes the UAF in dirent_ensure_inode:
	 * after this rcu_assign_pointer any reader that loads rd_inode under
	 * rcu_read_lock will see either NULL (we won the race) or a valid
	 * pointer (they loaded before this store and are protected by their
	 * own grace period).  Either way no reader can load a freed pointer.
	 *
	 * i_dirent is set by dirent_parent_attach and cleared by
	 * dirent_parent_release / dirent_release, so it is NULL if the dirent
	 * was already detached before we get here.
	 */
	if (inode->i_dirent)
		rcu_assign_pointer(inode->i_dirent->rd_inode, NULL);

	/*
	 * i_sb already holds the ref taken in inode_alloc; that ref keeps the
	 * superblock alive until inode_free_rcu nulls i_sb and calls
	 * super_block_put().  No additional get is needed here.
	 */
	call_rcu(&inode->i_rcu, inode_free_rcu);
}

struct inode *inode_get(struct inode *inode)
{
	if (!inode)
		return NULL;

	TRACE("inode = %p", (void *)inode);

	trace_fs_inode(inode, __func__, __LINE__);

	if (!urcu_ref_get_unless_zero(&inode->i_ref))
		return NULL;

	return inode;
}

void inode_put(struct inode *inode)
{
	if (!inode)
		return;

	trace_fs_inode(inode, __func__, __LINE__);

	urcu_ref_put(&inode->i_ref, inode_release);
}

/* ------------------------------------------------------------------ */
/* Active-use ref                                                       */
/* ------------------------------------------------------------------ */

struct inode *inode_active_get(struct inode *inode)
{
	if (!inode)
		return NULL;

	/* Bump memory-lifetime ref first; bail if already being freed. */
	if (!inode_get(inode))
		return NULL;

	int64_t prev =
		__atomic_fetch_add(&inode->i_active, 1, __ATOMIC_ACQ_REL);

	/*
	 * If we raced with eviction that already dropped i_active to -1
	 * (tombstone), back out.
	 */
	if (prev < 0) {
		__atomic_fetch_sub(&inode->i_active, 1, __ATOMIC_RELAXED);
		inode_put(inode);
		return NULL;
	}

	/* Pull it off the LRU if it was sitting there. */
	if (inode->i_sb) {
		pthread_mutex_lock(&inode->i_sb->sb_inode_lru_lock);
		inode_lru_del_locked(inode);
		pthread_mutex_unlock(&inode->i_sb->sb_inode_lru_lock);
	}

	return inode;
}

struct inode *inode_active_get_rcu(struct inode *inode)
{
	if (!inode)
		return NULL;

	if (!inode_get(inode))
		return NULL;

	int64_t prev =
		__atomic_fetch_add(&inode->i_active, 1, __ATOMIC_ACQ_REL);
	if (prev < 0) {
		__atomic_fetch_sub(&inode->i_active, 1, __ATOMIC_RELAXED);
		inode_put(inode);
		return NULL;
	}

	return inode;
}

void inode_active_lru_pull(struct inode *inode)
{
	if (inode && inode->i_sb) {
		pthread_mutex_lock(&inode->i_sb->sb_inode_lru_lock);
		inode_lru_del_locked(inode);
		pthread_mutex_unlock(&inode->i_sb->sb_inode_lru_lock);
	}
}

void inode_active_put(struct inode *inode)
{
	if (!inode)
		return;

	int64_t remaining =
		__atomic_sub_fetch(&inode->i_active, 1, __ATOMIC_ACQ_REL);
	if (remaining == 0)
		inode_lru_add(inode);

	inode_put(inode);
}

/* ------------------------------------------------------------------ */
/* Alloc / find                                                          */
/* ------------------------------------------------------------------ */

struct inode *inode_alloc(struct super_block *sb, uint64_t ino)
{
	struct inode *inode;
	struct inode *tmp;
	struct cds_lfht_node *node;
	unsigned long hash = XXH3_64bits(&ino, sizeof(ino));

	/* If it already exists, hand back an active ref. */
	inode = inode_find(sb, ino);
	if (inode)
		return inode;

	inode = calloc(1, sizeof(*inode));
	if (!inode) {
		LOG("Could not alloc a inode");
		return NULL;
	}

	cds_lfht_node_init(&inode->i_node);
	urcu_ref_init(&inode->i_ref);
	CDS_INIT_LIST_HEAD(&inode->i_lru);

	pthread_rwlock_init(&inode->i_db_rwlock, NULL);
	pthread_mutex_init(&inode->i_attr_mutex, NULL);
	pthread_mutex_init(&inode->i_lock_mutex, NULL);

	CDS_INIT_LIST_HEAD(&inode->i_locks);
	CDS_INIT_LIST_HEAD(&inode->i_shares);

	/* Start with one active ref for the caller. */
	inode->i_active = 1;

	/* Start at 1 for stateids */
	inode->i_stateid_next = 1;

	inode->i_stateids = cds_lfht_new(
		8, 8, 0, CDS_LFHT_AUTO_RESIZE | CDS_LFHT_ACCOUNTING, NULL);
	if (!inode->i_stateids) {
		LOG("Could not create a new hash table");
		free(inode);
		return NULL;
	}

	if (sb) {
		inode->i_sb = super_block_get(sb);
		assert(inode->i_sb);

		if (inode->i_sb) {
			atomic_fetch_add_explicit(&inode->i_sb->sb_inodes_used,
						  1, memory_order_relaxed);

			/* Make sure no one else beat us to it */
			rcu_read_lock();
			node = cds_lfht_add_unique(inode->i_sb->sb_inodes, hash,
						   inode_match, &ino,
						   &inode->i_node);
			if (node != &inode->i_node) {
				/*
				 * Lost the race -- grab the winner with an
				 * active ref and discard our newly-allocated
				 * inode.  We must drop i_active first so that
				 * inode_put doesn't try to LRU-add it.
				 */
				tmp = caa_container_of(node, struct inode,
						       i_node);
				__atomic_fetch_sub(&inode->i_active, 1,
						   __ATOMIC_RELAXED);
				inode_put(inode);
				inode = inode_active_get(tmp);
			} else {
				__atomic_fetch_or(&inode->i_state,
						  INODE_IS_HASHED,
						  __ATOMIC_ACQUIRE);
				/*
				 * urcu_ref_init sets i_ref to 1 for the hash
				 * table.  The caller also holds an active ref
				 * which pairs with an inode_put in
				 * inode_active_put.  Bump i_ref now so the
				 * two refs are accounted for independently.
				 */
				inode_get(inode);
			}
			rcu_read_unlock();
		}
	}

	if (!inode)
		return NULL;

	/*
	 * Only set i_ino and i_nlink on freshly-allocated inodes.
	 * If we lost the hash race and picked up the winner, it
	 * already has correct values — overwriting i_nlink would
	 * corrupt directories (nlink=2) and hardlinked files.
	 */
	if (inode->i_ino == 0) {
		inode->i_ino = ino;
		inode->i_nlink = 1;
	}

	if (inode->i_sb && inode->i_sb->sb_ops &&
	    inode->i_sb->sb_ops->inode_alloc) {
		int ret = inode->i_sb->sb_ops->inode_alloc(inode);
		if (ret != 0) {
			inode_active_put(inode);
			return NULL;
		}
	}

	/*
	 * Seed i_changeid from ctime if it's still zero (new inode or
	 * loaded from old disk format without id_changeid).  Production
	 * NFS servers return nanosecond-scale changeids; starting from
	 * zero causes the Linux NFS client to mishandle dcache
	 * invalidation via change_info.
	 */
	if (atomic_load_explicit(&inode->i_changeid, memory_order_relaxed) ==
	    0) {
		struct timespec now;
		clock_gettime(CLOCK_REALTIME, &now);
		uint64_t seed = (uint64_t)now.tv_sec * 1000000000ULL +
				(uint64_t)now.tv_nsec;
		if (seed == 0)
			seed = 1;
		atomic_store_explicit(&inode->i_changeid, seed,
				      memory_order_relaxed);
	}

	trace_fs_inode(inode, __func__, __LINE__);

	return inode;
}

struct inode *inode_find(struct super_block *sb, uint64_t ino)
{
	struct inode *inode = NULL;
	struct inode *tmp;
	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;
	unsigned long hash = XXH3_64bits(&ino, sizeof(ino));

	if (!sb)
		return NULL;

	rcu_read_lock();
	cds_lfht_lookup(sb->sb_inodes, hash, inode_match, &ino, &iter);
	node = cds_lfht_iter_get_node(&iter);
	if (node) {
		tmp = caa_container_of(node, struct inode, i_node);
		inode = inode_active_get(tmp);
	}
	rcu_read_unlock();

	return inode;
}

/* ------------------------------------------------------------------ */
/* Attribute helpers                                                    */
/* ------------------------------------------------------------------ */

void inode_update_times_now(struct inode *inode, uint64_t flags)
{
	struct timespec now;

	clock_gettime(CLOCK_REALTIME, &now);

	if (flags & REFFS_INODE_UPDATE_ATIME)
		inode->i_atime = now;

	if (flags & REFFS_INODE_UPDATE_CTIME)
		inode->i_ctime = now;

	if (flags & REFFS_INODE_UPDATE_MTIME)
		inode->i_mtime = now;

	/* Bump the monotonic change counter so NFSv4 clients see a
	 * change even if ctime doesn't advance (same nanosecond). */
	atomic_fetch_add_explicit(&inode->i_changeid, 1, memory_order_relaxed);

	inode_sync_to_disk(inode);
}

void inode_sync_to_disk(struct inode *inode)
{
	trace_fs_inode(inode, __func__, __LINE__);
	if (inode->i_sb && inode->i_sb->sb_ops &&
	    inode->i_sb->sb_ops->inode_sync) {
		inode->i_sb->sb_ops->inode_sync(inode);
	}
}

/* ------------------------------------------------------------------ */
/* Name lookup helpers                                                  */
/* ------------------------------------------------------------------ */

bool inode_name_is_child(struct inode *inode, char *name)
{
	bool exists = false;
	struct reffs_dirent *rd;

	if (!inode || !inode->i_dirent)
		return false;

	reffs_strng_compare cmp = reffs_text_case_cmp();

	rcu_read_lock();
	cds_list_for_each_entry_rcu(rd, &inode->i_dirent->rd_children,
				    rd_siblings) {
		if (!cmp(rd->rd_name, name)) {
			exists = true;
			break;
		}
	}
	rcu_read_unlock();

	return exists;
}

struct inode *inode_name_get_inode(struct inode *inode, char *name)
{
	struct inode *exists = NULL;
	struct reffs_dirent *rd;

	if (!strcmp(name, ".")) {
		return inode_active_get(inode);
	}

	if (!strcmp(name, "..")) {
		if (inode->i_dirent && inode->i_dirent->rd_parent)
			return inode_active_get(
				inode->i_dirent->rd_parent->rd_inode);
		return inode_active_get(inode); /* root case */
	}

	if (!inode->i_dirent)
		return NULL;

	reffs_strng_compare cmp = reffs_text_case_cmp();

	rcu_read_lock();
	cds_list_for_each_entry_rcu(rd, &inode->i_dirent->rd_children,
				    rd_siblings) {
		if (!cmp(rd->rd_name, name)) {
			/*
			 * rd_inode is weak -- use dirent_ensure_inode() to
			 * handle the case where the inode was evicted.
			 */
			exists = dirent_ensure_inode(rd);
			break;
		}
	}
	rcu_read_unlock();

	return exists;
}

/* ------------------------------------------------------------------ */
/* Parent dirent reconstruction                                         */
/* ------------------------------------------------------------------ */

/*
 * Ensure the parent dirent of this inode is available and return it ref-held.
 * The common fast path: inode->i_dirent->rd_parent is already set.
 * Miss path: load the parent inode, find its dirent, then search rd_children
 * for the child entry that points back at us.
 *
 * Returns a ref-held dirent; caller must dirent_put() it.
 * Returns NULL only on OOM / corrupt fs.
 */
struct reffs_dirent *inode_ensure_parent_dirent(struct inode *inode)
{
	struct reffs_dirent *rd;
	struct inode *parent_inode;

	if (!inode)
		return NULL;

	/* Fast path: i_dirent is set and its rd_parent is populated. */
	rcu_read_lock();
	rd = inode->i_dirent ? inode->i_dirent->rd_parent : NULL;
	if (rd)
		rd = dirent_get(rd);
	rcu_read_unlock();
	if (rd)
		return rd;

	if (!inode->i_parent_ino || !inode->i_sb)
		return NULL;

	/* Root inode is its own parent conceptually; return sb_dirent. */
	if (inode->i_ino == inode->i_parent_ino)
		return dirent_get(inode->i_sb->sb_dirent);

	/* Load the parent inode (may be a cache hit or a disk load). */
	parent_inode = inode_alloc(inode->i_sb, inode->i_parent_ino);
	if (!parent_inode)
		return NULL;

	/*
	 * Walk the parent dirent's rd_children looking for a dirent whose
	 * rd_ino matches ours.  The parent dirent must be reachable via the
	 * parent inode's i_dirent.
	 */
	rcu_read_lock();
	rd = NULL;
	if (parent_inode->i_dirent) {
		struct reffs_dirent *child;
		cds_list_for_each_entry_rcu(
			child, &parent_inode->i_dirent->rd_children,
			rd_siblings) {
			if (child->rd_ino == inode->i_ino) {
				rd = dirent_get(child);
				break;
			}
		}
	}
	rcu_read_unlock();

	inode_active_put(parent_inode);
	return rd;
}

/* ------------------------------------------------------------------ */
/* Path reconstruction after PUTFH (stale-handle recovery)             */
/* ------------------------------------------------------------------ */

/*
 * One link in the upward walk: everything we need to reconstruct a single
 * dirent once we replay the chain downward.
 */
struct path_link {
	uint64_t pl_parent_ino;
	uint64_t pl_ino;
	uint64_t pl_cookie;
	char pl_name[REFFS_MAX_NAME + 1];
};

/*
 * inode_already_anchored -- return true if this inode already has its dirent
 * in memory (i_dirent->rd_parent set, or it is the root).
 */
static bool inode_already_anchored(struct inode *inode)
{
	if (!inode)
		return false;
	if (inode->i_ino == inode->i_parent_ino)
		return true; /* root */
	rcu_read_lock();
	bool anchored = (inode->i_dirent && inode->i_dirent->rd_parent != NULL);
	rcu_read_unlock();
	return anchored;
}

/*
 * inode_reconstruct_path_to_root
 *
 * Walk upward from 'inode' via i_parent_ino, calling the backend
 * dir_find_entry_by_ino op at each step to recover the child's name and
 * cookie.  Stop when we reach an already-resident ancestor or the root.
 * Then replay downward, creating and attaching dirents with
 * reffs_life_action_load.
 *
 * Stack is heap-allocated to avoid blowing the call stack on deep trees.
 *
 * Returns 0 on success, ENOENT if any .dir file is missing a required entry
 * (NFS4ERR_STALE / NFS3ERR_STALE), ENOMEM on allocation failure, or another
 * errno on I/O error.
 */
int inode_reconstruct_path_to_root(struct inode *inode)
{
	if (!inode || !inode->i_sb)
		return -EINVAL;

	struct super_block *sb = inode->i_sb;

	/* Nothing to do if already anchored. */
	if (inode_already_anchored(inode))
		return 0;

	if (!sb->sb_ops || !sb->sb_ops->dir_find_entry_by_ino)
		return -ENOSYS;

	/* ---- Phase 1: walk upward, accumulate the path_link stack ---- */

	size_t stack_cap = 32;
	size_t stack_size = 0;
	struct path_link *stack = malloc(stack_cap * sizeof(*stack));
	if (!stack)
		return -ENOMEM;

	/*
	 * cur is the inode whose *name* we need to find (i.e. its entry in
	 * its parent's .dir file).  cur_active holds the active ref.
	 */
	struct inode *cur = inode_active_get(inode);
	if (!cur) {
		free(stack);
		return -ENOENT;
	}

	int err = 0;

	while (!inode_already_anchored(cur)) {
		if (cur->i_parent_ino == 0) {
			/* No parent recorded -- filesystem corrupt / new inode */
			LOG("ino %lu has no i_parent_ino, cannot reconstruct path",
			    cur->i_ino);
			err = -ENOENT;
			break;
		}

		/* Load parent inode (cache hit or disk). */
		struct inode *parent = inode_alloc(sb, cur->i_parent_ino);
		if (!parent) {
			err = -ENOENT;
			break;
		}

		/* Grow stack if needed. */
		if (stack_size == stack_cap) {
			size_t new_cap = stack_cap * 2;
			struct path_link *tmp =
				realloc(stack, new_cap * sizeof(*stack));
			if (!tmp) {
				inode_active_put(parent);
				err = -ENOMEM;
				break;
			}
			stack = tmp;
			stack_cap = new_cap;
		}

		struct path_link *link = &stack[stack_size];
		link->pl_parent_ino = cur->i_parent_ino;
		link->pl_ino = cur->i_ino;
		link->pl_cookie = 0;

		err = sb->sb_ops->dir_find_entry_by_ino(
			sb, cur->i_parent_ino, cur->i_ino, link->pl_name,
			sizeof(link->pl_name), &link->pl_cookie);

		if (err) {
			LOG("dir_find_entry_by_ino: parent=%lu child=%lu: %d",
			    cur->i_parent_ino, cur->i_ino, err);
			inode_active_put(parent);
			break;
		}

		stack_size++;

		struct inode *next = parent;
		inode_active_put(cur);
		cur = next;

		/*
		 * If cur is the root (self-parent), push one final link for
		 * the root itself then stop.
		 */
		if (cur->i_ino == cur->i_parent_ino)
			break;
	}

	inode_active_put(cur);

	if (err) {
		free(stack);
		return err;
	}

	/* ---- Phase 2: replay downward, attaching dirents ---- */

	/*
	 * stack[stack_size-1] is the link closest to the root;
	 * stack[0] is the link closest to 'inode'.
	 * Walk from the top of the stack downward.
	 */
	for (ssize_t i = (ssize_t)stack_size - 1; i >= 0; i--) {
		struct path_link *link = &stack[i];

		/*
		 * Get the parent dirent.  By the time we reach link[i] the
		 * parent's dirent must already be in memory (either it was
		 * already resident, or we just attached it in a prior
		 * iteration).
		 */
		struct inode *parent_inode =
			inode_find(sb, link->pl_parent_ino);
		if (!parent_inode) {
			/*
			 * Should not happen -- we loaded it during the upward
			 * walk and it should still be active.
			 */
			LOG("parent ino %lu vanished during replay",
			    link->pl_parent_ino);
			err = -ENOENT;
			break;
		}

		/*
		 * Find or create the parent's dirent.  For the root inode,
		 * sb_dirent is already the canonical dirent.
		 */
		struct reffs_dirent *parent_de;
		if (parent_inode->i_ino == sb->sb_dirent->rd_inode->i_ino) {
			parent_de = dirent_get(sb->sb_dirent);
		} else {
			parent_de = inode_ensure_parent_dirent(parent_inode);
		}

		inode_active_put(parent_inode);

		if (!parent_de) {
			LOG("could not get parent_de for ino %lu",
			    link->pl_parent_ino);
			err = -ENOENT;
			break;
		}

		/*
		 * Check whether the child dirent is already in memory (could
		 * have been attached by a concurrent thread).
		 */
		struct reffs_dirent *child_de = dirent_find(
			parent_de, reffs_text_case_sensitive, link->pl_name);

		if (!child_de) {
			/*
			 * Need to load the child inode so we know its mode
			 * (required for is_dir in dirent_parent_attach).
			 */
			struct inode *child_inode =
				inode_find(sb, link->pl_ino);
			if (!child_inode)
				child_inode = inode_alloc(sb, link->pl_ino);

			bool is_dir = child_inode &&
				      S_ISDIR(child_inode->i_mode);

			child_de = dirent_alloc(parent_de, link->pl_name,
						reffs_life_action_load, is_dir);
			if (!child_de) {
				if (child_inode)
					inode_active_put(child_inode);
				dirent_put(parent_de);
				err = -ENOMEM;
				break;
			}

			child_de->rd_ino = link->pl_ino;
			child_de->rd_cookie = link->pl_cookie;

			/* Wire up the weak inode pointer via dirent_attach_inode
			 * so i_dirent is set and inode_release can null rd_inode.
			 */
			if (child_inode) {
				dirent_attach_inode(child_de, child_inode);
				inode_active_put(child_inode);
			}
		}

		dirent_put(parent_de);
		dirent_put(child_de);
	}

	free(stack);
	return err;
}

/* ------------------------------------------------------------------ */
/* Delayed / LRU eviction                                              */
/* ------------------------------------------------------------------ */

struct inode_delayed_release {
	struct inode *idr_inode;
	time_t idr_release_time;
	struct cds_list_head idr_list;
};

static CDS_LIST_HEAD(delayed_release_list);
static pthread_mutex_t delayed_release_lock = PTHREAD_MUTEX_INITIALIZER;

static pthread_t reaper_thread;
static bool reaper_running = false;
static pthread_mutex_t reaper_lock = PTHREAD_MUTEX_INITIALIZER;

static void *reaper_thread_func(void *__attribute__((unused)) arg)
{
	while (1) {
		struct timespec _ts;

		clock_gettime(CLOCK_REALTIME, &_ts);
		time_t now = _ts.tv_sec;
		struct inode_delayed_release *idr, *tmp;
		bool list_empty = true;

		pthread_mutex_lock(&delayed_release_lock);
		cds_list_for_each_entry_safe(idr, tmp, &delayed_release_list,
					     idr_list) {
			if (idr->idr_release_time <= now) {
				struct inode *inode = idr->idr_inode;

				if (inode->i_sb)
					__atomic_sub_fetch(
						&inode->i_sb->sb_delayed_count,
						1, __ATOMIC_RELAXED);

				cds_list_del(&idr->idr_list);

				/*
				 * Only evict if there are no active users.
				 * inode_active_put() will LRU-queue it if
				 * the count was already zero.
				 */
				int64_t active = __atomic_load_n(
					&inode->i_active, __ATOMIC_ACQUIRE);
				if (active <= 0)
					inode_put(inode);
				else
					inode_active_put(inode);

				free(idr);
			} else {
				list_empty = false;
			}
		}

		if (list_empty && cds_list_empty(&delayed_release_list)) {
			reaper_running = false;
			pthread_mutex_unlock(&delayed_release_lock);
			break;
		}

		pthread_mutex_unlock(&delayed_release_lock);
		sleep(1);
	}

	return NULL;
}

static void ensure_reaper_thread(void)
{
	pthread_mutex_lock(&reaper_lock);
	if (!reaper_running) {
		reaper_running = true;
		pthread_create(&reaper_thread, NULL, reaper_thread_func, NULL);
		pthread_detach(reaper_thread);
	}
	pthread_mutex_unlock(&reaper_lock);
}

/*
 * Schedule a deferred active_put for this inode.
 * The caller has already called inode_active_get() (or equivalent) to
 * ensure the inode stays alive until the reaper fires.
 */
void inode_schedule_delayed_release(struct inode *inode, int delay_seconds)
{
	struct inode_delayed_release *idr =
		malloc(sizeof(struct inode_delayed_release));
	if (!idr) {
		/* Best-effort: just drop the active ref now. */
		inode_active_put(inode);
		return;
	}

	idr->idr_inode = inode; /* takes ownership of caller's active ref */
	struct timespec _ts2;

	clock_gettime(CLOCK_REALTIME, &_ts2);
	idr->idr_release_time = _ts2.tv_sec + delay_seconds;

	if (idr->idr_inode->i_sb)
		__atomic_add_fetch(&idr->idr_inode->i_sb->sb_delayed_count, 1,
				   __ATOMIC_RELAXED);

	pthread_mutex_lock(&delayed_release_lock);
	cds_list_add(&idr->idr_list, &delayed_release_list);
	pthread_mutex_unlock(&delayed_release_lock);

	ensure_reaper_thread();
}

void inode_remove_all_stateids(struct inode *inode)
{
	struct cds_lfht_iter iter;
	struct stateid *stid;

	if (!inode || !inode->i_stateids)
		return;

	rcu_read_lock();
	cds_lfht_first(inode->i_stateids, &iter);
	while (cds_lfht_iter_get_node(&iter) != NULL) {
		stid = caa_container_of(cds_lfht_iter_get_node(&iter),
					struct stateid, s_inode_node);
		cds_lfht_next(inode->i_stateids, &iter);
		if (stateid_inode_unhash(stid))
			stateid_put(stid);
	}
	rcu_read_unlock();
}
