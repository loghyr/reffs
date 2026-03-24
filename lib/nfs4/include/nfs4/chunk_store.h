/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * Chunk store — per-block metadata for CHUNK operations.
 *
 * Tracks the lifecycle state of each block in a data file managed
 * by CHUNK_WRITE / CHUNK_FINALIZE / CHUNK_COMMIT.  Block data is
 * stored in the inode's data_block via pwrite at offset * chunk_size;
 * this layer manages only the per-block metadata.
 *
 * For the happy-path demo, metadata is in-memory only.
 */

#ifndef NFS4_CHUNK_STORE_H
#define NFS4_CHUNK_STORE_H

#include <stdint.h>

/* Forward declarations — full definitions in nfsv42_xdr.h. */
struct inode;

enum chunk_state {
	CHUNK_STATE_EMPTY = 0,
	CHUNK_STATE_PENDING = 1,
	CHUNK_STATE_FINALIZED = 2,
	CHUNK_STATE_COMMITTED = 3,
};

struct chunk_block {
	enum chunk_state cb_state;
	uint32_t cb_gen_id; /* from chunk_guard4.cg_gen_id */
	uint32_t cb_client_id; /* from chunk_guard4.cg_client_id */
	uint32_t cb_owner_id; /* from chunk_owner4.co_id */
	uint32_t cb_payload_id;
	uint32_t cb_crc32;
	uint32_t cb_chunk_size;
};

/*
 * In-memory chunk store for an inode.  Grows on demand as blocks
 * are written.  Protected by the inode's i_attr_mutex.
 */
struct chunk_store {
	struct chunk_block *cs_blocks;
	uint32_t cs_nblocks; /* allocated entries */
};

/*
 * chunk_store_get -- get or create the chunk store for an inode.
 * Returns NULL on allocation failure.
 */
struct chunk_store *chunk_store_get(struct inode *inode);

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
 * chunk_store_destroy -- free the chunk store.
 */
void chunk_store_destroy(struct chunk_store *cs);

#endif /* NFS4_CHUNK_STORE_H */
