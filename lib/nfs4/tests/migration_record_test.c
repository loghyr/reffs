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
	suite_add_tcase(s, tc);

	return s;
}

int main(void)
{
	return reffs_test_run_suite(migration_record_suite(), NULL, NULL);
}
