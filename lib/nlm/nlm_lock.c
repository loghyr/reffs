/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
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
#include "reffs/trace/nlm.h"

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

static void nlm4_owner_release(struct urcu_ref *ref)
{
	struct nlm4_lock_owner *lo =
		caa_container_of(ref, struct nlm4_lock_owner, lo_base.lo_ref);
	pthread_mutex_lock(&nlm4_owners_mutex);
	cds_list_del(&lo->lo_base.lo_list);
	pthread_mutex_unlock(&nlm4_owners_mutex);
	free(lo->lo_oh.n_bytes);
	free(lo->lo_caller);
	free(lo);
}

static bool nlm4_owner_match_op(struct reffs_lock_owner *lo_base, void *arg)
{
	struct nlm4_lock_owner *lo =
		caa_container_of(lo_base, struct nlm4_lock_owner, lo_base);
	struct nlm4_lock *lock = arg;

	if (lo->lo_svid != (uint32_t)lock->svid)
		return false;
	if (lo->lo_oh.n_len != lock->oh.n_len)
		return false;
	if (strcmp(lo->lo_caller, lock->caller_name) != 0)
		return false;
	return memcmp(lo->lo_oh.n_bytes, lock->oh.n_bytes, lock->oh.n_len) == 0;
}

static struct nlm4_lock_owner *nlm4_find_owner(struct nlm4_lock *lock)
{
	struct nlm4_lock_owner *lo;

	cds_list_for_each_entry(lo, &nlm4_owners, lo_base.lo_list) {
		if (nlm4_owner_match_op(&lo->lo_base, lock)) {
			urcu_ref_get(&lo->lo_base.lo_ref);
			return lo;
		}
	}
	return NULL;
}

static struct nlm4_lock_owner *nlm4_get_owner(struct nlm4_lock *lock)
{
	struct nlm4_lock_owner *lo;

	pthread_mutex_lock(&nlm4_owners_mutex);
	lo = nlm4_find_owner(lock);
	if (!lo) {
		lo = calloc(1, sizeof(*lo));
		if (lo) {
			urcu_ref_init(&lo->lo_base.lo_ref);
			lo->lo_base.lo_release = nlm4_owner_release;
			lo->lo_base.lo_match = nlm4_owner_match_op;
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
			}
			cds_list_add(&lo->lo_base.lo_list, &nlm4_owners);
		}
	}
no_memory:
	pthread_mutex_unlock(&nlm4_owners_mutex);
	return lo;
}

/* Check for conflicts */

int reffs_nlm4_lock(struct inode *inode, struct nlm4_lockargs *args)
{
	struct nlm4_lock_owner *lo;
	struct reffs_lock *conflict;
	struct reffs_lock *lock;
	int ret;

	trace_nlm4_lock(inode, args->alock.svid, args->alock.l_offset,
			args->alock.l_len, args->exclusive);

	lo = nlm4_get_owner(&args->alock);

	if (!lo)
		return NLM4_DENIED_NOLOCKS;

	pthread_mutex_lock(&inode->i_lock_mutex);

	conflict = reffs_lock_find_conflict(inode, args->alock.l_offset,
					    args->alock.l_len, args->exclusive,
					    &lo->lo_base, &args->alock);
	if (conflict) {
		urcu_ref_put(&lo->lo_base.lo_ref, lo->lo_base.lo_release);
		pthread_mutex_unlock(&inode->i_lock_mutex);
		return NLM4_DENIED;
	}

	lock = calloc(1, sizeof(*lock));
	if (!lock) {
		urcu_ref_put(&lo->lo_base.lo_ref, lo->lo_base.lo_release);
		pthread_mutex_unlock(&inode->i_lock_mutex);
		return NLM4_DENIED_NOLOCKS;
	}

	lock->l_owner = &lo->lo_base; /* Already held lo_ref from get_owner */
	lock->l_offset = args->alock.l_offset;
	lock->l_len = args->alock.l_len;
	lock->l_exclusive = args->exclusive;
	lock->l_inode = inode_active_get(inode);
	if (!lock->l_inode) {
		reffs_lock_free(lock);
		pthread_mutex_unlock(&inode->i_lock_mutex);
		return NLM4_DENIED_NOLOCKS;
	}

	struct nlm4_host *host = nlm4_get_host(args->alock.caller_name);
	if (!host) {
		reffs_lock_free(lock);
		pthread_mutex_unlock(&inode->i_lock_mutex);
		return NLM4_DENIED_NOLOCKS;
	}

	ret = reffs_lock_add(inode, lock, &host->h_locks);

	pthread_mutex_unlock(&inode->i_lock_mutex);
	return (ret == 0) ? NLM4_GRANTED : NLM4_DENIED;
}

int reffs_nlm4_unlock(struct inode *inode, struct nlm4_unlockargs *args)
{
	struct nlm4_lock_owner *lo;
	struct nlm4_host *host;

	trace_nlm4_unlock(inode, args->alock.svid, args->alock.l_offset,
			  args->alock.l_len);

	lo = nlm4_get_owner(&args->alock);

	if (!lo)
		return NLM4_DENIED_NOLOCKS;

	host = nlm4_get_host(args->alock.caller_name);
	/* Put the extra ref from get_owner immediately, we only need to use it for comparison */
	urcu_ref_put(&lo->lo_base.lo_ref, lo->lo_base.lo_release);

	pthread_mutex_lock(&inode->i_lock_mutex);

	reffs_lock_remove(inode, args->alock.l_offset, args->alock.l_len,
			  &lo->lo_base, host ? &host->h_locks : NULL);

	pthread_mutex_unlock(&inode->i_lock_mutex);
	return NLM4_GRANTED;
}

