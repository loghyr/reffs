/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Unit tests for the DS trust table (trust_stateid.c).
 *
 * Tests cover:
 *   A. Init / fini lifecycle
 *   B. Register and idempotent update
 *   C. Find and reference counting
 *   D. Revoke (single and bulk)
 *   E. trust_stateid_convert_expire
 *
 * The trust table uses liburcu.  All tests call rcu_register_thread()
 * and rcu_unregister_thread() via setup/teardown.  The trust_ht is
 * torn down between tests via trust_stateid_fini().
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include <check.h>
#include <urcu.h>

#include "nfsv42_xdr.h"
#include "nfs4/trust_stateid.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/*
 * make_stateid -- build a stateid4 with a recognisable other[] pattern.
 * The seqid is set to 1 so stateid4_is_special() returns false.
 */
static stateid4 make_stateid(uint8_t fill)
{
	stateid4 s;

	s.seqid = 1;
	memset(s.other, fill, NFS4_OTHER_SIZE);
	return s;
}

/*
 * future_expire_ns -- CLOCK_MONOTONIC deadline 2 seconds in the future.
 *
 * trust_stateid_register()'s expire_mono_ns is a CLOCK_MONOTONIC value;
 * using CLOCK_REALTIME here would produce a semantically wrong value on
 * systems where the clocks have diverged (e.g., post-ntpd step).
 */
static uint64_t future_expire_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec +
	       2000000000ULL; /* +2 s */
}

/* ------------------------------------------------------------------ */
/* Fixtures                                                            */
/* ------------------------------------------------------------------ */

static void setup(void)
{
	rcu_register_thread();
	ck_assert_int_eq(trust_stateid_init(), 0);
}

static void teardown(void)
{
	trust_stateid_fini();
	rcu_unregister_thread();
}

/* ------------------------------------------------------------------ */
/* A. Init / fini                                                      */
/* ------------------------------------------------------------------ */

/*
 * Double fini must not crash -- trust_stateid_fini guards on NULL ht.
 */
START_TEST(test_init_fini_idempotent)
{
	/* setup() already called init; call fini twice. */
	trust_stateid_fini();
	trust_stateid_fini(); /* second call: no-op */
	/* reinit so teardown()'s fini is safe */
	ck_assert_int_eq(trust_stateid_init(), 0);
}
END_TEST

/*
 * find() before init (NULL ht) must return NULL, not crash.
 */
START_TEST(test_find_before_init)
{
	trust_stateid_fini(); /* drop ht */

	stateid4 s = make_stateid(0xAB);
	struct trust_entry *te = trust_stateid_find(&s);

	ck_assert_ptr_null(te);

	/* reinit so teardown is safe */
	ck_assert_int_eq(trust_stateid_init(), 0);
}
END_TEST

/* ------------------------------------------------------------------ */
/* B. Register                                                         */
/* ------------------------------------------------------------------ */

/*
 * Registering a new stateid returns 0 and the entry is findable.
 */
START_TEST(test_register_basic)
{
	stateid4 s = make_stateid(0x01);
	int ret = trust_stateid_register(&s, 42, 0xCAFE, LAYOUTIOMODE4_RW,
					 future_expire_ns(), "");
	ck_assert_int_eq(ret, 0);

	struct trust_entry *te = trust_stateid_find(&s);

	ck_assert_ptr_nonnull(te);
	ck_assert_int_eq(memcmp(te->te_other, s.other, NFS4_OTHER_SIZE), 0);
	ck_assert_uint_eq(te->te_ino, 42);
	ck_assert_uint_eq(te->te_clientid, 0xCAFE);
	ck_assert_int_eq(te->te_iomode, LAYOUTIOMODE4_RW);
	ck_assert_uint_ne(atomic_load_explicit(&te->te_expire_ns,
					       memory_order_relaxed),
			  0);
	ck_assert_uint_eq(atomic_load_explicit(&te->te_flags,
					       memory_order_relaxed) &
				  TRUST_ACTIVE,
			  TRUST_ACTIVE);
	trust_entry_put(te);
}
END_TEST

/*
 * Registering the same stateid.other twice is idempotent: the existing
 * entry is updated in-place and find() still returns one entry.
 */
