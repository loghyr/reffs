/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * Chunk store — in-memory per-block metadata for CHUNK operations.
 *
 * Each inode on a data server can have an associated chunk_store that
 * tracks the state of blocks written via CHUNK_WRITE.  The store is
 * a dynamically-sized array indexed by block offset.
 *
 * Thread safety: callers must hold the inode's i_attr_mutex.
 */

#include "nfs4/chunk_store.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "reffs/inode.h"

/* Initial allocation: 64 blocks.  Grows by doubling. */
#define CHUNK_STORE_INIT_BLOCKS 64

struct chunk_store *chunk_store_get(struct inode *inode)
{
	if (inode->i_chunk_store)
		return inode->i_chunk_store;

	struct chunk_store *cs = calloc(1, sizeof(*cs));

	if (!cs)
		return NULL;

	cs->cs_blocks = calloc(CHUNK_STORE_INIT_BLOCKS, sizeof(*cs->cs_blocks));
	if (!cs->cs_blocks) {
		free(cs);
		return NULL;
	}

	cs->cs_nblocks = CHUNK_STORE_INIT_BLOCKS;
	inode->i_chunk_store = cs;
	return cs;
}

struct chunk_block *chunk_store_lookup(struct chunk_store *cs, uint64_t offset)
{
	if (!cs || offset >= cs->cs_nblocks)
		return NULL;

	struct chunk_block *blk = &cs->cs_blocks[offset];

	if (blk->cb_state == CHUNK_STATE_EMPTY)
		return NULL;

	return blk;
}

/*
 * Grow the block array to hold at least new_cap entries.
 */
static int chunk_store_grow(struct chunk_store *cs, uint64_t new_cap)
{
	uint64_t cap = cs->cs_nblocks;

	if (new_cap > 1024 * 1024)
		return -ENOMEM;

	while (cap <= new_cap)
		cap *= 2;

	struct chunk_block *nb = calloc((size_t)cap, sizeof(*nb));

	if (!nb)
		return -ENOMEM;

	memcpy(nb, cs->cs_blocks, cs->cs_nblocks * sizeof(*nb));
	free(cs->cs_blocks);
	cs->cs_blocks = nb;
	cs->cs_nblocks = (uint32_t)cap;
	return 0;
}

int chunk_store_write(struct chunk_store *cs, uint64_t offset,
		      const struct chunk_block *blk)
{
	if (offset >= cs->cs_nblocks) {
		int ret = chunk_store_grow(cs, offset);

		if (ret)
			return ret;
	}

	cs->cs_blocks[offset] = *blk;
	return 0;
}

int chunk_store_transition(struct chunk_store *cs, uint64_t offset,
			   uint32_t count, uint32_t owner_id,
			   enum chunk_state from_state,
			   enum chunk_state to_state)
{
	for (uint32_t i = 0; i < count; i++) {
		uint64_t off = offset + i;

		if (off >= cs->cs_nblocks)
			return -EINVAL;

		struct chunk_block *blk = &cs->cs_blocks[off];

		if (blk->cb_state != from_state)
			return -EINVAL;
		if (blk->cb_owner_id != owner_id)
			return -EINVAL;

		blk->cb_state = to_state;
	}

	return 0;
}

void chunk_store_destroy(struct chunk_store *cs)
{
	if (!cs)
		return;
	free(cs->cs_blocks);
	free(cs);
}
