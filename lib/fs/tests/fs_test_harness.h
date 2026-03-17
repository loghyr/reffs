/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_TEST_FS_HARNESS_H
#define _REFFS_TEST_FS_HARNESS_H

#ifdef HAVE_CONFIG_H
#include "config.h"
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
#include "reffs/server.h"

#include "libreffs_test.h"

/*
 * Process-wide uid/gid captured during setup; used by tests that verify
 * uid/gid inheritance.
 */
extern uid_t fs_test_uid;
extern gid_t fs_test_gid;
extern uid_t fuse_test_uid;
extern gid_t fuse_test_gid;

/* Standard module setup/teardown */
static inline void fs_test_setup(void)
{
	reffs_test_setup_fs();
}

static inline void fs_test_teardown(void)
{
	reffs_test_teardown_fs();
}

/* Run an fs test suite */
static inline int fs_test_run(Suite *s)
{
	return reffs_test_run_suite(s, NULL, NULL);
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
