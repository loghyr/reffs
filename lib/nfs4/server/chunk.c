/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * CHUNK operations — data server block-level I/O.
 *
 * CHUNK_WRITE: store encoded chunks at a block offset, record
 * per-block metadata in the chunk store (PENDING state).
 *
 * CHUNK_FINALIZE: transition PENDING → FINALIZED (visible to owner).
 *
 * CHUNK_COMMIT: transition FINALIZED → COMMITTED (visible to all).
 *
 * CHUNK_READ: read committed (or owner's finalized) chunks.
 *
 * Happy-path implementation: no guard conflicts, no locks.
 */

#include <stdlib.h>
#include <string.h>

#include <zlib.h>

#include "nfsv42_xdr.h"
#include "reffs/data_block.h"
#include "reffs/inode.h"
#include "reffs/log.h"
#include "reffs/rpc.h"
#include "reffs/server.h"
#include "nfs4/chunk_store.h"
#include "nfs4/compound.h"
#include "nfs4/ops.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void chunk_write_verf(struct server_state *ss, verifier4 out_verf)
{
	memcpy(out_verf, ss->ss_uuid, NFS4_VERIFIER_SIZE - 2);
	uint16_t boot_seq = server_boot_seq(ss);

	memcpy(out_verf + NFS4_VERIFIER_SIZE - 2, &boot_seq, 2);
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

	if (network_file_handle_empty(&compound->c_curr_nfh)) {
		*status = NFS4ERR_NOFILEHANDLE;
		return 0;
	}

	if (!compound->c_inode || !S_ISREG(compound->c_inode->i_mode)) {
		*status = NFS4ERR_INVAL;
		return 0;
	}

	uint32_t chunk_size = args->cwa_chunk_size;

	if (chunk_size == 0 || args->cwa_chunks.cwa_chunks_len == 0) {
		*status = NFS4ERR_INVAL;
		return 0;
	}

	/*
	 * The chunks opaque blob contains one or more chunks of
	 * cwa_chunk_size bytes each.
	 */
	uint32_t nchunks = args->cwa_chunks.cwa_chunks_len / chunk_size;

	if (nchunks == 0) {
		*status = NFS4ERR_INVAL;
		return 0;
	}

	/* Validate CRC32 for each chunk if provided. */
	if (args->cwa_crc32s.cwa_crc32s_len > 0 &&
	    args->cwa_crc32s.cwa_crc32s_len != nchunks) {
		*status = NFS4ERR_INVAL;
		return 0;
	}

	for (uint32_t i = 0; i < args->cwa_crc32s.cwa_crc32s_len; i++) {
		const uint8_t *cdata =
			(const uint8_t *)args->cwa_chunks.cwa_chunks_val +
			(size_t)i * chunk_size;
		uint32_t computed =
			(uint32_t)crc32(0L, cdata, (uInt)chunk_size);

		if (computed != args->cwa_crc32s.cwa_crc32s_val[i]) {
			TRACE("CHUNK_WRITE: CRC mismatch chunk %u: "
			      "expected 0x%08x got 0x%08x",
			      i, args->cwa_crc32s.cwa_crc32s_val[i], computed);
			*status = NFS4ERR_INVAL;
			return 0;
		}
	}

	pthread_mutex_lock(&compound->c_inode->i_attr_mutex);

	struct chunk_store *cs = chunk_store_get(compound->c_inode);

	if (!cs) {
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
	int64_t new_end =
		(int64_t)(args->cwa_offset + nchunks) * (int64_t)chunk_size;
	if (new_end > compound->c_inode->i_size)
		compound->c_inode->i_size = new_end;

	pthread_rwlock_unlock(&compound->c_inode->i_db_rwlock);

	/* Record per-block metadata. */
	for (uint32_t i = 0; i < nchunks; i++) {
		struct chunk_block blk = {
			.cb_state = CHUNK_STATE_PENDING,
			.cb_gen_id = args->cwa_owner.co_guard.cg_gen_id,
			.cb_client_id = args->cwa_owner.co_guard.cg_client_id,
			.cb_owner_id = args->cwa_owner.co_id,
			.cb_payload_id = args->cwa_payload_id,
			.cb_crc32 = (args->cwa_crc32s.cwa_crc32s_len > i) ?
					    args->cwa_crc32s.cwa_crc32s_val[i] :
					    0,
			.cb_chunk_size = chunk_size,
		};

		if (chunk_store_write(cs, args->cwa_offset + i, &blk) < 0) {
			pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);
			*status = NFS4ERR_DELAY;
			return 0;
		}
	}

	pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);

	resok->cwr_count = nchunks;
	resok->cwr_committed = (args->cwa_stable == FILE_SYNC4) ? FILE_SYNC4 :
								  UNSTABLE4;
	chunk_write_verf(compound->c_server_state, resok->cwr_writeverf);

	/* Return per-chunk status (all OK for happy path). */
	resok->cwr_status.cwr_status_val = calloc(nchunks, sizeof(nfsstat4));
	if (!resok->cwr_status.cwr_status_val) {
		*status = NFS4ERR_DELAY;
		return 0;
	}
	resok->cwr_status.cwr_status_len = nchunks;
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

	pthread_mutex_lock(&compound->c_inode->i_attr_mutex);

	struct chunk_store *cs = compound->c_inode->i_chunk_store;

	if (!cs) {
		pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);
		*status = NFS4ERR_NOENT;
		return 0;
	}

	/* Determine how many blocks are available. */
	uint32_t avail = 0;

	for (uint32_t i = 0; i < count; i++) {
		struct chunk_block *blk =
			chunk_store_lookup(cs, args->cra_offset + i);

		if (blk && blk->cb_state >= CHUNK_STATE_FINALIZED)
			avail++;
		else
			break;
	}

	if (avail == 0) {
		pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);
		*status = NFS4ERR_NOENT;
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

		rc->cr_crc = blk->cb_crc32;
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

		if (compound->c_inode->i_db) {
			data_block_read(compound->c_inode->i_db,
					rc->cr_chunk.cr_chunk_val,
					blk->cb_chunk_size,
					(off_t)(off * blk->cb_chunk_size));
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

	pthread_mutex_lock(&compound->c_inode->i_attr_mutex);

	struct chunk_store *cs = compound->c_inode->i_chunk_store;

	if (!cs) {
		pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);
		*status = NFS4ERR_NOENT;
		return 0;
	}

	/*
	 * Transition each owner's blocks from PENDING → FINALIZED.
	 * The cfa_chunks array lists the chunk_owner4 entries to finalize.
	 */
	uint32_t nowners = args->cfa_chunks.cfa_chunks_len;

	resok->ccr_status.ccr_status_val = calloc(nowners, sizeof(nfsstat4));
	if (!resok->ccr_status.ccr_status_val) {
		pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);
		*status = NFS4ERR_DELAY;
		return 0;
	}
	resok->ccr_status.ccr_status_len = nowners;

	for (uint32_t i = 0; i < nowners; i++) {
		chunk_owner4 *co = &args->cfa_chunks.cfa_chunks_val[i];
		int ret = chunk_store_transition(cs, args->cfa_offset, count,
						 co->co_id, CHUNK_STATE_PENDING,
						 CHUNK_STATE_FINALIZED);
		resok->ccr_status.ccr_status_val[i] =
			(ret == 0) ? NFS4_OK : NFS4ERR_INVAL;
	}

	pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);

	chunk_write_verf(compound->c_server_state, resok->ccr_writeverf);

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

	pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);

	chunk_write_verf(compound->c_server_state, resok->ccr_writeverf);

	/* Sync to disk for FILE_SYNC4 semantics. */
	inode_sync_to_disk(compound->c_inode);

	return 0;
}