START_TEST(test_register_idempotent)
{
	stateid4 s = make_stateid(0x02);

	trust_stateid_register(&s, 10, 0xCAFE, LAYOUTIOMODE4_READ,
			       future_expire_ns(), "");
	trust_stateid_register(&s, 10, 0xCAFE, LAYOUTIOMODE4_RW,
			       future_expire_ns(), "");

	struct trust_entry *te = trust_stateid_find(&s);

	ck_assert_ptr_nonnull(te);
	/* Second call updated the iomode. */
	ck_assert_int_eq(te->te_iomode, LAYOUTIOMODE4_RW);
	trust_entry_put(te);
}
END_TEST

/*
 * Principal is stored and retrievable.
 */
START_TEST(test_register_with_principal)
{
	stateid4 s = make_stateid(0x03);
	const char *principal = "nfs/mds.example.com@EXAMPLE.COM";

	trust_stateid_register(&s, 1, 0, LAYOUTIOMODE4_RW, future_expire_ns(),
			       principal);

	struct trust_entry *te = trust_stateid_find(&s);

	ck_assert_ptr_nonnull(te);
	ck_assert_str_eq(te->te_principal, principal);
	trust_entry_put(te);
}
END_TEST

/*
 * Principal longer than TRUST_PRINCIPAL_MAX-1 must be safely truncated.
 */
START_TEST(test_register_principal_truncated)
{
	stateid4 s = make_stateid(0x04);
	char long_principal[TRUST_PRINCIPAL_MAX + 64];

	memset(long_principal, 'x', sizeof(long_principal) - 1);
	long_principal[sizeof(long_principal) - 1] = '\0';

	trust_stateid_register(&s, 1, 0, LAYOUTIOMODE4_RW, future_expire_ns(),
			       long_principal);

	struct trust_entry *te = trust_stateid_find(&s);

	ck_assert_ptr_nonnull(te);
	/* Must be NUL-terminated and within bound. */
	ck_assert_int_lt((int)strlen(te->te_principal), TRUST_PRINCIPAL_MAX);
	ck_assert_int_eq(te->te_principal[TRUST_PRINCIPAL_MAX - 1], '\0');
	trust_entry_put(te);
}
END_TEST

/*
 * Two different stateids coexist in the table independently.
 */
START_TEST(test_register_two_entries)
{
	stateid4 s1 = make_stateid(0x11);
	stateid4 s2 = make_stateid(0x22);

	trust_stateid_register(&s1, 1, 0, LAYOUTIOMODE4_RW, future_expire_ns(),
			       "");
	trust_stateid_register(&s2, 2, 0, LAYOUTIOMODE4_READ,
			       future_expire_ns(), "");

	struct trust_entry *te1 = trust_stateid_find(&s1);
	struct trust_entry *te2 = trust_stateid_find(&s2);

	ck_assert_ptr_nonnull(te1);
	ck_assert_ptr_nonnull(te2);
	ck_assert_ptr_ne(te1, te2);
	ck_assert_uint_eq(te1->te_ino, 1);
	ck_assert_uint_eq(te2->te_ino, 2);

	trust_entry_put(te1);
	trust_entry_put(te2);
}
END_TEST

/* ------------------------------------------------------------------ */
/* C. Find and reference counting                                      */
/* ------------------------------------------------------------------ */

/*
 * find() returns NULL for a stateid that was never registered.
 */
START_TEST(test_find_not_found)
{
	stateid4 s = make_stateid(0xFF);
	struct trust_entry *te = trust_stateid_find(&s);

	ck_assert_ptr_null(te);
}
END_TEST

/*
 * find() increments the refcount; put() decrements it.
 * The entry must still be findable after put() (creation ref remains).
 */
START_TEST(test_find_refcount)
{
	stateid4 s = make_stateid(0x05);

	trust_stateid_register(&s, 1, 0, LAYOUTIOMODE4_RW, future_expire_ns(),
			       "");

	struct trust_entry *te = trust_stateid_find(&s);

	ck_assert_ptr_nonnull(te);
	trust_entry_put(te);

	/* Entry still alive (creation ref remains). */
	struct trust_entry *te2 = trust_stateid_find(&s);

	ck_assert_ptr_nonnull(te2);
	trust_entry_put(te2);
}
END_TEST

/*
 * trust_entry_put(NULL) must not crash.
 */
START_TEST(test_put_null)
{
	trust_entry_put(NULL);
}
END_TEST

