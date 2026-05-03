/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * Chunk store -- per-block metadata for CHUNK operations.
 *
 * Tracks the lifecycle state of each block in a data file managed
 * by CHUNK_WRITE / CHUNK_FINALIZE / CHUNK_COMMIT.  Block data is
 * stored in the inode's data_block via pwrite at offset * chunk_size;
 * this layer manages only the per-block metadata.
 *
 * Metadata is persisted to a per-inode file at
 * <state_dir>/chunks/<inode_ino>.meta using the standard
 * write-temp/fdatasync/rename pattern.  Persistence is triggered
 * on FINALIZE and COMMIT transitions (PENDING is transient).
 *
 * On-disk format: fixed-size header + array of chunk_block_disk
 * entries indexed by block offset.  Maps naturally to a RocksDB
 * key-value store (key = inode_ino:block_offset, value = block).
 */

#ifndef NFS4_CHUNK_STORE_H
#define NFS4_CHUNK_STORE_H

#include <stdbool.h>
#include <stdint.h>

/* Forward declarations. */
struct inode;

enum chunk_state {
	CHUNK_STATE_EMPTY = 0,
	CHUNK_STATE_PENDING = 1,
	CHUNK_STATE_FINALIZED = 2,
	CHUNK_STATE_COMMITTED = 3,
};

/* Per-block flags (cb_flags).  Wire format: chrr_locked is bool<>,
 * but in-memory we use a flags word for extensibility. */
#define CHUNK_BLOCK_LOCKED 0x1

struct chunk_block {
	enum chunk_state cb_state;
	uint32_t cb_flags; /* CHUNK_BLOCK_LOCKED, etc. */
	uint32_t cb_gen_id; /* chunk_guard4.cg_gen_id */
	uint32_t cb_client_id; /* chunk_guard4.cg_client_id (client-supplied) */
	uint32_t cb_owner_id; /* chunk_owner4.co_id */
	uint32_t cb_payload_id;
	uint32_t cb_crc32;
	uint32_t cb_chunk_size; /* actual payload size (varies for Mojette) */
	/*
	 * clientid4 of the writing session (server-known identity,
	 * distinct from cb_client_id which is the client-supplied
	 * chunk_guard4.cg_client_id).  Set on CHUNK_WRITE; used by
	 * nfs4_client_expire to roll back PENDING/FINALIZED chunks
	 * left orphaned by a dead writer (the lease-driven half of
	 * the chunk-state-machine liveness story per
	 * draft-haynes-nfsv4-flexfiles-v2 sec-system-model-consistency).
	 */
	uint64_t cb_writer_clientid;
};

/*
 * On-disk format -- fixed-size records for crash-safe persistence.
 * Same field layout as chunk_block but with explicit uint32_t types
 * (no enum) for binary stability.
 */

#define CHUNK_STORE_MAGIC 0x434B5354 /* "CKST" */
#define CHUNK_STORE_VERSION 1

struct chunk_store_header {
	uint32_t csh_magic;
	uint32_t csh_version;
	uint64_t csh_nblocks; /* number of block entries that follow */
	uint64_t csh_inode_ino; /* owning inode number */
	uint32_t csh_chunk_size; /* nominal chunk size (disk stride) */
	uint32_t csh_pad; /* align to 32 bytes */
};

struct chunk_block_disk {
	uint32_t cbd_state;
	uint32_t cbd_flags;
	uint32_t cbd_gen_id;
	uint32_t cbd_client_id;
	uint32_t cbd_owner_id;
	uint32_t cbd_payload_id;
	uint32_t cbd_crc32;
	uint32_t cbd_chunk_size;
	uint64_t cbd_writer_clientid; /* see chunk_block.cb_writer_clientid */
};

/*
 * In-memory chunk store for an inode.  Grows on demand as blocks
 * are written.  Protected by the inode's i_attr_mutex.
 */
struct chunk_store {
	struct chunk_block *cs_blocks;
	uint64_t cs_nblocks; /* allocated entries */
	uint64_t cs_high_water; /* highest offset written + 1 */
	uint32_t cs_chunk_size; /* nominal chunk size (disk stride) */
	bool cs_dirty; /* needs persistence */
};

/*
 * chunk_store_get -- get or create the chunk store for an inode.
 * If a persisted metadata file exists, loads it.
 * state_dir may be NULL (skip disk load, in-memory only).
 * Returns NULL on allocation failure.
 */
struct chunk_store *chunk_store_get(struct inode *inode, const char *state_dir);

/*
 * chunk_store_lookup -- look up a block at the given offset.
 * Returns NULL if the offset has not been written.
 */
struct chunk_block *chunk_store_lookup(struct chunk_store *cs, uint64_t offset);

