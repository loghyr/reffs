/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_STATEID_H
#define _REFFS_STATEID_H

#include <stdint.h>
#include <stdbool.h>

#include <urcu/ref.h>
#include <urcu/rculfhash.h>

#define STID_IS_HASHED (1ULL << 0)

struct inode; /* forward decl */

struct stateid {
	uint32_t s_seqid;
	uint32_t s_id; /* per-inode counter; never 0 */
	uint32_t s_tag; /* opaque to fs layer; NFS uses for stateid_type */
	uint32_t s_cookie; /* random cookie sent to client */

	struct rcu_head s_rcu;
	struct urcu_ref s_ref;

	struct inode *s_inode; /* active ref held while hashed */

	uint64_t s_state; /* atomic flag word */
	struct cds_lfht_node s_node;

	/* Per-instance callbacks set at construction; no extern vtable. */
	void (*s_free_rcu)(struct rcu_head *rcu);
	void (*s_release)(struct stateid *stid);
};

/* ------------------------------------------------------------------ */
/* Lifetime                                                            */

/*
 * stateid_assign - initialise *stid, insert into inode->i_stateids.
 *
 * tag:      caller-defined discriminator (NFS layer stores stateid_type here)
 * free_rcu: called via call_rcu() after last ref is dropped
 * release:  called from stateid_release(); must call call_rcu()
 *
 * Returns 0 on success, -errno on failure.
 */
int stateid_assign(struct stateid *stid, struct inode *inode, uint32_t tag,
		   void (*free_rcu)(struct rcu_head *rcu),
		   void (*release)(struct stateid *stid));

/*
 * stateid_find - look up by (inode, wire id).
 * Returns a ref-bumped pointer or NULL.  Caller must stateid_put() it.
 */
struct stateid *stateid_find(struct inode *inode, uint32_t id);

/* Bump / drop ref */
struct stateid *stateid_get(struct stateid *stid);
void stateid_put(struct stateid *stid);

/* Remove from hash table (idempotent).  Returns true if it was hashed. */
bool stateid_unhash(struct stateid *stid);

#endif /* _REFFS_STATEID_H */
