/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * NFSv4.2 CHUNK I/O for the EC demo client.
 *
 * Sends CHUNK_WRITE, CHUNK_READ, CHUNK_FINALIZE, CHUNK_COMMIT
 * compounds to data servers via NFSv4.2.  Each DS has its own
 * mds_session (reusing the compound builder infrastructure).
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

#include "nfsv42_xdr.h"
#include "ec_client.h"

/* ------------------------------------------------------------------ */
/* CHUNK_WRITE                                                         */
/* ------------------------------------------------------------------ */

int ds_chunk_write(struct mds_session *ds, const uint8_t *fh, uint32_t fh_len,
		   uint64_t block_offset, uint32_t chunk_size,
		   const uint8_t *data, uint32_t data_len, uint32_t owner_id,
		   const stateid4 *stateid)
{
	struct mds_compound mc;
	nfs_argop4 *slot;
	int ret;

	/* SEQUENCE + PUTFH + CHUNK_WRITE = 3 ops */
	ret = mds_compound_init(&mc, 3, "chunk_write");
	if (ret)
		return ret;

	ret = mds_compound_add_sequence(&mc, ds);
	if (ret)
		goto out;

	slot = mds_compound_add_op(&mc, OP_PUTFH);
	if (!slot) {
		ret = -ENOSPC;
		goto out;
	}
	slot->nfs_argop4_u.opputfh.object.nfs_fh4_len = fh_len;
	slot->nfs_argop4_u.opputfh.object.nfs_fh4_val = (char *)fh;

	slot = mds_compound_add_op(&mc, OP_CHUNK_WRITE);
	if (!slot) {
		ret = -ENOSPC;
		goto out;
	}

	CHUNK_WRITE4args *cwa = &slot->nfs_argop4_u.opchunk_write;

	/*
	 * Use the real layout stateid for tight coupling (TRUST_STATEID),
	 * or the anonymous stateid for the traditional unauthenticated path.
	 */
	if (stateid)
		memcpy(&cwa->cwa_stateid, stateid, sizeof(stateid4));
	else
		memset(&cwa->cwa_stateid, 0, sizeof(cwa->cwa_stateid));
	cwa->cwa_offset = block_offset;
	cwa->cwa_stable = FILE_SYNC4;
	cwa->cwa_owner.co_guard.cg_gen_id = 1;
	cwa->cwa_owner.co_guard.cg_client_id = 1;
	cwa->cwa_owner.co_id = owner_id;
	cwa->cwa_payload_id = 0;
	cwa->cwa_flags = 0;
	cwa->cwa_guard.cwg_check = FALSE;
	cwa->cwa_chunk_size = chunk_size;

	/*
	 * Compute CRC32 per chunk.  The last chunk may be shorter than
	 * chunk_size when data_len is not a multiple (Mojette parity
	 * projections produce variable-sized shards).
	 */
	uint32_t nchunks = (data_len + chunk_size - 1) / chunk_size;

	cwa->cwa_crc32s.cwa_crc32s_len = nchunks;
	cwa->cwa_crc32s.cwa_crc32s_val = calloc(nchunks, sizeof(uint32_t));
	if (!cwa->cwa_crc32s.cwa_crc32s_val) {
		ret = -ENOMEM;
		goto out;
	}

	for (uint32_t i = 0; i < nchunks; i++) {
		uint32_t clen = chunk_size;

		if (i == nchunks - 1 && data_len % chunk_size != 0)
			clen = data_len % chunk_size;
		cwa->cwa_crc32s.cwa_crc32s_val[i] = (uint32_t)crc32(
			0L, data + (size_t)i * chunk_size, (uInt)clen);
	}

	cwa->cwa_chunks.cwa_chunks_len = data_len;
	cwa->cwa_chunks.cwa_chunks_val = (char *)data;

	ret = mds_compound_send(&mc, ds);
	/*
	 * mds_compound_send returns -EREMOTEIO when any op in the
	 * COMPOUND replied with status != NFS4_OK; mc_res.status holds
	 * the failing op's status (RFC 8881 S15.1.4).  When that op is
	 * CHUNK_WRITE failing with NFS4ERR_BAD_STATEID -- the trust-
	 * stateid revocation signal -- map to -ESTALE so the inner
	 * retry in ec_chunk_write and the outer retry in
	 * ec_write_codec (slice 1.6) can recognise it.  Without this
	 * remap the BAD_STATEID surfaces as -EREMOTEIO, the per-op
	 * status check below is unreachable, and the retry path is
	 * effectively dead for the only error mode it was designed
	 * to handle.
	 */
	if (ret == -EREMOTEIO && mc.mc_res.status == NFS4ERR_BAD_STATEID)
		ret = -ESTALE;
	if (ret)
		goto out_crc;

	nfs_resop4 *res_slot = mds_compound_result(&mc, 2);

	if (!res_slot) {
		ret = -EIO;
	} else {
		nfsstat4 st = res_slot->nfs_resop4_u.opchunk_write.cwr_status;

		if (st != NFS4_OK)
			ret = (st == NFS4ERR_BAD_STATEID) ? -ESTALE : -EIO;
	}

out_crc:
	/* Don't let mds_compound_fini free our caller's data buffer. */
	cwa->cwa_chunks.cwa_chunks_val = NULL;
	cwa->cwa_chunks.cwa_chunks_len = 0;
	free(cwa->cwa_crc32s.cwa_crc32s_val);
	cwa->cwa_crc32s.cwa_crc32s_val = NULL;
	cwa->cwa_crc32s.cwa_crc32s_len = 0;
out:
	/* Don't let fini free our caller's FH. */
	if (mc.mc_args.argarray.argarray_len > 1) {
		nfs_argop4 *pfh = &mc.mc_args.argarray.argarray_val[1];
		pfh->nfs_argop4_u.opputfh.object.nfs_fh4_val = NULL;
	}

	mds_compound_fini(&mc);
	return ret;
}

