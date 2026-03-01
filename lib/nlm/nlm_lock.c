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
#include <errno.h>
#include <pthread.h>

#include "reffs/log.h"
#include "reffs/inode.h"
#include "reffs/super_block.h"
#include "reffs/fs.h"
#include "reffs/nlm_lock.h"

static CDS_LIST_HEAD(nlm4_owners);
static pthread_mutex_t nlm4_owners_mutex = PTHREAD_MUTEX_INITIALIZER;

static time_t grace_period_end = 0;

void reffs_nlm4_init_grace(uint32_t seconds)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	grace_period_end = now.tv_sec + seconds;
	LOG("NLM4: Grace period initialized for %u seconds (ends at %ld)",
	    seconds, (long)grace_period_end);
}

bool reffs_nlm4_in_grace(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return now.tv_sec < grace_period_end;
}

/* Helper to compare lock owners */

static bool nlm4_owner_match(struct nlm4_lock_owner *a, struct nlm4_lock *b)
{
	if (a->lo_svid != (uint32_t)b->svid)
		return false;
	if (a->lo_oh.n_len != b->oh.n_len)
		return false;
	if (strcmp(a->lo_caller, b->caller_name) != 0)
		return false;
	return memcmp(a->lo_oh.n_bytes, b->oh.n_bytes, b->oh.n_len) == 0;
}

static struct nlm4_lock_owner *nlm4_find_owner(struct nlm4_lock *lock)
{
	struct nlm4_lock_owner *lo;

	cds_list_for_each_entry(lo, &nlm4_owners, lo_list) {
		if (nlm4_owner_match(lo, lock)) {
			urcu_ref_get(&lo->lo_ref);
			return lo;
		}
	}
	return NULL;
}

static void nlm4_owner_release(struct urcu_ref *ref)
{
	struct nlm4_lock_owner *lo =
		caa_container_of(ref, struct nlm4_lock_owner, lo_ref);
	pthread_mutex_lock(&nlm4_owners_mutex);
	cds_list_del(&lo->lo_list);
	pthread_mutex_unlock(&nlm4_owners_mutex);
	free(lo->lo_oh.n_bytes);
	free(lo->lo_caller);
	free(lo);
}

static void nlm4_owner_put(struct nlm4_lock_owner *lo)
{
	if (lo)
		urcu_ref_put(&lo->lo_ref, nlm4_owner_release);
}

static struct nlm4_lock_owner *nlm4_get_owner(struct nlm4_lock *lock)
{
	struct nlm4_lock_owner *lo;

	pthread_mutex_lock(&nlm4_owners_mutex);
	lo = nlm4_find_owner(lock);
	if (!lo) {
		lo = calloc(1, sizeof(*lo));
		if (lo) {
			urcu_ref_init(&lo->lo_ref);
			lo->lo_svid = lock->svid;
			lo->lo_oh.n_len = lock->oh.n_len;
			lo->lo_oh.n_bytes = malloc(lock->oh.n_len);
			memcpy(lo->lo_oh.n_bytes, lock->oh.n_bytes,
			       lock->oh.n_len);
			lo->lo_caller = strdup(lock->caller_name);
			cds_list_add(&lo->lo_list, &nlm4_owners);
		}
	}
	pthread_mutex_unlock(&nlm4_owners_mutex);
	return lo;
}

/* Helper to check if two ranges overlap */
static bool nlm4_range_overlap(uint64_t off1, uint64_t len1, uint64_t off2,
			       uint64_t len2)
{
	uint64_t end1 = (len1 == 0) ? UINT64_MAX : off1 + len1 - 1;
	uint64_t end2 = (len2 == 0) ? UINT64_MAX : off2 + len2 - 1;

	return (off1 <= end2) && (off2 <= end1);
}

