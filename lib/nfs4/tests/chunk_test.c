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
 * trust_stateid_test.c.  Successful operations here use a registered
 * per-test stateid so the chunk-store tests exercise the same authorization
 * boundary as the wire path.
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
#include "reffs/time.h"
#include "nfs4/attr.h"
#include "nfs4/chunk_checksum.h"
#include "nfs4/chunk_epoch.h"
#include "nfs4/chunk_store.h"
#include "nfs4/client.h"
#include "nfs4/compound.h"
#include "nfs4/ops.h"
#include "nfs4/trust_stateid.h"

#include "nfs4_test_harness.h"

/* ------------------------------------------------------------------ */
/* Compound mock helpers (adapted from trust_stateid_test.c)           */
/* ------------------------------------------------------------------ */

struct cm_ctx {
	struct rpc_trans rt;
	struct task task;
	struct compound *compound;
	struct nfs4_client *nc;
	stateid4 chunk_stateid;
	bool chunk_stateid_registered;
};

static uint8_t next_chunk_stateid = 1;

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

	/* CHUNK tests use a real registered layout stateid, not a probe id. */
	memset(&cm->chunk_stateid, next_chunk_stateid++,
	       sizeof(cm->chunk_stateid));
	cm->chunk_stateid.seqid = 1;
	ck_assert_int_eq(trust_stateid_register_fh(
				 &cm->chunk_stateid, inode->i_sb->sb_id,
				 inode->i_ino, cm->nc->nc_client.c_id,
				 cm->nc->nc_client.c_id,
				 CHUNK_GUARD_CLIENT_ID_NONE, LAYOUTIOMODE4_RW,
				 UINT64_MAX, ""),
			 0);
	cm->chunk_stateid_registered = true;
}

static void cm_set_op(struct cm_ctx *cm, unsigned int idx, nfs_opnum4 opnum)
{
	nfs_argop4 *arg;

	ck_assert_uint_lt(idx, cm->compound->c_args->argarray.argarray_len);
	arg = &cm->compound->c_args->argarray.argarray_val[idx];
	arg->argop = opnum;
	cm->compound->c_res->resarray.resarray_val[idx].resop = opnum;
	cm->compound->c_curr_op = (int)idx;

	if (!cm->chunk_stateid_registered)
		return;

	switch (opnum) {
	case OP_CHUNK_FINALIZE:
		arg->nfs_argop4_u.opchunk_finalize.cfa_stateid =
			cm->chunk_stateid;
		break;
	case OP_CHUNK_COMMIT:
		arg->nfs_argop4_u.opchunk_commit.cca_stateid =
			cm->chunk_stateid;
		break;
	case OP_CHUNK_ROLLBACK:
		arg->nfs_argop4_u.opchunk_rollback.crb_stateid =
			cm->chunk_stateid;
		break;
	default:
		break;
	}
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
 * Build CHUNK_WRITE args in slot 0.  Uses the per-test registered stateid
 * when a current filehandle is present.  Caller provides the data buffer and a
 * uint32_t CRC array (pass NULL/0 for no checksum validation).  The
 * function wraps each CRC into a checksum4 (CHECKSUM_ALG_CRC32, 4 bytes
 * big-endian) -- the cs_value buffers and the checksum4 array are heap-
 * allocated here and released by free_write_args.
 */
static void set_write_args(struct cm_ctx *cm, char *buf, uint32_t buf_len,
			   uint32_t chunk_size, uint64_t offset, uint32_t *crcs,
			   uint32_t ncrc)
{
	static uint32_t co_ids[64];
	cm_set_op(cm, 0, OP_CHUNK_WRITE);
	CHUNK_WRITE4args *args = &cm->compound->c_args->argarray.argarray_val[0]
					  .nfs_argop4_u.opchunk_write;

	if (cm->chunk_stateid_registered)
		args->cwa_stateid = cm->chunk_stateid;
	else
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
	args->cwa_cohort_id = 0;
	args->cwa_client_id = 0xBEEF;
	uint32_t nchunks =
		chunk_size ? (buf_len + chunk_size - 1) / chunk_size : 0;
	ck_assert_uint_le(nchunks, sizeof(co_ids) / sizeof(co_ids[0]));
	args->cwa_co_ids.cwa_co_ids_len = nchunks;
	args->cwa_co_ids.cwa_co_ids_val = co_ids;
	for (uint32_t i = 0; i < nchunks; i++)
		args->cwa_co_ids.cwa_co_ids_val[i] = 99 + (uint32_t)offset + i;
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
	args->cwa_co_ids.cwa_co_ids_len = 0;
}

/* Free the three per-chunk arrays from a successful CHUNK_WRITE result:
 * cwr_block_status, cwr_block_activated, cwr_owners.  Each is calloc'd
 * once per op in nfs4_op_chunk_write; without this teardown the LSAN
 * pass on every happy-path test reports direct leaks. */
static void free_write_res(struct cm_ctx *cm)
{
	CHUNK_WRITE4resok *ok = &cm->compound->c_res->resarray.resarray_val[0]
					 .nfs_resop4_u.opchunk_write
					 .CHUNK_WRITE4res_u.cwr_resok4;

	free(ok->cwr_block_status.cwr_block_status_val);
	ok->cwr_block_status.cwr_block_status_val = NULL;
	ok->cwr_block_status.cwr_block_status_len = 0;

	free(ok->cwr_block_activated.cwr_block_activated_val);
	ok->cwr_block_activated.cwr_block_activated_val = NULL;
	ok->cwr_block_activated.cwr_block_activated_len = 0;

	free(ok->cwr_owners.cwr_owners_val);
	ok->cwr_owners.cwr_owners_val = NULL;
	ok->cwr_owners.cwr_owners_len = 0;
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
		read_chunk4 *rc = &resok->crr_chunks.crr_chunks_val[i];

		/*
		 * cr_checksum.cs_value_val is allocated by
		 * chunk_checksum_pack() in nfs4_op_chunk_read; production
		 * releases it via the XDR-free path, tests must release it
		 * here.
		 */
		free(rc->cr_checksum.cs_value.cs_value_val);
		rc->cr_checksum.cs_value.cs_value_val = NULL;
		free(rc->cr_chunk.cr_chunk_val);
		rc->cr_chunk.cr_chunk_val = NULL;
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

static void set_chunk_lock_args(struct cm_ctx *cm, uint64_t offset,
				uint32_t count, uint64_t cohort,
				uint32_t client, uint32_t owner, uint32_t flags,
				bool adopt)
{
	cm_set_op(cm, 0, OP_CHUNK_LOCK);
	CHUNK_LOCK4args *args = &cm->compound->c_args->argarray.argarray_val[0]
					 .nfs_argop4_u.opchunk_lock;

	args->cla_stateid = cm->chunk_stateid;
	args->cla_offset = offset;
	args->cla_count = count;
	args->cla_flags = flags;
	args->cla_owner = (chunk_owner4){
		.co_cohort_id = cohort,
		.co_client_id = client,
		.co_id = owner,
	};
	args->cla_adopt.cla_adopt = adopt;
}

static void set_chunk_unlock_args(struct cm_ctx *cm, uint64_t offset,
				  uint32_t count, uint64_t cohort,
				  uint32_t client, uint32_t owner)
{
	cm_set_op(cm, 0, OP_CHUNK_UNLOCK);
	CHUNK_UNLOCK4args *args =
		&cm->compound->c_args->argarray.argarray_val[0]
			 .nfs_argop4_u.opchunk_unlock;

	args->cua_stateid = cm->chunk_stateid;
	args->cua_offset = offset;
	args->cua_count = count;
	args->cua_owner = (chunk_owner4){
		.co_cohort_id = cohort,
		.co_client_id = client,
		.co_id = owner,
	};
}

static void set_chunk_escrow_install_args(struct cm_ctx *cm, uint64_t epoch,
					  uint64_t offset, uint32_t count,
					  const escrow_id4 id)
{
	cm_set_op(cm, 0, OP_CHUNK_ESCROW_INSTALL);
	CHUNK_ESCROW_INSTALL4args *args =
		&cm->compound->c_args->argarray.argarray_val[0]
			 .nfs_argop4_u.opchunk_escrow_install;

	args->ceia_mds_epoch = epoch;
	args->ceia_offset = offset;
	args->ceia_count = count;
	memcpy(args->ceia_escrow_id, id, sizeof(args->ceia_escrow_id));
}

static void set_chunk_escrow_release_args(struct cm_ctx *cm, uint64_t epoch,
					  uint64_t offset, uint32_t count,
					  const escrow_id4 id)
{
	cm_set_op(cm, 0, OP_CHUNK_ESCROW_RELEASE);
	CHUNK_ESCROW_RELEASE4args *args =
		&cm->compound->c_args->argarray.argarray_val[0]
			 .nfs_argop4_u.opchunk_escrow_release;

	args->cera_mds_epoch = epoch;
	args->cera_offset = offset;
	args->cera_count = count;
	memcpy(args->cera_escrow_id, id, sizeof(args->cera_escrow_id));
}

static void set_chunk_escrow_enumerate_args(struct cm_ctx *cm, uint64_t epoch,
					    uint64_t offset, uint32_t count,
					    uint32_t maxcount)
{
	cm_set_op(cm, 0, OP_CHUNK_ESCROW_ENUMERATE);
	CHUNK_ESCROW_ENUMERATE4args *args =
		&cm->compound->c_args->argarray.argarray_val[0]
			 .nfs_argop4_u.opchunk_escrow_enumerate;

	args->ceea_mds_epoch = epoch;
	args->ceea_offset = offset;
	args->ceea_count = count;
	args->ceea_maxcount = maxcount;
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

START_TEST(test_chunk_write_special_stateid_rejected)
{
	static char buf[CHUNK_SZ];
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);
	set_write_args(cm, buf, CHUNK_SZ, CHUNK_SZ, 0, NULL, 0);
	memset(&cm->compound->c_args->argarray.argarray_val[0]
			.nfs_argop4_u.opchunk_write.cwa_stateid,
	       0, sizeof(stateid4));

	nfs4_op_chunk_write(cm->compound);
	ck_assert_int_eq(cm->compound->c_res->resarray.resarray_val[0]
				 .nfs_resop4_u.opchunk_write.cwr_status,
			 NFS4ERR_BAD_STATEID);
	cm_free(cm);
}
END_TEST

START_TEST(test_chunk_write_stateid_file_mismatch_rejected)
{
	static char buf[CHUNK_SZ];
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);
	set_write_args(cm, buf, CHUNK_SZ, CHUNK_SZ, 0, NULL, 0);
	cm->compound->c_curr_nfh.nfh_ino++;

	nfs4_op_chunk_write(cm->compound);
	ck_assert_int_eq(cm->compound->c_res->resarray.resarray_val[0]
				 .nfs_resop4_u.opchunk_write.cwr_status,
			 NFS4ERR_BAD_STATEID);
	cm_free(cm);
}
END_TEST

START_TEST(test_chunk_write_stateid_principal_mismatch_rejected)
{
	static char buf[CHUNK_SZ];
	static const char registered_principal[] = "client@example.com";
	stateid4 stid;
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);
	memset(&stid, 0xA5, sizeof(stid));
	stid.seqid = 1;
	ck_assert_int_eq(trust_stateid_register_fh(
				 &stid, g_inode->i_sb->sb_id, g_inode->i_ino,
				 cm->nc->nc_client.c_id, cm->nc->nc_client.c_id,
				 CHUNK_GUARD_CLIENT_ID_NONE, LAYOUTIOMODE4_RW,
				 UINT64_MAX, registered_principal),
			 0);
	cm->compound->c_gss_principal = "other@example.com";
	set_write_args(cm, buf, CHUNK_SZ, CHUNK_SZ, 0, NULL, 0);
	cm->compound->c_args->argarray.argarray_val[0]
		.nfs_argop4_u.opchunk_write.cwa_stateid = stid;

	nfs4_op_chunk_write(cm->compound);
	ck_assert_int_eq(cm->compound->c_res->resarray.resarray_val[0]
				 .nfs_resop4_u.opchunk_write.cwr_status,
			 NFS4ERR_ACCESS);
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
	CHUNK_WRITE4resok *ok = &res->CHUNK_WRITE4res_u.cwr_resok4;

	ck_assert_int_eq(res->cwr_status, NFS4_OK);
	ck_assert_uint_eq(ok->cwr_count, 1);

	/*
	 * Wire-conformance regression guard for the three co-indexed
	 * per-chunk arrays (draft sec-CHUNK_WRITE :9062-9065): all three
	 * MUST be len == n (one entry per payload chunk).  A regression
	 * back to len == 0 for cwr_block_activated or cwr_owners would
	 * break strict decoders (Linux kernel client
	 * fs/nfs/flexfilesv2/flexfilesv2_xdr_chunk.c:547-570 returns
	 * -EPROTO on len mismatch).  cwr_block_activated stays FALSE
	 * for every element because reffs never invokes the
	 * CHUNK_WRITE_FLAGS_ACTIVATE_IF_EMPTY activation shortcut
	 * (:9163-9173); cwr_owners echoes the caller-supplied
	 * the compact cohort/client/co-id carrier.
	 */
	ck_assert_uint_eq(ok->cwr_block_status.cwr_block_status_len, 1);
	ck_assert_uint_eq(ok->cwr_block_activated.cwr_block_activated_len, 1);
	ck_assert_uint_eq(ok->cwr_owners.cwr_owners_len, 1);
	ck_assert_int_eq(ok->cwr_block_status.cwr_block_status_val[0], NFS4_OK);
	ck_assert_int_eq(ok->cwr_block_activated.cwr_block_activated_val[0],
			 false);
	ck_assert_uint_eq(ok->cwr_owners.cwr_owners_val[0].co_id, 99);

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
	CHUNK_WRITE4resok *ok = &res->CHUNK_WRITE4res_u.cwr_resok4;
	ck_assert_int_eq(res->cwr_status, NFS4_OK);
	ck_assert_uint_eq(ok->cwr_count, 3);
	ck_assert_uint_eq(ok->cwr_block_status.cwr_block_status_len, 3);
	ck_assert_uint_eq(ok->cwr_block_activated.cwr_block_activated_len, 3);
	ck_assert_uint_eq(ok->cwr_owners.cwr_owners_len, 3);
	for (uint32_t i = 0; i < 3; i++) {
		ck_assert_int_eq(ok->cwr_block_status.cwr_block_status_val[i],
				 NFS4_OK);
		ck_assert_int_eq(
			ok->cwr_block_activated.cwr_block_activated_val[i],
			false);
		ck_assert_uint_eq(ok->cwr_owners.cwr_owners_val[i].co_cohort_id,
				  0);
		ck_assert_uint_eq(ok->cwr_owners.cwr_owners_val[i].co_client_id,
				  0xBEEF);
		ck_assert_uint_eq(ok->cwr_owners.cwr_owners_val[i].co_id,
				  99 + i);
	}

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
/* Server-side algorithm enforcement                                    */
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
	static uint32_t co_ids[64];
	cm_set_op(cm, 0, OP_CHUNK_WRITE);
	CHUNK_WRITE4args *args = &cm->compound->c_args->argarray.argarray_val[0]
					  .nfs_argop4_u.opchunk_write;

	if (cm->chunk_stateid_registered)
		args->cwa_stateid = cm->chunk_stateid;
	else
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
	args->cwa_cohort_id = 0;
	args->cwa_client_id = 0xBEEF;
	uint32_t nchunks =
		chunk_size ? (buf_len + chunk_size - 1) / chunk_size : 0;
	ck_assert_uint_le(nchunks, sizeof(co_ids) / sizeof(co_ids[0]));
	args->cwa_co_ids.cwa_co_ids_len = nchunks;
	args->cwa_co_ids.cwa_co_ids_val = co_ids;
	for (uint32_t i = 0; i < nchunks; i++)
		args->cwa_co_ids.cwa_co_ids_val[i] = 99 + (uint32_t)offset + i;
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
	chunk_owner4 owner = { .co_client_id = 0xBEEF, .co_id = 99 };
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
	chunk_owner4 owner = { .co_client_id = 0xBEEF, .co_id = 99 };
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
 * Lifecycle identity includes the cohort and client fields, not just
 * co_id.  A mismatched triple must not be able to finalize another
 * writer's pending block.
 */
START_TEST(test_chunk_finalize_requires_full_owner_triple)
{
	static char buf[CHUNK_SZ];
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);

	set_write_args(cm, buf, CHUNK_SZ, CHUNK_SZ, 0, NULL, 0);
	CHUNK_WRITE4args *wargs =
		&cm->compound->c_args->argarray.argarray_val[0]
			 .nfs_argop4_u.opchunk_write;
	wargs->cwa_cohort_id = 0x1111;
	nfs4_op_chunk_write(cm->compound);
	ck_assert_int_eq(cm->compound->c_res->resarray.resarray_val[0]
				 .nfs_resop4_u.opchunk_write.cwr_status,
			 NFS4_OK);
	free_write_res(cm);

	struct chunk_block *blk = chunk_store_lookup(g_inode->i_chunk_store, 0);
	ck_assert_ptr_nonnull(blk);
	ck_assert_int_eq(blk->cb_state, CHUNK_STATE_PENDING);

	cm_reset_slot(cm, 0);
	cm_set_op(cm, 0, OP_CHUNK_FINALIZE);
	chunk_owner4 owner = {
		.co_cohort_id = 0x2222,
		.co_client_id = 0xBEEF,
		.co_id = 99,
	};
	CHUNK_FINALIZE4args *fargs =
		&cm->compound->c_args->argarray.argarray_val[0]
			 .nfs_argop4_u.opchunk_finalize;
	fargs->cfa_offset = 0;
	fargs->cfa_count = 1;
	fargs->cfa_chunks.cfa_chunks_val = &owner;
	fargs->cfa_chunks.cfa_chunks_len = 1;

	nfs4_op_chunk_finalize(cm->compound);
	CHUNK_FINALIZE4res *fres =
		&cm->compound->c_res->resarray.resarray_val[0]
			 .nfs_resop4_u.opchunk_finalize;
	ck_assert_int_eq(fres->cfr_status, NFS4_OK);
	ck_assert_int_eq(fres->CHUNK_FINALIZE4res_u.cfr_resok4.cfr_status
				 .cfr_status_val[0],
			 NFS4ERR_INVAL);
	ck_assert_int_eq(blk->cb_state, CHUNK_STATE_PENDING);
	free_finalize_res(cm);

	cm_reset_slot(cm, 0);
	cm_set_op(cm, 0, OP_CHUNK_FINALIZE);
	owner.co_cohort_id = 0x1111;
	fargs = &cm->compound->c_args->argarray.argarray_val[0]
			 .nfs_argop4_u.opchunk_finalize;
	fargs->cfa_offset = 0;
	fargs->cfa_count = 1;
	fargs->cfa_chunks.cfa_chunks_val = &owner;
	fargs->cfa_chunks.cfa_chunks_len = 1;

	nfs4_op_chunk_finalize(cm->compound);
	fres = &cm->compound->c_res->resarray.resarray_val[0]
			.nfs_resop4_u.opchunk_finalize;
	ck_assert_int_eq(fres->cfr_status, NFS4_OK);
	ck_assert_int_eq(fres->CHUNK_FINALIZE4res_u.cfr_resok4.cfr_status
				 .cfr_status_val[0],
			 NFS4_OK);
	ck_assert_int_eq(fres->CHUNK_FINALIZE4res_u.cfr_resok4.cfr_status
				 .cfr_status_val[1],
			 NFS4_OK);
	ck_assert_int_eq(blk->cb_state, CHUNK_STATE_FINALIZED);
	free_finalize_res(cm);

	cm_free(cm);
}
END_TEST

