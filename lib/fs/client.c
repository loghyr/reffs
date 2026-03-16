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

/* ------------------------------------------------------------------ */
/* Module state                                                        */

static struct cds_lfht *client_ht;

#define CLIENT_MOD_INITIALIZED (1ULL << 0)
#define CLIENT_MOD_SHUTTING_DOWN (1ULL << 1)

static uint64_t client_mod_state;

int client_init(void)
{
	uint64_t old = __atomic_fetch_or(
		&client_mod_state, CLIENT_MOD_INITIALIZED, __ATOMIC_ACQUIRE);
	if (old & CLIENT_MOD_INITIALIZED) {
		LOG("client_init: already initialized");
		return -EALREADY;
	}

	client_ht = cds_lfht_new(8, 8, 0, CDS_LFHT_AUTO_RESIZE, NULL);
	if (!client_ht) {
		__atomic_fetch_and(&client_mod_state, ~CLIENT_MOD_INITIALIZED,
				   __ATOMIC_RELEASE);
		return -ENOMEM;
	}

	return 0;
}

int client_fini(void)
{
	uint64_t old;

	old = __atomic_fetch_or(&client_mod_state, CLIENT_MOD_SHUTTING_DOWN,
				__ATOMIC_ACQUIRE);
	if (!(old & CLIENT_MOD_INITIALIZED)) {
		LOG("client_fini: not initialized");
		return -EINVAL;
	}
	if (old & CLIENT_MOD_SHUTTING_DOWN) {
		LOG("client_fini: already shutting down");
		return -EALREADY;
	}

	/*
	 * Caller must have drained all clients before calling fini.
	 * cds_lfht_destroy() must be called outside rcu_read_lock().
	 */
	cds_lfht_destroy(client_ht, NULL);
	client_ht = NULL;

	__atomic_store_n(&client_mod_state, 0, __ATOMIC_RELEASE);
	return 0;
}

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
	int ret;

	state = __atomic_fetch_and(&client->c_state, ~CLIENT_IS_HASHED,
				   __ATOMIC_ACQUIRE);
	if (!(state & CLIENT_IS_HASHED))
		return false;

	ret = cds_lfht_del(client_ht, &client->c_node);
	assert(!ret);
	(void)ret;
	return true;
}

static void client_free_rcu(struct rcu_head *rcu)
{
	struct client *client = caa_container_of(rcu, struct client, c_rcu);

	if (client->c_stateids)
		cds_lfht_destroy(client->c_stateids, NULL);

	free(client);
}

static void client_release(struct urcu_ref *ref)
{
	struct client *client = caa_container_of(ref, struct client, c_ref);

	trace_fs_client(client, __func__, __LINE__);

	client_unhash(client);
	call_rcu(&client->c_rcu, client_free_rcu);
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
/* Alloc / find                                                        */

struct client *client_alloc(uint64_t id)
{
	struct cds_lfht_node *node;
	struct client *client;
	struct client *tmp;
	unsigned long hash = XXH3_64bits(&id, sizeof(id));

	client = calloc(1, sizeof(*client));
	if (!client)
		return NULL;

	client->c_id = id;

	client->c_stateids = cds_lfht_new(8, 8, 0, CDS_LFHT_AUTO_RESIZE, NULL);
	if (!client->c_stateids) {
		free(client);
		return NULL;
	}

	cds_lfht_node_init(&client->c_node);
	urcu_ref_init(&client->c_ref);

	rcu_read_lock();
	client->c_state |= CLIENT_IS_HASHED;
	node = cds_lfht_add_unique(client_ht, hash, client_match, &client->c_id,
				   &client->c_node);
	rcu_read_unlock();

	if (caa_unlikely(node != &client->c_node)) {
		/*
		 * Lost the race — another thread inserted the same id first.
		 * Drop our candidate and return a ref to the winner.
		 */
		client->c_state &= ~CLIENT_IS_HASHED;
		cds_lfht_destroy(client->c_stateids, NULL);
		free(client);
		tmp = caa_container_of(node, struct client, c_node);
		return client_get(tmp);
	}

	/*
	 * urcu_ref_init sets c_ref to 1 for the hash table.
	 * Bump once more for the caller's reference.
	 */
	client_get(client);
	return client;
}

struct client *client_find(uint64_t id)
{
	struct client *client = NULL;
	struct client *tmp;
	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;
	unsigned long hash = XXH3_64bits(&id, sizeof(id));

	if (!client_ht)
		return NULL;

	rcu_read_lock();
	cds_lfht_lookup(client_ht, hash, client_match, &id, &iter);
	node = cds_lfht_iter_get_node(&iter);
	if (node) {
		tmp = caa_container_of(node, struct client, c_node);
		client = client_get(tmp);
	}
	rcu_read_unlock();

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

	rcu_read_lock();
	cds_lfht_for_each_entry(client->c_stateids, &iter, stid,
				s_client_node) {
		if (stateid_client_unhash(stid))
			stateid_put(stid);
	}
	rcu_read_unlock();
}
