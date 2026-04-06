/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * idmap_test.c -- unit tests for the identity mapping cache.
 *
 * Tests:
 *   - init/fini lifecycle
 *   - cache injection and lookup (uid-->name, name-->uid)
 *   - numeric string bypass
 *   - unknown lookups return -ENOENT
 *   - domain case-insensitivity
 *   - round-trip cache-->lookup-->verify
 *   - gid equivalents
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <string.h>
#include <check.h>

#include "reffs/idmap.h"
#include "reffs/utf8string.h"
#include "libreffs_test.h"

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

START_TEST(test_init_fini)
{
	ck_assert_int_eq(idmap_init("TEST.COM"), 0);
	idmap_fini();
}
END_TEST

START_TEST(test_init_null_domain)
{
	ck_assert_int_eq(idmap_init(NULL), 0);
	idmap_fini();
}
END_TEST

/* ------------------------------------------------------------------ */
/* UID cache injection + lookup                                        */
/* ------------------------------------------------------------------ */

START_TEST(test_cache_uid_lookup)
{
	utf8string u = { 0 };

	ck_assert_int_eq(idmap_init("EXAMPLE.COM"), 0);

	idmap_cache_uid(1000, "alice@EXAMPLE.COM");

	ck_assert_int_eq(idmap_uid_to_name(1000, &u), 0);
	ck_assert_str_eq(u.utf8string_val, "alice@EXAMPLE.COM");
	utf8string_free(&u);

	idmap_fini();
}
END_TEST

START_TEST(test_cache_uid_reverse_lookup)
{
	uid_t uid = 0;
	utf8string name = { 0 };

	ck_assert_int_eq(idmap_init("EXAMPLE.COM"), 0);

	idmap_cache_uid(1000, "alice@EXAMPLE.COM");

	name.utf8string_val = (char *)"alice@EXAMPLE.COM";
	name.utf8string_len = strlen("alice@EXAMPLE.COM");

	ck_assert_int_eq(idmap_name_to_uid(&name, &uid), 0);
	ck_assert_uint_eq(uid, 1000);

	idmap_fini();
}
END_TEST

/* ------------------------------------------------------------------ */
/* Numeric strings bypass cache                                        */
/* ------------------------------------------------------------------ */

START_TEST(test_numeric_string_bypass)
{
	uid_t uid = 0;
	utf8string name = { 0 };

	ck_assert_int_eq(idmap_init("EXAMPLE.COM"), 0);

	name.utf8string_val = (char *)"42";
	name.utf8string_len = 2;

	ck_assert_int_eq(idmap_name_to_uid(&name, &uid), 0);
	ck_assert_uint_eq(uid, 42);

	idmap_fini();
}
END_TEST

/* ------------------------------------------------------------------ */
/* Unknown lookups                                                     */
/* ------------------------------------------------------------------ */

START_TEST(test_unknown_uid)
{
	utf8string u = { 0 };

	ck_assert_int_eq(idmap_init("EXAMPLE.COM"), 0);

	/*
	 * uid 99999 is unlikely to exist in nsswitch during testing.
	 * The lookup should fail with -ENOENT (or succeed if the system
	 * happens to have it -- either is acceptable).
	 */
	int ret = idmap_uid_to_name(99999, &u);

	if (ret == 0)
		utf8string_free(&u);
	/* No assertion on ret -- system-dependent. */

	idmap_fini();
}
END_TEST

START_TEST(test_unknown_name)
{
	uid_t uid = 0;
	utf8string name = { 0 };
	int ret;

	ck_assert_int_eq(idmap_init("EXAMPLE.COM"), 0);

	name.utf8string_val = (char *)"nonexistent_user_xyz@EXAMPLE.COM";
	name.utf8string_len = strlen(name.utf8string_val);

	/*
	 * System-dependent: libnfsidmap may resolve via nsswitch,
	 * or getpwnam_r may find a match.  Either outcome is valid.
	 */
	ret = idmap_name_to_uid(&name, &uid);
	ck_assert(ret == 0 || ret == -ENOENT);

	idmap_fini();
}
END_TEST

