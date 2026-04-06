/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * Per-sb security flavor tests.
 *
 * Tests for per-sb flavor assignment (via super_block_set_flavors shim
 * and directly via super_block_set_client_rules), flavor lint, and
 * verification that sb_all_flavors is recomputed correctly.
 *
 * super_block_set_flavors() is now a shim that synthesizes a single "*"
 * catch-all rule; sb_all_flavors reflects the union of all rule flavors.
 *
 * The WRONGSEC enforcement tests (nfs4_check_wrongsec using
 * compound->c_curr_sb->sb_all_flavors) require NFSv4 compound
 * infrastructure and are deferred to CI integration tests.
 * NOT_NOW_BROWN_COW: add compound-level WRONGSEC tests.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <string.h>

#include <check.h>

#include "reffs/dirent.h"
#include "reffs/fs.h"
#include "reffs/inode.h"
#include "reffs/settings.h"
#include "reffs/super_block.h"
#include "fs_test_harness.h"

/* ------------------------------------------------------------------ */
/* Per-sb flavor assignment                                            */
/* ------------------------------------------------------------------ */

/*
 * Intent: verify that the set_flavors shim stores the flavor list
 * in sb_all_flavors (via a synthesized "*" catch-all rule).
 */
START_TEST(test_sb_set_flavors)
{
	struct super_block *root = super_block_find(SUPER_BLOCK_ROOT_ID);

	ck_assert_ptr_nonnull(root);

	enum reffs_auth_flavor flavors[] = { REFFS_AUTH_SYS, REFFS_AUTH_KRB5 };

	super_block_set_flavors(root, flavors, 2);
	ck_assert_uint_eq(root->sb_nall_flavors, 2);
	ck_assert_int_eq(root->sb_all_flavors[0], REFFS_AUTH_SYS);
	ck_assert_int_eq(root->sb_all_flavors[1], REFFS_AUTH_KRB5);
	/* Shim creates one catch-all rule. */
	ck_assert_uint_eq(root->sb_nclient_rules, 1);

	super_block_put(root);
}
END_TEST

/*
 * Intent: verify that set_flavors with 0 clears the flavor union.
 */
START_TEST(test_sb_set_flavors_empty)
{
	struct super_block *root = super_block_find(SUPER_BLOCK_ROOT_ID);

	ck_assert_ptr_nonnull(root);

	super_block_set_flavors(root, NULL, 0);
	ck_assert_uint_eq(root->sb_nall_flavors, 0);
	/* 0 flavors -> 0 rules (shim creates none). */
	ck_assert_uint_eq(root->sb_nclient_rules, 0);

	super_block_put(root);
}
END_TEST

/*
 * Intent: verify that set_flavors clamps to REFFS_CONFIG_MAX_FLAVORS.
 * The shim deduplicates identical flavors, so passing 16 identical
 * AUTH_SYS entries collapses to 1 in the union.
 */
START_TEST(test_sb_set_flavors_clamp)
{
	struct super_block *root = super_block_find(SUPER_BLOCK_ROOT_ID);

	ck_assert_ptr_nonnull(root);

	/* Fill with distinct-enough flavors (use 4 real values). */
	enum reffs_auth_flavor many[] = {
		REFFS_AUTH_SYS,
		REFFS_AUTH_KRB5,
		REFFS_AUTH_KRB5I,
		REFFS_AUTH_KRB5P,
	};

	super_block_set_flavors(root, many, 4);
	/* Shim caps at REFFS_CONFIG_MAX_FLAVORS (8), 4 < 8 so all pass. */
	ck_assert_uint_eq(root->sb_nall_flavors, 4);

	super_block_put(root);
}
END_TEST

/*
 * Intent: verify super_block_set_client_rules with multiple rules
 * computes sb_all_flavors as the union across all rules.
 */