/*
 * Sparse FINALIZE: encodings with variable-size shards (Mojette
 * systematic; any future projection encoding) write blocks at a
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
	chunk_owner4 owners[2] = {
		{ .co_client_id = 0xBEEF, .co_id = 99 },
		{ .co_client_id = 0xBEEF, .co_id = 103 },
	};
	CHUNK_FINALIZE4args *fargs =
		&cm->compound->c_args->argarray.argarray_val[0]
			 .nfs_argop4_u.opchunk_finalize;
	fargs->cfa_offset = 0;
	fargs->cfa_count = 5;
	fargs->cfa_chunks.cfa_chunks_val = owners;
	fargs->cfa_chunks.cfa_chunks_len = 2;

	nfs4_op_chunk_finalize(cm->compound);

	CHUNK_FINALIZE4res *fres =
		&cm->compound->c_res->resarray.resarray_val[0]
			 .nfs_resop4_u.opchunk_finalize;
	ck_assert_int_eq(fres->cfr_status, NFS4_OK);
	ck_assert_int_eq(fres->CHUNK_FINALIZE4res_u.cfr_resok4.cfr_status
				 .cfr_status_val[0],
			 NFS4_OK);
	ck_assert_int_eq(fres->CHUNK_FINALIZE4res_u.cfr_resok4.cfr_status
				 .cfr_status_val[1],
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

/*
 * NFS4ERR_DELAY from either lifecycle operation must leave the block in
 * its prior state so a retry can complete the same transition.
 */
START_TEST(test_chunk_lifecycle_delay_preserves_state)
{
	static char buf[CHUNK_SZ];
	chunk_owner4 owner = { .co_client_id = 0xBEEF, .co_id = 99 };
	struct cm_ctx *cm = cm_alloc(1);
	struct chunk_block *blk;

	cm_set_inode(cm, g_inode);
	set_write_args(cm, buf, CHUNK_SZ, CHUNK_SZ, 0, NULL, 0);
	nfs4_op_chunk_write(cm->compound);
	ck_assert_int_eq(cm->compound->c_res->resarray.resarray_val[0]
				 .nfs_resop4_u.opchunk_write.cwr_status,
			 NFS4_OK);
	free_write_res(cm);

	blk = chunk_store_lookup(g_inode->i_chunk_store, 0);
	ck_assert_ptr_nonnull(blk);
	ck_assert_int_eq(blk->cb_state, CHUNK_STATE_PENDING);

	atomic_store_explicit(&cm->compound->c_server_state
				       ->ss_test_chunk_finalize_delay_count,
			      1, memory_order_relaxed);
	cm_reset_slot(cm, 0);
	cm_set_op(cm, 0, OP_CHUNK_FINALIZE);
	{
		CHUNK_FINALIZE4args *args =
			&cm->compound->c_args->argarray.argarray_val[0]
				 .nfs_argop4_u.opchunk_finalize;
		args->cfa_offset = 0;
		args->cfa_count = 1;
		args->cfa_chunks.cfa_chunks_val = &owner;
		args->cfa_chunks.cfa_chunks_len = 1;
	}
	nfs4_op_chunk_finalize(cm->compound);
	ck_assert_int_eq(cm->compound->c_res->resarray.resarray_val[0]
				 .nfs_resop4_u.opchunk_finalize.cfr_status,
			 NFS4ERR_DELAY);
	ck_assert_int_eq(blk->cb_state, CHUNK_STATE_PENDING);

	cm_reset_slot(cm, 0);
	cm_set_op(cm, 0, OP_CHUNK_FINALIZE);
	{
		CHUNK_FINALIZE4args *args =
			&cm->compound->c_args->argarray.argarray_val[0]
				 .nfs_argop4_u.opchunk_finalize;
		args->cfa_offset = 0;
		args->cfa_count = 1;
		args->cfa_chunks.cfa_chunks_val = &owner;
		args->cfa_chunks.cfa_chunks_len = 1;
	}
	nfs4_op_chunk_finalize(cm->compound);
	ck_assert_int_eq(cm->compound->c_res->resarray.resarray_val[0]
				 .nfs_resop4_u.opchunk_finalize.cfr_status,
			 NFS4_OK);
	free_finalize_res(cm);
	ck_assert_int_eq(blk->cb_state, CHUNK_STATE_FINALIZED);

	atomic_store_explicit(
		&cm->compound->c_server_state->ss_test_chunk_commit_delay_count,
		1, memory_order_relaxed);
	cm_reset_slot(cm, 0);
	cm_set_op(cm, 0, OP_CHUNK_COMMIT);
	{
		CHUNK_COMMIT4args *args =
			&cm->compound->c_args->argarray.argarray_val[0]
				 .nfs_argop4_u.opchunk_commit;
		args->cca_offset = 0;
		args->cca_count = 1;
		args->cca_chunks.cca_chunks_val = &owner;
		args->cca_chunks.cca_chunks_len = 1;
	}
	nfs4_op_chunk_commit(cm->compound);
	ck_assert_int_eq(cm->compound->c_res->resarray.resarray_val[0]
				 .nfs_resop4_u.opchunk_commit.ccr_status,
			 NFS4ERR_DELAY);
	ck_assert_int_eq(blk->cb_state, CHUNK_STATE_FINALIZED);

	cm_reset_slot(cm, 0);
	cm_set_op(cm, 0, OP_CHUNK_COMMIT);
	{
		CHUNK_COMMIT4args *args =
			&cm->compound->c_args->argarray.argarray_val[0]
				 .nfs_argop4_u.opchunk_commit;
		args->cca_offset = 0;
		args->cca_count = 1;
		args->cca_chunks.cca_chunks_val = &owner;
		args->cca_chunks.cca_chunks_len = 1;
	}
	nfs4_op_chunk_commit(cm->compound);
	ck_assert_int_eq(cm->compound->c_res->resarray.resarray_val[0]
				 .nfs_resop4_u.opchunk_commit.ccr_status,
			 NFS4_OK);
	free_commit_res(cm);
	ck_assert_int_eq(blk->cb_state, CHUNK_STATE_COMMITTED);

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
	chunk_owner4 owner = { .co_client_id = 0xBEEF, .co_id = 99 };
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
	chunk_owner4 owner = { .co_client_id = 0xBEEF, .co_id = 99 };
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
	chunk_owner4 owners[2] = {
		{ .co_client_id = 0xBEEF, .co_id = 99 },
		{ .co_client_id = 0xBEEF, .co_id = 103 },
	};
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
		fargs->cfa_chunks.cfa_chunks_val = owners;
		fargs->cfa_chunks.cfa_chunks_len = 2;
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
		cargs->cca_chunks.cca_chunks_val = owners;
		cargs->cca_chunks.cca_chunks_len = 2;
	}
	nfs4_op_chunk_commit(cm->compound);

	CHUNK_COMMIT4res *cres = &cm->compound->c_res->resarray.resarray_val[0]
					  .nfs_resop4_u.opchunk_commit;
	ck_assert_int_eq(cres->ccr_status, NFS4_OK);
	ck_assert_int_eq(
		cres->CHUNK_COMMIT4res_u.ccr_resok4.ccr_status.ccr_status_val[0],
		NFS4_OK);
	ck_assert_int_eq(
		cres->CHUNK_COMMIT4res_u.ccr_resok4.ccr_status.ccr_status_val[1],
		NFS4_OK);

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
	args->cra_stateid = cm->chunk_stateid;
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
	args->cra_stateid = cm->chunk_stateid;
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
 * The chunk-collision policy requires CHUNK_READ on a PENDING
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
	args->cra_stateid = cm->chunk_stateid;
	args->cra_offset = 0;
	args->cra_count = 1;

	nfs4_op_chunk_read(cm->compound);

	CHUNK_READ4res *res = &cm->compound->c_res->resarray.resarray_val[0]
				       .nfs_resop4_u.opchunk_read;
	ck_assert_int_eq(res->crr_status, NFS4ERR_DELAY);

	cm_free(cm);
}
END_TEST

