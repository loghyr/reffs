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

#include <urcu/compiler.h>
#include <urcu/rculfhash.h>

#include "nfsv42_xdr.h"
#include "reffs/log.h"
#include "reffs/inode.h"
#include "reffs/stateid.h"
#include "reffs/client.h"
#include "nfs4/stateid.h"
#include "nfs4/client.h"

/* ------------------------------------------------------------------ */
/* Special Stateids                                                   */

const stateid4 stateid4_anonymous = { .seqid = 0, .other = { 0 } };

const stateid4 stateid4_read_bypass = {
	.seqid = UINT32_MAX,
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-designator"
	.other = { [0 ... NFS4_OTHER_SIZE - 1] = 0xff }
#pragma clang diagnostic pop
};

const stateid4 stateid4_current = { .seqid = 1, .other = { 0 } };

const stateid4 stateid4_invalid = { .seqid = UINT32_MAX, .other = { 0 } };

/* ------------------------------------------------------------------ */
/* Wire pack / unpack                                                  */

void pack_stateid4(stateid4 *st, struct stateid *stid)
{
	uint8_t *other = (uint8_t *)st->other;
	uint32_t tag = stid->s_tag;

	st->seqid = __atomic_load_n(&stid->s_seqid, __ATOMIC_RELAXED);

	memcpy(other + 0, &stid->s_id, sizeof(uint32_t));
	memcpy(other + 4, &tag, sizeof(uint32_t));
	memcpy(other + 8, &stid->s_cookie, sizeof(uint32_t));
}

void unpack_stateid4(const stateid4 *st, uint32_t *seqid, uint32_t *id,
		     uint32_t *type, uint32_t *cookie)
{
	const uint8_t *other = (const uint8_t *)st->other;

	*seqid = st->seqid;
	memcpy(id, other + 0, sizeof(uint32_t));
	memcpy(type, other + 4, sizeof(uint32_t));
	memcpy(cookie, other + 8, sizeof(uint32_t));
}

/* ------------------------------------------------------------------ */
/* Open stateid                                                        */

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

struct open_stateid *open_stateid_alloc(struct inode *inode,
					struct client *client)
{
	struct open_stateid *os;
	int ret;

	os = calloc(1, sizeof(*os));
	if (!os) {
		LOG("open_stateid_alloc: OOM");
		return NULL;
	}

	ret = stateid_assign(&os->os_stid, inode, client, Open_Stateid,
			     open_stateid_free_rcu, open_stateid_release);
	if (ret) {
		free(os);
		return NULL;
	}

	return os;
}

/* ------------------------------------------------------------------ */
/* Delegation stateid                                                  */

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

struct delegation_stateid *delegation_stateid_alloc(struct inode *inode,
						    struct client *client)
{
	struct delegation_stateid *ds;
	int ret;

	ds = calloc(1, sizeof(*ds));
	if (!ds) {
		LOG("delegation_stateid_alloc: OOM");
		return NULL;
	}

	ret = stateid_assign(&ds->ds_stid, inode, client, Delegation_Stateid,
			     delegation_stateid_free_rcu,
			     delegation_stateid_release);
	if (ret) {
		free(ds);
		return NULL;
	}

	return ds;
}

/* ------------------------------------------------------------------ */
/* Lock stateid                                                        */

static void lock_stateid_free_rcu(struct rcu_head *rcu)
{
	struct stateid *stid = caa_container_of(rcu, struct stateid, s_rcu);
	struct lock_stateid *ls = stid_to_lock(stid);

	if (ls->ls_owner)
		urcu_ref_put(&ls->ls_owner->lo_base.lo_ref,
			     ls->ls_owner->lo_base.lo_release);
	if (ls->ls_open)
		stateid_put(&ls->ls_open->os_stid);
	free(ls);
}

static void lock_stateid_release(struct stateid *stid)
{
	call_rcu(&stid->s_rcu, lock_stateid_free_rcu);
}

struct lock_stateid *lock_stateid_alloc(struct inode *inode,
					struct client *client)
{
	struct lock_stateid *ls;
	int ret;

	ls = calloc(1, sizeof(*ls));
	if (!ls) {
		LOG("lock_stateid_alloc: OOM");
		return NULL;
	}

	ret = stateid_assign(&ls->ls_stid, inode, client, Lock_Stateid,
			     lock_stateid_free_rcu, lock_stateid_release);
	if (ret) {
		free(ls);
		return NULL;
	}

	return ls;
}

/* ------------------------------------------------------------------ */
/* Layout stateid                                                      */

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

struct layout_stateid *layout_stateid_alloc(struct inode *inode,
					    struct client *client)
{
	struct layout_stateid *ls;
	int ret;

	ls = calloc(1, sizeof(*ls));
	if (!ls) {
		LOG("layout_stateid_alloc: OOM");
		return NULL;
	}

	ret = stateid_assign(&ls->ls_stid, inode, client, Layout_Stateid,
			     layout_stateid_free_rcu, layout_stateid_release);
	if (ret) {
		free(ls);
		return NULL;
	}

	return ls;
}
