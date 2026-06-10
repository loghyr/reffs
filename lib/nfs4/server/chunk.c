/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * CHUNK operations -- data server block-level I/O.
 *
 * CHUNK_WRITE: store encoded chunks at a block offset, record
 * per-block metadata in the chunk store (PENDING state).
 *
 * CHUNK_FINALIZE: transition PENDING --> FINALIZED (visible to owner).
 *
 * CHUNK_COMMIT: transition FINALIZED --> COMMITTED (visible to all).
 *
 * CHUNK_READ: read committed (or owner's finalized) chunks.
 *
 * Happy-path implementation: no guard conflicts, no locks.
 */

#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <zlib.h>

#include "nfsv42_xdr.h"
#include "reffs/data_block.h"
#include "reffs/fs.h"
#include "reffs/inode.h"
#include "reffs/layout_segment.h"
#include "reffs/log.h"
#include "reffs/rpc.h"
#include "reffs/server.h"
#include "reffs/time.h"
#include "nfs4/chunk_checksum.h"
#include "nfs4/chunk_store.h"
#include "nfs4/client.h"
#include "nfs4/compound.h"
#include "nfs4/ops.h"
#include "nfs4/stateid.h"
#include "nfs4/trust_stateid.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void chunk_write_verf(struct server_state *ss, verifier4 out_verf)
{
	memcpy(out_verf, ss->ss_uuid, NFS4_VERIFIER_SIZE - 2);
	uint16_t boot_seq = server_boot_seq(ss);

	memcpy(out_verf + NFS4_VERIFIER_SIZE - 2, &boot_seq, 2);
}

/*
 * Reject client-supplied cg_client_id sentinels.  Per draft-haynes-
 * nfsv4-flexfiles-v2 sec-chunk_guard_none and sec-chunk_guard_mds,
 * clients MUST NOT present either reserved value, and the data
 * server MUST reject with NFS4ERR_INVAL.
 */
static inline bool chunk_cid_is_reserved(uint32_t cid)
{
	return cid == CHUNK_GUARD_CLIENT_ID_NONE ||
	       cid == CHUNK_GUARD_CLIENT_ID_MDS;
}

/*
 * chunk_write_validate_payload -- input + per-chunk-CRC validation
 * shared between OP_CHUNK_WRITE and OP_CHUNK_WRITE_REPAIR.
 *
 * Both ops carry the same payload shape (the cwa_/cwra_ XDR prefix
 * differs); this helper takes already-extracted fields so it is
 * struct-agnostic.  The per-op stateid / trust-table check stays
 * inline in each handler -- CHUNK_WRITE allows special stateids,
 * CHUNK_WRITE_REPAIR rejects them and additionally requires iomode
 * RW, so the auth shape differs even though the payload contract
 * does not.
 *
 * Validation rules (in order; first failure wins):
 *   1. current FH set                 -> NFS4ERR_NOFILEHANDLE
 *   2. current FH is a regular file   -> NFS4ERR_INVAL
 *   3. chunk_size > 0, chunks_len > 0 -> NFS4ERR_INVAL
 *   4. cg_client_id not reserved      -> NFS4ERR_INVAL
 *   5. nchunks > 0                    -> NFS4ERR_INVAL
 *   6. nchecksums == 0 || nchecksums == nchunks -> NFS4ERR_INVAL
 *   7. per-payload wire_algo is a known algorithm and every
 *      checksum4 matches it with the right cs_value length
 *                                     -> NFS4ERR_INVAL
 *   8. per-chunk CRC matches recomputed CRC of payload chunk
 *                                     -> NFS4ERR_INVAL
 *
 * Returns NFS4_OK on success and sets *out_wire_algo to the
 * per-payload checksum algorithm (0 if no checksums supplied) and
 * *out_nchunks to the computed chunk count.  On failure returns a
 * non-zero nfsstat4 (caller assigns to *status).  The op_log_tag
 * appears in any TRACE() line emitted for a rejection ("CHUNK_WRITE"
 * vs "CHUNK_WRITE_REPAIR") so log readers can attribute the
 * failure to the originating op.
 */
static nfsstat4
chunk_write_validate_payload(struct compound *compound, uint32_t chunk_size,
			     const char *chunks_data, uint32_t chunks_len,
			     uint32_t cg_client_id, const checksum4 *checksums,
			     uint32_t nchecksums, const char *op_log_tag,
			     uint32_t *out_wire_algo, uint32_t *out_nchunks)
{
	*out_wire_algo = 0;
	*out_nchunks = 0;

	if (network_file_handle_empty(&compound->c_curr_nfh))
		return NFS4ERR_NOFILEHANDLE;

	if (!compound->c_inode || !S_ISREG(compound->c_inode->i_mode))
		return NFS4ERR_INVAL;

	if (chunk_size == 0 || chunks_len == 0)
		return NFS4ERR_INVAL;

	if (chunk_cid_is_reserved(cg_client_id))
		return NFS4ERR_INVAL;

	/*
	 * The chunks opaque blob contains one or more chunks.  Most are
	 * chunk_size bytes; the last may be shorter when the shard size
	 * is not a multiple of chunk_size (Mojette parity projections
	 * produce variable-sized shards).
	 */
	uint32_t total_data = chunks_len;
	uint32_t nchunks = (total_data + chunk_size - 1) / chunk_size;

	if (nchunks == 0)
		return NFS4ERR_INVAL;

	if (nchecksums > 0 && nchecksums != nchunks)
		return NFS4ERR_INVAL;

	/*
	 * Validate per-chunk checksums if provided.  The wire type is
	 * checksum4; only CHECKSUM_ALG_CRC32 is supported by this
	 * implementation -- chunk_checksum_unpack_crc32() (below) rejects
	 * other algorithms or wrong cs_value lengths with NFS4ERR_INVAL.
	 *
	 * Step 8 pre-validation: every wire checksum4 uses the same
	 * algorithm and a value length that matches that algorithm's
	 * registered size.  Unknown algorithms, mixed-algorithm
	 * payloads, and wrong lengths are all NFS4ERR_INVAL.
	 *
	 * Per-file algorithm consistency (the second half of step 8)
	 * runs in the caller after chunk_store_get -- the chunk_store
	 * is the authoritative record of the file's established
	 * algorithm.
	 */
	uint32_t wire_algo = 0;

	if (nchecksums > 0) {
		wire_algo = checksums[0].cs_algorithm;
		int expected_value_len = chunk_checksum_expected_len(wire_algo);

		if (expected_value_len < 0) {
			TRACE("%s: unknown checksum algorithm %u", op_log_tag,
			      wire_algo);
			return NFS4ERR_INVAL;
		}
		for (uint32_t i = 0; i < nchecksums; i++) {
			const checksum4 *cs4 = &checksums[i];

			if (cs4->cs_algorithm != wire_algo) {
				TRACE("%s: mixed-algorithm payload "
				      "(entry %u: %u vs first %u)",
				      op_log_tag, i, cs4->cs_algorithm,
				      wire_algo);
				return NFS4ERR_INVAL;
			}
			if ((int)cs4->cs_value.cs_value_len !=
			    expected_value_len) {
				TRACE("%s: checksum length mismatch "
				      "(algo %u expects %d, got %u)",
				      op_log_tag, wire_algo, expected_value_len,
				      cs4->cs_value.cs_value_len);
				return NFS4ERR_INVAL;
			}
		}
	}

	for (uint32_t i = 0; i < nchecksums; i++) {
		uint32_t expected;
		nfsstat4 ust =
			chunk_checksum_unpack_crc32(&checksums[i], &expected);

		if (ust != NFS4_OK)
			return ust;

		const uint8_t *cdata =
			(const uint8_t *)chunks_data + (size_t)i * chunk_size;
		uint32_t clen = chunk_size;

		if (i == nchunks - 1 && total_data % chunk_size != 0)
			clen = total_data % chunk_size;
		uint32_t computed = (uint32_t)crc32(0L, cdata, (uInt)clen);

		if (computed != expected) {
			TRACE("%s: CRC mismatch chunk %u: "
			      "expected 0x%08x got 0x%08x",
			      op_log_tag, i, expected, computed);
			return NFS4ERR_INVAL;
		}
	}

	*out_wire_algo = wire_algo;
	*out_nchunks = nchunks;
	return NFS4_OK;
}

/* ------------------------------------------------------------------ */
/* CHUNK_WRITE                                                         */
/* ------------------------------------------------------------------ */

uint32_t nfs4_op_chunk_write(struct compound *compound)
{
	CHUNK_WRITE4args *args = NFS4_OP_ARG_SETUP(compound, opchunk_write);
	CHUNK_WRITE4res *res = NFS4_OP_RES_SETUP(compound, opchunk_write);
	nfsstat4 *status = &res->cwr_status;
	CHUNK_WRITE4resok *resok =
		NFS4_OP_RESOK_SETUP(res, CHUNK_WRITE4res_u, cwr_resok4);

	uint32_t wire_algo;
	uint32_t nchunks;
	nfsstat4 vs = chunk_write_validate_payload(
		compound, args->cwa_chunk_size, args->cwa_chunks.cwa_chunks_val,
		args->cwa_chunks.cwa_chunks_len,
		args->cwa_owner.co_guard.cg_client_id,
		args->cwa_checksums.cwa_checksums_val,
		args->cwa_checksums.cwa_checksums_len, "CHUNK_WRITE",
		&wire_algo, &nchunks);

	if (vs != NFS4_OK) {
		*status = vs;
		return 0;
	}

	uint32_t chunk_size = args->cwa_chunk_size;
	uint32_t total_data = args->cwa_chunks.cwa_chunks_len;
	uint32_t nchecksums = args->cwa_checksums.cwa_checksums_len;

	/*
	 * Trust table validation -- tightly-coupled DS.
	 *
	 * If the trust table is non-empty, validate that the stateid
	 * was registered by the MDS.  Special stateids (anonymous,
	 * read-bypass) bypass the check -- they are handled separately
	 * by the DS's own permission model.
	 */
	if (!stateid4_is_special(&args->cwa_stateid)) {
		struct trust_entry *te = trust_stateid_find(&args->cwa_stateid);

		if (!te) {
			*status = NFS4ERR_BAD_STATEID;
			return 0;
		}

		uint64_t now = reffs_now_ns();
		uint64_t exp = atomic_load_explicit(&te->te_expire_ns,
						    memory_order_acquire);
		uint32_t flags = atomic_load_explicit(&te->te_flags,
						      memory_order_acquire);

		if (flags & TRUST_PENDING) {
			trust_entry_put(te);
			*status = NFS4ERR_DELAY;
			return 0;
		}
		if (exp != 0 && now > exp) {
			trust_entry_put(te);
			*status = NFS4ERR_BAD_STATEID;
			return 0;
		}
		if (!(flags & TRUST_ACTIVE)) {
			trust_entry_put(te);
			*status = NFS4ERR_BAD_STATEID;
			return 0;
		}

		trust_entry_put(te);
	}

	pthread_mutex_lock(&compound->c_inode->i_attr_mutex);

	struct chunk_store *cs = chunk_store_get(
		compound->c_inode, compound->c_server_state->ss_state_dir);

	if (!cs) {
		pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);
		*status = NFS4ERR_DELAY;
		return 0;
	}

	/*
	 * Step 8 per-file consistency.  The first CHUNK_WRITE on a
	 * file establishes the algorithm; later writes must match it.
	 * A wire payload with no checksums (nchecksums == 0) leaves
	 * the file's policy untouched.  Mismatches are NFS4ERR_INVAL.
	 */
	if (nchecksums > 0) {
		if (cs->cs_checksum_algorithm == 0) {
			cs->cs_checksum_algorithm = wire_algo;
			cs->cs_dirty = true;
		} else if (cs->cs_checksum_algorithm != wire_algo) {
			TRACE("CHUNK_WRITE: per-file algorithm mismatch "
			      "(file=%u wire=%u)",
			      cs->cs_checksum_algorithm, wire_algo);
			pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);
			*status = NFS4ERR_INVAL;
			return 0;
		}
	}

	/* Record the nominal chunk size (disk stride for offset calc). */
	if (cs->cs_chunk_size == 0)
		cs->cs_chunk_size = chunk_size;

	/*
	 * Pull cstats up before the gate so the gate's reject path can
	 * bump cs_chunk_busy_delay.  The existing INV-1 increments below
	 * still see the same pointer.
	 */
	struct reffs_chunk_stats *cstats =
		compound->c_curr_sb ? &compound->c_curr_sb->sb_chunk_stats :
				      NULL;

	/*
	 * Track 1b chunk-collision gate (Option C, design/chunk-collision-
	 * validation.md).  Before committing any payload or metadata, scan
	 * the target range for an existing PENDING block owned by a different
	 * writer.  If we find one, refuse the write with NFS4ERR_DELAY -- the
	 * client retries the full RMW, picks up the prior writer's COMMITTED
	 * bytes on its CHUNK_READ, and re-encodes with fresh state.  Without
	 * this gate, the on-wire signature is "last-FINALIZE-wins": the
	 * second writer's CHUNK_WRITE payload (which encodes the prior writer's
	 * range as PRE-FILL because the second writer's RMW read happened too
	 * early) stomps the first writer's bytes.
	 *
	 * The check happens BEFORE data_block_write so we leave no half-
	 * applied payload on disk on the reject path; the check happens
	 * AFTER the cs lookup + algorithm-policy + chunk_size init because
	 * those are setup steps the prior writer also paid for, and skipping
	 * them on the reject path would diverge metadata between accept and
	 * reject paths.
	 *
	 * Per-owner identity matches on (co_id, cg_client_id) -- the wire-
	 * identifying tuple from chunk_owner4.co_guard plus co_id.  An
	 * idempotent retry from the SAME writer (same tuple) is allowed
	 * through; this is the same identity comparison
	 * cs_pending_displaced uses to count cross-writer displacement.
	 */
	/*
	 * Two independent rejection axes, both bump cs_chunk_busy_delay
	 * because both surface the same observer signal ("a writer was
	 * told to retry because the block is contended").
	 *
	 * (i)  PENDING from a different writer -- the writer is racing
	 *      ahead of another writer's still-in-flight write.
	 *
	 * (ii) cwa_guard.cwg_check == TRUE and the current block's
	 *      {cb_gen_id, cb_client_id} does not match cwg_guard --
	 *      CAS-style stale-RMW detection per draft-haynes-nfsv4-
	 *      flexfiles-v2 sec-write_chunk_guard4.  The writer
	 *      presented the version it read; the server enforces that
	 *      no concurrent writer has advanced the block since.  This
	 *      catches the case the PENDING gate misses: a prior writer
	 *      that already finished its full PENDING -> FINALIZED ->
	 *      COMMITTED cycle, after which the block looks "clean" to
	 *      a stale-RMW writer.
	 *
	 * Both checks fire BEFORE data_block_write so neither leaves
	 * half-applied payload on the reject path.  An EMPTY block
	 * with cwg_check=TRUE is allowed through with no version
	 * comparison -- a guarded write to a never-written block IS
	 * a first-write, which is exactly the contract the guard
	 * mechanism is for.
	 */
	bool guarded = (args->cwa_guard.cwg_check == TRUE);
	const chunk_guard4 *guard =
		guarded ? &args->cwa_guard.write_chunk_guard4_u.cwg_guard :
			  NULL;

	for (uint32_t i = 0; i < nchunks; i++) {
		struct chunk_block *prev =
			chunk_store_lookup(cs, args->cwa_offset + i);

		/* Axis (ii): CAS via cwa_guard. */
		if (guarded && prev &&
		    (prev->cb_gen_id != guard->cg_gen_id ||
		     prev->cb_client_id != guard->cg_client_id)) {
			if (cstats)
				atomic_fetch_add_explicit(
					&cstats->cs_chunk_busy_delay, 1,
					memory_order_relaxed);
			pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);
			*status = NFS4ERR_DELAY;
			return 0;
		}

		/* Axis (i): PENDING from a different writer. */
		if (!prev || prev->cb_state != CHUNK_STATE_PENDING)
			continue;
		if (prev->cb_owner_id == args->cwa_owner.co_id &&
		    prev->cb_client_id == args->cwa_owner.co_guard.cg_client_id)
			continue;

		if (cstats)
			atomic_fetch_add_explicit(&cstats->cs_chunk_busy_delay,
						  1, memory_order_relaxed);

		pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);
		*status = NFS4ERR_DELAY;
		return 0;
	}

	/* Ensure the data block exists. */
	pthread_rwlock_wrlock(&compound->c_inode->i_db_rwlock);
	if (!compound->c_inode->i_db) {
		compound->c_inode->i_db = data_block_alloc(
			compound->c_inode, args->cwa_chunks.cwa_chunks_val,
			args->cwa_chunks.cwa_chunks_len,
			(off_t)args->cwa_offset * chunk_size);
		if (!compound->c_inode->i_db) {
			pthread_rwlock_unlock(&compound->c_inode->i_db_rwlock);
			pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);
			*status = NFS4ERR_NOSPC;
			return 0;
		}
	} else {
		/* Write chunk data into existing data block. */
		ssize_t wret =
			data_block_write(compound->c_inode->i_db,
					 args->cwa_chunks.cwa_chunks_val,
					 args->cwa_chunks.cwa_chunks_len,
					 (off_t)args->cwa_offset * chunk_size);
		if (wret < 0) {
			pthread_rwlock_unlock(&compound->c_inode->i_db_rwlock);
			pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);
			*status = NFS4ERR_IO;
			return 0;
		}
	}

	/* Update inode size. */
	int64_t new_end = (int64_t)args->cwa_offset * (int64_t)chunk_size +
			  (int64_t)total_data;
	if (new_end > compound->c_inode->i_size)
		compound->c_inode->i_size = new_end;

	pthread_rwlock_unlock(&compound->c_inode->i_db_rwlock);

	/* Record per-block metadata.  Last block may be smaller. */
	for (uint32_t i = 0; i < nchunks; i++) {
		uint32_t blk_size = chunk_size;

		if (i == nchunks - 1 && total_data % chunk_size != 0)
			blk_size = total_data % chunk_size;

		/*
		 * Persist the algorithm and value bytes verbatim from the
		 * wire.  We already validated the entry above for the only
		 * algorithm this implementation accepts (CRC32, 4 bytes);
		 * the persistence path simply copies what the client sent
		 * so that CHUNK_READ can echo the same algorithm tag back.
		 *
		 * When no checksum was supplied, cb_checksum_len is 0 and
		 * the bit-rot check on CHUNK_READ is skipped.
		 */
		uint32_t blk_csum_algo = CHECKSUM_ALG_NONE;
		uint32_t blk_csum_len = 0;
		uint8_t blk_csum_value[CHUNK_VALUE_MAX] = { 0 };

		if (i < nchecksums) {
			const checksum4 *cs =
				&args->cwa_checksums.cwa_checksums_val[i];

			blk_csum_algo = cs->cs_algorithm;
			blk_csum_len = cs->cs_value.cs_value_len;
			if (blk_csum_len > sizeof(blk_csum_value))
				blk_csum_len = sizeof(blk_csum_value);
			memcpy(blk_csum_value, cs->cs_value.cs_value_val,
			       blk_csum_len);
		}

		struct chunk_block blk = {
			.cb_state = CHUNK_STATE_PENDING,
			.cb_gen_id = args->cwa_owner.co_guard.cg_gen_id,
			.cb_client_id = args->cwa_owner.co_guard.cg_client_id,
			.cb_owner_id = args->cwa_owner.co_id,
			.cb_payload_id = args->cwa_payload_id,
			.cb_checksum_algorithm = blk_csum_algo,
			.cb_checksum_len = blk_csum_len,
			.cb_chunk_size = blk_size,
			/*
			 * Server-known writer identity for lease-driven
			 * rollback: nfs4_client_expire calls
			 * chunk_store_rollback_for_client(cs, this_clientid)
			 * to release PENDING/FINALIZED chunks owned by a
			 * dead writer.  Zero when no NFSv4.1 client is
			 * attached (anonymous/legacy path), in which case
			 * the lease reaper sweep can never match and the
			 * rollback story degenerates to manual recovery via
			 * the CHUNK_ROLLBACK protocol op.
			 */
			.cb_writer_clientid =
				compound->c_nfs4_client ?
					nfs4_client_to_client(
						compound->c_nfs4_client)
						->c_id :
					0,
		};

		memcpy(blk.cb_checksum_value, blk_csum_value,
		       sizeof(blk.cb_checksum_value));

		/*
		 * INV-1 / chunk-collision instrumentation: peek at the
		 * prior block before overwriting.  Two axes:
		 *   - cs_blocks_first_write / cs_blocks_overwrite:
		 *     EMPTY vs non-EMPTY prior state -- INV-1's RMW
		 *     ratio answering Hellwig msg 5 (in-place update).
		 *   - cs_blocks_full / cs_blocks_partial: blk_size
		 *     vs chunk_size -- INV-1's full vs partial write
		 *     ratio answering Hellwig msg 9 (NFS block size).
		 *
		 * cs_pending_displaced (cross-writer PENDING overwrite)
		 * used to bump here, but the Option C gate above
		 * (commit d8a09448671b) rejects exactly that case
		 * before this loop runs.  cs_chunk_busy_delay is the
		 * post-Option-C counter that records the same shape of
		 * contention.  The cs_pending_displaced wire field
		 * stays in probe1's response struct for backward compat
		 * with deployed probe clients; it reports zero in any
		 * post-Option-C build (callers should read
		 * cs_chunk_busy_delay instead).
		 */
		if (cstats) {
			struct chunk_block *prev =
				chunk_store_lookup(cs, args->cwa_offset + i);

			/*
			 * chunk_store_lookup returns NULL for EMPTY (and for
			 * out-of-range offsets, which never happens here --
			 * chunk_store_write below grows the array if needed,
			 * but the lookup sees the pre-grow state).  Either
			 * way, NULL == first-write for INV-1's purposes.
			 */
			if (prev == NULL)
				atomic_fetch_add_explicit(
					&cstats->cs_blocks_first_write, 1,
					memory_order_relaxed);
			else
				atomic_fetch_add_explicit(
					&cstats->cs_blocks_overwrite, 1,
					memory_order_relaxed);

			if (blk_size == chunk_size)
				atomic_fetch_add_explicit(
					&cstats->cs_blocks_full, 1,
					memory_order_relaxed);
			else
				atomic_fetch_add_explicit(
					&cstats->cs_blocks_partial, 1,
					memory_order_relaxed);
		}

		if (chunk_store_write(cs, args->cwa_offset + i, &blk) < 0) {
			pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);
			*status = NFS4ERR_DELAY;
			return 0;
		}
	}

	if (cstats) {
		atomic_fetch_add_explicit(&cstats->cs_writes, 1,
					  memory_order_relaxed);
		/*
		 * INV-1 per-write block-count histogram.  Buckets sized
		 * for the practical range observed in T2 IOR runs
		 * (typically 1, 4, or 16 chunks per CHUNK_WRITE).
		 */
		_Atomic uint64_t *bucket;

		if (nchunks == 1)
			bucket = &cstats->cs_writes_1block;
		else if (nchunks < 8)
			bucket = &cstats->cs_writes_2to7;
		else if (nchunks < 32)
			bucket = &cstats->cs_writes_8to31;
		else
			bucket = &cstats->cs_writes_32plus;
		atomic_fetch_add_explicit(bucket, 1, memory_order_relaxed);
	}

	pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);

	resok->cwr_count = nchunks;
	resok->cwr_committed = (args->cwa_stable == FILE_SYNC4) ? FILE_SYNC4 :
								  UNSTABLE4;
	chunk_write_verf(compound->c_server_state, resok->cwr_writeverf);

	/* Return per-chunk status (all OK for happy path). */
	resok->cwr_block_status.cwr_block_status_val =
		calloc(nchunks, sizeof(nfsstat4));
	if (!resok->cwr_block_status.cwr_block_status_val) {
		*status = NFS4ERR_DELAY;
		return 0;
	}
	resok->cwr_block_status.cwr_block_status_len = nchunks;
	resok->cwr_owners.cwr_owners_len = 0;
	resok->cwr_owners.cwr_owners_val = NULL;

	return 0;
}