/* ------------------------------------------------------------------ */
/* CHUNK_READ                                                          */
/* ------------------------------------------------------------------ */

int ds_chunk_read(struct mds_session *ds, const uint8_t *fh, uint32_t fh_len,
		  uint64_t block_offset, uint32_t count, uint8_t *out_data,
		  uint32_t chunk_size, uint32_t *nread, const stateid4 *stateid)
{
	struct mds_compound mc;
	nfs_argop4 *slot;
	int ret;

	*nread = 0;

	ret = mds_compound_init(&mc, 3, "chunk_read");
	if (ret)
		return ret;

	ret = mds_compound_add_sequence(&mc, ds);
	if (ret)
		goto out;

	slot = mds_compound_add_op(&mc, OP_PUTFH);
	if (!slot) {
		ret = -ENOSPC;
		goto out;
	}
	slot->nfs_argop4_u.opputfh.object.nfs_fh4_len = fh_len;
	slot->nfs_argop4_u.opputfh.object.nfs_fh4_val = (char *)fh;

	slot = mds_compound_add_op(&mc, OP_CHUNK_READ);
	if (!slot) {
		ret = -ENOSPC;
		goto out;
	}

	CHUNK_READ4args *cra = &slot->nfs_argop4_u.opchunk_read;

	/*
	 * Use the real layout stateid for tight coupling, anonymous otherwise.
	 */
	if (stateid)
		memcpy(&cra->cra_stateid, stateid, sizeof(stateid4));
	else
		memset(&cra->cra_stateid, 0, sizeof(cra->cra_stateid));
	cra->cra_offset = block_offset;
	cra->cra_count = count;

	ret = mds_compound_send(&mc, ds);
	/* See ds_chunk_write: surface CHUNK_READ BAD_STATEID as -ESTALE
	 * so the slice 1.6 retry path can recognise it.
	 */
	if (ret == -EREMOTEIO && mc.mc_res.status == NFS4ERR_BAD_STATEID)
		ret = -ESTALE;
	if (ret)
		goto out;

	nfs_resop4 *res_slot = mds_compound_result(&mc, 2);

	if (!res_slot) {
		ret = -EIO;
		goto out;
	}

	if (res_slot->nfs_resop4_u.opchunk_read.crr_status != NFS4_OK) {
		nfsstat4 st = res_slot->nfs_resop4_u.opchunk_read.crr_status;

		ret = (st == NFS4ERR_BAD_STATEID) ? -ESTALE : -EIO;
		goto out;
	}

	CHUNK_READ4resok *resok =
		&res_slot->nfs_resop4_u.opchunk_read.CHUNK_READ4res_u.crr_resok4;

	uint32_t got = resok->crr_chunks.crr_chunks_len;

	for (uint32_t i = 0; i < got; i++) {
		read_chunk4 *rc = &resok->crr_chunks.crr_chunks_val[i];
		uint32_t copy = rc->cr_chunk.cr_chunk_len;

		if (copy > chunk_size)
			copy = chunk_size;
		memcpy(out_data + (size_t)i * chunk_size,
		       rc->cr_chunk.cr_chunk_val, copy);

		/*
		 * Verify CRC from server against received data.
		 * Detects network corruption on the read path.
		 */
		if (rc->cr_crc != 0) {
			uint32_t wire_crc = (uint32_t)crc32(
				0L, (const Bytef *)rc->cr_chunk.cr_chunk_val,
				(uInt)copy);

			if (wire_crc != rc->cr_crc) {
				fprintf(stderr,
					"ds_chunk_read: CRC mismatch "
					"block %u: server 0x%08x "
					"wire 0x%08x\n",
					i, rc->cr_crc, wire_crc);
				ret = -EIO;
				goto out;
			}
		}
	}

	*nread = got;

out:
	/* Don't free caller's FH. */
	if (mc.mc_args.argarray.argarray_len > 1) {
		nfs_argop4 *pfh = &mc.mc_args.argarray.argarray_val[1];

		pfh->nfs_argop4_u.opputfh.object.nfs_fh4_val = NULL;
	}
	mds_compound_fini(&mc);
	return ret;
}

