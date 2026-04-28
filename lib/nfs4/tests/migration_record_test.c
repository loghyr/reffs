/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Slice 6c-x.2: migration_record table primitives.
 *
 * Tests the dual-index cds_lfht (proxy_stateid.other + inode), the
 * Rule 6 ref-counted lifecycle, the create-time per-inode invariant,
 * the phase-CAS commit/abandon transitions, and the lease-aware
 * reaper.  Phase transitions and per-instance delta application
 * driven by the actual PROXY_DONE / PROXY_CANCEL handlers land in
 * slice 6c-x.3; the LAYOUTGET view-build hook is in slice 6c-x.4.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <check.h>

#include "nfsv42_xdr.h"
#include "nfs4/migration_record.h"
#include "nfs4/proxy_stateid.h"
#include "reffs/migration_persist.h"
#include "reffs/persist_ops.h"
#include "libreffs_test.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void mr_setup(void)
{
	ck_assert_int_eq(migration_record_init(), 0);
}

static void mr_teardown(void)
{
	migration_record_fini();
}

static stateid4 make_stid(uint16_t boot_seq)
{
	stateid4 s;

	ck_assert_int_eq(proxy_stateid_alloc(boot_seq, &s), 0);
	return s;
}

/*
 * Tests don't need a real super_block for table-level primitives;
 * a unique-pointer placeholder is sufficient since the field is
 * captured-but-unused at this slice (slice 6c-x.4 will read it).
 */
static struct super_block *fake_sb(void)
{
	static char sentinel;

