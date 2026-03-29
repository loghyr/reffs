/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * Phase 4 TDD: Per-sb security flavor tests.
 *
 * Tests for per-sb flavor assignment, flavor lint, and
 * verification that the root sb starts with all flavors.
 *
 * The WRONGSEC enforcement tests (nfs4_check_wrongsec using
 * compound->c_curr_sb->sb_flavors) require NFSv4 compound
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
 * Intent: verify that super_block_set_flavors stores the flavor list
 * on the sb and that it can be read back.
 */
START_TEST(test_sb_set_flavors)
{
	struct super_block *root = super_block_find(SUPER_BLOCK_ROOT_ID);

	ck_assert_ptr_nonnull(root);

	enum reffs_auth_flavor flavors[] = { REFFS_AUTH_SYS, REFFS_AUTH_KRB5 };

	super_block_set_flavors(root, flavors, 2);
	ck_assert_uint_eq(root->sb_nflavors, 2);
	ck_assert_int_eq(root->sb_flavors[0], REFFS_AUTH_SYS);
	ck_assert_int_eq(root->sb_flavors[1], REFFS_AUTH_KRB5);

	super_block_put(root);
}
END_TEST

/*
 * Intent: verify that set_flavors with 0 clears the list.
 */
START_TEST(test_sb_set_flavors_empty)
{
	struct super_block *root = super_block_find(SUPER_BLOCK_ROOT_ID);

	ck_assert_ptr_nonnull(root);

	super_block_set_flavors(root, NULL, 0);
	ck_assert_uint_eq(root->sb_nflavors, 0);

	super_block_put(root);
}
END_TEST

/*
 * Intent: verify that set_flavors clamps to REFFS_CONFIG_MAX_FLAVORS.
 */
START_TEST(test_sb_set_flavors_clamp)
{
	struct super_block *root = super_block_find(SUPER_BLOCK_ROOT_ID);

	ck_assert_ptr_nonnull(root);

	enum reffs_auth_flavor many[16];

	for (int i = 0; i < 16; i++)
		many[i] = REFFS_AUTH_SYS;

	super_block_set_flavors(root, many, 16);
	ck_assert_uint_eq(root->sb_nflavors, REFFS_CONFIG_MAX_FLAVORS);

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

	/* Child requires krb5 — not in root's list. */
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

	/* Child only needs krb5 — subset of root. */
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
