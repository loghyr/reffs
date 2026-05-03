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
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <zlib.h>

#include "nfsv42_xdr.h"
#include "reffs/data_block.h"
#include "reffs/fs.h"
#include "reffs/inode.h"
#include "reffs/log.h"
#include "reffs/rpc.h"
#include "reffs/server.h"
#include "reffs/time.h"
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
	 * The chunks opaque blob contains one or more chunks.  Most are
	 * cwa_chunk_size bytes; the last may be shorter when the shard
	 * size is not a multiple of chunk_size (Mojette parity projections
	 * produce variable-sized shards).
	 */
	uint32_t total_data = args->cwa_chunks.cwa_chunks_len;
	uint32_t nchunks = (total_data + chunk_size - 1) / chunk_size;

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
		uint32_t clen = chunk_size;

		if (i == nchunks - 1 && total_data % chunk_size != 0)
			clen = total_data % chunk_size;
		uint32_t computed = (uint32_t)crc32(0L, cdata, (uInt)clen);

		if (computed != args->cwa_crc32s.cwa_crc32s_val[i]) {
			TRACE("CHUNK_WRITE: CRC mismatch chunk %u: "
			      "expected 0x%08x got 0x%08x",
			      i, args->cwa_crc32s.cwa_crc32s_val[i], computed);
			*status = NFS4ERR_INVAL;
			return 0;
		}
	}

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

	/* Record the nominal chunk size (disk stride for offset calc). */
	if (cs->cs_chunk_size == 0)
		cs->cs_chunk_size = chunk_size;

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

		struct chunk_block blk = {
			.cb_state = CHUNK_STATE_PENDING,
			.cb_gen_id = args->cwa_owner.co_guard.cg_gen_id,
			.cb_client_id = args->cwa_owner.co_guard.cg_client_id,
			.cb_owner_id = args->cwa_owner.co_id,
			.cb_payload_id = args->cwa_payload_id,
			.cb_crc32 = (args->cwa_crc32s.cwa_crc32s_len > i) ?
					    args->cwa_crc32s.cwa_crc32s_val[i] :
					    0,
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
		 * Verify stored CRC against data read from disk.
		 * Detects silent data corruption (bit rot).
		 */
		if (blk->cb_crc32 != 0) {
			uint32_t disk_crc = (uint32_t)crc32(
				0L, (const Bytef *)rc->cr_chunk.cr_chunk_val,
				(uInt)blk->cb_chunk_size);

			if (disk_crc != blk->cb_crc32) {
				LOG("CHUNK_READ: CRC mismatch block %" PRIu64
				    ": stored 0x%08x disk 0x%08x",
				    off, blk->cb_crc32, disk_crc);
				rc->cr_crc = disk_crc;
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

uint32_t nfs4_op_chunk_repaired(struct compound *compound)
{
	CHUNK_REPAIRED4res *res = NFS4_OP_RES_SETUP(compound, opchunk_repair);
	nfsstat4 *status = &res->cpr_status;

	*status = NFS4ERR_NOTSUPP;

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

uint32_t nfs4_op_chunk_write_repair(struct compound *compound)
{
	CHUNK_WRITE_REPAIR4res *res =
		NFS4_OP_RES_SETUP(compound, opchunk_write_repair);
	nfsstat4 *status = &res->cwrr_status;

	*status = NFS4ERR_NOTSUPP;

	return 0;
}