/* ------------------------------------------------------------------ */
/* CHUNK_READ                                                          */
/* ------------------------------------------------------------------ */

uint32_t nfs4_op_chunk_read(struct compound *compound)
{
	CHUNK_READ4args *args = NFS4_OP_ARG_SETUP(compound, opchunk_read);
	CHUNK_READ4res *res = NFS4_OP_RES_SETUP(compound, opchunk_read);
	nfsstat4 *status = &res->crr_status;
	CHUNK_READ4resok *resok =
		NFS4_OP_RESOK_SETUP(res, CHUNK_READ4res_u, crr_resok4);

	if (network_file_handle_empty(&compound->c_curr_nfh)) {
		*status = NFS4ERR_NOFILEHANDLE;
		return 0;
	}

	if (!compound->c_inode || !S_ISREG(compound->c_inode->i_mode)) {
		*status = NFS4ERR_INVAL;
		return 0;
	}

	uint32_t count = (uint32_t)args->cra_count;

	if (count == 0) {
		resok->crr_eof = TRUE;
		resok->crr_chunks.crr_chunks_len = 0;
		resok->crr_chunks.crr_chunks_val = NULL;
		return 0;
	}

	/*
	 * Trust table validation -- tightly-coupled DS.
	 * Same logic as CHUNK_WRITE: special stateids bypass the check.
	 */
	if (!stateid4_is_special(&args->cra_stateid)) {
		struct trust_entry *te = trust_stateid_find(&args->cra_stateid);

		if (!te) {
			*status = NFS4ERR_BAD_STATEID;
			return 0;
		}

		uint64_t now = reffs_now_ns();
		uint64_t exp = atomic_load_explicit(&te->te_expire_ns,
						    memory_order_acquire);
		uint32_t flags = atomic_load_explicit(&te->te_flags,
						      memory_order_acquire);

		if (flags & TRUST_PENDING) {
			trust_entry_put(te);
			*status = NFS4ERR_DELAY;
			return 0;
		}
		if (exp != 0 && now > exp) {
			trust_entry_put(te);
			*status = NFS4ERR_BAD_STATEID;
			return 0;
		}
		if (!(flags & TRUST_ACTIVE)) {
			trust_entry_put(te);
			*status = NFS4ERR_BAD_STATEID;
			return 0;
		}

		trust_entry_put(te);
	}

	pthread_mutex_lock(&compound->c_inode->i_attr_mutex);

	struct chunk_store *cs = compound->c_inode->i_chunk_store;

	if (!cs) {
		pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);
		*status = NFS4ERR_NOENT;
		return 0;
	}

	/*
	 * Determine how many blocks are available.  A block that is
	 * PENDING (in-flight write) requires special handling for
	 * cross-writer RMW correctness (Track 1b Option C full):
	 *
	 *   - PENDING block from a DIFFERENT owner: the prior COMMITTED
	 *     bytes have already been overwritten on disk by the PENDING
	 *     writer's CHUNK_WRITE (data_block_write happens at write
	 *     time, not at FINALIZE).  Returning NFS4ERR_NOENT would be
	 *     wrong twice over: the block DOES exist, and the data the
	 *     reader can see (the PENDING bytes) isn't visible-to-all
	 *     yet.  Return NFS4ERR_DELAY so the client retries after
	 *     the PENDING writer finalizes + commits.
	 *
	 *   - PENDING block from the SAME owner: the writer is reading
	 *     its own in-flight bytes; allowed.  (Doesn't apply to the
	 *     read-block-zero loop here because cb_state < FINALIZED
	 *     is excluded; this comment captures intent for future
	 *     edits.)
	 *
	 *   - EMPTY or never-written block: NOENT, unchanged.
	 *
	 * Pre-Option-C, PENDING-from-different-owner returned NOENT,
	 * which read as "no data" to the client and aborted the RMW.
	 * Subchunk's two writers on the same 4 KiB block hit this
	 * race on roughly 15% of runs (one writer's CHUNK_WRITE
	 * arrived between the other writer's CHUNK_READ start and the
	 * pending-writer's FINALIZE).
	 */
	uint32_t avail = 0;
	bool pending_seen = false;

	for (uint32_t i = 0; i < count; i++) {
		struct chunk_block *blk =
			chunk_store_lookup(cs, args->cra_offset + i);

		if (blk && blk->cb_state >= CHUNK_STATE_FINALIZED) {
			avail++;
		} else if (blk && blk->cb_state == CHUNK_STATE_PENDING) {
			pending_seen = true;
			break;
		} else {
			break;
		}
	}

	if (avail == 0) {
		pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);
		*status = pending_seen ? NFS4ERR_DELAY : NFS4ERR_NOENT;
		return 0;
	}

	resok->crr_chunks.crr_chunks_len = avail;
	resok->crr_chunks.crr_chunks_val = calloc(avail, sizeof(read_chunk4));
	if (!resok->crr_chunks.crr_chunks_val) {
		pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);
		*status = NFS4ERR_DELAY;
		return 0;
	}

	pthread_rwlock_rdlock(&compound->c_inode->i_db_rwlock);

	for (uint32_t i = 0; i < avail; i++) {
		uint64_t off = args->cra_offset + i;
		struct chunk_block *blk = chunk_store_lookup(cs, off);
		read_chunk4 *rc = &resok->crr_chunks.crr_chunks_val[i];

		if (chunk_checksum_pack(
			    &rc->cr_checksum, blk->cb_checksum_algorithm,
			    blk->cb_checksum_value, blk->cb_checksum_len) < 0) {
			pthread_rwlock_unlock(&compound->c_inode->i_db_rwlock);
			pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);
			*status = NFS4ERR_DELAY;
			return 0;
		}
		rc->cr_effective_len = blk->cb_chunk_size;
		rc->cr_owner.co_guard.cg_gen_id = blk->cb_gen_id;
		rc->cr_owner.co_guard.cg_client_id = blk->cb_client_id;
		rc->cr_owner.co_id = blk->cb_owner_id;
		rc->cr_payload_id = blk->cb_payload_id;
		rc->cr_locked.cr_locked_len = 0;
		rc->cr_locked.cr_locked_val = NULL;
		rc->cr_status.cr_status_len = 0;
		rc->cr_status.cr_status_val = NULL;

		/* Read chunk data from data block. */
		rc->cr_chunk.cr_chunk_val = calloc(1, blk->cb_chunk_size);
		if (!rc->cr_chunk.cr_chunk_val) {
			/*
			 * Truncate the response to the chunks we have
			 * already populated so the client never sees a
			 * non-zero cr_chunk_len with a NULL data pointer.
			 */
			resok->crr_chunks.crr_chunks_len = i;
			break;
		}
		rc->cr_chunk.cr_chunk_len = blk->cb_chunk_size;

		/*
		 * Offset uses the nominal chunk_size (disk stride), not
		 * the per-block actual size.  The last block may be shorter
		 * than the stride (Mojette variable projections).
		 */
		if (compound->c_inode->i_db) {
			data_block_read(compound->c_inode->i_db,
					rc->cr_chunk.cr_chunk_val,
					blk->cb_chunk_size,
					(off_t)(off * cs->cs_chunk_size));
		}

		/*
		 * Verify the stored checksum against data read from disk.
		 * Detects silent data corruption (bit rot).  Only CRC32 is
		 * implemented; other algorithms are persisted verbatim but
		 * not recomputed (the integrity check degrades to "trust
		 * the stored bytes", which is still useful when the stored
		 * checksum itself was corrupted on disk -- the client side
		 * recomputes against the data anyway).
		 */
		if (blk->cb_checksum_len > 0 &&
		    blk->cb_checksum_algorithm == CHECKSUM_ALG_CRC32 &&
		    blk->cb_checksum_len == 4) {
			uint32_t stored_crc =
				((uint32_t)blk->cb_checksum_value[0] << 24) |
				((uint32_t)blk->cb_checksum_value[1] << 16) |
				((uint32_t)blk->cb_checksum_value[2] << 8) |
				(uint32_t)blk->cb_checksum_value[3];
			uint32_t disk_crc = (uint32_t)crc32(
				0L, (const Bytef *)rc->cr_chunk.cr_chunk_val,
				(uInt)blk->cb_chunk_size);

			if (disk_crc != stored_crc) {
				LOG("CHUNK_READ: CRC mismatch block %" PRIu64
				    ": stored 0x%08x disk 0x%08x",
				    off, stored_crc, disk_crc);
				/*
				 * Re-pack with the disk-derived CRC so the
				 * client sees what was actually read; XDR
				 * free will release the new allocation.
				 */
				free(rc->cr_checksum.cs_value.cs_value_val);
				rc->cr_checksum.cs_value.cs_value_val = NULL;
				rc->cr_checksum.cs_value.cs_value_len = 0;
				(void)chunk_checksum_pack_crc32(
					&rc->cr_checksum, disk_crc);
			}
		}
	}

	uint64_t cs_nblocks = cs->cs_nblocks;

	pthread_rwlock_unlock(&compound->c_inode->i_db_rwlock);
	pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);

	resok->crr_eof = (args->cra_offset + avail >= cs_nblocks) ? TRUE :
								    FALSE;

	return 0;
}

