/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_CLIENT_H
#define _REFFS_CLIENT_H

#include <stdint.h>
#include <stdbool.h>

#include <urcu/ref.h>
#include <urcu/rculfhash.h>

#define CLIENT_IS_HASHED (1ULL << 0)
#define CLIENT_IS_SHUTTING_DOWN (1ULL << 1)

struct client {
	uint64_t c_id; /* opaque to fs layer; NFS layer stores clientid4 */

	struct rcu_head c_rcu;
	struct urcu_ref c_ref;

	uint64_t c_state; /* atomic flag word */
	struct cds_lfht_node c_node;

	struct cds_lfht *c_stateids; /* per-client stateid hash table */

	/* Per-instance callbacks set at construction; no extern vtable. */
	void (*c_free_rcu)(struct rcu_head *rcu);
	void (*c_release)(struct urcu_ref *ref);
};

/* ------------------------------------------------------------------ */
/* Module lifecycle                                                    */

int client_init(void);
int client_fini(void);

/* ------------------------------------------------------------------ */
/* Lifetime                                                            */

/*
 * client_assign - initialise *client, insert into the global client_ht.
 * The caller (NFS layer) owns the allocation; free_rcu and release are
 * called from the fs layer's RCU / refcount paths.
 *
 * Returns 0 on success, -errno on failure.
 */
int client_assign(struct client *client, uint64_t id,
		  void (*free_rcu)(struct rcu_head *rcu),
		  void (*release)(struct urcu_ref *ref));

/*
 * client_find - look up by id.
 * Returns a ref-bumped pointer or NULL.  Caller must client_put() it.
 */
struct client *client_find(uint64_t id);

/* Bump / drop ref */
struct client *client_get(struct client *client);
void client_put(struct client *client);

/* Remove from hash table (idempotent).  Returns true if it was hashed. */
bool client_unhash(struct client *client);

/*
 * client_remove_all_stateids - drain c_stateids, dropping each ref.
 * Must be called before client_put() drops the last ref during expiry.
 */
void client_remove_all_stateids(struct client *client);

#endif /* _REFFS_CLIENT_H */
