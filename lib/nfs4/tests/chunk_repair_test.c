/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Unit tests for OP_CHUNK_WRITE_REPAIR (DS-side) and OP_CHUNK_REPAIRED
 * (MDS-side), the wire-level EC repair operations defined in
 * draft-haynes-nfsv4-flexfiles-v2.
 *
 * Slice 1 (this file's initial cut) covers OP_CHUNK_WRITE_REPAIR only.
 * Group F tests for OP_CHUNK_REPAIRED land in Slice 2.
 *
 * The handler is currently a NFS4ERR_NOTSUPP stub at chunk.c:1293; the
 * tests below define the contract the slice-1 implementation must
 * satisfy.  Until then these tests fail by design (TDD red baseline).
 *
 * Groups:
 *   A. Input validation -- missing FH, wrong type, zero sizes, reserved
 *      client id, CRC mismatch, per-file algorithm consistency.
 *   B. Stateid auth -- special-stateid rejected, unknown-stateid rejected,
 *      expired entry, TRUST_PENDING -> NFS4ERR_DELAY, READ-iomode entry
 *      rejected for repair-write.
 *   C. Happy path -- block transitions, CHUNK_BLOCK_REPAIR_PROVENANCE
 *      flag set, cs_repair_initiated bumped, multi-block, bypass of
 *      Track 1b PENDING-collision gate.
 *   D. Persistence -- REPAIR_PROVENANCE flag round-trips through
 *      chunk_store persist + load.
 *   E. Idempotence -- repair-write of identical bytes over already-
 *      COMMITTED block returns NFS4_OK per-chunk; counter still bumps.
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
#include <time.h>

#include <check.h>
#include <urcu.h>
#include <zlib.h>

#include "nfsv42_xdr.h"
#include "reffs/filehandle.h"
#include "reffs/inode.h"
#include "reffs/server.h"
#include "reffs/super_block.h"
#include "reffs/layout_segment.h"
#include "nfs4/chunk_checksum.h"
#include "nfs4/chunk_store.h"
#include "nfs4/client.h"
#include "nfs4/compound.h"
#include "nfs4/ops.h"
#include "nfs4/trust_stateid.h"

#include "nfs4_test_harness.h"

/* ------------------------------------------------------------------ */
/* Compound mock helpers (adapted from chunk_test.c + trust_stateid_test.c) */
/* ------------------------------------------------------------------ */

struct cm_ctx {
	struct rpc_trans rt;
	struct task task;
	struct compound *compound;
	struct nfs4_client *nc;
};

/*
 * Per-call client id counter so successive cm_alloc() calls do
 * not collide in the nfs4_client hash table on duplicate clientid.
 * The chunk_test.c equivalent works around this by reusing a single
 * cm_ctx across slots; for repair tests that need two independent
 * (writer + repair-writer) compounds, allocating distinct clientids
 * is simpler.
 */
static atomic_uint g_cm_alloc_seq;

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
	unsigned int cid = 0xDEAD0000U +
			   atomic_fetch_add_explicit(&g_cm_alloc_seq, 1,
						     memory_order_relaxed) +
			   1;

	memset(&v, 0x22, sizeof(v));
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(0x7f000004);
	sin.sin_port = htons(2049);

	cm->nc = nfs4_client_alloc(&v, &sin, 1, cid, 0);
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
		if (c->c_nfs4_client)
			nfs4_client_put(c->c_nfs4_client);
		free(c->c_args->argarray.argarray_val);
		free(c->c_res->resarray.resarray_val);
		free(c->c_args);
		free(c->c_res);
		free(c);
	}
	free(cm);
}

/* ------------------------------------------------------------------ */
/* Stateid helpers (from trust_stateid_test.c)                         */
/* ------------------------------------------------------------------ */

static stateid4 make_stateid(uint8_t fill)
{
	stateid4 s;

	s.seqid = 1;
	memset(s.other, fill, NFS4_OTHER_SIZE);
	return s;
}

/*
 * CLOCK_MONOTONIC deadline 5 s in the future -- comfortably outside
 * any test's runtime.  trust_stateid_register's expire_mono_ns is
 * monotonic ns; the trust-table check rejects entries with exp != 0
 * and now > exp.
 */
static uint64_t future_expire_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec +
	       5000000000ULL;
}

/*
 * CLOCK_MONOTONIC deadline in the past -- triggers the expired-entry
 * branch in trust_stateid_find without waiting.
 */
static uint64_t past_expire_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	uint64_t now =
		(uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
	return now > 1000000ULL ? now - 1000000ULL : 1;
}

/* ------------------------------------------------------------------ */
/* Fixture: trust table is initialised by nfs4_test_setup() via        */
/* nfs4_protocol_register; do not double-init.                          */
/* ------------------------------------------------------------------ */

static struct super_block *g_sb;
static struct inode *g_inode;

static void repair_setup(void)
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

static void repair_teardown(void)
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
 * Build CHUNK_WRITE_REPAIR args in slot 0.  The repair op carries a
 * non-special stateid (the trust-table layer authorises) -- caller
 * provides it.  CRCs are wrapped into checksum4 per element of crcs[];
 * pass NULL/0 to skip checksum validation.
 *
 * The cs_value buffers and the checksum4 array are heap-allocated here
 * and released by free_repair_args.
 */
