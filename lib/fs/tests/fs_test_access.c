/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/*
 * fs_test_access.c — reffs_fs_access() permission logic
 *
 * reffs_fs_access() takes explicit uid/gid arguments (unlike the FUSE shim
 * which pulls them from fuse_get_context).  This makes it straightforward
 * to test all three permission tiers — owner, group, other — without
 * changing the process's effective credentials.
 *
 * The logic in fs.c is: check owner first; if uid matches, only owner
 * bits are tested; else if gid matches, only group bits are tested;
 * else other bits are tested.
 *
 * Tests:
 *  - F_OK on existing path returns 0
 *  - F_OK on non-existent path returns -ENOENT
 *  - Owner: R_OK/W_OK/X_OK granted when bits set; -EACCES when not
 *  - Group: same, using group bits
 *  - Other: same, using other bits
 *  - Owner match short-circuits group/other bits
 */

#include "fs_test_harness.h"

uid_t fs_test_uid;
gid_t fs_test_gid;

/* Distinct uid/gid values that do not match the process credentials */
#define OWNER_UID 100
#define OWNER_GID 200
#define GROUP_GID 200 /* same as OWNER_GID to be "in the group" */
#define OTHER_UID 300
#define OTHER_GID 400

static void setup(void)
{
	fs_test_setup();
}
static void teardown(void)
{
	fs_test_teardown();
}

/* ------------------------------------------------------------------ */

START_TEST(test_f_ok_exists)
{
	ck_assert_int_eq(reffs_fs_create("/f", S_IFREG | 0000), 0);
	/* F_OK only checks existence, ignores permission bits */
	ck_assert_int_eq(reffs_fs_access("/f", F_OK, OWNER_UID, OWNER_GID), 0);
	ck_assert_int_eq(reffs_fs_unlink("/f"), 0);
}
END_TEST

START_TEST(test_f_ok_enoent)
{
	ck_assert_int_eq(reffs_fs_access("/no_such", F_OK, OWNER_UID,
					 OWNER_GID),
			 -ENOENT);
}
END_TEST

/* --- owner tier --------------------------------------------------- */

START_TEST(test_owner_read_granted)
{
	ck_assert_int_eq(reffs_fs_create("/f", S_IFREG | 0400), 0);
	ck_assert_int_eq(reffs_fs_chown("/f", OWNER_UID, OWNER_GID), 0);
	ck_assert_int_eq(reffs_fs_access("/f", R_OK, OWNER_UID, OWNER_GID), 0);
	ck_assert_int_eq(reffs_fs_unlink("/f"), 0);
}
END_TEST

START_TEST(test_owner_read_denied)
{
	ck_assert_int_eq(reffs_fs_create("/f", S_IFREG | 0000), 0);
	ck_assert_int_eq(reffs_fs_chown("/f", OWNER_UID, OWNER_GID), 0);
	ck_assert_int_eq(reffs_fs_access("/f", R_OK, OWNER_UID, OWNER_GID),
			 -EACCES);
	ck_assert_int_eq(reffs_fs_unlink("/f"), 0);
}
END_TEST

START_TEST(test_owner_write_granted)
{
	ck_assert_int_eq(reffs_fs_create("/f", S_IFREG | 0200), 0);
	ck_assert_int_eq(reffs_fs_chown("/f", OWNER_UID, OWNER_GID), 0);
	ck_assert_int_eq(reffs_fs_access("/f", W_OK, OWNER_UID, OWNER_GID), 0);
	ck_assert_int_eq(reffs_fs_unlink("/f"), 0);
}
END_TEST

START_TEST(test_owner_write_denied)
{
	ck_assert_int_eq(reffs_fs_create("/f", S_IFREG | 0000), 0);
	ck_assert_int_eq(reffs_fs_chown("/f", OWNER_UID, OWNER_GID), 0);
	ck_assert_int_eq(reffs_fs_access("/f", W_OK, OWNER_UID, OWNER_GID),
			 -EACCES);
	ck_assert_int_eq(reffs_fs_unlink("/f"), 0);
}
END_TEST

