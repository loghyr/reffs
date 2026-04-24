/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * Unit coverage for ps_sb_alloc_for_export.
 *
 * The function chains together allocation, binding attach, and mount
 * inside a listener-scoped namespace.  Because those calls pull the
 * full lib/fs dep graph, this test lives in lib/nfs4/ps/tests/ with
 * a per-binary _LDADD override -- the other PS unit tests keep their
 * lean link surface.
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <check.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <urcu.h>

#include "reffs/fs.h"
#include "reffs/ns.h"
#include "reffs/settings.h"
#include "reffs/super_block.h"

#include "ps_sb.h"
#include "ps_state.h"

#include "fs_test_harness.h"

#define TEST_LISTENER_ID 11

static void alloc_setup(void)
{
	fs_test_setup();
	ck_assert_int_eq(reffs_ns_init_proxy_listener(TEST_LISTENER_ID), 0);
	ck_assert_int_eq(ps_state_init(), 0);

	struct reffs_proxy_mds_config cfg;

	memset(&cfg, 0, sizeof(cfg));
	cfg.id = TEST_LISTENER_ID;
	cfg.port = 4098;
	cfg.mds_port = 2049;
	cfg.mds_probe = 20490;
	strncpy(cfg.address, "127.0.0.1", sizeof(cfg.address) - 1);

	ck_assert_int_eq(ps_state_register(&cfg), 0);
}

static void alloc_teardown(void)
{
	ps_state_fini();
	fs_test_teardown();
}

/*
 * Walk super_block_list looking for the proxy SB we just created.
 * Returns a ref-held pointer; caller must super_block_put().  Keeps
 * the test independent of the sb_id allocator (we don't know which
 * id was assigned without reading it off the found SB).
 */
static struct super_block *find_proxy_sb_by_path(uint32_t listener_id,
						 const char *path)
{
	struct cds_list_head *list = super_block_list_head();
	struct super_block *sb;
	struct super_block *found = NULL;

	rcu_read_lock();
	cds_list_for_each_entry_rcu(sb, list, sb_link) {
		if (sb->sb_listener_id != listener_id)
			continue;
		if (sb->sb_path && strcmp(sb->sb_path, path) == 0 &&
		    sb->sb_id != SUPER_BLOCK_ROOT_ID) {
			found = super_block_get(sb);
			break;
		}
	}
	rcu_read_unlock();
	return found;
}

/*
 * Happy-path end-to-end: after ps_sb_alloc_for_export returns 0,
 * - a new SB exists scoped to the proxy listener,
 * - the SB is MOUNTED at the requested path,
 * - the proxy binding is attached and carries the FH bytes verbatim,
 * - the listener's root dirent at `path` has the mount crossing set
 *   (exercised indirectly by super_block_mount's RD_MOUNTED_ON).
 */
START_TEST(test_alloc_for_export_happy)
{
	const struct ps_listener_state *pls = ps_state_find(TEST_LISTENER_ID);

	ck_assert_ptr_nonnull(pls);

	uint8_t fh[] = { 0xde, 0xad, 0xbe, 0xef, 0x12 };
	int ret = ps_sb_alloc_for_export(pls, "/data", fh, sizeof(fh));

	ck_assert_int_eq(ret, 0);

	struct super_block *sb =
		find_proxy_sb_by_path(TEST_LISTENER_ID, "/data");

	ck_assert_ptr_nonnull(sb);
	ck_assert_uint_eq(super_block_lifecycle(sb), SB_MOUNTED);

	/* Binding attached and faithful. */
	ck_assert_ptr_nonnull(sb->sb_proxy_binding);

	struct ps_sb_binding *binding = sb->sb_proxy_binding;

	ck_assert_uint_eq(binding->psb_listener_id, TEST_LISTENER_ID);
	ck_assert_uint_eq(binding->psb_mds_fh_len, sizeof(fh));
	ck_assert_mem_eq(binding->psb_mds_fh, fh, sizeof(fh));

	/* Drop the find ref; teardown drains the remaining creation ref. */
	super_block_put(sb);

	/*
	 * Explicitly tear the proxy SB down before fs_test_teardown
	 * runs so the dirent tree is released while the namespace is
	 * still in a well-defined state.  fs_test_teardown's ns_fini
	 * walks every SB and drains, but an in-place destroy keeps
	 * the assertion flow debuggable.
	 */
	sb = find_proxy_sb_by_path(TEST_LISTENER_ID, "/data");
	ck_assert_ptr_nonnull(sb);
	super_block_unmount(sb);
	super_block_destroy(sb);
	super_block_release_dirents(sb);
	super_block_put(sb); /* find ref */
	super_block_put(sb); /* alloc ref */
}
END_TEST

/*
 * Argument validation: every bad-arg combo must short-circuit before
 * any state mutation.  Verified indirectly by expecting the return
 * code and that no new SB shows up on the proxy listener.
 */
