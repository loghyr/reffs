/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Unit tests for the CHUNK operation state machine (chunk.c).
 *
 * Tests cover the PENDING -> FINALIZED -> COMMITTED lifecycle and
 * the associated validation logic in CHUNK_WRITE, CHUNK_FINALIZE,
 * CHUNK_COMMIT, and CHUNK_READ.
 *
 * Trust table validation (stateid auth) is covered by Group I in
 * trust_stateid_test.c.  These tests focus on the chunk store state
 * machine using anonymous (special) stateids to bypass trust checks.
 *
 * Groups:
 *   A. Input validation -- missing FH, zero chunk_size, CRC mismatch.
 *   B. CHUNK_WRITE happy path -- single block, multi-block, inode size.
 *   C. CHUNK_FINALIZE -- missing store, state transition.
 *   D. CHUNK_COMMIT   -- missing store, state transition.
 *   E. CHUNK_READ     -- missing store, zero count, read finalized data.
 *   F. Full state machine roundtrip: WRITE -> FINALIZE -> COMMIT -> READ.
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <netinet/in.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <check.h>
#include <urcu.h>
#include <zlib.h>

#include "nfsv42_xdr.h"
#include "reffs/filehandle.h"
#include "reffs/inode.h"
#include "reffs/server.h"
#include "reffs/super_block.h"
#include "nfs4/chunk_checksum.h"
#include "nfs4/chunk_store.h"
#include "nfs4/client.h"
#include "nfs4/compound.h"
#include "nfs4/ops.h"

#include "nfs4_test_harness.h"

/* ------------------------------------------------------------------ */
/* Compound mock helpers (adapted from trust_stateid_test.c)           */
/* ------------------------------------------------------------------ */

struct cm_ctx {
	struct rpc_trans rt;
	struct task task;
	struct compound *compound;
	struct nfs4_client *nc;
};

static struct cm_ctx *cm_alloc(unsigned int nops)
{
	struct cm_ctx *cm = calloc(1, sizeof(*cm));

	ck_assert_ptr_nonnull(cm);

	atomic_store_explicit(&cm->task.t_state, TASK_RUNNING,
			      memory_order_relaxed);
	cm->rt.rt_task = &cm->task;
	cm->rt.rt_fd = -1;

	struct compound *c = calloc(1, sizeof(*c));

	ck_assert_ptr_nonnull(c);
	c->c_rt = &cm->rt;
	c->c_args = calloc(1, sizeof(COMPOUND4args));
	c->c_res = calloc(1, sizeof(COMPOUND4res));
	ck_assert_ptr_nonnull(c->c_args);
	ck_assert_ptr_nonnull(c->c_res);

	if (nops > 0) {
		c->c_args->argarray.argarray_len = nops;
		c->c_args->argarray.argarray_val =
			calloc(nops, sizeof(nfs_argop4));
		ck_assert_ptr_nonnull(c->c_args->argarray.argarray_val);

		c->c_res->resarray.resarray_len = nops;
		c->c_res->resarray.resarray_val =
			calloc(nops, sizeof(nfs_resop4));
		ck_assert_ptr_nonnull(c->c_res->resarray.resarray_val);
	}

	c->c_server_state = server_state_find();
	ck_assert_ptr_nonnull(c->c_server_state);

	verifier4 v;
	struct sockaddr_in sin;

	memset(&v, 0x11, sizeof(v));
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(0x7f000003);
	sin.sin_port = htons(2049);

	cm->nc = nfs4_client_alloc(&v, &sin, 1, 0xC0DE0001, 0);
	ck_assert_ptr_nonnull(cm->nc);
	c->c_nfs4_client = nfs4_client_get(cm->nc);

	cm->rt.rt_compound = c;
	cm->compound = c;
	return cm;
}

static void cm_set_inode(struct cm_ctx *cm, struct inode *inode)
{
	struct compound *c = cm->compound;

	super_block_put(c->c_curr_sb);
	c->c_curr_sb = super_block_get(inode->i_sb);

	inode_active_put(c->c_inode);
	c->c_inode = inode;
	inode_active_get(inode);

	c->c_curr_nfh.nfh_sb = inode->i_sb->sb_id;
	c->c_curr_nfh.nfh_ino = inode->i_ino;
}

static void cm_set_op(struct cm_ctx *cm, unsigned int idx, nfs_opnum4 opnum)
{
	ck_assert_uint_lt(idx, cm->compound->c_args->argarray.argarray_len);
	cm->compound->c_args->argarray.argarray_val[idx].argop = opnum;
	cm->compound->c_res->resarray.resarray_val[idx].resop = opnum;
	cm->compound->c_curr_op = (int)idx;
}

static void cm_free(struct cm_ctx *cm)
{
	if (!cm)
		return;
	struct compound *c = cm->compound;

	if (c) {
		server_state_put(c->c_server_state);
		inode_active_put(c->c_inode);
		super_block_put(c->c_curr_sb);
		super_block_put(c->c_saved_sb);
		stateid_put(c->c_curr_stid);
		stateid_put(c->c_saved_stid);
		nfs4_client_put(c->c_nfs4_client);
		free(c->c_args->argarray.argarray_val);
		free(c->c_args);
		free(c->c_res->resarray.resarray_val);
		free(c->c_res);
		free(c);
	}
	nfs4_client_put(cm->nc);
	free(cm);
}

/* ------------------------------------------------------------------ */
/* Fixtures                                                            */
/* ------------------------------------------------------------------ */

static struct super_block *g_sb;
static struct inode *g_inode;

static void chunk_setup(void)
{
	nfs4_test_setup();
	g_sb = super_block_find(SUPER_BLOCK_ROOT_ID);
	ck_assert_ptr_nonnull(g_sb);

	uint64_t ino =
		__atomic_add_fetch(&g_sb->sb_next_ino, 1, __ATOMIC_RELAXED);
	g_inode = inode_alloc(g_sb, ino);
	ck_assert_ptr_nonnull(g_inode);
	g_inode->i_mode = S_IFREG | 0640;
}