/*
 * Bit-rot preservation contract: on stored-vs-disk CRC mismatch,
 * CHUNK_READ MUST return the STORED checksum on the wire, not a
 * recomputed CRC over the corrupted disk bytes.  A client-side
 * CRC verification then catches the rot as an integrity failure.
 * Repacking the CRC
 * over corrupted bytes would let the client happily verify and
 * launder the rot through an RMW round-trip.
 *
 * Test: write + finalize a chunk with a known payload and correct
 * CRC.  Simulate bit rot by mutating the stored cb_checksum_value
 * so it no longer matches the on-disk bytes (equivalent to the
 * on-disk bytes rotting, since the code path compares stored vs.
 * disk-computed).  READ and verify the wire cs_value equals the
 * mutated stored value, not the disk-derived CRC.
 */
/*
 * Attribute-90 SETATTR gate.
 *
 * This is the path that was silently dead for a commit: S1.1's
 * metadata-server settle step reaches a real NFSv4.2 data server as
 * an ordinary SETATTR over the control session, and S1b's original
 * guard rejected it unconditionally.  Combined mode could not catch
 * that -- dstore_ops_local sets the inode bit directly and never
 * crosses the wire -- so nothing exercised the wire path at all.
 *
 * These drive nfs4_op_setattr() the way the wire would.
 */
static void set_chunked_attr_args(struct cm_ctx *cm, bool chunked,
				  uint32_t *bits, char *val)
{
	cm_reset_slot(cm, 0);
	cm_set_op(cm, 0, OP_SETATTR);

	SETATTR4args *a = &cm->compound->c_args->argarray.argarray_val[0]
				   .nfs_argop4_u.opsetattr;

	memset(&a->stateid, 0, sizeof(a->stateid));

	/* Attribute 90: word 2, bit 26. */
	bits[0] = 0;
	bits[1] = 0;
	bits[2] = 1U << 26;
	val[0] = 0;
	val[1] = 0;
	val[2] = 0;
	val[3] = chunked ? 1 : 0;

	a->obj_attributes.attrmask.bitmap4_len = 3;
	a->obj_attributes.attrmask.bitmap4_val = bits;
	a->obj_attributes.attr_vals.attrlist4_len = 4;
	a->obj_attributes.attr_vals.attrlist4_val = val;
}

static nfsstat4 run_setattr(struct cm_ctx *cm)
{
	SETATTR4res *res = &cm->compound->c_res->resarray.resarray_val[0]
				    .nfs_resop4_u.opsetattr;

	nfs4_op_setattr(cm->compound);

	nfsstat4 status = res->status;

	/*
	 * nattr_to_inode() copies the attrmask into res->attrsset on
	 * the success path, allocating it.  Production releases that
	 * through the XDR free path once the compound is encoded;
	 * calling the handler directly means the test owns it.
	 */
	bitmap4_destroy(&res->attrsset);
	memset(&res->attrsset, 0, sizeof(res->attrsset));
	return status;
}

START_TEST(test_attr90_client_setattr_rejected)
{
	struct cm_ctx *cm = cm_alloc(1);
	uint32_t bits[3];
	char val[4];

	cm_set_inode(cm, g_inode);
	/* A plain client: no control-session flag. */
	cm->compound->c_nfs4_client->nc_exchgid_flags = 0;

	set_chunked_attr_args(cm, true, bits, val);
	ck_assert_int_eq(run_setattr(cm), NFS4ERR_INVAL);

	/* And the inode must be untouched -- still unidentified. */
	ck_assert_int_eq(inode_chunked_state(g_inode),
			 INODE_CHUNKED_UNIDENTIFIED);

	cm_free(cm);
}
END_TEST

START_TEST(test_attr90_mds_setattr_on_empty_accepted)
{
	struct cm_ctx *cm = cm_alloc(1);
	uint32_t bits[3];
	char val[4];

	cm_set_inode(cm, g_inode);
	cm->compound->c_nfs4_client->nc_exchgid_flags =
		EXCHGID4_FLAG_USE_PNFS_MDS;

	/* Empty file: the metadata server may settle the value. */
	set_chunked_attr_args(cm, true, bits, val);
	ck_assert_int_eq(run_setattr(cm), NFS4_OK);
	ck_assert_int_eq(inode_chunked_state(g_inode), INODE_CHUNKED_YES);

	/* FALSE is equally settable while empty, and is not the same
	 * state as never having been told. */
	set_chunked_attr_args(cm, false, bits, val);
	ck_assert_int_eq(run_setattr(cm), NFS4_OK);
	ck_assert_int_eq(inode_chunked_state(g_inode), INODE_CHUNKED_NO);

	cm_free(cm);
}
END_TEST

START_TEST(test_attr90_mds_setattr_on_nonempty_rejected)
{
	static char buf[CHUNK_SZ];
	uint32_t good_crc = (uint32_t)crc32(0L, (const Bytef *)buf, CHUNK_SZ);
	struct cm_ctx *cm = cm_alloc(1);
	uint32_t bits[3];
	char val[4];

	cm_set_inode(cm, g_inode);
	cm->compound->c_nfs4_client->nc_exchgid_flags =
		EXCHGID4_FLAG_USE_PNFS_MDS;

	/* Give the file content: a chunk store is one of the two
	 * signals reffs treats as "not empty". */
	set_write_args(cm, buf, CHUNK_SZ, CHUNK_SZ, 0, &good_crc, 1);
	nfs4_op_chunk_write(cm->compound);
	free_write_args(cm);
	free_write_res(cm);
	ck_assert_ptr_nonnull(g_inode->i_chunk_store);

	/*
	 * Even the metadata server may not change the value now: the
	 * attribute describes the format the content is already in.
	 */
	set_chunked_attr_args(cm, true, bits, val);
	ck_assert_int_eq(run_setattr(cm), NFS4ERR_INVAL);

	cm_free(cm);
}
END_TEST

/*
 * S1.5(B): CHUNK operations against an identified non-chunked file.
 *
 * draft-haynes-nfsv4-flexfiles-v2 sec-ops-client makes this a MUST
 * reject with NFS4ERR_NOTSUPP.  The three states are covered
 * separately because the interesting failure is not "does the reject
 * fire" but "does it fire on the wrong state" -- treating
 * UNIDENTIFIED as FALSE would return NFS4ERR_NOTSUPP for every CHUNK
 * operation in combined mode, which is the configuration
 * scripts/test_mirror_local.sh and the v2 benchmark variants run on.
 */

/* Drive the inode to a given identification state. */
static void mark_chunked(struct inode *inode, enum inode_chunked_state st)
{
	switch (st) {
	case INODE_CHUNKED_UNIDENTIFIED:
		inode->i_attr_flags &= ~(uint64_t)(INODE_CHUNKED_ATTR_PRESENT |
						   INODE_IS_CHUNKED_DATA_FILE);
		break;
	case INODE_CHUNKED_NO:
		inode->i_attr_flags |= INODE_CHUNKED_ATTR_PRESENT;
		inode->i_attr_flags &= ~(uint64_t)INODE_IS_CHUNKED_DATA_FILE;
		break;
	case INODE_CHUNKED_YES:
		inode->i_attr_flags |= INODE_CHUNKED_ATTR_PRESENT |
				       INODE_IS_CHUNKED_DATA_FILE;
		break;
	}
	ck_assert_int_eq(inode_chunked_state(inode), st);
}

/* One CHUNK_WRITE of a single valid chunk; returns the wire status. */
static nfsstat4 run_chunk_write(struct cm_ctx *cm, char *buf)
{
	uint32_t crc = (uint32_t)crc32(0L, (const Bytef *)buf, CHUNK_SZ);
	nfsstat4 st;

	cm_reset_slot(cm, 0);
	set_write_args(cm, buf, CHUNK_SZ, CHUNK_SZ, 0, &crc, 1);
	nfs4_op_chunk_write(cm->compound);
	st = cm->compound->c_res->resarray.resarray_val[0]
		     .nfs_resop4_u.opchunk_write.cwr_status;
	free_write_args(cm);
	free_write_res(cm);
	return st;
}

/* FINALIZE block 0 for one owner; returns the wire status. */
static nfsstat4 run_chunk_finalize(struct cm_ctx *cm, chunk_owner4 *owner)
{
	nfsstat4 st;

	cm_reset_slot(cm, 0);
	cm_set_op(cm, 0, OP_CHUNK_FINALIZE);
	{
		CHUNK_FINALIZE4args *a =
			&cm->compound->c_args->argarray.argarray_val[0]
				 .nfs_argop4_u.opchunk_finalize;
		a->cfa_offset = 0;
		a->cfa_count = 1;
		a->cfa_chunks.cfa_chunks_val = owner;
		a->cfa_chunks.cfa_chunks_len = 1;
	}
	nfs4_op_chunk_finalize(cm->compound);
	st = cm->compound->c_res->resarray.resarray_val[0]
		     .nfs_resop4_u.opchunk_finalize.cfr_status;
	free_finalize_res(cm);
	return st;
}

/* One CHUNK_READ of block 0; returns the wire status. */
static nfsstat4 run_chunk_read(struct cm_ctx *cm)
{
	nfsstat4 st;

	cm_reset_slot(cm, 0);
	cm_set_op(cm, 0, OP_CHUNK_READ);
	{
		CHUNK_READ4args *a =
			&cm->compound->c_args->argarray.argarray_val[0]
				 .nfs_argop4_u.opchunk_read;
		a->cra_stateid = cm->chunk_stateid;
		a->cra_offset = 0;
		a->cra_count = 1;
	}
	nfs4_op_chunk_read(cm->compound);
	st = cm->compound->c_res->resarray.resarray_val[0]
		     .nfs_resop4_u.opchunk_read.crr_status;
	free_read_res(cm);
	return st;
}

START_TEST(test_chunk_ops_rejected_on_non_chunked)
{
	static char buf[CHUNK_SZ];
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);
	mark_chunked(g_inode, INODE_CHUNKED_NO);

	ck_assert_int_eq(run_chunk_write(cm, buf), NFS4ERR_NOTSUPP);
	ck_assert_int_eq(run_chunk_read(cm), NFS4ERR_NOTSUPP);

	/* The reject must happen before any chunk state is created. */
	ck_assert_ptr_null(g_inode->i_chunk_store);

	cm_free(cm);
}
END_TEST

START_TEST(test_chunk_ops_allowed_when_unidentified)
{
	static char buf[CHUNK_SZ];
	chunk_owner4 owner = { .co_client_id = 0xBEEF, .co_id = 99 };
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);
	mark_chunked(g_inode, INODE_CHUNKED_UNIDENTIFIED);

	/*
	 * This is the combined-mode case: dstore_ops_local never sets
	 * attribute 90, so every file the mirror test touches is
	 * unidentified.  Enforcement keyed on the value bit alone would
	 * turn this red and take the mirror test with it.
	 *
	 * The read is after a FINALIZE because a PENDING block answers
	 * NFS4ERR_DELAY, which would mask the reject this test is
	 * looking for.
	 */
	ck_assert_int_eq(run_chunk_write(cm, buf), NFS4_OK);
	ck_assert_int_eq(run_chunk_finalize(cm, &owner), NFS4_OK);
	ck_assert_int_eq(run_chunk_read(cm), NFS4_OK);

	cm_free(cm);
}
END_TEST

START_TEST(test_chunk_ops_allowed_when_chunked)
{
	static char buf[CHUNK_SZ];
	chunk_owner4 owner = { .co_client_id = 0xBEEF, .co_id = 99 };
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);
	mark_chunked(g_inode, INODE_CHUNKED_YES);

	ck_assert_int_eq(run_chunk_write(cm, buf), NFS4_OK);
	ck_assert_int_eq(run_chunk_finalize(cm, &owner), NFS4_OK);
	ck_assert_int_eq(run_chunk_read(cm), NFS4_OK);

	cm_free(cm);
}
END_TEST

/*
 * The lifecycle ops carry the gate in their own prologues rather than
 * through chunk_write_validate_payload, so they are checked
 * separately -- a gate added to the write path alone would leave
 * FINALIZE, COMMIT and ROLLBACK reachable on a non-chunked file.
 */
START_TEST(test_chunk_lifecycle_ops_rejected_on_non_chunked)
{
	chunk_owner4 owner = { .co_client_id = 0xBEEF, .co_id = 99 };
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);
	mark_chunked(g_inode, INODE_CHUNKED_NO);

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
			 NFS4ERR_NOTSUPP);
	free_finalize_res(cm);

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
			 NFS4ERR_NOTSUPP);
	free_commit_res(cm);

	cm_free(cm);
}
END_TEST

/*
 * S7: cover the R7c behavioral change.
 *
 * R7c turned read_chunk4's cr_locked from an always-empty array
 * into a populated scalar, and cr_guard into a dual-write of the
 * per-block guard.  Nothing exercised either.  The existing tests
 * only read unlocked blocks, where the calloc'd zero happens to be
 * the right answer -- so a regression that stopped populating
 * cr_locked entirely would still have passed the whole suite.
 */
