/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * Unit coverage for ps_inode.c -- per-inode upstream FH storage in
 * the existing i_storage_private slot.
 *
 * Heavy fixture (fs_test_harness + ps_sb_alloc_for_export) because
 * a real proxy SB + real inode is the only way to exercise the
 * convention.  Same per-binary _LDADD pattern as ps_sb_alloc_test.
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <check.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <urcu.h>

#include "reffs/dirent.h"
#include "reffs/fs.h"
#include "reffs/inode.h"
#include "reffs/ns.h"
#include "reffs/settings.h"
#include "reffs/super_block.h"

#include "ps_inode.h"
#include "ps_proxy_ops.h"
#include "ps_sb.h"
#include "ps_state.h"

#include "fs_test_harness.h"

#define TEST_LISTENER_ID 21

static const struct ps_listener_state *g_pls;

static void inode_setup(void)
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

	g_pls = ps_state_find(TEST_LISTENER_ID);
	ck_assert_ptr_nonnull(g_pls);
}

static void inode_teardown(void)
{
	g_pls = NULL;
	ps_state_fini();
	fs_test_teardown();
}

/*
 * Walk super_block_list for a proxy SB we just created.  Copy of
 * the helper from ps_sb_alloc_test -- same contract; keeping both
 * self-contained is cheaper than cross-file sharing for one helper.
 */
static struct super_block *find_proxy_sb(const char *path)
{
	struct cds_list_head *list = super_block_list_head();
	struct super_block *sb;
	struct super_block *found = NULL;

	rcu_read_lock();
	cds_list_for_each_entry_rcu(sb, list, sb_link) {
		if (sb->sb_listener_id != TEST_LISTENER_ID)
			continue;
		if (sb->sb_id == SUPER_BLOCK_ROOT_ID)
			continue;
		if (sb->sb_path && strcmp(sb->sb_path, path) == 0) {
			found = super_block_get(sb);
			break;
		}
	}
	rcu_read_unlock();
	return found;
}

static void cleanup_proxy_sb(const char *path)
{
	struct super_block *sb = find_proxy_sb(path);

	if (!sb)
		return;
	super_block_unmount(sb);
	super_block_destroy(sb);
	super_block_release_dirents(sb);
	super_block_put(sb); /* find ref */
	super_block_put(sb); /* alloc ref */
}

/*
 * Proxy SB's root inode: accessor falls back to the SB binding's
 * root FH (cached at discovery time before the inode existed).  No
 * explicit set_upstream_fh call -- that's the point of the
 * fallback.
 */
START_TEST(test_get_upstream_fh_root_fallback)
{
	uint8_t bind_fh[] = { 0x12, 0x34, 0x56, 0x78 };

	ck_assert_int_eq(ps_sb_alloc_for_export(g_pls, "/root_test", bind_fh,
						sizeof(bind_fh)),
			 0);

	struct super_block *sb = find_proxy_sb("/root_test");

	ck_assert_ptr_nonnull(sb);
	ck_assert_ptr_nonnull(sb->sb_root_inode);

	uint8_t got[PS_MAX_FH_SIZE];
	uint32_t got_len = 0;

	ck_assert_int_eq(ps_inode_get_upstream_fh(sb->sb_root_inode, got,
						  sizeof(got), &got_len),
			 0);
	ck_assert_uint_eq(got_len, sizeof(bind_fh));
	ck_assert_mem_eq(got, bind_fh, sizeof(bind_fh));

	super_block_put(sb); /* find ref */
	cleanup_proxy_sb("/root_test");
}
END_TEST

/*
 * set -> get round-trip on the root inode (sets an explicit FH that
 * overrides the SB-binding fallback).  Proves the per-inode storage
 * path works and that set overrides fallback.
 */
