/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <check.h>
#include <errno.h>
#include <string.h>

#include "reffs/settings.h"

#include "ps_state.h"

static void setup(void)
{
	ps_state_init();
}

static void teardown(void)
{
	ps_state_fini();
}

static struct reffs_proxy_mds_config make_cfg(uint32_t id, const char *addr)
{
	struct reffs_proxy_mds_config cfg;

	memset(&cfg, 0, sizeof(cfg));
	cfg.id = id;
	cfg.port = 4098;
	cfg.mds_port = 2049;
	cfg.mds_probe = 20490;
	if (addr)
		strncpy(cfg.address, addr, sizeof(cfg.address) - 1);
	return cfg;
}

/*
 * Empty registry returns NULL for every lookup, including id 0
 * (native listener, never present here) and unregistered ids.
 */
START_TEST(test_find_empty_returns_null)
{
	ck_assert_ptr_null(ps_state_find(0));
	ck_assert_ptr_null(ps_state_find(1));
	ck_assert_ptr_null(ps_state_find(999));
}
END_TEST

/*
 * Register one entry, verify every field round-trips and the
 * returned pointer addresses the registry slot (stable for the
 * registry's lifetime).
 */
START_TEST(test_register_and_find)
{
	struct reffs_proxy_mds_config c = make_cfg(1, "10.0.0.5");

	ck_assert_int_eq(ps_state_register(&c), 0);

	const struct ps_listener_state *pls = ps_state_find(1);

	ck_assert_ptr_nonnull(pls);
	ck_assert_uint_eq(pls->pls_listener_id, 1);
	ck_assert_str_eq(pls->pls_upstream, "10.0.0.5");
	ck_assert_uint_eq(pls->pls_upstream_port, 2049);
	ck_assert_uint_eq(pls->pls_upstream_probe, 20490);

	/* Second call returns the same pointer. */
	ck_assert_ptr_eq(pls, ps_state_find(1));
}
END_TEST

/*
 * Empty address still registers.  A proxy_mds entry without an
 * upstream address is legal config -- the listener is up, no
 * discovery / forwarding is attempted.  Distinguished via
 * pls_upstream[0] == '\0'.
 */
START_TEST(test_register_empty_address)
{
	struct reffs_proxy_mds_config c = make_cfg(2, "");

	ck_assert_int_eq(ps_state_register(&c), 0);

	const struct ps_listener_state *pls = ps_state_find(2);

	ck_assert_ptr_nonnull(pls);
	ck_assert_str_eq(pls->pls_upstream, "");
	ck_assert_uint_eq(pls->pls_upstream_port, 2049);
}
END_TEST

/*
 * listener_id 0 is reserved for the native listener.  A proxy_mds
 * entry with id 0 is a config error that reffsd.c already logs and
 * skips; the registry rejects it as a safety net so callers can't
 * accidentally make the native listener findable here.
 */
START_TEST(test_register_id_zero_rejected)
{
	struct reffs_proxy_mds_config c = make_cfg(0, "10.0.0.5");

	ck_assert_int_eq(ps_state_register(&c), -EINVAL);
	ck_assert_ptr_null(ps_state_find(0));
}
END_TEST

/*
 * A second register for an id already in the table returns -EEXIST
 * and leaves the first entry intact.  reffsd.c can therefore call
 * register() unconditionally per proxy_mds entry and rely on this
 * check to catch a duplicate id in the config.
 */
START_TEST(test_register_duplicate_rejected)
{
	struct reffs_proxy_mds_config c1 = make_cfg(1, "10.0.0.5");
	struct reffs_proxy_mds_config c2 = make_cfg(1, "10.0.0.6");

	ck_assert_int_eq(ps_state_register(&c1), 0);
	ck_assert_int_eq(ps_state_register(&c2), -EEXIST);

	const struct ps_listener_state *pls = ps_state_find(1);

	ck_assert_str_eq(pls->pls_upstream, "10.0.0.5");
}
END_TEST

/*
 * Register up to the configured max; ensure each is separately
 * findable and an unregistered id returns NULL.
 */
START_TEST(test_register_multiple)
{
	struct reffs_proxy_mds_config c1 = make_cfg(1, "10.0.0.5");
	struct reffs_proxy_mds_config c2 = make_cfg(2, "10.0.0.6");
	struct reffs_proxy_mds_config c3 = make_cfg(3, "");

	ck_assert_int_eq(ps_state_register(&c1), 0);
	ck_assert_int_eq(ps_state_register(&c2), 0);
	ck_assert_int_eq(ps_state_register(&c3), 0);

	ck_assert_uint_eq(ps_state_find(1)->pls_listener_id, 1);
	ck_assert_str_eq(ps_state_find(1)->pls_upstream, "10.0.0.5");
	ck_assert_uint_eq(ps_state_find(2)->pls_listener_id, 2);
	ck_assert_str_eq(ps_state_find(2)->pls_upstream, "10.0.0.6");
	ck_assert_uint_eq(ps_state_find(3)->pls_listener_id, 3);
	ck_assert_str_eq(ps_state_find(3)->pls_upstream, "");

	ck_assert_ptr_null(ps_state_find(4));
}
END_TEST

/*
 * fini clears the registry; a subsequent init restores a clean
 * state and prior registrations are gone.  Important so the
 * single-process test runner can't leak state between tests (the
 * fixture does this, but the invariant is worth asserting).
 */
START_TEST(test_fini_clears_registry)
{
	struct reffs_proxy_mds_config c = make_cfg(1, "10.0.0.5");

	ck_assert_int_eq(ps_state_register(&c), 0);
	ps_state_fini();
	ps_state_init();

	ck_assert_ptr_null(ps_state_find(1));
	/* Can re-register after fini/init with no false-duplicate error. */
	ck_assert_int_eq(ps_state_register(&c), 0);
}
END_TEST

/*
 * A NULL config is rejected cleanly, not crashed.
 */
START_TEST(test_register_null_rejected)
{
	ck_assert_int_eq(ps_state_register(NULL), -EINVAL);
}
END_TEST

static Suite *ps_state_suite(void)
{
	Suite *s = suite_create("ps_state");
	TCase *tc = tcase_create("core");

	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_find_empty_returns_null);
	tcase_add_test(tc, test_register_and_find);
	tcase_add_test(tc, test_register_empty_address);
	tcase_add_test(tc, test_register_id_zero_rejected);
	tcase_add_test(tc, test_register_duplicate_rejected);
	tcase_add_test(tc, test_register_multiple);
	tcase_add_test(tc, test_fini_clears_registry);
	tcase_add_test(tc, test_register_null_rejected);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int failed;
	SRunner *sr = srunner_create(ps_state_suite());

	srunner_run_all(sr, CK_NORMAL);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return failed == 0 ? 0 : 1;
}
