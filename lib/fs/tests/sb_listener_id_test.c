/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * Proxy-server Phase 1: per-listener SB scoping.
 *
 * The PS design gives reffs a second RPC listener (default :4098) whose
 * SB table is separate from the native :2049 namespace.  Both tables
 * share one global sb_list but each super_block carries an sb_listener_id
 * that names which listener the SB is visible on.  Listener id 0 is the
 * native namespace, which is what every SB in reffs has today.
 * Listener ids 1+ correspond to [[proxy_mds]] config entries.
 *
 * Lookup from the NFS compound dispatch uses
 * super_block_find_for_listener(sb_id, listener_id), which returns NULL
 * if the SB is owned by a different listener.  That is what makes an FH
 * minted on :4098 and presented on :2049 fail sb lookup (and the
 * compound handler return NFS4ERR_STALE).
 *
 * These tests are pure data-structure unit tests: no RPC, no io_uring,
 * no live listener.  The listener-bind lifecycle is exercised by
 * functional tests under scripts/ later.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <stdlib.h>

#include <check.h>

#include "reffs/super_block.h"
#include "reffs/fs.h"
#include "fs_test_harness.h"

/* ------------------------------------------------------------------ */
/* Listener 0 is the native namespace.                                 */
/* ------------------------------------------------------------------ */

START_TEST(test_native_sb_has_listener_id_zero)
{
	struct super_block *sb;

	sb = super_block_find(SUPER_BLOCK_ROOT_ID);
	ck_assert_ptr_nonnull(sb);
	ck_assert_uint_eq(sb->sb_listener_id, 0);
	super_block_put(sb);
}
END_TEST

START_TEST(test_alloc_default_listener_id_is_zero)
{
	struct super_block *sb;

	sb = super_block_alloc(42, "/alpha", REFFS_STORAGE_RAM, NULL);
	ck_assert_ptr_nonnull(sb);
	ck_assert_uint_eq(sb->sb_listener_id, 0);
	super_block_put(sb);
}
END_TEST

/* ------------------------------------------------------------------ */
/* find_for_listener filters by listener id.                          */
/* ------------------------------------------------------------------ */

START_TEST(test_find_for_listener_native_finds_native)
{
	struct super_block *sb;

	/* Root is listener 0; finding it from listener 0 should succeed. */
	sb = super_block_find_for_listener(SUPER_BLOCK_ROOT_ID, 0);
	ck_assert_ptr_nonnull(sb);
	ck_assert_uint_eq(sb->sb_id, SUPER_BLOCK_ROOT_ID);
	ck_assert_uint_eq(sb->sb_listener_id, 0);
	super_block_put(sb);
}
END_TEST

START_TEST(test_find_for_listener_rejects_cross_listener)
{
	struct super_block *sb;

	/* Root is listener 0.  Asking for it from listener 1 must miss --
	 * this is the rule that makes an FH minted on :4098 presented on
	 * :2049 (or vice versa) return NFS4ERR_STALE. */
	sb = super_block_find_for_listener(SUPER_BLOCK_ROOT_ID, 1);
	ck_assert_ptr_null(sb);
}
END_TEST

START_TEST(test_find_for_listener_proxy_finds_proxy)
{
	struct super_block *native;
	struct super_block *proxy;
	struct super_block *found;
	int ret;

	/*
	 * Create a proxy SB owned by listener 1.  sb_id values are scoped
	 * per listener; this SB can share an id with native SBs without
	 * conflict -- the (sb_id, listener_id) pair is the key.
	 */
	proxy = super_block_alloc(99, "/proxy", REFFS_STORAGE_RAM, NULL);
	ck_assert_ptr_nonnull(proxy);
	proxy->sb_listener_id = 1;

	ret = super_block_dirent_create(proxy, NULL, reffs_life_action_birth);
	ck_assert_int_eq(ret, 0);

	/* Lookup from listener 1 finds it. */
	found = super_block_find_for_listener(99, 1);
	ck_assert_ptr_nonnull(found);
	ck_assert_ptr_eq(found, proxy);
	ck_assert_uint_eq(found->sb_listener_id, 1);
	super_block_put(found);

	/* Lookup from listener 0 (native) must not. */
	native = super_block_find_for_listener(99, 0);
	ck_assert_ptr_null(native);

	super_block_release_dirents(proxy);
	super_block_put(proxy);
}
END_TEST

START_TEST(test_find_global_ignores_listener)
{
	struct super_block *proxy;
	struct super_block *found;
	int ret;

	/*
	 * super_block_find() (no listener arg) still does a global lookup.
	 * Probe/admin paths rely on this -- reffs-probe can inspect SBs
	 * belonging to any listener, by design.  Only the per-compound
	 * NFS dispatch path cares about scoping.
	 */
	proxy = super_block_alloc(77, "/other", REFFS_STORAGE_RAM, NULL);
	ck_assert_ptr_nonnull(proxy);
	proxy->sb_listener_id = 1;

	ret = super_block_dirent_create(proxy, NULL, reffs_life_action_birth);
	ck_assert_int_eq(ret, 0);

	found = super_block_find(77);
	ck_assert_ptr_nonnull(found);
	ck_assert_uint_eq(found->sb_id, 77);
	super_block_put(found);

	super_block_release_dirents(proxy);
	super_block_put(proxy);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite                                                               */
/* ------------------------------------------------------------------ */

static Suite *sb_listener_id_suite(void)
{
	Suite *s;
	TCase *tc;

	s = suite_create("sb_listener_id");

	tc = tcase_create("native_default");
	tcase_add_checked_fixture(tc, fs_test_setup, fs_test_teardown);
	tcase_add_test(tc, test_native_sb_has_listener_id_zero);
	tcase_add_test(tc, test_alloc_default_listener_id_is_zero);
	suite_add_tcase(s, tc);

	tc = tcase_create("find_for_listener");
	tcase_add_checked_fixture(tc, fs_test_setup, fs_test_teardown);
	tcase_add_test(tc, test_find_for_listener_native_finds_native);
	tcase_add_test(tc, test_find_for_listener_rejects_cross_listener);
	tcase_add_test(tc, test_find_for_listener_proxy_finds_proxy);
	tcase_add_test(tc, test_find_global_ignores_listener);
	suite_add_tcase(s, tc);

	return s;
}

int main(void)
{
	int nfailed;
	Suite *s = sb_listener_id_suite();
	SRunner *sr = srunner_create(s);

	srunner_run_all(sr, CK_NORMAL);
	nfailed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return nfailed ? 1 : 0;
}
