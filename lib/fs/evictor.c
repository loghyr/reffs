/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * Background LRU evictor thread.
 *
 * Inode and dirent LRU eviction was previously synchronous on the
 * NFS worker thread, blocking the data path for 41ms+ during
 * inode_sync disk I/O.  This thread handles eviction in the
 * background so worker threads return immediately.
 *
 * Pattern: follows lease_reaper.c — single global thread, condvar
 * sleep, atomic running flag, clean shutdown via join.
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <time.h>

#include "reffs/log.h"
#include "reffs/rcu.h"
#include "reffs/super_block.h"
#include "reffs/evictor.h"

/* ------------------------------------------------------------------ */
/* Global state                                                        */
/* ------------------------------------------------------------------ */

static pthread_t evictor_thread;
static _Atomic uint32_t evictor_running;
static _Atomic uint32_t evictor_needed;
static pthread_mutex_t evictor_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t evictor_cv = PTHREAD_COND_INITIALIZER;

/* Drain synchronization — evictor_drain() waits on this. */
static _Atomic uint32_t evictor_drain_requested;
static pthread_cond_t evictor_drain_cv = PTHREAD_COND_INITIALIZER;

static _Atomic enum evictor_mode evictor_current_mode;

/* ------------------------------------------------------------------ */
/* Thread function                                                     */
/* ------------------------------------------------------------------ */

static void evict_all_sbs(void)
{
	struct cds_list_head *sb_list = super_block_list_head();
	struct super_block *sb;

	rcu_read_lock();
	cds_list_for_each_entry_rcu(sb, sb_list, sb_link) {
		if (!super_block_get(sb))
			continue;
		rcu_read_unlock();

		if (sb->sb_inode_lru_count > sb->sb_inode_lru_max)
			super_block_evict_inodes(sb,
						 sb->sb_inode_lru_count -
							 sb->sb_inode_lru_max);

		if (sb->sb_dirent_lru_count > sb->sb_dirent_lru_max)
			super_block_evict_dirents(
				sb, sb->sb_dirent_lru_count -
					    sb->sb_dirent_lru_max);

		super_block_put(sb);
		rcu_read_lock();
	}
	rcu_read_unlock();
}

static void *evictor_thread_fn(void *arg __attribute__((unused)))
{
	rcu_register_thread();

	while (atomic_load_explicit(&evictor_running, memory_order_relaxed)) {
		struct timespec ts;

		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_sec += 1; /* 1-second poll interval */

		pthread_mutex_lock(&evictor_mtx);
		while (!atomic_load_explicit(&evictor_needed,
					     memory_order_relaxed) &&
		       !atomic_load_explicit(&evictor_drain_requested,
					     memory_order_relaxed) &&
		       atomic_load_explicit(&evictor_running,
					    memory_order_relaxed)) {
			if (pthread_cond_timedwait(&evictor_cv, &evictor_mtx,
						   &ts) != 0)
				break; /* timeout — check again */
		}
		atomic_store_explicit(&evictor_needed, 0, memory_order_relaxed);
		pthread_mutex_unlock(&evictor_mtx);

		if (!atomic_load_explicit(&evictor_running,
					  memory_order_relaxed))
			break;

		evict_all_sbs();

		/* If drain was requested, signal completion. */
		if (atomic_load_explicit(&evictor_drain_requested,
					 memory_order_relaxed)) {
			atomic_store_explicit(&evictor_drain_requested, 0,
					      memory_order_release);
			pthread_mutex_lock(&evictor_mtx);
			pthread_cond_broadcast(&evictor_drain_cv);
			pthread_mutex_unlock(&evictor_mtx);
		}
	}

	rcu_unregister_thread();
	return NULL;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void evictor_signal(void)
{
	atomic_store_explicit(&evictor_needed, 1, memory_order_relaxed);
	pthread_cond_signal(&evictor_cv);
}

void evictor_drain(void)
{
	if (!atomic_load_explicit(&evictor_running, memory_order_relaxed))
		return;

	atomic_store_explicit(&evictor_drain_requested, 1,
			      memory_order_release);
	pthread_cond_signal(&evictor_cv);

	pthread_mutex_lock(&evictor_mtx);
	while (atomic_load_explicit(&evictor_drain_requested,
				    memory_order_acquire))
		pthread_cond_wait(&evictor_drain_cv, &evictor_mtx);
	pthread_mutex_unlock(&evictor_mtx);
}

void evictor_set_mode(enum evictor_mode mode)
{
	atomic_store_explicit(&evictor_current_mode, mode,
			      memory_order_relaxed);
}

enum evictor_mode evictor_get_mode(void)
{
	return atomic_load_explicit(&evictor_current_mode,
				    memory_order_relaxed);
}

int evictor_init(void)
{
	atomic_store_explicit(&evictor_running, 1, memory_order_relaxed);
	atomic_store_explicit(&evictor_needed, 0, memory_order_relaxed);
	atomic_store_explicit(&evictor_drain_requested, 0,
			      memory_order_relaxed);
	atomic_store_explicit(&evictor_current_mode, EVICTOR_ASYNC,
			      memory_order_relaxed);

	int ret =
		pthread_create(&evictor_thread, NULL, evictor_thread_fn, NULL);
	if (ret) {
		LOG("evictor_init: pthread_create failed: %d", ret);
		atomic_store_explicit(&evictor_running, 0,
				      memory_order_relaxed);
	}
	return ret;
}

void evictor_fini(void)
{
	if (!atomic_load_explicit(&evictor_running, memory_order_relaxed))
		return;

	atomic_store_explicit(&evictor_running, 0, memory_order_release);
	pthread_cond_signal(&evictor_cv);
	pthread_join(evictor_thread, NULL);
}