START_TEST(test_owner_exec_granted)
{
	ck_assert_int_eq(reffs_fs_create("/f", S_IFREG | 0100), 0);
	ck_assert_int_eq(reffs_fs_chown("/f", OWNER_UID, OWNER_GID), 0);
	ck_assert_int_eq(reffs_fs_access("/f", X_OK, OWNER_UID, OWNER_GID), 0);
	ck_assert_int_eq(reffs_fs_unlink("/f"), 0);
}
END_TEST

/* --- group tier --------------------------------------------------- */

START_TEST(test_group_read_granted)
{
	/* uid doesn't match owner, gid matches → group bits apply */
	ck_assert_int_eq(reffs_fs_create("/f", S_IFREG | 0040), 0);
	ck_assert_int_eq(reffs_fs_chown("/f", OWNER_UID, GROUP_GID), 0);
	ck_assert_int_eq(reffs_fs_access("/f", R_OK, OTHER_UID, GROUP_GID), 0);
	ck_assert_int_eq(reffs_fs_unlink("/f"), 0);
}
END_TEST

START_TEST(test_group_read_denied)
{
	ck_assert_int_eq(reffs_fs_create("/f", S_IFREG | 0000), 0);
	ck_assert_int_eq(reffs_fs_chown("/f", OWNER_UID, GROUP_GID), 0);
	ck_assert_int_eq(reffs_fs_access("/f", R_OK, OTHER_UID, GROUP_GID),
			 -EACCES);
	ck_assert_int_eq(reffs_fs_unlink("/f"), 0);
}
END_TEST

/* --- other tier --------------------------------------------------- */

START_TEST(test_other_read_granted)
{
	/* Neither uid nor gid matches → other bits apply */
	ck_assert_int_eq(reffs_fs_create("/f", S_IFREG | 0004), 0);
	ck_assert_int_eq(reffs_fs_chown("/f", OWNER_UID, OWNER_GID), 0);
	ck_assert_int_eq(reffs_fs_access("/f", R_OK, OTHER_UID, OTHER_GID), 0);
	ck_assert_int_eq(reffs_fs_unlink("/f"), 0);
}
END_TEST

START_TEST(test_other_read_denied)
{
	ck_assert_int_eq(reffs_fs_create("/f", S_IFREG | 0000), 0);
	ck_assert_int_eq(reffs_fs_chown("/f", OWNER_UID, OWNER_GID), 0);
	ck_assert_int_eq(reffs_fs_access("/f", R_OK, OTHER_UID, OTHER_GID),
			 -EACCES);
	ck_assert_int_eq(reffs_fs_unlink("/f"), 0);
}
END_TEST

/*
 * Owner uid match must short-circuit: even if owner bits deny and group
 * bits would allow, the owner check must win.
 */
START_TEST(test_owner_uid_match_shortcircuits_group)
{
	/* mode 0040: group can read, owner cannot */
	ck_assert_int_eq(reffs_fs_create("/f", S_IFREG | 0040), 0);
	ck_assert_int_eq(reffs_fs_chown("/f", OWNER_UID, OWNER_GID), 0);
	/* request with owner uid — must use owner bits, not group bits */
	ck_assert_int_eq(reffs_fs_access("/f", R_OK, OWNER_UID, OWNER_GID),
			 -EACCES);
	ck_assert_int_eq(reffs_fs_unlink("/f"), 0);
}
END_TEST

/* ------------------------------------------------------------------ */

Suite *fs_access_suite(void)
{
	Suite *s = suite_create("fs: access");
	TCase *tc = tcase_create("Core");
	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_f_ok_exists);
	tcase_add_test(tc, test_f_ok_enoent);
	tcase_add_test(tc, test_owner_read_granted);
	tcase_add_test(tc, test_owner_read_denied);
	tcase_add_test(tc, test_owner_write_granted);
	tcase_add_test(tc, test_owner_write_denied);
	tcase_add_test(tc, test_owner_exec_granted);
	tcase_add_test(tc, test_group_read_granted);
	tcase_add_test(tc, test_group_read_denied);
	tcase_add_test(tc, test_other_read_granted);
	tcase_add_test(tc, test_other_read_denied);
	tcase_add_test(tc, test_owner_uid_match_shortcircuits_group);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int failed;
	fs_test_global_init();
	SRunner *sr = srunner_create(fs_access_suite());
	srunner_set_fork_status(sr, CK_NOFORK);
	srunner_run_all(sr, CK_NORMAL);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	fs_test_global_fini();
	return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
