/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * Listener-scoped filesystem primitives.
 *
 * find_matching_directory_entry now takes a listener_id so callers in
 * the proxy-server path can mkdir / lookup within a [[proxy_mds]]
 * listener's own namespace rather than hard-coding the native root.
 * These tests exercise:
 *
 *   1. mkdir_p_for_listener creates the directory in the listener's
 *      root SB and NOT in the native root SB.
 *   2. super_block_mount resolves its mount-point path against the
 *      child SB's listener, so a proxy SB can mount inside its own
 *      [[proxy_mds]] namespace.
 *
 * The native mkdir / mkdir_p path is already covered by sb_mkdir_p_test;
 * this file only covers the new non-native listener paths.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <stdint.h>

#include <check.h>

#include "reffs/fs.h"
#include "reffs/ns.h"
#include "reffs/super_block.h"
#include "fs_test_harness.h"

/*
 * Single listener id used throughout.  Must be non-zero (0 is the
 * native listener's reserved slot -- reffs_ns_init_proxy_listener
 * rejects it).
 */
#define TEST_LISTENER_ID 7

static void proxy_fixture_setup(void)
{
	fs_test_setup();
	/*
	 * reffs_ns_init (inside fs_test_setup) already created the
	 * native root SB.  Now layer a proxy listener root SB on top
	 * so listener-scoped lookups have somewhere to resolve.
	 */
	ck_assert_int_eq(reffs_ns_init_proxy_listener(TEST_LISTENER_ID), 0);
}

static void proxy_fixture_teardown(void)
{
	/*
	 * fs_test_teardown calls reffs_ns_fini which walks every
	 * registered SB across all listeners, so the proxy root goes
	 * away with the native tree.  No explicit per-listener
	 * teardown call exists today.
	 */
	fs_test_teardown();
}

/*
 * mkdir_p_for_listener creates the directory in the listener's
 * namespace.  A native lookup for the same path must miss (-ENOENT
 * or -ENODEV) so we prove the two namespaces are actually separate.
 */
START_TEST(test_mkdir_p_for_listener_creates_in_proxy_ns)
{
	struct name_match *nm = NULL;
	int ret =
		reffs_fs_mkdir_p_for_listener(TEST_LISTENER_ID, "/alpha", 0755);

	ck_assert_int_eq(ret, 0);

	/* Found in the proxy listener. */
	ret = find_matching_directory_entry(&nm, TEST_LISTENER_ID, "/alpha",
					    LAST_COMPONENT_IS_MATCH);
	ck_assert_int_eq(ret, 0);
	name_match_free(nm);

	/* Not found in the native namespace. */
	nm = NULL;
	ret = find_matching_directory_entry(&nm, 0, "/alpha",
					    LAST_COMPONENT_IS_MATCH);
	ck_assert_int_eq(ret, -ENOENT);
}
END_TEST

/*
 * Multi-component mkdir_p against a proxy listener builds the full
 * chain.  Intermediate lookup in the same listener must find each
 * component; the deepest component exists (via LAST_COMPONENT_IS_MATCH).
 */
START_TEST(test_mkdir_p_for_listener_multi_component)
{
	struct name_match *nm = NULL;
	int ret = reffs_fs_mkdir_p_for_listener(TEST_LISTENER_ID, "/a/b/c/d",
						0755);

	ck_assert_int_eq(ret, 0);

	ret = find_matching_directory_entry(&nm, TEST_LISTENER_ID, "/a/b/c/d",
					    LAST_COMPONENT_IS_MATCH);
	ck_assert_int_eq(ret, 0);
	name_match_free(nm);

	/* Intermediate components also resolve. */
	nm = NULL;
	ret = find_matching_directory_entry(&nm, TEST_LISTENER_ID, "/a/b",
					    LAST_COMPONENT_IS_MATCH);
	ck_assert_int_eq(ret, 0);
	name_match_free(nm);
}
END_TEST