START_TEST(test_set_get_roundtrip_root)
{
	uint8_t bind_fh[] = { 0xAA, 0xBB };
	uint8_t override_fh[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02 };

	ck_assert_int_eq(ps_sb_alloc_for_export(g_pls, "/rw_test", bind_fh,
						sizeof(bind_fh)),
			 0);

	struct super_block *sb = find_proxy_sb("/rw_test");

	ck_assert_int_eq(ps_inode_set_upstream_fh(sb->sb_root_inode,
						  override_fh,
						  sizeof(override_fh)),
			 0);

	uint8_t got[PS_MAX_FH_SIZE];
	uint32_t got_len = 0;

	ck_assert_int_eq(ps_inode_get_upstream_fh(sb->sb_root_inode, got,
						  sizeof(got), &got_len),
			 0);
	ck_assert_uint_eq(got_len, sizeof(override_fh));
	ck_assert_mem_eq(got, override_fh, sizeof(override_fh));

	/* Second set replaces in place (no realloc). */
	uint8_t refreshed[] = { 0x55 };

	ck_assert_int_eq(ps_inode_set_upstream_fh(sb->sb_root_inode, refreshed,
						  sizeof(refreshed)),
			 0);
	got_len = 0;
	ck_assert_int_eq(ps_inode_get_upstream_fh(sb->sb_root_inode, got,
						  sizeof(got), &got_len),
			 0);
	ck_assert_uint_eq(got_len, sizeof(refreshed));
	ck_assert_mem_eq(got, refreshed, sizeof(refreshed));

	super_block_put(sb);
	cleanup_proxy_sb("/rw_test");
}
END_TEST

/*
 * Argument rejection: NULL / zero-len / oversized FH, and calling
 * set on a non-proxy SB's inode.  The non-proxy guard is what
 * prevents i_storage_private from being overwritten for inodes
 * whose SBs might use the slot differently in the future.
 */
START_TEST(test_set_upstream_fh_rejects_bad_args)
{
	struct super_block *root = super_block_find(SUPER_BLOCK_ROOT_ID);

	ck_assert_ptr_nonnull(root);
	struct inode *native_root = root->sb_root_inode;

	uint8_t fh[] = { 0x01 };
	uint8_t big[PS_MAX_FH_SIZE + 1];

	memset(big, 0xAB, sizeof(big));

	ck_assert_int_eq(ps_inode_set_upstream_fh(NULL, fh, sizeof(fh)),
			 -EINVAL);
	ck_assert_int_eq(ps_inode_set_upstream_fh(native_root, NULL,
						  sizeof(fh)),
			 -EINVAL);
	ck_assert_int_eq(ps_inode_set_upstream_fh(native_root, fh, 0), -EINVAL);
	ck_assert_int_eq(ps_inode_set_upstream_fh(native_root, big,
						  sizeof(big)),
			 -E2BIG);

	/* Native SB's inode (sb_proxy_binding == NULL) -> -EINVAL. */
	ck_assert_int_eq(ps_inode_set_upstream_fh(native_root, fh, sizeof(fh)),
			 -EINVAL);

	super_block_put(root);
}
END_TEST

/*
 * Getter returns -EINVAL for non-proxy SBs, -ENOSPC if the caller's
 * buffer is too small.  ENOSPC branch uses the root-fallback path
 * since we can reliably force "stored FH > buf_len" that way.
 */
START_TEST(test_get_upstream_fh_edge_cases)
{
	uint8_t fh[PS_MAX_FH_SIZE];
	uint32_t fh_len = 0;

	/* Non-proxy SB: -EINVAL. */
	struct super_block *root = super_block_find(SUPER_BLOCK_ROOT_ID);

	ck_assert_int_eq(ps_inode_get_upstream_fh(root->sb_root_inode, fh,
						  sizeof(fh), &fh_len),
			 -EINVAL);
	super_block_put(root);

	/* NULL args: -EINVAL. */
	ck_assert_int_eq(ps_inode_get_upstream_fh(NULL, fh, sizeof(fh),
						  &fh_len),
			 -EINVAL);

	/* Proxy SB, small buf: -ENOSPC via root-fallback path. */
	uint8_t bind_fh[64];

	memset(bind_fh, 0xEE, sizeof(bind_fh));
	ck_assert_int_eq(ps_sb_alloc_for_export(g_pls, "/too_small", bind_fh,
						sizeof(bind_fh)),
			 0);

	struct super_block *sb = find_proxy_sb("/too_small");
	uint8_t tiny[16];

	ck_assert_int_eq(ps_inode_get_upstream_fh(sb->sb_root_inode, tiny,
						  sizeof(tiny), &fh_len),
			 -ENOSPC);

	super_block_put(sb);
	cleanup_proxy_sb("/too_small");
}
END_TEST

