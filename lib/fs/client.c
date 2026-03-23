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
#include <xxhash.h>

#include "reffs/log.h"
#include "reffs/rcu.h"
#include "reffs/stateid.h"
#include "reffs/client.h"
#include "reffs/trace/fs.h"
#include "reffs/server.h"

/* ------------------------------------------------------------------ */
/* Hash table helpers                                                  */

static int client_match(struct cds_lfht_node *ht_node, const void *vkey)
{
	struct client *client =
		caa_container_of(ht_node, struct client, c_node);
	const uint64_t *key = vkey;

	return *key == client->c_id;
}

/* ------------------------------------------------------------------ */
/* Refcount / release                                                  */

bool client_unhash(struct client *client)
{
	uint64_t state;
	int __attribute__((unused)) ret;

	state = __atomic_fetch_and(&client->c_state, ~CLIENT_IS_HASHED,
				   __ATOMIC_ACQUIRE);
	if (!(state & CLIENT_IS_HASHED))
		return false;

	struct server_state *ss = server_state_find();
	if (!ss)
		return false;

	ret = cds_lfht_del(ss->ss_client_ht, &client->c_node);
	assert(!ret);

	server_state_put(ss);
	return true;
}

static void client_release(struct urcu_ref *ref)
{
	struct client *client = caa_container_of(ref, struct client, c_ref);

	trace_fs_client(client, __func__, __LINE__);

	client_unhash(client);
	client->c_release(ref);
}

struct client *client_get(struct client *client)
{
	if (!client)
		return NULL;

	if (!urcu_ref_get_unless_zero(&client->c_ref))
		return NULL;

	trace_fs_client(client, __func__, __LINE__);
	return client;
}

void client_put(struct client *client)
{
	if (!client)
		return;

	trace_fs_client(client, __func__, __LINE__);
	urcu_ref_put(&client->c_ref, client_release);
}

/* ------------------------------------------------------------------ */
/* Assign / find                                                       */

int client_assign(struct client *client, uint64_t id,
		  void (*free_rcu)(struct rcu_head *rcu),
		  void (*release)(struct urcu_ref *ref))
{
	struct cds_lfht_node *node;
	unsigned long hash = XXH3_64bits(&id, sizeof(id));

	client->c_id = id;
	client->c_free_rcu = free_rcu;
	client->c_release = release;

	client->c_stateids = cds_lfht_new(8, 8, 0, CDS_LFHT_AUTO_RESIZE, NULL);
	if (!client->c_stateids)
		return -ENOMEM;

	cds_lfht_node_init(&client->c_node);
	urcu_ref_init(&client->c_ref);

	struct server_state *ss = server_state_find();
	if (!ss)
		return -ESHUTDOWN;

	rcu_read_lock();
	client->c_state |= CLIENT_IS_HASHED;
	node = cds_lfht_add_unique(ss->ss_client_ht, hash, client_match,
				   &client->c_id, &client->c_node);
	rcu_read_unlock();

	server_state_put(ss);

	if (caa_unlikely(node != &client->c_node)) {
		/*
		 * Lost the race — a client with this id already exists.
		 * The caller owns the allocation so we just clean up
		 * what we set up and signal the collision.
		 */
		client->c_state &= ~CLIENT_IS_HASHED;
		cds_lfht_destroy(client->c_stateids, NULL);
		client->c_stateids = NULL;
		return -EEXIST;
	}

	/*
	 * urcu_ref_init sets c_ref to 1 for the hash table.
	 * Bump once more for the caller's reference.
	 */
	client_get(client);
	trace_fs_client(client, __func__, __LINE__);
	return 0;
}

struct client *client_find(uint64_t id)
{
	struct client *client = NULL;
	struct client *tmp;
	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;
	unsigned long hash = XXH3_64bits(&id, sizeof(id));

	struct server_state *ss = server_state_find();
	if (!ss)
		return NULL;

	rcu_read_lock();
	cds_lfht_lookup(ss->ss_client_ht, hash, client_match, &id, &iter);
	node = cds_lfht_iter_get_node(&iter);
	if (node) {
		tmp = caa_container_of(node, struct client, c_node);
		client = client_get(tmp);
	}
	rcu_read_unlock();

	server_state_put(ss);

	return client;
}

/* ------------------------------------------------------------------ */
/* Stateid drain                                                       */

void client_remove_all_stateids(struct client *client)
{
	struct cds_lfht_iter iter;
	struct stateid *stid;

	if (!client || !client->c_stateids)
		return;

	trace_fs_client(client, __func__, __LINE__);

	rcu_read_lock();
	cds_lfht_first(client->c_stateids, &iter);
	while (cds_lfht_iter_get_node(&iter) != NULL) {
		stid = caa_container_of(cds_lfht_iter_get_node(&iter),
					struct stateid, s_client_node);
		cds_lfht_next(client->c_stateids, &iter);
		if (stateid_client_unhash(stid))
			stateid_put(stid);
	}
	rcu_read_unlock();
}

void client_unload_all_clients(void)
{
	struct cds_lfht_iter iter;
	struct client *client;

	TRACE("unloading all clients");

	struct server_state *ss = server_state_find();
	if (!ss) {
		LOG("No server state");
		return;
	}

	if (!ss->ss_client_ht) {
		server_state_put(ss);
		return;
	}

	rcu_read_lock();
	cds_lfht_first(ss->ss_client_ht, &iter);
	while (cds_lfht_iter_get_node(&iter) != NULL) {
		client = caa_container_of(cds_lfht_iter_get_node(&iter),
					  struct client, c_node);
		cds_lfht_next(ss->ss_client_ht, &iter);
		trace_fs_client(client, __func__, __LINE__);
		client_remove_all_stateids(client);
		if (client_unhash(client))
			client_put(client);
	}
	rcu_read_unlock();

	server_state_put(ss);
}
