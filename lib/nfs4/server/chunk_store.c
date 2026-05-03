/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * Chunk store -- per-block metadata for CHUNK operations.
 *
 * Each inode on a data server can have an associated chunk_store that
 * tracks the state of blocks written via CHUNK_WRITE.  The store is
 * a dynamically-sized array indexed by block offset.
 *
 * Persistence: metadata is written to <state_dir>/chunks/<ino>.meta
 * using write-temp/fdatasync/rename.  Only non-EMPTY blocks are
 * written (sparse on disk).  On load, the file is read and the
 * in-memory array is populated.
 *
 * Thread safety: callers must hold the inode's i_attr_mutex.
 */

#include "nfs4/chunk_store.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "reffs/inode.h"
#include "reffs/log.h"
#include "reffs/posix_shims.h"

/* Initial allocation: 64 blocks.  Grows by doubling. */
#define CHUNK_STORE_INIT_BLOCKS 64
#define CHUNK_STORE_MAX_BLOCKS (1024 * 1024)

/* ------------------------------------------------------------------ */
/* Path helpers                                                        */
/* ------------------------------------------------------------------ */

static int chunk_meta_path(char *buf, size_t bufsz, const char *state_dir,
			   uint64_t inode_ino)
{
	int n = snprintf(buf, bufsz, "%s/chunks/%" PRIu64 ".meta", state_dir,
			 inode_ino);

	if (n < 0 || (size_t)n >= bufsz)
		return -ENAMETOOLONG;
	return 0;
}

static int ensure_chunks_dir(const char *state_dir)
{
	char dir[512];
	int n = snprintf(dir, sizeof(dir), "%s/chunks", state_dir);

	if (n < 0 || (size_t)n >= sizeof(dir))
		return -ENAMETOOLONG;
	if (mkdir(dir, 0700) && errno != EEXIST)
		return -errno;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Disk format conversion                                              */
/* ------------------------------------------------------------------ */

static void block_to_disk(const struct chunk_block *blk,
			  struct chunk_block_disk *dsk)
{
	dsk->cbd_state = (uint32_t)blk->cb_state;
	dsk->cbd_flags = blk->cb_flags;
	dsk->cbd_gen_id = blk->cb_gen_id;
	dsk->cbd_client_id = blk->cb_client_id;
	dsk->cbd_owner_id = blk->cb_owner_id;
	dsk->cbd_payload_id = blk->cb_payload_id;
	dsk->cbd_crc32 = blk->cb_crc32;
	dsk->cbd_chunk_size = blk->cb_chunk_size;
}

static void disk_to_block(const struct chunk_block_disk *dsk,
			  struct chunk_block *blk)
{
	blk->cb_state = (enum chunk_state)dsk->cbd_state;
	blk->cb_flags = dsk->cbd_flags;
	blk->cb_gen_id = dsk->cbd_gen_id;
	blk->cb_client_id = dsk->cbd_client_id;
	blk->cb_owner_id = dsk->cbd_owner_id;
	blk->cb_payload_id = dsk->cbd_payload_id;
	blk->cb_crc32 = dsk->cbd_crc32;
	blk->cb_chunk_size = dsk->cbd_chunk_size;
}

/* ------------------------------------------------------------------ */
/* In-memory operations                                                */
/* ------------------------------------------------------------------ */

struct chunk_store *chunk_store_get(struct inode *inode, const char *state_dir)
{
	if (inode->i_chunk_store)
		return inode->i_chunk_store;

	/* Try loading persisted metadata from disk. */
	if (state_dir) {
		struct chunk_store *cs =
			chunk_store_load(state_dir, inode->i_ino);
		if (cs) {
			inode->i_chunk_store = cs;
			return cs;
		}
	}

	/* No persisted state -- create a fresh in-memory store. */
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

	if (new_cap > CHUNK_STORE_MAX_BLOCKS)
		return -ENOMEM;

	if (cap == 0)
		cap = CHUNK_STORE_INIT_BLOCKS;

	while (cap <= new_cap)
		cap *= 2;

	struct chunk_block *nb = calloc((size_t)cap, sizeof(*nb));

	if (!nb)
		return -ENOMEM;

	memcpy(nb, cs->cs_blocks, cs->cs_nblocks * sizeof(*nb));
	free(cs->cs_blocks);
	cs->cs_blocks = nb;
	cs->cs_nblocks = cap;
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
	if (offset + 1 > cs->cs_high_water)
		cs->cs_high_water = offset + 1;
	cs->cs_dirty = true;
	return 0;
}

int chunk_store_transition(struct chunk_store *cs, uint64_t offset,
			   uint32_t count, uint32_t owner_id,
			   enum chunk_state from_state,
			   enum chunk_state to_state)
{
	uint32_t ntransitioned = 0;

	for (uint32_t i = 0; i < count; i++) {
		uint64_t off = offset + i;

		if (off >= cs->cs_nblocks)
			return -EINVAL;

		struct chunk_block *blk = &cs->cs_blocks[off];

		/*
		 * Skip EMPTY blocks in the requested range.  Codecs with
		 * variable-size shards (Mojette systematic; any future
		 * projection codec) write sparsely: a data shard may
		 * write 1 block per stripe while the largest parity
		 * shard writes 4, leaving 3 holes per stripe in the data
		 * shard's file.  FINALIZE / COMMIT span the full nominal
		 * range and must tolerate the holes -- there is nothing
		 * to transition for an EMPTY block.  Other state
		 * mismatches (e.g. COMMIT on PENDING without an
		 * intervening FINALIZE) remain hard errors so the state
		 * machine stays monotonic.
		 */
		if (blk->cb_state == CHUNK_STATE_EMPTY)
			continue;
		if (blk->cb_state != from_state)
			return -EINVAL;
		if (blk->cb_owner_id != owner_id)
			return -EINVAL;

		blk->cb_state = to_state;
		ntransitioned++;
	}

	/*
	 * Only mark the store dirty if we actually moved at least one
	 * block.  An all-EMPTY range (e.g. a sparse-writing shard whose
	 * stride is 1 block per stripe, finalised at a stage where no
	 * blocks have yet been written) is a legitimate no-op and must
	 * not trigger a chunk_store_persist meta-file rewrite.
	 */
	if (ntransitioned > 0)
		cs->cs_dirty = true;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Persistence                                                         */
/* ------------------------------------------------------------------ */

int chunk_store_persist(struct chunk_store *cs, const char *state_dir,
			uint64_t inode_ino)
{
	char path[512], tmp[520];
	int ret;

	if (!cs || !cs->cs_dirty)
		return 0;

	ret = ensure_chunks_dir(state_dir);
	if (ret)
		return ret;

	ret = chunk_meta_path(path, sizeof(path), state_dir, inode_ino);
	if (ret)
		return ret;

	if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp))
		return -ENAMETOOLONG;

	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);

	if (fd < 0) {
		LOG("chunk_store_persist: open(%s): %m", tmp);
		return -errno;
	}

	/* Write header. */
	struct chunk_store_header hdr = {
		.csh_magic = CHUNK_STORE_MAGIC,
		.csh_version = CHUNK_STORE_VERSION,
		.csh_nblocks = cs->cs_high_water,
		.csh_inode_ino = inode_ino,
		.csh_chunk_size = cs->cs_chunk_size,
	};
	ssize_t n = write(fd, &hdr, sizeof(hdr));

	if (n != (ssize_t)sizeof(hdr)) {
		LOG("chunk_store_persist: header write: %m");
		ret = n < 0 ? -errno : -EIO;
		goto err_close;
	}

	/* Write block entries up to high_water. */
	for (uint64_t i = 0; i < cs->cs_high_water; i++) {
		struct chunk_block_disk dsk;

		block_to_disk(&cs->cs_blocks[i], &dsk);
		n = write(fd, &dsk, sizeof(dsk));
		if (n != (ssize_t)sizeof(dsk)) {
			LOG("chunk_store_persist: block %" PRIu64 " write: %m",
			    i);
			ret = n < 0 ? -errno : -EIO;
			goto err_close;
		}
	}

	if (reffs_fdatasync(fd)) {
		LOG("chunk_store_persist: fdatasync(%s): %m", tmp);
		ret = -errno;
		goto err_close;
	}

	close(fd);
	fd = -1;

	if (rename(tmp, path)) {
		LOG("chunk_store_persist: rename(%s, %s): %m", tmp, path);
		ret = -errno;
		goto err_unlink;
	}

	cs->cs_dirty = false;
	return 0;