/* ------------------------------------------------------------------ */
/* CHUNK_FINALIZE                                                      */
/* ------------------------------------------------------------------ */

uint32_t nfs4_op_chunk_finalize(struct compound *compound)
{
	CHUNK_FINALIZE4args *args =
		NFS4_OP_ARG_SETUP(compound, opchunk_finalize);
	CHUNK_FINALIZE4res *res = NFS4_OP_RES_SETUP(compound, opchunk_finalize);
	nfsstat4 *status = &res->cfr_status;
	CHUNK_FINALIZE4resok *resok =
		NFS4_OP_RESOK_SETUP(res, CHUNK_FINALIZE4res_u, cfr_resok4);

	if (network_file_handle_empty(&compound->c_curr_nfh)) {
		*status = NFS4ERR_NOFILEHANDLE;
		return 0;
	}

	if (!compound->c_inode) {
		*status = NFS4ERR_INVAL;
		return 0;
	}

	uint32_t count = (uint32_t)args->cfa_count;

	if (count == 0 || args->cfa_chunks.cfa_chunks_len == 0) {
		*status = NFS4ERR_INVAL;
		return 0;
	}

	for (uint32_t i = 0; i < args->cfa_chunks.cfa_chunks_len; i++) {
		if (chunk_cid_is_reserved(args->cfa_chunks.cfa_chunks_val[i]
						  .co_guard.cg_client_id)) {
			*status = NFS4ERR_INVAL;
			return 0;
		}
	}

	pthread_mutex_lock(&compound->c_inode->i_attr_mutex);

	struct chunk_store *cs = compound->c_inode->i_chunk_store;

	if (!cs) {
		pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);
		*status = NFS4ERR_NOENT;
		return 0;
	}

	/*
	 * Transition each owner's blocks from PENDING --> FINALIZED.
	 * The cfa_chunks array lists the chunk_owner4 entries to finalize.
	 */
	uint32_t nowners = args->cfa_chunks.cfa_chunks_len;

	resok->cfr_status.cfr_status_val = calloc(nowners, sizeof(nfsstat4));
	if (!resok->cfr_status.cfr_status_val) {
		pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);
		*status = NFS4ERR_DELAY;
		return 0;
	}
	resok->cfr_status.cfr_status_len = nowners;

	for (uint32_t i = 0; i < nowners; i++) {
		chunk_owner4 *co = &args->cfa_chunks.cfa_chunks_val[i];
		int ret = chunk_store_transition(cs, args->cfa_offset, count,
						 co->co_id, CHUNK_STATE_PENDING,
						 CHUNK_STATE_FINALIZED);
		resok->cfr_status.cfr_status_val[i] =
			(ret == 0) ? NFS4_OK : NFS4ERR_INVAL;
	}

	/* Persist metadata -- FINALIZED state must survive DS restart. */
	chunk_store_persist(cs, compound->c_server_state->ss_state_dir,
			    compound->c_inode->i_ino);

	pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);

	chunk_write_verf(compound->c_server_state, resok->cfr_writeverf);

	return 0;
}