/*
 * TRUST_ACTIVE flag is set after register.
 */
START_TEST(test_flags_active_after_register)
{
	stateid4 s = make_stateid(0x06);

	trust_stateid_register(&s, 1, 0, LAYOUTIOMODE4_RW, future_expire_ns(),
			       "");

	struct trust_entry *te = trust_stateid_find(&s);

	ck_assert_ptr_nonnull(te);
	ck_assert_uint_ne(atomic_load_explicit(&te->te_flags,
					       memory_order_acquire) &
				  TRUST_ACTIVE,
			  0);
	trust_entry_put(te);
}
END_TEST

/* ------------------------------------------------------------------ */
/* D. Revoke                                                           */
/* ------------------------------------------------------------------ */

/*
 * After revoke(), the entry is no longer findable.
 */
START_TEST(test_revoke_removes_entry)
{
	stateid4 s = make_stateid(0x07);

	trust_stateid_register(&s, 1, 0, LAYOUTIOMODE4_RW, future_expire_ns(),
			       "");
	trust_stateid_revoke(&s);

	struct trust_entry *te = trust_stateid_find(&s);

	ck_assert_ptr_null(te);
}
END_TEST

/*
 * Revoking a stateid that was never registered must not crash.
 */
START_TEST(test_revoke_not_found)
{
	stateid4 s = make_stateid(0xDE);

	trust_stateid_revoke(&s); /* must be a no-op */
}
END_TEST

/*
 * Revoking then re-registering the same stateid.other works.
 */
START_TEST(test_revoke_then_reregister)
{
	stateid4 s = make_stateid(0x08);

	trust_stateid_register(&s, 1, 0, LAYOUTIOMODE4_RW, future_expire_ns(),
			       "");
	trust_stateid_revoke(&s);
	trust_stateid_register(&s, 2, 0, LAYOUTIOMODE4_READ, future_expire_ns(),
			       "");

	struct trust_entry *te = trust_stateid_find(&s);

	ck_assert_ptr_nonnull(te);
	ck_assert_uint_eq(te->te_ino, 2);
	trust_entry_put(te);
}
END_TEST

/*
 * bulk_revoke with a specific clientid removes only that client's entries.
 */
START_TEST(test_bulk_revoke_by_clientid)
{
	stateid4 s1 = make_stateid(0x30);
	stateid4 s2 = make_stateid(0x31);
	stateid4 s3 = make_stateid(0x32);
	const clientid4 cid_a = 0xAAAA;
	const clientid4 cid_b = 0xBBBB;

	trust_stateid_register(&s1, 1, cid_a, LAYOUTIOMODE4_RW,
			       future_expire_ns(), "");
	trust_stateid_register(&s2, 2, cid_a, LAYOUTIOMODE4_RW,
			       future_expire_ns(), "");
	trust_stateid_register(&s3, 3, cid_b, LAYOUTIOMODE4_RW,
			       future_expire_ns(), "");

	trust_stateid_bulk_revoke(cid_a);

	struct trust_entry *te1 = trust_stateid_find(&s1);
	struct trust_entry *te2 = trust_stateid_find(&s2);
	struct trust_entry *te3 = trust_stateid_find(&s3);

	ck_assert_ptr_null(te1);
	ck_assert_ptr_null(te2);
	ck_assert_ptr_nonnull(te3);
	trust_entry_put(te3);
}
END_TEST

/*
 * bulk_revoke with clientid 0 clears all entries.
 */
START_TEST(test_bulk_revoke_all)
{
	stateid4 s1 = make_stateid(0x40);
	stateid4 s2 = make_stateid(0x41);
	stateid4 s3 = make_stateid(0x42);

	trust_stateid_register(&s1, 1, 0xAAAA, LAYOUTIOMODE4_RW,
			       future_expire_ns(), "");
	trust_stateid_register(&s2, 2, 0xBBBB, LAYOUTIOMODE4_RW,
			       future_expire_ns(), "");
	trust_stateid_register(&s3, 3, 0xCCCC, LAYOUTIOMODE4_RW,
			       future_expire_ns(), "");

	trust_stateid_bulk_revoke(0); /* 0 = clear all */

	ck_assert_ptr_null(trust_stateid_find(&s1));
	ck_assert_ptr_null(trust_stateid_find(&s2));
	ck_assert_ptr_null(trust_stateid_find(&s3));
}
END_TEST

