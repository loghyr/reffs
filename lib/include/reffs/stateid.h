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

#include "nfsv42_xdr.h"

/* NFS wire type */
#define NFS4_OTHER_SIZE 12

/*
 * other[0..3]  = s_id    (host byte order, packed via memcpy)
 * other[4..7]  = s_type
 * other[8..11] = s_thumb
 */

/* ------------------------------------------------------------------ */

enum stateid_type {
	Open_Stateid = 0,
	Delegation_Stateid = 1,
	Lock_Stateid = 2,
	Layout_Stateid = 3,
	Max_Stateid = 4
};

#define STID_IS_HASHED (1ULL << 0)

struct inode; /* forward decl */

struct stateid {
	uint32_t s_seqid;
	uint32_t s_id; /* per-inode counter; never 0 */
	enum stateid_type s_type;
	uint32_t s_thumb; /* random cookie sent to client */

	struct rcu_head s_rcu;
	struct urcu_ref s_ref;

	struct inode *s_inode; /* active ref held while hashed */

	uint64_t s_state; /* atomic flag word */
	struct cds_lfht_node s_node;
};

/* Per-type vtable */
struct stid_ops {
	void (*so_free_rcu)(struct rcu_head *rcu);
	void (*so_release)(struct stateid *stid);
};

extern const struct stid_ops stid_ops[Max_Stateid];

/* ------------------------------------------------------------------ */
/* Concrete stateid subtypes                                           */

struct open_stateid {
	uint64_t os_state;
	struct stateid os_stid;
};

struct delegation_stateid {
	uint64_t ds_state;
	struct stateid ds_stid;
};

struct lock_stateid {
	uint64_t ls_state;
	struct stateid ls_stid;
};

struct layout_stateid {
	uint64_t ls_state; /* avoid ls_ clash with lock */
	struct stateid ls_stid;
};

/* ------------------------------------------------------------------ */
/* Wire pack / unpack                                                  */

void pack_stateid4(stateid4 *st, struct stateid *stid);
void unpack_stateid4(const stateid4 *st, uint32_t *seqid, uint32_t *id,
		     uint32_t *type, uint32_t *thumb);

/* ------------------------------------------------------------------ */
/* Lifetime                                                            */

/*
 * stateid_find - look up by (inode, wire id); returns a ref or NULL.
 * Caller must hold rcu_read_lock() around the call or rely on the
 * internal lock taken inside.
 */
struct stateid *stateid_find(struct inode *inode, uint32_t id);

/* Bump / drop ref */
struct stateid *stateid_get(struct stateid *stid);
void stateid_put(struct stateid *stid);

/* Remove from hash table (idempotent).  Returns true if it was hashed. */
bool stateid_unhash(struct stateid *stid);

/* ------------------------------------------------------------------ */
/* Per-type allocators                                                 */

struct open_stateid *open_stateid_alloc(struct inode *inode);
struct delegation_stateid *delegation_stateid_alloc(struct inode *inode);
struct lock_stateid *lock_stateid_alloc(struct inode *inode);
struct layout_stateid *layout_stateid_alloc(struct inode *inode);

/* Convenience: embed -> wire stateid */
static inline struct stateid *open_stateid_to_stid(struct open_stateid *os)
{
	return &os->os_stid;
}

static inline struct stateid *
delegation_stateid_to_stid(struct delegation_stateid *ds)
{
	return &ds->ds_stid;
}

static inline struct stateid *lock_stateid_to_stid(struct lock_stateid *ls)
{
	return &ls->ls_stid;
}

static inline struct stateid *layout_stateid_to_stid(struct layout_stateid *ls)
{
	return &ls->ls_stid;
}

/* Convenience: stid -> containing subtype (no type check; caller knows) */
static inline struct open_stateid *stid_to_open(struct stateid *stid)
{
	return caa_container_of(stid, struct open_stateid, os_stid);
}

static inline struct delegation_stateid *
stid_to_delegation(struct stateid *stid)
{
	return caa_container_of(stid, struct delegation_stateid, ds_stid);
}

static inline struct lock_stateid *stid_to_lock(struct stateid *stid)
{
	return caa_container_of(stid, struct lock_stateid, ls_stid);
}

static inline struct layout_stateid *stid_to_layout(struct stateid *stid)
{
	return caa_container_of(stid, struct layout_stateid, ls_stid);
}

#endif /* STATEID_H */
