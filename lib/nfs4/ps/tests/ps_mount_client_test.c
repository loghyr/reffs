/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <check.h>
#include <errno.h>
#include <stddef.h>

#include "ps_mount_client.h"

/*
 * Null args return -EINVAL rather than crash.  The registry guards
 * against misuse before ever touching the network.
 */
START_TEST(test_fetch_invalid_args)
{
	struct ps_export_entry *arr = NULL;
	size_t n = 0;

	ck_assert_int_eq(ps_mount_fetch_exports(NULL, &arr, &n), -EINVAL);
	ck_assert_int_eq(ps_mount_fetch_exports("", &arr, &n), -EINVAL);
	ck_assert_int_eq(ps_mount_fetch_exports("127.0.0.1", NULL, &n),
			 -EINVAL);
	ck_assert_int_eq(ps_mount_fetch_exports("127.0.0.1", &arr, NULL),
			 -EINVAL);
}
END_TEST

/*
 * Free is NULL-tolerant -- matches the project convention for other
 * put / free helpers (inode_put, super_block_put, etc.) and simplifies
 * caller cleanup on error paths.
 */
START_TEST(test_free_null)
{
	ps_mount_free_exports(NULL);
}
END_TEST

/*
 * Attempt to fetch from a host / port with nothing listening (port 1
 * on loopback is reliably refused on Linux).  The exact errno is
 * implementation-dependent (TIRPC's clnt_create can map various
 * connect failures) so we just require that the call fails cleanly
 * rather than crashing or leaking: out pointer stays NULL, out count
 * stays 0, return is negative.
 *
 * Skipped note: this still goes through libtirpc's portmap lookup,
 * which in some CI environments may have longer timeouts than unit
 * tests tolerate.  The check is guarded by a short internal TIRPC
 * timeout; if CI flakiness appears, this test would be the first to
 * mark as timed/flaky, not the first to remove.
 */
START_TEST(test_fetch_connect_refused)
{
	struct ps_export_entry *arr = (struct ps_export_entry *)0xDEAD;
	size_t n = 42;

	int r = ps_mount_fetch_exports("127.0.0.1", &arr, &n);

	ck_assert_int_lt(r, 0);
	ck_assert_ptr_null(arr);
	ck_assert_uint_eq(n, 0);
}
END_TEST

static Suite *ps_mount_client_suite(void)
{
	Suite *s = suite_create("ps_mount_client");
	TCase *tc = tcase_create("core");

	tcase_add_test(tc, test_fetch_invalid_args);
	tcase_add_test(tc, test_free_null);
	tcase_add_test(tc, test_fetch_connect_refused);
	/*
	 * Keep the refused-connect test inside the default 2s budget --
	 * libtirpc's internal pmap lookup usually errors out quickly on
	 * loopback.  Bump explicitly if this flakes.
	 */
	tcase_set_timeout(tc, 5);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int failed;
	SRunner *sr = srunner_create(ps_mount_client_suite());

	srunner_run_all(sr, CK_NORMAL);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return failed == 0 ? 0 : 1;
}