/* ------------------------------------------------------------------ */
/* CHUNK_COMMIT                                                        */
/* ------------------------------------------------------------------ */

uint32_t nfs4_op_chunk_commit(struct compound *compound)
{
	CHUNK_COMMIT4args *args = NFS4_OP_ARG_SETUP(compound, opchunk_commit);
	CHUNK_COMMIT4res *res = NFS4_OP_RES_SETUP(compound, opchunk_commit);
	nfsstat4 *status = &res->ccr_status;
	CHUNK_COMMIT4resok *resok =
		NFS4_OP_RESOK_SETUP(res, CHUNK_COMMIT4res_u, ccr_resok4);

	if (network_file_handle_empty(&compound->c_curr_nfh)) {
		*status = NFS4ERR_NOFILEHANDLE;
		return 0;
	}

	if (!compound->c_inode) {
		*status = NFS4ERR_INVAL;
		return 0;
	}

	uint32_t count = (uint32_t)args->cca_count;

	if (count == 0 || args->cca_chunks.cca_chunks_len == 0) {
		*status = NFS4ERR_INVAL;
		return 0;
	}

	for (uint32_t i = 0; i < args->cca_chunks.cca_chunks_len; i++) {
		if (chunk_cid_is_reserved(args->cca_chunks.cca_chunks_val[i]
						  .co_guard.cg_client_id)) {
			*status = NFS4ERR_INVAL;
			return 0;
		}
	}

	pthread_mutex_lock(&compound->c_inode->i_attr_mutex);

	struct chunk_store *cs = compound->c_inode->i_chunk_store;

	if (!cs) {
		pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);
		*status = NFS4ERR_NOENT;
		return 0;
	}

	uint32_t nowners = args->cca_chunks.cca_chunks_len;

	resok->ccr_status.ccr_status_val = calloc(nowners, sizeof(nfsstat4));
	if (!resok->ccr_status.ccr_status_val) {
		pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);
		*status = NFS4ERR_DELAY;
		return 0;
	}
	resok->ccr_status.ccr_status_len = nowners;

	for (uint32_t i = 0; i < nowners; i++) {
		chunk_owner4 *co = &args->cca_chunks.cca_chunks_val[i];
		int ret = chunk_store_transition(cs, args->cca_offset, count,
						 co->co_id,
						 CHUNK_STATE_FINALIZED,
						 CHUNK_STATE_COMMITTED);
		resok->ccr_status.ccr_status_val[i] =
			(ret == 0) ? NFS4_OK : NFS4ERR_INVAL;
	}

	/* Persist metadata -- COMMITTED state is the durability guarantee. */
	chunk_store_persist(cs, compound->c_server_state->ss_state_dir,
			    compound->c_inode->i_ino);

	/* Sync data to disk for FILE_SYNC4 semantics.  Done under the
	 * mutex to prevent interleaving with concurrent writes. */
	inode_sync_to_disk(compound->c_inode);

	pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);

	chunk_write_verf(compound->c_server_state, resok->ccr_writeverf);

	return 0;
}

