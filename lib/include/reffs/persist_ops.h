/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * Persistence operations vtable.
 *
 * Abstracts server-wide persistent storage (server state, client
 * identity/incarnation, sb registry, identity domains/mappings, NSM).
 * Two implementations:
 *   - flatfile: existing write-temp/fdatasync/rename pattern (POSIX/RAM)
 *   - rocksdb: namespace RocksDB database (future)
 *
 * The active implementation is selected at server_state_init() based
 * on the configured backend type.
 */

#ifndef _REFFS_PERSIST_OPS_H
#define _REFFS_PERSIST_OPS_H

#include <stdint.h>
#include <stddef.h>

struct server_persistent_state;
struct client_identity_record;
struct client_incarnation_record;

struct persist_ops {
	/* Server state */
	int (*server_state_save)(void *ctx,
				 const struct server_persistent_state *sps);
	int (*server_state_load)(void *ctx,
				 struct server_persistent_state *sps);

	/* SB registry */
	int (*registry_save)(void *ctx);
	int (*registry_load)(void *ctx);
	uint64_t (*registry_alloc_id)(void *ctx);

	/* Client identity (write-once per slot) */
	int (*client_identity_append)(void *ctx,
				      const struct client_identity_record *cir);
	int (*client_identity_load)(
		void *ctx,
		int (*cb)(const struct client_identity_record *cir, void *arg),
		void *arg);

	/* Client incarnations (current active clients) */
	int (*client_incarnation_add)(
		void *ctx, const struct client_incarnation_record *crc);
	int (*client_incarnation_remove)(void *ctx, uint32_t slot);
	int (*client_incarnation_load)(void *ctx,
				       struct client_incarnation_record *recs,
				       size_t max_recs, size_t *count);

	/* Cleanup */
	void (*fini)(void *ctx);
};

/*
 * Flatfile implementation — wraps existing persistence functions.
 * ctx is a strdup'd state_dir path.
 */
const struct persist_ops *flatfile_persist_ops_get(void);

/*
 * RocksDB namespace implementation — opens <state_dir>/namespace.rocksdb/.
 * On success, sets *ops_out and *ctx_out.  Returns 0 or -errno.
 * Only available when HAVE_ROCKSDB is defined.
 */
#ifdef HAVE_ROCKSDB
int rocksdb_namespace_init(const char *state_dir,
			   const struct persist_ops **ops_out, void **ctx_out);
#endif

#endif /* _REFFS_PERSIST_OPS_H */
