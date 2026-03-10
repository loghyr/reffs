/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
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
#include <time.h>
#include <unistd.h>
#include <xxhash.h>
#include "reffs/rcu.h"
#include "reffs/backend.h"
#include "reffs/cmp.h"
#include "reffs/data_block.h"
#include "reffs/dirent.h"
#include "reffs/inode.h"
#include "reffs/log.h"
#include "reffs/super_block.h"
#include "reffs/trace/fs.h"
#include "reffs/test.h"

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

static void inode_free_rcu(struct rcu_head *rcu)
{
	struct inode *inode = caa_container_of(rcu, struct inode, i_rcu);

	trace_fs_inode(inode, __func__, __LINE__);

	if (inode->i_db)
		data_block_put(inode->i_db);

	pthread_rwlock_destroy(&inode->i_db_rwlock);
	pthread_mutex_destroy(&inode->i_attr_mutex);
	pthread_mutex_destroy(&inode->i_lock_mutex);

	free(inode->i_symlink);
	free(inode);
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

	pthread_mutex_lock(&sb->sb_inode_lru_lock);
	if (!(__atomic_fetch_or(&inode->i_state, INODE_IS_ON_LRU,
				__ATOMIC_ACQ_REL) &
	      INODE_IS_ON_LRU)) {
		cds_list_add_tail(&inode->i_lru, &sb->sb_inode_lru);
		sb->sb_inode_lru_count++;
	}
	pthread_mutex_unlock(&sb->sb_inode_lru_lock);

	/* Pressure-evict if over the limit */
	if (sb->sb_inode_lru_count > sb->sb_inode_lru_max)
		super_block_evict_inodes(sb, sb->sb_inode_lru_count -
						     sb->sb_inode_lru_max);
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
		__atomic_fetch_sub(&inode->i_sb->sb_inodes_used, 1,
				   __ATOMIC_RELAXED);

		if (inode->i_nlink == 0) {
			size_t size = inode->i_size;
			size_t old_used;
			size_t new_used;

			do {
				__atomic_load(&inode->i_sb->sb_bytes_used,
					      &old_used, __ATOMIC_RELAXED);
				if (old_used >= size)
					new_used = old_used - size;
				else
					new_used = 0;
			} while (!__atomic_compare_exchange(
				&inode->i_sb->sb_bytes_used, &old_used,
				&new_used, false, __ATOMIC_SEQ_CST,
				__ATOMIC_RELAXED));

			if (inode->i_sb->sb_ops &&
			    inode->i_sb->sb_ops->inode_free) {
				inode->i_sb->sb_ops->inode_free(inode);
			}
		}
	}
	super_block_put(inode->i_sb);
	inode->i_sb = NULL;

	call_rcu(&inode->i_rcu, inode_free_rcu);
}

struct inode *inode_get(struct inode *inode)
{
	if (!inode)
		return NULL;

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

	CDS_INIT_LIST_HEAD(&inode->i_children);
	CDS_INIT_LIST_HEAD(&inode->i_locks);
	CDS_INIT_LIST_HEAD(&inode->i_shares);

	/* Start with one active ref for the caller. */
	inode->i_active = 1;

	if (sb) {
		inode->i_sb = super_block_get(sb);
		assert(inode->i_sb);

		if (inode->i_sb) {
			__atomic_fetch_add(&inode->i_sb->sb_inodes_used, 1,
					   __ATOMIC_RELAXED);

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

	inode->i_ino = ino;
	inode->i_nlink = 1;

	if (inode->i_sb && inode->i_sb->sb_ops &&
	    inode->i_sb->sb_ops->inode_alloc) {
		int ret = inode->i_sb->sb_ops->inode_alloc(inode);
		if (ret != 0) {
			inode_active_put(inode);
			return NULL;
		}
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

	reffs_strng_compare cmp = reffs_text_case_cmp();

	rcu_read_lock();
	cds_list_for_each_entry_rcu(rd, &inode->i_children, rd_siblings) {
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
		if (inode->i_parent && inode->i_parent->rd_parent)
			return inode_active_get(
				inode->i_parent->rd_parent->rd_inode);
		return inode_active_get(inode); /* root case */
	}

	reffs_strng_compare cmp = reffs_text_case_cmp();

	rcu_read_lock();
	cds_list_for_each_entry_rcu(rd, &inode->i_children, rd_siblings) {
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
 * Ensure inode->i_parent is populated.  Walk up via i_parent_ino if needed.
 * Returns a ref-held dirent (dirent_put when done), or NULL on failure.
 *
 * The common case is a cache hit: i_parent != NULL.
 * On a miss we load the parent inode by ino number and then search its
 * children list for a dirent that points back at us.
 */
struct reffs_dirent *inode_ensure_parent_dirent(struct inode *inode)
{
	struct reffs_dirent *rd;
	struct inode *parent_inode;

	if (!inode)
		return NULL;

	/* Fast path: already have the parent dirent in memory. */
	rcu_read_lock();
	rd = inode->i_parent;
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
	 * Walk the parent's children list looking for a dirent whose rd_ino
	 * matches ours.
	 */
	rcu_read_lock();
	struct reffs_dirent *child;
	rd = NULL;
	cds_list_for_each_entry_rcu(child, &parent_inode->i_children,
				    rd_siblings) {
		if (child->rd_ino == inode->i_ino) {
			rd = dirent_get(child);
			break;
		}
	}
	rcu_read_unlock();

	if (rd && S_ISDIR(inode->i_mode))
		inode->i_parent = rd; /* weak, no extra ref */

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
 * in memory (i_parent set, or it is the root).
 */
static bool inode_already_anchored(struct inode *inode)
{
	if (!inode)
		return false;
	if (inode->i_ino == inode->i_parent_ino)
		return true; /* root */
	rcu_read_lock();
	bool anchored = (inode->i_parent != NULL);
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

			/* Wire up the weak inode pointer if we have it. */
			if (child_inode) {
				child_de->rd_inode = child_inode;
				if (is_dir)
					child_inode->i_parent = child_de;
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
		time_t now = time(NULL);
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
	idr->idr_release_time = time(NULL) + delay_seconds;

	if (idr->idr_inode->i_sb)
		__atomic_add_fetch(&idr->idr_inode->i_sb->sb_delayed_count, 1,
				   __ATOMIC_RELAXED);

	pthread_mutex_lock(&delayed_release_lock);
	cds_list_add(&idr->idr_list, &delayed_release_list);
	pthread_mutex_unlock(&delayed_release_lock);

	ensure_reaper_thread();
}