START_TEST(test_chunk_read_locked_flag_reported)
{
	static char buf[CHUNK_SZ];
	uint32_t good_crc = (uint32_t)crc32(0L, (const Bytef *)buf, CHUNK_SZ);
	chunk_owner4 owner = { .co_client_id = 0xBEEF, .co_id = 99 };
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);

	set_write_args(cm, buf, CHUNK_SZ, CHUNK_SZ, 0, &good_crc, 1);
	nfs4_op_chunk_write(cm->compound);
	free_write_args(cm);
	free_write_res(cm);

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
	free_finalize_res(cm);

	/* Unlocked block: cr_locked must report no state flags. */
	cm_reset_slot(cm, 0);
	cm_set_op(cm, 0, OP_CHUNK_READ);
	{
		CHUNK_READ4args *a =
			&cm->compound->c_args->argarray.argarray_val[0]
				 .nfs_argop4_u.opchunk_read;
		a->cra_stateid = cm->chunk_stateid;
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
		ck_assert_uint_eq(resok->crr_chunks.crr_chunks_val[0].cr_locked,
				  0);
	}
	free_read_res(cm);

	/*
	 * Now set the in-memory lock bit and re-read.  This is the
	 * assertion that would have caught a regression: it fails if
	 * cr_locked is left at its calloc'd zero.
	 */
	{
		struct chunk_block *blk =
			chunk_store_lookup(g_inode->i_chunk_store, 0);

		ck_assert_ptr_nonnull(blk);
		blk->cb_flags |= CHUNK_BLOCK_LOCKED;
	}

	cm_reset_slot(cm, 0);
	cm_set_op(cm, 0, OP_CHUNK_READ);
	{
		CHUNK_READ4args *a =
			&cm->compound->c_args->argarray.argarray_val[0]
				 .nfs_argop4_u.opchunk_read;
		a->cra_stateid = cm->chunk_stateid;
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
		ck_assert_uint_eq(resok->crr_chunks.crr_chunks_val[0].cr_locked,
				  CHUNK_STATE_FLAGS_LOCKED);
		/* cr_status is per-chunk and scalar since R7c. */
		ck_assert_int_eq(resok->crr_chunks.crr_chunks_val[0].cr_status,
				 NFS4_OK);
	}
	free_read_res(cm);

	cm_free(cm);
}
END_TEST

/*
 * cr_owner and cr_guard answer different questions and no longer
 * share a generation.  cr_owner is the {cohort, client, chunk}
 * triple the writer supplied; cr_guard is the server's own CAS
 * state, whose cg_gen_id the server assigns (0 on a first write)
 * and no client can choose.  Assert the split so a change that
 * re-merges them has to face this test.
 */
START_TEST(test_chunk_read_owner_and_guard_are_distinct)
{
	static char buf[CHUNK_SZ];
	uint32_t good_crc = (uint32_t)crc32(0L, (const Bytef *)buf, CHUNK_SZ);
	chunk_owner4 owner = { .co_client_id = 0xBEEF, .co_id = 99 };
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);

	set_write_args(cm, buf, CHUNK_SZ, CHUNK_SZ, 0, &good_crc, 1);
	nfs4_op_chunk_write(cm->compound);
	free_write_args(cm);
	free_write_res(cm);

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
	free_finalize_res(cm);

	cm_reset_slot(cm, 0);
	cm_set_op(cm, 0, OP_CHUNK_READ);
	{
		CHUNK_READ4args *a =
			&cm->compound->c_args->argarray.argarray_val[0]
				 .nfs_argop4_u.opchunk_read;
		a->cra_stateid = cm->chunk_stateid;
		a->cra_offset = 0;
		a->cra_count = 1;
	}
	nfs4_op_chunk_read(cm->compound);
	{
		CHUNK_READ4res *res =
			&cm->compound->c_res->resarray.resarray_val[0]
				 .nfs_resop4_u.opchunk_read;
		ck_assert_int_eq(res->crr_status, NFS4_OK);

		read_chunk4 *rc = &res->CHUNK_READ4res_u.crr_resok4.crr_chunks
					   .crr_chunks_val[0];

		/*
		 * First write to a virgin offset -- the fixture is
		 * per-test -- so the server-assigned generation is 0.
		 */
		ck_assert_uint_eq(rc->cr_guard.cg_gen_id, 0);
		/* The owner triple is echoed back as the writer sent it. */
		ck_assert_uint_eq(rc->cr_owner.co_client_id, 0xBEEF);
		ck_assert_uint_eq(rc->cr_owner.co_id, 99);
		/*
		 * cg_client_id is the last writer's id, which for a
		 * single-writer block coincides with the owner's.
		 */
		ck_assert_uint_eq(rc->cr_guard.cg_client_id, 0xBEEF);
	}
	free_read_res(cm);

	cm_free(cm);
}
END_TEST

/*
 * Helpers for the generation-semantics tests below.  Each drives one
 * step of the WRITE -> FINALIZE -> READ cycle on block 0 so the tests
 * read as the sequence of wire ops they are.
 */
static void gen_write(struct cm_ctx *cm, char *buf, uint32_t crc,
		      const chunk_guard4 *guard, nfsstat4 want)
{
	cm_reset_slot(cm, 0);
	set_write_args(cm, buf, CHUNK_SZ, CHUNK_SZ, 0, &crc, 1);
	{
		CHUNK_WRITE4args *a =
			&cm->compound->c_args->argarray.argarray_val[0]
				 .nfs_argop4_u.opchunk_write;
		if (guard) {
			a->cwa_guard.cwg_check = TRUE;
			a->cwa_guard.write_chunk_guard4_u.cwg_guard = *guard;
		} else {
			a->cwa_guard.cwg_check = FALSE;
		}
	}
	nfs4_op_chunk_write(cm->compound);
	{
		CHUNK_WRITE4res *res =
			&cm->compound->c_res->resarray.resarray_val[0]
				 .nfs_resop4_u.opchunk_write;
		ck_assert_uint_eq(res->cwr_status, want);
	}
	free_write_args(cm);
	free_write_res(cm);
}

static void gen_finalize(struct cm_ctx *cm, chunk_owner4 *owner)
{
	cm_reset_slot(cm, 0);
	cm_set_op(cm, 0, OP_CHUNK_FINALIZE);
	{
		CHUNK_FINALIZE4args *a =
			&cm->compound->c_args->argarray.argarray_val[0]
				 .nfs_argop4_u.opchunk_finalize;
		a->cfa_offset = 0;
		a->cfa_count = 1;
		a->cfa_chunks.cfa_chunks_val = owner;
		a->cfa_chunks.cfa_chunks_len = 1;
	}
	nfs4_op_chunk_finalize(cm->compound);
	free_finalize_res(cm);
}

/* Returns the server's current guard for block 0. */
static chunk_guard4 gen_read_guard(struct cm_ctx *cm)
{
	chunk_guard4 g = { 0 };

	cm_reset_slot(cm, 0);
	cm_set_op(cm, 0, OP_CHUNK_READ);
	{
		CHUNK_READ4args *a =
			&cm->compound->c_args->argarray.argarray_val[0]
				 .nfs_argop4_u.opchunk_read;
		a->cra_stateid = cm->chunk_stateid;
		a->cra_offset = 0;
		a->cra_count = 1;
	}
	nfs4_op_chunk_read(cm->compound);
	{
		CHUNK_READ4res *res =
			&cm->compound->c_res->resarray.resarray_val[0]
				 .nfs_resop4_u.opchunk_read;
		ck_assert_int_eq(res->crr_status, NFS4_OK);
		g = res->CHUNK_READ4res_u.crr_resok4.crr_chunks
			    .crr_chunks_val[0]
			    .cr_guard;
	}
	free_read_res(cm);
	return g;
}

/*
 * The generation is the data server's to assign.  A first write to a
 * never-written block starts at 0 no matter what the writer says, and
 * each accepted write advances it by one.  This is what makes the
 * cwa_guard CAS a real check: while the client minted the generation
 * and the server stored it verbatim, the client supplied both sides
 * of the server's comparison and the guard could not fail.
 */
START_TEST(test_chunk_write_gen_id_server_assigned_and_increments)
{
	static char buf[CHUNK_SZ];
	uint32_t crc = (uint32_t)crc32(0L, (const Bytef *)buf, CHUNK_SZ);
	chunk_owner4 owner = { .co_client_id = 0xBEEF, .co_id = 99 };
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);

	/* First write to a virgin block -- server assigns generation 0. */
	gen_write(cm, buf, crc, NULL, NFS4_OK);
	gen_finalize(cm, &owner);

	chunk_guard4 g0 = gen_read_guard(cm);

	ck_assert_uint_eq(g0.cg_gen_id, 0);

	/* Re-present exactly what we read: the CAS passes, gen advances. */
	gen_write(cm, buf, crc, &g0, NFS4_OK);
	gen_finalize(cm, &owner);

	chunk_guard4 g1 = gen_read_guard(cm);

	ck_assert_uint_eq(g1.cg_gen_id, 1);

	cm_free(cm);
}
END_TEST

/*
 * A writer that acts on a generation someone else has already
 * superseded must be told to back off.  This is the case the PENDING
 * gate cannot catch -- the earlier writer finished its whole
 * PENDING -> FINALIZED cycle, so the block looks clean.
 */
START_TEST(test_chunk_write_stale_guard_rejected)
{
	static char buf[CHUNK_SZ];
	uint32_t crc = (uint32_t)crc32(0L, (const Bytef *)buf, CHUNK_SZ);
	chunk_owner4 owner = { .co_client_id = 0xBEEF, .co_id = 99 };
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);

	gen_write(cm, buf, crc, NULL, NFS4_OK);
	gen_finalize(cm, &owner);

	/* Our reader captures the guard here, at generation 0. */
	chunk_guard4 stale = gen_read_guard(cm);

	/* Another writer lands and completes, advancing the block. */
	gen_write(cm, buf, crc, &stale, NFS4_OK);
	gen_finalize(cm, &owner);

	/* Our write, still holding the generation-0 guard, is refused. */
	gen_write(cm, buf, crc, &stale, NFS4ERR_DELAY);

	cm_free(cm);
}
END_TEST

START_TEST(test_chunk_read_bit_rot_preserves_stored_checksum)
{
	static char buf[CHUNK_SZ]; /* zero-filled */
	uint32_t good_crc = (uint32_t)crc32(0L, (const Bytef *)buf, CHUNK_SZ);
	chunk_owner4 owner = { .co_client_id = 0xBEEF, .co_id = 99 };
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);

	/* WRITE with correct CRC over zero payload. */
	set_write_args(cm, buf, CHUNK_SZ, CHUNK_SZ, 0, &good_crc, 1);
	nfs4_op_chunk_write(cm->compound);
	ck_assert_int_eq(cm->compound->c_res->resarray.resarray_val[0]
				 .nfs_resop4_u.opchunk_write.cwr_status,
			 NFS4_OK);
	free_write_args(cm);
	free_write_res(cm);

	/* FINALIZE so the block is visible to READ. */
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

	/*
	 * Simulate bit rot by mutating stored cb_checksum_value so it
	 * diverges from the actual disk bytes.  From the CHUNK_READ
	 * handler's perspective this is indistinguishable from the
	 * disk bytes rotting under a still-correct stored checksum:
	 * both scenarios yield stored != disk-computed.  Preservation
	 * of the stored value on the wire is the load-bearing
	 * property the client CRC verify relies on.
	 */
	struct chunk_block *blk = chunk_store_lookup(g_inode->i_chunk_store, 0);

	ck_assert_ptr_nonnull(blk);
	ck_assert_uint_eq(blk->cb_checksum_algorithm, CHECKSUM_ALG_CRC32);
	ck_assert_uint_eq(blk->cb_checksum_len, 4);
	uint8_t rotted[4] = { 0xDE, 0xAD, 0xBE, 0xEF };

	memcpy(blk->cb_checksum_value, rotted, 4);

	/* READ and verify wire cs_value == rotted (stored), not disk_crc. */
	cm_reset_slot(cm, 0);
	cm_set_op(cm, 0, OP_CHUNK_READ);
	{
		CHUNK_READ4args *a =
			&cm->compound->c_args->argarray.argarray_val[0]
				 .nfs_argop4_u.opchunk_read;
		a->cra_stateid = cm->chunk_stateid;
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
		checksum4 *cs =
			&resok->crr_chunks.crr_chunks_val[0].cr_checksum;
		ck_assert_uint_eq(cs->cs_algorithm, CHECKSUM_ALG_CRC32);
		ck_assert_uint_eq(cs->cs_value.cs_value_len, 4);
		/*
		 * cs_value MUST be the preserved (rotted) stored value.
		 * A regression to the old repack-with-disk-CRC path
		 * would show good_crc big-endian bytes here instead.
		 */
		ck_assert_int_eq(memcmp(cs->cs_value.cs_value_val, rotted, 4),
				 0);
		/* Sanity: it must NOT equal the disk-derived good_crc. */
		uint8_t disk_be[4] = { (uint8_t)(good_crc >> 24),
				       (uint8_t)(good_crc >> 16),
				       (uint8_t)(good_crc >> 8),
				       (uint8_t)good_crc };
		ck_assert_int_ne(memcmp(cs->cs_value.cs_value_val, disk_be, 4),
				 0);
	}
	free_read_res(cm);
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
	chunk_owner4 owner = { .co_client_id = 0xBEEF, .co_id = 99 };

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
		a->cra_stateid = cm->chunk_stateid;
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
		a->cra_stateid = cm->chunk_stateid;
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