/*
 * ps_proxy_lookup_forward_for_inode arg validation.  The live-MDS
 * happy path is deferred to CI integration + slice 2e-iv-g.  The
 * "not a proxy SB" guard is the interesting one -- it prevents the
 * function from calling into the PS session registry for inodes
 * whose SBs might in the future use i_storage_private differently.
 */
START_TEST(test_lookup_forward_rejects_native_sb)
{
	uint8_t child_fh[PS_MAX_FH_SIZE];
	uint32_t child_len = 0;
	const char *name = "file.txt";

	struct super_block *root = super_block_find(SUPER_BLOCK_ROOT_ID);

	ck_assert_ptr_nonnull(root);

	/*
	 * Native-SB inode: the function must refuse rather than try
	 * to look up a non-existent binding / listener session.
	 */
	ck_assert_int_eq(ps_proxy_lookup_forward_for_inode(
				 root->sb_root_inode, name, 8, child_fh,
				 sizeof(child_fh), &child_len, NULL, 0, NULL,
				 NULL),
			 -EINVAL);

	super_block_put(root);
}
END_TEST

START_TEST(test_lookup_forward_bad_args)
{
	uint8_t bind_fh[] = { 0x77, 0x88 };
	uint8_t child_fh[PS_MAX_FH_SIZE];
	uint32_t child_len = 0;

	ck_assert_int_eq(ps_sb_alloc_for_export(g_pls, "/look_test", bind_fh,
						sizeof(bind_fh)),
			 0);

	struct super_block *sb = find_proxy_sb("/look_test");
	struct inode *proxy_root = sb->sb_root_inode;

	ck_assert_int_eq(ps_proxy_lookup_forward_for_inode(
				 NULL, "x", 1, child_fh, sizeof(child_fh),
				 &child_len, NULL, 0, NULL, NULL),
			 -EINVAL);
	ck_assert_int_eq(
		ps_proxy_lookup_forward_for_inode(proxy_root, NULL, 1, child_fh,
						  sizeof(child_fh), &child_len,
						  NULL, 0, NULL, NULL),
		-EINVAL);
	ck_assert_int_eq(ps_proxy_lookup_forward_for_inode(
				 proxy_root, "x", 0, child_fh, sizeof(child_fh),
				 &child_len, NULL, 0, NULL, NULL),
			 -EINVAL);
	ck_assert_int_eq(ps_proxy_lookup_forward_for_inode(
				 proxy_root, "x", 1, NULL, sizeof(child_fh),
				 &child_len, NULL, 0, NULL, NULL),
			 -EINVAL);
	ck_assert_int_eq(ps_proxy_lookup_forward_for_inode(
				 proxy_root, "x", 1, child_fh, sizeof(child_fh),
				 NULL, NULL, 0, NULL, NULL),
			 -EINVAL);

	super_block_put(sb);
	cleanup_proxy_sb("/look_test");
}
END_TEST

/*
 * Proxy SB with a registered listener but NULL session (session
 * never opened, or torn down) -> -ENOTCONN.  Lets the op-handler
 * hook distinguish "transient" (DELAY) from "bad arg" (BUG) and
 * "upstream missing" (NOENT).
 */
