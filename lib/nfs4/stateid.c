/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

#include <urcu/compiler.h> /* caa_container_of */
#include <urcu/ref.h>
#include <urcu/rculfhash.h>
#include <xxhash.h>

#include "reffs/log.h"
#include "reffs/inode.h"
#include "reffs/stateid.h"
#include "reffs/trace/nfs4_server.h"
#include "nfsv42_xdr.h"

/* ------------------------------------------------------------------ */
/* Forward declarations of per-type callbacks                          */

static void open_stateid_free_rcu(struct rcu_head *rcu);
static void open_stateid_release(struct stateid *stid);

static void delegation_stateid_free_rcu(struct rcu_head *rcu);
static void delegation_stateid_release(struct stateid *stid);

static void lock_stateid_free_rcu(struct rcu_head *rcu);
static void lock_stateid_release(struct stateid *stid);

static void layout_stateid_free_rcu(struct rcu_head *rcu);
static void layout_stateid_release(struct stateid *stid);

/* ------------------------------------------------------------------ */
/* vtable                                                              */

const struct stid_ops stid_ops[Max_Stateid] = {
        [Open_Stateid] = {
                .so_free_rcu = open_stateid_free_rcu,
                .so_release  = open_stateid_release,
        },
        [Delegation_Stateid] = {
                .so_free_rcu = delegation_stateid_free_rcu,
                .so_release  = delegation_stateid_release,
        },
        [Lock_Stateid] = {
                .so_free_rcu = lock_stateid_free_rcu,
                .so_release  = lock_stateid_release,
        },
        [Layout_Stateid] = {
                .so_free_rcu = layout_stateid_free_rcu,
                .so_release  = layout_stateid_release,
        },
};

/* ------------------------------------------------------------------ */
/* Wire pack / unpack                                                  */

/*
 * other layout (all host byte order; the client treats it as opaque):
 *   other[0..3]  = s_id
 *   other[4..7]  = s_type
 *   other[8..11] = s_thumb
 */
void pack_stateid4(stateid4 *st, struct stateid *stid)
{
	uint8_t *other = (uint8_t *)st->other;
	uint32_t type = (uint32_t)stid->s_type;

	st->seqid = __atomic_load_n(&stid->s_seqid, __ATOMIC_RELAXED);

	memcpy(other + 0, &stid->s_id, sizeof(uint32_t));
	memcpy(other + 4, &type, sizeof(uint32_t));
	memcpy(other + 8, &stid->s_thumb, sizeof(uint32_t));
}

void unpack_stateid4(const stateid4 *st, uint32_t *seqid, uint32_t *id,
		     uint32_t *type, uint32_t *thumb)
{
	const uint8_t *other = (const uint8_t *)st->other;

	*seqid = st->seqid;
	memcpy(id, other + 0, sizeof(uint32_t));
	memcpy(type, other + 4, sizeof(uint32_t));
	memcpy(thumb, other + 8, sizeof(uint32_t));
}

/* ------------------------------------------------------------------ */
/* Hash table helpers                                                  */

static int stateid_match(struct cds_lfht_node *ht_node, const void *vkey)
{
	struct stateid *stid =
		caa_container_of(ht_node, struct stateid, s_node);
	const uint32_t *key = vkey;

	return *key == stid->s_id;
}

/* ------------------------------------------------------------------ */
/* Internal: assign common fields and insert into inode's hash table  */

static int stateid_assign(struct stateid *stid, struct inode *inode,
			  enum stateid_type st_type)
{
	struct cds_lfht_node *node;
	unsigned long hash;

	/*
         * Grab the next per-inode counter.  Wrap around but skip 0 so
         * that id==0 can serve as a sentinel.
         */
	do {
		stid->s_id = __atomic_fetch_add(&inode->i_stateid_next, 1,
						__ATOMIC_RELAXED) +
			     1;
	} while (stid->s_id == 0);

	hash = XXH3_64bits(&stid->s_id, sizeof(stid->s_id));

	stid->s_inode = inode_active_get(inode);
	if (!stid->s_inode)
		return -ENOMEM;

	stid->s_type = st_type;
	stid->s_seqid = 0;

	/*
         * s_thumb: use the low 32 bits of a hash of the pointer so
         * each allocation is distinct even if addresses are recycled.
         */
	stid->s_thumb = (uint32_t)XXH3_64bits(&stid, sizeof(stid));

	cds_lfht_node_init(&stid->s_node);
	urcu_ref_init(&stid->s_ref);

	rcu_read_lock();
	stid->s_state |= STID_IS_HASHED;
	node = cds_lfht_add_unique(inode->i_stateids, hash, stateid_match,
				   &stid->s_id, &stid->s_node);
	rcu_read_unlock();

