/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include "reffs/lock.h"
#include "reffs/inode.h"

bool reffs_lock_range_overlap(uint64_t off1, uint64_t len1, uint64_t off2,
			      uint64_t len2)
{
	uint64_t end1 = (len1 == 0) ? UINT64_MAX : off1 + len1 - 1;
	uint64_t end2 = (len2 == 0) ? UINT64_MAX : off2 + len2 - 1;

	return (off1 <= end2) && (off2 <= end1);
}

struct reffs_lock *reffs_lock_find_conflict(struct inode *inode,
					    uint64_t offset, uint64_t len,
					    bool exclusive,
					    struct reffs_lock_owner *owner,
					    void *match_arg)
{
	struct reffs_lock *rl;

	cds_list_for_each_entry(rl, &inode->i_locks, l_list) {
		if (!exclusive && !rl->l_exclusive)
			continue;

		if (owner && rl->l_owner == owner)
			continue;

		/*
		 * If we have a match function, use it to check if it's the same owner
		 * even if the pointers differ (e.g. NLM vs NFSv4)
		 */
		if (owner && owner->lo_match &&
		    owner->lo_match(rl->l_owner, match_arg))
			continue;

		if (reffs_lock_range_overlap(rl->l_offset, rl->l_len, offset,
					     len))
			return rl;
	}
	return NULL;
}

int reffs_lock_add(struct inode *inode, struct reffs_lock *lock,
		   struct cds_list_head *host_list)
{
	struct reffs_lock *le;

	/* Handle re-lock by same owner: if same range, just update exclusivity */
	cds_list_for_each_entry(le, &inode->i_locks, l_list) {
		if (le->l_owner == lock->l_owner &&
		    le->l_offset == lock->l_offset &&
		    le->l_len == lock->l_len) {
			le->l_exclusive = lock->l_exclusive;
			return 0;
		}
	}

	cds_list_add(&lock->l_list, &inode->i_locks);
	if (host_list) {
		cds_list_add(&lock->l_host_list, host_list);
	}
	return 0;
}

int reffs_lock_remove(struct inode *inode, uint64_t offset, uint64_t len,
		      struct reffs_lock_owner *owner,
		      struct cds_list_head *host_list)
{
	struct reffs_lock *le, *tmp;
	uint64_t u_off = offset;
	uint64_t u_len = len;
	uint64_t u_end = (u_len == 0) ? UINT64_MAX : u_off + u_len - 1;

	cds_list_for_each_entry_safe(le, tmp, &inode->i_locks, l_list) {
		if (le->l_owner != owner)
			continue;

		uint64_t l_off = le->l_offset;
		uint64_t l_len = le->l_len;
		uint64_t l_end = (l_len == 0) ? UINT64_MAX : l_off + l_len - 1;

		if (!reffs_lock_range_overlap(l_off, l_len, u_off, u_len))
			continue;

		if (u_off <= l_off && u_end >= l_end) {
			/* Full removal */
			cds_list_del(&le->l_list);
			if (host_list)
				cds_list_del(&le->l_host_list);
			reffs_lock_free(le);
		} else if (u_off > l_off && u_end < l_end) {
			/* Split in middle */
			struct reffs_lock *new_le = calloc(1, sizeof(*new_le));
			if (!new_le)
				return -ENOMEM;

			new_le->l_owner = le->l_owner;
			urcu_ref_get(&new_le->l_owner->lo_ref);
			new_le->l_inode = inode_get(le->l_inode);
			new_le->l_exclusive = le->l_exclusive;
			new_le->l_offset = u_end + 1;
			new_le->l_len = (l_len == 0) ?
						0 :
						(l_end - new_le->l_offset + 1);

			le->l_len = u_off - l_off;

			cds_list_add(&new_le->l_list, &le->l_list);
			if (host_list)
				cds_list_add(&new_le->l_host_list, host_list);
		} else if (u_off <= l_off) {
			/* Truncate start */
			le->l_offset = u_end + 1;
			le->l_len = (l_len == 0) ? 0 :
						   (l_end - le->l_offset + 1);
		} else {
			/* Truncate end */
			le->l_len = u_off - l_off;
		}
	}
	return 0;
}

void reffs_lock_free(struct reffs_lock *lock)
{
	if (!lock)
		return;
	inode_put(lock->l_inode);
	if (lock->l_owner)
		urcu_ref_put(&lock->l_owner->lo_ref, lock->l_owner->lo_release);
	free(lock);
}

/* Share Logic */

static bool reffs_share_conflict(uint32_t s1_mode, uint32_t s1_access,
				 uint32_t s2_mode, uint32_t s2_access)
{
	/*
	 * fsh4_mode (deny) enums: fsm_DN=0, fsm_DR=1, fsm_DW=2, fsm_DRW=3
	 * fsh4_access enums: fsa_NONE=0, fsa_R=1, fsa_W=2, fsa_RW=3
	 * These enums conveniently act as bitmasks (bit 0 = READ, bit 1 = WRITE).
	 */

	/* If S2 wants access that S1 denies, conflict */
	if ((s2_access & s1_mode) != 0)
		return true;
	/* If S1 has access that S2 denies, conflict */
	if ((s1_access & s2_mode) != 0)
		return true;
	return false;
}

int reffs_share_add(struct inode *inode, struct reffs_share *share,
		    struct cds_list_head *host_list)
{
	struct reffs_share *se, *existing = NULL;

	cds_list_for_each_entry(se, &inode->i_shares, s_list) {
		if (se->s_owner == share->s_owner) {
			existing = se;
			continue;
		}

		if (reffs_share_conflict(se->s_mode, se->s_access,
					 share->s_mode, share->s_access))
			return -EACCES;
	}

	if (existing) {
		/* Upgrade/Update existing share */
		existing->s_mode = share->s_mode;
		existing->s_access = share->s_access;
		reffs_share_free(share);
		return 0;
	}

	cds_list_add(&share->s_list, &inode->i_shares);
	if (host_list) {
		cds_list_add(&share->s_host_list, host_list);
	}
	return 0;
}

int reffs_share_remove(struct inode *inode, struct reffs_lock_owner *owner,
		       struct cds_list_head *host_list)
{
	struct reffs_share *se, *tmp;
	cds_list_for_each_entry_safe(se, tmp, &inode->i_shares, s_list) {
		if (se->s_owner == owner) {
			cds_list_del(&se->s_list);
			if (host_list)
				cds_list_del(&se->s_host_list);
			reffs_share_free(se);
		}
	}
	return 0;
}

void reffs_share_free(struct reffs_share *share)
{
	if (!share)
		return;
	inode_put(share->s_inode);
	if (share->s_owner)
		urcu_ref_put(&share->s_owner->lo_ref,
			     share->s_owner->lo_release);
	free(share);
}
