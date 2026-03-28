/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * Unit tests for reffs_id type, macros, and enum.
 */

#ifdef HAVE_CONFIG_H
#include "config.h" /* IWYU pragma: keep */
#endif

#include <stdlib.h>

#include <check.h>

#include "reffs/identity_types.h"

/* Round-trip: MAKE then extract components. */
START_TEST(test_make_extract)
{
	reffs_id id = REFFS_ID_MAKE(REFFS_ID_KRB5, 42, 1000);

	ck_assert_uint_eq(REFFS_ID_TYPE(id), REFFS_ID_KRB5);
	ck_assert_uint_eq(REFFS_ID_DOMAIN(id), 42);
	ck_assert_uint_eq(REFFS_ID_LOCAL(id), 1000);
}
END_TEST

/* Backward compat: UNIX type with domain 0, low 32 bits == uid. */
START_TEST(test_unix_backward_compat)
{
	reffs_id id = REFFS_ID_MAKE(REFFS_ID_UNIX, 0, 1000);

	ck_assert_uint_eq(id, 1000);
	ck_assert_uint_eq((uint32_t)id, 1000);
	ck_assert_uint_eq(REFFS_ID_LOCAL(id), 1000);
	ck_assert(REFFS_ID_IS_UNIX(id));
}
END_TEST

/* Root is UNIX type 0, domain 0, local 0. */
START_TEST(test_root)
{
	reffs_id id = REFFS_ID_ROOT_VAL;

	ck_assert_uint_eq(REFFS_ID_TYPE(id), REFFS_ID_UNIX);
	ck_assert_uint_eq(REFFS_ID_LOCAL(id), 0);
	ck_assert(REFFS_ID_IS_UNIX(id));
}
END_TEST

/* Nobody is type 15, local 65534. */
START_TEST(test_nobody)
{
	reffs_id id = REFFS_ID_NOBODY_VAL;

	ck_assert_uint_eq(REFFS_ID_TYPE(id), REFFS_ID_NOBODY);
	ck_assert_uint_eq(REFFS_ID_LOCAL(id), 65534);
	ck_assert(REFFS_ID_IS_NOBODY(id));
	ck_assert(!REFFS_ID_IS_UNIX(id));
}
END_TEST

/* Synthetic type for fencing. */
START_TEST(test_synth)
{
	reffs_id id = REFFS_ID_MAKE(REFFS_ID_SYNTH, 0, 1024);

	ck_assert_uint_eq(REFFS_ID_TYPE(id), REFFS_ID_SYNTH);
	ck_assert_uint_eq(REFFS_ID_LOCAL(id), 1024);
	ck_assert(!REFFS_ID_IS_UNIX(id));
}
END_TEST

/* Max domain index (28 bits). */
START_TEST(test_max_domain)
{
	reffs_id id = REFFS_ID_MAKE(REFFS_ID_SID, 0x0FFFFFFF, 500);

	ck_assert_uint_eq(REFFS_ID_DOMAIN(id), 0x0FFFFFFF);
	ck_assert_uint_eq(REFFS_ID_LOCAL(id), 500);
}
END_TEST

/* Domain overflow masked to 28 bits. */
START_TEST(test_domain_overflow)
{
	reffs_id id = REFFS_ID_MAKE(REFFS_ID_SID, 0x1FFFFFFF, 500);

	ck_assert_uint_eq(REFFS_ID_DOMAIN(id), 0x0FFFFFFF);
}
END_TEST

static Suite *identity_types_suite(void)
{
	Suite *s = suite_create("identity_types");
	TCase *tc = tcase_create("reffs_id");

	tcase_add_test(tc, test_make_extract);
	tcase_add_test(tc, test_unix_backward_compat);
	tcase_add_test(tc, test_root);
	tcase_add_test(tc, test_nobody);
	tcase_add_test(tc, test_synth);
	tcase_add_test(tc, test_max_domain);
	tcase_add_test(tc, test_domain_overflow);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int nfailed;
	Suite *s = identity_types_suite();
	SRunner *sr = srunner_create(s);

	srunner_run_all(sr, CK_NORMAL);
	nfailed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return nfailed ? 1 : 0;
}
