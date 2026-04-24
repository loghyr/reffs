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

#include "ps_discovery.h"
#include "ps_state.h"

/*
 * The happy-path walker requires a live MDS session (CI integration
 * covers that).  These unit tests cover only the argument-validation
 * and path-parsing edges that don't touch the network.
 *
 * ms=NULL causes ps_discovery_walk_path() to return -EINVAL BEFORE
 * any network call, so we can use a NULL session pointer for these
 * tests without crashing.
 */

START_TEST(test_walk_null_args)
{
	uint8_t fh[128];
	uint32_t fh_len = 42;

	/* Every NULL pointer combo short-circuits to -EINVAL. */
	ck_assert_int_eq(ps_discovery_walk_path(NULL, "/x", fh, sizeof(fh),
						&fh_len),
			 -EINVAL);
	ck_assert_int_eq(ps_discovery_walk_path((void *)1, NULL, fh, sizeof(fh),
						&fh_len),
			 -EINVAL);
	ck_assert_int_eq(ps_discovery_walk_path((void *)1, "/x", NULL,
						sizeof(fh), &fh_len),
			 -EINVAL);
	ck_assert_int_eq(ps_discovery_walk_path((void *)1, "/x", fh, sizeof(fh),
						NULL),
			 -EINVAL);
}
END_TEST

/*
 * Relative paths are rejected immediately -- no network, just input
 * validation.  Guards against a caller mishandling the root-sb mount
 * path stored in sb_path (which is absolute in reffsd's config).
 */
START_TEST(test_walk_rejects_relative_path)
{
	uint8_t fh[128];
	uint32_t fh_len = 0;

	ck_assert_int_eq(ps_discovery_walk_path((void *)1, "foo", fh,
						sizeof(fh), &fh_len),
			 -EINVAL);
	ck_assert_int_eq(ps_discovery_walk_path((void *)1, "foo/bar", fh,
						sizeof(fh), &fh_len),
			 -EINVAL);
	ck_assert_int_eq(ps_discovery_walk_path((void *)1, "", fh, sizeof(fh),
						&fh_len),
			 -EINVAL);
}
END_TEST

/*
 * A path deeper than PS_DISCOVERY_MAX_DEPTH is rejected before any
 * network call.  Reproduce by constructing "/a/a/a/..." with N+1
 * components.
 */
START_TEST(test_walk_rejects_excessive_depth)
{
	char path[PS_DISCOVERY_MAX_DEPTH * 3 + 8];
	uint8_t fh[128];
	uint32_t fh_len = 0;

	/* Build a path with MAX_DEPTH+1 one-char components. */
	size_t off = 0;

	for (unsigned int i = 0; i <= PS_DISCOVERY_MAX_DEPTH; i++) {
		path[off++] = '/';
		path[off++] = 'a';
	}
	path[off] = '\0';

	ck_assert_int_eq(ps_discovery_walk_path((void *)1, path, fh, sizeof(fh),
						&fh_len),
			 -E2BIG);
}
END_TEST

/*
 * A single oversized component (>255 bytes) is rejected before any
 * network call, separate from the total-depth check.
 */
START_TEST(test_walk_rejects_long_component)
{
	char path[PS_DISCOVERY_COMPONENT_MAX + 8];
	uint8_t fh[128];
	uint32_t fh_len = 0;

	path[0] = '/';
	memset(path + 1, 'a', PS_DISCOVERY_COMPONENT_MAX + 1);
	path[PS_DISCOVERY_COMPONENT_MAX + 2] = '\0';

	ck_assert_int_eq(ps_discovery_walk_path((void *)1, path, fh, sizeof(fh),
						&fh_len),
			 -E2BIG);
}
END_TEST

/*
 * ps_discovery_run arg validation: NULL pls short-circuits to
 * -EINVAL before any MOUNT3 or compound attempt.  No network
 * traffic, safe without a live upstream.
 */
START_TEST(test_run_rejects_null_pls)
{
	ck_assert_int_eq(ps_discovery_run(NULL), -EINVAL);
}
END_TEST

/*
 * Empty pls_upstream means the listener is configured without an
 * upstream address (legal config; nothing to discover).  Coordinator
 * short-circuits at the prereq check rather than asking MOUNT3 to
 * resolve "" via the portmapper.
 */
START_TEST(test_run_rejects_empty_upstream)
{
	struct ps_listener_state pls;

	memset(&pls, 0, sizeof(pls));
	pls.pls_listener_id = 1;
	/* pls_upstream left as all-zeros (empty string). */
	/* pls_session value irrelevant -- upstream check runs first. */

	ck_assert_int_eq(ps_discovery_run(&pls), -EINVAL);
}
END_TEST

/*
 * NULL pls_session is a programming error -- reffsd is expected to
 * open the MDS session before calling the coordinator.  Return a
 * distinct sentinel (-ENOTCONN) so the caller's logs can tell this
 * apart from the "no upstream configured" case above.
 */
START_TEST(test_run_rejects_null_session)
{
	struct ps_listener_state pls;

	memset(&pls, 0, sizeof(pls));
	pls.pls_listener_id = 1;
	strncpy(pls.pls_upstream, "127.0.0.1", sizeof(pls.pls_upstream) - 1);
	/* pls_session left NULL. */

	ck_assert_int_eq(ps_discovery_run(&pls), -ENOTCONN);
}
END_TEST

static Suite *ps_discovery_suite(void)
{
	Suite *s = suite_create("ps_discovery");
	TCase *tc = tcase_create("walk-validation");

	tcase_add_test(tc, test_walk_null_args);
	tcase_add_test(tc, test_walk_rejects_relative_path);
	tcase_add_test(tc, test_walk_rejects_excessive_depth);
	tcase_add_test(tc, test_walk_rejects_long_component);
	tcase_add_test(tc, test_run_rejects_null_pls);
	tcase_add_test(tc, test_run_rejects_empty_upstream);
	tcase_add_test(tc, test_run_rejects_null_session);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int failed;
	SRunner *sr = srunner_create(ps_discovery_suite());

	srunner_run_all(sr, CK_NORMAL);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return failed == 0 ? 0 : 1;
}
