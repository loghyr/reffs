/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifndef _REFFS_LOCK_H
#define _REFFS_LOCK_H

#include <stdint.h>
#include <stdbool.h>
#include <urcu/rculist.h>
#include <urcu/ref.h>

struct inode;

/* Generic owner structure that can be extended by NLM or NFSv4 */
struct reffs_lock_owner {
	struct urcu_ref lo_ref;
	struct cds_list_head lo_list;
	void (*lo_release)(struct urcu_ref *);
	bool (*lo_match)(struct reffs_lock_owner *, void *arg);
};

struct reffs_lock {
	struct cds_list_head l_list; /* link in inode->i_locks */
	struct cds_list_head l_host_list; /* link in host list */
	struct inode *l_inode;
	struct reffs_lock_owner *l_owner;
	uint64_t l_offset;
	uint64_t l_len;
	bool l_exclusive;
};

struct reffs_share {
	struct cds_list_head s_list; /* link in inode->i_shares */
	struct cds_list_head s_host_list; /* link in host list */
	struct inode *s_inode;
	struct reffs_lock_owner *s_owner;
	uint32_t s_mode; /* Deny modes (e.g., NLM4_DENY_*) */
	uint32_t s_access; /* Access modes (e.g., NLM4_ACCESS_*) */
};

/* Unified Locking API */
bool reffs_lock_range_overlap(uint64_t off1, uint64_t len1, uint64_t off2,
			      uint64_t len2);

struct reffs_lock *reffs_lock_find_conflict(struct inode *inode,
					    uint64_t offset, uint64_t len,
					    bool exclusive,
					    struct reffs_lock_owner *owner);

int reffs_lock_add(struct inode *inode, struct reffs_lock *lock,
		   struct cds_list_head *host_list);

int reffs_lock_remove(struct inode *inode, uint64_t offset, uint64_t len,
		      struct reffs_lock_owner *owner,
		      struct cds_list_head *host_list);

/* Unified Share API */
int reffs_share_add(struct inode *inode, struct reffs_share *share,
		    struct cds_list_head *host_list);

int reffs_share_remove(struct inode *inode, struct reffs_lock_owner *owner,
		       struct cds_list_head *host_list);

void reffs_lock_free(struct reffs_lock *lock);
void reffs_share_free(struct reffs_share *share);

#endif /* _REFFS_LOCK_H */
