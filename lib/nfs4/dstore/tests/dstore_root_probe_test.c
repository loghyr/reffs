/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * Unit tests for dstore_probe_root_access().
 *
 * A test ops vtable with controllable return values allows each test
 * to simulate success, NFS3ERR_ACCES, and prior-breadcrumb scenarios
 * without a running NFS server.
 *
 * Tests:
 *   1. test_root_probe_success        -- CREATE ok, file removed, available
 *   2. test_root_probe_acces          -- CREATE returns -EACCES, unavailable
 *   3. test_root_probe_breadcrumb_cleanup -- prior .root_probe removed first
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <check.h>

#include "reffs/dstore.h"
#include "reffs/dstore_ops.h"
#include "nfs4_test_harness.h"

/* ------------------------------------------------------------------ */
/* Mock vtable                                                         */
/* ------------------------------------------------------------------ */

/* Shared test state -- reset in each test's setup. */
static int g_create_ret;
static int g_remove_call_count;
static int g_create_call_count;
static char g_remove_names[8][64];

static int probe_test_create(struct dstore *ds __attribute__((unused)),
			     const uint8_t *dir_fh __attribute__((unused)),
			     uint32_t dir_fh_len __attribute__((unused)),
			     const char *name __attribute__((unused)),
			     uint8_t *out_fh, uint32_t *out_fh_len)
{
	g_create_call_count++;
	if (g_create_ret == 0) {
		/* Provide a minimal fake FH on success. */
		if (out_fh)
			out_fh[0] = 0x42;
		if (out_fh_len)
			*out_fh_len = 1;
	}
	return g_create_ret;
}

static int probe_test_remove(struct dstore *ds __attribute__((unused)),
			     const uint8_t *dir_fh __attribute__((unused)),
			     uint32_t dir_fh_len __attribute__((unused)),
			     const char *name)
{
	if (g_remove_call_count < 8)
		strncpy(g_remove_names[g_remove_call_count], name,
			sizeof(g_remove_names[0]) - 1);
	g_remove_call_count++;
	return 0;
}

static const struct dstore_ops dstore_ops_probe_test = {
	.name = "probe_test",
	.create = probe_test_create,
	.remove = probe_test_remove,
};

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/*
 * Allocate an unmounted dstore and inject the test vtable.
 * The dstore is hashed; caller must dstore_put() after use.
 */
static struct dstore *make_probe_dstore(uint32_t id)
{
	struct dstore *ds = dstore_alloc(id, "192.0.2.1", 0, "/test",
					 REFFS_DS_PROTO_NFSV3, false, false);
	if (!ds)
		return NULL;

	/* Inject test vtable (cast away const for testing). */
	*(const struct dstore_ops **)&ds->ds_ops = &dstore_ops_probe_test;

	/* Mark as if MOUNT succeeded so the probe runs against our vtable. */
	__atomic_or_fetch(&ds->ds_state, DSTORE_IS_MOUNTED, __ATOMIC_RELEASE);

	return ds;
}

/* ------------------------------------------------------------------ */
/* Setup / teardown                                                    */
/* ------------------------------------------------------------------ */

static void setup(void)
{
	nfs4_test_setup();
	ck_assert_int_eq(dstore_init(), 0);

	g_create_ret = 0;
	g_remove_call_count = 0;
	g_create_call_count = 0;
	memset(g_remove_names, 0, sizeof(g_remove_names));
}

static void teardown(void)
{
	dstore_fini();
	nfs4_test_teardown();
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

/*
 * test_root_probe_success -- CREATE succeeds; probe file is removed;
 * dstore remains available.
 */
START_TEST(test_root_probe_success)
{
	struct dstore *ds = make_probe_dstore(1);

	ck_assert_ptr_nonnull(ds);

	g_create_ret = 0;

	int ret = dstore_probe_root_access(ds);

	ck_assert_int_eq(ret, 0);

	/* Breadcrumb cleanup (remove before create) + post-create remove. */
	ck_assert_int_ge(g_remove_call_count, 2);

	/* Dstore must still be available. */
	ck_assert(dstore_is_available(ds));

	dstore_put(ds);
}
END_TEST

/*
 * test_root_probe_acces -- CREATE returns -EACCES (root_squash set);
 * probe returns -EACCES; caller would clear MOUNTED.
 */
START_TEST(test_root_probe_acces)
{
	struct dstore *ds = make_probe_dstore(2);

	ck_assert_ptr_nonnull(ds);

	g_create_ret = -EACCES;

	int ret = dstore_probe_root_access(ds);

	ck_assert_int_eq(ret, -EACCES);

	/*
	 * Probe returns -EACCES; dstore_alloc() clears MOUNTED on this
	 * return.  The probe itself does not clear the flag -- that is the
	 * caller's responsibility.  Verify create was attempted.
	 */
	ck_assert_int_ge(g_create_call_count, 1);

	dstore_put(ds);
}
END_TEST

/*
 * test_root_probe_breadcrumb_cleanup -- prior .root_probe must be removed
 * before the new probe is created.
 */
START_TEST(test_root_probe_breadcrumb_cleanup)
{
	struct dstore *ds = make_probe_dstore(3);

	ck_assert_ptr_nonnull(ds);

	g_create_ret = 0;

	int ret = dstore_probe_root_access(ds);

	ck_assert_int_eq(ret, 0);

	/*
	 * The first REMOVE call is the breadcrumb cleanup before CREATE.
	 * The second REMOVE call cleans up the new probe file.
	 * Verify the breadcrumb removal happened: first remove name is
	 * ".root_probe".
	 */
	ck_assert_int_ge(g_remove_call_count, 2);
	ck_assert_str_eq(g_remove_names[0], ".root_probe");
	ck_assert_str_eq(g_remove_names[1], ".root_probe");

	dstore_put(ds);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite                                                               */
/* ------------------------------------------------------------------ */

Suite *dstore_root_probe_suite(void)
{
	Suite *s = suite_create("dstore_root_probe");
	TCase *tc = tcase_create("probe");

	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_root_probe_success);
	tcase_add_test(tc, test_root_probe_acces);
	tcase_add_test(tc, test_root_probe_breadcrumb_cleanup);

	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	Suite *s = dstore_root_probe_suite();
	SRunner *sr = srunner_create(s);

	srunner_run_all(sr, CK_NORMAL);
	int failed = srunner_ntests_failed(sr);

	srunner_free(sr);
	return failed ? 1 : 0;
}