/*
 * chunk_store_write -- record a chunk write at the given block offset.
 * Grows the store if needed.  Stores metadata; caller writes data
 * separately into the data_block.
 * Returns 0 on success, -ENOMEM on failure.
 */
int chunk_store_write(struct chunk_store *cs, uint64_t offset,
		      const struct chunk_block *blk);

/*
 * chunk_store_transition -- move blocks from one state to another.
 * Transitions blocks matching owner at offsets [offset, offset+count).
 * Returns 0 on success, -EINVAL if a block is not in from_state.
 */
int chunk_store_transition(struct chunk_store *cs, uint64_t offset,
			   uint32_t count, uint32_t owner_id,
			   enum chunk_state from_state,
			   enum chunk_state to_state);

/*
 * chunk_store_rollback -- transition PENDING and/or FINALIZED blocks
 * matching owner_id to EMPTY at offsets [offset, offset+count).
 *
 * Implements the CHUNK_ROLLBACK protocol-op semantics from
 * draft-haynes-nfsv4-flexfiles-v2 fig-chunk-state-machine:
 *   PENDING   -> EMPTY   (discard PENDING)
 *   FINALIZED -> EMPTY   (discard FINALIZED)
 *   COMMITTED -> -ENOTSUP (repair-path; requires cg_gen_id handling
 *                          not implemented in this slice)
 *   EMPTY     -> skip    (no-op; sparse-rollback semantics)
 *
 * Returns 0 on success, -EINVAL if a block has wrong owner_id,
 * -ENOTSUP if a matching block is COMMITTED.
 */
int chunk_store_rollback(struct chunk_store *cs, uint64_t offset,
			 uint32_t count, uint32_t owner_id);

/*
 * chunk_store_rollback_for_client -- sweep ALL blocks in PENDING or
 * FINALIZED state owned by writer_clientid, transitioning them to
 * EMPTY.  Used by the lease reaper when a client's lease expires.
 *
 * Differs from chunk_store_rollback (the protocol-op helper):
 *   - Matches on cb_writer_clientid, not cb_owner_id (clients pick
 *     arbitrary co_id values; clientid4 is the server-known identity
 *     the lease reaper has).
 *   - Skips COMMITTED silently (lease expiry recovers in-flight state
 *     only; already-committed work stays committed).
 *   - No range; sweeps the entire chunk_store.
 *
 * Returns the number of blocks transitioned (zero is legitimate --
 * the dead client may have had no in-flight chunks on this inode).
 */
uint32_t chunk_store_rollback_for_client(struct chunk_store *cs,
					 uint64_t writer_clientid);

/*
 * chunk_store_persist -- write metadata to disk.
 *
 * Path: <state_dir>/chunks/<inode_ino>.meta
 * Uses write-temp/fdatasync/rename for crash safety.
 * Called after FINALIZE and COMMIT transitions.
 *
 * state_dir: the server's ss_state_dir.
 * inode_ino: the owning inode's i_ino.
 * Returns 0 on success, negative errno on failure.
 */
int chunk_store_persist(struct chunk_store *cs, const char *state_dir,
			uint64_t inode_ino);

/*
 * chunk_store_load -- load metadata from disk into a new chunk_store.
 *
 * Returns a populated chunk_store on success, NULL if the file does
 * not exist or is corrupt (caller should create a fresh store).
 */
struct chunk_store *chunk_store_load(const char *state_dir, uint64_t inode_ino);

/*
 * chunk_store_destroy -- free the chunk store.
 */
void chunk_store_destroy(struct chunk_store *cs);

/*
 * chunk_rollback_for_client -- server-wide sweep.
 *
 * For every inode in every superblock, transition any PENDING or
 * FINALIZED chunk owned by writer_clientid to EMPTY.  Persists each
 * affected chunk_store to disk.
 *
 * Two-pass implementation per .claude/patterns/rcu-violations.md
 * Pattern 1: collect inode active-refs under rcu_read_lock; drop
 * the lock; then per-inode lock + chunk_store_rollback_for_client +
 * persist + unlock + drop ref.
 *
 * Called from nfs4_client_expire after the TRUST_STATEID bulk-revoke
 * to release in-flight chunks orphaned by the dying client (the
 * lease-driven half of the chunk-state-machine liveness story per
 * draft-haynes-nfsv4-flexfiles-v2 sec-system-model-consistency).
 *
 * state_dir: server's ss_state_dir (may be NULL to skip persist).
 * Returns total blocks rolled back across all inodes.
 */
uint32_t chunk_rollback_for_client(uint64_t writer_clientid,
				   const char *state_dir);

#endif /* NFS4_CHUNK_STORE_H */