static void chunk_teardown(void)
{
	if (g_inode) {
		inode_active_put(g_inode);
		g_inode = NULL;
	}
	if (g_sb) {
		super_block_put(g_sb);
		g_sb = NULL;
	}
	nfs4_test_teardown();
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

#define CHUNK_SZ 512

/*
 * Build CHUNK_WRITE args in slot 0.  Uses an anonymous stateid so trust
 * table validation is bypassed.  Caller provides the data buffer and a
 * uint32_t CRC array (pass NULL/0 for no checksum validation).  The
 * function wraps each CRC into a checksum4 (CHECKSUM_ALG_CRC32, 4 bytes
 * big-endian) -- the cs_value buffers and the checksum4 array are heap-
 * allocated here and released by free_write_args.
 */
static void set_write_args(struct cm_ctx *cm, char *buf, uint32_t buf_len,
			   uint32_t chunk_size, uint64_t offset, uint32_t *crcs,
			   uint32_t ncrc)
{
	cm_set_op(cm, 0, OP_CHUNK_WRITE);
	CHUNK_WRITE4args *args = &cm->compound->c_args->argarray.argarray_val[0]
					  .nfs_argop4_u.opchunk_write;

	/* Anonymous stateid: all zeros (seqid=0, other={0}) */
	memset(&args->cwa_stateid, 0, sizeof(args->cwa_stateid));
	args->cwa_offset = offset;
	args->cwa_stable = UNSTABLE4;
	args->cwa_chunk_size = chunk_size;
	args->cwa_chunks.cwa_chunks_val = buf;
	args->cwa_chunks.cwa_chunks_len = buf_len;

	if (ncrc > 0 && crcs != NULL) {
		args->cwa_checksums.cwa_checksums_val =
			calloc(ncrc, sizeof(checksum4));
		args->cwa_checksums.cwa_checksums_len = ncrc;
		for (uint32_t i = 0; i < ncrc; i++) {
			(void)chunk_checksum_pack_crc32(
				&args->cwa_checksums.cwa_checksums_val[i],
				crcs[i]);
		}
	} else {
		args->cwa_checksums.cwa_checksums_val = NULL;
		args->cwa_checksums.cwa_checksums_len = 0;
	}

	args->cwa_payload_id = 0x4242;
	args->cwa_owner.co_guard.cg_gen_id = 7;
	args->cwa_owner.co_guard.cg_client_id = 0xBEEF;
	args->cwa_owner.co_id = 99;
}

/*
 * Release the heap allocations made by set_write_args for the
 * cwa_checksums array.  Safe to call when the array is empty.
 */
static void free_write_args(struct cm_ctx *cm)
{
	CHUNK_WRITE4args *args = &cm->compound->c_args->argarray.argarray_val[0]
					  .nfs_argop4_u.opchunk_write;

	if (args->cwa_checksums.cwa_checksums_val) {
		for (uint32_t i = 0; i < args->cwa_checksums.cwa_checksums_len;
		     i++) {
			free(args->cwa_checksums.cwa_checksums_val[i]
				     .cs_value.cs_value_val);
		}
		free(args->cwa_checksums.cwa_checksums_val);
		args->cwa_checksums.cwa_checksums_val = NULL;
		args->cwa_checksums.cwa_checksums_len = 0;
	}
}

/* Free the per-chunk status array from a successful CHUNK_WRITE result. */
static void free_write_res(struct cm_ctx *cm)
{
	CHUNK_WRITE4res *res = &cm->compound->c_res->resarray.resarray_val[0]
					.nfs_resop4_u.opchunk_write;

	free(res->CHUNK_WRITE4res_u.cwr_resok4.cwr_block_status
		     .cwr_block_status_val);
	res->CHUNK_WRITE4res_u.cwr_resok4.cwr_block_status.cwr_block_status_val =
		NULL;
}

/* Free the per-owner status array from a CHUNK_FINALIZE result. */
static void free_finalize_res(struct cm_ctx *cm)
{
	CHUNK_FINALIZE4res *res = &cm->compound->c_res->resarray.resarray_val[0]
					   .nfs_resop4_u.opchunk_finalize;

	free(res->CHUNK_FINALIZE4res_u.cfr_resok4.cfr_status.cfr_status_val);
	res->CHUNK_FINALIZE4res_u.cfr_resok4.cfr_status.cfr_status_val = NULL;
}

/* Free the per-owner status array from a CHUNK_COMMIT result. */
static void free_commit_res(struct cm_ctx *cm)
{
	CHUNK_COMMIT4res *res = &cm->compound->c_res->resarray.resarray_val[0]
					 .nfs_resop4_u.opchunk_commit;

	free(res->CHUNK_COMMIT4res_u.ccr_resok4.ccr_status.ccr_status_val);
	res->CHUNK_COMMIT4res_u.ccr_resok4.ccr_status.ccr_status_val = NULL;
}

/* Free the chunk data buffers from a CHUNK_READ result. */
static void free_read_res(struct cm_ctx *cm)
{
	CHUNK_READ4res *res = &cm->compound->c_res->resarray.resarray_val[0]
				       .nfs_resop4_u.opchunk_read;
	CHUNK_READ4resok *resok = &res->CHUNK_READ4res_u.crr_resok4;

	for (u_int i = 0; i < resok->crr_chunks.crr_chunks_len; i++) {
		free(resok->crr_chunks.crr_chunks_val[i].cr_chunk.cr_chunk_val);
		resok->crr_chunks.crr_chunks_val[i].cr_chunk.cr_chunk_val =
			NULL;
	}
	free(resok->crr_chunks.crr_chunks_val);
	resok->crr_chunks.crr_chunks_val = NULL;
}

/*
 * Clear arg/res slot idx so it can be reused for a fresh op within
 * the same compound.  Call after freeing dynamic result allocations
 * (e.g., free_write_res) so no heap pointers are overwritten.
 *
 * Multi-step tests reuse a single cm_ctx to avoid a second
 * nfs4_client_alloc with the same clientid colliding in the hash
 * table (the first cm_free drops the caller refs but the table ref
 * keeps the client hashed until nfs4_test_teardown).
 */
static void cm_reset_slot(struct cm_ctx *cm, unsigned int idx)
{
	memset(&cm->compound->c_args->argarray.argarray_val[idx], 0,
	       sizeof(nfs_argop4));
	memset(&cm->compound->c_res->resarray.resarray_val[idx], 0,
	       sizeof(nfs_resop4));
}

/* ------------------------------------------------------------------ */
/* Group A: Input validation                                           */
/* ------------------------------------------------------------------ */

/*
 * No current filehandle must return NFS4ERR_NOFILEHANDLE.  Applies
 * to all four chunk ops; we test WRITE as the representative.
 */
START_TEST(test_chunk_write_no_fh)
{
	static char buf[CHUNK_SZ];
	struct cm_ctx *cm = cm_alloc(1);

	/* Deliberately no cm_set_inode -- FH remains empty. */
	set_write_args(cm, buf, CHUNK_SZ, CHUNK_SZ, 0, NULL, 0);

	nfs4_op_chunk_write(cm->compound);

	CHUNK_WRITE4res *res = &cm->compound->c_res->resarray.resarray_val[0]
					.nfs_resop4_u.opchunk_write;
	ck_assert_int_eq(res->cwr_status, NFS4ERR_NOFILEHANDLE);

	cm_free(cm);
}
END_TEST

/*
 * chunk_size == 0 must return NFS4ERR_INVAL.
 */
START_TEST(test_chunk_write_zero_chunk_size)
{
	static char buf[CHUNK_SZ];
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);
	set_write_args(cm, buf, CHUNK_SZ, 0 /* chunk_size=0 */, 0, NULL, 0);

	nfs4_op_chunk_write(cm->compound);

	CHUNK_WRITE4res *res = &cm->compound->c_res->resarray.resarray_val[0]
					.nfs_resop4_u.opchunk_write;
	ck_assert_int_eq(res->cwr_status, NFS4ERR_INVAL);

	cm_free(cm);
}
END_TEST

/*
 * CRC32 mismatch must return NFS4ERR_INVAL.  The payload is zeros so
 * we supply a deliberately wrong CRC value.
 */
START_TEST(test_chunk_write_crc_mismatch)
{
	static char buf[CHUNK_SZ]; /* zero-filled */
	uint32_t bad_crc = 0xDEADBEEF; /* not the real crc32 of zeros */
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);
	set_write_args(cm, buf, CHUNK_SZ, CHUNK_SZ, 0, &bad_crc, 1);

	nfs4_op_chunk_write(cm->compound);

	CHUNK_WRITE4res *res = &cm->compound->c_res->resarray.resarray_val[0]
					.nfs_resop4_u.opchunk_write;
	ck_assert_int_eq(res->cwr_status, NFS4ERR_INVAL);

	free_write_args(cm);
	cm_free(cm);
}
END_TEST

/*
 * A non-regular-file inode (directory) must return NFS4ERR_INVAL.
 */
START_TEST(test_chunk_write_not_regular_file)
{
	static char buf[CHUNK_SZ];
	struct cm_ctx *cm = cm_alloc(1);

	/* Temporarily make the test inode a directory. */
	mode_t saved = g_inode->i_mode;
	g_inode->i_mode = S_IFDIR | 0755;

	cm_set_inode(cm, g_inode);
	set_write_args(cm, buf, CHUNK_SZ, CHUNK_SZ, 0, NULL, 0);

	nfs4_op_chunk_write(cm->compound);

	CHUNK_WRITE4res *res = &cm->compound->c_res->resarray.resarray_val[0]
					.nfs_resop4_u.opchunk_write;
	ck_assert_int_eq(res->cwr_status, NFS4ERR_INVAL);

	g_inode->i_mode = saved;
	cm_free(cm);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Group B: CHUNK_WRITE happy path                                     */
/* ------------------------------------------------------------------ */

/*
 * Write a single chunk.  Expect NFS4_OK, cwr_count=1, and the
 * inode's chunk store populated with one PENDING block.
 */
START_TEST(test_chunk_write_single_block)
{
	static char buf[CHUNK_SZ];
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);
	set_write_args(cm, buf, CHUNK_SZ, CHUNK_SZ, 0, NULL, 0);

	nfs4_op_chunk_write(cm->compound);

	CHUNK_WRITE4res *res = &cm->compound->c_res->resarray.resarray_val[0]
					.nfs_resop4_u.opchunk_write;
	ck_assert_int_eq(res->cwr_status, NFS4_OK);
	ck_assert_uint_eq(res->CHUNK_WRITE4res_u.cwr_resok4.cwr_count, 1);

	/* Chunk store must exist with one PENDING block at offset 0. */
	struct chunk_store *cs = g_inode->i_chunk_store;

	ck_assert_ptr_nonnull(cs);
	struct chunk_block *blk = chunk_store_lookup(cs, 0);

	ck_assert_ptr_nonnull(blk);
	ck_assert_int_eq(blk->cb_state, CHUNK_STATE_PENDING);
	ck_assert_uint_eq(blk->cb_chunk_size, CHUNK_SZ);
	ck_assert_uint_eq(blk->cb_owner_id, 99);

	free_write_res(cm);
	cm_free(cm);
}
END_TEST

/*
 * Write three chunks in a single call.  The opaque payload is 3 *
 * chunk_size bytes so nchunks == 3.  Expect three PENDING blocks.
 */
START_TEST(test_chunk_write_multi_block)
{
	static char buf[3 * CHUNK_SZ];
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);
	set_write_args(cm, buf, 3 * CHUNK_SZ, CHUNK_SZ, 0, NULL, 0);

	nfs4_op_chunk_write(cm->compound);

	CHUNK_WRITE4res *res = &cm->compound->c_res->resarray.resarray_val[0]
					.nfs_resop4_u.opchunk_write;
	ck_assert_int_eq(res->cwr_status, NFS4_OK);
	ck_assert_uint_eq(res->CHUNK_WRITE4res_u.cwr_resok4.cwr_count, 3);

	struct chunk_store *cs = g_inode->i_chunk_store;

	ck_assert_ptr_nonnull(cs);
	for (uint64_t i = 0; i < 3; i++) {
		struct chunk_block *blk = chunk_store_lookup(cs, i);

		ck_assert_ptr_nonnull(blk);
		ck_assert_int_eq(blk->cb_state, CHUNK_STATE_PENDING);
	}

	free_write_res(cm);
	cm_free(cm);
}
END_TEST

/*
 * CHUNK_WRITE must update i_size.  After writing one chunk at offset 0,
 * i_size must equal chunk_size.
 */
START_TEST(test_chunk_write_updates_inode_size)
{
	static char buf[CHUNK_SZ];
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);
	set_write_args(cm, buf, CHUNK_SZ, CHUNK_SZ, 0, NULL, 0);
	g_inode->i_size = 0;

	nfs4_op_chunk_write(cm->compound);

	CHUNK_WRITE4res *res = &cm->compound->c_res->resarray.resarray_val[0]
					.nfs_resop4_u.opchunk_write;
	ck_assert_int_eq(res->cwr_status, NFS4_OK);
	ck_assert_int_eq(g_inode->i_size, CHUNK_SZ);

	free_write_res(cm);
	cm_free(cm);
}
END_TEST

/*
 * CHUNK_WRITE with a valid CRC32 must succeed.
 */
