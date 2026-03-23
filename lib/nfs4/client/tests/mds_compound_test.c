/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * Unit tests for the MDS COMPOUND builder.
 *
 * These tests exercise the compound init/fini, op addition, overflow,
 * sequence insertion, and result accessor — all without a live server.
 */

#include <check.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "nfsv42_xdr.h"
#include "ec_client.h"

/* ------------------------------------------------------------------ */
/* Init / fini                                                         */
/* ------------------------------------------------------------------ */

START_TEST(test_compound_init_basic)
{
	struct mds_compound mc;
	int ret = mds_compound_init(&mc, 4, "test");

	ck_assert_int_eq(ret, 0);
	ck_assert_uint_eq(mc.mc_max_ops, 4);
	ck_assert_uint_eq(mc.mc_count, 0);
	ck_assert_uint_eq(mc.mc_args.minorversion, 2);
	ck_assert_uint_eq(mc.mc_args.tag.utf8string_len, 4);
	ck_assert_str_eq(mc.mc_args.tag.utf8string_val, "test");

	mds_compound_fini(&mc);
}
END_TEST

START_TEST(test_compound_init_null_tag)
{
	struct mds_compound mc;
	int ret = mds_compound_init(&mc, 2, NULL);

	ck_assert_int_eq(ret, 0);
	ck_assert_uint_eq(mc.mc_args.tag.utf8string_len, 0);

	mds_compound_fini(&mc);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Add ops                                                             */
/* ------------------------------------------------------------------ */

START_TEST(test_add_op_single)
{
	struct mds_compound mc;

	mds_compound_init(&mc, 4, "t");

	nfs_argop4 *slot = mds_compound_add_op(&mc, OP_PUTROOTFH);

	ck_assert_ptr_nonnull(slot);
	ck_assert_uint_eq(slot->argop, OP_PUTROOTFH);
	ck_assert_uint_eq(mc.mc_count, 1);
	ck_assert_uint_eq(mc.mc_args.argarray.argarray_len, 1);

	mds_compound_fini(&mc);
}
END_TEST

START_TEST(test_add_op_fills_to_max)
{
	struct mds_compound mc;

	mds_compound_init(&mc, 3, "t");

	ck_assert_ptr_nonnull(mds_compound_add_op(&mc, OP_PUTROOTFH));
	ck_assert_ptr_nonnull(mds_compound_add_op(&mc, OP_OPEN));
	ck_assert_ptr_nonnull(mds_compound_add_op(&mc, OP_GETFH));
	ck_assert_uint_eq(mc.mc_count, 3);

	mds_compound_fini(&mc);
}
END_TEST

START_TEST(test_add_op_overflow)
{
	struct mds_compound mc;

	mds_compound_init(&mc, 2, "t");

	ck_assert_ptr_nonnull(mds_compound_add_op(&mc, OP_PUTROOTFH));
	ck_assert_ptr_nonnull(mds_compound_add_op(&mc, OP_OPEN));

	/* Third op should fail — max is 2. */
	nfs_argop4 *overflow = mds_compound_add_op(&mc, OP_GETFH);

	ck_assert_ptr_null(overflow);
	ck_assert_uint_eq(mc.mc_count, 2);

	mds_compound_fini(&mc);
}
END_TEST

/* ------------------------------------------------------------------ */
/* SEQUENCE helper                                                     */
/* ------------------------------------------------------------------ */

START_TEST(test_add_sequence)
{
	struct mds_compound mc;
	struct mds_session ms;

	memset(&ms, 0, sizeof(ms));
	memset(ms.ms_sessionid, 0xAB, sizeof(sessionid4));
	ms.ms_slot_seqid = 42;

	mds_compound_init(&mc, 4, "t");

	int ret = mds_compound_add_sequence(&mc, &ms);

	ck_assert_int_eq(ret, 0);
	ck_assert_uint_eq(mc.mc_count, 1);

	/* Verify the SEQUENCE args were filled correctly. */
	nfs_argop4 *slot = &mc.mc_args.argarray.argarray_val[0];

	ck_assert_uint_eq(slot->argop, OP_SEQUENCE);

	SEQUENCE4args *seq = &slot->nfs_argop4_u.opsequence;

	ck_assert_uint_eq(seq->sa_sequenceid, 42);
	ck_assert_uint_eq(seq->sa_slotid, 0);
	ck_assert_uint_eq(seq->sa_highest_slotid, 0);

	/* Verify sessionid was copied. */
	for (int i = 0; i < NFS4_SESSIONID_SIZE; i++)
		ck_assert_uint_eq((uint8_t)seq->sa_sessionid[i], 0xAB);

	mds_compound_fini(&mc);
}
END_TEST

START_TEST(test_add_sequence_overflow)
{
	struct mds_compound mc;
	struct mds_session ms;

	memset(&ms, 0, sizeof(ms));
	ms.ms_slot_seqid = 1;

	/* Only room for 1 op, SEQUENCE fills it. */
	mds_compound_init(&mc, 1, "t");

	int ret = mds_compound_add_sequence(&mc, &ms);

	ck_assert_int_eq(ret, 0);

	/* Second add should fail. */
	ret = mds_compound_add_sequence(&mc, &ms);
	ck_assert_int_eq(ret, -ENOSPC);

	mds_compound_fini(&mc);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Result accessor                                                     */
/* ------------------------------------------------------------------ */

START_TEST(test_result_accessor_null)
{
	struct mds_compound mc;

	mds_compound_init(&mc, 2, "t");

	/* No response yet — resarray_len is 0. */
	ck_assert_ptr_null(mds_compound_result(&mc, 0));
	ck_assert_ptr_null(mds_compound_result(&mc, 5));

	mds_compound_fini(&mc);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite setup                                                         */
/* ------------------------------------------------------------------ */

static Suite *mds_compound_suite(void)
{
	Suite *s = suite_create("mds_compound");
	TCase *tc = tcase_create("core");

	tcase_add_test(tc, test_compound_init_basic);
	tcase_add_test(tc, test_compound_init_null_tag);
	tcase_add_test(tc, test_add_op_single);
	tcase_add_test(tc, test_add_op_fills_to_max);
	tcase_add_test(tc, test_add_op_overflow);
	tcase_add_test(tc, test_add_sequence);
	tcase_add_test(tc, test_add_sequence_overflow);
	tcase_add_test(tc, test_result_accessor_null);

	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int failed;
	Suite *s = mds_compound_suite();
	SRunner *sr = srunner_create(s);

	srunner_run_all(sr, CK_NORMAL);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return (failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