/* A fresh inode view must recover lifecycle metadata before CHUNK_READ. */
START_TEST(test_chunk_read_loads_persisted_store_after_restart)
{
	static char wbuf[CHUNK_SZ];
	chunk_owner4 owner = { .co_client_id = 0xBEEF, .co_id = 99 };
	struct cm_ctx *cm = cm_alloc(1);

	memset(wbuf, 0x5A, sizeof(wbuf));
	cm_set_inode(cm, g_inode);
	ck_assert_ptr_nonnull(cm->compound->c_server_state->ss_state_dir);

	ck_assert_int_eq(run_chunk_write(cm, wbuf), NFS4_OK);
	ck_assert_int_eq(run_chunk_finalize(cm, &owner), NFS4_OK);

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

	/* Exercise the reserved CHUNK_LOCK metadata fields on disk. */
	pthread_mutex_lock(&g_inode->i_attr_mutex);
	struct chunk_block *lockblk =
		chunk_store_lookup(g_inode->i_chunk_store, 0);
	lockblk->cb_flags |= CHUNK_BLOCK_LOCKED | CHUNK_BLOCK_ESCROW;
	lockblk->cb_lock_cohort_id = 0x1234;
	lockblk->cb_lock_client_id = 0xBEEF;
	lockblk->cb_lock_owner_id = 99;
	lockblk->cb_lock_offset = 0;
	lockblk->cb_lock_count = 1;
	lockblk->cb_lock_flags = 0x1;
	memset(lockblk->cb_lock_stateid, 0xA5,
	       sizeof(lockblk->cb_lock_stateid));
	memset(lockblk->cb_lock_escrow_id, 0x5A,
	       sizeof(lockblk->cb_lock_escrow_id));
	g_inode->i_chunk_store->cs_dirty = true;
	ck_assert_int_eq(
		chunk_store_persist(g_inode->i_chunk_store,
				    cm->compound->c_server_state->ss_state_dir,
				    g_inode->i_ino),
		0);
	pthread_mutex_unlock(&g_inode->i_attr_mutex);

	/* Drop the in-memory index to model a server restart. */
	pthread_mutex_lock(&g_inode->i_attr_mutex);
	struct chunk_store *old = g_inode->i_chunk_store;
	g_inode->i_chunk_store = NULL;
	chunk_store_destroy(old);
	pthread_mutex_unlock(&g_inode->i_attr_mutex);

	ck_assert_ptr_null(g_inode->i_chunk_store);
	ck_assert_int_eq(run_chunk_read(cm), NFS4_OK);
	ck_assert_ptr_nonnull(g_inode->i_chunk_store);
	ck_assert_int_eq(
		chunk_store_lookup(g_inode->i_chunk_store, 0)->cb_state,
		CHUNK_STATE_COMMITTED);
	lockblk = chunk_store_lookup(g_inode->i_chunk_store, 0);
	ck_assert_uint_eq(lockblk->cb_flags & CHUNK_BLOCK_LOCKED,
			  CHUNK_BLOCK_LOCKED);
	ck_assert_uint_eq(lockblk->cb_lock_cohort_id, 0x1234);
	ck_assert_uint_eq(lockblk->cb_lock_client_id, 0xBEEF);
	ck_assert_uint_eq(lockblk->cb_lock_owner_id, 99);
	ck_assert_uint_eq(lockblk->cb_lock_count, 1);
	ck_assert_uint_eq(lockblk->cb_flags & CHUNK_BLOCK_ESCROW,
			  CHUNK_BLOCK_ESCROW);
	ck_assert_uint_eq(g_inode->i_chunk_store->cs_nescrows, 1);
	ck_assert_uint_eq(g_inode->i_chunk_store->cs_escrows[0].cer_offset, 0);
	ck_assert_uint_eq(g_inode->i_chunk_store->cs_escrows[0].cer_count, 1);
	uint8_t expected_escrow[CHUNK_LOCK_ESCROW_ID_SIZE];
	memset(expected_escrow, 0x5A, sizeof(expected_escrow));
	ck_assert_mem_eq(g_inode->i_chunk_store->cs_escrows[0].cer_id,
			 expected_escrow, sizeof(expected_escrow));
	uint8_t expected_stateid[CHUNK_LOCK_STATEID_SIZE];
	memset(expected_stateid, 0xA5, sizeof(expected_stateid));
	ck_assert_mem_eq(lockblk->cb_lock_stateid, expected_stateid,
			 CHUNK_LOCK_STATEID_SIZE);

	cm_free(cm);
}
END_TEST

START_TEST(test_chunk_error_quarantines_committed_chunk)
{
	static char wbuf[CHUNK_SZ];
	chunk_owner4 owner = { .co_client_id = 0xBEEF, .co_id = 99 };
	struct cm_ctx *cm = cm_alloc(1);

	memset(wbuf, 0xC3, sizeof(wbuf));
	cm_set_inode(cm, g_inode);
	ck_assert_int_eq(run_chunk_write(cm, wbuf), NFS4_OK);
	ck_assert_int_eq(run_chunk_finalize(cm, &owner), NFS4_OK);

	cm_reset_slot(cm, 0);
	cm_set_op(cm, 0, OP_CHUNK_COMMIT);
	{
		CHUNK_COMMIT4args *a =
			&cm->compound->c_args->argarray.argarray_val[0]
				 .nfs_argop4_u.opchunk_commit;
		a->cca_stateid = cm->chunk_stateid;
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

	cm_reset_slot(cm, 0);
	cm_set_op(cm, 0, OP_CHUNK_ERROR);
	{
		CHUNK_ERROR4args *a =
			&cm->compound->c_args->argarray.argarray_val[0]
				 .nfs_argop4_u.opchunk_error;
		a->cea_stateid = cm->chunk_stateid;
		a->cea_offset = 0;
		a->cea_count = CHUNK_MAX_CHUNKS_PER_OP + 1;
		a->cea_error = NFS4ERR_PAYLOAD_NOT_ATOMIC;
		a->cea_owner = owner;
	}
	nfs4_op_chunk_error(cm->compound);
	ck_assert_int_eq(cm->compound->c_res->resarray.resarray_val[0]
				 .nfs_resop4_u.opchunk_error.cer_status,
			 NFS4ERR_INVAL);
	ck_assert_uint_eq(
		chunk_store_lookup(g_inode->i_chunk_store, 0)->cb_flags &
			CHUNK_BLOCK_ERROR,
		0);

	cm_reset_slot(cm, 0);
	cm_set_op(cm, 0, OP_CHUNK_ERROR);
	{
		CHUNK_ERROR4args *a =
			&cm->compound->c_args->argarray.argarray_val[0]
				 .nfs_argop4_u.opchunk_error;
		a->cea_stateid = cm->chunk_stateid;
		a->cea_offset = 0;
		a->cea_count = 1;
		a->cea_error = NFS4ERR_PAYLOAD_NOT_ATOMIC;
		a->cea_owner = owner;
	}
	nfs4_op_chunk_error(cm->compound);
	ck_assert_int_eq(cm->compound->c_res->resarray.resarray_val[0]
				 .nfs_resop4_u.opchunk_error.cer_status,
			 NFS4_OK);
	ck_assert_uint_eq(
		chunk_store_lookup(g_inode->i_chunk_store, 0)->cb_flags &
			CHUNK_BLOCK_ERROR,
		CHUNK_BLOCK_ERROR);

	/* CHUNK_ERROR must persist across a server restart. */
	pthread_mutex_lock(&g_inode->i_attr_mutex);
	struct chunk_store *old = g_inode->i_chunk_store;
	g_inode->i_chunk_store = NULL;
	chunk_store_destroy(old);
	struct chunk_store *reloaded = chunk_store_get(
		g_inode, cm->compound->c_server_state->ss_state_dir);
	ck_assert_ptr_nonnull(reloaded);
	ck_assert_uint_eq(chunk_store_lookup(reloaded, 0)->cb_flags &
				  CHUNK_BLOCK_ERROR,
			  CHUNK_BLOCK_ERROR);
	pthread_mutex_unlock(&g_inode->i_attr_mutex);

	cm_reset_slot(cm, 0);
	cm_set_op(cm, 0, OP_CHUNK_READ);
	{
		CHUNK_READ4args *a =
			&cm->compound->c_args->argarray.argarray_val[0]
				 .nfs_argop4_u.opchunk_read;
		a->cra_stateid = cm->chunk_stateid;
		a->cra_offset = 0;
		a->cra_count = 1;
	}
	nfs4_op_chunk_read(cm->compound);
	{
		CHUNK_READ4res *res =
			&cm->compound->c_res->resarray.resarray_val[0]
				 .nfs_resop4_u.opchunk_read;
		CHUNK_READ4resok *ok = &res->CHUNK_READ4res_u.crr_resok4;

		ck_assert_int_eq(res->crr_status, NFS4_OK);
		ck_assert_uint_eq(ok->crr_chunks.crr_chunks_len, 1);
		ck_assert_int_eq(ok->crr_chunks.crr_chunks_val[0].cr_status,
				 NFS4ERR_PAYLOAD_NOT_ATOMIC);
		ck_assert_uint_eq(
			ok->crr_chunks.crr_chunks_val[0].cr_chunk.cr_chunk_len,
			0);
	}
	free_read_res(cm);
	cm_free(cm);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Group G: chunk-collision counter observability                     */
/*                                                                     */
/* Earlier implementations incremented cs_pending_displaced when a     */
/* landed at an offset whose previous PENDING block came from a        */
/* different writer.  The current CHUNK_WRITE gate rejects that case  */
/* before the displaced-counting code runs;                            */
/* cs_chunk_busy_delay is the new counter that records the same        */
/* contention pattern with reject-semantics instead of                 */
/* observe-then-allow.  The tests in this group still drive the same   */
/* two-distinct-writers scenario, but the assertion target moves to    */
/* cs_chunk_busy_delay and the second writer's CHUNK_WRITE is          */
/* expected to return NFS4ERR_DELAY (not NFS4_OK with a side-effect). */
/* ------------------------------------------------------------------ */

/*
 * Override the owner fields on the CHUNK_WRITE in slot 0 so the
 * test can simulate two distinct writers contending on the same
 * block.  Call AFTER set_write_args.
 *
 * There is no generation parameter: the server assigns cg_gen_id,
 * so a writer cannot choose one.  Tests that need to exercise the
 * CAS set cwa_guard with a generation obtained from a CHUNK_READ.
 */
static void set_owner(struct cm_ctx *cm, uint64_t client_id, uint64_t owner_id)
{
	CHUNK_WRITE4args *args = &cm->compound->c_args->argarray.argarray_val[0]
					  .nfs_argop4_u.opchunk_write;

	args->cwa_client_id = client_id;
	if (args->cwa_co_ids.cwa_co_ids_len > 0)
		args->cwa_co_ids.cwa_co_ids_val[0] = owner_id;
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
	set_owner(cm, /* client_id */ 0xA1, /* owner_id */ 10);
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
	set_owner(cm, /* client_id */ 0xB2, /* owner_id */ 20);
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
	set_owner(cm, /* client_id */ 0xA1, /* owner_id */ 10);
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
	 * collision gate rejects this write with
	 * NFS4ERR_DELAY: PS-B's owner triple does not match the PENDING
	 * block PS-A still holds.  PS-B's client is expected to retry
	 * the whole RMW after PS-A's FINALIZE+COMMIT.  The counter
	 * cs_chunk_busy_delay records
	 * the rejection.
	 */
	set_write_args(cm, buf, CHUNK_SZ, CHUNK_SZ, /* offset */ 0, NULL, 0);
	set_owner(cm, /* client_id */ 0xB2, /* owner_id */ 20);
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
	 * collision gate fires BEFORE the older displaced-counting code
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
/* Partial-stripe write instrumentation                                 */
/*                                                                     */
/* Quantifies the block and fragmentation shape observed by data servers. */
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
	set_owner(cm, /* client_id */ 0xA1, /* owner_id */ 10);
	nfs4_op_chunk_write(cm->compound);
	free_write_res(cm);
	cm_reset_slot(cm, 0);

	/*
	 * PS-B write to same offset.  The chunk-store
	 * rejects cross-owner overwrite of a PENDING block: PS-B gets
	 * NFS4ERR_DELAY.  The cs_blocks_overwrite counter is therefore
	 * NOT incremented (the metadata-recording loop runs only on
	 * the accept path), and cs_chunk_busy_delay records the
	 * rejection instead.
	 */
	set_write_args(cm, buf, CHUNK_SZ, CHUNK_SZ, /* offset */ 0, NULL, 0);
	set_owner(cm, /* client_id */ 0xB2, /* owner_id */ 20);
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
	/* The collision gate fired; this is the contention signal. */
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
/* S1.5(A): non-CHUNK operations on a chunked data file                */
/* ------------------------------------------------------------------ */

/*
 * These drive dispatch_compound() rather than an op handler directly,
 * because the gate lives in the dispatch loop and a handler call would
 * walk straight past it.
 *
 * OP_ACCESS is the probe for the three state cases below.  It is not on
 * the draft's list of operations permitted against a data file, so the
 * gate covers it -- and unlike READ or SETATTR its handler is safe to
 * actually run on this mock (it only consults the inode's mode bits),
 * so the "gate stays silent" cases can be told apart from the "gate
 * fires" case by the status alone.
 */
static nfsstat4 dispatch_one(struct cm_ctx *cm, nfs_opnum4 opnum)
{
	cm_set_op(cm, 0, opnum);
	dispatch_compound(cm->compound);
	return cm->compound->c_res->status;
}

/*
 * The rule itself: an operation the draft does not permit against a
 * data file is refused once the file is identified as chunked.
 */
START_TEST(test_non_chunk_op_rejected_on_chunked_file)
{
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);
	mark_chunked(g_inode, INODE_CHUNKED_YES);

	ck_assert_int_eq(dispatch_one(cm, OP_ACCESS), NFS4ERR_NOTSUPP);

	cm_free(cm);
}
END_TEST

/*
 * UNIDENTIFIED is not a YES.  A data server that cannot classify the
 * file must not start refusing ordinary NFSv4 traffic on it -- every
 * file on a data server that never had attribute 90 settled would stop
 * working.  The handler runs and answers for itself.
 */
START_TEST(test_non_chunk_op_allowed_when_unidentified)
{
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);
	mark_chunked(g_inode, INODE_CHUNKED_UNIDENTIFIED);

	ck_assert_int_ne(dispatch_one(cm, OP_ACCESS), NFS4ERR_NOTSUPP);

	cm_free(cm);
}
END_TEST

/*
 * Identified non-chunked is likewise not a YES -- that direction is
 * S1.5(B)'s business, and this gate must leave it alone.
 */
START_TEST(test_non_chunk_op_allowed_when_non_chunked)
{
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);
	mark_chunked(g_inode, INODE_CHUNKED_NO);

	ck_assert_int_ne(dispatch_one(cm, OP_ACCESS), NFS4ERR_NOTSUPP);

	cm_free(cm);
}
END_TEST

