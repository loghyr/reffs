/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <check.h>
#include <errno.h>
#include <stdint.h>
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

/*
 * A freshly-registered listener has pls_session == NULL.  The
 * session is attached later by reffsd.c after mds_session_create()
 * succeeds; until then, op handlers that read pls_session see NULL
 * and must fail gracefully.
 */
START_TEST(test_session_defaults_null)
{
	struct reffs_proxy_mds_config c = make_cfg(1, "10.0.0.5");

	ck_assert_int_eq(ps_state_register(&c), 0);
	ck_assert_ptr_null(ps_state_find(1)->pls_session);
}
END_TEST

/*
 * ps_state_set_session stores the pointer verbatim.  Registry does
 * not dereference or own-destroy -- the caller is the owner.  A
 * sentinel non-NULL pointer (cast from an integer) is enough to
 * prove round-trip storage; no real mds_session is needed for this
 * contract.
 */
START_TEST(test_set_session_stores_pointer)
{
	struct reffs_proxy_mds_config c = make_cfg(1, "10.0.0.5");
	struct mds_session *sentinel =
		(struct mds_session *)(uintptr_t)0xDEADBEEF;

	ck_assert_int_eq(ps_state_register(&c), 0);
	ck_assert_int_eq(ps_state_set_session(1, sentinel), 0);
	ck_assert_ptr_eq(ps_state_find(1)->pls_session, sentinel);

	/* NULL clears the stored pointer (used at shutdown). */
	ck_assert_int_eq(ps_state_set_session(1, NULL), 0);
	ck_assert_ptr_null(ps_state_find(1)->pls_session);
}
END_TEST

/*
 * Setting the session on an unregistered listener returns -ENOENT,
 * not a silent no-op -- callers need to know the registry is in an
 * unexpected state.
 */
START_TEST(test_set_session_unknown_id_fails)
{
	struct mds_session *sentinel =
		(struct mds_session *)(uintptr_t)0xDEADBEEF;

	/* Registry is empty (setup calls ps_state_init). */
	ck_assert_int_eq(ps_state_set_session(42, sentinel), -ENOENT);
	ck_assert_int_eq(ps_state_set_session(0, sentinel), -ENOENT);
}
END_TEST

/*
 * A freshly-registered listener has no MDS root FH stored
 * (pls_mds_root_fh_len == 0).  Discovery populates this after
 * mds_session_create succeeds.
 */
START_TEST(test_mds_root_fh_defaults_empty)
{
	struct reffs_proxy_mds_config c = make_cfg(1, "10.0.0.5");

	ck_assert_int_eq(ps_state_register(&c), 0);
	ck_assert_uint_eq(ps_state_find(1)->pls_mds_root_fh_len, 0);
}
END_TEST

/*
 * set_mds_root_fh copies the bytes verbatim and records the length.
 * Caller-owned buffer -- registry does not keep a reference to it.
 */
START_TEST(test_set_mds_root_fh_stores_bytes)
{
	struct reffs_proxy_mds_config c = make_cfg(1, "10.0.0.5");
	uint8_t fh[] = { 0x01, 0x02, 0x03, 0x04, 0x05 };

	ck_assert_int_eq(ps_state_register(&c), 0);
	ck_assert_int_eq(ps_state_set_mds_root_fh(1, fh, sizeof(fh)), 0);

	const struct ps_listener_state *pls = ps_state_find(1);

	ck_assert_uint_eq(pls->pls_mds_root_fh_len, sizeof(fh));
	ck_assert_mem_eq(pls->pls_mds_root_fh, fh, sizeof(fh));

	/* Mutating the caller's buffer does not disturb the stored copy. */
	fh[0] = 0xFF;
	ck_assert_uint_eq(pls->pls_mds_root_fh[0], 0x01);
}
END_TEST

/*
 * Clearing (fh_len=0) is legal and does NOT require a non-NULL
 * buffer -- it's the "forget what we learned" path.
 */
START_TEST(test_set_mds_root_fh_clear)
{
	struct reffs_proxy_mds_config c = make_cfg(1, "10.0.0.5");
	uint8_t fh[] = { 0x01, 0x02, 0x03 };

	ck_assert_int_eq(ps_state_register(&c), 0);
	ck_assert_int_eq(ps_state_set_mds_root_fh(1, fh, sizeof(fh)), 0);
	ck_assert_int_eq(ps_state_set_mds_root_fh(1, NULL, 0), 0);
	ck_assert_uint_eq(ps_state_find(1)->pls_mds_root_fh_len, 0);
}
END_TEST

/*
 * NFSv4 FHs are <= 128 bytes (RFC 8881).  An over-size buffer
 * returns -E2BIG so the caller can't silently truncate.
 */
START_TEST(test_set_mds_root_fh_too_big)
{
	struct reffs_proxy_mds_config c = make_cfg(1, "10.0.0.5");
	uint8_t fh[PS_MAX_FH_SIZE + 1];

	memset(fh, 0xAB, sizeof(fh));

	ck_assert_int_eq(ps_state_register(&c), 0);
	ck_assert_int_eq(ps_state_set_mds_root_fh(1, fh, sizeof(fh)), -E2BIG);
	/* Previous (empty) state preserved on failure. */
	ck_assert_uint_eq(ps_state_find(1)->pls_mds_root_fh_len, 0);
}
END_TEST

/*
 * Setting on an unregistered listener returns -ENOENT, same
 * contract as set_session.
 */
START_TEST(test_set_mds_root_fh_unknown_id)
{
	uint8_t fh[] = { 0x01 };

	ck_assert_int_eq(ps_state_set_mds_root_fh(42, fh, 1), -ENOENT);
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
	tcase_add_test(tc, test_session_defaults_null);
	tcase_add_test(tc, test_set_session_stores_pointer);
	tcase_add_test(tc, test_set_session_unknown_id_fails);
	tcase_add_test(tc, test_mds_root_fh_defaults_empty);
	tcase_add_test(tc, test_set_mds_root_fh_stores_bytes);
	tcase_add_test(tc, test_set_mds_root_fh_clear);
	tcase_add_test(tc, test_set_mds_root_fh_too_big);
	tcase_add_test(tc, test_set_mds_root_fh_unknown_id);
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
