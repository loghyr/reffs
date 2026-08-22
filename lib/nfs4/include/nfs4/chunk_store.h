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

#include <assert.h>
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

/* Per-block flags (cb_flags).  Wire format: chrr_locked (and cr_locked
 * in read_chunk4) is chunk_state_flags4<> -- a bit-field typedef
 * whose only defined bit today is CHUNK_STATE_FLAGS_LOCKED (0x1).
 * In-memory the same bit lives in cb_flags; future flag bits fit
 * without another XDR revision. */
#define CHUNK_BLOCK_LOCKED 0x1
/*
 * Set by OP_CHUNK_WRITE_REPAIR on every block it touches; persisted
 * via cbd_flags.  Purely informational (operator audit trail for
 * "this block was the result of an EC repair, not a normal write");
 * does not gate any state transition.  See
 * .claude/design/ec-repair.md sec 2 (Option C state-machine
 * choice).
 */
#define CHUNK_BLOCK_REPAIR_PROVENANCE 0x2
/* Set by CHUNK_ERROR until a repair is confirmed. */
#define CHUNK_BLOCK_ERROR 0x4
/* Set on a block held by the metadata-server escrow owner. */
#define CHUNK_BLOCK_ESCROW 0x8

/* Persistent lock identity fields reserved for the CHUNK_LOCK seam. */
#define CHUNK_LOCK_STATEID_SIZE 16
#define CHUNK_LOCK_ESCROW_ID_SIZE 16

/*
 * CHUNK_VALUE_MAX is sized for the largest supported checksum (SHA512,
 * 64 bytes).  CRC32 / CRC32C / FLETCHER4 use 4-8 of the 64 bytes; the
 * rest is zero-padded.  The wasted space is acceptable for the Phase 1
 * fixed-size record format; a variable-length payload area can replace
 * it when storage density warrants it.
 */
#define CHUNK_VALUE_MAX 64

struct chunk_block {
	enum chunk_state cb_state;
	uint32_t cb_flags; /* CHUNK_BLOCK_LOCKED, etc. */
	/*
	 * chunk_guard4.cg_gen_id -- the DATA SERVER's per-chunk monotonic
	 * generation counter, not a client-supplied value.  Starts at 0
	 * when the chunk is first written and increments on each
	 * successful CHUNK_WRITE by any client (draft-haynes-nfsv4-
	 * flexfiles-v2 sec-chunk_guard4).  The client's cwa_guard carries
	 * only the value it EXPECTS to find, which the CAS compares
	 * against this.
	 */
	uint32_t cb_gen_id;
	/*
	 * The layout-granted writer identity.  Appears on the wire twice
	 * -- as chunk_owner4.co_client_id and chunk_guard4.cg_client_id --
	 * which the draft calls redundant carriers of the same value so
	 * that cohort records and CAS state are each self-contained.
	 */
	uint32_t cb_client_id;
	uint64_t cb_cohort_id; /* chunk_owner4.co_cohort_id */
	uint32_t cb_owner_id; /* chunk_owner4.co_id */
	uint32_t cb_payload_id;
	/*
	 * Checksum carried with the chunk on the wire and persisted with
	 * the metadata.  cb_checksum_algorithm is one of CHECKSUM_ALG_*;
	 * cb_checksum_value holds the algorithm's raw bytes (big-endian
	 * for fixed-width integers, opaque for hashes) with cb_checksum_len
	 * bytes valid.  Length 0 means "no checksum supplied" (legacy /
	 * untrusted writer path).
	 */
	uint32_t cb_checksum_algorithm;
	uint32_t cb_checksum_len;
	uint8_t cb_checksum_value[CHUNK_VALUE_MAX];
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
	/* CHUNK_LOCK and metadata-server escrow state. */
	uint64_t cb_lock_cohort_id;
	uint32_t cb_lock_client_id;
	uint32_t cb_lock_owner_id;
	uint64_t cb_lock_offset;
	uint32_t cb_lock_count;
	uint32_t cb_lock_flags;
	uint8_t cb_lock_stateid[CHUNK_LOCK_STATEID_SIZE];
	uint8_t cb_lock_escrow_id[CHUNK_LOCK_ESCROW_ID_SIZE];
};

/*
 * On-disk format -- fixed-size records for crash-safe persistence.
 * Same field layout as chunk_block but with explicit uint32_t types
 * (no enum) for binary stability.
 */

#define CHUNK_STORE_MAGIC 0x434B5354 /* "CKST" */
#define CHUNK_STORE_VERSION 2

struct chunk_store_header {
	uint32_t csh_magic;
	uint32_t csh_version;
	uint64_t csh_nblocks; /* number of block entries that follow */
	uint64_t csh_inode_ino; /* owning inode number */
	uint32_t csh_chunk_size; /* nominal chunk size (disk stride) */
	/*
	 * Pending Change 6 step 8: per-file checksum algorithm (see
	 * struct chunk_store.cs_checksum_algorithm).  Reused csh_pad
	 * slot -- no version bump per CLAUDE.md "Deployment Status:
	 * No persistent storage has been deployed".
	 */
	uint32_t csh_checksum_algorithm;
};