static void set_repair_args(struct cm_ctx *cm, const stateid4 *stid, char *buf,
			    uint32_t buf_len, uint32_t chunk_size,
			    uint64_t offset, uint32_t *crcs, uint32_t ncrc)
{
	cm_set_op(cm, 0, OP_CHUNK_WRITE_REPAIR);
	CHUNK_WRITE_REPAIR4args *args =
		&cm->compound->c_args->argarray.argarray_val[0]
			 .nfs_argop4_u.opchunk_write_repair;

	args->cwra_stateid = *stid;
	args->cwra_offset = offset;
	args->cwra_stable = UNSTABLE4;
	args->cwra_payload_id = 0;
	args->cwra_chunk_size = chunk_size;
	args->cwra_chunks.cwra_chunks_val = buf;
	args->cwra_chunks.cwra_chunks_len = buf_len;

	/* cwra_owner.co_guard.cg_client_id is the wire-level repair
	 * owner; use a real client id (not the reserved NONE/MDS
	 * values).  cg_seq is the per-owner monotonic counter; the
	 * handler does not interpret it.
	 */
	args->cwra_owner.co_guard.cg_client_id = 0xDEAD0001;
	args->cwra_owner.co_guard.cg_gen_id = 1;

	if (ncrc > 0 && crcs != NULL) {
		args->cwra_checksums.cwra_checksums_val =
			calloc(ncrc, sizeof(checksum4));
		args->cwra_checksums.cwra_checksums_len = ncrc;
		for (uint32_t i = 0; i < ncrc; i++) {
			(void)chunk_checksum_pack_crc32(
				&args->cwra_checksums.cwra_checksums_val[i],
				crcs[i]);
		}
	} else {
		args->cwra_checksums.cwra_checksums_val = NULL;
		args->cwra_checksums.cwra_checksums_len = 0;
	}
}

static void free_repair_args(struct cm_ctx *cm)
{
	CHUNK_WRITE_REPAIR4args *args =
		&cm->compound->c_args->argarray.argarray_val[0]
			 .nfs_argop4_u.opchunk_write_repair;

	if (args->cwra_checksums.cwra_checksums_val) {
		for (uint32_t i = 0;
		     i < args->cwra_checksums.cwra_checksums_len; i++) {
			free(args->cwra_checksums.cwra_checksums_val[i]
				     .cs_value.cs_value_val);
		}
		free(args->cwra_checksums.cwra_checksums_val);
		args->cwra_checksums.cwra_checksums_val = NULL;
		args->cwra_checksums.cwra_checksums_len = 0;
	}
}

static void free_repair_res(struct cm_ctx *cm)
{
	CHUNK_WRITE_REPAIR4res *res =
		&cm->compound->c_res->resarray.resarray_val[0]
			 .nfs_resop4_u.opchunk_write_repair;
	CHUNK_WRITE_REPAIR4resok *resok =
		&res->CHUNK_WRITE_REPAIR4res_u.cwrr_resok4;

	free(resok->cwrr_status.cwrr_status_val);
	resok->cwrr_status.cwrr_status_val = NULL;
}

/*
 * Register a trust-table entry mapping stid -> (inode, clientid) with
 * the given iomode.  Returns once the registration is committed so the
 * handler's trust_stateid_find sees it.
 */
static void register_trust(const stateid4 *stid, uint64_t ino, clientid4 cid,
			   layoutiomode4 iomode, uint64_t expire_ns)
{
	int ret = trust_stateid_register(stid, ino, cid, iomode, expire_ns, "");

	ck_assert_int_eq(ret, 0);
}

/* ------------------------------------------------------------------ */
/* Group A: Input validation                                           */
/* ------------------------------------------------------------------ */

/* No current filehandle -> NFS4ERR_NOFILEHANDLE. */
START_TEST(test_repair_no_fh)
{
	static char buf[CHUNK_SZ];
	struct cm_ctx *cm = cm_alloc(1);
	stateid4 stid = make_stateid(0xA1);

	/* Deliberately no cm_set_inode -- FH remains empty. */
	set_repair_args(cm, &stid, buf, CHUNK_SZ, CHUNK_SZ, 0, NULL, 0);

	nfs4_op_chunk_write_repair(cm->compound);

	CHUNK_WRITE_REPAIR4res *res =
		&cm->compound->c_res->resarray.resarray_val[0]
			 .nfs_resop4_u.opchunk_write_repair;

	ck_assert_int_eq(res->cwrr_status, NFS4ERR_NOFILEHANDLE);

	free_repair_args(cm);
	cm_free(cm);
}
END_TEST

/* Directory FH -> NFS4ERR_INVAL (regular file required for chunk ops). */
START_TEST(test_repair_not_regular_file)
{
	static char buf[CHUNK_SZ];
	struct cm_ctx *cm = cm_alloc(1);
	stateid4 stid = make_stateid(0xA2);

	uint64_t ino =
		__atomic_add_fetch(&g_sb->sb_next_ino, 1, __ATOMIC_RELAXED);
	struct inode *dir = inode_alloc(g_sb, ino);

	ck_assert_ptr_nonnull(dir);
	dir->i_mode = S_IFDIR | 0750;

	cm_set_inode(cm, dir);
	set_repair_args(cm, &stid, buf, CHUNK_SZ, CHUNK_SZ, 0, NULL, 0);

	nfs4_op_chunk_write_repair(cm->compound);

	CHUNK_WRITE_REPAIR4res *res =
		&cm->compound->c_res->resarray.resarray_val[0]
			 .nfs_resop4_u.opchunk_write_repair;

	ck_assert_int_eq(res->cwrr_status, NFS4ERR_INVAL);

	free_repair_args(cm);
	cm_free(cm);
	inode_active_put(dir);
}
END_TEST

/* cwra_chunk_size == 0 -> NFS4ERR_INVAL. */
START_TEST(test_repair_zero_chunk_size)
{
	static char buf[CHUNK_SZ];
	struct cm_ctx *cm = cm_alloc(1);
	stateid4 stid = make_stateid(0xA3);

	cm_set_inode(cm, g_inode);
	set_repair_args(cm, &stid, buf, CHUNK_SZ, 0, 0, NULL, 0);

	nfs4_op_chunk_write_repair(cm->compound);

	CHUNK_WRITE_REPAIR4res *res =
		&cm->compound->c_res->resarray.resarray_val[0]
			 .nfs_resop4_u.opchunk_write_repair;

	ck_assert_int_eq(res->cwrr_status, NFS4ERR_INVAL);

	free_repair_args(cm);
	cm_free(cm);
}
END_TEST

