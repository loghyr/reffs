/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * Unit tests for dstore_wcc_check().
 *
 * Tests:
 *   1. null_wcc:           NULL wcc pointer -- no crash, no update
 *   2. invalid_wcc:        wcc_valid=0 -- no update
 *   3. null_ldf:           NULL ldf pointer -- no crash
 *   4. update_attrs:       valid WCC updates ldf cached attrs
 *   5. no_update_on_fail:  invalid WCC leaves ldf unchanged
 *   6. backwards_mtime:    mtime goes backwards -- ldf still updated
 *   7. backwards_ctime:    ctime goes backwards -- ldf still updated
 *   8. wwwl_no_layout:     changed attrs without write layout -- ldf updated
 *   9. no_wwwl_with_layout: changed attrs with write layout -- ldf updated
 *  10. atime_only_change:  only atime differs -- no WWWL (ldf updated)
 */

#include <string.h>

#include <check.h>

#include "reffs/dstore.h"
#include "reffs/dstore_ops.h"
#include "reffs/dstore_wcc.h"
#include "reffs/layout_segment.h"
#include "nfs4_test_harness.h"

static struct layout_data_file ldf;
static struct dstore_wcc wcc;

static void setup(void)
{
	nfs4_test_setup();
	memset(&ldf, 0, sizeof(ldf));
	memset(&wcc, 0, sizeof(wcc));

	/* Seed ldf with known cached attrs. */
	ldf.ldf_dstore_id = 1;
	ldf.ldf_size = 1000;
	ldf.ldf_mtime.tv_sec = 100;
	ldf.ldf_mtime.tv_nsec = 0;
	ldf.ldf_ctime.tv_sec = 100;
	ldf.ldf_ctime.tv_nsec = 0;
}