	return (struct super_block *)&sentinel;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

START_TEST(test_init_fini_idempotent)
{
	migration_record_fini();
	migration_record_fini();
}
END_TEST

/* ------------------------------------------------------------------ */
/* Create + find                                                       */
/* ------------------------------------------------------------------ */

START_TEST(test_create_find_by_stateid)
{
	stateid4 s = make_stid(0x0427);
	struct migration_record *mr = NULL;

	ck_assert_int_eq(migration_record_create(&s, fake_sb(), 1001,
						 "ps-instance-1", 13, NULL, 0,
						 1000ULL * 1000000000ULL, &mr),
			 0);
	ck_assert_ptr_nonnull(mr);

	struct migration_record *found = migration_record_find_by_stateid(&s);

	ck_assert_ptr_eq(found, mr);
	migration_record_put(found);
	migration_record_abandon(mr); /* clean up */
}
END_TEST

START_TEST(test_create_find_by_inode)
{
	stateid4 s = make_stid(0x0427);
	struct migration_record *mr = NULL;

	ck_assert_int_eq(migration_record_create(&s, fake_sb(), 1002, "ps", 2,
						 NULL, 0, 0, &mr),
			 0);

	struct migration_record *found = migration_record_find_by_inode(1002);

	ck_assert_ptr_eq(found, mr);
	migration_record_put(found);
	migration_record_abandon(mr);
}
END_TEST

START_TEST(test_find_returns_null_when_absent)
{
	stateid4 s = make_stid(0x0427);

	ck_assert_ptr_null(migration_record_find_by_stateid(&s));
	ck_assert_ptr_null(migration_record_find_by_inode(99999));
}
END_TEST

/* ------------------------------------------------------------------ */
/* Per-inode invariant                                                 */
/* ------------------------------------------------------------------ */

START_TEST(test_create_busy_when_inode_already_active)
{
	stateid4 s1 = make_stid(0x0427);
	stateid4 s2 = make_stid(0x0427);
	struct migration_record *mr1 = NULL, *mr2 = NULL;

	ck_assert_int_eq(migration_record_create(&s1, fake_sb(), 2001, "ps", 2,
						 NULL, 0, 0, &mr1),
			 0);
	/*
	 * Different proxy_stateid, same inode -- second create must
	 * return -EBUSY without replacing the prior record.
	 */
	ck_assert_int_eq(migration_record_create(&s2, fake_sb(), 2001, "ps", 2,
						 NULL, 0, 0, &mr2),
			 -EBUSY);
	ck_assert_ptr_null(mr2);

	/* The original record is intact. */
	struct migration_record *found = migration_record_find_by_stateid(&s1);

	ck_assert_ptr_eq(found, mr1);
	migration_record_put(found);
	migration_record_abandon(mr1);
}
END_TEST

START_TEST(test_create_after_abandon_succeeds)
{
	stateid4 s1 = make_stid(0x0427);
	stateid4 s2 = make_stid(0x0427);
	struct migration_record *mr1 = NULL, *mr2 = NULL;

	ck_assert_int_eq(migration_record_create(&s1, fake_sb(), 3001, "ps", 2,
						 NULL, 0, 0, &mr1),
			 0);
	ck_assert_int_eq(migration_record_abandon(mr1), 0);

	/* Once the prior record is abandoned, a new one for the same inode is OK. */
	ck_assert_int_eq(migration_record_create(&s2, fake_sb(), 3001, "ps", 2,
						 NULL, 0, 0, &mr2),
			 0);
	migration_record_abandon(mr2);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Argument validation                                                 */
/* ------------------------------------------------------------------ */

START_TEST(test_create_bad_args)
{
	stateid4 s = make_stid(0x0427);
	struct migration_record *mr = NULL;

	/* NULL stateid */
	ck_assert_int_eq(migration_record_create(NULL, fake_sb(), 1, "p", 1,
						 NULL, 0, 0, &mr),
			 -EINVAL);
	/* NULL out pointer */
	ck_assert_int_eq(migration_record_create(&s, fake_sb(), 1, "p", 1, NULL,
						 0, 0, NULL),
			 -EINVAL);
	/* NULL owner_reg */
	ck_assert_int_eq(migration_record_create(&s, fake_sb(), 1, NULL, 1,
						 NULL, 0, 0, &mr),
			 -EINVAL);
	/* Zero owner_reg_len */
	ck_assert_int_eq(migration_record_create(&s, fake_sb(), 1, "p", 0, NULL,
						 0, 0, &mr),
			 -EINVAL);
	/* Oversized owner_reg_len */
	ck_assert_int_eq(migration_record_create(&s, fake_sb(), 1, "p",
						 MIGRATION_OWNER_REG_MAX + 1,
						 NULL, 0, 0, &mr),
			 -EINVAL);
	/* deltas != NULL but ndeltas == 0 is allowed (caller's choice);
	 * deltas == NULL but ndeltas > 0 is invalid. */
	ck_assert_int_eq(migration_record_create(&s, fake_sb(), 1, "p", 1, NULL,
						 5, 0, &mr),
			 -EINVAL);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Owner identity                                                      */
/* ------------------------------------------------------------------ */

START_TEST(test_owner_reg_bytes_copied)
{
	stateid4 s = make_stid(0x0427);
	struct migration_record *mr = NULL;
	const char id_bytes[] = "host/ps.example.com@REALM";
	uint32_t len = (uint32_t)strlen(id_bytes);

	ck_assert_int_eq(migration_record_create(&s, fake_sb(), 4001, id_bytes,
						 len, NULL, 0, 0, &mr),
			 0);
	ck_assert_uint_eq(mr->mr_owner_reg_len, len);
	ck_assert_mem_eq(mr->mr_owner_reg, id_bytes, len);
	migration_record_abandon(mr);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Phase transitions                                                   */
/* ------------------------------------------------------------------ */

START_TEST(test_commit_unhashes_record)
{
	stateid4 s = make_stid(0x0427);
	struct migration_record *mr = NULL;

	ck_assert_int_eq(migration_record_create(&s, fake_sb(), 5001, "ps", 2,
						 NULL, 0, 0, &mr),
			 0);
	ck_assert_int_eq(migration_record_commit(mr), 0);

	ck_assert_ptr_null(migration_record_find_by_stateid(&s));
	ck_assert_ptr_null(migration_record_find_by_inode(5001));
}
END_TEST

START_TEST(test_abandon_unhashes_record)
{
	stateid4 s = make_stid(0x0427);
	struct migration_record *mr = NULL;

	ck_assert_int_eq(migration_record_create(&s, fake_sb(), 5002, "ps", 2,
						 NULL, 0, 0, &mr),
			 0);
	ck_assert_int_eq(migration_record_abandon(mr), 0);

	ck_assert_ptr_null(migration_record_find_by_stateid(&s));
	ck_assert_ptr_null(migration_record_find_by_inode(5002));
}
END_TEST

START_TEST(test_commit_then_commit_returns_already)
{
	stateid4 s = make_stid(0x0427);
	struct migration_record *mr = NULL;

	ck_assert_int_eq(migration_record_create(&s, fake_sb(), 5003, "ps", 2,
						 NULL, 0, 0, &mr),
			 0);
	ck_assert_int_eq(migration_record_commit(mr), 0);
	/*
	 * Second commit: phase is already COMMITTED so the CAS fails
	 * for both PENDING->COMMITTED and IN_PROGRESS->COMMITTED.
	 */
	ck_assert_int_eq(migration_record_commit(mr), -EALREADY);
}
END_TEST

START_TEST(test_abandon_then_commit_returns_already)
{
	stateid4 s = make_stid(0x0427);
	struct migration_record *mr = NULL;

	ck_assert_int_eq(migration_record_create(&s, fake_sb(), 5004, "ps", 2,
						 NULL, 0, 0, &mr),
			 0);
	ck_assert_int_eq(migration_record_abandon(mr), 0);
	ck_assert_int_eq(migration_record_commit(mr), -EALREADY);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Renew                                                               */
/* ------------------------------------------------------------------ */

START_TEST(test_renew_updates_progress_timestamp)
{
	stateid4 s = make_stid(0x0427);
	struct migration_record *mr = NULL;
	uint64_t t0 = 1000ULL * 1000000000ULL;
	uint64_t t1 = t0 + 5ULL * 1000000000ULL;

	ck_assert_int_eq(migration_record_create(&s, fake_sb(), 6001, "ps", 2,
						 NULL, 0, t0, &mr),
			 0);
	ck_assert_int_eq(migration_record_renew(&s, t1), 0);
	ck_assert_uint_eq(atomic_load_explicit(&mr->mr_last_progress_mono_ns,
					       memory_order_acquire),
			  t1);
	migration_record_abandon(mr);
}
END_TEST

START_TEST(test_renew_unknown_stateid_returns_enoent)
{
	stateid4 s = make_stid(0x0427);

	ck_assert_int_eq(migration_record_renew(&s, 1234), -ENOENT);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Reaper                                                              */
/* ------------------------------------------------------------------ */

START_TEST(test_reaper_abandons_silent_record)
{
	stateid4 s = make_stid(0x0427);
	struct migration_record *mr = NULL;
	uint64_t t0 = 1000ULL * 1000000000ULL;
	uint64_t now = t0 + 200ULL * 1000000000ULL; /* 200 s later */
	uint64_t silence = 90ULL * 1000000000ULL; /* 90 s threshold */

	ck_assert_int_eq(migration_record_create(&s, fake_sb(), 7001, "ps", 2,
						 NULL, 0, t0, &mr),
			 0);

	migration_record_reaper_scan(silence, now);

	/*
	 * Reaper transitions phase to ABANDONED, unhashes, and drops
	 * the creation ref.  A fresh find by stateid returns NULL.
	 */
	ck_assert_ptr_null(migration_record_find_by_stateid(&s));
	ck_assert_ptr_null(migration_record_find_by_inode(7001));
}
END_TEST

START_TEST(test_reaper_keeps_recent_record)
{
	stateid4 s = make_stid(0x0427);
	struct migration_record *mr = NULL;
	uint64_t t0 = 1000ULL * 1000000000ULL;
	uint64_t now = t0 + 30ULL * 1000000000ULL; /* 30 s later */
	uint64_t silence = 90ULL * 1000000000ULL; /* 90 s threshold */

	ck_assert_int_eq(migration_record_create(&s, fake_sb(), 7002, "ps", 2,
						 NULL, 0, t0, &mr),
			 0);

	migration_record_reaper_scan(silence, now);

	struct migration_record *found = migration_record_find_by_stateid(&s);

	ck_assert_ptr_eq(found, mr);
	migration_record_put(found);
	migration_record_abandon(mr);
}
END_TEST

START_TEST(test_reaper_skips_zero_progress)
{
	stateid4 s = make_stid(0x0427);
	struct migration_record *mr = NULL;

	/*
	 * mr_last_progress_mono_ns == 0 means "no heartbeat recorded
	 * yet"; the reaper must NOT abandon such a record (it would
	 * otherwise destroy fresh records before the PS has had a
	 * chance to ack the assignment).
	 */
	ck_assert_int_eq(migration_record_create(&s, fake_sb(), 7003, "ps", 2,
						 NULL, 0, 0, &mr),
			 0);

	migration_record_reaper_scan(90ULL * 1000000000ULL,
				     1000ULL * 1000000000ULL);

	struct migration_record *found = migration_record_find_by_stateid(&s);

	ck_assert_ptr_eq(found, mr);
	migration_record_put(found);
	migration_record_abandon(mr);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Drain                                                               */
/* ------------------------------------------------------------------ */

START_TEST(test_fini_drains_outstanding_records)
{
	stateid4 s1 = make_stid(0x0427);
	stateid4 s2 = make_stid(0x0427);
	struct migration_record *mr1 = NULL, *mr2 = NULL;

	ck_assert_int_eq(migration_record_create(&s1, fake_sb(), 8001, "ps", 2,
						 NULL, 0, 1, &mr1),
			 0);
	ck_assert_int_eq(migration_record_create(&s2, fake_sb(), 8002, "ps", 2,
						 NULL, 0, 1, &mr2),
			 0);

	/*
	 * fini drops creation refs; release callback unhashes from both
	 * indices and schedules call_rcu free.  ASAN clean if no UAF.
	 */
	migration_record_fini();
	/* re-init so teardown's fini doesn't double-free */
	ck_assert_int_eq(migration_record_init(), 0);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Slice 6c-x.4: apply-deltas view                                     */
/* ------------------------------------------------------------------ */

static struct layout_data_file make_ldf(uint32_t dstore_id, uint8_t fh_byte)
{
	struct layout_data_file ldf = { 0 };

	ldf.ldf_dstore_id = dstore_id;
	ldf.ldf_fh_len = 4;
	ldf.ldf_fh[0] = fh_byte;
	return ldf;
}

START_TEST(test_apply_deltas_no_record_returns_base_unchanged)
{
	stateid4 s = make_stid(0x0427);
	struct migration_record *mr = NULL;
	struct layout_data_file files[2] = { make_ldf(1, 0xAA),
					     make_ldf(2, 0xBB) };
	struct layout_segment base = {
		.ls_offset = 0,
		.ls_length = 0,
		.ls_layout_type = LAYOUT4_FLEX_FILES_V2,
		.ls_nfiles = 2,
		.ls_files = files,
	};
	struct layout_segment view = { 0 };

	ck_assert_int_eq(migration_record_create(&s, fake_sb(), 9001, "ps", 2,
						 NULL, 0, 1, &mr),
			 0);
	ck_assert_int_eq(migration_apply_deltas_to_segment(&base, 0, mr, &view),
			 0);
	ck_assert_uint_eq(view.ls_nfiles, 2);
	ck_assert_uint_eq(view.ls_files[0].ldf_dstore_id, 1);
	ck_assert_uint_eq(view.ls_files[1].ldf_dstore_id, 2);
	migration_release_view(&view);
	migration_record_abandon(mr);
}
END_TEST

START_TEST(test_apply_deltas_draining_omitted_incoming_inserted)
{
	stateid4 s = make_stid(0x0427);
	struct migration_record *mr = NULL;
	struct layout_data_file files[3] = { make_ldf(1, 0xAA),
					     make_ldf(2, 0xBB),
					     make_ldf(3, 0xCC) };
	struct layout_segment base = {
		.ls_layout_type = LAYOUT4_FLEX_FILES_V2,
		.ls_nfiles = 3,
		.ls_files = files,
	};
	/*
	 * Drain instance 1 (dstore 2), replace with INCOMING dstore 4.
	 * Expected view: { dstore 1, dstore 3, dstore 4 } -- DRAINING
	 * is omitted, INCOMING appended.
	 */
	struct migration_instance_delta *deltas = calloc(2, sizeof(*deltas));

	deltas[0].mid_seg_index = 0;
	deltas[0].mid_instance_index = 1;
	deltas[0].mid_state = MIGRATION_INSTANCE_DRAINING;
	deltas[0].mid_replacement_delta_idx = 1;

	deltas[1].mid_seg_index = 0;
	deltas[1].mid_instance_index = 0; /* unused for INCOMING */
	deltas[1].mid_state = MIGRATION_INSTANCE_INCOMING;
	deltas[1].mid_replacement_file = make_ldf(4, 0xDD);

	ck_assert_int_eq(migration_record_create(&s, fake_sb(), 9002, "ps", 2,
						 deltas, 2, 1, &mr),
			 0);

	struct layout_segment view = { 0 };

	ck_assert_int_eq(migration_apply_deltas_to_segment(&base, 0, mr, &view),
			 0);
	ck_assert_uint_eq(view.ls_nfiles, 3);
	ck_assert_uint_eq(view.ls_files[0].ldf_dstore_id, 1);
	ck_assert_uint_eq(view.ls_files[1].ldf_dstore_id, 3);
	ck_assert_uint_eq(view.ls_files[2].ldf_dstore_id, 4);
	migration_release_view(&view);
	migration_record_abandon(mr);
}
END_TEST

START_TEST(test_apply_deltas_pure_drain_no_replacement)
{
	stateid4 s = make_stid(0x0427);
	struct migration_record *mr = NULL;
	struct layout_data_file files[2] = { make_ldf(1, 0xAA),
					     make_ldf(2, 0xBB) };
	struct layout_segment base = {
		.ls_layout_type = LAYOUT4_FLEX_FILES_V2,
		.ls_nfiles = 2,
		.ls_files = files,
	};
	struct migration_instance_delta *deltas = calloc(1, sizeof(*deltas));

	deltas[0].mid_seg_index = 0;
	deltas[0].mid_instance_index = 0;
	deltas[0].mid_state = MIGRATION_INSTANCE_DRAINING;
	deltas[0].mid_replacement_delta_idx = UINT32_MAX; /* no pair */

	ck_assert_int_eq(migration_record_create(&s, fake_sb(), 9003, "ps", 2,
						 deltas, 1, 1, &mr),
			 0);

	struct layout_segment view = { 0 };

	ck_assert_int_eq(migration_apply_deltas_to_segment(&base, 0, mr, &view),
			 0);
	ck_assert_uint_eq(view.ls_nfiles, 1);
	ck_assert_uint_eq(view.ls_files[0].ldf_dstore_id, 2);
	migration_release_view(&view);
	migration_record_abandon(mr);
}
END_TEST

START_TEST(test_apply_deltas_other_segment_untouched)
{
	stateid4 s = make_stid(0x0427);
	struct migration_record *mr = NULL;
	struct layout_data_file files[2] = { make_ldf(1, 0xAA),
					     make_ldf(2, 0xBB) };
	struct layout_segment base = {
		.ls_layout_type = LAYOUT4_FLEX_FILES_V2,
		.ls_nfiles = 2,
		.ls_files = files,
	};
	/* Delta targets segment index 5; we apply to base at index 0. */
	struct migration_instance_delta *deltas = calloc(1, sizeof(*deltas));

	deltas[0].mid_seg_index = 5;
	deltas[0].mid_instance_index = 0;
	deltas[0].mid_state = MIGRATION_INSTANCE_DRAINING;
	deltas[0].mid_replacement_delta_idx = UINT32_MAX;

	ck_assert_int_eq(migration_record_create(&s, fake_sb(), 9004, "ps", 2,
						 deltas, 1, 1, &mr),
			 0);

	struct layout_segment view = { 0 };

	ck_assert_int_eq(migration_apply_deltas_to_segment(&base, 0, mr, &view),
			 0);
	/* No deltas matched seg_index 0 -- view equals base. */
	ck_assert_uint_eq(view.ls_nfiles, 2);
	ck_assert_uint_eq(view.ls_files[0].ldf_dstore_id, 1);
	ck_assert_uint_eq(view.ls_files[1].ldf_dstore_id, 2);
	migration_release_view(&view);
	migration_record_abandon(mr);
}
END_TEST

START_TEST(test_apply_deltas_empty_input_segment)
{
	stateid4 s = make_stid(0x0427);
	struct migration_record *mr = NULL;
	struct layout_segment base = {
		.ls_layout_type = LAYOUT4_FLEX_FILES_V2,
		.ls_nfiles = 0,
		.ls_files = NULL,
	};

	ck_assert_int_eq(migration_record_create(&s, fake_sb(), 9005, "ps", 2,
						 NULL, 0, 1, &mr),
			 0);

	struct layout_segment view = { 0 };

	ck_assert_int_eq(migration_apply_deltas_to_segment(&base, 0, mr, &view),
			 0);
	ck_assert_uint_eq(view.ls_nfiles, 0);
	ck_assert_ptr_null(view.ls_files);
	migration_release_view(&view); /* idempotent on zero */
	migration_record_abandon(mr);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Slice 6c-x.5: post-commit recall                                    */
/* ------------------------------------------------------------------ */

/*
 * Stub-level coverage (per design doc revision "6c-x.5 -- CB_LAYOUTRECALL
 * on DONE(OK) when DRAINING removed.  Reuse existing recall infra; just
 * queue the recalls.  Tests: recall-emitted (stub-level)").  Full
 * end-to-end recall delivery exercises the existing CB infrastructure
 * and is covered by the integration soak harness; here we just pin
 * the no-stateid behavior so the helper is safe to call from
 * PROXY_DONE on a fresh-OPEN file.
 */
START_TEST(test_recall_layouts_no_inode_returns_zero)
{
	ck_assert_uint_eq(migration_recall_layouts(NULL, NULL, NULL), 0);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Slice 6c-zz wiring: persist backend + reload                        */
/* ------------------------------------------------------------------ */

/*
 * Tiny in-test persist backend that captures save / remove calls in
 * a fixed-size array, so a "restart" can be simulated without hitting
 * the filesystem.  Mirrors the flatfile backend's contract: append on
 * save, drop-by-stateid on remove, callback-iterate on load.
 */
#define MOCK_PERSIST_CAP 8
struct mock_persist {
	struct migration_record_persistent recs[MOCK_PERSIST_CAP];
	size_t nrecs;
};

static int mock_persist_save(void *ctx,
			     const struct migration_record_persistent *mrp)
{
	struct mock_persist *mp = ctx;

	if (mp->nrecs >= MOCK_PERSIST_CAP)
		return -ENOSPC;
	mp->recs[mp->nrecs++] = *mrp;
	return 0;
}

static int mock_persist_remove(void *ctx, const uint8_t *stateid_other)
{
	struct mock_persist *mp = ctx;

	for (size_t i = 0; i < mp->nrecs; i++) {
		if (memcmp(mp->recs[i].mrp_stateid_other, stateid_other,
			   MR_PERSIST_NFS4_OTHER_SIZE) == 0) {
			memmove(&mp->recs[i], &mp->recs[i + 1],
				(mp->nrecs - i - 1) * sizeof(mp->recs[0]));
			mp->nrecs--;
			return 0;
		}
	}
	return 0; /* idempotent: missing == success */
}

static int
mock_persist_load(void *ctx,
		  int (*cb)(const struct migration_record_persistent *, void *),
		  void *cb_arg)
{
	struct mock_persist *mp = ctx;

	for (size_t i = 0; i < mp->nrecs; i++) {
		int ret = cb(&mp->recs[i], cb_arg);

		if (ret < 0)
			return ret;
	}
	return 0;
}

static const struct persist_ops mock_persist_ops = {
	.migration_record_save = mock_persist_save,
	.migration_record_remove = mock_persist_remove,
	.migration_record_load = mock_persist_load,
};

START_TEST(test_persist_save_on_create)
{
	struct mock_persist mp = { 0 };

	migration_record_persist_attach(&mock_persist_ops, &mp);

	stateid4 s = make_stid(0x0427);
	struct migration_record *mr = NULL;

	ck_assert_int_eq(migration_record_create(&s, fake_sb(), 4001, "ps", 2,
						 NULL, 0, 1, &mr),
			 0);
	ck_assert_uint_eq(mp.nrecs, 1);
	ck_assert_uint_eq(mp.recs[0].mrp_ino, 4001);

	migration_record_abandon(mr);
	migration_record_persist_attach(NULL, NULL);
}
END_TEST

START_TEST(test_persist_remove_on_abandon)
{
	struct mock_persist mp = { 0 };

	migration_record_persist_attach(&mock_persist_ops, &mp);

	stateid4 s = make_stid(0x0427);
	struct migration_record *mr = NULL;

	ck_assert_int_eq(migration_record_create(&s, fake_sb(), 4002, "ps", 2,
						 NULL, 0, 1, &mr),
			 0);
	ck_assert_uint_eq(mp.nrecs, 1);

	/*
	 * abandon calls unhash internally, which calls remove_one against
	 * the persistence backend.  Symmetric with the save in create.
	 */
	migration_record_abandon(mr);
	ck_assert_uint_eq(mp.nrecs, 0);

	migration_record_persist_attach(NULL, NULL);
}
END_TEST

START_TEST(test_persist_remove_on_commit)
{
	struct mock_persist mp = { 0 };

	migration_record_persist_attach(&mock_persist_ops, &mp);

	stateid4 s = make_stid(0x0427);
	struct migration_record *mr = NULL;

	ck_assert_int_eq(migration_record_create(&s, fake_sb(), 4005, "ps", 2,
						 NULL, 0, 1, &mr),
			 0);
	ck_assert_uint_eq(mp.nrecs, 1);

	migration_record_commit(mr);
	ck_assert_uint_eq(mp.nrecs, 0);

	migration_record_persist_attach(NULL, NULL);
}
END_TEST

START_TEST(test_persist_reload_round_trip)
{
	struct mock_persist mp = { 0 };
	stateid4 s = make_stid(0x0427);

	/* Phase 1: create + persist a record. */
	migration_record_persist_attach(&mock_persist_ops, &mp);

	struct migration_record *mr = NULL;

	ck_assert_int_eq(migration_record_create(&s, fake_sb(), 4003,
						 "ps-instance-zz", 14, NULL, 0,
						 1234ULL * 1000000000ULL, &mr),
			 0);
	migration_record_put(mr); /* drop creation-extra ref */

	/*
	 * Phase 2: simulate MDS restart -- detach the backend, fini the
	 * tables (drains in-memory state), re-init, reload from persist.
	 */
	migration_record_persist_attach(NULL, NULL);
	migration_record_fini();
	ck_assert_int_eq(migration_record_init(), 0);

	int loaded = migration_record_load_persisted(&mock_persist_ops, &mp);

	ck_assert_int_eq(loaded, 1);

	/* Phase 3: the reloaded record is findable on both indices. */
	struct migration_record *found = migration_record_find_by_stateid(&s);

	ck_assert_ptr_nonnull(found);
	ck_assert_uint_eq(found->mr_ino, 4003);
	ck_assert_uint_eq(found->mr_owner_reg_len, 14);
	ck_assert_mem_eq(found->mr_owner_reg, "ps-instance-zz", 14);
	migration_record_put(found);

	struct migration_record *by_ino = migration_record_find_by_inode(4003);

	ck_assert_ptr_nonnull(by_ino);
	migration_record_put(by_ino);
}
END_TEST

START_TEST(test_persist_reload_empty_backend)
{
	struct mock_persist mp = { 0 };

	int loaded = migration_record_load_persisted(&mock_persist_ops, &mp);

	ck_assert_int_eq(loaded, 0);
}
END_TEST

START_TEST(test_persist_attach_null_is_safe)
{
	/* Detach with NULL must not crash; subsequent create must work. */
	migration_record_persist_attach(NULL, NULL);

	stateid4 s = make_stid(0x0427);
	struct migration_record *mr = NULL;

	ck_assert_int_eq(migration_record_create(&s, fake_sb(), 4004, "ps", 2,
						 NULL, 0, 1, &mr),
			 0);
	migration_record_abandon(mr);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite                                                               */
/* ------------------------------------------------------------------ */

static Suite *migration_record_suite(void)
{
	Suite *s = suite_create("migration_record");
	TCase *tc = tcase_create("migration_record");

	tcase_add_checked_fixture(tc, mr_setup, mr_teardown);
	tcase_add_test(tc, test_init_fini_idempotent);
	tcase_add_test(tc, test_create_find_by_stateid);
	tcase_add_test(tc, test_create_find_by_inode);
	tcase_add_test(tc, test_find_returns_null_when_absent);
	tcase_add_test(tc, test_create_busy_when_inode_already_active);
	tcase_add_test(tc, test_create_after_abandon_succeeds);
	tcase_add_test(tc, test_create_bad_args);
	tcase_add_test(tc, test_owner_reg_bytes_copied);
	tcase_add_test(tc, test_commit_unhashes_record);
	tcase_add_test(tc, test_abandon_unhashes_record);
	tcase_add_test(tc, test_commit_then_commit_returns_already);
	tcase_add_test(tc, test_abandon_then_commit_returns_already);
	tcase_add_test(tc, test_renew_updates_progress_timestamp);
	tcase_add_test(tc, test_renew_unknown_stateid_returns_enoent);
	tcase_add_test(tc, test_reaper_abandons_silent_record);
	tcase_add_test(tc, test_reaper_keeps_recent_record);
	tcase_add_test(tc, test_reaper_skips_zero_progress);
	tcase_add_test(tc, test_fini_drains_outstanding_records);
	tcase_add_test(tc, test_apply_deltas_no_record_returns_base_unchanged);
	tcase_add_test(tc,
		       test_apply_deltas_draining_omitted_incoming_inserted);
	tcase_add_test(tc, test_apply_deltas_pure_drain_no_replacement);
	tcase_add_test(tc, test_apply_deltas_other_segment_untouched);
	tcase_add_test(tc, test_apply_deltas_empty_input_segment);
	tcase_add_test(tc, test_recall_layouts_no_inode_returns_zero);
	tcase_add_test(tc, test_persist_save_on_create);
	tcase_add_test(tc, test_persist_remove_on_abandon);
	tcase_add_test(tc, test_persist_remove_on_commit);
	tcase_add_test(tc, test_persist_reload_round_trip);
	tcase_add_test(tc, test_persist_reload_empty_backend);
	tcase_add_test(tc, test_persist_attach_null_is_safe);
	suite_add_tcase(s, tc);

	return s;
}

int main(void)
{
	return reffs_test_run_suite(migration_record_suite(), NULL, NULL);
}
