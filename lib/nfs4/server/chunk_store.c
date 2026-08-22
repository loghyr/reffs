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
#include "nfs4/chunk_epoch.h"

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

static bool chunk_block_has_escrow_custody(const struct chunk_block *blk)
{
	static const uint8_t zero[CHUNK_LOCK_ESCROW_ID_SIZE];

	return (blk->cb_flags & CHUNK_BLOCK_LOCKED) &&
	       memcmp(blk->cb_lock_escrow_id, zero, sizeof(zero)) != 0;
}

/* Initial allocation: 64 blocks.  Grows by doubling. */
#define CHUNK_STORE_INIT_BLOCKS 64
#define CHUNK_STORE_MAX_BLOCKS (1024 * 1024)

static void escrow_range_to_disk(const struct chunk_escrow_range *range,
				 struct chunk_escrow_range_disk *dsk)
{
	dsk->cerd_offset = range->cer_offset;
	dsk->cerd_count = range->cer_count;
	dsk->cerd_pad = 0;
	memcpy(dsk->cerd_id, range->cer_id, sizeof(dsk->cerd_id));
}

static void escrow_range_from_disk(const struct chunk_escrow_range_disk *dsk,
				   struct chunk_escrow_range *range)
{
	range->cer_offset = dsk->cerd_offset;
	range->cer_count = dsk->cerd_count;
	memcpy(range->cer_id, dsk->cerd_id, sizeof(range->cer_id));
}

static bool escrow_id_is_zero(const uint8_t id[CHUNK_LOCK_ESCROW_ID_SIZE])
{
	static const uint8_t zero[CHUNK_LOCK_ESCROW_ID_SIZE];

	return memcmp(id, zero, sizeof(zero)) == 0;
}

static bool escrow_range_valid(uint64_t offset, uint32_t count,
			       const uint8_t id[CHUNK_LOCK_ESCROW_ID_SIZE])
{
	return count != 0 && offset <= UINT64_MAX - count &&
	       !escrow_id_is_zero(id);
}

static bool escrow_range_equal(const struct chunk_escrow_range *a,
			       uint64_t offset, uint32_t count,
			       const uint8_t id[CHUNK_LOCK_ESCROW_ID_SIZE])
{
	return a->cer_offset == offset && a->cer_count == count &&
	       memcmp(a->cer_id, id, CHUNK_LOCK_ESCROW_ID_SIZE) == 0;
}

/* Rebuild the deduplicated range index from the authoritative block flags. */
int chunk_store_refresh_escrows(struct chunk_store *cs)
{
	struct chunk_escrow_range *ranges = NULL;
	uint32_t nranges = 0;
	uint32_t cap = 0;

	for (uint64_t off = 0; off < cs->cs_high_water; off++) {
		const struct chunk_block *blk = &cs->cs_blocks[off];

		if (!(blk->cb_flags & CHUNK_BLOCK_ESCROW))
			continue;
		if (!escrow_range_valid(blk->cb_lock_offset, blk->cb_lock_count,
					blk->cb_lock_escrow_id)) {
			free(ranges);
			return -EINVAL;
		}

		bool found = false;
		for (uint32_t i = 0; i < nranges; i++) {
			if (escrow_range_equal(&ranges[i], blk->cb_lock_offset,
					       blk->cb_lock_count,
					       blk->cb_lock_escrow_id)) {
				found = true;
				break;
			}
		}
		if (found)
			continue;
		if (nranges == CHUNK_STORE_MAX_ESCROWS) {
			free(ranges);
			return -E2BIG;
		}
		if (nranges == cap) {
			uint32_t new_cap = cap ? cap * 2 : 16;
			struct chunk_escrow_range *new_ranges;

			if (new_cap > CHUNK_STORE_MAX_ESCROWS)
				new_cap = CHUNK_STORE_MAX_ESCROWS;
			new_ranges = realloc(
				ranges, (size_t)new_cap * sizeof(*new_ranges));
			if (!new_ranges) {
				free(ranges);
				return -ENOMEM;
			}
			ranges = new_ranges;
			cap = new_cap;
		}
		ranges[nranges].cer_offset = blk->cb_lock_offset;
		ranges[nranges].cer_count = blk->cb_lock_count;
		memcpy(ranges[nranges].cer_id, blk->cb_lock_escrow_id,
		       sizeof(ranges[nranges].cer_id));
		nranges++;
	}

	free(cs->cs_escrows);
	cs->cs_escrows = ranges;
	cs->cs_nescrows = nranges;
	cs->cs_escrow_cap = cap;
	return 0;
}