/*
 * The restriction is on what a client sends.  The metadata server
 * reaches the same file over its control session to do the things the
 * draft assigns to it, so the gate must not close on it.
 */
START_TEST(test_non_chunk_op_allowed_on_control_session)
{
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);
	mark_chunked(g_inode, INODE_CHUNKED_YES);
	cm->compound->c_nfs4_client->nc_exchgid_flags =
		EXCHGID4_FLAG_USE_PNFS_MDS;

	ck_assert_int_ne(dispatch_one(cm, OP_ACCESS), NFS4ERR_NOTSUPP);

	cm_free(cm);
}
END_TEST

/*
 * The operations the draft names explicitly.  Each is refused before
 * its handler runs, which is also why running them against this mock
 * is safe -- none of them is ever entered.
 */
START_TEST(test_named_non_chunk_ops_rejected_on_chunked_file)
{
	static const nfs_opnum4 ops[] = {
		OP_READ,      OP_WRITE,	       OP_COMMIT,   OP_SETATTR,
		OP_READ_PLUS, OP_SEEK,	       OP_ALLOCATE, OP_DEALLOCATE,
		OP_OPEN,      OP_CLOSE,	       OP_LOCK,	    OP_LOCKU,
		OP_LAYOUTGET, OP_LAYOUTRETURN, OP_CLONE,    OP_COPY,
	};

	/*
	 * One compound reused across the list rather than one per op:
	 * cm_alloc registers an nfs4_client, and sixteen of them in a
	 * single test exhausts the client table.
	 */
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);
	mark_chunked(g_inode, INODE_CHUNKED_YES);

	for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
		cm->compound->c_res->status = NFS4_OK;
		ck_assert_int_eq(dispatch_one(cm, ops[i]), NFS4ERR_NOTSUPP);
		cm_reset_slot(cm, 0);
	}

	cm_free(cm);
}
END_TEST

/*
 * GETFH is filehandle plumbing, not an operation on the file, and a
 * client legitimately issues it after PUTFH on a data file.  Guards the
 * allowlist against being narrowed to the CHUNK family alone.
 */
START_TEST(test_filehandle_plumbing_allowed_on_chunked_file)
{
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);
	mark_chunked(g_inode, INODE_CHUNKED_YES);

	ck_assert_int_ne(dispatch_one(cm, OP_GETFH), NFS4ERR_NOTSUPP);

	/* GETFH ran for real, so its result owns a copy of the handle. */
	free(cm->compound->c_res->resarray.resarray_val[0]
		     .nfs_resop4_u.opgetfh.GETFH4res_u.resok4.object
		     .nfs_fh4_val);

	cm_free(cm);
}
END_TEST

/*
 * The regression that matters: the whole point of a chunked data file
 * is that CHUNK operations work on it.  A full CHUNK_WRITE through
 * dispatch, not through the handler, so the gate is in the path.
 */
START_TEST(test_chunk_write_still_works_through_dispatch)
{
	struct cm_ctx *cm = cm_alloc(1);
	char buf[CHUNK_SZ];

	cm_set_inode(cm, g_inode);
	mark_chunked(g_inode, INODE_CHUNKED_YES);

	memset(buf, 'D', sizeof(buf));
	set_write_args(cm, buf, CHUNK_SZ, CHUNK_SZ, 0, NULL, 0);
	cm_set_op(cm, 0, OP_CHUNK_WRITE);

	dispatch_compound(cm->compound);

	ck_assert_int_eq(cm->compound->c_res->status, NFS4_OK);

	free_write_res(cm);
	cm_free(cm);
}
END_TEST

START_TEST(test_chunk_lock_and_unlock_empty_range)
{
	struct cm_ctx *cm = cm_alloc(1);
	struct chunk_block *blk;

	cm_set_inode(cm, g_inode);
	mark_chunked(g_inode, INODE_CHUNKED_YES);
	set_chunk_lock_args(cm, 4, 2, 0x1234, 0xBEEF, 7, 0, false);
	nfs4_op_chunk_lock(cm->compound);
	ck_assert_int_eq(cm->compound->c_res->resarray.resarray_val[0]
				 .nfs_resop4_u.opchunk_lock.clr_status,
			 NFS4_OK);

	blk = chunk_store_lookup_any(g_inode->i_chunk_store, 4);
	ck_assert_ptr_nonnull(blk);
	ck_assert_int_eq(blk->cb_state, CHUNK_STATE_EMPTY);
	ck_assert_msg(blk->cb_flags & CHUNK_BLOCK_LOCKED,
		      "empty chunks retain their persisted lock");
	blk = chunk_store_lookup_any(g_inode->i_chunk_store, 5);
	ck_assert_ptr_nonnull(blk);
	ck_assert_msg(blk->cb_flags & CHUNK_BLOCK_LOCKED,
		      "every chunk in the range is locked");

	cm_reset_slot(cm, 0);
	set_chunk_unlock_args(cm, 4, 2, 0x1234, 0xBEEF, 7);
	nfs4_op_chunk_unlock(cm->compound);
	ck_assert_int_eq(cm->compound->c_res->resarray.resarray_val[0]
				 .nfs_resop4_u.opchunk_unlock.cur_status,
			 NFS4_OK);
	ck_assert_int_eq(
		chunk_store_lookup_any(g_inode->i_chunk_store, 4)->cb_flags &
			CHUNK_BLOCK_LOCKED,
		0);

	cm_free(cm);
}
END_TEST

START_TEST(test_chunk_lock_conflict_reports_holder)
{
	struct cm_ctx *cm = cm_alloc(1);
	CHUNK_LOCK4res *res;

	cm_set_inode(cm, g_inode);
	mark_chunked(g_inode, INODE_CHUNKED_YES);
	set_chunk_lock_args(cm, 2, 1, 0x100, 0xBEEF, 1, 0, false);
	nfs4_op_chunk_lock(cm->compound);
	ck_assert_int_eq(cm->compound->c_res->resarray.resarray_val[0]
				 .nfs_resop4_u.opchunk_lock.clr_status,
			 NFS4_OK);

	{
		char buf[CHUNK_SZ];

		memset(buf, 'L', sizeof(buf));
		cm_reset_slot(cm, 0);
		set_write_args(cm, buf, CHUNK_SZ, CHUNK_SZ, 2, NULL, 0);
		nfs4_op_chunk_write(cm->compound);
		ck_assert_int_eq(cm->compound->c_res->resarray.resarray_val[0]
					 .nfs_resop4_u.opchunk_write.cwr_status,
				 NFS4ERR_CHUNK_LOCKED);
		free_write_args(cm);
		free_write_res(cm);
	}

	cm_reset_slot(cm, 0);
	set_chunk_lock_args(cm, 2, 1, 0x200, 0xBEEF, 2, 0, false);
	nfs4_op_chunk_lock(cm->compound);
	res = &cm->compound->c_res->resarray.resarray_val[0]
		       .nfs_resop4_u.opchunk_lock;
	ck_assert_int_eq(res->clr_status, NFS4ERR_CHUNK_LOCKED);
	ck_assert_uint_eq(res->CHUNK_LOCK4res_u.clr_owner.co_cohort_id, 0x100);
	ck_assert_uint_eq(res->CHUNK_LOCK4res_u.clr_owner.co_id, 1);

	cm_free(cm);
}
END_TEST

START_TEST(test_chunk_lock_takeover_is_not_supported)
{
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);
	mark_chunked(g_inode, INODE_CHUNKED_YES);
	set_chunk_lock_args(cm, 0, 1, 0x100, 0xBEEF, 1,
			    CHUNK_LOCK_FLAGS_TAKEOVER, false);
	nfs4_op_chunk_lock(cm->compound);
	ck_assert_int_eq(cm->compound->c_res->resarray.resarray_val[0]
				 .nfs_resop4_u.opchunk_lock.clr_status,
			 NFS4ERR_NOTSUPP);

	cm_free(cm);
}
END_TEST

START_TEST(test_chunk_mds_epoch_persistence_roundtrip)
{
	struct server_state *ss = server_state_find();
	struct chunk_mds_epoch saved = {
		.epoch = 41,
		.expires_at_ns = 987654321,
		.issuer_clientid = 0x1122334455667788ULL,
	};
	struct chunk_mds_epoch loaded = { 0 };

	ck_assert_ptr_nonnull(ss);
	ck_assert_ptr_nonnull(ss->ss_state_dir);
	ck_assert_int_eq(chunk_mds_epoch_persist(ss->ss_state_dir, &saved), 0);
	ck_assert_int_eq(chunk_mds_epoch_load(ss->ss_state_dir, &loaded), 0);
	ck_assert_uint_eq(loaded.epoch, saved.epoch);
	ck_assert_uint_eq(loaded.expires_at_ns, saved.expires_at_ns);
	ck_assert_uint_eq(loaded.issuer_clientid, saved.issuer_clientid);

	server_state_put(ss);
}
END_TEST

START_TEST(test_chunk_escrow_install_and_release)
{
	static const escrow_id4 escrow = {
		0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
		0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
	};
	struct server_state *ss = server_state_find();
	struct chunk_mds_epoch epoch = {
		.epoch = 77,
		.expires_at_ns = reffs_now_ns() + 60000000000ULL,
	};
	struct cm_ctx *cm = cm_alloc(1);
	struct chunk_block *blk;

	ck_assert_ptr_nonnull(ss);
	ck_assert_int_eq(chunk_mds_epoch_persist(ss->ss_state_dir, &epoch), 0);
	server_state_put(ss);

	cm_set_inode(cm, g_inode);
	mark_chunked(g_inode, INODE_CHUNKED_YES);
	cm->compound->c_nfs4_client->nc_exchgid_flags =
		EXCHGID4_FLAG_USE_PNFS_MDS;
	ss = server_state_find();
	ck_assert_ptr_nonnull(ss);
	atomic_store(&ss->ss_test_chunk_persist_fail_count, 1);
	server_state_put(ss);
	set_chunk_escrow_install_args(cm, epoch.epoch, 6, 2, escrow);
	nfs4_op_chunk_escrow_install(cm->compound);
	ck_assert_int_eq(
		cm->compound->c_res->resarray.resarray_val[0]
			.nfs_resop4_u.opchunk_escrow_install.ceir_status,
		NFS4ERR_SERVERFAULT);
	blk = chunk_store_lookup_any(g_inode->i_chunk_store, 6);
	ck_assert_ptr_nonnull(blk);
	ck_assert_int_eq(blk->cb_flags & CHUNK_BLOCK_ESCROW, 0);

	cm_reset_slot(cm, 0);
	set_chunk_escrow_install_args(cm, epoch.epoch, 6, 2, escrow);
	nfs4_op_chunk_escrow_install(cm->compound);
	ck_assert_int_eq(
		cm->compound->c_res->resarray.resarray_val[0]
			.nfs_resop4_u.opchunk_escrow_install.ceir_status,
		NFS4_OK);
	blk = chunk_store_lookup_any(g_inode->i_chunk_store, 6);
	ck_assert_ptr_nonnull(blk);
	ck_assert_msg((blk->cb_flags &
		       (CHUNK_BLOCK_LOCKED | CHUNK_BLOCK_ESCROW)) ==
			      (CHUNK_BLOCK_LOCKED | CHUNK_BLOCK_ESCROW),
		      "install records an MDS escrow lock");
	ck_assert_int_eq(blk->cb_lock_client_id, CHUNK_GUARD_CLIENT_ID_MDS);
	ck_assert_mem_eq(blk->cb_lock_escrow_id, escrow, sizeof(escrow));
	ck_assert_uint_eq(g_inode->i_chunk_store->cs_nescrows, 1);
	ck_assert_uint_eq(g_inode->i_chunk_store->cs_escrows[0].cer_offset, 6);
	ck_assert_uint_eq(g_inode->i_chunk_store->cs_escrows[0].cer_count, 2);
	ck_assert_mem_eq(g_inode->i_chunk_store->cs_escrows[0].cer_id, escrow,
			 sizeof(escrow));
	blk = chunk_store_lookup_any(g_inode->i_chunk_store, 7);
	ck_assert_ptr_nonnull(blk);
	ck_assert_uint_eq(blk->cb_flags,
			  CHUNK_BLOCK_LOCKED | CHUNK_BLOCK_ESCROW);
	ck_assert_uint_eq(blk->cb_lock_offset, 6);
	ck_assert_uint_eq(blk->cb_lock_count, 2);
	ck_assert_uint_eq(blk->cb_lock_client_id, CHUNK_GUARD_CLIENT_ID_MDS);
	ck_assert_mem_eq(blk->cb_lock_escrow_id, escrow, sizeof(escrow));

	cm_reset_slot(cm, 0);
	set_chunk_escrow_release_args(cm, epoch.epoch, 6, 2, escrow);
	ss = server_state_find();
	ck_assert_ptr_nonnull(ss);
	atomic_store(&ss->ss_test_chunk_persist_fail_count, 1);
	server_state_put(ss);
	nfs4_op_chunk_escrow_release(cm->compound);
	ck_assert_int_eq(
		cm->compound->c_res->resarray.resarray_val[0]
			.nfs_resop4_u.opchunk_escrow_release.cerr_status,
		NFS4ERR_SERVERFAULT);
	blk = chunk_store_lookup_any(g_inode->i_chunk_store, 6);
	ck_assert_int_eq(blk->cb_flags & CHUNK_BLOCK_ESCROW,
			 CHUNK_BLOCK_ESCROW);

	cm_reset_slot(cm, 0);
	set_chunk_escrow_release_args(cm, epoch.epoch, 6, 2, escrow);
	nfs4_op_chunk_escrow_release(cm->compound);
	ck_assert_int_eq(
		cm->compound->c_res->resarray.resarray_val[0]
			.nfs_resop4_u.opchunk_escrow_release.cerr_status,
		NFS4_OK);
	ck_assert_int_eq(
		chunk_store_lookup_any(g_inode->i_chunk_store, 6)->cb_flags &
			CHUNK_BLOCK_LOCKED,
		0);
	ck_assert_uint_eq(g_inode->i_chunk_store->cs_nescrows, 0);

	cm_free(cm);
}
END_TEST