/* ------------------------------------------------------------------ */
/* Stub ops -- not needed for happy-path demo                           */
/* ------------------------------------------------------------------ */

uint32_t nfs4_op_chunk_error(struct compound *compound)
{
	CHUNK_ERROR4res *res = NFS4_OP_RES_SETUP(compound, opchunk_error);
	nfsstat4 *status = &res->cer_status;

	*status = NFS4ERR_NOTSUPP;

	return 0;
}

uint32_t nfs4_op_chunk_header_read(struct compound *compound)
{
	CHUNK_HEADER_READ4res *res =
		NFS4_OP_RES_SETUP(compound, opchunk_header_read);
	nfsstat4 *status = &res->chrr_status;

	*status = NFS4ERR_NOTSUPP;

	return 0;
}

uint32_t nfs4_op_chunk_lock(struct compound *compound)
{
	CHUNK_LOCK4res *res = NFS4_OP_RES_SETUP(compound, opchunk_lock);
	nfsstat4 *status = &res->clr_status;

	*status = NFS4ERR_NOTSUPP;

	return 0;
}

/*
 * OP_CHUNK_REPAIRED -- client tells the MDS that an EC repair landed
 * on a previously REPAIR-flagged mirror.  The MDS clears
 * FFV2_DS_FLAGS_REPAIR on every flagged mirror covered by the
 * requested range and persists the layout segments so the cleared
 * state survives an MDS restart.
 *
 * Validation rules (.claude/design/ec-repair.md sec 4):
 *   1. current FH set                 -> NFS4ERR_NOFILEHANDLE
 *   2. current FH is a regular file   -> NFS4ERR_INVAL
 *   3. cpa_owner.cg_client_id not reserved -> NFS4ERR_INVAL
 *   4. inode has i_layout_segments    -> NFS4ERR_INVAL
 *   5. (idempotent) no mirror is FFV2_DS_FLAGS_REPAIR-flagged ->
 *      NFS4_OK with no state change.  Covers the crash-recovery
 *      retry case where the client re-issues the op after the MDS
 *      already persisted the clear.
 *
 * cs_repair_completed bumps by the COUNT of mirrors actually
 * cleared (per Open Question 4 answer 2026-06-09: per-mirror, not
 * per-call).  An idempotent retry that clears zero mirrors does
 * not bump the counter.
 *
 * NOT_NOW_BROWN_COW: rigorous cpa_stateid validation (must resolve
 * to a valid OPEN or layout stateid on the inode) is deferred to a
 * follow-up.  The IETF-demo cooperative-client model treats
 * stateid auth as a layout-layer concern enforced by the MDS at
 * LAYOUTGET time; the chunk-state-clearing surface here is
 * controlled by the layout's own clientid match (Open Question 1
 * answer: defence-in-depth TRUST_STATEID hint is out of scope).
 *
 * NOT_NOW_BROWN_COW: cpa_offset / cpa_count range matching against
 * the layout segments' byte ranges.  Single-segment whole-file
 * layouts (the demo cell shape) trivially satisfy any range; once
 * striped repair lands, this needs the per-segment chunk_size
 * plumbing flagged in Open Question 3.
 *
 * NOT_NOW_BROWN_COW: clientid match between the layout-holder and
 * the calling client (rule 7 in the design doc).  Deferred to the
 * same follow-up as stateid validation; the demo's cooperative
 * single-writer model is unaffected.
 */