bool chunk_store_has_escrow(const struct chunk_store *cs, uint64_t offset,
			    uint32_t count, const void *id)
{
	if (!cs || !escrow_range_valid(offset, count, id))
		return false;
	for (uint32_t i = 0; i < cs->cs_nescrows; i++) {
		if (escrow_range_equal(&cs->cs_escrows[i], offset, count, id))
			return true;
	}
	return false;
}

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

struct chunk_mds_epoch_disk {
	uint32_t magic;
	uint32_t version;
	struct chunk_mds_epoch value;
};

static int chunk_mds_epoch_path(char *buf, size_t bufsz, const char *state_dir)
{
	int n;

	if (!state_dir)
		return -EINVAL;
	n = snprintf(buf, bufsz, "%s/chunk_mds_epoch", state_dir);
	if (n < 0 || (size_t)n >= bufsz)
		return -ENAMETOOLONG;
	return 0;
}

int chunk_mds_epoch_load(const char *state_dir, struct chunk_mds_epoch *out)
{
	char path[512];
	struct chunk_mds_epoch_disk disk;
	int fd;
	ssize_t n;

	if (!out)
		return -EINVAL;
	if (chunk_mds_epoch_path(path, sizeof(path), state_dir))
		return -EINVAL;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -errno;
	n = read(fd, &disk, sizeof(disk));
	close(fd);
	if (n != (ssize_t)sizeof(disk) || disk.magic != CHUNK_MDS_EPOCH_MAGIC ||
	    disk.version != CHUNK_MDS_EPOCH_VERSION)
		return -EINVAL;

	*out = disk.value;
	return 0;
}

int chunk_mds_epoch_persist(const char *state_dir,
			    const struct chunk_mds_epoch *epoch)
{
	char path[512], tmp[520];
	struct chunk_mds_epoch_disk disk;
	int fd;
	ssize_t n;

	if (!epoch)
		return -EINVAL;
	disk.magic = CHUNK_MDS_EPOCH_MAGIC;
	disk.version = CHUNK_MDS_EPOCH_VERSION;
	disk.value = *epoch;
	if (chunk_mds_epoch_path(path, sizeof(path), state_dir))
		return -EINVAL;
	if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp))
		return -ENAMETOOLONG;

	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		return -errno;
	n = write(fd, &disk, sizeof(disk));
	if (n != (ssize_t)sizeof(disk)) {
		int ret = n < 0 ? -errno : -EIO;

		close(fd);
		unlink(tmp);
		return ret;
	}
	if (reffs_fdatasync(fd)) {
		int ret = -errno;

		close(fd);
		unlink(tmp);
		return ret;
	}
	if (close(fd)) {
		int ret = -errno;

		unlink(tmp);
		return ret;
	}
	if (rename(tmp, path)) {
		int ret = -errno;

		unlink(tmp);
		return ret;
	}
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
	dsk->cbd_cohort_id = blk->cb_cohort_id;
	dsk->cbd_payload_id = blk->cb_payload_id;
	dsk->cbd_checksum_algorithm = blk->cb_checksum_algorithm;
	dsk->cbd_checksum_len = blk->cb_checksum_len;
	memcpy(dsk->cbd_checksum_value, blk->cb_checksum_value,
	       sizeof(dsk->cbd_checksum_value));
	dsk->cbd_chunk_size = blk->cb_chunk_size;
	dsk->cbd_pad = 0;
	dsk->cbd_writer_clientid = blk->cb_writer_clientid;
	dsk->cbd_lock_cohort_id = blk->cb_lock_cohort_id;
	dsk->cbd_lock_client_id = blk->cb_lock_client_id;
	dsk->cbd_lock_owner_id = blk->cb_lock_owner_id;
	dsk->cbd_lock_offset = blk->cb_lock_offset;
	dsk->cbd_lock_count = blk->cb_lock_count;
	dsk->cbd_lock_flags = blk->cb_lock_flags;
	memcpy(dsk->cbd_lock_stateid, blk->cb_lock_stateid,
	       sizeof(dsk->cbd_lock_stateid));
	memcpy(dsk->cbd_lock_escrow_id, blk->cb_lock_escrow_id,
	       sizeof(dsk->cbd_lock_escrow_id));
}