static void teardown(void)
{
	nfs4_test_teardown();
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

START_TEST(test_null_wcc)
{
	int64_t orig_size = ldf.ldf_size;

	dstore_wcc_check(NULL, &ldf, false, 1, 42);

	/* ldf unchanged. */
	ck_assert_int_eq(ldf.ldf_size, orig_size);
}
END_TEST

START_TEST(test_invalid_wcc)
{
	wcc.wcc_valid = 0;
	wcc.wcc_size = 9999;
	wcc.wcc_mtime.tv_sec = 999;

	dstore_wcc_check(&wcc, &ldf, false, 1, 42);

	/* ldf unchanged because wcc_valid=0. */
	ck_assert_int_eq(ldf.ldf_size, 1000);
	ck_assert_int_eq(ldf.ldf_mtime.tv_sec, 100);
}
END_TEST

START_TEST(test_null_ldf)
{
	wcc.wcc_valid = 1;
	wcc.wcc_size = 2000;

	/* Should not crash. */
	dstore_wcc_check(&wcc, NULL, false, 1, 42);
}
END_TEST

START_TEST(test_update_attrs)
{
	wcc.wcc_valid = 1;
	wcc.wcc_size = 2000;
	wcc.wcc_mtime.tv_sec = 200;
	wcc.wcc_mtime.tv_nsec = 500;
	wcc.wcc_ctime.tv_sec = 200;
	wcc.wcc_ctime.tv_nsec = 500;

	dstore_wcc_check(&wcc, &ldf, true, 1, 42);

	/* ldf updated from WCC. */
	ck_assert_int_eq(ldf.ldf_size, 2000);
	ck_assert_int_eq(ldf.ldf_mtime.tv_sec, 200);
	ck_assert_int_eq(ldf.ldf_mtime.tv_nsec, 500);
	ck_assert_int_eq(ldf.ldf_ctime.tv_sec, 200);
	ck_assert_int_eq(ldf.ldf_ctime.tv_nsec, 500);
}
END_TEST

START_TEST(test_no_update_on_fail)
{
	wcc.wcc_valid = 0;
	wcc.wcc_size = 5000;

	dstore_wcc_check(&wcc, &ldf, true, 1, 42);

	/* ldf unchanged. */
	ck_assert_int_eq(ldf.ldf_size, 1000);
}
END_TEST

START_TEST(test_backwards_mtime)
{
	/* mtime goes backwards: 100 -> 50 */
	wcc.wcc_valid = 1;
	wcc.wcc_size = 1000;
	wcc.wcc_mtime.tv_sec = 50;
	wcc.wcc_mtime.tv_nsec = 0;
	wcc.wcc_ctime.tv_sec = 100;
	wcc.wcc_ctime.tv_nsec = 0;

	dstore_wcc_check(&wcc, &ldf, true, 1, 42);

	/* ldf updated even though mtime went backwards (LOG emitted). */
	ck_assert_int_eq(ldf.ldf_mtime.tv_sec, 50);
}
END_TEST

START_TEST(test_backwards_ctime)
{
	/* ctime goes backwards: 100 -> 50 */
	wcc.wcc_valid = 1;
	wcc.wcc_size = 1000;
	wcc.wcc_mtime.tv_sec = 100;
	wcc.wcc_mtime.tv_nsec = 0;
	wcc.wcc_ctime.tv_sec = 50;
	wcc.wcc_ctime.tv_nsec = 0;

	dstore_wcc_check(&wcc, &ldf, true, 1, 42);

	/* ldf updated even though ctime went backwards (LOG emitted). */
	ck_assert_int_eq(ldf.ldf_ctime.tv_sec, 50);
}
END_TEST

START_TEST(test_wwwl_no_layout)
{
	/* mtime changed, no write layout -- WWWL logged. */
	wcc.wcc_valid = 1;
	wcc.wcc_size = 2000;
	wcc.wcc_mtime.tv_sec = 200;
	wcc.wcc_mtime.tv_nsec = 0;
	wcc.wcc_ctime.tv_sec = 200;
	wcc.wcc_ctime.tv_nsec = 0;

	dstore_wcc_check(&wcc, &ldf, false, 1, 42);

	/* ldf updated regardless. */
	ck_assert_int_eq(ldf.ldf_size, 2000);
	ck_assert_int_eq(ldf.ldf_mtime.tv_sec, 200);
}
END_TEST

START_TEST(test_no_wwwl_with_layout)
{
	/* Same changed attrs but with write layout -- no WWWL. */
	wcc.wcc_valid = 1;
	wcc.wcc_size = 2000;
	wcc.wcc_mtime.tv_sec = 200;
	wcc.wcc_mtime.tv_nsec = 0;
	wcc.wcc_ctime.tv_sec = 200;
	wcc.wcc_ctime.tv_nsec = 0;

	dstore_wcc_check(&wcc, &ldf, true, 1, 42);

	/* ldf updated. */
	ck_assert_int_eq(ldf.ldf_size, 2000);
}
END_TEST

START_TEST(test_atime_only_change)
{
	/*
	 * mtime and ctime unchanged, only atime differs.
	 * This is NOT a WWWL -- reads legitimately update atime.
	 */
	wcc.wcc_valid = 1;
	wcc.wcc_size = 1000;
	wcc.wcc_mtime.tv_sec = 100;
	wcc.wcc_mtime.tv_nsec = 0;
	wcc.wcc_ctime.tv_sec = 100;
	wcc.wcc_ctime.tv_nsec = 0;

	dstore_wcc_check(&wcc, &ldf, false, 1, 42);

	/* ldf attrs match -- no change visible. */
	ck_assert_int_eq(ldf.ldf_size, 1000);
	ck_assert_int_eq(ldf.ldf_mtime.tv_sec, 100);
	ck_assert_int_eq(ldf.ldf_ctime.tv_sec, 100);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite                                                               */
/* ------------------------------------------------------------------ */

Suite *dstore_wcc_suite(void)
{
	Suite *s = suite_create("Dstore WCC Check");

	TCase *tc = tcase_create("wcc_check");

	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_null_wcc);
	tcase_add_test(tc, test_invalid_wcc);
	tcase_add_test(tc, test_null_ldf);
	tcase_add_test(tc, test_update_attrs);
	tcase_add_test(tc, test_no_update_on_fail);
	tcase_add_test(tc, test_backwards_mtime);
	tcase_add_test(tc, test_backwards_ctime);
	tcase_add_test(tc, test_wwwl_no_layout);
	tcase_add_test(tc, test_no_wwwl_with_layout);
	tcase_add_test(tc, test_atime_only_change);
	suite_add_tcase(s, tc);

	return s;
}

int main(void)
{
	return nfs4_test_run(dstore_wcc_suite());
}