START_TEST(test_lookup_forward_no_session)
{
	uint8_t bind_fh[] = { 0xCA, 0xFE };
	uint8_t child_fh[PS_MAX_FH_SIZE];
	uint32_t child_len = 0;

	ck_assert_int_eq(ps_sb_alloc_for_export(g_pls, "/nosession", bind_fh,
						sizeof(bind_fh)),
			 0);

	struct super_block *sb = find_proxy_sb("/nosession");

	/*
	 * The fixture registered listener TEST_LISTENER_ID via
	 * ps_state_register but did NOT ps_state_set_session -- so
	 * pls->pls_session is NULL and the forwarder must refuse
	 * before attempting any RPC.
	 */
	ck_assert_int_eq(ps_proxy_lookup_forward_for_inode(
				 sb->sb_root_inode, "file", 4, child_fh,
				 sizeof(child_fh), &child_len, NULL, 0, NULL,
				 NULL),
			 -ENOTCONN);

	super_block_put(sb);
	cleanup_proxy_sb("/nosession");
}
END_TEST

/*
 * Happy path: after the LOOKUP forwarder returns a child FH, the
 * materialize hook creates a local dirent + inode on the proxy SB,
 * wires the upstream FH through ps_inode_set_upstream_fh, and leaves
 * both entries discoverable via dirent_find for subsequent LOOKUPs
 * from the same client.
 */
START_TEST(test_materialize_creates_dirent_and_inode)
{
	uint8_t bind_fh[] = { 0x11, 0x22 };
	uint8_t child_fh[] = { 0xBE, 0xEF, 0xF0, 0x0D };

	ck_assert_int_eq(ps_sb_alloc_for_export(g_pls, "/mat_ok", bind_fh,
						sizeof(bind_fh)),
			 0);

	struct super_block *sb = find_proxy_sb("/mat_ok");
	struct inode *parent = sb->sb_root_inode;

	struct reffs_dirent *child_de = NULL;
	struct inode *child = NULL;

	ck_assert_int_eq(ps_lookup_materialize(parent, "alice.txt", 9, child_fh,
					       sizeof(child_fh), NULL,
					       &child_de, &child),
			 0);
	ck_assert_ptr_nonnull(child_de);
	ck_assert_ptr_nonnull(child);

	/* Visible to a subsequent in-memory LOOKUP. */
	struct reffs_dirent *found =
		dirent_find(parent->i_dirent, reffs_case_get(), "alice.txt");

	ck_assert_ptr_nonnull(found);
	ck_assert_ptr_eq(found, child_de);
	dirent_put(found);

	/* Upstream FH is retrievable. */
	uint8_t got[PS_MAX_FH_SIZE];
	uint32_t got_len = 0;

	ck_assert_int_eq(
		ps_inode_get_upstream_fh(child, got, sizeof(got), &got_len), 0);
	ck_assert_uint_eq(got_len, sizeof(child_fh));
	ck_assert_mem_eq(got, child_fh, sizeof(child_fh));

	inode_active_put(child);
	dirent_put(child_de);
	super_block_put(sb);
	cleanup_proxy_sb("/mat_ok");
}
END_TEST

/*
 * Second materialize with the same name surfaces -EEXIST (concurrent-
 * LOOKUP race).  Callers treat this as "another thread already did it"
 * and fall back to the in-memory dirent.
 */
START_TEST(test_materialize_rejects_duplicate)
{
	uint8_t bind_fh[] = { 0x33 };
	uint8_t fh_a[] = { 0xA1, 0xA2 };
	uint8_t fh_b[] = { 0xB1, 0xB2, 0xB3 };

	ck_assert_int_eq(ps_sb_alloc_for_export(g_pls, "/mat_dup", bind_fh,
						sizeof(bind_fh)),
			 0);

	struct super_block *sb = find_proxy_sb("/mat_dup");
	struct inode *parent = sb->sb_root_inode;

	struct reffs_dirent *de1 = NULL;
	struct inode *in1 = NULL;

	ck_assert_int_eq(ps_lookup_materialize(parent, "dup", 3, fh_a,
					       sizeof(fh_a), NULL, &de1, &in1),
			 0);

	struct reffs_dirent *de2 = NULL;
	struct inode *in2 = NULL;

	ck_assert_int_eq(ps_lookup_materialize(parent, "dup", 3, fh_b,
					       sizeof(fh_b), NULL, &de2, &in2),
			 -EEXIST);
	ck_assert_ptr_null(de2);
	ck_assert_ptr_null(in2);

	inode_active_put(in1);
	dirent_put(de1);
	super_block_put(sb);
	cleanup_proxy_sb("/mat_dup");
}
END_TEST