static void disk_to_block(const struct chunk_block_disk *dsk,
			  struct chunk_block *blk)
{
	blk->cb_state = (enum chunk_state)dsk->cbd_state;
	blk->cb_flags = dsk->cbd_flags;
	blk->cb_gen_id = dsk->cbd_gen_id;
	blk->cb_client_id = dsk->cbd_client_id;
	blk->cb_owner_id = dsk->cbd_owner_id;
	blk->cb_cohort_id = dsk->cbd_cohort_id;
	blk->cb_payload_id = dsk->cbd_payload_id;
	blk->cb_checksum_algorithm = dsk->cbd_checksum_algorithm;
	blk->cb_checksum_len = dsk->cbd_checksum_len;
	if (blk->cb_checksum_len > sizeof(blk->cb_checksum_value))
		blk->cb_checksum_len = sizeof(blk->cb_checksum_value);
	memcpy(blk->cb_checksum_value, dsk->cbd_checksum_value,
	       sizeof(blk->cb_checksum_value));
	blk->cb_chunk_size = dsk->cbd_chunk_size;
	blk->cb_writer_clientid = dsk->cbd_writer_clientid;
	blk->cb_lock_cohort_id = dsk->cbd_lock_cohort_id;
	blk->cb_lock_client_id = dsk->cbd_lock_client_id;
	blk->cb_lock_owner_id = dsk->cbd_lock_owner_id;
	blk->cb_lock_offset = dsk->cbd_lock_offset;
	blk->cb_lock_count = dsk->cbd_lock_count;
	blk->cb_lock_flags = dsk->cbd_lock_flags;
	memcpy(blk->cb_lock_stateid, dsk->cbd_lock_stateid,
	       sizeof(blk->cb_lock_stateid));
	memcpy(blk->cb_lock_escrow_id, dsk->cbd_lock_escrow_id,
	       sizeof(blk->cb_lock_escrow_id));
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
	struct chunk_block *blk = chunk_store_lookup_any(cs, offset);

	if (!blk || blk->cb_state == CHUNK_STATE_EMPTY)
		return NULL;

	return blk;
}

