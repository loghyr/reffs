/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifndef _REFFS_NLM_LOCK_H
#define _REFFS_NLM_LOCK_H

#include <stdint.h>
#include <stdbool.h>
#include <urcu.h>
#include <urcu/rculist.h>
#include "nlm4_prot.h"

struct inode;

struct nlm4_lock_owner {
	struct urcu_ref lo_ref;
	struct cds_list_head lo_list; /* link in nlm4_owners */
	netobj lo_oh;
	uint32_t lo_svid;
	char *lo_caller;
};

struct nlm4_lock_entry {
	struct cds_list_head le_list; /* link in inode->i_locks */
	struct nlm4_lock_owner *le_owner;
	uint64_t le_offset;
	uint64_t le_len;
	bool le_exclusive;
};

struct nlm4_share_entry {
	struct cds_list_head se_list; /* link in inode->i_shares */
	struct nlm4_lock_owner *se_owner;
	fsh4_mode se_mode;
	fsh4_access se_access;
};

/* Lock Manager Functions */
void reffs_nlm4_init_grace(uint32_t seconds);
bool reffs_nlm4_in_grace(void);

int reffs_nlm4_lock(struct inode *inode, struct nlm4_lockargs *args);

int reffs_nlm4_unlock(struct inode *inode, struct nlm4_unlockargs *args);
int reffs_nlm4_test(struct inode *inode, struct nlm4_testargs *args,
		    nlm4_testres *res);
int reffs_nlm4_cancel(struct inode *inode, struct nlm4_cancargs *args);

int reffs_nlm4_share(struct inode *inode, struct nlm4_shareargs *args);
int reffs_nlm4_unshare(struct inode *inode, struct nlm4_shareargs *args);

void reffs_nlm4_free_all(struct nlm4_notify *args);

#endif /* _REFFS_NLM_LOCK_H */