/*
 * A proxy SB can mount inside its own listener's namespace.  This
 * is the super_block_mount path the PS-driven allocator (slice
 * 2e-iii-g) depends on: sb_listener_id = TEST_LISTENER_ID, the
 * mount-point path resolves in listener TEST_LISTENER_ID's root,
 * and super_block_find_for_listener(SUPER_BLOCK_ROOT_ID, N) wires
 * the parent SB pointer correctly.
 */
START_TEST(test_super_block_mount_in_proxy_ns)
{
	/* Provide a mount point in the proxy listener's root. */
	ck_assert_int_eq(reffs_fs_mkdir_p_for_listener(TEST_LISTENER_ID,
						       "/export", 0755),
			 0);

	/*
	 * Allocate a child SB, stamp its listener_id, build its root
	 * dirent tree, then mount.  The id here (500) is arbitrary
	 * -- only (sb_id, sb_listener_id) uniqueness matters and the
	 * listener root uses sb_id=1.
	 */
	struct super_block *child =
		super_block_alloc(500, "/export", REFFS_STORAGE_RAM, NULL);
	ck_assert_ptr_nonnull(child);
	child->sb_listener_id = TEST_LISTENER_ID;

	ck_assert_int_eq(super_block_dirent_create(child, NULL,
						   reffs_life_action_birth),
			 0);

	ck_assert_int_eq(super_block_mount(child, "/export"), 0);
	ck_assert_uint_eq(super_block_lifecycle(child), SB_MOUNTED);

	/* The parent SB reference points at the proxy listener root. */
	ck_assert_ptr_nonnull(child->sb_parent_sb);
	ck_assert_uint_eq(child->sb_parent_sb->sb_listener_id,
			  TEST_LISTENER_ID);

	super_block_unmount(child);
	super_block_destroy(child);
	super_block_release_dirents(child);
	super_block_put(child);
}
END_TEST

/*
 * Mounting a proxy SB at a path that doesn't exist in its listener's
 * namespace returns -ENOENT (not a crash, not a match in the native
 * namespace where the path might happen to exist).
 */
START_TEST(test_super_block_mount_proxy_missing_path)
{
	/* Native namespace has "/elsewhere"; proxy listener does not. */
	ck_assert_int_eq(reffs_fs_mkdir("/elsewhere", 0755), 0);

	struct super_block *child =
		super_block_alloc(501, "/elsewhere", REFFS_STORAGE_RAM, NULL);

	ck_assert_ptr_nonnull(child);
	child->sb_listener_id = TEST_LISTENER_ID;
	ck_assert_int_eq(super_block_dirent_create(child, NULL,
						   reffs_life_action_birth),
			 0);

	ck_assert_int_eq(super_block_mount(child, "/elsewhere"), -ENOENT);
	ck_assert_uint_eq(super_block_lifecycle(child), SB_CREATED);

	/*
	 * Belt-and-braces: the failed mount must not have mutated the
	 * native tree that holds "/elsewhere".  A lingering RD_MOUNTED_ON
	 * flag or stolen dirent ref would have corrupted that namespace;
	 * verify we can still resolve it natively.
	 */
	struct name_match *native_nm = NULL;

	ck_assert_int_eq(find_matching_directory_entry(&native_nm, 0,
						       "/elsewhere",
						       LAST_COMPONENT_IS_MATCH),
			 0);
	name_match_free(native_nm);

	super_block_destroy(child);
	super_block_release_dirents(child);
	super_block_put(child);
}
END_TEST

static Suite *listener_scoped_suite(void)
{
	Suite *s = suite_create("listener_scoped_fs");
	TCase *tc = tcase_create("core");

	tcase_add_checked_fixture(tc, proxy_fixture_setup,
				  proxy_fixture_teardown);
	tcase_add_test(tc, test_mkdir_p_for_listener_creates_in_proxy_ns);
	tcase_add_test(tc, test_mkdir_p_for_listener_multi_component);
	tcase_add_test(tc, test_super_block_mount_in_proxy_ns);
	tcase_add_test(tc, test_super_block_mount_proxy_missing_path);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int failed;
	SRunner *sr = srunner_create(listener_scoped_suite());

	srunner_run_all(sr, CK_NORMAL);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return failed == 0 ? 0 : 1;
}
