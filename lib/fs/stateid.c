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
#include <errno.h>

#include <urcu/compiler.h>
#include <urcu/ref.h>
#include <urcu/rculfhash.h>
#include <xxhash.h>

#include "reffs/log.h"
#include "reffs/rcu.h"
#include "reffs/client.h"
#include "reffs/inode.h"
#include "reffs/stateid.h"
#include "reffs/trace/fs.h"

/* ------------------------------------------------------------------ */
/* Hash table helpers                                                  */

static int stateid_match(struct cds_lfht_node *ht_node, const void *vkey)
{
	struct stateid *stid =
		caa_container_of(ht_node, struct stateid, s_inode_node);
	const uint32_t *key = vkey;

	return *key == stid->s_id;
}

/* ------------------------------------------------------------------ */
/* Internal assign                                                     */

int stateid_assign(struct stateid *stid, struct inode *inode,
		   struct client *client, uint32_t tag,
		   void (*free_rcu)(struct rcu_head *rcu),
		   void (*release)(struct stateid *stid))
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

	stid->s_inode = inode_active_get(inode);
	if (!stid->s_inode)
		return -ENOMEM;

	stid->s_client = client_get(client);
	if (!stid->s_client) {
		inode_active_put(stid->s_inode);
		stid->s_inode = NULL;
		return -ENOMEM;
	}

	stid->s_tag = tag;
	stid->s_seqid = 0;

	/*
	 * s_cookie: hash of the stid pointer — cheap, non-cryptographic,
	 * distinct across recycled addresses.
	 */
	stid->s_cookie = (uint32_t)XXH3_64bits(&stid, sizeof(stid));

	stid->s_free_rcu = free_rcu;
	stid->s_release = release;

	cds_lfht_node_init(&stid->s_inode_node);
	cds_lfht_node_init(&stid->s_client_node);
	urcu_ref_init(&stid->s_ref);

	uint64_t state = __atomic_load_n(&inode->i_state, __ATOMIC_ACQUIRE);
	if (!(state & INODE_IS_SHUTTING_DOWN)) {
		hash = XXH3_64bits(&stid->s_id, sizeof(stid->s_id));

		rcu_read_lock();
		stid->s_state |= STID_IS_INODE_HASHED;
		node = cds_lfht_add_unique(inode->i_stateids, hash,
					   stateid_match, &stid->s_id,
					   &stid->s_inode_node);
		rcu_read_unlock();

		if (caa_unlikely(node != &stid->s_inode_node)) {
			/* Collision – should never happen with a 32-bit counter */
			LOG("stateid_assign: duplicate id %u", stid->s_id);
			stid->s_state &= ~STID_IS_INODE_HASHED;
			inode_active_put(stid->s_inode);
			stid->s_inode = NULL;
			return -EEXIST;
		}

		rcu_read_lock();
		stid->s_state |= STID_IS_CLIENT_HASHED;
		node = cds_lfht_add_unique(client->c_stateids, hash,
					   stateid_match, &stid->s_id,
					   &stid->s_client_node);
		rcu_read_unlock();

		if (caa_unlikely(node != &stid->s_client_node)) {
			/* Collision – should never happen with a 32-bit counter */
			LOG("stateid_assign: duplicate id %u", stid->s_id);
			stid->s_state &= ~STID_IS_CLIENT_HASHED;

			/* Yes, could be from someone else */
			if (stateid_inode_unhash(stid))
				inode_active_put(stid->s_inode);
			client_put(stid->s_client);
			stid->s_client = NULL;
			return -EEXIST;
		}
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/* Refcount / release                                                  */

bool stateid_client_unhash(struct stateid *stid)
{
	uint64_t state;
	int ret;

	state = __atomic_fetch_and(&stid->s_state, ~STID_IS_CLIENT_HASHED,
				   __ATOMIC_ACQUIRE);
	if (!(state & STID_IS_CLIENT_HASHED))
		return false;

	assert(stid->s_client);
	ret = cds_lfht_del(stid->s_client->c_stateids, &stid->s_client_node);
	assert(!ret);
	(void)ret;
	return true;
}

bool stateid_inode_unhash(struct stateid *stid)
{
	uint64_t state;
	int ret;

	state = __atomic_fetch_and(&stid->s_state, ~STID_IS_INODE_HASHED,
				   __ATOMIC_ACQUIRE);
	if (!(state & STID_IS_INODE_HASHED))
		return false;

	assert(stid->s_inode);
	ret = cds_lfht_del(stid->s_inode->i_stateids, &stid->s_inode_node);
	assert(!ret);
	(void)ret;
	return true;
}

static void stateid_release(struct urcu_ref *ref)
{
	struct stateid *stid = caa_container_of(ref, struct stateid, s_ref);

	trace_fs_stateid(stid, __func__, __LINE__);

	stateid_inode_unhash(stid);
	inode_active_put(stid->s_inode);

	stateid_client_unhash(stid);
	client_put(stid->s_client);

	stid->s_release(stid);
}

struct stateid *stateid_get(struct stateid *stid)
{
	if (!stid)
		return NULL;

	if (!urcu_ref_get_unless_zero(&stid->s_ref))
		return NULL;

	trace_fs_stateid(stid, __func__, __LINE__);
	return stid;
}

void stateid_put(struct stateid *stid)
{
	if (!stid)
		return;

	trace_fs_stateid(stid, __func__, __LINE__);
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
		tmp = caa_container_of(node, struct stateid, s_inode_node);
		stid = stateid_get(tmp);
	}
	rcu_read_unlock();

	return stid;
}