	if (caa_unlikely(node != &stid->s_node)) {
		/* Collision – should never happen with a 32-bit counter */
		LOG("stateid_assign: duplicate id %u", stid->s_id);
		stid->s_state &= ~STID_IS_HASHED;
		inode_active_put(stid->s_inode);
		stid->s_inode = NULL;
		return -EEXIST;
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/* Refcount / release                                                  */

bool stateid_unhash(struct stateid *stid)
{
	uint64_t state;
	int ret;

	state = __atomic_fetch_and(&stid->s_state, ~STID_IS_HASHED,
				   __ATOMIC_ACQUIRE);
	if (!(state & STID_IS_HASHED))
		return false;

	ret = cds_lfht_del(stid->s_inode->i_stateids, &stid->s_node);
	assert(!ret);
	(void)ret;
	return true;
}

static void stateid_release(struct urcu_ref *ref)
{
	struct stateid *stid = caa_container_of(ref, struct stateid, s_ref);

	trace_nfs4_stateid(stid, __func__, __LINE__);

	stateid_unhash(stid);
	inode_active_put(stid->s_inode);

	stid_ops[stid->s_type].so_release(stid);
}

struct stateid *stateid_get(struct stateid *stid)
{
	if (!stid)
		return NULL;

	if (!urcu_ref_get_unless_zero(&stid->s_ref))
		return NULL;

	trace_nfs4_stateid(stid, __func__, __LINE__);
	return stid;
}

void stateid_put(struct stateid *stid)
{
	if (!stid)
		return;

	trace_nfs4_stateid(stid, __func__, __LINE__);
	urcu_ref_put(&stid->s_ref, stateid_release);
}

struct stateid *stateid_find(struct inode *inode, uint32_t id)
{
	struct stateid *stid = NULL;
	struct stateid *tmp;
	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;
	unsigned long hash = XXH3_64bits(&id, sizeof(id));

	if (!inode)
		return NULL;

	rcu_read_lock();
	cds_lfht_lookup(inode->i_stateids, hash, stateid_match, &id, &iter);
	node = cds_lfht_iter_get_node(&iter);
	if (node) {
		tmp = caa_container_of(node, struct stateid, s_node);
		stid = stateid_get(tmp);
	}
	rcu_read_unlock();

	return stid;
}

/* ------------------------------------------------------------------ */
/* Open stateid                                                        */

struct open_stateid *open_stateid_alloc(struct inode *inode)
{
	struct open_stateid *os;
	int ret;

	os = calloc(1, sizeof(*os));
	if (!os) {
		LOG("open_stateid_alloc: OOM");
		return NULL;
	}

	ret = stateid_assign(&os->os_stid, inode, Open_Stateid);
	if (ret) {
		free(os);
		return NULL;
	}

	return os;
}

static void open_stateid_free_rcu(struct rcu_head *rcu)
{
	struct stateid *stid = caa_container_of(rcu, struct stateid, s_rcu);
	struct open_stateid *os = stid_to_open(stid);

	free(os);
}

static void open_stateid_release(struct stateid *stid)
{
	call_rcu(&stid->s_rcu, open_stateid_free_rcu);
}

/* ------------------------------------------------------------------ */
/* Delegation stateid                                                  */

struct delegation_stateid *delegation_stateid_alloc(struct inode *inode)
{
	struct delegation_stateid *ds;
	int ret;

	ds = calloc(1, sizeof(*ds));
	if (!ds) {
		LOG("delegation_stateid_alloc: OOM");
		return NULL;
	}

	ret = stateid_assign(&ds->ds_stid, inode, Delegation_Stateid);
	if (ret) {
		free(ds);
		return NULL;
	}

	return ds;
}

static void delegation_stateid_free_rcu(struct rcu_head *rcu)
{
	struct stateid *stid = caa_container_of(rcu, struct stateid, s_rcu);
	struct delegation_stateid *ds = stid_to_delegation(stid);

	free(ds);
}

static void delegation_stateid_release(struct stateid *stid)
{
	call_rcu(&stid->s_rcu, delegation_stateid_free_rcu);
}

/* ------------------------------------------------------------------ */
/* Lock stateid                                                        */

struct lock_stateid *lock_stateid_alloc(struct inode *inode)
{
	struct lock_stateid *ls;
	int ret;

	ls = calloc(1, sizeof(*ls));
	if (!ls) {
		LOG("lock_stateid_alloc: OOM");
		return NULL;
	}

	ret = stateid_assign(&ls->ls_stid, inode, Lock_Stateid);
	if (ret) {
		free(ls);
		return NULL;
	}

	return ls;
}

static void lock_stateid_free_rcu(struct rcu_head *rcu)
{
	struct stateid *stid = caa_container_of(rcu, struct stateid, s_rcu);
	struct lock_stateid *ls = stid_to_lock(stid);

	free(ls);
}

static void lock_stateid_release(struct stateid *stid)
{
	call_rcu(&stid->s_rcu, lock_stateid_free_rcu);
}

/* ------------------------------------------------------------------ */
/* Layout stateid                                                      */

struct layout_stateid *layout_stateid_alloc(struct inode *inode)
{
	struct layout_stateid *ls;
	int ret;

	ls = calloc(1, sizeof(*ls));
	if (!ls) {
		LOG("layout_stateid_alloc: OOM");
		return NULL;
	}

	ret = stateid_assign(&ls->ls_stid, inode, Layout_Stateid);
	if (ret) {
		free(ls);
		return NULL;
	}

	return ls;
}

static void layout_stateid_free_rcu(struct rcu_head *rcu)
{
	struct stateid *stid = caa_container_of(rcu, struct stateid, s_rcu);
	struct layout_stateid *ls = stid_to_layout(stid);

	free(ls);
}

static void layout_stateid_release(struct stateid *stid)
{
	call_rcu(&stid->s_rcu, layout_stateid_free_rcu);
}