/* ------------------------------------------------------------------ */
/* Stub ops — not needed for happy-path demo                           */
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

uint32_t nfs4_op_chunk_repaired(struct compound *compound)
{
	CHUNK_REPAIRED4res *res = NFS4_OP_RES_SETUP(compound, opchunk_repair);
	nfsstat4 *status = &res->crr_status;

	*status = NFS4ERR_NOTSUPP;

	return 0;
}

uint32_t nfs4_op_chunk_rollback(struct compound *compound)
{
	CHUNK_ROLLBACK4res *res = NFS4_OP_RES_SETUP(compound, opchunk_rollback);
	nfsstat4 *status = &res->crbr_status;

	*status = NFS4ERR_NOTSUPP;

	return 0;
}

uint32_t nfs4_op_chunk_unlock(struct compound *compound)
{
	CHUNK_UNLOCK4res *res = NFS4_OP_RES_SETUP(compound, opchunk_unlock);
	nfsstat4 *status = &res->cur_status;

	*status = NFS4ERR_NOTSUPP;

	return 0;
}

uint32_t nfs4_op_chunk_write_repair(struct compound *compound)
{
	CHUNK_WRITE_REPAIR4res *res =
		NFS4_OP_RES_SETUP(compound, opchunk_write_repair);
	nfsstat4 *status = &res->cwrr_status;

	*status = NFS4ERR_NOTSUPP;

	return 0;
}
