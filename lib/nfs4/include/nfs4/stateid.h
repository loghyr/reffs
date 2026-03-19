/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_NFS4_STATEID_H
#define _REFFS_NFS4_STATEID_H

#include <stdint.h>

#include "nfsv42_xdr.h"
#include "reffs/stateid.h" /* struct stateid, stateid_get/put/find */
#include "reffs/client.h"

/* ------------------------------------------------------------------ */
/* NFS4 stateid type tag — stored in s_tag                            */

enum stateid_type {
	Open_Stateid = 0,
	Delegation_Stateid = 1,
	Lock_Stateid = 2,
	Layout_Stateid = 3,
	Max_Stateid = 4
};

/* ------------------------------------------------------------------ */
/* Concrete stateid subtypes                                           */
/*
 * The embedded struct stateid carries s_free_rcu and s_release,
 * set at alloc time — no extern vtable needed.
 */

/* Bits stored in open_stateid.os_state for I/O access-mode checks. */
#define OPEN_STATEID_ACCESS_READ (1ULL << 0)
#define OPEN_STATEID_ACCESS_WRITE (1ULL << 1)

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
	uint64_t ls_state;
	struct stateid ls_stid;
};

/* ------------------------------------------------------------------ */
/* Special stateids                                                   */
static inline bool stateid4_other_is_zero(const stateid4 *st)
{
	static const char zeros[NFS4_OTHER_SIZE] = { 0 };
	return memcmp(st->other, zeros, NFS4_OTHER_SIZE) == 0;
}

static inline bool stateid4_other_is_ones(const stateid4 *st)
{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-designator"
	static const char ones[NFS4_OTHER_SIZE] = { [0 ... NFS4_OTHER_SIZE -
						     1] = 0xff };
#pragma clang diagnostic pop
	return memcmp(st->other, ones, NFS4_OTHER_SIZE) == 0;
}

/* nfs4_stateid_const.h */

extern const stateid4 stateid4_anonymous;
extern const stateid4 stateid4_read_bypass;
extern const stateid4 stateid4_current;
extern const stateid4 stateid4_invalid;

static inline bool stateid4_is_anonymous(const stateid4 *st)
{
	return st->seqid == 0 && stateid4_other_is_zero(st);
}

static inline bool stateid4_is_read_bypass(const stateid4 *st)
{
	return st->seqid == UINT32_MAX && stateid4_other_is_ones(st);
}

static inline bool stateid4_is_current(const stateid4 *st)
{
	return st->seqid == 1 && stateid4_other_is_zero(st);
}

static inline bool stateid4_is_invalid(const stateid4 *st)
{
	return st->seqid == UINT32_MAX && stateid4_other_is_zero(st);
}

static inline bool stateid4_is_special(const stateid4 *st)
{
	return stateid4_other_is_zero(st) || stateid4_other_is_ones(st);
}

static inline bool stateid4_equal(const stateid4 *a, const stateid4 *b)
{
	return a->seqid == b->seqid &&
	       memcmp(a->other, b->other, NFS4_OTHER_SIZE) == 0;
}

/* ------------------------------------------------------------------ */
/* Wire pack / unpack                                                 */
/*
 * other layout (host byte order; client treats as opaque):
 *   other[0..3]  = s_id
 *   other[4..7]  = s_tag  (stateid_type)
 *   other[8..11] = s_thumb
 */

void pack_stateid4(stateid4 *st, struct stateid *stid);
void unpack_stateid4(const stateid4 *st, uint32_t *seqid, uint32_t *id,
		     uint32_t *type, uint32_t *thumb);

/* ------------------------------------------------------------------ */
/* Per-type allocators                                                 */

struct open_stateid *open_stateid_alloc(struct inode *inode,
					struct client *client);
struct delegation_stateid *delegation_stateid_alloc(struct inode *inode,
						    struct client *client);
struct lock_stateid *lock_stateid_alloc(struct inode *inode,
					struct client *client);
struct layout_stateid *layout_stateid_alloc(struct inode *inode,
					    struct client *client);

/* ------------------------------------------------------------------ */
/* Subtype accessors                                                   */

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

#endif /* _REFFS_NFS4_STATEID_H */