/* Check for conflicts */
static struct nlm4_lock_entry *
nlm4_find_conflict(struct inode *inode, struct nlm4_lock *lock, bool exclusive)
{
	struct nlm4_lock_entry *le;

	cds_list_for_each_entry(le, &inode->i_locks, le_list) {
		/* If both are shared, no conflict */
		if (!exclusive && !le->le_exclusive)
			continue;

		/* If same owner, no conflict (it's a re-lock or upgrade/downgrade) */
		if (nlm4_owner_match(le->le_owner, lock))
			continue;

		/* Check for overlap */
		if (nlm4_range_overlap(le->le_offset, le->le_len,
				       lock->l_offset, lock->l_len))
			return le;
	}

	return NULL;
}

int reffs_nlm4_lock(struct inode *inode, struct nlm4_lockargs *args)
{
	struct nlm4_lock_entry *le_conflict;
	struct nlm4_lock_entry *le;

	pthread_mutex_lock(&inode->i_lock_mutex);

	le_conflict = nlm4_find_conflict(inode, &args->alock, args->exclusive);
	if (le_conflict) {
		pthread_mutex_unlock(&inode->i_lock_mutex);
		return NLM4_DENIED;
	}

	/* Handle re-lock by same owner: if same range, just update exclusivity */
	cds_list_for_each_entry(le, &inode->i_locks, le_list) {
		if (nlm4_owner_match(le->le_owner, &args->alock) &&
		    le->le_offset == args->alock.l_offset &&
		    le->le_len == args->alock.l_len) {
			le->le_exclusive = args->exclusive;
			pthread_mutex_unlock(&inode->i_lock_mutex);
			return NLM4_GRANTED;
		}
	}

	/* Success! Create new lock entry */
	le = calloc(1, sizeof(*le));
	if (!le) {
		pthread_mutex_unlock(&inode->i_lock_mutex);
		return NLM4_DENIED_NOLOCKS;
	}

	le->le_owner = nlm4_get_owner(&args->alock);
	le->le_offset = args->alock.l_offset;
	le->le_len = args->alock.l_len;
	le->le_exclusive = args->exclusive;

	cds_list_add(&le->le_list, &inode->i_locks);

	pthread_mutex_unlock(&inode->i_lock_mutex);
	return NLM4_GRANTED;
}

int reffs_nlm4_unlock(struct inode *inode, struct nlm4_unlockargs *args)
{
	struct nlm4_lock_entry *le, *tmp;

	pthread_mutex_lock(&inode->i_lock_mutex);

	/* For now, just remove exactly matching locks.
	 * TODO: Implement splitting of locks for partial unlocks.
	 */
	cds_list_for_each_entry_safe(le, tmp, &inode->i_locks, le_list) {
		if (nlm4_owner_match(le->le_owner, &args->alock) &&
		    le->le_offset == args->alock.l_offset &&
		    le->le_len == args->alock.l_len) {
			cds_list_del(&le->le_list);
			nlm4_owner_put(le->le_owner);
			free(le);
		}
	}

	pthread_mutex_unlock(&inode->i_lock_mutex);
	return NLM4_GRANTED;
}

int reffs_nlm4_test(struct inode *inode, struct nlm4_testargs *args,
		    nlm4_testres *res)
{
	struct nlm4_lock_entry *le_conflict;

	pthread_mutex_lock(&inode->i_lock_mutex);

	le_conflict = nlm4_find_conflict(inode, &args->alock, args->exclusive);
	if (le_conflict) {
		res->stat.stat = NLM4_DENIED;
		res->stat.nlm4_testrply_u.holder.exclusive =
			le_conflict->le_exclusive;
		res->stat.nlm4_testrply_u.holder.svid =
			le_conflict->le_owner->lo_svid;
		res->stat.nlm4_testrply_u.holder.oh.n_len =
			le_conflict->le_owner->lo_oh.n_len;
		res->stat.nlm4_testrply_u.holder.oh.n_bytes =
			malloc(le_conflict->le_owner->lo_oh.n_len);
		memcpy(res->stat.nlm4_testrply_u.holder.oh.n_bytes,
		       le_conflict->le_owner->lo_oh.n_bytes,
		       le_conflict->le_owner->lo_oh.n_len);
		res->stat.nlm4_testrply_u.holder.l_offset =
			le_conflict->le_offset;
		res->stat.nlm4_testrply_u.holder.l_len = le_conflict->le_len;
	} else {
		res->stat.stat = NLM4_GRANTED;
	}

	pthread_mutex_unlock(&inode->i_lock_mutex);
	return 0;
}

