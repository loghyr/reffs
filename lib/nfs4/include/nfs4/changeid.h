/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_NFS4_CHANGEID_H
#define _REFFS_NFS4_CHANGEID_H

#include <stdatomic.h>

#include "nfsv42_xdr.h"
#include "reffs/inode.h"

/*
 * Read the current change ID of an inode with relaxed ordering.
 * Used to populate change_info4.before / change_info4.after when
 * the caller already holds the inode's i_db_rwlock (which provides
 * the required memory ordering for visibility of the protected data).
 *
 * RFC 8881 S5.11.1: change_info4.before / .after allow clients to
 * detect whether a modifying operation occurred exactly once, which
 * is particularly important for self-inverse operations such as
 * EXCHANGE_RANGE (draft-haynes-nfsv4-swap).
 */
static inline changeid4 inode_changeid(struct inode *inode)
{
	return (changeid4)atomic_load_explicit(&inode->i_changeid,
					       memory_order_relaxed);
}

#endif /* _REFFS_NFS4_CHANGEID_H */
