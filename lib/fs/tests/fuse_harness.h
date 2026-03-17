/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_TEST_FUSE_HARNESS_H
#define _REFFS_TEST_FUSE_HARNESS_H

/*
 * Common setup/teardown for fuse-layer unit tests.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_JEMALLOC
#include <jemalloc/jemalloc.h>
#endif

#include "fs_test_harness.h"
#include "reffs/fuse.h"

/*
 * Per-suite checked setup: initialise the namespace and capture root
 * credentials.  Aborts the test on failure.
 */
static inline void fuse_test_setup(void)
{
	fs_test_setup();
	fuse_test_uid = fs_test_uid;
	fuse_test_gid = fs_test_gid;
}

static inline void fuse_test_teardown(void)
{
	fs_test_teardown();
}

/*
 * Set environment for FUSE unit tests
 */
static inline void fuse_test_init(void)
{
	setenv("REFFS_FUSE_UNIT_TEST", "1", 1);
}

/* Run a fuse test suite */
static inline int fuse_test_run(Suite *s)
{
	return reffs_test_run_suite(s, fuse_test_init, NULL);
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

#endif /* _REFFS_TEST_FUSE_HARNESS_H */
