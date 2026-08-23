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
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <zlib.h>

#include "nfsv42_xdr.h"
#include "ec_client.h"
#include "nfs4/chunk_checksum.h"

/* ------------------------------------------------------------------ */
/* Per-process chunk-write identity                                    */
/*                                                                     */
/* Each writer process needs a UNIQUE cg_client_id so the server's     */
/* CAS-guard check (cwa_guard.cwg_check=TRUE) can distinguish writers. */
/* Pre-Option-C all writers hard-coded {cg_gen_id=1, cg_client_id=1}   */
/* which collapsed every block's "writer identity" to the same tuple   */
/* -- the guard CAS could not see cross-writer contention even when    */
/* it tried.                                                           */
/*                                                                     */
/* Derive cg_client_id from getpid() (with the two reserved sentinels  */
/* avoided per draft-haynes-nfsv4-flexfiles-v2 sec-chunk_guard_none /  */
/* sec-chunk_guard_mds: 0x00000000 and 0xFFFFFFFF respectively).       */
/*                                                                     */
/* cg_gen_id is NOT minted here.  The data server owns it: it stores   */
/* old+1 on every accepted CHUNK_WRITE and returns the current value   */
/* in cr_guard on CHUNK_READ.  A writer only ever echoes back the      */
/* generation it read, which is what makes the CAS a real check --     */
/* while the client picked the generation, it supplied both sides of   */
/* the server's comparison and the guard could not fail.               */
/* ------------------------------------------------------------------ */

/*
 * chunk_writer_client_id -- the cg_client_id to stamp on a CHUNK
 * operation against this mirror.
 *
 * draft-haynes-nfsv4-flexfiles-v2: the writer identity belongs to the
 * metadata server.  It assigns it, publishes it to the client as the
 * layout's ffv2m_client_id, and registers the same value with each
 * data server as tsa_client_id; a data server rejects a CHUNK
 * operation presenting anything else with NFS4ERR_BAD_STATEID,
 * because a client presenting another writer's identity is
 * indistinguishable from one spoofing it.  So when the layout carries
 * an identity, echo it -- this is not the client's choice to make.
 *
 * layout_client_id == CHUNK_GUARD_CLIENT_ID_NONE means the metadata
 * server assigned nothing.  Presenting NONE would be rejected outright
 * (it is a reserved sentinel), so fall back to the older self-assigned
 * value, which is what every metadata server expected before the
 * identity became server-assigned.  A data server that registered no
 * binding does not compare, so this stays interoperable in both
 * directions; one that did register a binding will reject us, which is
 * the correct answer to a metadata server and data server that
 * disagree.
 */
static uint32_t chunk_writer_client_id(uint32_t layout_client_id)
{
	if (layout_client_id != CHUNK_GUARD_CLIENT_ID_NONE &&
	    layout_client_id != CHUNK_GUARD_CLIENT_ID_MDS)
		return layout_client_id;

	pid_t pid = getpid();
	uint32_t cid = (uint32_t)pid;
	/* Sentinels NONE=0, MDS=0xFFFFFFFF: rotate either away to keep
	 * the wire valid; pid==0 is impossible but the check is cheap.
	 * pid 0xFFFFFFFF can't occur on Linux/Darwin either, but the
	 * defensive check costs nothing. */
	if (cid == 0)
		cid = 1;
	else if (cid == 0xFFFFFFFFu)
		cid = 0xFFFFFFFEu;
	return cid;
}

/*
 * A client-side generation counter used to live here and was written
 * into chunk_owner4.co_guard.cg_gen_id.  Removed: cg_gen_id is the
 * data server's per-chunk counter (draft sec-chunk_guard4), so a
 * writer choosing its own value made the compare-and-swap compare
 * against something the writer controlled.  Clients state an EXPECTED
 * generation via cwa_guard; they do not mint one.
 */

/* ------------------------------------------------------------------ */
/* CHUNK_WRITE                                                         */
/* ------------------------------------------------------------------ */

