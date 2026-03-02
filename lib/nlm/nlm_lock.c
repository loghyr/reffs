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

struct nlm4_host {
	struct cds_list_head h_list;
	char *h_name;
	struct cds_list_head h_locks;
	struct cds_list_head h_shares;
};

static CDS_LIST_HEAD(nlm4_hosts);
static pthread_mutex_t nlm4_hosts_mutex = PTHREAD_MUTEX_INITIALIZER;

static struct nlm4_host *nlm4_get_host(const char *hostname)
{
	struct nlm4_host *host;

	pthread_mutex_lock(&nlm4_hosts_mutex);
	cds_list_for_each_entry(host, &nlm4_hosts, h_list) {
		if (strcmp(host->h_name, hostname) == 0) {
			pthread_mutex_unlock(&nlm4_hosts_mutex);
			return host;
		}
	}

	host = calloc(1, sizeof(*host));
	if (host) {
		host->h_name = strdup(hostname);
		if (!host->h_name) {
			free(host);
			pthread_mutex_unlock(&nlm4_hosts_mutex);
			return NULL;
		}

		CDS_INIT_LIST_HEAD(&host->h_locks);
		CDS_INIT_LIST_HEAD(&host->h_shares);
		cds_list_add(&host->h_list, &nlm4_hosts);
	}
	pthread_mutex_unlock(&nlm4_hosts_mutex);
	return host;
}

static void nlm4_lock_entry_free(struct nlm4_lock_entry *le)
{
	if (!le)
		return;
	inode_put(le->le_inode);
	reffs_nlm4_owner_put(le->le_owner);
	free(le);
}

static void nlm4_share_entry_free(struct nlm4_share_entry *se)
{
	if (!se)
		return;
	inode_put(se->se_inode);
	reffs_nlm4_owner_put(se->se_owner);
	free(se);
}

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

void reffs_nlm4_owner_put(struct nlm4_lock_owner *lo)
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
			if (!lo->lo_oh.n_bytes) {
				free(lo);
				lo = NULL;
				goto no_memory;
			}
			memcpy(lo->lo_oh.n_bytes, lock->oh.n_bytes,
			       lock->oh.n_len);
			lo->lo_caller = strdup(lock->caller_name);
			if (!lo->lo_caller) {
				free(lo->lo_oh.n_bytes);
				free(lo);
				lo = NULL;
				goto no_memory;
			} else {
				cds_list_add(&lo->lo_list, &nlm4_owners);
			}
		}
	}
no_memory:
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
	le->le_inode = inode_get(inode);
	if (!le->le_inode) {
		nlm4_lock_entry_free(le);
		pthread_mutex_unlock(&inode->i_lock_mutex);
		return NLM4_DENIED_NOLOCKS;
	}

	struct nlm4_host *host = nlm4_get_host(args->alock.caller_name);
	if (!host) {
		nlm4_lock_entry_free(le);
		pthread_mutex_unlock(&inode->i_lock_mutex);
		return NLM4_DENIED_NOLOCKS;
	}

	cds_list_add(&le->le_list, &inode->i_locks);
	pthread_mutex_lock(&nlm4_hosts_mutex);
	cds_list_add(&le->le_host_list, &host->h_locks);
	pthread_mutex_unlock(&nlm4_hosts_mutex);

	pthread_mutex_unlock(&inode->i_lock_mutex);
	return NLM4_GRANTED;
}