err_close:
	close(fd);
err_unlink:
	unlink(tmp);
	return ret;
}

struct chunk_store *chunk_store_load(const char *state_dir, uint64_t inode_ino)
{
	char path[512];

	if (chunk_meta_path(path, sizeof(path), state_dir, inode_ino))
		return NULL;

	int fd = open(path, O_RDONLY);

	if (fd < 0)
		return NULL;

	struct chunk_store_header hdr;
	ssize_t n = read(fd, &hdr, sizeof(hdr));

	if (n != (ssize_t)sizeof(hdr) || hdr.csh_magic != CHUNK_STORE_MAGIC ||
	    hdr.csh_version != CHUNK_STORE_VERSION ||
	    hdr.csh_inode_ino != inode_ino ||
	    hdr.csh_nblocks > CHUNK_STORE_MAX_BLOCKS) {
		TRACE("chunk_store_load: bad header for ino %" PRIu64,
		      inode_ino);
		close(fd);
		return NULL;
	}

	uint64_t nblocks = hdr.csh_nblocks;
	uint64_t alloc = nblocks;

	/* Round up to INIT_BLOCKS minimum. */
	if (alloc < CHUNK_STORE_INIT_BLOCKS)
		alloc = CHUNK_STORE_INIT_BLOCKS;

	struct chunk_store *cs = calloc(1, sizeof(*cs));

	if (!cs) {
		close(fd);
		return NULL;
	}

	cs->cs_blocks = calloc((size_t)alloc, sizeof(*cs->cs_blocks));
	if (!cs->cs_blocks) {
		free(cs);
		close(fd);
		return NULL;
	}

	cs->cs_nblocks = alloc;
	cs->cs_high_water = nblocks;
	cs->cs_chunk_size = hdr.csh_chunk_size;

	for (uint64_t i = 0; i < nblocks; i++) {
		struct chunk_block_disk dsk;

		n = read(fd, &dsk, sizeof(dsk));
		if (n != (ssize_t)sizeof(dsk)) {
			TRACE("chunk_store_load: short read at block %" PRIu64,
			      i);
			chunk_store_destroy(cs);
			close(fd);
			return NULL;
		}
		disk_to_block(&dsk, &cs->cs_blocks[i]);
	}

	close(fd);
	cs->cs_dirty = false;
	return cs;
}

void chunk_store_destroy(struct chunk_store *cs)
{
	if (!cs)
		return;
	free(cs->cs_blocks);
	free(cs);
}