struct chunk_block_disk {
	uint32_t cbd_state;
	uint32_t cbd_flags;
	uint32_t cbd_gen_id;
	uint32_t cbd_client_id;
	uint32_t cbd_owner_id;
	uint32_t cbd_payload_id;
	uint32_t cbd_checksum_algorithm; /* CHECKSUM_ALG_* */
	uint32_t cbd_checksum_len; /* bytes valid in cbd_checksum_value */
	uint8_t cbd_checksum_value[CHUNK_VALUE_MAX];
	uint32_t cbd_chunk_size;
	uint32_t cbd_pad; /* keep 8-byte alignment for the u64s below */
	uint64_t cbd_writer_clientid; /* see chunk_block.cb_writer_clientid */
	/*
	 * chunk_owner4.co_cohort_id.  Appended rather than reusing
	 * cbd_pad because it is 64-bit.  No CHUNK_STORE_VERSION bump and
	 * no migration code: per CLAUDE.md "Deployment Status", no
	 * persistent storage has been deployed and all on-disk formats
	 * are version 1.  Re-read that section before assuming this still
	 * holds -- once a deployment with persistent data ships, changes
	 * here need a version bump plus migration.
	 */
	uint64_t cbd_cohort_id;
	uint64_t cbd_lock_cohort_id;
	uint32_t cbd_lock_client_id;
	uint32_t cbd_lock_owner_id;
	uint64_t cbd_lock_offset;
	uint32_t cbd_lock_count;
	uint32_t cbd_lock_flags;
	uint8_t cbd_lock_stateid[CHUNK_LOCK_STATEID_SIZE];
	uint8_t cbd_lock_escrow_id[CHUNK_LOCK_ESCROW_ID_SIZE];
};

/*
 * chunk_store_load validates magic, version, inode and block count,
 * but nothing tells it the record size it was written with -- a file
 * written by a build with a different sizeof() parses as garbage and
 * the store is silently re-created empty.  Pin the size so the next
 * change to this struct is a compile error here rather than a silent
 * data loss on somebody's soak host.  Changing this number means
 * every existing per-inode file under <state_dir>/chunks must be
 * cleared.
 */
static_assert(sizeof(struct chunk_block_disk) == 184,
	      "chunk_block_disk size changed -- on-disk chunk metadata "
	      "written by an older build will misparse; clear "
	      "<state_dir>/chunks before running, then update this size");

/*
 * In-memory chunk store for an inode.  Grows on demand as blocks
 * are written.  Protected by the inode's i_attr_mutex.
 */
struct chunk_store {
	struct chunk_block *cs_blocks;
	uint64_t cs_nblocks; /* allocated entries */
	uint64_t cs_high_water; /* highest offset written + 1 */
	uint32_t cs_chunk_size; /* nominal chunk size (disk stride) */
	/*
	 * Pending Change 6 step 8: the file's checksum algorithm,
	 * captured on first CHUNK_WRITE.  Subsequent CHUNK_WRITE /
	 * CHUNK_WRITE_REPAIR with a wire cs_algorithm that does not
	 * match this value are rejected with NFS4ERR_INVAL.  Zero
	 * (CHECKSUM_ALG_NONE) means "no algorithm established yet";
	 * the next CHUNK_WRITE establishes it.  Persisted in
	 * chunk_store_header so a DS restart preserves the per-file
	 * policy.
	 */
	uint32_t cs_checksum_algorithm;
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
 * chunk_store_lookup_any -- return the allocated entry at offset,
 * including an EMPTY entry carrying a persisted CHUNK_LOCK.  The
 * ordinary lookup intentionally hides EMPTY entries from data-path
 * callers; lock management must still see a lock on an unwritten chunk.
 */
struct chunk_block *chunk_store_lookup_any(struct chunk_store *cs,
					   uint64_t offset);

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
 * Transitions blocks matching the owner triple at offsets
 * [offset, offset+count).
 * Returns 0 after transitioning the matching owner block, or -EINVAL if
 * no matching block exists or it is not in from_state.
 */
int chunk_store_transition(struct chunk_store *cs, uint64_t offset,
			   uint32_t count, uint64_t cohort_id,
			   uint32_t client_id, uint32_t owner_id,
			   enum chunk_state from_state,
			   enum chunk_state to_state);

/*
 * chunk_store_rollback -- transition PENDING and/or FINALIZED blocks
 * matching the owner triple to EMPTY at offsets [offset, offset+count).
 *
 * Implements the CHUNK_ROLLBACK protocol-op semantics from
 * draft-haynes-nfsv4-flexfiles-v2 fig-chunk-state-machine:
 *   PENDING   -> EMPTY   (discard PENDING)
 *   FINALIZED -> EMPTY   (discard FINALIZED)
 *   COMMITTED -> -ENOTSUP (repair-path; requires cg_gen_id handling
 *                          not implemented in this slice)
 *   EMPTY     -> skip    (no-op; sparse-rollback semantics)
 *
 * Returns 0 after removing the matching owner block, -EINVAL if no matching
 * block exists, or -ENOTSUP if a matching block is COMMITTED.
 */
int chunk_store_rollback(struct chunk_store *cs, uint64_t offset,
			 uint32_t count, uint64_t cohort_id, uint32_t client_id,
			 uint32_t owner_id);

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
 * chunk_store_clear -- discard all per-chunk metadata for a fresh file
 * incarnation (for example, SETATTR size=0/O_TRUNC).  The caller owns
 * the store and must hold the inode's i_attr_mutex.
 */
void chunk_store_clear(struct chunk_store *cs);

/*
 * chunk_store_destroy -- free the chunk store.
 */
void chunk_store_destroy(struct chunk_store *cs);

/*
 * chunk_store_count_runs -- count contiguous runs of non-EMPTY
 * blocks separated by EMPTY gaps.
 *
 * INV-1 fragmentation measurement (see
 * .claude/design/inv1-ds-instrumentation.md).  A defragmented
 * file is one run; a shared-file workload with interleaved
 * writes from multiple writers produces many more.
 *
 * Caller must hold the owning inode's i_attr_mutex (matches the
 * convention for every other chunk_store_* function -- the cs
 * array can be grown by chunk_store_write, so a lock-free reader
 * would race a concurrent grow).
 *
 * Returns the run count.  An empty store returns 0.
 */
uint64_t chunk_store_count_runs(const struct chunk_store *cs);

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
