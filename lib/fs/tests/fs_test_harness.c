/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <pthread.h>
#include <time.h>
#include <sys/types.h>
#include <unistd.h>
#include <urcu.h>
#include "reffs/context.h"
#include "reffs/fs.h"
#include "reffs/ns.h"
#include "reffs/super_block.h"
#include "reffs/inode.h"
#include "reffs/server.h"
#include "reffs/client.h"
#include "fs_test_harness.h"

uid_t fs_test_uid;
gid_t fs_test_gid;
uid_t fuse_test_uid;
gid_t fuse_test_gid;

/* Server state for tests that need it (like NFS4) */
static struct server_state *fs_test_ss;
static char *fs_test_state_dir;

void reffs_test_setup_fs(void)
{
	struct super_block *sb;
	struct inode *inode;
	int ret;
	struct reffs_context ctx;

	fs_test_uid = getuid();
	fs_test_gid = getgid();
	ctx.uid = fs_test_uid;
	ctx.gid = fs_test_gid;
	reffs_set_context(&ctx);

	rcu_barrier();
	/* Ensure we use RAM backend for tests by default */
	reffs_fs_set_storage(REFFS_STORAGE_RAM, NULL);

	ret = reffs_ns_init();
	ck_assert_int_eq(ret, 0);

	sb = super_block_find(SUPER_BLOCK_ROOT_ID);
	ck_assert_ptr_nonnull(sb);

	inode = inode_find(sb, INODE_ROOT_ID);
	ck_assert_ptr_nonnull(inode);

	inode->i_uid = REFFS_ID_MAKE(REFFS_ID_UNIX, 0, fs_test_uid);
	inode->i_gid = REFFS_ID_MAKE(REFFS_ID_UNIX, 0, fs_test_gid);

	inode_put(inode);
	super_block_put(sb);
}

void reffs_test_teardown_fs(void)
{
	reffs_ns_fini();
	rcu_barrier();
}

void reffs_test_setup_server(void)
{
	if (fs_test_ss)
		return;

	fs_test_state_dir = reffs_test_create_state_dir();
	ck_assert_ptr_nonnull(fs_test_state_dir);

	fs_test_ss = server_state_init(fs_test_state_dir, 2049,
				       reffs_text_case_sensitive,
				       REFFS_STORAGE_RAM);
	ck_assert_ptr_nonnull(fs_test_ss);
}

void reffs_test_teardown_server(void)
{
	if (fs_test_ss) {
		client_unload_all_clients();
		server_state_fini(fs_test_ss);
		fs_test_ss = NULL;
	}
	if (fs_test_state_dir) {
		reffs_test_remove_state_dir(fs_test_state_dir);
		fs_test_state_dir = NULL;
	}
}
