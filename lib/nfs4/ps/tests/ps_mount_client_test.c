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

	ck_assert_int_eq(ps_mount_fetch_exports(NULL, 0, &arr, &n), -EINVAL);
	ck_assert_int_eq(ps_mount_fetch_exports("", 0, &arr, &n), -EINVAL);
	ck_assert_int_eq(ps_mount_fetch_exports("127.0.0.1", 0, NULL, &n),
			 -EINVAL);
	ck_assert_int_eq(ps_mount_fetch_exports("127.0.0.1", 0, &arr, NULL),
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

	int r = ps_mount_fetch_exports("127.0.0.1", 0, &arr, &n);

	ck_assert_int_lt(r, 0);
	ck_assert_ptr_null(arr);
	ck_assert_uint_eq(n, 0);
}
END_TEST

/*
 * Same shape as test_fetch_connect_refused but exercises the
 * explicit-port code path (port > 0 takes clnttcp_create instead
 * of clnt_create).  Port 1 on loopback is reliably refused (not
 * listening, no portmap involvement).  Failing cleanly here proves
 * the explicit-port path's connect-failure handling matches the
 * portmap path's (out=NULL, n=0, return < 0).
 */
START_TEST(test_fetch_connect_refused_explicit_port)
{
	struct ps_export_entry *arr = (struct ps_export_entry *)0xDEAD;
	size_t n = 42;

	int r = ps_mount_fetch_exports("127.0.0.1", 1, &arr, &n);

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
	/*
	 * test_fetch_connect_refused is DISABLED while libtirpc's
	 * clnt_create connect-refused path leaks 1 byte that LSAN
	 * flags as a hard failure on reffs-ci.  The leak is in
	 * libtirpc's portmap probe internals -- not in reffs code --
	 * and there is no clean ASAN suppression for an unnamed-
	 * symbol allocation inside the .so.  Re-enable when fixed
	 * upstream or once we add a .lsan_suppressions file scoped
	 * to libtirpc.so.  Tracked: GitHub issue #57.
	 */
	(void)test_fetch_connect_refused;
	/*
	 * test_fetch_connect_refused_explicit_port exercises the new
	 * clnttcp_create explicit-port path (port > 0 in
	 * ps_mount_fetch_exports).  Enabled because the explicit-port
	 * path does NOT call into libtirpc's pmap_set /
	 * authunix_create_default leak surface (issue #57 affects only
	 * the portmap path).  This is the only test giving make-check
	 * coverage of the new code path the slice exists to add.
	 */
	tcase_add_test(tc, test_fetch_connect_refused_explicit_port);
	/*
	 * test_fetch_connect_refused calls libtirpc clnt_create against a
	 * host with no rpcbind / portmap listener.  On Darwin (and on
	 * Linux hosts where rpcbind is not running) TIRPC's portmap
	 * probe to port 111 takes ~5 seconds to fail before clnt_create
	 * gives up and returns NULL.  Five back-to-back local runs on
	 * Darwin all measured 5.03s total.  A 5s libcheck deadline hits
	 * that boundary and trips intermittently.  15s gives enough
	 * headroom for TIRPC's slow-fail path on every supported host
	 * without slowing CI meaningfully when the test passes -- the
	 * happy path is still ~5s and well under the bumped budget.
	 */
	tcase_set_timeout(tc, 15);
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