/* ------------------------------------------------------------------ */
/* CHUNK_FINALIZE                                                      */
/* ------------------------------------------------------------------ */

int ds_chunk_finalize(struct mds_session *ds, const uint8_t *fh,
		      uint32_t fh_len, uint64_t block_offset, uint32_t count,
		      uint32_t owner_id)
{
	struct mds_compound mc;
	nfs_argop4 *slot;
	int ret;

	ret = mds_compound_init(&mc, 3, "chunk_finalize");
	if (ret)
		return ret;

	ret = mds_compound_add_sequence(&mc, ds);
	if (ret)
		goto out;

	slot = mds_compound_add_op(&mc, OP_PUTFH);
	if (!slot) {
		ret = -ENOSPC;
		goto out;
	}
	slot->nfs_argop4_u.opputfh.object.nfs_fh4_len = fh_len;
	slot->nfs_argop4_u.opputfh.object.nfs_fh4_val = (char *)fh;

	slot = mds_compound_add_op(&mc, OP_CHUNK_FINALIZE);
	if (!slot) {
		ret = -ENOSPC;
		goto out;
	}

	CHUNK_FINALIZE4args *cfa = &slot->nfs_argop4_u.opchunk_finalize;

	cfa->cfa_offset = block_offset;
	cfa->cfa_count = count;
	cfa->cfa_chunks.cfa_chunks_len = 1;
	cfa->cfa_chunks.cfa_chunks_val = calloc(1, sizeof(chunk_owner4));
	if (!cfa->cfa_chunks.cfa_chunks_val) {
		ret = -ENOMEM;
		goto out;
	}
	cfa->cfa_chunks.cfa_chunks_val[0].co_guard.cg_gen_id = 1;
	cfa->cfa_chunks.cfa_chunks_val[0].co_guard.cg_client_id = 1;
	cfa->cfa_chunks.cfa_chunks_val[0].co_id = owner_id;

	ret = mds_compound_send(&mc, ds);
	if (ret)
		goto out;

	nfs_resop4 *res_slot = mds_compound_result(&mc, 2);

	if (!res_slot ||
	    res_slot->nfs_resop4_u.opchunk_finalize.cfr_status != NFS4_OK)
		ret = -EIO;

out:
	if (mc.mc_args.argarray.argarray_len > 2) {
		nfs_argop4 *op = &mc.mc_args.argarray.argarray_val[2];

		free(op->nfs_argop4_u.opchunk_finalize.cfa_chunks
			     .cfa_chunks_val);
		op->nfs_argop4_u.opchunk_finalize.cfa_chunks.cfa_chunks_val =
			NULL;
	}
	if (mc.mc_args.argarray.argarray_len > 1) {
		nfs_argop4 *pfh = &mc.mc_args.argarray.argarray_val[1];

		pfh->nfs_argop4_u.opputfh.object.nfs_fh4_val = NULL;
	}
	mds_compound_fini(&mc);
	return ret;
}

