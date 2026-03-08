/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifndef _REFFS_TEST_FS_HARNESS_H
#define _REFFS_TEST_FS_HARNESS_H

/*
 * Shared harness for fs.c unit tests.
 *
 * Each fs_*.c test file includes this header.  The harness wraps
 * reffs_ns_init()/reffs_ns_fini() so every test gets a pristine namespace,
 * and provides the ck_assert_timespec_* macros that replace the verbose
 * two-field comparisons seen in fuse_test.c.
 *
 * Design rules:
 *  - Tests call reffs_fs_*() directly — never through the fuse shim.
 *  - Each START_TEST creates everything it needs and removes it before
 *    returning.  The checked_fixture teardown calls reffs_ns_fini() so
 *    ASAN sees a clean shutdown even if a test assertion fires mid-way.
 *  - CK_NOFORK is used so ASAN covers the full process lifetime.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_JEMALLOC
#include <jemalloc/jemalloc.h>
#endif

#include <check.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <urcu.h>
#include <urcu/rculist.h>
#include <urcu/ref.h>

#include "reffs/super_block.h"
#include "reffs/inode.h"
#include "reffs/fs.h"
#include "reffs/ns.h"
#include "reffs/context.h"
#include "reffs/log.h"

/*
 * Process-wide uid/gid captured during setup; used by tests that verify
 * uid/gid inheritance from reffs_fs_mkdir / reffs_fs_create.
 */
extern uid_t fs_test_uid;
extern gid_t fs_test_gid;

static inline void fs_test_setup(void)
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

	ret = reffs_ns_init();
	ck_assert_int_eq(ret, 0);

	sb = super_block_find(SUPER_BLOCK_ROOT_ID);
	ck_assert_ptr_nonnull(sb);

	inode = inode_find(sb, 1);
	ck_assert_ptr_nonnull(inode);

	fs_test_uid = getuid();
	fs_test_gid = getgid();
	inode->i_uid = fs_test_uid;
	inode->i_gid = fs_test_gid;

	inode_put(inode);
	super_block_put(sb);
}

static inline void fs_test_teardown(void)
{
	reffs_ns_fini();
}

static inline void fs_test_global_init(void)
{
	rcu_register_thread();
	reffs_trace_init(NULL);
	reffs_trace_enable_all_categories();
	reffs_log_file = stderr;
}

static inline void fs_test_global_fini(void)
{
	reffs_trace_close();
	synchronize_rcu();
	rcu_barrier();
	rcu_unregister_thread();
}

/* Assert timespec A is strictly less than timespec B */
#define ck_assert_timespec_lt(a, b)                                          \
	ck_assert_msg(((a).tv_sec < (b).tv_sec) ||                           \
			      (((a).tv_sec == (b).tv_sec) &&                 \
			       ((a).tv_nsec < (b).tv_nsec)),                 \
		      "expected " #a " < " #b ": "                           \
		      "%ld.%09ld >= %ld.%09ld",                              \
		      (long)(a).tv_sec, (long)(a).tv_nsec, (long)(b).tv_sec, \
		      (long)(b).tv_nsec)

/* Assert timespec A == timespec B */
#define ck_assert_timespec_eq(a, b)                                 \
	do {                                                        \
		ck_assert_msg((a).tv_sec == (b).tv_sec &&           \
				      (a).tv_nsec == (b).tv_nsec,   \
			      "expected " #a " == " #b ": "         \
			      "%ld.%09ld != %ld.%09ld",             \
			      (long)(a).tv_sec, (long)(a).tv_nsec,  \
			      (long)(b).tv_sec, (long)(b).tv_nsec); \
	} while (0)

#endif /* _REFFS_TEST_FS_HARNESS_H */