START_TEST(test_alloc_for_export_bad_args)
{
	const struct ps_listener_state *pls = ps_state_find(TEST_LISTENER_ID);
	uint8_t fh[] = { 0x01 };
	uint8_t big[PS_MAX_FH_SIZE + 1];

	memset(big, 0xAB, sizeof(big));

	/* NULL pls. */
	ck_assert_int_eq(ps_sb_alloc_for_export(NULL, "/x", fh, sizeof(fh)),
			 -EINVAL);
	/* NULL / empty / relative path. */
	ck_assert_int_eq(ps_sb_alloc_for_export(pls, NULL, fh, sizeof(fh)),
			 -EINVAL);
	ck_assert_int_eq(ps_sb_alloc_for_export(pls, "", fh, sizeof(fh)),
			 -EINVAL);
	ck_assert_int_eq(ps_sb_alloc_for_export(pls, "nope", fh, sizeof(fh)),
			 -EINVAL);
	/* NULL / zero-length FH. */
	ck_assert_int_eq(ps_sb_alloc_for_export(pls, "/y", NULL, sizeof(fh)),
			 -EINVAL);
	ck_assert_int_eq(ps_sb_alloc_for_export(pls, "/y", fh, 0), -EINVAL);
	/* Oversized FH. */
	ck_assert_int_eq(ps_sb_alloc_for_export(pls, "/y", big, sizeof(big)),
			 -E2BIG);

	/* No SB should have been created. */
	ck_assert_ptr_null(find_proxy_sb_by_path(TEST_LISTENER_ID, "/x"));
	ck_assert_ptr_null(find_proxy_sb_by_path(TEST_LISTENER_ID, "/y"));
}
END_TEST

/*
 * Multi-component path: the allocator's mkdir_p_for_listener
 * responsibility includes intermediate components.  "/nested/deep/x"
 * should result in "/nested" and "/nested/deep" existing in the
 * listener's root namespace.
 */
START_TEST(test_alloc_for_export_creates_parent_dirs)
{
	const struct ps_listener_state *pls = ps_state_find(TEST_LISTENER_ID);
	uint8_t fh[] = { 0x55 };

	ck_assert_int_eq(ps_sb_alloc_for_export(pls, "/nested/deep/x", fh,
						sizeof(fh)),
			 0);

	/* Intermediate directories resolve in the listener's namespace. */
	struct name_match *nm = NULL;

	ck_assert_int_eq(find_matching_directory_entry(&nm, TEST_LISTENER_ID,
						       "/nested",
						       LAST_COMPONENT_IS_MATCH),
			 0);
	name_match_free(nm);

	nm = NULL;
	ck_assert_int_eq(find_matching_directory_entry(&nm, TEST_LISTENER_ID,
						       "/nested/deep",
						       LAST_COMPONENT_IS_MATCH),
			 0);
	name_match_free(nm);

	struct super_block *sb =
		find_proxy_sb_by_path(TEST_LISTENER_ID, "/nested/deep/x");
	ck_assert_ptr_nonnull(sb);

	super_block_put(sb);

	sb = find_proxy_sb_by_path(TEST_LISTENER_ID, "/nested/deep/x");
	super_block_unmount(sb);
	super_block_destroy(sb);
	super_block_release_dirents(sb);
	super_block_put(sb); /* find ref */
	super_block_put(sb); /* alloc ref */
}
END_TEST

/*
 * Two allocations for the same path fail the second time with
 * -EEXIST from super_block_check_path_conflict.  Matches the
 * semantics the ps_state_add_export layer above this uses:
 * idempotency lives at the ps_state layer (update-in-place);
 * the SB layer refuses duplicate mounts.
 */
START_TEST(test_alloc_for_export_duplicate_path_rejected)
{
	const struct ps_listener_state *pls = ps_state_find(TEST_LISTENER_ID);
	uint8_t fh[] = { 0x10, 0x20 };

	ck_assert_int_eq(ps_sb_alloc_for_export(pls, "/share", fh, sizeof(fh)),
			 0);
	ck_assert_int_eq(ps_sb_alloc_for_export(pls, "/share", fh, sizeof(fh)),
			 -EEXIST);

	struct super_block *sb =
		find_proxy_sb_by_path(TEST_LISTENER_ID, "/share");
	ck_assert_ptr_nonnull(sb);
	super_block_put(sb);

	sb = find_proxy_sb_by_path(TEST_LISTENER_ID, "/share");
	super_block_unmount(sb);
	super_block_destroy(sb);
	super_block_release_dirents(sb);
	super_block_put(sb);
	super_block_put(sb);
}
END_TEST

/*
 * Cross-listener isolation: a native SB mounted at "/shared" must not
 * block a proxy listener from mounting its own "/shared".  The two
 * namespaces are disjoint by design (sb_listener_id is part of the
 * identity pair), so super_block_check_path_conflict inside
 * ps_sb_alloc_for_export scopes the walk to the caller's listener.
 * Without that scoping this test would fail with -EEXIST.
 */