int ds_chunk_write(struct mds_session *ds, const uint8_t *fh, uint32_t fh_len,
		   uint64_t block_offset, uint32_t chunk_size,
		   const uint8_t *data, uint32_t data_len, uint64_t cohort_id,
		   uint32_t owner_id, uint32_t layout_client_id,
		   const stateid4 *stateid, const chunk_guard4 *guard)
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
	/*
	 * No generation here: cg_gen_id is the DATA SERVER's
	 * per-chunk counter, not something a writer chooses
	 * (draft sec-chunk_guard4).  The client states what it
	 * EXPECTS via cwa_guard when it wants a compare-and-swap;
	 * the owner carries transaction identity only.
	 *
	 * co_cohort_id names this write transaction and owner_id is the
	 * first co_id in the payload.  The compact carrier below emits one
	 * co_id per payload chunk.
	 */
	cwa->cwa_cohort_id = cohort_id;
	cwa->cwa_client_id = chunk_writer_client_id(layout_client_id);
	cwa->cwa_payload_id = 0;
	cwa->cwa_flags = 0;
	if (guard) {
		/*
		 * CAS guard: present the version the caller captured at
		 * its prior CHUNK_READ.  Server rejects with NFS4ERR_DELAY
		 * if the current block has a different {cg_gen_id,
		 * cg_client_id}; mapped below to -EAGAIN for the RMW retry
		 * path (see ec_write_encoding_range).
		 */
		cwa->cwa_guard.cwg_check = TRUE;
		cwa->cwa_guard.write_chunk_guard4_u.cwg_guard = *guard;
	} else {
		cwa->cwa_guard.cwg_check = FALSE;
	}
	cwa->cwa_chunk_size = chunk_size;

	/*
	 * Compute CRC32 per chunk and wrap each in a checksum4.  The
	 * last chunk may be shorter than chunk_size when data_len is
	 * not a multiple (Mojette parity projections produce variable-
	 * sized shards).
	 */
	uint32_t nchunks = (data_len + chunk_size - 1) / chunk_size;
	cwa->cwa_co_ids.cwa_co_ids_len = nchunks;
	cwa->cwa_co_ids.cwa_co_ids_val = calloc(nchunks, sizeof(uint32_t));
	if (!cwa->cwa_co_ids.cwa_co_ids_val) {
		ret = -ENOMEM;
		goto out;
	}
	for (uint32_t i = 0; i < nchunks; i++)
		cwa->cwa_co_ids.cwa_co_ids_val[i] = owner_id + i;

	cwa->cwa_checksums.cwa_checksums_len = nchunks;
	cwa->cwa_checksums.cwa_checksums_val =
		calloc(nchunks, sizeof(checksum4));
	if (!cwa->cwa_checksums.cwa_checksums_val) {
		ret = -ENOMEM;
		goto out;
	}

	for (uint32_t i = 0; i < nchunks; i++) {
		uint32_t clen = chunk_size;

		if (i == nchunks - 1 && data_len % chunk_size != 0)
			clen = data_len % chunk_size;
		uint32_t crc = (uint32_t)crc32(
			0L, data + (size_t)i * chunk_size, (uInt)clen);

		if (chunk_checksum_pack_crc32(
			    &cwa->cwa_checksums.cwa_checksums_val[i], crc) <
		    0) {
			ret = -ENOMEM;
			goto out_crc;
		}
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
	 * ec_write_encoding can recognise it.  Without this
	 * remap the BAD_STATEID surfaces as -EREMOTEIO, the per-op
	 * status check below is unreachable, and the retry path is
	 * effectively dead for the only error mode it was designed
	 * to handle.
	 */
	if (ret == -EREMOTEIO) {
		/* Surface the failing op's status so retry callers (and
		 * developers) can see WHICH op failed -- resarray_len
		 * tells us how far the COMPOUND got (1 = SEQUENCE failed,
		 * 2 = PUTFH failed, 3 = CHUNK_WRITE failed).
		 */
		fprintf(stderr,
			"ds_chunk_write: COMPOUND failed status=%u (resarray_len=%u)\n",
			(unsigned)mc.mc_res.status,
			mc.mc_res.resarray.resarray_len);
		if (mc.mc_res.status == NFS4ERR_BAD_STATEID)
			ret = -ESTALE;
		else if (mc.mc_res.status == NFS4ERR_DELAY)
			ret = -EAGAIN;
	}
	if (ret)
		goto out_crc;

	nfs_resop4 *res_slot = mds_compound_result(&mc, 2);

	if (!res_slot) {
		ret = -EIO;
	} else {
		nfsstat4 st = res_slot->nfs_resop4_u.opchunk_write.cwr_status;

		if (st != NFS4_OK) {
			if (st == NFS4ERR_BAD_STATEID)
				ret = -ESTALE;
			else if (st == NFS4ERR_DELAY)
				ret = -EAGAIN;
			else
				ret = -EIO;
		}
	}

out_crc:
	free(cwa->cwa_co_ids.cwa_co_ids_val);
	cwa->cwa_co_ids.cwa_co_ids_val = NULL;
	cwa->cwa_co_ids.cwa_co_ids_len = 0;
	/* Don't let mds_compound_fini free our caller's data buffer. */
	cwa->cwa_chunks.cwa_chunks_val = NULL;
	cwa->cwa_chunks.cwa_chunks_len = 0;
	/*
	 * Release the cs_value buffer inside each checksum4 entry
	 * before freeing the array.  mds_compound_fini cannot walk
	 * into the inner opaque<> buffers for us because it operates
	 * on the top-level argop array.
	 */
	if (cwa->cwa_checksums.cwa_checksums_val) {
		for (uint32_t i = 0; i < cwa->cwa_checksums.cwa_checksums_len;
		     i++) {
			free(cwa->cwa_checksums.cwa_checksums_val[i]
				     .cs_value.cs_value_val);
		}
		free(cwa->cwa_checksums.cwa_checksums_val);
	}
	cwa->cwa_checksums.cwa_checksums_val = NULL;
	cwa->cwa_checksums.cwa_checksums_len = 0;
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
/* CHUNK_WRITE_REPAIR                                                    */
/* ------------------------------------------------------------------ */

/*
 * ds_chunk_write_repair -- OP_CHUNK_WRITE_REPAIR to a data server.
 *
 * Wire-equivalent of ds_chunk_write minus the chunk_guard4 -- the
 * repair path bypasses CHUNK_WRITE's guarded-CAS gate because the
 * MDS-issued repair-flagged layout (FFV2_DS_FLAGS_REPAIR + iomode RW
 * trust entry) gates concurrency at the layout layer, not the chunk
 * layer.  See lib/nfs4/server/chunk.c nfs4_op_chunk_write_repair
 * for the matching server-side justification.
 *
 * Stateid is REQUIRED (not NULL) -- the repair handler rejects
 * special stateids with NFS4ERR_BAD_STATEID.  Caller passes the real
 * layout stateid from the LAYOUTGET that triggered the repair.
 *
 * Returns 0 on success, -ESTALE for NFS4ERR_BAD_STATEID, -EAGAIN
 * for NFS4ERR_DELAY, or -EIO for other status.  Mirrors
 * ds_chunk_write's mapping so the ec_repair_encoding retry loop can
 * reuse the same backoff logic.
 */
int ds_chunk_write_repair(struct mds_session *ds, const uint8_t *fh,
			  uint32_t fh_len, uint64_t block_offset,
			  uint32_t chunk_size, const uint8_t *data,
			  uint32_t data_len, uint64_t cohort_id,
			  uint32_t owner_id, uint32_t layout_client_id,
			  const stateid4 *stateid)
{
	struct mds_compound mc;
	nfs_argop4 *slot;
	int ret;

	if (!stateid)
		return -EINVAL;

	/* SEQUENCE + PUTFH + CHUNK_WRITE_REPAIR = 3 ops */
	ret = mds_compound_init(&mc, 3, "chunk_write_repair");
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

	slot = mds_compound_add_op(&mc, OP_CHUNK_WRITE_REPAIR);
	if (!slot) {
		ret = -ENOSPC;
		goto out;
	}

	CHUNK_WRITE_REPAIR4args *cwra =
		&slot->nfs_argop4_u.opchunk_write_repair;

	memcpy(&cwra->cwra_stateid, stateid, sizeof(stateid4));
	cwra->cwra_offset = block_offset;
	cwra->cwra_stable = FILE_SYNC4;
	/*
	 * No generation here: cg_gen_id is the DATA SERVER's
	 * per-chunk counter, not something a writer chooses
	 * (draft sec-chunk_guard4).  The client states what it
	 * EXPECTS via cwa_guard when it wants a compare-and-swap;
	 * the owner carries transaction identity only.
	 *
	 * co_cohort_id names this repair transaction and owner_id is the
	 * first co_id in the payload.  The compact carrier emits one co_id
	 * per reconstructed chunk.
	 */
	cwra->cwra_cohort_id = cohort_id;
	cwra->cwra_client_id = chunk_writer_client_id(layout_client_id);
	cwra->cwra_payload_id = 0;
	cwra->cwra_chunk_size = chunk_size;

	uint32_t nchunks = (data_len + chunk_size - 1) / chunk_size;
	cwra->cwra_co_ids.cwra_co_ids_len = nchunks;
	cwra->cwra_co_ids.cwra_co_ids_val = calloc(nchunks, sizeof(uint32_t));
	if (!cwra->cwra_co_ids.cwra_co_ids_val) {
		ret = -ENOMEM;
		goto out;
	}
	for (uint32_t i = 0; i < nchunks; i++)
		cwra->cwra_co_ids.cwra_co_ids_val[i] = owner_id + i;

	cwra->cwra_checksums.cwra_checksums_len = nchunks;
	cwra->cwra_checksums.cwra_checksums_val =
		calloc(nchunks, sizeof(checksum4));
	if (!cwra->cwra_checksums.cwra_checksums_val) {
		ret = -ENOMEM;
		goto out;
	}

	for (uint32_t i = 0; i < nchunks; i++) {
		uint32_t clen = chunk_size;

		if (i == nchunks - 1 && data_len % chunk_size != 0)
			clen = data_len % chunk_size;
		uint32_t crc = (uint32_t)crc32(
			0L, data + (size_t)i * chunk_size, (uInt)clen);

		if (chunk_checksum_pack_crc32(
			    &cwra->cwra_checksums.cwra_checksums_val[i], crc) <
		    0) {
			ret = -ENOMEM;
			goto out_crc;
		}
	}

	cwra->cwra_chunks.cwra_chunks_len = data_len;
	cwra->cwra_chunks.cwra_chunks_val = (char *)data;

	ret = mds_compound_send(&mc, ds);
	if (ret == -EREMOTEIO) {
		if (mc.mc_res.status == NFS4ERR_BAD_STATEID)
			ret = -ESTALE;
		else if (mc.mc_res.status == NFS4ERR_DELAY)
			ret = -EAGAIN;
	}
	if (ret)
		goto out_crc;

	nfs_resop4 *res_slot = mds_compound_result(&mc, 2);

	if (!res_slot) {
		ret = -EIO;
	} else {
		nfsstat4 st =
			res_slot->nfs_resop4_u.opchunk_write_repair.cwrr_status;

		if (st != NFS4_OK) {
			if (st == NFS4ERR_BAD_STATEID)
				ret = -ESTALE;
			else if (st == NFS4ERR_DELAY)
				ret = -EAGAIN;
			else
				ret = -EIO;
		}
	}

out_crc:
	free(cwra->cwra_co_ids.cwra_co_ids_val);
	cwra->cwra_co_ids.cwra_co_ids_val = NULL;
	cwra->cwra_co_ids.cwra_co_ids_len = 0;
	cwra->cwra_chunks.cwra_chunks_val = NULL;
	cwra->cwra_chunks.cwra_chunks_len = 0;
	if (cwra->cwra_checksums.cwra_checksums_val) {
		for (uint32_t i = 0;
		     i < cwra->cwra_checksums.cwra_checksums_len; i++) {
			free(cwra->cwra_checksums.cwra_checksums_val[i]
				     .cs_value.cs_value_val);
		}
		free(cwra->cwra_checksums.cwra_checksums_val);
	}
	cwra->cwra_checksums.cwra_checksums_val = NULL;
	cwra->cwra_checksums.cwra_checksums_len = 0;
out:
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
		  uint32_t chunk_size, uint32_t *nread, const stateid4 *stateid,
		  chunk_owner4 *out_owners, chunk_guard4 *out_guards)
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
	/* Surface CHUNK_READ BAD_STATEID as -ESTALE
	 * so the retry path can recognise it.  NFS4ERR_DELAY
	 * is the "in-flight write, retry shortly" signal --
	 * map to -EAGAIN so the RMW retry can distinguish it from
	 * fatal -EIO.
	 */
	if (ret == -EREMOTEIO) {
		if (mc.mc_res.status == NFS4ERR_BAD_STATEID)
			ret = -ESTALE;
		else if (mc.mc_res.status == NFS4ERR_DELAY)
			ret = -EAGAIN;
	}
	if (ret)
		goto out;

	nfs_resop4 *res_slot = mds_compound_result(&mc, 2);

	if (!res_slot) {
		ret = -EIO;
		goto out;
	}

	if (res_slot->nfs_resop4_u.opchunk_read.crr_status != NFS4_OK) {
		nfsstat4 st = res_slot->nfs_resop4_u.opchunk_read.crr_status;

		if (st == NFS4ERR_BAD_STATEID)
			ret = -ESTALE;
		else if (st == NFS4ERR_DELAY)
			ret = -EAGAIN;
		else
			ret = -EIO;
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
		 * Capture per-block chunk_owner4 if the caller wants it.
		 * The RMW path uses this to build a cwa_guard for the
		 * matching CHUNK_WRITE -- presents the version it just
		 * read so the server can CAS-check on write.
		 */
		if (out_owners)
			out_owners[i] = rc->cr_owner;
		if (out_guards)
			out_guards[i] = rc->cr_guard;

		/*
		 * Test-only fault-injection hook (experiment 14, bit-flip-
		 * on-wire row).  When EC_DEMO_INJECT_WIRE_FLIP is set in the
		 * environment, XOR the byte at the configured offset within
		 * each returned chunk BEFORE the CRC verify below runs.
		 * The verify should then catch the flip and return -EIO,
		 * which is exactly what the deck claim about client-side
		 * CRC32 catching wire corruption needs to demonstrate.
		 *
		 * Two recognised values:
		 *   EC_DEMO_INJECT_WIRE_FLIP=1            flip byte 0 (default)
		 *   EC_DEMO_INJECT_WIRE_FLIP=<int N>      flip byte N
		 *
		 * Production builds never have this set; deliberately
		 * env-gated so an accidentally-shipped binary can't
		 * silently corrupt reads.  Drops out of every flow when the
		 * env var is absent.
		 */
		{
			const char *flip = getenv("EC_DEMO_INJECT_WIRE_FLIP");

			if (flip && *flip && copy > 0) {
				long off = atol(flip);
				if (off < 0)
					off = 0;
				if ((uint32_t)off < copy) {
					/*
					 * Mutate the wire payload, NOT the
					 * output buffer.  The verify below
					 * recomputes CRC over rc->cr_chunk;
					 * the flip has to land there for the
					 * detection path to fire.  We also
					 * patch out_data so the caller sees
					 * the same corrupted bytes if -EIO
					 * weren't returned (defensive: today
					 * the verify returns -EIO and the
					 * caller never reads out_data for
					 * this block).
					 */
					rc->cr_chunk.cr_chunk_val[off] ^= 0x01;
					((uint8_t *)out_data +
					 (size_t)i * chunk_size)[off] ^= 0x01;
					fprintf(stderr,
						"ds_chunk_read: TEST wire-flip "
						"block %u byte %ld\n",
						i, off);
				}
			}
		}

		/*
		 * Verify the server-supplied checksum against the received
		 * data.  Detects network corruption on the read path.
		 *
		 * Only CHECKSUM_ALG_CRC32 is implemented; other algorithms
		 * (or a malformed entry) are skipped after a warning -- the
		 * data is still delivered, but unverified.  The MDS-side
		 * ffm_checksum_algorithm assignment plus client-side
		 * LAYOUTGET check should keep the wire to algorithms we can
		 * compute, so hitting this skip path is an anomaly worth
		 * surfacing.
		 */
		uint32_t server_crc;

		if (chunk_checksum_unpack_crc32(&rc->cr_checksum,
						&server_crc) != NFS4_OK) {
			fprintf(stderr,
				"ds_chunk_read: block %u checksum algorithm "
				"%u not supported; skipping verification\n",
				i, (unsigned)rc->cr_checksum.cs_algorithm);
		} else if (server_crc != 0) {
			uint32_t wire_crc = (uint32_t)crc32(
				0L, (const Bytef *)rc->cr_chunk.cr_chunk_val,
				(uInt)copy);

			if (wire_crc != server_crc) {
				fprintf(stderr,
					"ds_chunk_read: CRC mismatch "
					"block %u: server 0x%08x "
					"wire 0x%08x\n",
					i, server_crc, wire_crc);
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
		      uint64_t cohort_id, uint32_t layout_client_id,
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

	/*
	 * The slot is zero-initialised here, so the current PS path sends
	 * the anonymous stateid.  The caller's layout stateid must be
	 * threaded through before enabling DS trust enforcement.
	 */
	cfa->cfa_offset = block_offset;
	cfa->cfa_count = count;
	cfa->cfa_chunks.cfa_chunks_len = count;
	cfa->cfa_chunks.cfa_chunks_val = calloc(count, sizeof(chunk_owner4));
	if (!cfa->cfa_chunks.cfa_chunks_val) {
		ret = -ENOMEM;
		goto out;
	}
	/*
	 * Lifecycle operations identify the stored block by the full owner
	 * triple.  Keep the cohort and client identity identical to the
	 * corresponding write; co_id alone is not sufficient.  The client
	 * identity is derived here in the same way as CHUNK_WRITE so an
	 * unassigned layout identity remains interoperable.
	 */
	for (uint32_t i = 0; i < count; i++) {
		cfa->cfa_chunks.cfa_chunks_val[i].co_cohort_id = cohort_id;
		cfa->cfa_chunks.cfa_chunks_val[i].co_client_id =
			chunk_writer_client_id(layout_client_id);
		cfa->cfa_chunks.cfa_chunks_val[i].co_id = owner_id + i;
	}

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
		    uint64_t block_offset, uint32_t count, uint64_t cohort_id,
		    uint32_t layout_client_id, uint32_t owner_id,
		    uint8_t writeverf_out[8])
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

	/*
	 * As with CHUNK_FINALIZE, the current PS path sends the
	 * zero-initialized stateid until DS trust-stateid plumbing is
	 * connected end to end.
	 */
	cca->cca_offset = block_offset;
	cca->cca_count = count;
	cca->cca_chunks.cca_chunks_len = count;
	cca->cca_chunks.cca_chunks_val = calloc(count, sizeof(chunk_owner4));
	if (!cca->cca_chunks.cca_chunks_val) {
		ret = -ENOMEM;
		goto out;
	}
	/*
	 * Lifecycle operations identify the stored block by the full owner
	 * triple.  Keep the cohort and client identity identical to the
	 * corresponding write; co_id alone is not sufficient.  The client
	 * identity is derived here in the same way as CHUNK_WRITE so an
	 * unassigned layout identity remains interoperable.
	 */
	for (uint32_t i = 0; i < count; i++) {
		cca->cca_chunks.cca_chunks_val[i].co_cohort_id = cohort_id;
		cca->cca_chunks.cca_chunks_val[i].co_client_id =
			chunk_writer_client_id(layout_client_id);
		cca->cca_chunks.cca_chunks_val[i].co_id = owner_id + i;
	}

	ret = mds_compound_send(&mc, ds);
	if (ret)
		goto out;

	nfs_resop4 *res_slot = mds_compound_result(&mc, 2);

	if (!res_slot ||
	    res_slot->nfs_resop4_u.opchunk_commit.ccr_status != NFS4_OK) {
		ret = -EIO;
	} else if (writeverf_out) {
		/*
		 * Capture the per-DS writeverf.
		 * Folded into the PS composed write verifier so a DS
		 * reboot between WRITE and COMMIT surfaces to the client
		 * as a verifier mismatch.
		 */
		memcpy(writeverf_out,
		       res_slot->nfs_resop4_u.opchunk_commit.CHUNK_COMMIT4res_u
			       .ccr_resok4.ccr_writeverf,
		       8);
	}

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