/*
 * Native SB (no proxy binding) -> -EINVAL.  Keeps the helper from
 * creating bogus dirents on the server's local namespace SBs.
 */
START_TEST(test_materialize_rejects_native_sb)
{
	uint8_t fh[] = { 0x01, 0x02 };
	struct reffs_dirent *de = NULL;
	struct inode *in = NULL;

	struct super_block *root = super_block_find(SUPER_BLOCK_ROOT_ID);

	ck_assert_ptr_nonnull(root);

	ck_assert_int_eq(ps_lookup_materialize(root->sb_root_inode, "x", 1, fh,
					       sizeof(fh), NULL, &de, &in),
			 -EINVAL);

	super_block_put(root);
}
END_TEST

/*
 * NULL / zero-length / oversize-FH guards.  Same flavor as the
 * set_upstream_fh arg tests; belts and braces on the materialize API
 * since it will be reachable from the LOOKUP op handler in the next
 * slice and bad args there would be a server bug we want loud.
 */
START_TEST(test_materialize_rejects_bad_args)
{
	uint8_t bind_fh[] = { 0x44 };
	uint8_t fh[] = { 0x01 };
	uint8_t big[PS_MAX_FH_SIZE + 1];

	memset(big, 0xAB, sizeof(big));

	ck_assert_int_eq(ps_sb_alloc_for_export(g_pls, "/mat_args", bind_fh,
						sizeof(bind_fh)),
			 0);

	struct super_block *sb = find_proxy_sb("/mat_args");
	struct inode *parent = sb->sb_root_inode;
	struct reffs_dirent *de = NULL;
	struct inode *in = NULL;

	ck_assert_int_eq(ps_lookup_materialize(NULL, "a", 1, fh, sizeof(fh),
					       NULL, &de, &in),
			 -EINVAL);
	ck_assert_int_eq(ps_lookup_materialize(parent, NULL, 1, fh, sizeof(fh),
					       NULL, &de, &in),
			 -EINVAL);
	ck_assert_int_eq(ps_lookup_materialize(parent, "a", 0, fh, sizeof(fh),
					       NULL, &de, &in),
			 -EINVAL);
	ck_assert_int_eq(ps_lookup_materialize(parent, "a", 1, NULL, sizeof(fh),
					       NULL, &de, &in),
			 -EINVAL);
	ck_assert_int_eq(ps_lookup_materialize(parent, "a", 1, fh, 0, NULL, &de,
					       &in),
			 -EINVAL);
	ck_assert_int_eq(ps_lookup_materialize(parent, "a", 1, fh, sizeof(fh),
					       NULL, NULL, &in),
			 -EINVAL);
	ck_assert_int_eq(ps_lookup_materialize(parent, "a", 1, fh, sizeof(fh),
					       NULL, &de, NULL),
			 -EINVAL);
	ck_assert_int_eq(ps_lookup_materialize(parent, "a", 1, big, sizeof(big),
					       NULL, &de, &in),
			 -E2BIG);

	super_block_put(sb);
	cleanup_proxy_sb("/mat_args");
}
END_TEST

/*
 * Type promotion: when the caller supplies ps_proxy_attrs_min with
 * have_type set to NF4DIR, materialise sets i_mode's type bits to
 * S_IFDIR and initialises i_nlink to 2 (directory convention).
 * Without this promotion, deep-path traversal via LOOKUP across a
 * proxy SB trips on the S_ISDIR check in nfs4_op_lookup.
 */