uint32_t nfs4_op_chunk_repaired(struct compound *compound)
{
	CHUNK_REPAIRED4args *args = NFS4_OP_ARG_SETUP(compound, opchunk_repair);
	CHUNK_REPAIRED4res *res = NFS4_OP_RES_SETUP(compound, opchunk_repair);
	nfsstat4 *status = &res->cpr_status;

	if (network_file_handle_empty(&compound->c_curr_nfh)) {
		*status = NFS4ERR_NOFILEHANDLE;
		return 0;
	}

	if (!compound->c_inode || !S_ISREG(compound->c_inode->i_mode)) {
		*status = NFS4ERR_INVAL;
		return 0;
	}

	if (chunk_cid_is_reserved(args->cpa_owner.co_guard.cg_client_id)) {
		*status = NFS4ERR_INVAL;
		return 0;
	}

	pthread_mutex_lock(&compound->c_inode->i_attr_mutex);

	struct layout_segments *lss = compound->c_inode->i_layout_segments;

	if (!lss || lss->lss_count == 0) {
		pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);
		*status = NFS4ERR_INVAL;
		return 0;
	}

	/*
	 * Demo cell shape is single-segment whole-file -- iterate every
	 * mirror in every segment and clear FFV2_DS_FLAGS_REPAIR.  The
	 * cleared count tells cs_repair_completed how much work
	 * actually happened (zero on idempotent retry, N on a real
	 * clear).
	 */
	uint32_t cleared = 0;

	for (uint32_t s = 0; s < lss->lss_count; s++) {
		struct layout_segment *seg = &lss->lss_segs[s];

		for (uint32_t f = 0; f < seg->ls_nfiles; f++) {
			struct layout_data_file *ldf = &seg->ls_files[f];

			if (ldf->ldf_flags & FFV2_DS_FLAGS_REPAIR) {
				ldf->ldf_flags &= ~FFV2_DS_FLAGS_REPAIR;
				cleared++;
			}
		}
	}

	if (cleared > 0) {
		/*
		 * Persist the cleared flag bits BEFORE returning NFS4_OK
		 * so the client can rely on the MDS-side state surviving
		 * a power-fail-after-reply.  Crash-recovery story:
		 * .claude/design/ec-repair.md sec 3 covers each failure
		 * point; idempotent rule 5 above handles client retries
		 * that win the race against MDS persistence.
		 */
		inode_sync_to_disk(compound->c_inode);

		if (compound->c_curr_sb)
			atomic_fetch_add_explicit(
				&compound->c_curr_sb->sb_chunk_stats
					 .cs_repair_completed,
				cleared, memory_order_relaxed);
	}

	pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);

	/* *status stays NFS4_OK (calloc'd default). */
	return 0;
}

uint32_t nfs4_op_chunk_rollback(struct compound *compound)
{
	CHUNK_ROLLBACK4args *args =
		NFS4_OP_ARG_SETUP(compound, opchunk_rollback);
	CHUNK_ROLLBACK4res *res = NFS4_OP_RES_SETUP(compound, opchunk_rollback);
	nfsstat4 *status = &res->crr_status;
	CHUNK_ROLLBACK4resok *resok =
		NFS4_OP_RESOK_SETUP(res, CHUNK_ROLLBACK4res_u, crr_resok4);

	if (compound->c_curr_sb)
		atomic_fetch_add_explicit(
			&compound->c_curr_sb->sb_chunk_stats.cs_rollback_invoked,
			1, memory_order_relaxed);

	if (network_file_handle_empty(&compound->c_curr_nfh)) {
		*status = NFS4ERR_NOFILEHANDLE;
		return 0;
	}

	if (!compound->c_inode) {
		*status = NFS4ERR_INVAL;
		return 0;
	}

	uint32_t count = (uint32_t)args->crb_count;

	if (count == 0 || args->crb_chunks.crb_chunks_len == 0) {
		*status = NFS4ERR_INVAL;
		return 0;
	}

	pthread_mutex_lock(&compound->c_inode->i_attr_mutex);

	struct chunk_store *cs = compound->c_inode->i_chunk_store;

	if (!cs) {
		pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);
		*status = NFS4ERR_NOENT;
		return 0;
	}

	/*
	 * Per draft-haynes-nfsv4-flexfiles-v2 fig-chunk-state-machine:
	 * ROLLBACK admits PENDING -> EMPTY and FINALIZED -> EMPTY.  The
	 * COMMITTED -> newer-COMMITTED repair path requires cg_gen_id
	 * handling and is NOT_NOW_BROWN_COW; chunk_store_rollback returns
	 * -ENOTSUP for that case and we surface NFS4ERR_NOTSUPP.
	 *
	 * Unlike FINALIZE/COMMIT (which return per-owner status arrays),
	 * the CHUNK_ROLLBACK4resok shape carries only writeverf -- the
	 * outer crr_status reports the operation's overall outcome.  We
	 * apply each owner's rollback in sequence; the first failure
	 * stops the loop and surfaces in *status.
	 */
	uint32_t nowners = args->crb_chunks.crb_chunks_len;

	for (uint32_t i = 0; i < nowners; i++) {
		chunk_owner4 *co = &args->crb_chunks.crb_chunks_val[i];
		int ret = chunk_store_rollback(cs, args->crb_offset, count,
					       co->co_id);

		if (ret == -ENOTSUP) {
			pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);
			*status = NFS4ERR_NOTSUPP;
			return 0;
		}
		if (ret < 0) {
			pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);
			*status = NFS4ERR_INVAL;
			return 0;
		}
	}

	/* Persist the EMPTY state -- rollback must survive DS restart. */
	chunk_store_persist(cs, compound->c_server_state->ss_state_dir,
			    compound->c_inode->i_ino);

	pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);

	chunk_write_verf(compound->c_server_state, resok->crr_writeverf);

	return 0;
}

uint32_t nfs4_op_chunk_unlock(struct compound *compound)
{
	CHUNK_UNLOCK4res *res = NFS4_OP_RES_SETUP(compound, opchunk_unlock);
	nfsstat4 *status = &res->cur_status;

	*status = NFS4ERR_NOTSUPP;

	return 0;
}

/* ------------------------------------------------------------------ */
/* Server-wide rollback for a dying writer                             */
/* ------------------------------------------------------------------ */

/*
 * Two-pass collection of inodes that may carry orphan chunks for a
 * dying writer.  Pass 1 (under rcu_read_lock, in the callback) takes
 * inode_active_get_rcu refs and records inode pointers.  Pass 2
 * (outside rcu) drops the lock and per-inode locks i_attr_mutex,
 * sweeps the chunk_store, persists, releases.
 *
 * The collection buffer grows in increments outside RCU; the
 * callback under RCU only appends to a pre-sized region and signals
 * "buffer full" to the caller so the caller can grow + retry.  This
 * keeps the RCU read-side critical section blocking-free per
 * .claude/patterns/rcu-violations.md Pattern 1.
 */