START_TEST(test_sb_set_client_rules_all_flavors)
{
	struct super_block *root = super_block_find(SUPER_BLOCK_ROOT_ID);

	ck_assert_ptr_nonnull(root);

	struct sb_client_rule rules[2];

	memset(rules, 0, sizeof(rules));
	/* Rule 0: 10.0.0.0/8 gets sys */
	strncpy(rules[0].scr_match, "10.0.0.0/8", SB_CLIENT_MATCH_MAX - 1);
	rules[0].scr_rw = true;
	rules[0].scr_flavors[0] = REFFS_AUTH_SYS;
	rules[0].scr_nflavors = 1;
	/* Rule 1: * gets krb5 */
	strncpy(rules[1].scr_match, "*", SB_CLIENT_MATCH_MAX - 1);
	rules[1].scr_rw = false;
	rules[1].scr_flavors[0] = REFFS_AUTH_KRB5;
	rules[1].scr_nflavors = 1;

	super_block_set_client_rules(root, rules, 2);

	ck_assert_uint_eq(root->sb_nclient_rules, 2);
	/* Union of sys + krb5 = 2 distinct flavors. */
	ck_assert_uint_eq(root->sb_nall_flavors, 2);

	super_block_put(root);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Lint flavors                                                        */
/* ------------------------------------------------------------------ */

/*
 * Intent: lint warns when a child sb requires a flavor that the
 * root sb does not support.
 */
START_TEST(test_lint_flavors_warns)
{
	ck_assert_int_eq(reffs_fs_mkdir("/lint_a", 0755), 0);

	/* Root only has AUTH_SYS. */
	struct super_block *root = super_block_find(SUPER_BLOCK_ROOT_ID);

	ck_assert_ptr_nonnull(root);
	enum reffs_auth_flavor root_f[] = { REFFS_AUTH_SYS };

	super_block_set_flavors(root, root_f, 1);
	super_block_put(root);

	/* Child requires krb5 -- not in root's list. */
	struct super_block *child = super_block_alloc(30, (char *)"/lint_a",
						      REFFS_STORAGE_RAM, NULL);

	ck_assert_ptr_nonnull(child);
	ck_assert_int_eq(super_block_dirent_create(child, NULL,
						   reffs_life_action_birth),
			 0);
	ck_assert_int_eq(super_block_mount(child, "/lint_a"), 0);

	enum reffs_auth_flavor child_f[] = { REFFS_AUTH_KRB5 };

	super_block_set_flavors(child, child_f, 1);

	/* Lint should find the mismatch. */
	int warnings = super_block_lint_flavors();

	ck_assert_int_ge(warnings, 1);

	super_block_unmount(child);
	super_block_destroy(child);
	super_block_release_dirents(child);
	super_block_put(child);
	ck_assert_int_eq(reffs_fs_rmdir("/lint_a"), 0);
}
END_TEST

/*
 * Intent: lint returns 0 when all child flavors are covered by the
 * root sb's flavor list.
 */
START_TEST(test_lint_flavors_clean)
{
	ck_assert_int_eq(reffs_fs_mkdir("/lint_b", 0755), 0);

	/* Root has sys + krb5. */
	struct super_block *root = super_block_find(SUPER_BLOCK_ROOT_ID);

	ck_assert_ptr_nonnull(root);
	enum reffs_auth_flavor root_f[] = { REFFS_AUTH_SYS, REFFS_AUTH_KRB5 };

	super_block_set_flavors(root, root_f, 2);
	super_block_put(root);

	/* Child only needs krb5 -- subset of root. */
	struct super_block *child = super_block_alloc(31, (char *)"/lint_b",
						      REFFS_STORAGE_RAM, NULL);

	ck_assert_ptr_nonnull(child);
	ck_assert_int_eq(super_block_dirent_create(child, NULL,
						   reffs_life_action_birth),
			 0);
	ck_assert_int_eq(super_block_mount(child, "/lint_b"), 0);

	enum reffs_auth_flavor child_f[] = { REFFS_AUTH_KRB5 };

	super_block_set_flavors(child, child_f, 1);

	int warnings = super_block_lint_flavors();

	ck_assert_int_eq(warnings, 0);

	super_block_unmount(child);
	super_block_destroy(child);
	super_block_release_dirents(child);
	super_block_put(child);
	ck_assert_int_eq(reffs_fs_rmdir("/lint_b"), 0);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite                                                               */
/* ------------------------------------------------------------------ */

static Suite *sb_security_suite(void)
{
	Suite *s = suite_create("sb_security");
	TCase *tc;

	tc = tcase_create("flavors");
	tcase_add_checked_fixture(tc, fs_test_setup, fs_test_teardown);
	tcase_add_test(tc, test_sb_set_flavors);
	tcase_add_test(tc, test_sb_set_flavors_empty);
	tcase_add_test(tc, test_sb_set_flavors_clamp);
	tcase_add_test(tc, test_sb_set_client_rules_all_flavors);
	suite_add_tcase(s, tc);

	tc = tcase_create("lint");
	tcase_add_checked_fixture(tc, fs_test_setup, fs_test_teardown);
	tcase_add_test(tc, test_lint_flavors_warns);
	tcase_add_test(tc, test_lint_flavors_clean);
	suite_add_tcase(s, tc);

	return s;
}

int main(void)
{
	int nfailed;
	Suite *s = sb_security_suite();
	SRunner *sr = srunner_create(s);

	srunner_run_all(sr, CK_NORMAL);
	nfailed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return nfailed ? 1 : 0;
}