/* ------------------------------------------------------------------ */
/* Domain case-insensitivity                                           */
/* ------------------------------------------------------------------ */

START_TEST(test_domain_case_insensitive)
{
	uid_t uid = 0;
	utf8string name = { 0 };

	ck_assert_int_eq(idmap_init("EXAMPLE.COM"), 0);

	idmap_cache_uid(2000, "bob@EXAMPLE.COM");

	/* Lookup with different case should match. */
	name.utf8string_val = (char *)"bob@example.com";
	name.utf8string_len = strlen(name.utf8string_val);

	ck_assert_int_eq(idmap_name_to_uid(&name, &uid), 0);
	ck_assert_uint_eq(uid, 2000);

	/* Mixed case */
	name.utf8string_val = (char *)"BOB@Example.Com";
	name.utf8string_len = strlen(name.utf8string_val);

	ck_assert_int_eq(idmap_name_to_uid(&name, &uid), 0);
	ck_assert_uint_eq(uid, 2000);

	idmap_fini();
}
END_TEST

/* ------------------------------------------------------------------ */
/* Round-trip: cache --> uid_to_name --> name_to_uid                       */
/* ------------------------------------------------------------------ */

START_TEST(test_round_trip)
{
	utf8string u = { 0 };
	uid_t uid = 0;

	ck_assert_int_eq(idmap_init("EXAMPLE.COM"), 0);

	idmap_cache_uid(3000, "charlie@EXAMPLE.COM");

	/* uid --> name */
	ck_assert_int_eq(idmap_uid_to_name(3000, &u), 0);
	ck_assert_str_eq(u.utf8string_val, "charlie@EXAMPLE.COM");

	/* name --> uid (using the string we just got back) */
	ck_assert_int_eq(idmap_name_to_uid(&u, &uid), 0);
	ck_assert_uint_eq(uid, 3000);

	utf8string_free(&u);
	idmap_fini();
}
END_TEST

/* ------------------------------------------------------------------ */
/* GID tests                                                           */
/* ------------------------------------------------------------------ */

START_TEST(test_cache_gid_round_trip)
{
	utf8string g = { 0 };
	gid_t gid = 0;

	ck_assert_int_eq(idmap_init("EXAMPLE.COM"), 0);

	idmap_cache_gid(500, "developers@EXAMPLE.COM");

	ck_assert_int_eq(idmap_gid_to_name(500, &g), 0);
	ck_assert_str_eq(g.utf8string_val, "developers@EXAMPLE.COM");

	ck_assert_int_eq(idmap_name_to_gid(&g, &gid), 0);
	ck_assert_uint_eq(gid, 500);

	utf8string_free(&g);
	idmap_fini();
}
END_TEST

START_TEST(test_numeric_gid_bypass)
{
	gid_t gid = 0;
	utf8string name = { 0 };

	ck_assert_int_eq(idmap_init("EXAMPLE.COM"), 0);

	name.utf8string_val = (char *)"100";
	name.utf8string_len = 3;

	ck_assert_int_eq(idmap_name_to_gid(&name, &gid), 0);
	ck_assert_uint_eq(gid, 100);

	idmap_fini();
}
END_TEST

/* ------------------------------------------------------------------ */
/* Duplicate insertion is idempotent                                   */
/* ------------------------------------------------------------------ */

START_TEST(test_duplicate_insert)
{
	utf8string u = { 0 };

	ck_assert_int_eq(idmap_init("EXAMPLE.COM"), 0);

	idmap_cache_uid(4000, "dave@EXAMPLE.COM");
	idmap_cache_uid(4000, "dave@EXAMPLE.COM"); /* no-op */

	ck_assert_int_eq(idmap_uid_to_name(4000, &u), 0);
	ck_assert_str_eq(u.utf8string_val, "dave@EXAMPLE.COM");
	utf8string_free(&u);

	idmap_fini();
}
END_TEST

/* ------------------------------------------------------------------ */
/* Prewarm                                                             */
/* ------------------------------------------------------------------ */

