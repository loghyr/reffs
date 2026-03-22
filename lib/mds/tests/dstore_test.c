/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * Unit tests for dstore lifecycle.
 *
 * These tests exercise the hash table, refcount, and state transitions.
 * No real NFS server is contacted — dstore_alloc with unreachable
 * addresses creates dstores in the unmounted state, which is a valid
 * and supported configuration (the MDS logs the error and continues).
 *
 * Tests:
 *   1. init/fini:    global hash table lifecycle
 *   2. alloc/find:   insert and lookup by ID
 *   3. refcount:     get/put lifecycle
 *   4. duplicate ID: rejected by cds_lfht_add_unique
 *   5. unmounted:    dstore_is_available returns false
 *   6. unload_all:   drains the hash table
 */

#include <stdlib.h>
#include <string.h>

#include <check.h>

#include "reffs/dstore.h"
#include "nfs4_test_harness.h"

#define FAKE_DS_ADDR "192.0.2.1"
#define FAKE_DS_PATH "/nonexistent"

static void setup(void)
{
	nfs4_test_setup();
	ck_assert_int_eq(dstore_init(), 0);
}

static void teardown(void)
{
	dstore_fini();
	nfs4_test_teardown();
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

START_TEST(test_init_fini)
{
	/* setup/teardown exercises the normal init/fini path.
	 * Verify dstore_fini is safe to call on an already-finalized
	 * subsystem (double-fini is a no-op). */
	dstore_fini();
	dstore_fini(); /* second call is a no-op */
	/* Reinit so the remaining tests and teardown work. */
	ck_assert_int_eq(dstore_init(), 0);
}
END_TEST

START_TEST(test_alloc_find)
{
	struct dstore *ds = dstore_alloc(42, FAKE_DS_ADDR, FAKE_DS_PATH, false);

	ck_assert_ptr_nonnull(ds);
	ck_assert_uint_eq(ds->ds_id, 42);
	ck_assert_str_eq(ds->ds_address, FAKE_DS_ADDR);
	ck_assert_str_eq(ds->ds_path, FAKE_DS_PATH);

	/* Should be findable. */
	struct dstore *found = dstore_find(42);

	ck_assert_ptr_nonnull(found);
	ck_assert_ptr_eq(found, ds);

	/* Not findable with wrong ID. */
	ck_assert_ptr_null(dstore_find(99));

	dstore_put(found); /* find ref */
	dstore_put(ds);	   /* alloc ref */
}
END_TEST

START_TEST(test_refcount)
{
	struct dstore *ds = dstore_alloc(1, FAKE_DS_ADDR, FAKE_DS_PATH, false);

	ck_assert_ptr_nonnull(ds);

	/* Bump ref. */
	struct dstore *ref2 = dstore_get(ds);

	ck_assert_ptr_eq(ref2, ds);

	/* Drop extra ref — dstore should still be alive. */
	dstore_put(ref2);

	/* Still findable. */
	struct dstore *found = dstore_find(1);

	ck_assert_ptr_nonnull(found);
	dstore_put(found);

	/* Drop alloc ref — hash table still holds one. */
	dstore_put(ds);

	/* Still findable via hash table ref. */
	found = dstore_find(1);
	ck_assert_ptr_nonnull(found);
	dstore_put(found);
}
END_TEST

START_TEST(test_duplicate_id)
{
	struct dstore *ds1 = dstore_alloc(7, FAKE_DS_ADDR, FAKE_DS_PATH, false);

	ck_assert_ptr_nonnull(ds1);

	/* Same ID should fail. */
	struct dstore *ds2 = dstore_alloc(7, "192.0.2.2", "/other", false);

	ck_assert_ptr_null(ds2);

	/* Original still findable. */
	struct dstore *found = dstore_find(7);

	ck_assert_ptr_nonnull(found);
	ck_assert_str_eq(found->ds_address, FAKE_DS_ADDR);
	dstore_put(found);
	dstore_put(ds1);
}
END_TEST

START_TEST(test_unmounted_not_available)
{
	struct dstore *ds = dstore_alloc(3, FAKE_DS_ADDR, FAKE_DS_PATH, false);

	ck_assert_ptr_nonnull(ds);

	/* Mount to a fake address fails — dstore is not available. */
	ck_assert(!dstore_is_available(ds));

	/* But it IS in the hash table. */
	struct dstore *found = dstore_find(3);

	ck_assert_ptr_nonnull(found);
	dstore_put(found);
	dstore_put(ds);
}
END_TEST

START_TEST(test_unload_all)
{
	struct dstore *ds1 = dstore_alloc(10, FAKE_DS_ADDR, FAKE_DS_PATH, false);
	struct dstore *ds2 = dstore_alloc(20, "192.0.2.2", FAKE_DS_PATH, false);

	ck_assert_ptr_nonnull(ds1);
	ck_assert_ptr_nonnull(ds2);

	/* Drop caller refs — hash table still owns them. */
	dstore_put(ds1);
	dstore_put(ds2);

	/* Both findable. */
	struct dstore *f1 = dstore_find(10);
	struct dstore *f2 = dstore_find(20);

	ck_assert_ptr_nonnull(f1);
	ck_assert_ptr_nonnull(f2);
	dstore_put(f1);
	dstore_put(f2);

	/* Drain. */
	dstore_unload_all();

	/* No longer findable. */
	ck_assert_ptr_null(dstore_find(10));
	ck_assert_ptr_null(dstore_find(20));
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite                                                               */
/* ------------------------------------------------------------------ */

Suite *dstore_suite(void)
{
	Suite *s = suite_create("Dstore Lifecycle");

	TCase *tc = tcase_create("lifecycle");

	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_init_fini);
	tcase_add_test(tc, test_alloc_find);
	tcase_add_test(tc, test_refcount);
	tcase_add_test(tc, test_duplicate_id);
	tcase_add_test(tc, test_unmounted_not_available);
	tcase_add_test(tc, test_unload_all);
	suite_add_tcase(s, tc);

	return s;
}

int main(void)
{
	return nfs4_test_run(dstore_suite());
}