/*
 * bulk_revoke on an empty table must not crash.
 */
START_TEST(test_bulk_revoke_empty)
{
	trust_stateid_bulk_revoke(0);
	trust_stateid_bulk_revoke(0xDEAD);
}
END_TEST

/* ------------------------------------------------------------------ */
/* E. trust_stateid_convert_expire                                     */
/* ------------------------------------------------------------------ */

/*
 * Valid future wall-clock expiry maps to a future monotonic deadline.
 */
START_TEST(test_convert_expire_future)
{
	struct timespec wall_ts, mono_ts;

	clock_gettime(CLOCK_REALTIME, &wall_ts);
	clock_gettime(CLOCK_MONOTONIC, &mono_ts);

	uint64_t now_wall =
		(uint64_t)wall_ts.tv_sec * 1000000000ULL + wall_ts.tv_nsec;
	uint64_t now_mono =
		(uint64_t)mono_ts.tv_sec * 1000000000ULL + mono_ts.tv_nsec;

	/* Expire 10 seconds in the future. */
	nfstime4 expire;
	expire.seconds = wall_ts.tv_sec + 10;
	expire.nseconds = (uint32_t)wall_ts.tv_nsec;

	uint64_t deadline =
		trust_stateid_convert_expire(&expire, now_wall, now_mono);

	ck_assert_uint_gt(deadline, now_mono);
	/* Should be roughly now_mono + 10s (within 1s tolerance). */
	ck_assert_uint_gt(deadline, now_mono + 9000000000ULL);
	ck_assert_uint_lt(deadline, now_mono + 11000000000ULL);
}
END_TEST

/*
 * Expiry already in the past returns 0 (rejected; op handler returns
 * NFS4ERR_INVAL rather than registering an already-expired entry).
 */
START_TEST(test_convert_expire_past)
{
	struct timespec wall_ts, mono_ts;

	clock_gettime(CLOCK_REALTIME, &wall_ts);
	clock_gettime(CLOCK_MONOTONIC, &mono_ts);

	uint64_t now_wall =
		(uint64_t)wall_ts.tv_sec * 1000000000ULL + wall_ts.tv_nsec;
	uint64_t now_mono =
		(uint64_t)mono_ts.tv_sec * 1000000000ULL + mono_ts.tv_nsec;

	/* Expire 10 seconds in the past. */
	nfstime4 expire;
	expire.seconds = wall_ts.tv_sec > 10 ? (int64_t)wall_ts.tv_sec - 10 : 0;
	expire.nseconds = 0;

	uint64_t deadline =
		trust_stateid_convert_expire(&expire, now_wall, now_mono);

	ck_assert_uint_eq(deadline, 0);
}
END_TEST

/*
 * Invalid nseconds (>= 1e9) returns 0.
 */
START_TEST(test_convert_expire_invalid_nsec)
{
	nfstime4 expire;
	expire.seconds = 0;
	expire.nseconds = 1000000000u; /* invalid */

	uint64_t deadline = trust_stateid_convert_expire(&expire, 0, 0);

	ck_assert_uint_eq(deadline, 0);
}
END_TEST

/*
 * Max safe value: remaining_ns near UINT64_MAX should not overflow.
 */