START_TEST(test_chunk_escrow_release_rejects_stale_id)
{
	static const escrow_id4 escrow = {
		0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28,
		0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30,
	};
	static const escrow_id4 wrong = {
		0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
		0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40,
	};
	struct server_state *ss = server_state_find();
	struct chunk_mds_epoch epoch = {
		.epoch = 78,
		.expires_at_ns = reffs_now_ns() + 60000000000ULL,
	};
	struct cm_ctx *cm = cm_alloc(1);
	struct chunk_block *blk;

	ck_assert_ptr_nonnull(ss);
	ck_assert_int_eq(chunk_mds_epoch_persist(ss->ss_state_dir, &epoch), 0);
	server_state_put(ss);
	cm_set_inode(cm, g_inode);
	mark_chunked(g_inode, INODE_CHUNKED_YES);
	cm->compound->c_nfs4_client->nc_exchgid_flags =
		EXCHGID4_FLAG_USE_PNFS_MDS;
	set_chunk_escrow_install_args(cm, epoch.epoch, 8, 1, escrow);
	nfs4_op_chunk_escrow_install(cm->compound);
	ck_assert_int_eq(
		cm->compound->c_res->resarray.resarray_val[0]
			.nfs_resop4_u.opchunk_escrow_install.ceir_status,
		NFS4_OK);

	cm_reset_slot(cm, 0);
	set_chunk_escrow_release_args(cm, epoch.epoch, 8, 1, wrong);
	nfs4_op_chunk_escrow_release(cm->compound);
	ck_assert_int_eq(
		cm->compound->c_res->resarray.resarray_val[0]
			.nfs_resop4_u.opchunk_escrow_release.cerr_status,
		NFS4ERR_STALE_ESCROW);
	blk = chunk_store_lookup_any(g_inode->i_chunk_store, 8);
	ck_assert_ptr_nonnull(blk);
	ck_assert_msg(blk->cb_flags & CHUNK_BLOCK_ESCROW,
		      "stale release leaves escrow installed");
	ck_assert_mem_eq(blk->cb_lock_escrow_id, escrow, sizeof(escrow));

	cm_free(cm);
}
END_TEST

START_TEST(test_chunk_lock_adopts_escrow)
{
	static const escrow_id4 escrow = {
		0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
		0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f, 0x50,
	};
	struct server_state *ss = server_state_find();
	struct chunk_mds_epoch epoch = {
		.epoch = 81,
		.expires_at_ns = reffs_now_ns() + 60000000000ULL,
	};
	struct cm_ctx *cm = cm_alloc(1);
	struct chunk_block *blk;
	CHUNK_LOCK4args *lock_args;

	ck_assert_ptr_nonnull(ss);
	ck_assert_int_eq(chunk_mds_epoch_persist(ss->ss_state_dir, &epoch), 0);
	server_state_put(ss);
	cm_set_inode(cm, g_inode);
	mark_chunked(g_inode, INODE_CHUNKED_YES);
	cm->compound->c_nfs4_client->nc_exchgid_flags =
		EXCHGID4_FLAG_USE_PNFS_MDS;
	set_chunk_escrow_install_args(cm, epoch.epoch, 10, 1, escrow);
	nfs4_op_chunk_escrow_install(cm->compound);
	ck_assert_int_eq(
		cm->compound->c_res->resarray.resarray_val[0]
			.nfs_resop4_u.opchunk_escrow_install.ceir_status,
		NFS4_OK);

	cm_reset_slot(cm, 0);
	set_chunk_lock_args(cm, 10, 1, 0x200, 0xBEEF, 9, CHUNK_LOCK_FLAGS_ADOPT,
			    true);
	lock_args = &cm->compound->c_args->argarray.argarray_val[0]
			     .nfs_argop4_u.opchunk_lock;
	memcpy(lock_args->cla_adopt.chunk_lock_adopt4_u.cla_escrow_id, escrow,
	       sizeof(escrow));
	ss = server_state_find();
	ck_assert_ptr_nonnull(ss);
	atomic_store(&ss->ss_test_chunk_persist_fail_count, 1);
	server_state_put(ss);
	nfs4_op_chunk_lock(cm->compound);
	ck_assert_int_eq(cm->compound->c_res->resarray.resarray_val[0]
				 .nfs_resop4_u.opchunk_lock.clr_status,
			 NFS4ERR_SERVERFAULT);
	blk = chunk_store_lookup_any(g_inode->i_chunk_store, 10);
	ck_assert_int_eq(blk->cb_lock_client_id, CHUNK_GUARD_CLIENT_ID_MDS);

	cm_reset_slot(cm, 0);
	set_chunk_lock_args(cm, 10, 1, 0x200, 0xBEEF, 9, CHUNK_LOCK_FLAGS_ADOPT,
			    true);
	lock_args = &cm->compound->c_args->argarray.argarray_val[0]
			     .nfs_argop4_u.opchunk_lock;
	memcpy(lock_args->cla_adopt.chunk_lock_adopt4_u.cla_escrow_id, escrow,
	       sizeof(escrow));
	nfs4_op_chunk_lock(cm->compound);
	ck_assert_int_eq(cm->compound->c_res->resarray.resarray_val[0]
				 .nfs_resop4_u.opchunk_lock.clr_status,
			 NFS4_OK);
	blk = chunk_store_lookup_any(g_inode->i_chunk_store, 10);
	ck_assert_ptr_nonnull(blk);
	ck_assert_int_eq(blk->cb_lock_client_id, 0xBEEF);
	ck_assert_int_eq(blk->cb_lock_owner_id, 9);
	ck_assert_int_eq(blk->cb_flags & CHUNK_BLOCK_ESCROW, 0);
	ck_assert_mem_eq(blk->cb_lock_escrow_id, escrow, sizeof(escrow));

	/* Reload after ADOPT: client custody survives, MDS escrow does not. */
	pthread_mutex_lock(&g_inode->i_attr_mutex);
	struct chunk_store *old_store = g_inode->i_chunk_store;
	g_inode->i_chunk_store = NULL;
	chunk_store_destroy(old_store);
	ss = server_state_find();
	ck_assert_ptr_nonnull(ss);
	struct chunk_store *reloaded =
		chunk_store_load(ss->ss_state_dir, g_inode->i_ino);
	server_state_put(ss);
	ck_assert_ptr_nonnull(reloaded);
	g_inode->i_chunk_store = reloaded;
	pthread_mutex_unlock(&g_inode->i_attr_mutex);
	blk = chunk_store_lookup_any(g_inode->i_chunk_store, 10);
	ck_assert_int_eq(blk->cb_lock_client_id, 0xBEEF);
	ck_assert_int_eq(blk->cb_lock_owner_id, 9);
	ck_assert_int_eq(blk->cb_flags & CHUNK_BLOCK_ESCROW, 0);
	ck_assert_uint_eq(g_inode->i_chunk_store->cs_nescrows, 0);

	cm_reset_slot(cm, 0);
	set_chunk_escrow_release_args(cm, epoch.epoch, 10, 1, escrow);
	nfs4_op_chunk_escrow_release(cm->compound);
	ck_assert_int_eq(
		cm->compound->c_res->resarray.resarray_val[0]
			.nfs_resop4_u.opchunk_escrow_release.cerr_status,
		NFS4ERR_STALE_ESCROW);
	blk = chunk_store_lookup_any(g_inode->i_chunk_store, 10);
	ck_assert_int_eq(blk->cb_flags & CHUNK_BLOCK_LOCKED,
			 CHUNK_BLOCK_LOCKED);
	ck_assert_int_eq(blk->cb_lock_owner_id, 9);

	cm_reset_slot(cm, 0);
	set_chunk_unlock_args(cm, 10, 1, 0x200, 0xBEEF, 9);
	nfs4_op_chunk_unlock(cm->compound);
	ck_assert_int_eq(cm->compound->c_res->resarray.resarray_val[0]
				 .nfs_resop4_u.opchunk_unlock.cur_status,
			 NFS4_OK);
	blk = chunk_store_lookup_any(g_inode->i_chunk_store, 10);
	ck_assert_int_eq(blk->cb_flags & CHUNK_BLOCK_LOCKED, 0);
	ck_assert_mem_eq(blk->cb_lock_escrow_id, escrow, sizeof(escrow));

	cm_free(cm);
}
END_TEST

START_TEST(test_chunk_escrow_survives_writer_expiry_cleanup)
{
	struct server_state *ss = server_state_find();
	struct chunk_block escrowed = {
		.cb_state = CHUNK_STATE_FINALIZED,
		.cb_flags = CHUNK_BLOCK_LOCKED | CHUNK_BLOCK_ESCROW,
		.cb_writer_clientid = 0xfeed,
	};
	struct chunk_block adopted = {
		.cb_state = CHUNK_STATE_FINALIZED,
		.cb_flags = CHUNK_BLOCK_LOCKED,
		.cb_lock_client_id = 0xBEEF,
		.cb_writer_clientid = 0xfeed,
		.cb_lock_escrow_id = { 0x51 },
	};
	struct chunk_store *cs;
	struct cm_ctx *cm = cm_alloc(1);

	ck_assert_ptr_nonnull(ss);
	cm_set_inode(cm, g_inode);
	mark_chunked(g_inode, INODE_CHUNKED_YES);
	cs = chunk_store_get(g_inode, ss->ss_state_dir);
	ck_assert_ptr_nonnull(cs);
	ck_assert_int_eq(chunk_store_write(cs, 12, &escrowed), 0);
	ck_assert_int_eq(chunk_store_write(cs, 13, &adopted), 0);
	ck_assert_uint_eq(chunk_store_rollback_for_client(cs, 0xfeed), 1);
	ck_assert_int_eq(chunk_store_lookup_any(cs, 12)->cb_state,
			 CHUNK_STATE_FINALIZED);
	ck_assert_msg(chunk_store_lookup_any(cs, 12)->cb_flags &
			      CHUNK_BLOCK_ESCROW,
		      "expiry cleanup must not release an escrowed payload");
	ck_assert_msg(chunk_store_lookup_any(cs, 13)->cb_flags &
			      CHUNK_BLOCK_ESCROW,
		      "expiry cleanup must return adopted custody to the MDS");
	ck_assert_int_eq(chunk_store_lookup_any(cs, 13)->cb_lock_client_id,
			 CHUNK_GUARD_CLIENT_ID_MDS);
	cm_free(cm);
	server_state_put(ss);
}
END_TEST

START_TEST(test_chunk_escrow_requires_mds_session)
{
	static const escrow_id4 escrow = { 1 };
	struct server_state *ss = server_state_find();
	struct chunk_mds_epoch epoch = {
		.epoch = 79,
		.expires_at_ns = reffs_now_ns() + 60000000000ULL,
	};
	struct cm_ctx *cm = cm_alloc(1);

	ck_assert_ptr_nonnull(ss);
	ck_assert_int_eq(chunk_mds_epoch_persist(ss->ss_state_dir, &epoch), 0);
	server_state_put(ss);
	cm_set_inode(cm, g_inode);
	mark_chunked(g_inode, INODE_CHUNKED_YES);
	set_chunk_escrow_install_args(cm, epoch.epoch, 0, 1, escrow);
	nfs4_op_chunk_escrow_install(cm->compound);
	ck_assert_int_eq(
		cm->compound->c_res->resarray.resarray_val[0]
			.nfs_resop4_u.opchunk_escrow_install.ceir_status,
		NFS4ERR_PERM);

	cm_free(cm);
}
END_TEST