struct chunk_block *chunk_store_lookup_any(struct chunk_store *cs,
					   uint64_t offset)
{
	if (!cs || offset >= cs->cs_nblocks)
		return NULL;

	return &cs->cs_blocks[offset];
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

int chunk_store_touch(struct chunk_store *cs, uint64_t offset)
{
	if (offset >= cs->cs_nblocks) {
		int ret = chunk_store_grow(cs, offset);

		if (ret)
			return ret;
	}
	if (offset + 1 > cs->cs_high_water)
		cs->cs_high_water = offset + 1;
	cs->cs_dirty = true;
	return 0;
}

int chunk_store_transition(struct chunk_store *cs, uint64_t offset,
			   uint32_t count, uint64_t cohort_id,
			   uint32_t client_id, uint32_t owner_id,
			   enum chunk_state from_state,
			   enum chunk_state to_state)
{
	for (uint32_t i = 0; i < count; i++) {
		uint64_t off = offset + i;

		if (off >= cs->cs_nblocks)
			return -EINVAL;

		struct chunk_block *blk = &cs->cs_blocks[off];

		if (blk->cb_state == CHUNK_STATE_EMPTY)
			continue;
		if (blk->cb_cohort_id != cohort_id ||
		    blk->cb_client_id != client_id ||
		    blk->cb_owner_id != owner_id)
			continue;
		if (blk->cb_state != from_state)
			return -EINVAL;

		blk->cb_state = to_state;
		cs->cs_dirty = true;
		return 0;
	}

	/* Lifecycle owners name a specific persisted chunk, not a range. */
	return -EINVAL;
}

int chunk_store_rollback(struct chunk_store *cs, uint64_t offset,
			 uint32_t count, uint64_t cohort_id, uint32_t client_id,
			 uint32_t owner_id)
{
	for (uint32_t i = 0; i < count; i++) {
		uint64_t off = offset + i;

		if (off >= cs->cs_nblocks)
			return -EINVAL;

		struct chunk_block *blk = &cs->cs_blocks[off];

		if (blk->cb_state == CHUNK_STATE_EMPTY)
			continue;

		if (blk->cb_cohort_id != cohort_id ||
		    blk->cb_client_id != client_id ||
		    blk->cb_owner_id != owner_id)
			continue;

		switch (blk->cb_state) {
		case CHUNK_STATE_PENDING:
		case CHUNK_STATE_FINALIZED:
			blk->cb_state = CHUNK_STATE_EMPTY;
			cs->cs_dirty = true;
			return 0;
		case CHUNK_STATE_COMMITTED:
			/*
			 * Repair-path: COMMITTED rollback requires cg_gen_id
			 * handling per draft-haynes-nfsv4-flexfiles-v2
			 * sec-CHUNK_ROLLBACK; not implemented in this slice.
			 * NOT_NOW_BROWN_COW.
			 */
			return -ENOTSUP;
		default:
			return -EINVAL;
		}
	}

	return -EINVAL;
}

uint32_t chunk_store_rollback_for_client(struct chunk_store *cs,
					 uint64_t writer_clientid)
{
	uint32_t ntransitioned = 0;

	if (!cs)
		return 0;

	for (uint64_t off = 0; off < cs->cs_nblocks; off++) {
		struct chunk_block *blk = &cs->cs_blocks[off];

		if (blk->cb_writer_clientid != writer_clientid)
			continue;
		/* An MDS escrow pins the payload and owner association. */
		if (blk->cb_flags & CHUNK_BLOCK_ESCROW)
			continue;
		if (chunk_block_has_escrow_custody(blk)) {
			/* Return adopted custody to the MDS on lease expiry. */
			blk->cb_flags |= CHUNK_BLOCK_ESCROW;
			blk->cb_lock_cohort_id = 0;
			blk->cb_lock_client_id = UINT32_MAX;
			blk->cb_lock_owner_id = 0;
			blk->cb_lock_flags = 0;
			memset(blk->cb_lock_stateid, 0,
			       sizeof(blk->cb_lock_stateid));
			blk->cb_writer_clientid = 0;
			ntransitioned++;
			continue;
		}

		/*
		 * Lease-driven cleanup: in-flight (PENDING) and
		 * finalised-but-uncommitted (FINALIZED) chunks owned by
		 * a dead writer are released back to EMPTY.  Already-
		 * COMMITTED chunks stay committed -- the writer's durable
		 * work survives its session expiry by design.
		 */
		if (blk->cb_state == CHUNK_STATE_PENDING ||
		    blk->cb_state == CHUNK_STATE_FINALIZED) {
			blk->cb_state = CHUNK_STATE_EMPTY;
			ntransitioned++;
		}
	}

	if (ntransitioned > 0)
		cs->cs_dirty = true;
	return ntransitioned;
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

	ret = chunk_store_refresh_escrows(cs);
	if (ret)
		return ret;

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
		.csh_checksum_algorithm = cs->cs_checksum_algorithm,
		.csh_escrow_count = cs->cs_nescrows,
		.csh_escrow_record_size =
			sizeof(struct chunk_escrow_range_disk),
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

	for (uint32_t i = 0; i < cs->cs_nescrows; i++) {
		struct chunk_escrow_range_disk dsk;

		escrow_range_to_disk(&cs->cs_escrows[i], &dsk);
		n = write(fd, &dsk, sizeof(dsk));
		if (n != (ssize_t)sizeof(dsk)) {
			LOG("chunk_store_persist: escrow range %u write: %m",
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
	    hdr.csh_nblocks > CHUNK_STORE_MAX_BLOCKS ||
	    hdr.csh_escrow_count > CHUNK_STORE_MAX_ESCROWS ||
	    hdr.csh_escrow_record_size !=
		    sizeof(struct chunk_escrow_range_disk)) {
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
	cs->cs_checksum_algorithm = hdr.csh_checksum_algorithm;
	cs->cs_nescrows = hdr.csh_escrow_count;
	cs->cs_escrow_cap = hdr.csh_escrow_count;
	if (cs->cs_escrow_cap) {
		cs->cs_escrows =
			calloc(cs->cs_escrow_cap, sizeof(*cs->cs_escrows));
		if (!cs->cs_escrows) {
			chunk_store_destroy(cs);
			close(fd);
			return NULL;
		}
	}

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

	for (uint32_t i = 0; i < cs->cs_nescrows; i++) {
		struct chunk_escrow_range_disk dsk;
		struct chunk_escrow_range *range = &cs->cs_escrows[i];

		n = read(fd, &dsk, sizeof(dsk));
		if (n != (ssize_t)sizeof(dsk)) {
			TRACE("chunk_store_load: short read at escrow range %u",
			      i);
			chunk_store_destroy(cs);
			close(fd);
			return NULL;
		}
		escrow_range_from_disk(&dsk, range);
		if (!escrow_range_valid(range->cer_offset, range->cer_count,
					range->cer_id) ||
		    range->cer_offset > nblocks ||
		    range->cer_count > nblocks - range->cer_offset) {
			TRACE("chunk_store_load: invalid escrow range %u", i);
			chunk_store_destroy(cs);
			close(fd);
			return NULL;
		}
	}

	close(fd);
	cs->cs_dirty = false;
	return cs;
}

void chunk_store_clear(struct chunk_store *cs)
{
	if (!cs)
		return;

	memset(cs->cs_blocks, 0, cs->cs_nblocks * sizeof(*cs->cs_blocks));
	free(cs->cs_escrows);
	cs->cs_escrows = NULL;
	cs->cs_nescrows = 0;
	cs->cs_escrow_cap = 0;
	cs->cs_high_water = 0;
	cs->cs_chunk_size = 0;
	cs->cs_checksum_algorithm = 0;
	cs->cs_dirty = true;
}

void chunk_store_destroy(struct chunk_store *cs)
{
	if (!cs)
		return;
	free(cs->cs_blocks);
	free(cs->cs_escrows);
	free(cs);
}

uint64_t chunk_store_count_runs(const struct chunk_store *cs)
{
	uint64_t runs = 0;
	bool in_run = false;

	if (!cs || cs->cs_nblocks == 0)
		return 0;

	for (uint64_t off = 0; off < cs->cs_nblocks; off++) {
		bool occupied = cs->cs_blocks[off].cb_state !=
				CHUNK_STATE_EMPTY;

		if (occupied && !in_run) {
			/* Transition EMPTY -> non-EMPTY starts a new run. */
			runs++;
			in_run = true;
		} else if (!occupied) {
			in_run = false;
		}
	}
	return runs;
}