/* cwra_chunks_len == 0 -> NFS4ERR_INVAL (nothing to write). */
START_TEST(test_repair_zero_chunks_len)
{
	struct cm_ctx *cm = cm_alloc(1);
	stateid4 stid = make_stateid(0xA4);

	cm_set_inode(cm, g_inode);
	set_repair_args(cm, &stid, NULL, 0, CHUNK_SZ, 0, NULL, 0);

	nfs4_op_chunk_write_repair(cm->compound);

	CHUNK_WRITE_REPAIR4res *res =
		&cm->compound->c_res->resarray.resarray_val[0]
			 .nfs_resop4_u.opchunk_write_repair;

	ck_assert_int_eq(res->cwrr_status, NFS4ERR_INVAL);

	free_repair_args(cm);
	cm_free(cm);
}
END_TEST

/*
 * Reserved owner client id (CHUNK_GUARD_CLIENT_ID_NONE = 0,
 * CHUNK_GUARD_CLIENT_ID_MDS = 1) -> NFS4ERR_INVAL.  The repair op is
 * never issued by NONE or MDS; only a real client.
 */
START_TEST(test_repair_reserved_client_id_none)
{
	static char buf[CHUNK_SZ];
	struct cm_ctx *cm = cm_alloc(1);
	stateid4 stid = make_stateid(0xA5);

	cm_set_inode(cm, g_inode);
	set_repair_args(cm, &stid, buf, CHUNK_SZ, CHUNK_SZ, 0, NULL, 0);

	CHUNK_WRITE_REPAIR4args *args =
		&cm->compound->c_args->argarray.argarray_val[0]
			 .nfs_argop4_u.opchunk_write_repair;

	args->cwra_owner.co_guard.cg_client_id = CHUNK_GUARD_CLIENT_ID_NONE;

	nfs4_op_chunk_write_repair(cm->compound);

	CHUNK_WRITE_REPAIR4res *res =
		&cm->compound->c_res->resarray.resarray_val[0]
			 .nfs_resop4_u.opchunk_write_repair;

	ck_assert_int_eq(res->cwrr_status, NFS4ERR_INVAL);

	free_repair_args(cm);
	cm_free(cm);
}
END_TEST

START_TEST(test_repair_reserved_client_id_mds)
{
	static char buf[CHUNK_SZ];
	struct cm_ctx *cm = cm_alloc(1);
	stateid4 stid = make_stateid(0xA6);

	cm_set_inode(cm, g_inode);
	set_repair_args(cm, &stid, buf, CHUNK_SZ, CHUNK_SZ, 0, NULL, 0);

	CHUNK_WRITE_REPAIR4args *args =
		&cm->compound->c_args->argarray.argarray_val[0]
			 .nfs_argop4_u.opchunk_write_repair;

	args->cwra_owner.co_guard.cg_client_id = CHUNK_GUARD_CLIENT_ID_MDS;

	nfs4_op_chunk_write_repair(cm->compound);

	CHUNK_WRITE_REPAIR4res *res =
		&cm->compound->c_res->resarray.resarray_val[0]
			 .nfs_resop4_u.opchunk_write_repair;

	ck_assert_int_eq(res->cwrr_status, NFS4ERR_INVAL);

	free_repair_args(cm);
	cm_free(cm);
}
END_TEST

/*
 * Per-chunk CRC mismatch -> NFS4ERR_INVAL.  Compute the real CRC of
 * the buffer, flip a bit in it, supply the corrupted CRC on the wire.
 */