START_TEST(test_chunk_write_valid_crc)
{
	static char buf[CHUNK_SZ]; /* zero-filled */
	uint32_t good_crc = (uint32_t)crc32(0L, (const Bytef *)buf, CHUNK_SZ);
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);
	set_write_args(cm, buf, CHUNK_SZ, CHUNK_SZ, 0, &good_crc, 1);

	nfs4_op_chunk_write(cm->compound);

	CHUNK_WRITE4res *res = &cm->compound->c_res->resarray.resarray_val[0]
					.nfs_resop4_u.opchunk_write;
	ck_assert_int_eq(res->cwr_status, NFS4_OK);

	/*
	 * Stored checksum must match what we provided: algorithm
	 * CRC32, 4 bytes, big-endian encoding of good_crc.
	 */
	struct chunk_block *blk = chunk_store_lookup(g_inode->i_chunk_store, 0);

	ck_assert_ptr_nonnull(blk);
	ck_assert_uint_eq(blk->cb_checksum_algorithm, CHECKSUM_ALG_CRC32);
	ck_assert_uint_eq(blk->cb_checksum_len, 4);
	uint32_t stored = ((uint32_t)blk->cb_checksum_value[0] << 24) |
			  ((uint32_t)blk->cb_checksum_value[1] << 16) |
			  ((uint32_t)blk->cb_checksum_value[2] << 8) |
			  (uint32_t)blk->cb_checksum_value[3];
	ck_assert_uint_eq(stored, good_crc);

	free_write_args(cm);
	free_write_res(cm);
	cm_free(cm);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Pending Change 6 step 8: server-side algorithm enforcement          */
/* ------------------------------------------------------------------ */

/*
 * Build a CHUNK_WRITE args with a single hand-built checksum4 entry.
 * Caller is responsible for free_write_args() since we allocate the
 * checksums array via calloc directly here (set_write_args' shape
 * is uint32_t CRC -> checksum4, which doesn't let us exercise the
 * unknown-algorithm / mismatched-length paths).
 */
static void set_write_args_raw_checksum(struct cm_ctx *cm, char *buf,
					uint32_t buf_len, uint32_t chunk_size,
					uint64_t offset, uint32_t algo,
					const uint8_t *value,
					uint32_t value_len)
{
	cm_set_op(cm, 0, OP_CHUNK_WRITE);
	CHUNK_WRITE4args *args = &cm->compound->c_args->argarray.argarray_val[0]
					  .nfs_argop4_u.opchunk_write;

	memset(&args->cwa_stateid, 0, sizeof(args->cwa_stateid));
	args->cwa_offset = offset;
	args->cwa_stable = UNSTABLE4;
	args->cwa_chunk_size = chunk_size;
	args->cwa_chunks.cwa_chunks_val = buf;
	args->cwa_chunks.cwa_chunks_len = buf_len;

	args->cwa_checksums.cwa_checksums_val = calloc(1, sizeof(checksum4));
	args->cwa_checksums.cwa_checksums_len = 1;
	args->cwa_checksums.cwa_checksums_val[0].cs_algorithm = algo;
	args->cwa_checksums.cwa_checksums_val[0].cs_value.cs_value_len =
		value_len;
	if (value_len > 0) {
		uint8_t *v = calloc(1, value_len);

		if (value)
			memcpy(v, value, value_len);
		args->cwa_checksums.cwa_checksums_val[0].cs_value.cs_value_val =
			(char *)v;
	}

	args->cwa_payload_id = 0x4242;
	args->cwa_owner.co_guard.cg_gen_id = 7;
	args->cwa_owner.co_guard.cg_client_id = 0xBEEF;
	args->cwa_owner.co_id = 99;
}

/*
 * An unknown wire algorithm (numerically outside the registered
 * CHECKSUM_ALG_* range) must be rejected before any per-file state
 * is established.  After the rejection the chunk_store must NOT have
 * been created (the test relies on i_chunk_store staying NULL).
 */
START_TEST(test_chunk_write_unknown_algorithm_rejected)
{
	static char buf[CHUNK_SZ];
	uint8_t junk[4] = { 0, 0, 0, 0 };
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);
	set_write_args_raw_checksum(cm, buf, CHUNK_SZ, CHUNK_SZ, 0,
				    /* algo */ 0xDEADBEEF, junk, 4);

	nfs4_op_chunk_write(cm->compound);

	CHUNK_WRITE4res *res = &cm->compound->c_res->resarray.resarray_val[0]
					.nfs_resop4_u.opchunk_write;
	ck_assert_int_eq(res->cwr_status, NFS4ERR_INVAL);
	ck_assert_ptr_null(g_inode->i_chunk_store);

	free_write_args(cm);
	cm_free(cm);
}
END_TEST

/*
 * A wire algorithm we recognise but with the wrong cs_value length
 * is rejected.  CRC32 with 8 bytes is the canonical case: the
 * algorithm is fine, the length is not.
 */
START_TEST(test_chunk_write_wrong_length_rejected)
{
	static char buf[CHUNK_SZ];
	uint8_t junk[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);
	set_write_args_raw_checksum(cm, buf, CHUNK_SZ, CHUNK_SZ, 0,
				    CHECKSUM_ALG_CRC32, junk, 8);

	nfs4_op_chunk_write(cm->compound);

	CHUNK_WRITE4res *res = &cm->compound->c_res->resarray.resarray_val[0]
					.nfs_resop4_u.opchunk_write;
	ck_assert_int_eq(res->cwr_status, NFS4ERR_INVAL);

	free_write_args(cm);
	cm_free(cm);
}
END_TEST

/*
 * First CHUNK_WRITE establishes the per-file algorithm; the
 * chunk_store carries the value forward across subsequent writes,
 * and a second write declaring a different algorithm is rejected
 * with NFS4ERR_INVAL even when that algorithm is otherwise valid
 * (correct length, in the registered set).
 */
START_TEST(test_chunk_write_per_file_algorithm_consistency)
{
	/* Step 1: write with CRC32, expect OK + cs_checksum_algorithm set. */
	static char buf[CHUNK_SZ];
	uint32_t good_crc = (uint32_t)crc32(0L, (const Bytef *)buf, CHUNK_SZ);
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);
	set_write_args(cm, buf, CHUNK_SZ, CHUNK_SZ, 0, &good_crc, 1);
	nfs4_op_chunk_write(cm->compound);

	CHUNK_WRITE4res *res = &cm->compound->c_res->resarray.resarray_val[0]
					.nfs_resop4_u.opchunk_write;
	ck_assert_int_eq(res->cwr_status, NFS4_OK);
	ck_assert_ptr_nonnull(g_inode->i_chunk_store);
	ck_assert_uint_eq(g_inode->i_chunk_store->cs_checksum_algorithm,
			  CHECKSUM_ALG_CRC32);

	free_write_args(cm);
	free_write_res(cm);

	/*
	 * Step 2: same file, raw CHECKSUM_ALG_CRC32C entry with the
	 * right length (4 bytes) -- pre-validation accepts the wire,
	 * but the per-file consistency check rejects with INVAL
	 * because the file is already locked to CRC32.
	 */
	cm_reset_slot(cm, 0);
	uint8_t four_bytes[4] = { 0xDE, 0xAD, 0xBE, 0xEF };

	set_write_args_raw_checksum(cm, buf, CHUNK_SZ, CHUNK_SZ, 1,
				    CHECKSUM_ALG_CRC32C, four_bytes, 4);
	nfs4_op_chunk_write(cm->compound);

	res = &cm->compound->c_res->resarray.resarray_val[0]
		       .nfs_resop4_u.opchunk_write;
	ck_assert_int_eq(res->cwr_status, NFS4ERR_INVAL);

	/* File's algorithm must remain CRC32 -- the second write did
	 * not overwrite the policy. */
	ck_assert_uint_eq(g_inode->i_chunk_store->cs_checksum_algorithm,
			  CHECKSUM_ALG_CRC32);

	free_write_args(cm);
	cm_free(cm);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Group C: CHUNK_FINALIZE                                             */
/* ------------------------------------------------------------------ */

/*
 * CHUNK_FINALIZE without a prior CHUNK_WRITE (no chunk store) must
 * return NFS4ERR_NOENT.
 */
START_TEST(test_chunk_finalize_no_store)
{
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);
	cm_set_op(cm, 0, OP_CHUNK_FINALIZE);

	/*
	 * cg_client_id must be a non-reserved value
	 * (CHUNK_GUARD_CLIENT_ID_NONE = 0 and CHUNK_GUARD_CLIENT_ID_MDS
	 * = 0xFFFFFFFF are rejected with NFS4ERR_INVAL per
	 * draft-haynes-nfsv4-flexfiles-v2 sec-chunk_guard_none).  0xBEEF
	 * matches the value set_write_args() uses on the writer side so
	 * owner-id lookups across WRITE / FINALIZE / COMMIT line up.
	 */
	chunk_owner4 owner = { .co_guard.cg_client_id = 0xBEEF, .co_id = 99 };
	CHUNK_FINALIZE4args *args =
		&cm->compound->c_args->argarray.argarray_val[0]
			 .nfs_argop4_u.opchunk_finalize;
	args->cfa_offset = 0;
	args->cfa_count = 1;
	args->cfa_chunks.cfa_chunks_val = &owner;
	args->cfa_chunks.cfa_chunks_len = 1;

	nfs4_op_chunk_finalize(cm->compound);

	CHUNK_FINALIZE4res *res = &cm->compound->c_res->resarray.resarray_val[0]
					   .nfs_resop4_u.opchunk_finalize;
	ck_assert_int_eq(res->cfr_status, NFS4ERR_NOENT);

	cm_free(cm);
}
END_TEST

/*
 * CHUNK_FINALIZE must transition PENDING blocks to FINALIZED.
 * Uses a single cm_ctx to avoid clientid hash collision between steps.
 */
