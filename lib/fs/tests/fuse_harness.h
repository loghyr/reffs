/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifndef _REFFS_TEST_FUSE_HARNESS_H
#define _REFFS_TEST_FUSE_HARNESS_H

/*
 * Common setup/teardown for fuse-layer unit tests.
 *
 * Each fuse_N.c test calls fuse_test_setup() in its checked_setup fixture and
 * fuse_test_teardown() in its checked_teardown fixture.  Because CK_NOFORK is
 * used, the namespace is shared across all tests in a single suite run; each
 * individual test is responsible for leaving the namespace clean (i.e. it
 * must remove everything it creates).
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_JEMALLOC
#include <jemalloc/jemalloc.h>
#endif

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <urcu.h>
#include <urcu/rculist.h>
#include <urcu/ref.h>
#include <check.h>

#include "reffs/super_block.h"
#include "reffs/inode.h"
#include "reffs/fuse.h"
#include "reffs/log.h"
#include "reffs/fs.h"
#include "reffs/ns.h"

/*
 * Root uid/gid captured during setup; tests that verify uid/gid inheritance
 * compare against these.
 */
extern uid_t fuse_test_uid;
extern gid_t fuse_test_gid;

/*
 * Call once per process before srunner_run_all().  Registers the RCU thread.
 * Returns 0 on success.
 */
static inline int fuse_test_global_init(void)
{
	rcu_register_thread();
	setenv("REFFS_FUSE_UNIT_TEST", "1", 1);
	return 0;
}

static inline void fuse_test_global_fini(void)
{
	synchronize_rcu();
	rcu_barrier();
	rcu_unregister_thread();
}

/*
 * Per-suite checked setup: initialise the namespace and capture root
 * credentials.  Aborts the test on failure.
 */
static inline void fuse_test_setup(void)
{
	struct super_block *sb;
	struct inode *inode;
	int ret;

	ret = reffs_ns_init();
	ck_assert_int_eq(ret, 0);

	sb = super_block_find(1);
	ck_assert_ptr_nonnull(sb);

	inode = inode_find(sb, 1);
	ck_assert_ptr_nonnull(inode);

	fuse_test_uid = getuid();
	fuse_test_gid = getgid();
	inode->i_uid = fuse_test_uid;
	inode->i_gid = fuse_test_gid;

	inode_put(inode);
	super_block_put(sb);
}

static inline void fuse_test_teardown(void)
{
	reffs_ns_fini();
}

/*
 * Sleep until the realtime clock has advanced past ref.  Avoids flaky
 * timestamp assertions on fast machines where usleep(1000) may not
 * guarantee a nanosecond-resolution advance.
 */
static inline void sleep_past(const struct timespec *ref)
{
	struct timespec now;
	int tries = 0;
	do {
		usleep(1000);
		clock_gettime(CLOCK_REALTIME, &now);
	} while (now.tv_sec == ref->tv_sec && now.tv_nsec <= ref->tv_nsec &&
		 ++tries < 100);
}

/*
 * Convenience: assert that timespec A < timespec B.
 */
#define ck_assert_timespec_lt(a, b)                          \
	ck_assert_msg(((a).tv_sec < (b).tv_sec) ||           \
			      (((a).tv_sec == (b).tv_sec) && \
			       ((a).tv_nsec < (b).tv_nsec)), \
		      "expected " #a " < " #b)

#define ck_assert_timespec_eq(a, b)                         \
	do {                                                \
		ck_assert_int_eq((a).tv_sec, (b).tv_sec);   \
		ck_assert_int_eq((a).tv_nsec, (b).tv_nsec); \
	} while (0)

#endif /* _REFFS_TEST_FUSE_HARNESS_H */