#define CHUNK_ROLLBACK_INITIAL_INODE_CAP 64
#define CHUNK_ROLLBACK_MAX_GROW_RETRIES 5 /* 64 -> 128 -> ... -> 2048 */

struct chunk_rollback_collect {
	struct inode **inodes; /* pre-sized buffer */
	uint32_t cap;
	uint32_t n;
	bool overflow; /* set if cap was reached during a pass */
};

static int chunk_rollback_collect_cb(struct inode *inode, void *arg)
{
	struct chunk_rollback_collect *ctx = arg;

	/*
	 * Quick filter: only collect inodes that already have a
	 * chunk_store.  Reading i_chunk_store without the i_attr_mutex
	 * is deliberate-loose: a stale NULL just means we skip an inode
	 * whose chunks pass-2 would re-discover under the lock anyway,
	 * which is the worst-case "missed sweep" -- and even that is
	 * a benign miss because the dying writer's session is already
	 * gone, so it cannot be issuing fresh CHUNK_WRITEs that would
	 * stick PENDING after this expiry.  Pass 2 always re-reads
	 * i_chunk_store under i_attr_mutex before doing real work, so
	 * a stale read here cannot cause incorrect rollback.  The
	 * non-NULL case is monotonic: a chunk_store, once attached to
	 * an inode by chunk_store_get (under i_attr_mutex), is not
	 * detached until inode_release; pass-2 sees the same pointer
	 * we recorded here.
	 */
	if (!inode->i_chunk_store)
		return 0;

	if (ctx->n >= ctx->cap) {
		ctx->overflow = true;
		return 1; /* signal "stop iterating; resize" */
	}

	/*
	 * Take the active ref BEFORE recording in the buffer.  A failed
	 * inode_active_get_rcu (inode is dying) returns 0 and never
	 * advances ctx->n, so pass 2 never sees a NULL slot.  This
	 * ordering is load-bearing -- a refactor that increments ctx->n
	 * before the get would either leak the slot or feed pass 2 a
	 * NULL pointer to dereference.
	 */
	if (!inode_active_get_rcu(inode))
		return 0; /* inode is being torn down -- skip */

	ctx->inodes[ctx->n++] = inode;
	return 0;
}

uint32_t chunk_rollback_for_client(uint64_t writer_clientid,
				   const char *state_dir)
{
	struct chunk_rollback_collect ctx = {
		.cap = CHUNK_ROLLBACK_INITIAL_INODE_CAP,
		.n = 0,
		.overflow = false,
	};

	ctx.inodes = calloc(ctx.cap, sizeof(*ctx.inodes));
	if (!ctx.inodes)
		return 0;

	/*
	 * Collect-and-grow loop.  reffs_fs_for_each_inode currently
	 * ignores the callback's non-zero return for early-stop; if
	 * the buffer fills, we still walk the rest under RCU but the
	 * callback short-circuits at the buffer-full check.  Until
	 * the FS layer supports a real early-stop, the safe shape is
	 * to size generously up front and re-walk if overflow occurred.
	 *
	 * Bounded by CHUNK_ROLLBACK_MAX_GROW_RETRIES so a pathological
	 * insert-storm during the walk cannot loop forever.  The cap
	 * (~2048 inodes-with-chunk-stores by the last grow) is generous
	 * relative to current bench scale; if it ever fires we want a
	 * loud TRACE and graceful give-up, not silent unboundedness.
	 */
	for (uint32_t retry = 0;; retry++) {
		ctx.n = 0;
		ctx.overflow = false;

		reffs_fs_for_each_inode(chunk_rollback_collect_cb, &ctx);

		if (!ctx.overflow)
			break;

		/* Drop the active refs collected in this aborted pass. */
		for (uint32_t i = 0; i < ctx.n; i++)
			inode_active_put(ctx.inodes[i]);

		if (retry >= CHUNK_ROLLBACK_MAX_GROW_RETRIES) {
			TRACE("chunk_rollback_for_client: giving up after %u "
			      "grow retries (cap=%u, clientid=0x%" PRIx64 ")",
			      retry, ctx.cap, writer_clientid);
			free(ctx.inodes);
			return 0;
		}

		/* Grow + retry. */
		uint32_t new_cap = ctx.cap * 2;
		struct inode **nbuf =
			realloc(ctx.inodes, new_cap * sizeof(*ctx.inodes));

		if (!nbuf) {
			free(ctx.inodes);
			return 0;
		}
		ctx.inodes = nbuf;
		ctx.cap = new_cap;
		TRACE("chunk_rollback_for_client: grew inode buffer to %u "
		      "(retry %u, clientid=0x%" PRIx64 ")",
		      new_cap, retry + 1, writer_clientid);
	}

	/* Pass 2: outside RCU, do the real work per inode. */
	uint32_t total = 0;

	for (uint32_t i = 0; i < ctx.n; i++) {
		struct inode *inode = ctx.inodes[i];

		pthread_mutex_lock(&inode->i_attr_mutex);
		if (inode->i_chunk_store) {
			uint32_t n = chunk_store_rollback_for_client(
				inode->i_chunk_store, writer_clientid);
			if (n > 0) {
				total += n;
				if (state_dir)
					chunk_store_persist(
						inode->i_chunk_store, state_dir,
						inode->i_ino);
			}
		}
		pthread_mutex_unlock(&inode->i_attr_mutex);

		inode_active_put(inode);
	}

	free(ctx.inodes);

	if (total > 0)
		TRACE("chunk_rollback_for_client: clientid=0x%" PRIx64
		      " rolled back %u block(s) across %u inode(s)",
		      writer_clientid, total, ctx.n);

	return total;
}

/*
 * OP_CHUNK_WRITE_REPAIR -- write a reconstructed shard back to a DS
 * after EC-decode found the original missing or corrupt.  Mirrors the
 * CHUNK_WRITE payload contract (cwra_/cwa_ field-prefix differs but
 * the validation rules are identical -- see chunk_write_validate_
 * payload) and diverges from CHUNK_WRITE on two axes:
 *
 *   1. Stateid requirement.  CHUNK_WRITE accepts special stateids
 *      (anonymous, read-bypass) and bypasses the trust-table check
 *      for them.  CHUNK_WRITE_REPAIR rejects special stateids
 *      (NFS4ERR_BAD_STATEID) and additionally requires the trust
 *      entry's iomode to be LAYOUTIOMODE4_RW (NFS4ERR_ACCESS for
 *      a READ-only entry).  See .claude/design/ec-repair.md sec 4.
 *
 *   2. Concurrency control.  CHUNK_WRITE has the Track 1b chunk-
 *      collision gate (Option C, lines 393-424 above) that rejects
 *      writes landing on a PENDING block owned by a different
 *      writer.  CHUNK_WRITE_REPAIR explicitly bypasses that gate
 *      because the layout-layer (FFV2_DS_FLAGS_REPAIR +
 *      iomode=RW trust entry) gates concurrency at a higher level:
 *      a repair client is the sole authorised writer for the slot
 *      while its repair-flagged layout is in effect.
 *
 *      NOT_NOW_BROWN_COW: tighter concurrency control inside the
 *      repair-write path itself is needed if the MDS ever issues a
 *      repair-flagged layout while a normal writer is mid-PENDING.
 *      The demo cells do not exercise that case (the MDS issues
 *      repair only when the mirror is known-missing data, not
 *      mid-write); the production scenario needs a synchronisation
 *      with the normal-writer path.
 *
 * Every touched block gets CHUNK_BLOCK_REPAIR_PROVENANCE set in
 * cb_flags -- purely informational (operator audit trail for "this
 * block was the result of an EC repair, not a normal write");
 * persisted via cbd_flags.
 *
 * cs_repair_initiated is bumped once per call (the existing INV-1
 * counter; the stub also bumped it).  cs_repair_completed is the
 * MDS-side counter that the OP_CHUNK_REPAIRED handler bumps when a
 * client tells the MDS the repair landed -- that handler is Slice
 * 2 of the ec-repair work.
 */