START_TEST(test_chunk_finalize_transitions_state)
{
	static char buf[CHUNK_SZ];
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);

	/* Step 1: write one block -- ends up PENDING. */
	set_write_args(cm, buf, CHUNK_SZ, CHUNK_SZ, 0, NULL, 0);
	nfs4_op_chunk_write(cm->compound);

	CHUNK_WRITE4res *wres = &cm->compound->c_res->resarray.resarray_val[0]
					 .nfs_resop4_u.opchunk_write;
	ck_assert_int_eq(wres->cwr_status, NFS4_OK);
	free_write_res(cm);

	/* Verify PENDING state before finalize. */
	struct chunk_block *blk = chunk_store_lookup(g_inode->i_chunk_store, 0);
	ck_assert_ptr_nonnull(blk);
	ck_assert_int_eq(blk->cb_state, CHUNK_STATE_PENDING);

	/* Step 2: finalize using the same compound context. */
	cm_reset_slot(cm, 0);
	cm_set_op(cm, 0, OP_CHUNK_FINALIZE);

	/*
	 * cg_client_id must be a non-reserved value
	 * (CHUNK_GUARD_CLIENT_ID_NONE = 0 and CHUNK_GUARD_CLIENT_ID_MDS
	 * = 0xFFFFFFFF are rejected with NFS4ERR_INVAL per
	 * draft-haynes-nfsv4-flexfiles-v2 sec-chunk_guard_none).  0xBEEF
	 * matches the value set_write_args() uses on the writer side so
	 * owner-id lookups across WRITE / FINALIZE / COMMIT line up.
	 */
	chunk_owner4 owner = { .co_guard.cg_client_id = 0xBEEF, .co_id = 99 };
	CHUNK_FINALIZE4args *args =
		&cm->compound->c_args->argarray.argarray_val[0]
			 .nfs_argop4_u.opchunk_finalize;
	args->cfa_offset = 0;
	args->cfa_count = 1;
	args->cfa_chunks.cfa_chunks_val = &owner;
	args->cfa_chunks.cfa_chunks_len = 1;

	nfs4_op_chunk_finalize(cm->compound);

	CHUNK_FINALIZE4res *fres =
		&cm->compound->c_res->resarray.resarray_val[0]
			 .nfs_resop4_u.opchunk_finalize;
	ck_assert_int_eq(fres->cfr_status, NFS4_OK);
	ck_assert_int_eq(fres->CHUNK_FINALIZE4res_u.cfr_resok4.cfr_status
				 .cfr_status_val[0],
			 NFS4_OK);

	/* Block must now be FINALIZED. */
	ck_assert_int_eq(blk->cb_state, CHUNK_STATE_FINALIZED);

	free_finalize_res(cm);
	cm_free(cm);
}
END_TEST

/*
 * Sparse FINALIZE: codecs with variable-size shards (Mojette
 * systematic; any future projection codec) write blocks at a
 * stride wider than they actually fill -- a data shard may write
 * one chunk per stripe while the largest parity shard writes
 * four, leaving holes at offsets in between.  FINALIZE / COMMIT
 * span the full nominal range and must tolerate the EMPTY holes
 * rather than aborting at the first one (the latent bug
 * surfaced by experiment 14, see commit history).
 *
 * This test writes blocks at offsets 0 and 4 (leaving offsets 1,
 * 2, 3 as EMPTY holes), then FINALIZEs the contiguous range
 * [0, 5).  The expected behaviour after the chunk_store_transition
 * fix: NFS4_OK, blocks 0 and 4 transition to FINALIZED, blocks
 * 1/2/3 remain EMPTY.
 */
START_TEST(test_chunk_finalize_skips_empty_in_range)
{
	static char buf[CHUNK_SZ];
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);

	/* Write block 0. */
	set_write_args(cm, buf, CHUNK_SZ, CHUNK_SZ, 0, NULL, 0);
	nfs4_op_chunk_write(cm->compound);
	ck_assert_int_eq(cm->compound->c_res->resarray.resarray_val[0]
				 .nfs_resop4_u.opchunk_write.cwr_status,
			 NFS4_OK);
	free_write_res(cm);

	/* Write block 4 (leaves 1, 2, 3 as EMPTY holes). */
	cm_reset_slot(cm, 0);
	set_write_args(cm, buf, CHUNK_SZ, CHUNK_SZ, 4, NULL, 0);
	nfs4_op_chunk_write(cm->compound);
	ck_assert_int_eq(cm->compound->c_res->resarray.resarray_val[0]
				 .nfs_resop4_u.opchunk_write.cwr_status,
			 NFS4_OK);
	free_write_res(cm);

	struct chunk_store *cs = g_inode->i_chunk_store;

	ck_assert_ptr_nonnull(cs);
	ck_assert_int_eq(chunk_store_lookup(cs, 0)->cb_state,
			 CHUNK_STATE_PENDING);
	/* EMPTY blocks are masked by chunk_store_lookup (returns NULL);
	 * read the underlying array directly to verify the holes. */
	ck_assert_int_eq(cs->cs_blocks[1].cb_state, CHUNK_STATE_EMPTY);
	ck_assert_int_eq(cs->cs_blocks[2].cb_state, CHUNK_STATE_EMPTY);
	ck_assert_int_eq(cs->cs_blocks[3].cb_state, CHUNK_STATE_EMPTY);
	ck_assert_int_eq(chunk_store_lookup(cs, 4)->cb_state,
			 CHUNK_STATE_PENDING);

	/* FINALIZE the full nominal stride [0, 5) -- must skip holes. */
	cm_reset_slot(cm, 0);
	cm_set_op(cm, 0, OP_CHUNK_FINALIZE);

	/*
	 * cg_client_id must be a non-reserved value
	 * (CHUNK_GUARD_CLIENT_ID_NONE = 0 and CHUNK_GUARD_CLIENT_ID_MDS
	 * = 0xFFFFFFFF are rejected with NFS4ERR_INVAL per
	 * draft-haynes-nfsv4-flexfiles-v2 sec-chunk_guard_none).  0xBEEF
	 * matches the value set_write_args() uses on the writer side so
	 * owner-id lookups across WRITE / FINALIZE / COMMIT line up.
	 */
	chunk_owner4 owner = { .co_guard.cg_client_id = 0xBEEF, .co_id = 99 };
	CHUNK_FINALIZE4args *fargs =
		&cm->compound->c_args->argarray.argarray_val[0]
			 .nfs_argop4_u.opchunk_finalize;
	fargs->cfa_offset = 0;
	fargs->cfa_count = 5;
	fargs->cfa_chunks.cfa_chunks_val = &owner;
	fargs->cfa_chunks.cfa_chunks_len = 1;

	nfs4_op_chunk_finalize(cm->compound);

	CHUNK_FINALIZE4res *fres =
		&cm->compound->c_res->resarray.resarray_val[0]
			 .nfs_resop4_u.opchunk_finalize;
	ck_assert_int_eq(fres->cfr_status, NFS4_OK);
	ck_assert_int_eq(fres->CHUNK_FINALIZE4res_u.cfr_resok4.cfr_status
				 .cfr_status_val[0],
			 NFS4_OK);

	/* Written blocks transitioned; holes still EMPTY (masked by
	 * lookup -- read cs_blocks directly). */
	ck_assert_int_eq(chunk_store_lookup(cs, 0)->cb_state,
			 CHUNK_STATE_FINALIZED);
	ck_assert_int_eq(cs->cs_blocks[1].cb_state, CHUNK_STATE_EMPTY);
	ck_assert_int_eq(cs->cs_blocks[2].cb_state, CHUNK_STATE_EMPTY);
	ck_assert_int_eq(cs->cs_blocks[3].cb_state, CHUNK_STATE_EMPTY);
	ck_assert_int_eq(chunk_store_lookup(cs, 4)->cb_state,
			 CHUNK_STATE_FINALIZED);

	free_finalize_res(cm);
	cm_free(cm);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Group D: CHUNK_COMMIT                                               */
/* ------------------------------------------------------------------ */

/*
 * CHUNK_COMMIT without a prior CHUNK_WRITE (no chunk store) must
 * return NFS4ERR_NOENT.
 */
START_TEST(test_chunk_commit_no_store)
{
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);
	cm_set_op(cm, 0, OP_CHUNK_COMMIT);

	/*
	 * cg_client_id must be a non-reserved value
	 * (CHUNK_GUARD_CLIENT_ID_NONE = 0 and CHUNK_GUARD_CLIENT_ID_MDS
	 * = 0xFFFFFFFF are rejected with NFS4ERR_INVAL per
	 * draft-haynes-nfsv4-flexfiles-v2 sec-chunk_guard_none).  0xBEEF
	 * matches the value set_write_args() uses on the writer side so
	 * owner-id lookups across WRITE / FINALIZE / COMMIT line up.
	 */
	chunk_owner4 owner = { .co_guard.cg_client_id = 0xBEEF, .co_id = 99 };
	CHUNK_COMMIT4args *args =
		&cm->compound->c_args->argarray.argarray_val[0]
			 .nfs_argop4_u.opchunk_commit;
	args->cca_offset = 0;
	args->cca_count = 1;
	args->cca_chunks.cca_chunks_val = &owner;
	args->cca_chunks.cca_chunks_len = 1;

	nfs4_op_chunk_commit(cm->compound);

	CHUNK_COMMIT4res *res = &cm->compound->c_res->resarray.resarray_val[0]
					 .nfs_resop4_u.opchunk_commit;
	ck_assert_int_eq(res->ccr_status, NFS4ERR_NOENT);

	cm_free(cm);
}
END_TEST