START_TEST(test_materialize_promotes_directory_type)
{
	uint8_t bind_fh[] = { 0x55 };
	uint8_t child_fh[] = { 0xAA, 0xBB, 0xCC };
	struct ps_proxy_attrs_min attrs = {
		.have_type = true,
		.type = 2, /* NF4DIR */
		.have_mode = true,
		.mode = 0755,
	};

	ck_assert_int_eq(ps_sb_alloc_for_export(g_pls, "/mat_type", bind_fh,
						sizeof(bind_fh)),
			 0);

	struct super_block *sb = find_proxy_sb("/mat_type");
	struct inode *parent = sb->sb_root_inode;
	struct reffs_dirent *de = NULL;
	struct inode *child = NULL;

	ck_assert_int_eq(ps_lookup_materialize(parent, "subdir", 6, child_fh,
					       sizeof(child_fh), &attrs, &de,
					       &child),
			 0);
	ck_assert_ptr_nonnull(child);
	ck_assert(S_ISDIR(child->i_mode));
	ck_assert_uint_eq(child->i_mode & 07777, 0755u);
	ck_assert_uint_eq(atomic_load_explicit(&child->i_nlink,
					       memory_order_relaxed),
			  2u);

	inode_active_put(child);
	dirent_put(de);
	super_block_put(sb);
	cleanup_proxy_sb("/mat_type");
}
END_TEST

/*
 * Type promotion: NF4REG with mode 0640 produces a regular file
 * with nlink=1 and the requested permission bits.  Complements the
 * directory case above.
 */
START_TEST(test_materialize_promotes_regular_type)
{
	uint8_t bind_fh[] = { 0x66 };
	uint8_t child_fh[] = { 0x11 };
	struct ps_proxy_attrs_min attrs = {
		.have_type = true,
		.type = 1, /* NF4REG */
		.have_mode = true,
		.mode = 0640,
	};

	ck_assert_int_eq(ps_sb_alloc_for_export(g_pls, "/mat_reg", bind_fh,
						sizeof(bind_fh)),
			 0);

	struct super_block *sb = find_proxy_sb("/mat_reg");
	struct inode *parent = sb->sb_root_inode;
	struct reffs_dirent *de = NULL;
	struct inode *child = NULL;

	ck_assert_int_eq(ps_lookup_materialize(parent, "hello.txt", 9, child_fh,
					       sizeof(child_fh), &attrs, &de,
					       &child),
			 0);
	ck_assert(S_ISREG(child->i_mode));
	ck_assert_uint_eq(child->i_mode & 07777, 0640u);
	ck_assert_uint_eq(atomic_load_explicit(&child->i_nlink,
					       memory_order_relaxed),
			  1u);

	inode_active_put(child);
	dirent_put(de);
	super_block_put(sb);
	cleanup_proxy_sb("/mat_reg");
}
END_TEST

/*
 * After a forwarded REMOVE / RENAME succeeds, the proxy needs to
 * drop the corresponding cached dirent or a follow-up LOOKUP
 * through the PS resolves the stale entry.  Materialise a child,
 * confirm it is in cache, invalidate it, confirm it is gone.
 */
START_TEST(test_invalidate_drops_resident_dirent)
{
	uint8_t bind_fh[] = { 0x77 };
	uint8_t child_fh[] = { 0xC0, 0xFF, 0xEE };

	ck_assert_int_eq(ps_sb_alloc_for_export(g_pls, "/inv_drop", bind_fh,
						sizeof(bind_fh)),
			 0);

	struct super_block *sb = find_proxy_sb("/inv_drop");
	struct inode *parent = sb->sb_root_inode;
	struct reffs_dirent *de = NULL;
	struct inode *child = NULL;

	ck_assert_int_eq(ps_lookup_materialize(parent, "doomed", 6, child_fh,
					       sizeof(child_fh), NULL, &de,
					       &child),
			 0);
	/* drop our own caller refs -- only the dirent's parent ref keeps
	 * it alive in the cache after this. */
	inode_active_put(child);
	dirent_put(de);

	struct reffs_dirent *seen = dirent_find(
		parent->i_dirent, reffs_text_case_sensitive, "doomed");
	ck_assert_ptr_nonnull(seen);
	dirent_put(seen);

	ps_invalidate_local_dirent(parent, "doomed", 6);

	struct reffs_dirent *gone = dirent_find(
		parent->i_dirent, reffs_text_case_sensitive, "doomed");
	ck_assert_ptr_null(gone);

	super_block_put(sb);
	cleanup_proxy_sb("/inv_drop");
}
END_TEST