START_TEST(test_chunk_escrow_enumerate_probe)
{
	static const escrow_id4 escrow_a = { 0x81 };
	static const escrow_id4 escrow_b = { 0x82 };
	struct server_state *ss = server_state_find();
	struct chunk_mds_epoch epoch = {
		.epoch = 80,
		.expires_at_ns = reffs_now_ns() + 60000000000ULL,
	};
	struct cm_ctx *cm = cm_alloc(1);
	CHUNK_ESCROW_ENUMERATE4res *res;

	ck_assert_ptr_nonnull(ss);
	ck_assert_int_eq(chunk_mds_epoch_persist(ss->ss_state_dir, &epoch), 0);
	server_state_put(ss);
	cm_set_inode(cm, g_inode);
	mark_chunked(g_inode, INODE_CHUNKED_YES);
	cm->compound->c_nfs4_client->nc_exchgid_flags =
		EXCHGID4_FLAG_USE_PNFS_MDS;
	set_chunk_escrow_enumerate_args(cm, epoch.epoch, 0, 1, 0);
	nfs4_op_chunk_escrow_enumerate(cm->compound);
	res = &cm->compound->c_res->resarray.resarray_val[0]
		       .nfs_resop4_u.opchunk_escrow_enumerate;
	ck_assert_int_eq(res->ceer_status, NFS4_OK);
	ck_assert(res->CHUNK_ESCROW_ENUMERATE4res_u.ceer_resok4.ceer_eof);
	ck_assert_uint_eq(res->CHUNK_ESCROW_ENUMERATE4res_u.ceer_resok4
				  .ceer_entries.ceer_entries_len,
			  0);

	/* The probe is followed by two real ranges to exercise paging. */
	cm_reset_slot(cm, 0);
	set_chunk_escrow_install_args(cm, epoch.epoch, 0, 1, escrow_a);
	nfs4_op_chunk_escrow_install(cm->compound);
	ck_assert_int_eq(
		cm->compound->c_res->resarray.resarray_val[0]
			.nfs_resop4_u.opchunk_escrow_install.ceir_status,
		NFS4_OK);
	cm_reset_slot(cm, 0);
	set_chunk_escrow_install_args(cm, epoch.epoch, 4, 1, escrow_b);
	nfs4_op_chunk_escrow_install(cm->compound);
	ck_assert_int_eq(
		cm->compound->c_res->resarray.resarray_val[0]
			.nfs_resop4_u.opchunk_escrow_install.ceir_status,
		NFS4_OK);

	cm_reset_slot(cm, 0);
	set_chunk_escrow_enumerate_args(cm, epoch.epoch, 0, 5, 1);
	nfs4_op_chunk_escrow_enumerate(cm->compound);
	res = &cm->compound->c_res->resarray.resarray_val[0]
		       .nfs_resop4_u.opchunk_escrow_enumerate;
	ck_assert_int_eq(res->ceer_status, NFS4_OK);
	CHUNK_ESCROW_ENUMERATE4resok *resok =
		&res->CHUNK_ESCROW_ENUMERATE4res_u.ceer_resok4;
	ck_assert_uint_eq(resok->ceer_entries.ceer_entries_len, 1);
	ck_assert_uint_eq(resok->ceer_entries.ceer_entries_val[0].eee_offset,
			  0);
	ck_assert_mem_eq(resok->ceer_entries.ceer_entries_val[0].eee_escrow_id,
			 escrow_a, sizeof(escrow_a));
	ck_assert(!resok->ceer_eof);
	ck_assert_uint_eq(resok->ceer_cookie.ceer_cookie_len, 16);
	char cookie[CHUNK_ESCROW_ENUMERATE_COOKIE_MAX4];
	memcpy(cookie, resok->ceer_cookie.ceer_cookie_val,
	       resok->ceer_cookie.ceer_cookie_len);
	free(resok->ceer_entries.ceer_entries_val);
	free(resok->ceer_cookie.ceer_cookie_val);

	cm_reset_slot(cm, 0);
	set_chunk_escrow_enumerate_args(cm, epoch.epoch, 0, 5, 1);
	CHUNK_ESCROW_ENUMERATE4args *enum_args =
		&cm->compound->c_args->argarray.argarray_val[0]
			 .nfs_argop4_u.opchunk_escrow_enumerate;
	enum_args->ceea_cookie.ceea_cookie_len = 16;
	enum_args->ceea_cookie.ceea_cookie_val = cookie;
	nfs4_op_chunk_escrow_enumerate(cm->compound);
	res = &cm->compound->c_res->resarray.resarray_val[0]
		       .nfs_resop4_u.opchunk_escrow_enumerate;
	ck_assert_int_eq(res->ceer_status, NFS4_OK);
	resok = &res->CHUNK_ESCROW_ENUMERATE4res_u.ceer_resok4;
	ck_assert_uint_eq(resok->ceer_entries.ceer_entries_len, 1);
	ck_assert_uint_eq(resok->ceer_entries.ceer_entries_val[0].eee_offset,
			  4);
	ck_assert_mem_eq(resok->ceer_entries.ceer_entries_val[0].eee_escrow_id,
			 escrow_b, sizeof(escrow_b));
	ck_assert(resok->ceer_eof);
	free(resok->ceer_entries.ceer_entries_val);

	cm_reset_slot(cm, 0);
	set_chunk_escrow_enumerate_args(cm, epoch.epoch, 0, 5, 1);
	enum_args = &cm->compound->c_args->argarray.argarray_val[0]
			     .nfs_argop4_u.opchunk_escrow_enumerate;
	enum_args->ceea_cookie.ceea_cookie_len = 1;
	enum_args->ceea_cookie.ceea_cookie_val = cookie;
	nfs4_op_chunk_escrow_enumerate(cm->compound);
	res = &cm->compound->c_res->resarray.resarray_val[0]
		       .nfs_resop4_u.opchunk_escrow_enumerate;
	ck_assert_int_eq(res->ceer_status, NFS4ERR_BAD_COOKIE);

	cm_free(cm);
}
END_TEST

START_TEST(test_chunk_escrow_rejects_stale_epoch)
{
	static const escrow_id4 escrow = { 0x61 };
	struct server_state *ss = server_state_find();
	struct chunk_mds_epoch epoch = {
		.epoch = 90,
		.expires_at_ns = reffs_now_ns() + 60000000000ULL,
	};
	struct cm_ctx *cm = cm_alloc(1);

	ck_assert_ptr_nonnull(ss);
	ck_assert_int_eq(chunk_mds_epoch_persist(ss->ss_state_dir, &epoch), 0);
	server_state_put(ss);
	cm_set_inode(cm, g_inode);
	mark_chunked(g_inode, INODE_CHUNKED_YES);
	cm->compound->c_nfs4_client->nc_exchgid_flags =
		EXCHGID4_FLAG_USE_PNFS_MDS;
	set_chunk_escrow_install_args(cm, epoch.epoch + 1, 0, 1, escrow);
	nfs4_op_chunk_escrow_install(cm->compound);
	ck_assert_int_eq(
		cm->compound->c_res->resarray.resarray_val[0]
			.nfs_resop4_u.opchunk_escrow_install.ceir_status,
		NFS4ERR_STALE_MDS_EPOCH);

	epoch.epoch++;
	epoch.expires_at_ns = reffs_now_ns() - 1;
	ss = server_state_find();
	ck_assert_ptr_nonnull(ss);
	ck_assert_int_eq(chunk_mds_epoch_persist(ss->ss_state_dir, &epoch), 0);
	server_state_put(ss);
	cm_reset_slot(cm, 0);
	set_chunk_escrow_enumerate_args(cm, epoch.epoch, 0, 1, 0);
	nfs4_op_chunk_escrow_enumerate(cm->compound);
	ck_assert_int_eq(
		cm->compound->c_res->resarray.resarray_val[0]
			.nfs_resop4_u.opchunk_escrow_enumerate.ceer_status,
		NFS4ERR_STALE_MDS_EPOCH);

	cm_free(cm);
}
END_TEST

START_TEST(test_chunk_escrow_takeover_remains_disabled)
{
	struct server_state *ss = server_state_find();
	struct chunk_mds_epoch before = {
		.epoch = 7,
		.expires_at_ns = reffs_now_ns() + 60000000000ULL,
		.issuer_clientid = 0x1234,
	};
	struct chunk_mds_epoch after;
	struct cm_ctx *cm = cm_alloc(1);
	CHUNK_ESCROW_TAKEOVER4args *args;
	CHUNK_ESCROW_TAKEOVER4res *res;
	struct chunk_store *cs;

	ck_assert_ptr_nonnull(ss);
	ck_assert_int_eq(chunk_mds_epoch_persist(ss->ss_state_dir, &before), 0);
	server_state_put(ss);

	cm_set_inode(cm, g_inode);
	mark_chunked(g_inode, INODE_CHUNKED_YES);
	cm->compound->c_nfs4_client->nc_exchgid_flags =
		EXCHGID4_FLAG_USE_PNFS_MDS;
	cm_set_op(cm, 0, OP_CHUNK_ESCROW_TAKEOVER);
	args = &cm->compound->c_args->argarray.argarray_val[0]
			.nfs_argop4_u.opchunk_escrow_takeover;
	args->ceta_expected_prior_epoch = 1;
	args->ceta_new_epoch = 2;
	args->ceta_proof_profile = PROOF_PROFILE_HA_AUTHORITY_ED25519;
	res = &cm->compound->c_res->resarray.resarray_val[0]
		       .nfs_resop4_u.opchunk_escrow_takeover;
	nfs4_op_chunk_escrow_takeover(cm->compound);
	ck_assert_int_eq(res->cetar_status, NFS4ERR_NOTSUPP);
	cs = g_inode->i_chunk_store;
	ck_assert_ptr_null(cs);
	ck_assert_int_eq(
		chunk_mds_epoch_load(cm->compound->c_server_state->ss_state_dir,
				     &after),
		0);
	ck_assert_mem_eq(&after, &before, sizeof(after));

	cm_free(cm);
}
END_TEST

START_TEST(test_chunk_escrow_install_conflict_is_all_or_nothing)
{
	static const escrow_id4 escrow = { 0x71 };
	struct server_state *ss = server_state_find();
	struct chunk_mds_epoch epoch = {
		.epoch = 91,
		.expires_at_ns = reffs_now_ns() + 60000000000ULL,
	};
	struct cm_ctx *cm = cm_alloc(1);
	struct chunk_block *blk;

	ck_assert_ptr_nonnull(ss);
	ck_assert_int_eq(chunk_mds_epoch_persist(ss->ss_state_dir, &epoch), 0);
	server_state_put(ss);
	cm_set_inode(cm, g_inode);
	mark_chunked(g_inode, INODE_CHUNKED_YES);
	set_chunk_lock_args(cm, 20, 1, 0x300, 0xBEEF, 21, 0, false);
	nfs4_op_chunk_lock(cm->compound);
	ck_assert_int_eq(cm->compound->c_res->resarray.resarray_val[0]
				 .nfs_resop4_u.opchunk_lock.clr_status,
			 NFS4_OK);

	cm->compound->c_nfs4_client->nc_exchgid_flags =
		EXCHGID4_FLAG_USE_PNFS_MDS;
	cm_reset_slot(cm, 0);
	set_chunk_escrow_install_args(cm, epoch.epoch, 20, 2, escrow);
	nfs4_op_chunk_escrow_install(cm->compound);
	ck_assert_int_eq(
		cm->compound->c_res->resarray.resarray_val[0]
			.nfs_resop4_u.opchunk_escrow_install.ceir_status,
		NFS4ERR_CHUNK_LOCKED);
	blk = chunk_store_lookup_any(g_inode->i_chunk_store, 20);
	ck_assert_ptr_nonnull(blk);
	ck_assert_int_eq(blk->cb_flags & CHUNK_BLOCK_ESCROW, 0);
	blk = chunk_store_lookup_any(g_inode->i_chunk_store, 21);
	ck_assert_msg(!blk || !(blk->cb_flags & CHUNK_BLOCK_ESCROW),
		      "conflicting install must not mark later chunks");

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
	tcase_add_test(tc_a, test_chunk_write_special_stateid_rejected);
	tcase_add_test(tc_a, test_chunk_write_stateid_file_mismatch_rejected);
	tcase_add_test(tc_a,
		       test_chunk_write_stateid_principal_mismatch_rejected);
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
	tcase_add_test(tc_c, test_chunk_finalize_requires_full_owner_triple);
	tcase_add_test(tc_c, test_chunk_lifecycle_delay_preserves_state);
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
	tcase_add_test(tc_e, test_chunk_read_bit_rot_preserves_stored_checksum);
	suite_add_tcase(s, tc_e);

	TCase *tc_f = tcase_create("full_cycle");
	tcase_add_checked_fixture(tc_f, chunk_setup, chunk_teardown);
	tcase_add_test(tc_f, test_chunk_full_cycle);
	tcase_add_test(tc_f, test_chunk_error_quarantines_committed_chunk);
	tcase_add_test(tc_f,
		       test_chunk_read_loads_persisted_store_after_restart);
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
	tcase_add_test(tc_h, test_chunk_read_locked_flag_reported);
	tcase_add_test(tc_h, test_chunk_read_owner_and_guard_are_distinct);
	tcase_add_test(tc_h,
		       test_chunk_write_gen_id_server_assigned_and_increments);
	tcase_add_test(tc_h, test_chunk_write_stale_guard_rejected);
	tcase_add_test(tc_h, test_attr90_client_setattr_rejected);
	tcase_add_test(tc_h, test_attr90_mds_setattr_on_empty_accepted);
	tcase_add_test(tc_h, test_attr90_mds_setattr_on_nonempty_rejected);
	tcase_add_test(tc_h, test_chunk_ops_rejected_on_non_chunked);
	tcase_add_test(tc_h, test_chunk_ops_allowed_when_unidentified);
	tcase_add_test(tc_h, test_chunk_ops_allowed_when_chunked);
	tcase_add_test(tc_h, test_chunk_lifecycle_ops_rejected_on_non_chunked);
	tcase_add_test(tc_h, test_non_chunk_op_rejected_on_chunked_file);
	tcase_add_test(tc_h, test_non_chunk_op_allowed_when_unidentified);
	tcase_add_test(tc_h, test_non_chunk_op_allowed_when_non_chunked);
	tcase_add_test(tc_h, test_non_chunk_op_allowed_on_control_session);
	tcase_add_test(tc_h, test_named_non_chunk_ops_rejected_on_chunked_file);
	tcase_add_test(tc_h, test_filehandle_plumbing_allowed_on_chunked_file);
	tcase_add_test(tc_h, test_chunk_write_still_works_through_dispatch);
	tcase_add_test(tc_h, test_chunk_lock_and_unlock_empty_range);
	tcase_add_test(tc_h, test_chunk_lock_conflict_reports_holder);
	tcase_add_test(tc_h, test_chunk_lock_takeover_is_not_supported);
	tcase_add_test(tc_h, test_chunk_mds_epoch_persistence_roundtrip);
	tcase_add_test(tc_h, test_chunk_escrow_install_and_release);
	tcase_add_test(tc_h, test_chunk_escrow_release_rejects_stale_id);
	tcase_add_test(tc_h, test_chunk_lock_adopts_escrow);
	tcase_add_test(tc_h, test_chunk_escrow_survives_writer_expiry_cleanup);
	tcase_add_test(tc_h, test_chunk_escrow_requires_mds_session);
	tcase_add_test(tc_h, test_chunk_escrow_enumerate_probe);
	tcase_add_test(tc_h, test_chunk_escrow_rejects_stale_epoch);
	tcase_add_test(tc_h, test_chunk_escrow_takeover_remains_disabled);
	tcase_add_test(tc_h,
		       test_chunk_escrow_install_conflict_is_all_or_nothing);
	suite_add_tcase(s, tc_h);

	return s;
}

int main(void)
{
	return reffs_test_run_suite(chunk_suite(), NULL, NULL);
}