/*
 * CHUNK_COMMIT must transition FINALIZED blocks to COMMITTED.
 * Drives the block through WRITE -> FINALIZE -> COMMIT using a single
 * cm_ctx to avoid clientid hash collision between steps.
 */
START_TEST(test_chunk_commit_transitions_state)
{
	static char buf[CHUNK_SZ];
	/*
	 * cg_client_id must be a non-reserved value
	 * (CHUNK_GUARD_CLIENT_ID_NONE = 0 and CHUNK_GUARD_CLIENT_ID_MDS
	 * = 0xFFFFFFFF are rejected with NFS4ERR_INVAL per
	 * draft-haynes-nfsv4-flexfiles-v2 sec-chunk_guard_none).  0xBEEF
	 * matches the value set_write_args() uses on the writer side so
	 * owner-id lookups across WRITE / FINALIZE / COMMIT line up.
	 */
	chunk_owner4 owner = { .co_guard.cg_client_id = 0xBEEF, .co_id = 99 };
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);

	/* Step 1: WRITE. */
	set_write_args(cm, buf, CHUNK_SZ, CHUNK_SZ, 0, NULL, 0);
	nfs4_op_chunk_write(cm->compound);
	ck_assert_int_eq(cm->compound->c_res->resarray.resarray_val[0]
				 .nfs_resop4_u.opchunk_write.cwr_status,
			 NFS4_OK);
	free_write_res(cm);

	/* Step 2: FINALIZE. */
	cm_reset_slot(cm, 0);
	cm_set_op(cm, 0, OP_CHUNK_FINALIZE);

	CHUNK_FINALIZE4args *fargs =
		&cm->compound->c_args->argarray.argarray_val[0]
			 .nfs_argop4_u.opchunk_finalize;
	fargs->cfa_offset = 0;
	fargs->cfa_count = 1;
	fargs->cfa_chunks.cfa_chunks_val = &owner;
	fargs->cfa_chunks.cfa_chunks_len = 1;

	nfs4_op_chunk_finalize(cm->compound);
	ck_assert_int_eq(cm->compound->c_res->resarray.resarray_val[0]
				 .nfs_resop4_u.opchunk_finalize.cfr_status,
			 NFS4_OK);
	free_finalize_res(cm);

	/* Verify FINALIZED state. */
	struct chunk_block *blk = chunk_store_lookup(g_inode->i_chunk_store, 0);

	ck_assert_ptr_nonnull(blk);
	ck_assert_int_eq(blk->cb_state, CHUNK_STATE_FINALIZED);

	/* Step 3: COMMIT. */
	cm_reset_slot(cm, 0);
	cm_set_op(cm, 0, OP_CHUNK_COMMIT);

	CHUNK_COMMIT4args *cargs =
		&cm->compound->c_args->argarray.argarray_val[0]
			 .nfs_argop4_u.opchunk_commit;
	cargs->cca_offset = 0;
	cargs->cca_count = 1;
	cargs->cca_chunks.cca_chunks_val = &owner;
	cargs->cca_chunks.cca_chunks_len = 1;

	nfs4_op_chunk_commit(cm->compound);

	CHUNK_COMMIT4res *cres = &cm->compound->c_res->resarray.resarray_val[0]
					  .nfs_resop4_u.opchunk_commit;
	ck_assert_int_eq(cres->ccr_status, NFS4_OK);
	ck_assert_int_eq(
		cres->CHUNK_COMMIT4res_u.ccr_resok4.ccr_status.ccr_status_val[0],
		NFS4_OK);

	/* Block must now be COMMITTED. */
	ck_assert_int_eq(blk->cb_state, CHUNK_STATE_COMMITTED);

	free_commit_res(cm);
	cm_free(cm);
}
END_TEST

/*
 * Sparse COMMIT: same shape as test_chunk_finalize_skips_empty_in_range
 * but exercises the FINALIZED -> COMMITTED transition through
 * chunk_store_transition.  Same EMPTY-skip rule must apply: COMMIT on
 * a range with EMPTY interior holes finalizes only the FINALIZED
 * blocks and leaves the holes EMPTY.
 */
START_TEST(test_chunk_commit_skips_empty_in_range)
{
	static char buf[CHUNK_SZ];
	/*
	 * cg_client_id must be a non-reserved value
	 * (CHUNK_GUARD_CLIENT_ID_NONE = 0 and CHUNK_GUARD_CLIENT_ID_MDS
	 * = 0xFFFFFFFF are rejected with NFS4ERR_INVAL per
	 * draft-haynes-nfsv4-flexfiles-v2 sec-chunk_guard_none).  0xBEEF
	 * matches the value set_write_args() uses on the writer side so
	 * owner-id lookups across WRITE / FINALIZE / COMMIT line up.
	 */
	chunk_owner4 owner = { .co_guard.cg_client_id = 0xBEEF, .co_id = 99 };
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);

	/* Write block 0. */
	set_write_args(cm, buf, CHUNK_SZ, CHUNK_SZ, 0, NULL, 0);
	nfs4_op_chunk_write(cm->compound);
	ck_assert_int_eq(cm->compound->c_res->resarray.resarray_val[0]
				 .nfs_resop4_u.opchunk_write.cwr_status,
			 NFS4_OK);
	free_write_res(cm);

	/* Write block 4 (leaves 1, 2, 3 as EMPTY holes). */
	cm_reset_slot(cm, 0);
	set_write_args(cm, buf, CHUNK_SZ, CHUNK_SZ, 4, NULL, 0);
	nfs4_op_chunk_write(cm->compound);
	ck_assert_int_eq(cm->compound->c_res->resarray.resarray_val[0]
				 .nfs_resop4_u.opchunk_write.cwr_status,
			 NFS4_OK);
	free_write_res(cm);

	/* FINALIZE the full nominal range -- covers the holes. */
	cm_reset_slot(cm, 0);
	cm_set_op(cm, 0, OP_CHUNK_FINALIZE);
	{
		CHUNK_FINALIZE4args *fargs =
			&cm->compound->c_args->argarray.argarray_val[0]
				 .nfs_argop4_u.opchunk_finalize;
		fargs->cfa_offset = 0;
		fargs->cfa_count = 5;
		fargs->cfa_chunks.cfa_chunks_val = &owner;
		fargs->cfa_chunks.cfa_chunks_len = 1;
	}
	nfs4_op_chunk_finalize(cm->compound);
	ck_assert_int_eq(cm->compound->c_res->resarray.resarray_val[0]
				 .nfs_resop4_u.opchunk_finalize.cfr_status,
			 NFS4_OK);
	free_finalize_res(cm);

	/* COMMIT the full nominal range -- must also skip the holes. */
	cm_reset_slot(cm, 0);
	cm_set_op(cm, 0, OP_CHUNK_COMMIT);
	{
		CHUNK_COMMIT4args *cargs =
			&cm->compound->c_args->argarray.argarray_val[0]
				 .nfs_argop4_u.opchunk_commit;
		cargs->cca_offset = 0;
		cargs->cca_count = 5;
		cargs->cca_chunks.cca_chunks_val = &owner;
		cargs->cca_chunks.cca_chunks_len = 1;
	}
	nfs4_op_chunk_commit(cm->compound);

	CHUNK_COMMIT4res *cres = &cm->compound->c_res->resarray.resarray_val[0]
					  .nfs_resop4_u.opchunk_commit;
	ck_assert_int_eq(cres->ccr_status, NFS4_OK);

	struct chunk_store *cs = g_inode->i_chunk_store;

	ck_assert_int_eq(chunk_store_lookup(cs, 0)->cb_state,
			 CHUNK_STATE_COMMITTED);
	ck_assert_int_eq(cs->cs_blocks[1].cb_state, CHUNK_STATE_EMPTY);
	ck_assert_int_eq(cs->cs_blocks[2].cb_state, CHUNK_STATE_EMPTY);
	ck_assert_int_eq(cs->cs_blocks[3].cb_state, CHUNK_STATE_EMPTY);
	ck_assert_int_eq(chunk_store_lookup(cs, 4)->cb_state,
			 CHUNK_STATE_COMMITTED);

	free_commit_res(cm);
	cm_free(cm);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Group E: CHUNK_READ                                                 */
/* ------------------------------------------------------------------ */

/*
 * CHUNK_READ without a prior write (no chunk store) must return
 * NFS4ERR_NOENT.
 */
START_TEST(test_chunk_read_no_store)
{
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);
	cm_set_op(cm, 0, OP_CHUNK_READ);

	CHUNK_READ4args *args = &cm->compound->c_args->argarray.argarray_val[0]
					 .nfs_argop4_u.opchunk_read;
	memset(&args->cra_stateid, 0, sizeof(args->cra_stateid));
	args->cra_offset = 0;
	args->cra_count = 1;

	nfs4_op_chunk_read(cm->compound);

	CHUNK_READ4res *res = &cm->compound->c_res->resarray.resarray_val[0]
				       .nfs_resop4_u.opchunk_read;
	ck_assert_int_eq(res->crr_status, NFS4ERR_NOENT);

	cm_free(cm);
}
END_TEST

/*
 * CHUNK_READ with count=0 must return NFS4_OK with an empty chunk
 * list and crr_eof=TRUE.  It should not require a chunk store.
 */
START_TEST(test_chunk_read_count_zero)
{
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);
	cm_set_op(cm, 0, OP_CHUNK_READ);

	CHUNK_READ4args *args = &cm->compound->c_args->argarray.argarray_val[0]
					 .nfs_argop4_u.opchunk_read;
	memset(&args->cra_stateid, 0, sizeof(args->cra_stateid));
	args->cra_offset = 0;
	args->cra_count = 0;

	nfs4_op_chunk_read(cm->compound);

	CHUNK_READ4res *res = &cm->compound->c_res->resarray.resarray_val[0]
				       .nfs_resop4_u.opchunk_read;
	ck_assert_int_eq(res->crr_status, NFS4_OK);
	ck_assert_int_eq(res->CHUNK_READ4res_u.crr_resok4.crr_eof, TRUE);
	ck_assert_uint_eq(
		res->CHUNK_READ4res_u.crr_resok4.crr_chunks.crr_chunks_len, 0);

	cm_free(cm);
}
END_TEST

