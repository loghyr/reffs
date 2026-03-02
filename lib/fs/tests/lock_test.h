/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifndef _REFFS_TEST_LOCK_H
#define _REFFS_TEST_LOCK_H

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <urcu.h>
#include <urcu/rculist.h>
#include <urcu/ref.h>
#include "reffs/inode.h"
#include "reffs/lock.h"
#include "reffs/super_block.h"

/*
 * Share mode/access constants, mirroring the NFSv4 fsh4_mode / fsh4_access
 * enums used by lock.c.  Bit 0 = READ, bit 1 = WRITE.
 */
#define FSM_DN 0u /* deny none   */
#define FSM_DR 1u /* deny read   */
#define FSM_DW 2u /* deny write  */
#define FSM_DRW 3u /* deny both   */

#define FSA_NONE 0u /* access none  */
#define FSA_R 1u /* access read  */
#define FSA_W 2u /* access write */
#define FSA_RW 3u /* access both  */

/*
 * Minimal lock_owner that satisfies the reffs_lock_owner API.
 * lo_release just frees the containing struct.
 * lo_match is left NULL by default; set it per-test when needed.
 */
struct test_lock_owner {
	struct reffs_lock_owner base;
	int id; /* for human-readable test assertions */
};

static inline void test_owner_release(struct urcu_ref *ref)
{
	struct test_lock_owner *o =
		caa_container_of(ref, struct test_lock_owner, base.lo_ref);
	free(o);
}

static inline struct test_lock_owner *test_owner_alloc(int id)
{
	struct test_lock_owner *o = calloc(1, sizeof(*o));
	if (!o)
		return NULL;
	urcu_ref_init(&o->base.lo_ref);
	CDS_INIT_LIST_HEAD(&o->base.lo_list);
	o->base.lo_release = test_owner_release;
	o->base.lo_match = NULL;
	o->id = id;
	return o;
}

static inline void test_owner_put(struct test_lock_owner *o)
{
	urcu_ref_put(&o->base.lo_ref, test_owner_release);
}

/*
 * Allocate a reffs_lock fully initialized for use with reffs_lock_add().
 * Caller is responsible for calling reffs_lock_free() or letting
 * reffs_lock_remove() handle it.
 */
static inline struct reffs_lock *test_lock_alloc(struct inode *inode,
						 struct test_lock_owner *owner,
						 uint64_t offset, uint64_t len,
						 bool exclusive)
{
	struct reffs_lock *rl = calloc(1, sizeof(*rl));
	if (!rl)
		return NULL;
	CDS_INIT_LIST_HEAD(&rl->l_list);
	CDS_INIT_LIST_HEAD(&rl->l_host_list);
	rl->l_inode = inode_get(inode);
	rl->l_owner = &owner->base;
	urcu_ref_get(&owner->base.lo_ref);
	rl->l_offset = offset;
	rl->l_len = len;
	rl->l_exclusive = exclusive;
	return rl;
}

/*
 * Allocate a reffs_share fully initialized for use with reffs_share_add().
 */
static inline struct reffs_share *
test_share_alloc(struct inode *inode, struct test_lock_owner *owner,
		 uint32_t mode, uint32_t access)
{
	struct reffs_share *rs = calloc(1, sizeof(*rs));
	if (!rs)
		return NULL;
	CDS_INIT_LIST_HEAD(&rs->s_list);
	CDS_INIT_LIST_HEAD(&rs->s_host_list);
	rs->s_inode = inode_get(inode);
	rs->s_owner = &owner->base;
	urcu_ref_get(&owner->base.lo_ref);
	rs->s_mode = mode;
	rs->s_access = access;
	return rs;
}

/*
 * Count entries in inode->i_locks.
 */
static inline int lock_count(struct inode *inode)
{
	struct reffs_lock *rl;
	int n = 0;
	cds_list_for_each_entry(rl, &inode->i_locks, l_list)
		n++;
	return n;
}

/*
 * Count entries in inode->i_shares.
 */
static inline int share_count(struct inode *inode)
{
	struct reffs_share *rs;
	int n = 0;
	cds_list_for_each_entry(rs, &inode->i_shares, s_list)
		n++;
	return n;
}

/*
 * Remove and free all locks and shares on an inode, then drop the inode ref.
 * Used at the end of each test to avoid leaking memory that would trigger
 * assertions in super_block_remove_all_inodes().
 */
static inline void test_inode_cleanup(struct inode *inode,
				      struct test_lock_owner **owners,
				      int n_owners)
{
	struct reffs_lock *rl, *rltmp;
	struct reffs_share *rs, *rstmp;

	cds_list_for_each_entry_safe(rl, rltmp, &inode->i_locks, l_list) {
		cds_list_del(&rl->l_list);
		reffs_lock_free(rl);
	}
	cds_list_for_each_entry_safe(rs, rstmp, &inode->i_shares, s_list) {
		cds_list_del(&rs->s_list);
		reffs_share_free(rs);
	}
	inode_put(inode);

	for (int i = 0; i < n_owners; i++)
		if (owners[i])
			test_owner_put(owners[i]);
}

#endif /* _REFFS_TEST_LOCK_H */