/*
 * Invalidate on a name that was never resident is a no-op -- no
 * disk fault-in (the helper uses dirent_find, not
 * dirent_load_child_by_name) and no crash.  Important because the
 * RENAME forwarder always invalidates `newname` in the destination
 * dir even when there was no collision.
 */
START_TEST(test_invalidate_missing_name_is_noop)
{
	uint8_t bind_fh[] = { 0x78 };

	ck_assert_int_eq(ps_sb_alloc_for_export(g_pls, "/inv_miss", bind_fh,
						sizeof(bind_fh)),
			 0);

	struct super_block *sb = find_proxy_sb("/inv_miss");

	/* No dirent ever materialised for "ghost" -- helper must return
	 * cleanly without faulting in from disk. */
	ps_invalidate_local_dirent(sb->sb_root_inode, "ghost", 5);

	struct reffs_dirent *gone = dirent_find(sb->sb_root_inode->i_dirent,
						reffs_text_case_sensitive,
						"ghost");
	ck_assert_ptr_null(gone);

	super_block_put(sb);
	cleanup_proxy_sb("/inv_miss");
}
END_TEST

/*
 * Helper must guard against bad inputs without crashing -- the
 * dispatcher in dir.c calls it unconditionally on the proxy
 * fast-path success.  NULL parent, NULL/empty name, native SB
 * inode all reach this without any other check.
 */
START_TEST(test_invalidate_handles_bad_args)
{
	/* All-NULL / zero-length cases. */
	ps_invalidate_local_dirent(NULL, "x", 1);
	ps_invalidate_local_dirent((struct inode *)0xDEAD, NULL, 1);
	ps_invalidate_local_dirent((struct inode *)0xDEAD, "x", 0);

	/* Native SB inode -- helper checks sb_proxy_binding before
	 * touching anything else.  Grab sb up front so we can drop
	 * the inode active ref before the super_block_put without
	 * risking root->i_sb becoming UAF in the path. */
	struct super_block *native = super_block_find(SUPER_BLOCK_ROOT_ID);
	struct inode *root = inode_find(native, INODE_ROOT_ID);

	ck_assert_ptr_nonnull(root);
	ck_assert_ptr_null(native->sb_proxy_binding);

	ps_invalidate_local_dirent(root, "anything", 8);
	/* Dirent on the native SB must still be intact and
	 * reachable. */
	ck_assert_ptr_nonnull(root->i_dirent);

	inode_active_put(root);
	super_block_put(native);
}
END_TEST

static Suite *ps_inode_suite(void)
{
	Suite *s = suite_create("ps_inode");
	TCase *tc = tcase_create("core");

	tcase_add_checked_fixture(tc, inode_setup, inode_teardown);
	tcase_add_test(tc, test_get_upstream_fh_root_fallback);
	tcase_add_test(tc, test_set_get_roundtrip_root);
	tcase_add_test(tc, test_set_upstream_fh_rejects_bad_args);
	tcase_add_test(tc, test_get_upstream_fh_edge_cases);
	tcase_add_test(tc, test_lookup_forward_rejects_native_sb);
	tcase_add_test(tc, test_lookup_forward_bad_args);
	tcase_add_test(tc, test_lookup_forward_no_session);
	tcase_add_test(tc, test_materialize_creates_dirent_and_inode);
	tcase_add_test(tc, test_materialize_rejects_duplicate);
	tcase_add_test(tc, test_materialize_rejects_native_sb);
	tcase_add_test(tc, test_materialize_rejects_bad_args);
	tcase_add_test(tc, test_materialize_promotes_directory_type);
	tcase_add_test(tc, test_materialize_promotes_regular_type);
	tcase_add_test(tc, test_invalidate_drops_resident_dirent);
	tcase_add_test(tc, test_invalidate_missing_name_is_noop);
	tcase_add_test(tc, test_invalidate_handles_bad_args);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int failed;
	SRunner *sr = srunner_create(ps_inode_suite());

	srunner_run_all(sr, CK_NORMAL);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return failed == 0 ? 0 : 1;
}