/*
 * CHUNK_READ on a PENDING block (not yet FINALIZED) must return
 * NFS4ERR_NOENT because PENDING blocks are not visible to readers.
 */
/*
 * Track 1b Option C (commit 5b20efae8509,
 * design/chunk-collision-validation.md): CHUNK_READ on a PENDING
 * block returns NFS4ERR_DELAY -- "in-flight write, retry shortly"
 * -- not NFS4ERR_NOENT.  Pre-Option-C this returned NOENT, which
 * the client read as 'no data' and aborted the RMW; the new code
 * lets the client retry until the pending writer finalizes.
 *
 * EMPTY blocks (never written) still return NFS4ERR_NOENT below
 * in a separate test if/when needed; the chunk_store distinguishes
 * "block exists, write in flight" from "block never written."
 */
START_TEST(test_chunk_read_pending_returns_delay)
{
	static char buf[CHUNK_SZ];
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);

	/* Write one block -- it ends up PENDING. */
	set_write_args(cm, buf, CHUNK_SZ, CHUNK_SZ, 0, NULL, 0);
	nfs4_op_chunk_write(cm->compound);
	ck_assert_int_eq(cm->compound->c_res->resarray.resarray_val[0]
				 .nfs_resop4_u.opchunk_write.cwr_status,
			 NFS4_OK);
	free_write_res(cm);

	/* Attempt to read the PENDING block using the same context. */
	cm_reset_slot(cm, 0);
	cm_set_op(cm, 0, OP_CHUNK_READ);

	CHUNK_READ4args *args = &cm->compound->c_args->argarray.argarray_val[0]
					 .nfs_argop4_u.opchunk_read;
	memset(&args->cra_stateid, 0, sizeof(args->cra_stateid));
	args->cra_offset = 0;
	args->cra_count = 1;

	nfs4_op_chunk_read(cm->compound);

	CHUNK_READ4res *res = &cm->compound->c_res->resarray.resarray_val[0]
				       .nfs_resop4_u.opchunk_read;
	ck_assert_int_eq(res->crr_status, NFS4ERR_DELAY);

	cm_free(cm);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Group F: Full state machine roundtrip                               */
/* ------------------------------------------------------------------ */

/*
 * Complete cycle: WRITE -> FINALIZE -> COMMIT -> READ.
 *
 * Verifies that:
 *   - After WRITE:    block is PENDING
 *   - After FINALIZE: block is FINALIZED, read returns data
 *   - After COMMIT:   block is COMMITTED, read still returns data
 *   - Read data matches the written payload
 */
START_TEST(test_chunk_full_cycle)
{
	/* Use a recognisable non-zero payload to verify data integrity. */
	static char wbuf[CHUNK_SZ];

	memset(wbuf, 0xAB, sizeof(wbuf));
	/*
	 * cg_client_id must be a non-reserved value
	 * (CHUNK_GUARD_CLIENT_ID_NONE = 0 and CHUNK_GUARD_CLIENT_ID_MDS
	 * = 0xFFFFFFFF are rejected with NFS4ERR_INVAL per
	 * draft-haynes-nfsv4-flexfiles-v2 sec-chunk_guard_none).  0xBEEF
	 * matches the value set_write_args() uses on the writer side so
	 * owner-id lookups across WRITE / FINALIZE / COMMIT line up.
	 */
	chunk_owner4 owner = { .co_guard.cg_client_id = 0xBEEF, .co_id = 99 };

	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);

	/* -- WRITE -- */
	set_write_args(cm, wbuf, CHUNK_SZ, CHUNK_SZ, 0, NULL, 0);
	nfs4_op_chunk_write(cm->compound);
	ck_assert_int_eq(cm->compound->c_res->resarray.resarray_val[0]
				 .nfs_resop4_u.opchunk_write.cwr_status,
			 NFS4_OK);
	free_write_res(cm);

	ck_assert_int_eq(
		chunk_store_lookup(g_inode->i_chunk_store, 0)->cb_state,
		CHUNK_STATE_PENDING);

	/* -- FINALIZE -- */
	cm_reset_slot(cm, 0);
	cm_set_op(cm, 0, OP_CHUNK_FINALIZE);
	{
		CHUNK_FINALIZE4args *a =
			&cm->compound->c_args->argarray.argarray_val[0]
				 .nfs_argop4_u.opchunk_finalize;
		a->cfa_offset = 0;
		a->cfa_count = 1;
		a->cfa_chunks.cfa_chunks_val = &owner;
		a->cfa_chunks.cfa_chunks_len = 1;
	}
	nfs4_op_chunk_finalize(cm->compound);
	ck_assert_int_eq(cm->compound->c_res->resarray.resarray_val[0]
				 .nfs_resop4_u.opchunk_finalize.cfr_status,
			 NFS4_OK);
	free_finalize_res(cm);

	ck_assert_int_eq(
		chunk_store_lookup(g_inode->i_chunk_store, 0)->cb_state,
		CHUNK_STATE_FINALIZED);

	/* -- READ after FINALIZE -- */
	cm_reset_slot(cm, 0);
	cm_set_op(cm, 0, OP_CHUNK_READ);
	{
		CHUNK_READ4args *a =
			&cm->compound->c_args->argarray.argarray_val[0]
				 .nfs_argop4_u.opchunk_read;
		memset(&a->cra_stateid, 0, sizeof(a->cra_stateid));
		a->cra_offset = 0;
		a->cra_count = 1;
	}
	nfs4_op_chunk_read(cm->compound);
	{
		CHUNK_READ4res *res =
			&cm->compound->c_res->resarray.resarray_val[0]
				 .nfs_resop4_u.opchunk_read;
		ck_assert_int_eq(res->crr_status, NFS4_OK);

		CHUNK_READ4resok *resok = &res->CHUNK_READ4res_u.crr_resok4;

		ck_assert_uint_eq(resok->crr_chunks.crr_chunks_len, 1);
		ck_assert_uint_eq(
			resok->crr_chunks.crr_chunks_val[0].cr_effective_len,
			CHUNK_SZ);
		ck_assert_uint_eq(resok->crr_chunks.crr_chunks_val[0]
					  .cr_chunk.cr_chunk_len,
				  CHUNK_SZ);

		/* Data must match what was written. */
		ck_assert_int_eq(memcmp(resok->crr_chunks.crr_chunks_val[0]
						.cr_chunk.cr_chunk_val,
					wbuf, CHUNK_SZ),
				 0);
	}
	free_read_res(cm);

	/* -- COMMIT -- */
	cm_reset_slot(cm, 0);
	cm_set_op(cm, 0, OP_CHUNK_COMMIT);
	{
		CHUNK_COMMIT4args *a =
			&cm->compound->c_args->argarray.argarray_val[0]
				 .nfs_argop4_u.opchunk_commit;
		a->cca_offset = 0;
		a->cca_count = 1;
		a->cca_chunks.cca_chunks_val = &owner;
		a->cca_chunks.cca_chunks_len = 1;
	}
	nfs4_op_chunk_commit(cm->compound);
	ck_assert_int_eq(cm->compound->c_res->resarray.resarray_val[0]
				 .nfs_resop4_u.opchunk_commit.ccr_status,
			 NFS4_OK);
	free_commit_res(cm);

	ck_assert_int_eq(
		chunk_store_lookup(g_inode->i_chunk_store, 0)->cb_state,
		CHUNK_STATE_COMMITTED);

	/* -- READ after COMMIT -- data still accessible -- */
	cm_reset_slot(cm, 0);
	cm_set_op(cm, 0, OP_CHUNK_READ);
	{
		CHUNK_READ4args *a =
			&cm->compound->c_args->argarray.argarray_val[0]
				 .nfs_argop4_u.opchunk_read;
		memset(&a->cra_stateid, 0, sizeof(a->cra_stateid));
		a->cra_offset = 0;
		a->cra_count = 1;
	}
	nfs4_op_chunk_read(cm->compound);
	{
		CHUNK_READ4res *res =
			&cm->compound->c_res->resarray.resarray_val[0]
				 .nfs_resop4_u.opchunk_read;
		ck_assert_int_eq(res->crr_status, NFS4_OK);

		CHUNK_READ4resok *resok = &res->CHUNK_READ4res_u.crr_resok4;

		ck_assert_uint_eq(resok->crr_chunks.crr_chunks_len, 1);
		ck_assert_int_eq(memcmp(resok->crr_chunks.crr_chunks_val[0]
						.cr_chunk.cr_chunk_val,
					wbuf, CHUNK_SZ),
				 0);
	}
	free_read_res(cm);

	cm_free(cm);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Group G: chunk-collision counter observability (Phase 4b.7)         */
/*                                                                     */
/* The cs_pending_displaced counter increments when a CHUNK_WRITE      */
/* lands at an offset whose previous PENDING block came from a         */
/* different writer (cb_gen_id / cb_client_id / cb_owner_id differs).  */
/* The PS-pipeline framing in the design document ("two simulated PS   */
/* clientids") is conceptual; the actual contract under test is the   */
/* per-sb chunk-stats increment logic, which the design's chunk-       */
/* collision validation slice (BLOCKER 2) shipped just before this    */
/* slice.  Group E in proxy-server-phase4b.md.                         */
/* ------------------------------------------------------------------ */