int reffs_nlm4_unlock(struct inode *inode, struct nlm4_unlockargs *args)
{
	struct nlm4_lock_entry *le, *tmp;
	uint64_t u_off = args->alock.l_offset;
	uint64_t u_len = args->alock.l_len;
	uint64_t u_end = (u_len == 0) ? UINT64_MAX : u_off + u_len - 1;

	pthread_mutex_lock(&inode->i_lock_mutex);

	cds_list_for_each_entry_safe(le, tmp, &inode->i_locks, le_list) {
		if (!nlm4_owner_match(le->le_owner, &args->alock))
			continue;

		uint64_t l_off = le->le_offset;
		uint64_t l_len = le->le_len;
		uint64_t l_end = (l_len == 0) ? UINT64_MAX : l_off + l_len - 1;

		if (!nlm4_range_overlap(l_off, l_len, u_off, u_len))
			continue;

		if (u_off <= l_off && u_end >= l_end) {
			/* Case 1: Full removal */
			cds_list_del(&le->le_list);
			pthread_mutex_lock(&nlm4_hosts_mutex);
			cds_list_del(&le->le_host_list);
			pthread_mutex_unlock(&nlm4_hosts_mutex);
			nlm4_lock_entry_free(le);
		} else if (u_off > l_off && u_end < l_end) {
			/* Case 2: Split in middle */
			struct nlm4_lock_entry *new_le =
				calloc(1, sizeof(*new_le));
			if (!new_le) {
				pthread_mutex_unlock(&inode->i_lock_mutex);
				return NLM4_DENIED_NOLOCKS;
			}

			/* New lock for the tail part */
			new_le->le_owner = le->le_owner;
			urcu_ref_get(&new_le->le_owner->lo_ref);
			new_le->le_inode = inode_get(le->le_inode);
			new_le->le_exclusive = le->le_exclusive;
			new_le->le_offset = u_end + 1;
			new_le->le_len =
				(l_len == 0) ? 0 :
					       (l_end - new_le->le_offset + 1);

			/* Update existing lock to be the head part */
			le->le_len = u_off - l_off;

			/* Add new tail lock to lists */
			cds_list_add(&new_le->le_list, &le->le_list);

			struct nlm4_host *host =
				nlm4_get_host(args->alock.caller_name);
			if (host) {
				pthread_mutex_lock(&nlm4_hosts_mutex);
				cds_list_add(&new_le->le_host_list,
					     &host->h_locks);
				pthread_mutex_unlock(&nlm4_hosts_mutex);
			} else {
				/* Should not happen if owner exists */
				nlm4_lock_entry_free(new_le);
				pthread_mutex_unlock(&inode->i_lock_mutex);
				return NLM4_FAILED;
			}
		} else if (u_off <= l_off) {
			/* Case 3: Truncate start */
			le->le_offset = u_end + 1;
			le->le_len = (l_len == 0) ? 0 :
						    (l_end - le->le_offset + 1);
		} else {
			/* Case 4: Truncate end */
			le->le_len = u_off - l_off;
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
		if (!res->stat.nlm4_testrply_u.holder.oh.n_bytes) {
			res->stat.stat = NLM4_FAILED;
			pthread_mutex_unlock(&inode->i_lock_mutex);
			return 0;
		}
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
	se->se_inode = inode_get(inode);
	if (!se->se_inode) {
		nlm4_share_entry_free(se);
		pthread_mutex_unlock(&inode->i_lock_mutex);
		return NLM4_DENIED_NOLOCKS;
	}

	struct nlm4_host *host = nlm4_get_host(args->share.caller_name);
	if (!host) {
		nlm4_share_entry_free(se);
		pthread_mutex_unlock(&inode->i_lock_mutex);
		return NLM4_DENIED_NOLOCKS;
	}

	cds_list_add(&se->se_list, &inode->i_shares);
	pthread_mutex_lock(&nlm4_hosts_mutex);
	cds_list_add(&se->se_host_list, &host->h_shares);
	pthread_mutex_unlock(&nlm4_hosts_mutex);

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
			pthread_mutex_lock(&nlm4_hosts_mutex);
			cds_list_del(&se->se_host_list);
			pthread_mutex_unlock(&nlm4_hosts_mutex);
			nlm4_share_entry_free(se);
		}
	}

	pthread_mutex_unlock(&inode->i_lock_mutex);
	return NLM4_GRANTED;
}

void reffs_nlm4_free_all(struct nlm4_notify *args)
{
	struct nlm4_host *host = NULL;
	struct nlm4_lock_entry *le, *le_tmp;
	struct nlm4_share_entry *se, *se_tmp;

	LOG("NLM4: FREE_ALL for host %s", args->name);

	pthread_mutex_lock(&nlm4_hosts_mutex);
	cds_list_for_each_entry(host, &nlm4_hosts, h_list) {
		if (strcmp(host->h_name, args->name) == 0) {
			break;
		}
	}
	if (!host) {
		pthread_mutex_unlock(&nlm4_hosts_mutex);
		return;
	}

	/* Process locks */
	cds_list_for_each_entry_safe(le, le_tmp, &host->h_locks, le_host_list) {
		struct inode *inode = le->le_inode;
		pthread_mutex_lock(&inode->i_lock_mutex);
		cds_list_del(&le->le_list);
		cds_list_del(&le->le_host_list);
		pthread_mutex_unlock(&inode->i_lock_mutex);

		nlm4_lock_entry_free(le);
	}

	/* Process shares */
	cds_list_for_each_entry_safe(se, se_tmp, &host->h_shares,
				     se_host_list) {
		struct inode *inode = se->se_inode;
		pthread_mutex_lock(&inode->i_lock_mutex);
		cds_list_del(&se->se_list);
		cds_list_del(&se->se_host_list);
		pthread_mutex_unlock(&inode->i_lock_mutex);

		nlm4_share_entry_free(se);
	}

	pthread_mutex_unlock(&nlm4_hosts_mutex);
}