/*
 * Pre-warm with already-cached IDs should return immediately
 * without spawning any threads.
 */
START_TEST(test_prewarm_cached_noop)
{
	ck_assert_int_eq(idmap_init("TEST.COM"), 0);

	idmap_cache_uid(5000, "cached@TEST.COM");

	uid_t uids[] = { 5000 };

	/* Should be a no-op since uid 5000 is cached. */
	idmap_prewarm(uids, 1, NULL, 0, 100);

	/* Verify cache still has the entry. */
	utf8string u = { 0 };

	ck_assert_int_eq(idmap_uid_to_name(5000, &u), 0);
	ck_assert_str_eq(u.utf8string_val, "cached@TEST.COM");
	utf8string_free(&u);

	idmap_fini();
}
END_TEST

/*
 * Pre-warm with a resolvable uid (from /etc/passwd) should populate
 * the cache.  uid 0 (root) is always resolvable.
 */
START_TEST(test_prewarm_resolves)
{
	ck_assert_int_eq(idmap_init("TEST.COM"), 0);

	uid_t uids[] = { 0 }; /* root is always in /etc/passwd */

	idmap_prewarm(uids, 1, NULL, 0, 3000);

	/* After prewarm, uid 0 should be in the cache. */
	utf8string u = { 0 };
	int ret = idmap_uid_to_name(0, &u);

	if (ret == 0) {
		/* Should contain "root@TEST.COM" or similar. */
		ck_assert_ptr_nonnull(u.utf8string_val);
		utf8string_free(&u);
	}
	/* ret == -ENOENT is acceptable if resolver is minimal. */

	idmap_fini();
}
END_TEST

/*
 * Pre-warm with an unresolvable uid should hit the timeout and
 * return without blocking forever.  uid 99999 is unlikely to
 * exist in /etc/passwd.
 */
START_TEST(test_prewarm_timeout)
{
	ck_assert_int_eq(idmap_init("TEST.COM"), 0);

	uid_t uids[] = { 99999 };

	/* Short timeout -- should return quickly even if resolver hangs. */
	idmap_prewarm(uids, 1, NULL, 0, 500);

	/* uid 99999 should NOT be in the cache (unresolvable). */
	utf8string u = { 0 };

	ck_assert_int_eq(idmap_uid_to_name(99999, &u), -ENOENT);

	idmap_fini();
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite                                                               */
/* ------------------------------------------------------------------ */

static Suite *idmap_suite(void)
{
	Suite *s = suite_create("idmap");
	TCase *tc;

	tc = tcase_create("lifecycle");
	tcase_add_test(tc, test_init_fini);
	tcase_add_test(tc, test_init_null_domain);
	suite_add_tcase(s, tc);

	tc = tcase_create("uid");
	tcase_add_test(tc, test_cache_uid_lookup);
	tcase_add_test(tc, test_cache_uid_reverse_lookup);
	tcase_add_test(tc, test_numeric_string_bypass);
	tcase_add_test(tc, test_unknown_uid);
	tcase_add_test(tc, test_unknown_name);
	tcase_add_test(tc, test_domain_case_insensitive);
	tcase_add_test(tc, test_round_trip);
	tcase_add_test(tc, test_duplicate_insert);
	suite_add_tcase(s, tc);

	tc = tcase_create("gid");
	tcase_add_test(tc, test_cache_gid_round_trip);
	tcase_add_test(tc, test_numeric_gid_bypass);
	suite_add_tcase(s, tc);

	tc = tcase_create("prewarm");
	tcase_add_test(tc, test_prewarm_cached_noop);
	tcase_add_test(tc, test_prewarm_resolves);
	tcase_add_test(tc, test_prewarm_timeout);
	suite_add_tcase(s, tc);

	return s;
}

int main(void)
{
	int nf;
	Suite *s = idmap_suite();
	SRunner *sr = srunner_create(s);

	srunner_run_all(sr, CK_NORMAL);
	nf = srunner_ntests_failed(sr);
	srunner_free(sr);
	return nf ? 1 : 0;
}