/*
 * Override the owner fields on the CHUNK_WRITE in slot 0 so the
 * test can simulate two distinct writers contending on the same
 * block.  Call AFTER set_write_args.
 */
static void set_owner(struct cm_ctx *cm, uint64_t gen_id, uint64_t client_id,
		      uint64_t owner_id)
{
	CHUNK_WRITE4args *args = &cm->compound->c_args->argarray.argarray_val[0]
					  .nfs_argop4_u.opchunk_write;

	args->cwa_owner.co_guard.cg_gen_id = gen_id;
	args->cwa_owner.co_guard.cg_client_id = client_id;
	args->cwa_owner.co_id = owner_id;
}

START_TEST(test_multi_ps_disjoint_stripes_no_collisions)
{
	static char buf[CHUNK_SZ];
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);

	/*
	 * PS-A writes to block 0; PS-B writes to block 1.  Disjoint
	 * offsets so the chunk-collision detection in chunk.c never
	 * sees a PENDING block from a different owner.
	 */
	set_write_args(cm, buf, CHUNK_SZ, CHUNK_SZ, /* offset */ 0, NULL, 0);
	set_owner(cm, /* gen_id */ 1, /* client_id */ 0xA1, /* owner_id */ 10);
	nfs4_op_chunk_write(cm->compound);
	{
		CHUNK_WRITE4res *res =
			&cm->compound->c_res->resarray.resarray_val[0]
				 .nfs_resop4_u.opchunk_write;
		ck_assert_int_eq(res->cwr_status, NFS4_OK);
	}
	free_write_res(cm);
	cm_reset_slot(cm, 0);

	set_write_args(cm, buf, CHUNK_SZ, CHUNK_SZ, /* block index */ 1, NULL,
		       0);
	set_owner(cm, /* gen_id */ 2, /* client_id */ 0xB2, /* owner_id */ 20);
	nfs4_op_chunk_write(cm->compound);
	{
		CHUNK_WRITE4res *res =
			&cm->compound->c_res->resarray.resarray_val[0]
				 .nfs_resop4_u.opchunk_write;
		ck_assert_int_eq(res->cwr_status, NFS4_OK);
	}
	free_write_res(cm);

	/*
	 * Disjoint stripes -> no collision counter increment.  Per-
	 * stripe ordering in pNFS-FF v2 keeps writers off each other's
	 * blocks; this is the smoking-gun that the design's "two PS
	 * clientids COMMIT disjoint stripes" pattern does not produce
	 * spurious counter bumps.
	 */
	ck_assert_uint_eq(
		atomic_load_explicit(&g_sb->sb_chunk_stats.cs_pending_displaced,
				     memory_order_relaxed),
		0);

	cm_free(cm);
}
END_TEST

START_TEST(test_multi_ps_overlap_stripe_increments_displaced)
{
	static char buf[CHUNK_SZ];
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);

	/* PS-A writes to block 0 with owner A. */
	set_write_args(cm, buf, CHUNK_SZ, CHUNK_SZ, /* offset */ 0, NULL, 0);
	set_owner(cm, /* gen_id */ 1, /* client_id */ 0xA1, /* owner_id */ 10);
	nfs4_op_chunk_write(cm->compound);
	{
		CHUNK_WRITE4res *res =
			&cm->compound->c_res->resarray.resarray_val[0]
				 .nfs_resop4_u.opchunk_write;
		ck_assert_int_eq(res->cwr_status, NFS4_OK);
	}
	free_write_res(cm);
	cm_reset_slot(cm, 0);

	/*
	 * PS-B writes to block 0 with a different owner triple.  Per
	 * Option C (commit 03d91554a34c, design/chunk-collision-
	 * validation.md), the chunk-store gate rejects this write with
	 * NFS4ERR_DELAY: PS-B's owner triple does not match the PENDING
	 * block PS-A still holds.  PS-B's client is expected to retry
	 * the whole RMW after PS-A's FINALIZE+COMMIT.  The counter
	 * cs_chunk_busy_delay (which replaced the observational
	 * cs_pending_displaced for the post-Option-C call sites) records
	 * the rejection.
	 */
	set_write_args(cm, buf, CHUNK_SZ, CHUNK_SZ, /* offset */ 0, NULL, 0);
	set_owner(cm, /* gen_id */ 2, /* client_id */ 0xB2, /* owner_id */ 20);
	nfs4_op_chunk_write(cm->compound);
	{
		CHUNK_WRITE4res *res =
			&cm->compound->c_res->resarray.resarray_val[0]
				 .nfs_resop4_u.opchunk_write;
		ck_assert_int_eq(res->cwr_status, NFS4ERR_DELAY);
	}
	free_write_res(cm);

	/*
	 * cs_chunk_busy_delay must have incremented for the rejected
	 * second write.  cs_pending_displaced stays at zero -- the
	 * Option C gate fires BEFORE the older displaced-counting code
	 * runs, and the displaced-counting code is dead in the new
	 * path (it counted a state -- "PENDING block overwritten by a
	 * different owner" -- that no longer exists, the gate prevents
	 * the overwrite).
	 */
	ck_assert_uint_ge(
		atomic_load_explicit(&g_sb->sb_chunk_stats.cs_chunk_busy_delay,
				     memory_order_relaxed),
		1);

	cm_free(cm);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Group H: INV-1 partial-stripe write instrumentation                 */
/*                                                                     */
/* Quantifies what DSes actually see during T1b / T2 to answer         */
/* Hellwig msg 5 (in-place update) + msg 9 (NFS block size).           */
/* See .claude/design/inv1-ds-instrumentation.md.                      */
/* ------------------------------------------------------------------ */

/*
 * Single CHUNK_WRITE with len == chunk_size: counts as a full block,
 * a first-write (empty prior state), and a 1-block batch.
 */
START_TEST(test_inv1_full_block_counted)
{
	static char buf[CHUNK_SZ];
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);
	set_write_args(cm, buf, CHUNK_SZ, CHUNK_SZ, /* offset */ 0, NULL, 0);
	nfs4_op_chunk_write(cm->compound);
	{
		CHUNK_WRITE4res *res =
			&cm->compound->c_res->resarray.resarray_val[0]
				 .nfs_resop4_u.opchunk_write;
		ck_assert_int_eq(res->cwr_status, NFS4_OK);
	}
	free_write_res(cm);

	struct reffs_chunk_stats *st = &g_sb->sb_chunk_stats;

	ck_assert_uint_eq(atomic_load_explicit(&st->cs_blocks_full,
					       memory_order_relaxed),
			  1);
	ck_assert_uint_eq(atomic_load_explicit(&st->cs_blocks_partial,
					       memory_order_relaxed),
			  0);
	ck_assert_uint_eq(atomic_load_explicit(&st->cs_writes_1block,
					       memory_order_relaxed),
			  1);

	cm_free(cm);
}
END_TEST

/*
 * Two-block CHUNK_WRITE where total_data is not a multiple of
 * chunk_size.  The chunk.c loop sizes the last block to the
 * remainder; that block is counted as partial.
 */
START_TEST(test_inv1_partial_tail_counted)
{
	const uint32_t total = CHUNK_SZ + (CHUNK_SZ / 2); /* 1.5 chunks */
	static char buf[CHUNK_SZ + CHUNK_SZ / 2];
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);
	set_write_args(cm, buf, total, CHUNK_SZ, /* offset */ 0, NULL, 0);
	nfs4_op_chunk_write(cm->compound);
	{
		CHUNK_WRITE4res *res =
			&cm->compound->c_res->resarray.resarray_val[0]
				 .nfs_resop4_u.opchunk_write;
		ck_assert_int_eq(res->cwr_status, NFS4_OK);
	}
	free_write_res(cm);

	struct reffs_chunk_stats *st = &g_sb->sb_chunk_stats;

	ck_assert_uint_eq(atomic_load_explicit(&st->cs_blocks_full,
					       memory_order_relaxed),
			  1);
	ck_assert_uint_eq(atomic_load_explicit(&st->cs_blocks_partial,
					       memory_order_relaxed),
			  1);
	/* Two blocks in one write -> 2to7 bucket. */
	ck_assert_uint_eq(atomic_load_explicit(&st->cs_writes_2to7,
					       memory_order_relaxed),
			  1);

	cm_free(cm);
}
END_TEST

/*
 * Single CHUNK_WRITE into an empty chunk_store: prior block is EMPTY,
 * counted as first-write.
 */
START_TEST(test_inv1_first_write_counted)
{
	static char buf[CHUNK_SZ];
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);
	set_write_args(cm, buf, CHUNK_SZ, CHUNK_SZ, /* offset */ 0, NULL, 0);
	nfs4_op_chunk_write(cm->compound);
	free_write_res(cm);

	struct reffs_chunk_stats *st = &g_sb->sb_chunk_stats;

	ck_assert_uint_eq(atomic_load_explicit(&st->cs_blocks_first_write,
					       memory_order_relaxed),
			  1);
	ck_assert_uint_eq(atomic_load_explicit(&st->cs_blocks_overwrite,
					       memory_order_relaxed),
			  0);

	cm_free(cm);
}
END_TEST

/*
 * Two writes to the same offset: first counts as first-write, second
 * counts as overwrite.  Move in lockstep with cs_pending_displaced --
 * verifies the new counters are wired alongside the existing one
 * without disturbing it.
 */