int reffs_nlm4_test(struct inode *inode, struct nlm4_testargs *args,
		    nlm4_testres *res)
{
	struct nlm4_lock_owner *lo;
	struct reffs_lock *conflict;

	trace_nlm4_test(inode, args->alock.svid, args->alock.l_offset,
			args->alock.l_len, args->exclusive);

	lo = nlm4_get_owner(&args->alock);

	if (!lo) {
		res->stat.stat = NLM4_DENIED_NOLOCKS;
		return 0;
	}

	pthread_mutex_lock(&inode->i_lock_mutex);

	conflict = reffs_lock_find_conflict(inode, args->alock.l_offset,
					    args->alock.l_len, args->exclusive,
					    &lo->lo_base, &args->alock);
	if (conflict) {
		struct nlm4_lock_owner *clo = caa_container_of(
			conflict->l_owner, struct nlm4_lock_owner, lo_base);
		res->stat.stat = NLM4_DENIED;
		res->stat.nlm4_testrply_u.holder.exclusive =
			conflict->l_exclusive;
		res->stat.nlm4_testrply_u.holder.svid = clo->lo_svid;
		res->stat.nlm4_testrply_u.holder.oh.n_len = clo->lo_oh.n_len;
		res->stat.nlm4_testrply_u.holder.oh.n_bytes =
			malloc(clo->lo_oh.n_len);
		if (!res->stat.nlm4_testrply_u.holder.oh.n_bytes) {
			res->stat.stat = NLM4_FAILED;
			urcu_ref_put(&lo->lo_base.lo_ref,
				     lo->lo_base.lo_release);
			pthread_mutex_unlock(&inode->i_lock_mutex);
			return 0;
		}
		memcpy(res->stat.nlm4_testrply_u.holder.oh.n_bytes,
		       clo->lo_oh.n_bytes, clo->lo_oh.n_len);
		res->stat.nlm4_testrply_u.holder.l_offset = conflict->l_offset;
		res->stat.nlm4_testrply_u.holder.l_len = conflict->l_len;
	} else {
		res->stat.stat = NLM4_GRANTED;
	}

	urcu_ref_put(&lo->lo_base.lo_ref, lo->lo_base.lo_release);
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
	struct nlm4_lock_owner *lo;
	struct reffs_share *share;
	struct nlm4_host *host;
	int ret;

	struct nlm4_lock tmp_lock = {
		.caller_name = args->share.caller_name,
		.oh = args->share.oh,
		.svid = 0,
	};

	lo = nlm4_get_owner(&tmp_lock);
	if (!lo)
		return NLM4_DENIED_NOLOCKS;

	pthread_mutex_lock(&inode->i_lock_mutex);

	share = calloc(1, sizeof(*share));
	if (!share) {
		urcu_ref_put(&lo->lo_base.lo_ref, lo->lo_base.lo_release);
		pthread_mutex_unlock(&inode->i_lock_mutex);
		return NLM4_DENIED_NOLOCKS;
	}

	share->s_owner = &lo->lo_base;
	share->s_mode = args->share.mode;
	share->s_access = args->share.access;
	share->s_inode = inode_active_get(inode);
	if (!share->s_inode) {
		reffs_share_free(share);
		pthread_mutex_unlock(&inode->i_lock_mutex);
		return NLM4_DENIED_NOLOCKS;
	}

	host = nlm4_get_host(args->share.caller_name);
	if (!host) {
		reffs_share_free(share);
		pthread_mutex_unlock(&inode->i_lock_mutex);
		return NLM4_DENIED_NOLOCKS;
	}

	ret = reffs_share_add(inode, share, &host->h_shares);

	pthread_mutex_unlock(&inode->i_lock_mutex);
	return (ret == 0) ? NLM4_GRANTED : NLM4_DENIED;
}

int reffs_nlm4_unshare(struct inode *inode, struct nlm4_shareargs *args)
{
	struct nlm4_lock_owner *lo;
	struct nlm4_host *host;
	struct nlm4_lock tmp_lock = {
		.caller_name = args->share.caller_name,
		.oh = args->share.oh,
		.svid = 0,
	};

	lo = nlm4_get_owner(&tmp_lock);
	if (!lo)
		return NLM4_DENIED_NOLOCKS;

	host = nlm4_get_host(args->share.caller_name);
	urcu_ref_put(&lo->lo_base.lo_ref, lo->lo_base.lo_release);

	pthread_mutex_lock(&inode->i_lock_mutex);

	reffs_share_remove(inode, &lo->lo_base, host ? &host->h_shares : NULL);

	pthread_mutex_unlock(&inode->i_lock_mutex);
	return NLM4_GRANTED;
}

void reffs_nlm4_free_all(struct nlm4_notify *args)
{
	struct nlm4_host *host = NULL;
	struct reffs_lock *le, *le_tmp;
	struct reffs_share *se, *se_tmp;

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
	cds_list_for_each_entry_safe(le, le_tmp, &host->h_locks, l_host_list) {
		struct inode *inode = le->l_inode;
		pthread_mutex_lock(&inode->i_lock_mutex);
		cds_list_del(&le->l_list);
		cds_list_del(&le->l_host_list);
		pthread_mutex_unlock(&inode->i_lock_mutex);

		reffs_lock_free(le);
	}

	/* Process shares */
	cds_list_for_each_entry_safe(se, se_tmp, &host->h_shares, s_host_list) {
		struct inode *inode = se->s_inode;
		pthread_mutex_lock(&inode->i_lock_mutex);
		cds_list_del(&se->s_list);
		cds_list_del(&se->s_host_list);
		pthread_mutex_unlock(&inode->i_lock_mutex);

		reffs_share_free(se);
	}

	pthread_mutex_unlock(&nlm4_hosts_mutex);
}