int reffs_nlm4_cancel(struct inode *inode, struct nlm4_cancargs *args)
{
	(void)inode;
	(void)args;
	/* Stub for now: we don't support blocking yet, so nothing to cancel */
	return NLM4_GRANTED;
}

int reffs_nlm4_share(struct inode *inode, struct nlm4_shareargs *args)
{
	struct nlm4_share_entry *se;

	pthread_mutex_lock(&inode->i_lock_mutex);

	/* Check for existing conflicting shares */
	/* TODO: Implement full share reservation logic */

	se = calloc(1, sizeof(*se));
	if (!se) {
		pthread_mutex_unlock(&inode->i_lock_mutex);
		return NLM4_DENIED_NOLOCKS;
	}

	/* Create a temporary lock object to get the owner */
	struct nlm4_lock tmp_lock = {
		.caller_name = args->share.caller_name,
		.oh = args->share.oh,
		.svid = 0, /* svid not used in share */
	};

	se->se_owner = nlm4_get_owner(&tmp_lock);
	se->se_mode = args->share.mode;
	se->se_access = args->share.access;

	cds_list_add(&se->se_list, &inode->i_shares);

	pthread_mutex_unlock(&inode->i_lock_mutex);
	return NLM4_GRANTED;
}

int reffs_nlm4_unshare(struct inode *inode, struct nlm4_shareargs *args)
{
	struct nlm4_share_entry *se, *tmp;
	struct nlm4_lock tmp_lock = {
		.caller_name = args->share.caller_name,
		.oh = args->share.oh,
		.svid = 0,
	};

	pthread_mutex_lock(&inode->i_lock_mutex);

	cds_list_for_each_entry_safe(se, tmp, &inode->i_shares, se_list) {
		if (nlm4_owner_match(se->se_owner, &tmp_lock)) {
			cds_list_del(&se->se_list);
			nlm4_owner_put(se->se_owner);
			free(se);
		}
	}

	pthread_mutex_unlock(&inode->i_lock_mutex);
	return NLM4_GRANTED;
}

static void nlm4_free_inode_locks(struct inode *inode, const char *hostname)
{
	struct nlm4_lock_entry *le, *le_tmp;
	struct nlm4_share_entry *se, *se_tmp;

	pthread_mutex_lock(&inode->i_lock_mutex);

	cds_list_for_each_entry_safe(le, le_tmp, &inode->i_locks, le_list) {
		if (strcmp(le->le_owner->lo_caller, hostname) == 0) {
			cds_list_del(&le->le_list);
			nlm4_owner_put(le->le_owner);
			free(le);
		}
	}

	cds_list_for_each_entry_safe(se, se_tmp, &inode->i_shares, se_list) {
		if (strcmp(se->se_owner->lo_caller, hostname) == 0) {
			cds_list_del(&se->se_list);
			nlm4_owner_put(se->se_owner);
			free(se);
		}
	}

	pthread_mutex_unlock(&inode->i_lock_mutex);
}

static int nlm4_free_host_callback(struct inode *inode, void *arg)
{
	const char *hostname = arg;
	nlm4_free_inode_locks(inode, hostname);
	return 0;
}

void reffs_nlm4_free_all(struct nlm4_notify *args)
{
	LOG("NLM4: FREE_ALL for host %s", args->name);
	reffs_fs_for_each_inode(nlm4_free_host_callback, args->name);
}