START_TEST(test_alloc_for_export_crosses_native_listener)
{
	const struct ps_listener_state *pls = ps_state_find(TEST_LISTENER_ID);
	uint8_t fh[] = { 0x77 };

	/*
	 * Stand up a native-listener SB mounted at "/shared".  Uses the
	 * same pattern the probe SB_CREATE handler follows: mkdir_p the
	 * native mount-point, alloc + dirent_create + mount.
	 */
	ck_assert_int_eq(reffs_fs_mkdir_p("/shared", 0755), 0);

	struct super_block *native =
		super_block_alloc(999, "/shared", REFFS_STORAGE_RAM, NULL);
	ck_assert_ptr_nonnull(native);
	/* native->sb_listener_id stays 0 by alloc default. */
	ck_assert_int_eq(super_block_dirent_create(native, NULL,
						   reffs_life_action_birth),
			 0);
	ck_assert_int_eq(super_block_mount(native, "/shared"), 0);

	/* Proxy listener mounts its own "/shared" -- must succeed. */
	ck_assert_int_eq(ps_sb_alloc_for_export(pls, "/shared", fh, sizeof(fh)),
			 0);

	/* Both SBs coexist, each on its own listener. */
	struct super_block *proxy =
		find_proxy_sb_by_path(TEST_LISTENER_ID, "/shared");
	ck_assert_ptr_nonnull(proxy);
	ck_assert_uint_eq(proxy->sb_listener_id, TEST_LISTENER_ID);
	ck_assert_uint_eq(native->sb_listener_id, 0);
	super_block_put(proxy);

	/* Cleanup: proxy first, then native. */
	proxy = find_proxy_sb_by_path(TEST_LISTENER_ID, "/shared");
	super_block_unmount(proxy);
	super_block_destroy(proxy);
	super_block_release_dirents(proxy);
	super_block_put(proxy); /* find ref */
	super_block_put(proxy); /* alloc ref */

	super_block_unmount(native);
	super_block_destroy(native);
	super_block_release_dirents(native);
	super_block_put(native);
}
END_TEST

/*
 * Root-export case: the upstream MDS advertises "/" (reffsd's default
 * [[export]] path).  Rather than fail, ps_sb_alloc_for_export attaches
 * the proxy binding to the listener's existing root SB.  After this
 * call, ps_inode_is_proxy() returns true for inodes on the listener
 * root and every forward hook (LOOKUP / GETATTR) routes upstream.
 */
START_TEST(test_alloc_for_export_root_attaches_to_listener_root)
{
	const struct ps_listener_state *pls = ps_state_find(TEST_LISTENER_ID);
	uint8_t fh[] = { 0xaa, 0xbb, 0xcc };

	ck_assert_int_eq(ps_sb_alloc_for_export(pls, "/", fh, sizeof(fh)), 0);

	/*
	 * Listener root SB (sb_id == SUPER_BLOCK_ROOT_ID, listener_id ==
	 * TEST_LISTENER_ID) now carries a proxy binding.  No new SB was
	 * created at "/" -- the root was upgraded in place.
	 */
	struct super_block *root = super_block_find_for_listener(
		SUPER_BLOCK_ROOT_ID, TEST_LISTENER_ID);

	ck_assert_ptr_nonnull(root);
	ck_assert_ptr_nonnull(root->sb_proxy_binding);
	super_block_put(root);

	/* find_proxy_sb_by_path explicitly skips root SBs; nothing at "/". */
	ck_assert_ptr_null(find_proxy_sb_by_path(TEST_LISTENER_ID, "/"));
}
END_TEST

/*
 * Double-bind refused: a second ps_sb_alloc_for_export(pls, "/", ...)
 * after the first succeeded returns -EEXIST rather than silently
 * overwriting the binding.  Protects against discovery running twice
 * with a different FH (which would need an explicit refresh path
 * with teardown semantics).
 */
START_TEST(test_alloc_for_export_root_double_bind_rejected)
{
	const struct ps_listener_state *pls = ps_state_find(TEST_LISTENER_ID);
	uint8_t fh1[] = { 0x11 };
	uint8_t fh2[] = { 0x22, 0x33 };

	ck_assert_int_eq(ps_sb_alloc_for_export(pls, "/", fh1, sizeof(fh1)), 0);
	ck_assert_int_eq(ps_sb_alloc_for_export(pls, "/", fh2, sizeof(fh2)),
			 -EEXIST);
}
END_TEST

static Suite *ps_sb_alloc_suite(void)
{
	Suite *s = suite_create("ps_sb_alloc");
	TCase *tc = tcase_create("core");

	tcase_add_checked_fixture(tc, alloc_setup, alloc_teardown);
	tcase_add_test(tc, test_alloc_for_export_happy);
	tcase_add_test(tc, test_alloc_for_export_bad_args);
	tcase_add_test(tc, test_alloc_for_export_creates_parent_dirs);
	tcase_add_test(tc, test_alloc_for_export_duplicate_path_rejected);
	tcase_add_test(tc, test_alloc_for_export_crosses_native_listener);
	tcase_add_test(tc,
		       test_alloc_for_export_root_attaches_to_listener_root);
	tcase_add_test(tc, test_alloc_for_export_root_double_bind_rejected);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int failed;
	SRunner *sr = srunner_create(ps_sb_alloc_suite());

	srunner_run_all(sr, CK_NORMAL);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return failed == 0 ? 0 : 1;
}