/* ------------------------------------------------------------------ */
/* CHUNK_COMMIT                                                        */
/* ------------------------------------------------------------------ */

int ds_chunk_commit(struct mds_session *ds, const uint8_t *fh, uint32_t fh_len,
		    uint64_t block_offset, uint32_t count, uint32_t owner_id)
{
	struct mds_compound mc;
	nfs_argop4 *slot;
	int ret;

	ret = mds_compound_init(&mc, 3, "chunk_commit");
	if (ret)
		return ret;

	ret = mds_compound_add_sequence(&mc, ds);
	if (ret)
		goto out;

	slot = mds_compound_add_op(&mc, OP_PUTFH);
	if (!slot) {
		ret = -ENOSPC;
		goto out;
	}
	slot->nfs_argop4_u.opputfh.object.nfs_fh4_len = fh_len;
	slot->nfs_argop4_u.opputfh.object.nfs_fh4_val = (char *)fh;

	slot = mds_compound_add_op(&mc, OP_CHUNK_COMMIT);
	if (!slot) {
		ret = -ENOSPC;
		goto out;
	}

	CHUNK_COMMIT4args *cca = &slot->nfs_argop4_u.opchunk_commit;

	cca->cca_offset = block_offset;
	cca->cca_count = count;
	cca->cca_chunks.cca_chunks_len = 1;
	cca->cca_chunks.cca_chunks_val = calloc(1, sizeof(chunk_owner4));
	if (!cca->cca_chunks.cca_chunks_val) {
		ret = -ENOMEM;
		goto out;
	}
	cca->cca_chunks.cca_chunks_val[0].co_guard.cg_gen_id = 1;
	cca->cca_chunks.cca_chunks_val[0].co_guard.cg_client_id = 1;
	cca->cca_chunks.cca_chunks_val[0].co_id = owner_id;

	ret = mds_compound_send(&mc, ds);
	if (ret)
		goto out;

	nfs_resop4 *res_slot = mds_compound_result(&mc, 2);

	if (!res_slot ||
	    res_slot->nfs_resop4_u.opchunk_commit.ccr_status != NFS4_OK)
		ret = -EIO;

out:
	if (mc.mc_args.argarray.argarray_len > 2) {
		nfs_argop4 *op = &mc.mc_args.argarray.argarray_val[2];

		free(op->nfs_argop4_u.opchunk_commit.cca_chunks.cca_chunks_val);
		op->nfs_argop4_u.opchunk_commit.cca_chunks.cca_chunks_val =
			NULL;
	}
	if (mc.mc_args.argarray.argarray_len > 1) {
		nfs_argop4 *pfh = &mc.mc_args.argarray.argarray_val[1];

		pfh->nfs_argop4_u.opputfh.object.nfs_fh4_val = NULL;
	}
	mds_compound_fini(&mc);
	return ret;
}
