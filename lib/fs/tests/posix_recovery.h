/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_TEST_POSIX_RECOVERY_H
#define _REFFS_TEST_POSIX_RECOVERY_H

#include <stdint.h>
#include <stdbool.h>
#include <limits.h>
#include "reffs/super_block.h"
#include "reffs/inode.h"
#include "reffs/dirent.h"
#include "libreffs_test.h"
#include "fs_test_harness.h"

struct test_context {
	char backend_path[PATH_MAX];
	struct super_block *sb;
};

/* Setup a clean test environment with a temporary directory */
int test_setup(struct test_context *ctx);

/* Cleanup the test environment and temporary directory */
void test_teardown(struct test_context *ctx);

/* Helper to write a .meta file */
int test_write_meta(struct test_context *ctx, uint64_t ino,
		    struct inode_disk *id);

/* Helper to write a .dat file */
int test_write_dat(struct test_context *ctx, uint64_t ino, const void *data,
		   size_t size);

/* Helper to write a .lnk file */
int test_write_lnk(struct test_context *ctx, uint64_t ino, const char *target);

/* Helper to write a .dir file (v2 format with cookies) */
int test_write_dir_header(struct test_context *ctx, uint64_t ino,
			  uint64_t cookie_next, int *fd_out);
int test_write_dir_entry(int fd, uint64_t cookie, uint64_t ino,
			 const char *name);

#endif /* _REFFS_TEST_POSIX_RECOVERY_H */