START_TEST(test_inv1_overwrite_counted)
{
	static char buf[CHUNK_SZ];
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);

	/* PS-A first write -> EMPTY prior state, first-write. */
	set_write_args(cm, buf, CHUNK_SZ, CHUNK_SZ, /* offset */ 0, NULL, 0);
	set_owner(cm, /* gen_id */ 1, /* client_id */ 0xA1, /* owner_id */ 10);
	nfs4_op_chunk_write(cm->compound);
	free_write_res(cm);
	cm_reset_slot(cm, 0);

	/*
	 * PS-B write to same offset.  Per Option C
	 * (design/chunk-collision-validation.md), the chunk-store
	 * rejects cross-owner overwrite of a PENDING block: PS-B gets
	 * NFS4ERR_DELAY.  The cs_blocks_overwrite counter is therefore
	 * NOT incremented (the metadata-recording loop runs only on
	 * the accept path), and cs_chunk_busy_delay records the
	 * rejection instead.
	 */
	set_write_args(cm, buf, CHUNK_SZ, CHUNK_SZ, /* offset */ 0, NULL, 0);
	set_owner(cm, /* gen_id */ 2, /* client_id */ 0xB2, /* owner_id */ 20);
	nfs4_op_chunk_write(cm->compound);
	{
		CHUNK_WRITE4res *res =
			&cm->compound->c_res->resarray.resarray_val[0]
				 .nfs_resop4_u.opchunk_write;
		ck_assert_int_eq(res->cwr_status, NFS4ERR_DELAY);
	}
	free_write_res(cm);

	struct reffs_chunk_stats *st = &g_sb->sb_chunk_stats;

	ck_assert_uint_eq(atomic_load_explicit(&st->cs_blocks_first_write,
					       memory_order_relaxed),
			  1);
	/* No overwrite happened -- the gate rejected before the
	 * metadata loop ran. */
	ck_assert_uint_eq(atomic_load_explicit(&st->cs_blocks_overwrite,
					       memory_order_relaxed),
			  0);
	/* Gate fired -- this is the post-Option-C contention signal. */
	ck_assert_uint_eq(atomic_load_explicit(&st->cs_chunk_busy_delay,
					       memory_order_relaxed),
			  1);

	cm_free(cm);
}
END_TEST

/*
 * Three writes with 1, 4, and 16 blocks respectively.  Each lands in
 * a distinct bucket: cs_writes_1block, cs_writes_2to7, cs_writes_8to31.
 * The 32+ bucket stays at zero -- the bucket boundaries are what they
 * are; if they ever move, this test must move with them.
 */
START_TEST(test_inv1_batch_histogram)
{
	static char buf[16 * CHUNK_SZ];
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);

	/* 1-block write at offset 0. */
	set_write_args(cm, buf, CHUNK_SZ, CHUNK_SZ, /* offset */ 0, NULL, 0);
	nfs4_op_chunk_write(cm->compound);
	free_write_res(cm);
	cm_reset_slot(cm, 0);

	/* 4-block write at offset 1 (PENDING -> overwrites the first
	 * block once, then writes 3 fresh ones -- bucket counter cares
	 * about nchunks per call, not collisions). */
	set_write_args(cm, buf, 4 * CHUNK_SZ, CHUNK_SZ, /* offset */ 1, NULL,
		       0);
	nfs4_op_chunk_write(cm->compound);
	free_write_res(cm);
	cm_reset_slot(cm, 0);

	/* 16-block write at offset 100 (well past prior blocks). */
	set_write_args(cm, buf, 16 * CHUNK_SZ, CHUNK_SZ, /* offset */ 100, NULL,
		       0);
	nfs4_op_chunk_write(cm->compound);
	free_write_res(cm);

	struct reffs_chunk_stats *st = &g_sb->sb_chunk_stats;

	ck_assert_uint_eq(atomic_load_explicit(&st->cs_writes_1block,
					       memory_order_relaxed),
			  1);
	ck_assert_uint_eq(atomic_load_explicit(&st->cs_writes_2to7,
					       memory_order_relaxed),
			  1);
	ck_assert_uint_eq(atomic_load_explicit(&st->cs_writes_8to31,
					       memory_order_relaxed),
			  1);
	ck_assert_uint_eq(atomic_load_explicit(&st->cs_writes_32plus,
					       memory_order_relaxed),
			  0);

	cm_free(cm);
}
END_TEST

/*
 * Empty chunk_store: count_runs returns 0.
 *
 * NULL-tolerant per the function's doc-comment -- callers that
 * enumerate inodes from a fill_sb_info path may hit inodes whose
 * i_chunk_store has never been allocated.
 */
START_TEST(test_inv1_fragmentation_zero_runs)
{
	ck_assert_uint_eq(chunk_store_count_runs(NULL), 0);
}
END_TEST

/*
 * Dense write fills blocks [0..3] PENDING -- one contiguous run.
 */
START_TEST(test_inv1_fragmentation_one_run)
{
	static char buf[4 * CHUNK_SZ];
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);
	set_write_args(cm, buf, 4 * CHUNK_SZ, CHUNK_SZ, /* offset */ 0, NULL,
		       0);
	nfs4_op_chunk_write(cm->compound);
	free_write_res(cm);

	ck_assert_uint_eq(chunk_store_count_runs(g_inode->i_chunk_store), 1);

	cm_free(cm);
}
END_TEST

/*
 * Three sparse writes at offsets 0, 8, 16 produce three runs separated
 * by EMPTY gaps.  Sweep must count each run exactly once.
 */
START_TEST(test_inv1_fragmentation_three_runs)
{
	static char buf[CHUNK_SZ];
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);

	const uint64_t offsets[] = { 0, 8, 16 };

	for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
		set_write_args(cm, buf, CHUNK_SZ, CHUNK_SZ, offsets[i], NULL,
			       0);
		nfs4_op_chunk_write(cm->compound);
		free_write_res(cm);
		cm_reset_slot(cm, 0);
	}

	ck_assert_uint_eq(chunk_store_count_runs(g_inode->i_chunk_store), 3);

	cm_free(cm);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite                                                               */
/* ------------------------------------------------------------------ */

static Suite *chunk_suite(void)
{
	Suite *s = suite_create("chunk");

	TCase *tc_a = tcase_create("validation");
	tcase_add_checked_fixture(tc_a, chunk_setup, chunk_teardown);
	tcase_add_test(tc_a, test_chunk_write_no_fh);
	tcase_add_test(tc_a, test_chunk_write_zero_chunk_size);
	tcase_add_test(tc_a, test_chunk_write_crc_mismatch);
	tcase_add_test(tc_a, test_chunk_write_not_regular_file);
	suite_add_tcase(s, tc_a);

	TCase *tc_b = tcase_create("chunk_write");
	tcase_add_checked_fixture(tc_b, chunk_setup, chunk_teardown);
	tcase_add_test(tc_b, test_chunk_write_single_block);
	tcase_add_test(tc_b, test_chunk_write_multi_block);
	tcase_add_test(tc_b, test_chunk_write_updates_inode_size);
	tcase_add_test(tc_b, test_chunk_write_valid_crc);
	tcase_add_test(tc_b, test_chunk_write_unknown_algorithm_rejected);
	tcase_add_test(tc_b, test_chunk_write_wrong_length_rejected);
	tcase_add_test(tc_b, test_chunk_write_per_file_algorithm_consistency);
	suite_add_tcase(s, tc_b);

	TCase *tc_c = tcase_create("chunk_finalize");
	tcase_add_checked_fixture(tc_c, chunk_setup, chunk_teardown);
	tcase_add_test(tc_c, test_chunk_finalize_no_store);
	tcase_add_test(tc_c, test_chunk_finalize_transitions_state);
	tcase_add_test(tc_c, test_chunk_finalize_skips_empty_in_range);
	suite_add_tcase(s, tc_c);

	TCase *tc_d = tcase_create("chunk_commit");
	tcase_add_checked_fixture(tc_d, chunk_setup, chunk_teardown);
	tcase_add_test(tc_d, test_chunk_commit_no_store);
	tcase_add_test(tc_d, test_chunk_commit_transitions_state);
	tcase_add_test(tc_d, test_chunk_commit_skips_empty_in_range);
	suite_add_tcase(s, tc_d);

	TCase *tc_e = tcase_create("chunk_read");
	tcase_add_checked_fixture(tc_e, chunk_setup, chunk_teardown);
	tcase_add_test(tc_e, test_chunk_read_no_store);
	tcase_add_test(tc_e, test_chunk_read_count_zero);
	tcase_add_test(tc_e, test_chunk_read_pending_returns_delay);
	suite_add_tcase(s, tc_e);

	TCase *tc_f = tcase_create("full_cycle");
	tcase_add_checked_fixture(tc_f, chunk_setup, chunk_teardown);
	tcase_add_test(tc_f, test_chunk_full_cycle);
	suite_add_tcase(s, tc_f);

	TCase *tc_g = tcase_create("collision_counter");
	tcase_add_checked_fixture(tc_g, chunk_setup, chunk_teardown);
	tcase_add_test(tc_g, test_multi_ps_disjoint_stripes_no_collisions);
	tcase_add_test(tc_g, test_multi_ps_overlap_stripe_increments_displaced);
	suite_add_tcase(s, tc_g);

	TCase *tc_h = tcase_create("inv1_instrumentation");
	tcase_add_checked_fixture(tc_h, chunk_setup, chunk_teardown);
	tcase_add_test(tc_h, test_inv1_full_block_counted);
	tcase_add_test(tc_h, test_inv1_partial_tail_counted);
	tcase_add_test(tc_h, test_inv1_first_write_counted);
	tcase_add_test(tc_h, test_inv1_overwrite_counted);
	tcase_add_test(tc_h, test_inv1_batch_histogram);
	tcase_add_test(tc_h, test_inv1_fragmentation_zero_runs);
	tcase_add_test(tc_h, test_inv1_fragmentation_one_run);
	tcase_add_test(tc_h, test_inv1_fragmentation_three_runs);
	suite_add_tcase(s, tc_h);

	return s;
}

int main(void)
{
	return reffs_test_run_suite(chunk_suite(), NULL, NULL);
}