START_TEST(test_repair_crc_mismatch)
{
	static char buf[CHUNK_SZ];
	struct cm_ctx *cm = cm_alloc(1);
	stateid4 stid = make_stateid(0xA7);

	cm_set_inode(cm, g_inode);

	memset(buf, 0x5A, sizeof(buf));
	uint32_t real_crc = crc32(0, (const Bytef *)buf, sizeof(buf));
	uint32_t bad_crc = real_crc ^ 0x1;

	register_trust(&stid, g_inode->i_ino, 0xDEAD0001, LAYOUTIOMODE4_RW,
		       future_expire_ns());
	set_repair_args(cm, &stid, buf, CHUNK_SZ, CHUNK_SZ, 0, &bad_crc, 1);

	nfs4_op_chunk_write_repair(cm->compound);

	CHUNK_WRITE_REPAIR4res *res =
		&cm->compound->c_res->resarray.resarray_val[0]
			 .nfs_resop4_u.opchunk_write_repair;

	ck_assert_int_eq(res->cwrr_status, NFS4ERR_INVAL);

	free_repair_args(cm);
	trust_stateid_revoke(&stid);
	cm_free(cm);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Group B: Stateid auth                                               */
/* ------------------------------------------------------------------ */

/*
 * Repair MUST use a real layout stateid; anonymous / special stateids
 * are rejected with NFS4ERR_BAD_STATEID.  Mirrors draft-flexfiles-v2
 * line 8956.
 */
START_TEST(test_repair_special_stateid_rejected)
{
	static char buf[CHUNK_SZ];
	struct cm_ctx *cm = cm_alloc(1);
	stateid4 anon;

	memset(&anon, 0, sizeof(anon));

	cm_set_inode(cm, g_inode);
	set_repair_args(cm, &anon, buf, CHUNK_SZ, CHUNK_SZ, 0, NULL, 0);

	nfs4_op_chunk_write_repair(cm->compound);

	CHUNK_WRITE_REPAIR4res *res =
		&cm->compound->c_res->resarray.resarray_val[0]
			 .nfs_resop4_u.opchunk_write_repair;

	ck_assert_int_eq(res->cwrr_status, NFS4ERR_BAD_STATEID);

	free_repair_args(cm);
	cm_free(cm);
}
END_TEST

/* Stateid present but not in trust table -> NFS4ERR_BAD_STATEID. */
START_TEST(test_repair_unknown_stateid_rejected)
{
	static char buf[CHUNK_SZ];
	struct cm_ctx *cm = cm_alloc(1);
	stateid4 stid = make_stateid(0xB2);

	cm_set_inode(cm, g_inode);
	/* No register_trust call: stid is unknown to the table. */
	set_repair_args(cm, &stid, buf, CHUNK_SZ, CHUNK_SZ, 0, NULL, 0);

	nfs4_op_chunk_write_repair(cm->compound);

	CHUNK_WRITE_REPAIR4res *res =
		&cm->compound->c_res->resarray.resarray_val[0]
			 .nfs_resop4_u.opchunk_write_repair;

	ck_assert_int_eq(res->cwrr_status, NFS4ERR_BAD_STATEID);

	free_repair_args(cm);
	cm_free(cm);
}
END_TEST

/* Expired trust entry -> NFS4ERR_BAD_STATEID (entry returned but rejected). */
START_TEST(test_repair_expired_trust_entry)
{
	static char buf[CHUNK_SZ];
	struct cm_ctx *cm = cm_alloc(1);
	stateid4 stid = make_stateid(0xB3);

	cm_set_inode(cm, g_inode);
	register_trust(&stid, g_inode->i_ino, 0xDEAD0001, LAYOUTIOMODE4_RW,
		       past_expire_ns());
	set_repair_args(cm, &stid, buf, CHUNK_SZ, CHUNK_SZ, 0, NULL, 0);

	nfs4_op_chunk_write_repair(cm->compound);

	CHUNK_WRITE_REPAIR4res *res =
		&cm->compound->c_res->resarray.resarray_val[0]
			 .nfs_resop4_u.opchunk_write_repair;

	ck_assert_int_eq(res->cwrr_status, NFS4ERR_BAD_STATEID);

	free_repair_args(cm);
	trust_stateid_revoke(&stid);
	cm_free(cm);
}
END_TEST

/*
 * Trust entry exists but iomode is READ-only -> NFS4ERR_ACCESS.  Repair
 * is a write, so it requires LAYOUTIOMODE4_RW.
 */
START_TEST(test_repair_read_iomode_rejected)
{
	static char buf[CHUNK_SZ];
	struct cm_ctx *cm = cm_alloc(1);
	stateid4 stid = make_stateid(0xB4);

	cm_set_inode(cm, g_inode);
	register_trust(&stid, g_inode->i_ino, 0xDEAD0001, LAYOUTIOMODE4_READ,
		       future_expire_ns());
	set_repair_args(cm, &stid, buf, CHUNK_SZ, CHUNK_SZ, 0, NULL, 0);

	nfs4_op_chunk_write_repair(cm->compound);

	CHUNK_WRITE_REPAIR4res *res =
		&cm->compound->c_res->resarray.resarray_val[0]
			 .nfs_resop4_u.opchunk_write_repair;

	ck_assert_int_eq(res->cwrr_status, NFS4ERR_ACCESS);

	free_repair_args(cm);
	trust_stateid_revoke(&stid);
	cm_free(cm);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Group C: Happy path                                                 */
/* ------------------------------------------------------------------ */

/*
 * Single-block repair from EMPTY: block transitions EMPTY -> PENDING,
 * CHUNK_BLOCK_REPAIR_PROVENANCE flag is set, cs_repair_initiated bumps,
 * per-chunk status is NFS4_OK.
 */
START_TEST(test_repair_single_block_empty)
{
	static char buf[CHUNK_SZ];
	struct cm_ctx *cm = cm_alloc(1);
	stateid4 stid = make_stateid(0xC1);

	memset(buf, 0xA5, sizeof(buf));
	uint32_t crc = crc32(0, (const Bytef *)buf, sizeof(buf));

	cm_set_inode(cm, g_inode);
	register_trust(&stid, g_inode->i_ino, 0xDEAD0001, LAYOUTIOMODE4_RW,
		       future_expire_ns());

	uint64_t initiated_before =
		atomic_load_explicit(&g_sb->sb_chunk_stats.cs_repair_initiated,
				     memory_order_relaxed);

	set_repair_args(cm, &stid, buf, CHUNK_SZ, CHUNK_SZ, 0, &crc, 1);

	nfs4_op_chunk_write_repair(cm->compound);

	CHUNK_WRITE_REPAIR4res *res =
		&cm->compound->c_res->resarray.resarray_val[0]
			 .nfs_resop4_u.opchunk_write_repair;

	ck_assert_int_eq(res->cwrr_status, NFS4_OK);

	CHUNK_WRITE_REPAIR4resok *resok =
		&res->CHUNK_WRITE_REPAIR4res_u.cwrr_resok4;

	ck_assert_uint_eq(resok->cwrr_status.cwrr_status_len, 1);
	ck_assert_int_eq(resok->cwrr_status.cwrr_status_val[0], NFS4_OK);

	/* Block at offset 0 should be PENDING with REPAIR_PROVENANCE set. */
	struct chunk_store *cs = g_inode->i_chunk_store;

	ck_assert_ptr_nonnull(cs);

	struct chunk_block *cb = chunk_store_lookup(cs, 0);

	ck_assert_ptr_nonnull(cb);
	ck_assert_int_eq(cb->cb_state, CHUNK_STATE_PENDING);
	ck_assert_uint_eq(cb->cb_flags & CHUNK_BLOCK_REPAIR_PROVENANCE,
			  CHUNK_BLOCK_REPAIR_PROVENANCE);

	uint64_t initiated_after =
		atomic_load_explicit(&g_sb->sb_chunk_stats.cs_repair_initiated,
				     memory_order_relaxed);

	ck_assert_uint_eq(initiated_after, initiated_before + 1);

	free_repair_res(cm);
	free_repair_args(cm);
	trust_stateid_revoke(&stid);
	cm_free(cm);
}
END_TEST

/*
 * Multi-block repair: 4 blocks written; each gets REPAIR_PROVENANCE;
 * all per-chunk statuses are NFS4_OK; counter bumps once per call (not
 * once per block) -- the counter measures repair OPERATIONS, not
 * block-writes.
 */
START_TEST(test_repair_multi_block)
{
	const uint32_t nblocks = 4;
	static char buf[CHUNK_SZ * 4];
	struct cm_ctx *cm = cm_alloc(1);
	stateid4 stid = make_stateid(0xC2);

	memset(buf, 0x33, sizeof(buf));
	uint32_t crcs[4];

	for (uint32_t i = 0; i < nblocks; i++) {
		crcs[i] = crc32(0, (const Bytef *)&buf[i * CHUNK_SZ], CHUNK_SZ);
	}

	cm_set_inode(cm, g_inode);
	register_trust(&stid, g_inode->i_ino, 0xDEAD0001, LAYOUTIOMODE4_RW,
		       future_expire_ns());

	uint64_t initiated_before =
		atomic_load_explicit(&g_sb->sb_chunk_stats.cs_repair_initiated,
				     memory_order_relaxed);

	set_repair_args(cm, &stid, buf, sizeof(buf), CHUNK_SZ, 0, crcs,
			nblocks);

	nfs4_op_chunk_write_repair(cm->compound);

	CHUNK_WRITE_REPAIR4res *res =
		&cm->compound->c_res->resarray.resarray_val[0]
			 .nfs_resop4_u.opchunk_write_repair;

	ck_assert_int_eq(res->cwrr_status, NFS4_OK);

	CHUNK_WRITE_REPAIR4resok *resok =
		&res->CHUNK_WRITE_REPAIR4res_u.cwrr_resok4;

	ck_assert_uint_eq(resok->cwrr_status.cwrr_status_len, nblocks);
	for (uint32_t i = 0; i < nblocks; i++) {
		ck_assert_int_eq(resok->cwrr_status.cwrr_status_val[i],
				 NFS4_OK);

		/*
		 * chunk_store_lookup keys by block index, not byte offset;
		 * the handler writes blocks at args->cwra_offset + i and
		 * the test passes cwra_offset = 0.
		 */
		struct chunk_block *cb =
			chunk_store_lookup(g_inode->i_chunk_store, i);

		ck_assert_ptr_nonnull(cb);
		ck_assert_uint_eq(cb->cb_flags & CHUNK_BLOCK_REPAIR_PROVENANCE,
				  CHUNK_BLOCK_REPAIR_PROVENANCE);
	}

	uint64_t initiated_after =
		atomic_load_explicit(&g_sb->sb_chunk_stats.cs_repair_initiated,
				     memory_order_relaxed);

	ck_assert_uint_eq(initiated_after, initiated_before + 1);

	free_repair_res(cm);
	free_repair_args(cm);
	trust_stateid_revoke(&stid);
	cm_free(cm);
}
END_TEST

/*
 * The Track 1b PENDING-collision gate (chunk.c:351-382) rejects a
 * normal CHUNK_WRITE that lands on a PENDING block owned by a
 * different writer.  CHUNK_WRITE_REPAIR explicitly bypasses that gate
 * -- the repair client is the sole authorised writer for the slot
 * (gated at the layout layer via FFV2_DS_FLAGS_REPAIR + iomode=RW
 * trust-table entry).
 *
 * Pre-populate a PENDING block from a different owner; verify
 * CHUNK_WRITE_REPAIR succeeds (does not return NFS4ERR_DELAY).
 */
START_TEST(test_repair_bypasses_pending_collision_gate)
{
	static char buf[CHUNK_SZ];
	struct cm_ctx *cm_pre = cm_alloc(1);
	struct cm_ctx *cm = cm_alloc(1);
	stateid4 anon;
	stateid4 stid_repair = make_stateid(0xC3);

	memset(&anon, 0, sizeof(anon));
	memset(buf, 0xCC, sizeof(buf));
	uint32_t crc = crc32(0, (const Bytef *)buf, sizeof(buf));

	/* First writer (anonymous stateid, different owner) lands PENDING. */
	cm_set_inode(cm_pre, g_inode);
	cm_set_op(cm_pre, 0, OP_CHUNK_WRITE);

	CHUNK_WRITE4args *wargs =
		&cm_pre->compound->c_args->argarray.argarray_val[0]
			 .nfs_argop4_u.opchunk_write;

	wargs->cwa_stateid = anon;
	wargs->cwa_offset = 0;
	wargs->cwa_stable = UNSTABLE4;
	wargs->cwa_chunk_size = CHUNK_SZ;
	wargs->cwa_chunks.cwa_chunks_val = buf;
	wargs->cwa_chunks.cwa_chunks_len = CHUNK_SZ;
	wargs->cwa_owner.co_guard.cg_client_id = 0x0BAD0001;
	wargs->cwa_owner.co_guard.cg_gen_id = 1;
	wargs->cwa_checksums.cwa_checksums_val = calloc(1, sizeof(checksum4));
	wargs->cwa_checksums.cwa_checksums_len = 1;
	(void)chunk_checksum_pack_crc32(
		&wargs->cwa_checksums.cwa_checksums_val[0], crc);

	nfs4_op_chunk_write(cm_pre->compound);

	struct chunk_block *cb_pre =
		chunk_store_lookup(g_inode->i_chunk_store, 0);

	ck_assert_ptr_nonnull(cb_pre);
	ck_assert_int_eq(cb_pre->cb_state, CHUNK_STATE_PENDING);

	/* Now the repair-write from a different owner must succeed. */
	cm_set_inode(cm, g_inode);
	register_trust(&stid_repair, g_inode->i_ino, 0xDEAD0001,
		       LAYOUTIOMODE4_RW, future_expire_ns());
	set_repair_args(cm, &stid_repair, buf, CHUNK_SZ, CHUNK_SZ, 0, &crc, 1);

	nfs4_op_chunk_write_repair(cm->compound);

	CHUNK_WRITE_REPAIR4res *res =
		&cm->compound->c_res->resarray.resarray_val[0]
			 .nfs_resop4_u.opchunk_write_repair;

	ck_assert_int_eq(res->cwrr_status, NFS4_OK);

	CHUNK_WRITE_REPAIR4resok *resok =
		&res->CHUNK_WRITE_REPAIR4res_u.cwrr_resok4;

	ck_assert_uint_eq(resok->cwrr_status.cwrr_status_len, 1);
	ck_assert_int_eq(resok->cwrr_status.cwrr_status_val[0], NFS4_OK);

	free_repair_res(cm);
	free_repair_args(cm);
	trust_stateid_revoke(&stid_repair);

	/* Free the pre-populator's checksum array. */
	free(wargs->cwa_checksums.cwa_checksums_val[0].cs_value.cs_value_val);
	free(wargs->cwa_checksums.cwa_checksums_val);
	cm_free(cm);
	cm_free(cm_pre);
}
END_TEST

/*
 * NOT_NOW_BROWN_COW: Group D (persistence round-trip via
 * chunk_store_persist + chunk_store_load) lands in a follow-up
 * once the chunk_store lifecycle helpers are clarified.  The in-
 * memory flag-set check in test_repair_single_block_empty +
 * test_repair_multi_block exercises the same cb_flags path that
 * the persist code copies; full reload coverage is additive, not
 * load-bearing for the Slice 1 contract.
 */

/* ------------------------------------------------------------------ */
/* Group F: OP_CHUNK_REPAIRED (MDS-side) -- ec-repair slice 2          */
/* ------------------------------------------------------------------ */

/*
 * Build CHUNK_REPAIRED args in slot 0 with the given stateid and
 * range.  The owner uses a real (non-reserved) client id.
 */
static void set_repaired_args(struct cm_ctx *cm, const stateid4 *stid,
			      uint64_t offset, uint32_t count)
{
	cm_set_op(cm, 0, OP_CHUNK_REPAIRED);
	CHUNK_REPAIRED4args *args =
		&cm->compound->c_args->argarray.argarray_val[0]
			 .nfs_argop4_u.opchunk_repair;

	args->cpa_stateid = *stid;
	args->cpa_offset = offset;
	args->cpa_count = count;
	args->cpa_owner.co_guard.cg_client_id = 0xDEAD0001;
	args->cpa_owner.co_guard.cg_gen_id = 1;
}

/*
 * Attach an i_layout_segments to g_inode with `nfiles` mirrors.
 * `flagmask` is OR'd onto each ldf_flags so the caller can preset
 * FFV2_DS_FLAGS_REPAIR on every mirror; per-mirror customisation
 * happens after this returns.
 */
static void attach_layout_segments(uint32_t nfiles, uint32_t flagmask)
{
	struct layout_segments *lss = layout_segments_alloc();
	struct layout_data_file *files =
		calloc(nfiles, sizeof(struct layout_data_file));

	ck_assert_ptr_nonnull(files);

	for (uint32_t i = 0; i < nfiles; i++) {
		files[i].ldf_dstore_id = 100 + i;
		files[i].ldf_fh_len = 8;
		memset(files[i].ldf_fh, (uint8_t)(0xA0 + i), 8);
		files[i].ldf_uid = 1000;
		files[i].ldf_gid = 1000;
		files[i].ldf_mode = 0644;
		files[i].ldf_flags = flagmask;
	}

	struct layout_segment seg = {
		.ls_offset = 0,
		.ls_length = 0,
		.ls_stripe_unit = 65536,
		.ls_k = nfiles > 1 ? nfiles - 1 : 1,
		.ls_m = nfiles > 1 ? 1 : 0,
		.ls_nfiles = nfiles,
		.ls_layout_type = LAYOUT4_FLEX_FILES_V2,
		.ls_files = files,
	};

	layout_segments_add(lss, &seg);
	g_inode->i_layout_segments = lss;
}

static void detach_layout_segments(void)
{
	if (g_inode->i_layout_segments) {
		layout_segments_free(g_inode->i_layout_segments);
		g_inode->i_layout_segments = NULL;
	}
}

/* No current filehandle -> NFS4ERR_NOFILEHANDLE. */
START_TEST(test_repaired_no_fh)
{
	struct cm_ctx *cm = cm_alloc(1);
	stateid4 stid = make_stateid(0xF1);

	set_repaired_args(cm, &stid, 0, 0);
	nfs4_op_chunk_repaired(cm->compound);

	CHUNK_REPAIRED4res *res = &cm->compound->c_res->resarray.resarray_val[0]
					   .nfs_resop4_u.opchunk_repair;

	ck_assert_int_eq(res->cpr_status, NFS4ERR_NOFILEHANDLE);

	cm_free(cm);
}
END_TEST

/* Directory FH -> NFS4ERR_INVAL. */
START_TEST(test_repaired_not_regular_file)
{
	struct cm_ctx *cm = cm_alloc(1);
	stateid4 stid = make_stateid(0xF2);

	uint64_t ino =
		__atomic_add_fetch(&g_sb->sb_next_ino, 1, __ATOMIC_RELAXED);
	struct inode *dir = inode_alloc(g_sb, ino);

	ck_assert_ptr_nonnull(dir);
	dir->i_mode = S_IFDIR | 0750;

	cm_set_inode(cm, dir);
	set_repaired_args(cm, &stid, 0, 0);
	nfs4_op_chunk_repaired(cm->compound);

	CHUNK_REPAIRED4res *res = &cm->compound->c_res->resarray.resarray_val[0]
					   .nfs_resop4_u.opchunk_repair;

	ck_assert_int_eq(res->cpr_status, NFS4ERR_INVAL);

	cm_free(cm);
	inode_active_put(dir);
}
END_TEST

START_TEST(test_repaired_reserved_client_id_none)
{
	struct cm_ctx *cm = cm_alloc(1);
	stateid4 stid = make_stateid(0xF3);

	cm_set_inode(cm, g_inode);
	set_repaired_args(cm, &stid, 0, 0);

	CHUNK_REPAIRED4args *args =
		&cm->compound->c_args->argarray.argarray_val[0]
			 .nfs_argop4_u.opchunk_repair;

	args->cpa_owner.co_guard.cg_client_id = CHUNK_GUARD_CLIENT_ID_NONE;

	nfs4_op_chunk_repaired(cm->compound);

	CHUNK_REPAIRED4res *res = &cm->compound->c_res->resarray.resarray_val[0]
					   .nfs_resop4_u.opchunk_repair;

	ck_assert_int_eq(res->cpr_status, NFS4ERR_INVAL);

	cm_free(cm);
}
END_TEST

START_TEST(test_repaired_reserved_client_id_mds)
{
	struct cm_ctx *cm = cm_alloc(1);
	stateid4 stid = make_stateid(0xF4);

	cm_set_inode(cm, g_inode);
	set_repaired_args(cm, &stid, 0, 0);

	CHUNK_REPAIRED4args *args =
		&cm->compound->c_args->argarray.argarray_val[0]
			 .nfs_argop4_u.opchunk_repair;

	args->cpa_owner.co_guard.cg_client_id = CHUNK_GUARD_CLIENT_ID_MDS;

	nfs4_op_chunk_repaired(cm->compound);

	CHUNK_REPAIRED4res *res = &cm->compound->c_res->resarray.resarray_val[0]
					   .nfs_resop4_u.opchunk_repair;

	ck_assert_int_eq(res->cpr_status, NFS4ERR_INVAL);

	cm_free(cm);
}
END_TEST

/*
 * Inode with no i_layout_segments -> NFS4ERR_INVAL.  The repair was
 * never issued for this file.
 */
START_TEST(test_repaired_no_layout_segments)
{
	struct cm_ctx *cm = cm_alloc(1);
	stateid4 stid = make_stateid(0xF5);

	cm_set_inode(cm, g_inode);
	ck_assert_ptr_null(g_inode->i_layout_segments);
	set_repaired_args(cm, &stid, 0, 0);
	nfs4_op_chunk_repaired(cm->compound);

	CHUNK_REPAIRED4res *res = &cm->compound->c_res->resarray.resarray_val[0]
					   .nfs_resop4_u.opchunk_repair;

	ck_assert_int_eq(res->cpr_status, NFS4ERR_INVAL);

	cm_free(cm);
}
END_TEST

/*
 * No mirror has FFV2_DS_FLAGS_REPAIR set -> NFS4_OK idempotent.
 * Covers the crash-recovery case where the client retries an op the
 * MDS already executed and persisted.  Counter does NOT bump on the
 * idempotent path -- it counts mirrors actually cleared, not calls.
 */
START_TEST(test_repaired_no_mirror_in_repair)
{
	struct cm_ctx *cm = cm_alloc(1);
	stateid4 stid = make_stateid(0xF6);

	cm_set_inode(cm, g_inode);
	attach_layout_segments(2, 0);

	uint64_t completed_before =
		atomic_load_explicit(&g_sb->sb_chunk_stats.cs_repair_completed,
				     memory_order_relaxed);

	set_repaired_args(cm, &stid, 0, 0);
	nfs4_op_chunk_repaired(cm->compound);

	CHUNK_REPAIRED4res *res = &cm->compound->c_res->resarray.resarray_val[0]
					   .nfs_resop4_u.opchunk_repair;

	ck_assert_int_eq(res->cpr_status, NFS4_OK);

	uint64_t completed_after =
		atomic_load_explicit(&g_sb->sb_chunk_stats.cs_repair_completed,
				     memory_order_relaxed);

	ck_assert_uint_eq(completed_after, completed_before);

	detach_layout_segments();
	cm_free(cm);
}
END_TEST

START_TEST(test_repaired_clears_single_mirror)
{
	struct cm_ctx *cm = cm_alloc(1);
	stateid4 stid = make_stateid(0xF7);

	cm_set_inode(cm, g_inode);
	attach_layout_segments(2, 0);
	g_inode->i_layout_segments->lss_segs[0].ls_files[1].ldf_flags =
		FFV2_DS_FLAGS_REPAIR;

	uint64_t completed_before =
		atomic_load_explicit(&g_sb->sb_chunk_stats.cs_repair_completed,
				     memory_order_relaxed);

	set_repaired_args(cm, &stid, 0, 0);
	nfs4_op_chunk_repaired(cm->compound);

	CHUNK_REPAIRED4res *res = &cm->compound->c_res->resarray.resarray_val[0]
					   .nfs_resop4_u.opchunk_repair;

	ck_assert_int_eq(res->cpr_status, NFS4_OK);
	ck_assert_uint_eq(
		g_inode->i_layout_segments->lss_segs[0].ls_files[1].ldf_flags &
			FFV2_DS_FLAGS_REPAIR,
		0);

	uint64_t completed_after =
		atomic_load_explicit(&g_sb->sb_chunk_stats.cs_repair_completed,
				     memory_order_relaxed);

	ck_assert_uint_eq(completed_after, completed_before + 1);

	detach_layout_segments();
	cm_free(cm);
}
END_TEST

START_TEST(test_repaired_clears_multiple_mirrors)
{
	struct cm_ctx *cm = cm_alloc(1);
	stateid4 stid = make_stateid(0xF8);

	cm_set_inode(cm, g_inode);
	attach_layout_segments(3, FFV2_DS_FLAGS_REPAIR);

	uint64_t completed_before =
		atomic_load_explicit(&g_sb->sb_chunk_stats.cs_repair_completed,
				     memory_order_relaxed);

	set_repaired_args(cm, &stid, 0, 0);
	nfs4_op_chunk_repaired(cm->compound);

	CHUNK_REPAIRED4res *res = &cm->compound->c_res->resarray.resarray_val[0]
					   .nfs_resop4_u.opchunk_repair;

	ck_assert_int_eq(res->cpr_status, NFS4_OK);

	for (uint32_t i = 0; i < 3; i++)
		ck_assert_uint_eq(g_inode->i_layout_segments->lss_segs[0]
						  .ls_files[i]
						  .ldf_flags &
					  FFV2_DS_FLAGS_REPAIR,
				  0);

	uint64_t completed_after =
		atomic_load_explicit(&g_sb->sb_chunk_stats.cs_repair_completed,
				     memory_order_relaxed);

	ck_assert_uint_eq(completed_after, completed_before + 3);

	detach_layout_segments();
	cm_free(cm);
}
END_TEST

START_TEST(test_repaired_idempotent_second_call)
{
	struct cm_ctx *cm = cm_alloc(1);
	stateid4 stid = make_stateid(0xF9);

	cm_set_inode(cm, g_inode);
	attach_layout_segments(2, FFV2_DS_FLAGS_REPAIR);

	uint64_t completed_start =
		atomic_load_explicit(&g_sb->sb_chunk_stats.cs_repair_completed,
				     memory_order_relaxed);

	set_repaired_args(cm, &stid, 0, 0);
	nfs4_op_chunk_repaired(cm->compound);

	CHUNK_REPAIRED4res *res = &cm->compound->c_res->resarray.resarray_val[0]
					   .nfs_resop4_u.opchunk_repair;

	ck_assert_int_eq(res->cpr_status, NFS4_OK);

	uint64_t completed_after_first =
		atomic_load_explicit(&g_sb->sb_chunk_stats.cs_repair_completed,
				     memory_order_relaxed);

	ck_assert_uint_eq(completed_after_first, completed_start + 2);

	/* Reset slot and issue the same op a second time. */
	memset(&cm->compound->c_args->argarray.argarray_val[0], 0,
	       sizeof(nfs_argop4));
	memset(&cm->compound->c_res->resarray.resarray_val[0], 0,
	       sizeof(nfs_resop4));
	set_repaired_args(cm, &stid, 0, 0);
	nfs4_op_chunk_repaired(cm->compound);

	res = &cm->compound->c_res->resarray.resarray_val[0]
		       .nfs_resop4_u.opchunk_repair;
	ck_assert_int_eq(res->cpr_status, NFS4_OK);

	uint64_t completed_after_second =
		atomic_load_explicit(&g_sb->sb_chunk_stats.cs_repair_completed,
				     memory_order_relaxed);

	ck_assert_uint_eq(completed_after_second, completed_after_first);

	detach_layout_segments();
	cm_free(cm);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite                                                               */
/* ------------------------------------------------------------------ */

static Suite *chunk_repair_suite(void)
{
	Suite *s = suite_create("chunk_repair");

	TCase *tc_a = tcase_create("validation");
	tcase_add_checked_fixture(tc_a, repair_setup, repair_teardown);
	tcase_add_test(tc_a, test_repair_no_fh);
	tcase_add_test(tc_a, test_repair_not_regular_file);
	tcase_add_test(tc_a, test_repair_zero_chunk_size);
	tcase_add_test(tc_a, test_repair_zero_chunks_len);
	tcase_add_test(tc_a, test_repair_reserved_client_id_none);
	tcase_add_test(tc_a, test_repair_reserved_client_id_mds);
	tcase_add_test(tc_a, test_repair_crc_mismatch);
	suite_add_tcase(s, tc_a);

	TCase *tc_b = tcase_create("stateid_auth");
	tcase_add_checked_fixture(tc_b, repair_setup, repair_teardown);
	tcase_add_test(tc_b, test_repair_special_stateid_rejected);
	tcase_add_test(tc_b, test_repair_unknown_stateid_rejected);
	tcase_add_test(tc_b, test_repair_expired_trust_entry);
	tcase_add_test(tc_b, test_repair_read_iomode_rejected);
	suite_add_tcase(s, tc_b);

	TCase *tc_c = tcase_create("happy_path");
	tcase_add_checked_fixture(tc_c, repair_setup, repair_teardown);
	tcase_add_test(tc_c, test_repair_single_block_empty);
	tcase_add_test(tc_c, test_repair_multi_block);
	tcase_add_test(tc_c, test_repair_bypasses_pending_collision_gate);
	suite_add_tcase(s, tc_c);

	TCase *tc_f = tcase_create("chunk_repaired_mds");
	tcase_add_checked_fixture(tc_f, repair_setup, repair_teardown);
	tcase_add_test(tc_f, test_repaired_no_fh);
	tcase_add_test(tc_f, test_repaired_not_regular_file);
	tcase_add_test(tc_f, test_repaired_reserved_client_id_none);
	tcase_add_test(tc_f, test_repaired_reserved_client_id_mds);
	tcase_add_test(tc_f, test_repaired_no_layout_segments);
	tcase_add_test(tc_f, test_repaired_no_mirror_in_repair);
	tcase_add_test(tc_f, test_repaired_clears_single_mirror);
	tcase_add_test(tc_f, test_repaired_clears_multiple_mirrors);
	tcase_add_test(tc_f, test_repaired_idempotent_second_call);
	suite_add_tcase(s, tc_f);

	return s;
}

int main(void)
{
	return reffs_test_run_suite(chunk_repair_suite(), NULL, NULL);
}
