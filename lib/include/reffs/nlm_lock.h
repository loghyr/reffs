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
#include "reffs/lock.h"

struct inode;

struct nlm4_lock_owner {
	struct reffs_lock_owner lo_base;
	netobj lo_oh;
	uint32_t lo_svid;
	char *lo_caller;
};

/* Lock Manager Functions */
#define REFFS_NLM4_GRACE_PERIOD 45
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

void reffs_nlm4_owner_put(struct nlm4_lock_owner *lo);

#endif /* _REFFS_NLM_LOCK_H */
