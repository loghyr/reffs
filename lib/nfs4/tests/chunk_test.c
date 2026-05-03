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
 * table validation is bypassed.  Caller provides the data buffer and CRC
 * array (pass NULL/0 for no CRC validation).
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
	args->cwa_crc32s.cwa_crc32s_val = crcs;
	args->cwa_crc32s.cwa_crc32s_len = ncrc;
	args->cwa_payload_id = 0x4242;
	args->cwa_owner.co_guard.cg_gen_id = 7;
	args->cwa_owner.co_guard.cg_client_id = 0xBEEF;
	args->cwa_owner.co_id = 99;
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

	/* Stored CRC must match what we provided. */
	struct chunk_block *blk = chunk_store_lookup(g_inode->i_chunk_store, 0);

	ck_assert_ptr_nonnull(blk);
	ck_assert_uint_eq(blk->cb_crc32, good_crc);

	free_write_res(cm);
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

	chunk_owner4 owner = { .co_id = 99 };
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

	chunk_owner4 owner = { .co_id = 99 };
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

	chunk_owner4 owner = { .co_id = 99 };
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

	chunk_owner4 owner = { .co_id = 99 };
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
	chunk_owner4 owner = { .co_id = 99 };
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
	chunk_owner4 owner = { .co_id = 99 };
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
START_TEST(test_chunk_read_pending_not_visible)
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
	ck_assert_int_eq(res->crr_status, NFS4ERR_NOENT);

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
	chunk_owner4 owner = { .co_id = 99 };

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
	tcase_add_test(tc_e, test_chunk_read_pending_not_visible);
	suite_add_tcase(s, tc_e);

	TCase *tc_f = tcase_create("full_cycle");
	tcase_add_checked_fixture(tc_f, chunk_setup, chunk_teardown);
	tcase_add_test(tc_f, test_chunk_full_cycle);
	suite_add_tcase(s, tc_f);

	return s;
}

int main(void)
{
	return reffs_test_run_suite(chunk_suite(), NULL, NULL);
}