uint32_t nfs4_op_chunk_write_repair(struct compound *compound)
{
	CHUNK_WRITE_REPAIR4args *args =
		NFS4_OP_ARG_SETUP(compound, opchunk_write_repair);
	CHUNK_WRITE_REPAIR4res *res =
		NFS4_OP_RES_SETUP(compound, opchunk_write_repair);
	nfsstat4 *status = &res->cwrr_status;
	CHUNK_WRITE_REPAIR4resok *resok =
		NFS4_OP_RESOK_SETUP(res, CHUNK_WRITE_REPAIR4res_u, cwrr_resok4);

	uint32_t wire_algo;
	uint32_t nchunks;
	nfsstat4 vs = chunk_write_validate_payload(
		compound, args->cwra_chunk_size,
		args->cwra_chunks.cwra_chunks_val,
		args->cwra_chunks.cwra_chunks_len,
		args->cwra_owner.co_guard.cg_client_id,
		args->cwra_checksums.cwra_checksums_val,
		args->cwra_checksums.cwra_checksums_len, "CHUNK_WRITE_REPAIR",
		&wire_algo, &nchunks);

	if (vs != NFS4_OK) {
		*status = vs;
		return 0;
	}

	uint32_t chunk_size = args->cwra_chunk_size;
	uint32_t total_data = args->cwra_chunks.cwra_chunks_len;
	uint32_t nchecksums = args->cwra_checksums.cwra_checksums_len;

	/*
	 * Stateid auth (rules 6/7/8 of .claude/design/ec-repair.md sec
	 * 4).  Repair MUST use a real layout stateid -- special
	 * stateids do not carry authorisation for a repair-write.
	 */
	if (stateid4_is_special(&args->cwra_stateid)) {
		*status = NFS4ERR_BAD_STATEID;
		return 0;
	}

	struct trust_entry *te = trust_stateid_find(&args->cwra_stateid);

	if (!te) {
		*status = NFS4ERR_BAD_STATEID;
		return 0;
	}

	uint64_t now = reffs_now_ns();
	uint64_t exp =
		atomic_load_explicit(&te->te_expire_ns, memory_order_acquire);
	uint32_t te_flags =
		atomic_load_explicit(&te->te_flags, memory_order_acquire);

	if (te_flags & TRUST_PENDING) {
		trust_entry_put(te);
		*status = NFS4ERR_DELAY;
		return 0;
	}
	if (exp != 0 && now > exp) {
		trust_entry_put(te);
		*status = NFS4ERR_BAD_STATEID;
		return 0;
	}
	if (!(te_flags & TRUST_ACTIVE)) {
		trust_entry_put(te);
		*status = NFS4ERR_BAD_STATEID;
		return 0;
	}
	if (te->te_iomode != LAYOUTIOMODE4_RW) {
		trust_entry_put(te);
		*status = NFS4ERR_ACCESS;
		return 0;
	}

	trust_entry_put(te);

	if (compound->c_curr_sb)
		atomic_fetch_add_explicit(
			&compound->c_curr_sb->sb_chunk_stats.cs_repair_initiated,
			1, memory_order_relaxed);

	pthread_mutex_lock(&compound->c_inode->i_attr_mutex);

	struct chunk_store *cs = chunk_store_get(
		compound->c_inode, compound->c_server_state->ss_state_dir);

	if (!cs) {
		pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);
		*status = NFS4ERR_DELAY;
		return 0;
	}

	/*
	 * Per-file algorithm consistency -- same rule as CHUNK_WRITE:
	 * the first write on a file establishes the checksum algorithm;
	 * later writes (including repair-writes) must match it.  An
	 * incoming repair-write with a different algorithm is a wire-
	 * level inconsistency and gets NFS4ERR_INVAL.
	 */
	if (nchecksums > 0) {
		if (cs->cs_checksum_algorithm == 0) {
			cs->cs_checksum_algorithm = wire_algo;
			cs->cs_dirty = true;
		} else if (cs->cs_checksum_algorithm != wire_algo) {
			TRACE("CHUNK_WRITE_REPAIR: per-file algorithm mismatch "
			      "(file=%u wire=%u)",
			      cs->cs_checksum_algorithm, wire_algo);
			pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);
			*status = NFS4ERR_INVAL;
			return 0;
		}
	}

	if (cs->cs_chunk_size == 0)
		cs->cs_chunk_size = chunk_size;

	/*
	 * CHUNK_WRITE's guarded-CAS and PENDING-collision gates are
	 * deliberately bypassed here -- see header comment for why.
	 */

	pthread_rwlock_wrlock(&compound->c_inode->i_db_rwlock);
	if (!compound->c_inode->i_db) {
		compound->c_inode->i_db = data_block_alloc(
			compound->c_inode, args->cwra_chunks.cwra_chunks_val,
			args->cwra_chunks.cwra_chunks_len,
			(off_t)args->cwra_offset * chunk_size);
		if (!compound->c_inode->i_db) {
			pthread_rwlock_unlock(&compound->c_inode->i_db_rwlock);
			pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);
			*status = NFS4ERR_NOSPC;
			return 0;
		}
	} else {
		ssize_t wret =
			data_block_write(compound->c_inode->i_db,
					 args->cwra_chunks.cwra_chunks_val,
					 args->cwra_chunks.cwra_chunks_len,
					 (off_t)args->cwra_offset * chunk_size);

		if (wret < 0) {
			pthread_rwlock_unlock(&compound->c_inode->i_db_rwlock);
			pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);
			*status = NFS4ERR_IO;
			return 0;
		}
	}

	int64_t new_end = (int64_t)args->cwra_offset * (int64_t)chunk_size +
			  (int64_t)total_data;

	if (new_end > compound->c_inode->i_size)
		compound->c_inode->i_size = new_end;

	pthread_rwlock_unlock(&compound->c_inode->i_db_rwlock);

	for (uint32_t i = 0; i < nchunks; i++) {
		uint32_t blk_size = chunk_size;

		if (i == nchunks - 1 && total_data % chunk_size != 0)
			blk_size = total_data % chunk_size;

		uint32_t blk_csum_algo = CHECKSUM_ALG_NONE;
		uint32_t blk_csum_len = 0;
		uint8_t blk_csum_value[CHUNK_VALUE_MAX] = { 0 };

		if (i < nchecksums) {
			const checksum4 *cs4 =
				&args->cwra_checksums.cwra_checksums_val[i];

			blk_csum_algo = cs4->cs_algorithm;
			blk_csum_len = cs4->cs_value.cs_value_len;
			if (blk_csum_len > sizeof(blk_csum_value))
				blk_csum_len = sizeof(blk_csum_value);
			memcpy(blk_csum_value, cs4->cs_value.cs_value_val,
			       blk_csum_len);
		}

		struct chunk_block blk = {
			.cb_state = CHUNK_STATE_PENDING,
			.cb_flags = CHUNK_BLOCK_REPAIR_PROVENANCE,
			.cb_gen_id = args->cwra_owner.co_guard.cg_gen_id,
			.cb_client_id = args->cwra_owner.co_guard.cg_client_id,
			.cb_owner_id = args->cwra_owner.co_id,
			.cb_payload_id = args->cwra_payload_id,
			.cb_checksum_algorithm = blk_csum_algo,
			.cb_checksum_len = blk_csum_len,
			.cb_chunk_size = blk_size,
			.cb_writer_clientid =
				compound->c_nfs4_client ?
					nfs4_client_to_client(
						compound->c_nfs4_client)
						->c_id :
					0,
		};

		memcpy(blk.cb_checksum_value, blk_csum_value,
		       sizeof(blk.cb_checksum_value));

		if (chunk_store_write(cs, args->cwra_offset + i, &blk) < 0) {
			pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);
			*status = NFS4ERR_DELAY;
			return 0;
		}
	}

	pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);

	resok->cwrr_count = nchunks;
	resok->cwrr_committed = (args->cwra_stable == FILE_SYNC4) ? FILE_SYNC4 :
								    UNSTABLE4;
	chunk_write_verf(compound->c_server_state, resok->cwrr_writeverf);

	resok->cwrr_status.cwrr_status_val = calloc(nchunks, sizeof(nfsstat4));
	if (!resok->cwrr_status.cwrr_status_val) {
		*status = NFS4ERR_DELAY;
		return 0;
	}
	resok->cwrr_status.cwrr_status_len = nchunks;

	return 0;
}