START_TEST(test_convert_expire_no_overflow)
{
	/*
	 * Give a future expire that would overflow if added naively.
	 * The function must return UINT64_MAX as the capped value.
	 */
	nfstime4 expire;
	expire.seconds = UINT32_MAX; /* very far future */
	expire.nseconds = 0;

	/* now values that force remaining_ns to be huge */
	uint64_t now_wall = 1000000000ULL; /* 1 second */
	uint64_t now_mono = UINT64_MAX - 10; /* near max */

	uint64_t deadline =
		trust_stateid_convert_expire(&expire, now_wall, now_mono);

	ck_assert_uint_eq(deadline, UINT64_MAX);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite                                                               */
/* ------------------------------------------------------------------ */

static Suite *trust_stateid_suite(void)
{
	Suite *s = suite_create("trust_stateid");

	TCase *tc_init = tcase_create("init_fini");
	tcase_add_checked_fixture(tc_init, setup, teardown);
	tcase_add_test(tc_init, test_init_fini_idempotent);
	tcase_add_test(tc_init, test_find_before_init);
	suite_add_tcase(s, tc_init);

	TCase *tc_register = tcase_create("register");
	tcase_add_checked_fixture(tc_register, setup, teardown);
	tcase_add_test(tc_register, test_register_basic);
	tcase_add_test(tc_register, test_register_idempotent);
	tcase_add_test(tc_register, test_register_with_principal);
	tcase_add_test(tc_register, test_register_principal_truncated);
	tcase_add_test(tc_register, test_register_two_entries);
	suite_add_tcase(s, tc_register);

	TCase *tc_find = tcase_create("find");
	tcase_add_checked_fixture(tc_find, setup, teardown);
	tcase_add_test(tc_find, test_find_not_found);
	tcase_add_test(tc_find, test_find_refcount);
	tcase_add_test(tc_find, test_put_null);
	tcase_add_test(tc_find, test_flags_active_after_register);
	suite_add_tcase(s, tc_find);

	TCase *tc_revoke = tcase_create("revoke");
	tcase_add_checked_fixture(tc_revoke, setup, teardown);
	tcase_add_test(tc_revoke, test_revoke_removes_entry);
	tcase_add_test(tc_revoke, test_revoke_not_found);
	tcase_add_test(tc_revoke, test_revoke_then_reregister);
	tcase_add_test(tc_revoke, test_bulk_revoke_by_clientid);
	tcase_add_test(tc_revoke, test_bulk_revoke_all);
	tcase_add_test(tc_revoke, test_bulk_revoke_empty);
	suite_add_tcase(s, tc_revoke);

	TCase *tc_expire = tcase_create("convert_expire");
	tcase_add_test(tc_expire, test_convert_expire_future);
	tcase_add_test(tc_expire, test_convert_expire_past);
	tcase_add_test(tc_expire, test_convert_expire_invalid_nsec);
	tcase_add_test(tc_expire, test_convert_expire_no_overflow);
	suite_add_tcase(s, tc_expire);

	return s;
}

/*
 * NOT_NOW_BROWN_COW: Groups B/C/D/E from the design plan (op-handler
 * tests and CHUNK hook tests) are deferred until a compound mock
 * harness that can fabricate struct compound, struct nfs4_client with
 * nc_exchgid_flags, and a stub dispatcher is available.
 *
 * Planned tests (see .claude/design/trust-stateid.md):
 *
 * Group B (TRUST_STATEID op handler):
 *   test_op_trust_stateid_ok
 *   test_op_trust_stateid_not_from_mds        -- NFS4ERR_PERM
 *   test_op_trust_stateid_anon_rejected        -- NFS4ERR_INVAL for special stateid
 *   test_op_trust_stateid_probe_response       -- special stateid -> NFS4ERR_INVAL
 *   test_op_trust_stateid_bad_iomode           -- NFS4ERR_INVAL
 *   test_op_trust_stateid_past_expire          -- NFS4ERR_INVAL
 *   test_op_trust_stateid_no_fh                -- NFS4ERR_NOFILEHANDLE
 *   test_op_trust_stateid_principal_mismatch   -- NFS4ERR_ACCESS
 *
 * Group C (REVOKE_STATEID op handler):
 *   test_op_revoke_stateid_ok
 *   test_op_revoke_stateid_not_from_mds        -- NFS4ERR_PERM
 *   test_op_revoke_stateid_special_stateid     -- NFS4ERR_BAD_STATEID
 *   test_op_revoke_stateid_no_fh               -- NFS4ERR_NOFILEHANDLE
 *
 * Group D (BULK_REVOKE_STATEID op handler):
 *   test_op_bulk_revoke_stateid_ok
 *   test_op_bulk_revoke_stateid_not_from_mds   -- NFS4ERR_PERM
 *
 * Group E (CHUNK hook -- nfs4_op_chunk_write / nfs4_op_chunk_read):
 *   test_chunk_trusted_stateid_allowed
 *   test_chunk_untrusted_stateid_rejected      -- NFS4ERR_BAD_STATEID
 *   test_chunk_expired_stateid_rejected        -- NFS4ERR_BAD_STATEID
 *   test_chunk_pending_stateid_delay           -- NFS4ERR_DELAY
 */

int main(void)
{
	int failed;
	Suite *s = trust_stateid_suite();
	SRunner *sr = srunner_create(s);

	srunner_run_all(sr, CK_NORMAL);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return (failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
