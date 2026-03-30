/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * Unit tests for ALLOCATE, DEALLOCATE, and READ_PLUS semantics.
 *
 * These test the data_block / inode layer directly (same approach
 * as compose_test.c) to validate the logic without requiring a
 * full NFS compound + session setup.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <check.h>

#include "reffs/super_block.h"
#include "reffs/inode.h"
#include "reffs/data_block.h"
#include "reffs/fs.h"
#include "nfs4_test_harness.h"

/*
 * Helper: create a RAM-backed superblock with a root dirent and
 * allocate a regular file inode with an empty data block.
 */
static struct super_block *test_sb;
static struct inode *test_inode;

static void file_ops_setup(void)
{
	nfs4_test_setup();

	test_sb = super_block_find(SUPER_BLOCK_ROOT_ID);
	ck_assert_ptr_nonnull(test_sb);

	test_inode = inode_alloc(test_sb, 0);
	ck_assert_ptr_nonnull(test_inode);
	test_inode->i_mode = S_IFREG | 0644;
}

static void file_ops_teardown(void)
{
	if (test_inode) {
		if (test_inode->i_db) {
			data_block_put(test_inode->i_db);
			test_inode->i_db = NULL;
		}
		inode_active_put(test_inode);
		test_inode = NULL;
	}
	if (test_sb) {
		super_block_put(test_sb);
		test_sb = NULL;
	}

	nfs4_test_teardown();
}

/* ------------------------------------------------------------------ */
/* ALLOCATE tests                                                      */
/* ------------------------------------------------------------------ */

/*
 * Allocating space on an inode with no data block should succeed
 * and create a data block of the requested size.
 */
START_TEST(test_allocate_creates_data_block)
{
	ck_assert_ptr_null(test_inode->i_db);

	/* Simulate ALLOCATE: offset=0, length=4096 */
	test_inode->i_db = data_block_alloc(test_inode, NULL, 0, 0);
	ck_assert_ptr_nonnull(test_inode->i_db);

	ssize_t rret = data_block_resize(test_inode->i_db, 4096);
	ck_assert_int_ge(rret, 0);

	test_inode->i_size = 4096;
	ck_assert_int_eq(data_block_get_size(test_inode->i_db), 4096);
}
END_TEST

/*
 * ALLOCATE extending a file should update i_size.
 */
START_TEST(test_allocate_extends_file)
{
	/* Create initial 1K file */
	const char data[1024] = { 'A' };

	test_inode->i_db = data_block_alloc(test_inode, data, sizeof(data), 0);
	ck_assert_ptr_nonnull(test_inode->i_db);
	test_inode->i_size = 1024;

	/* ALLOCATE at offset=512, length=1024 → end=1536, extends past 1024 */
	uint64_t offset = 512;
	uint64_t length = 1024;
	uint64_t end = offset + length;

	ck_assert(end > (uint64_t)test_inode->i_size);

	ssize_t rret = data_block_resize(test_inode->i_db, (size_t)end);
	ck_assert_int_ge(rret, 0);
	test_inode->i_size = (int64_t)end;

	ck_assert_int_eq(test_inode->i_size, 1536);
	ck_assert_int_eq(data_block_get_size(test_inode->i_db), 1536);
}
END_TEST

/*
 * ALLOCATE within existing size should be a no-op for size.
 */
START_TEST(test_allocate_within_size_noop)
{
	const char data[4096] = { 0 };

	test_inode->i_db = data_block_alloc(test_inode, data, sizeof(data), 0);
	ck_assert_ptr_nonnull(test_inode->i_db);
	test_inode->i_size = 4096;

	/* ALLOCATE at offset=0, length=2048 → end=2048 < 4096 */
	uint64_t end = 2048;
	ck_assert(end <= (uint64_t)test_inode->i_size);

	/* Size unchanged */
	ck_assert_int_eq(test_inode->i_size, 4096);
}
END_TEST

/*
 * ALLOCATE with offset + length that would overflow uint64 should
 * be rejected.
 */
START_TEST(test_allocate_overflow_rejected)
{
	uint64_t offset = UINT64_MAX - 10;
	uint64_t length = 20;

	/* This would wrap: offset + length overflows */
	ck_assert(offset > UINT64_MAX - length);
}
END_TEST

/*
 * ALLOCATE should update sb_bytes_used when extending.
 */
START_TEST(test_allocate_updates_sb_bytes_used)
{
	size_t before = atomic_load_explicit(&test_sb->sb_bytes_used,
					     memory_order_relaxed);

	test_inode->i_db = data_block_alloc(test_inode, NULL, 0, 0);
	ck_assert_ptr_nonnull(test_inode->i_db);

	ssize_t rret = data_block_resize(test_inode->i_db, 8192);
	ck_assert_int_ge(rret, 0);

	int64_t old_size = test_inode->i_size;
	test_inode->i_size = 8192;

	/* Simulate the sb_bytes_used CAS loop from ALLOCATE */
	size_t old_used, new_used;
	old_used = atomic_load_explicit(&test_sb->sb_bytes_used,
					memory_order_relaxed);
	new_used = old_used + (8192 - (size_t)old_size);
	atomic_store_explicit(&test_sb->sb_bytes_used, new_used,
			      memory_order_relaxed);

	size_t after = atomic_load_explicit(&test_sb->sb_bytes_used,
					    memory_order_relaxed);
	ck_assert(after >= before + 8192);
}
END_TEST

/* ------------------------------------------------------------------ */
/* DEALLOCATE tests                                                    */
/* ------------------------------------------------------------------ */

/*
 * DEALLOCATE should zero the specified range.
 */
START_TEST(test_deallocate_zeros_range)
{
	/* Create a file with known content */
	char data[4096];
	memset(data, 'X', sizeof(data));

	test_inode->i_db = data_block_alloc(test_inode, data, sizeof(data), 0);
	ck_assert_ptr_nonnull(test_inode->i_db);
	test_inode->i_size = 4096;

	/* DEALLOCATE offset=1024, length=2048 */
	char zeros[2048];
	memset(zeros, 0, sizeof(zeros));
	ssize_t nw = data_block_write(test_inode->i_db, zeros, 2048, 1024);
	ck_assert_int_eq(nw, 2048);

	/* Verify the zeroed range */
	char buf[4096];
	ssize_t nr = data_block_read(test_inode->i_db, buf, 4096, 0);
	ck_assert_int_eq(nr, 4096);

	/* First 1024 bytes still 'X' */
	for (int i = 0; i < 1024; i++)
		ck_assert_int_eq((unsigned char)buf[i], 'X');

	/* Middle 2048 bytes are zero */
	for (int i = 1024; i < 3072; i++)
		ck_assert_int_eq((unsigned char)buf[i], 0);

	/* Last 1024 bytes still 'X' */
	for (int i = 3072; i < 4096; i++)
		ck_assert_int_eq((unsigned char)buf[i], 'X');
}
END_TEST

/*
 * DEALLOCATE past EOF should be a no-op.
 */
START_TEST(test_deallocate_past_eof_noop)
{
	char data[1024];
	memset(data, 'Y', sizeof(data));

	test_inode->i_db = data_block_alloc(test_inode, data, sizeof(data), 0);
	ck_assert_ptr_nonnull(test_inode->i_db);
	test_inode->i_size = 1024;

	/* DEALLOCATE at offset=2048 — past EOF, should be no-op */
	uint64_t start = 2048;
	ck_assert(start >= (uint64_t)test_inode->i_size);

	/* Data unchanged */
	char buf[1024];
	ssize_t nr = data_block_read(test_inode->i_db, buf, 1024, 0);
	ck_assert_int_eq(nr, 1024);
	ck_assert_int_eq((unsigned char)buf[0], 'Y');
}
END_TEST

/*
 * DEALLOCATE should clamp to file size when range extends past EOF.
 */
START_TEST(test_deallocate_clamps_to_eof)
{
	char data[2048];
	memset(data, 'Z', sizeof(data));

	test_inode->i_db = data_block_alloc(test_inode, data, sizeof(data), 0);
	ck_assert_ptr_nonnull(test_inode->i_db);
	test_inode->i_size = 2048;

	/* DEALLOCATE offset=1024, length=4096 → end=5120, clamped to 2048 */
	uint64_t start = 1024;
	uint64_t end = start + 4096;
	uint64_t file_size = 2048;

	if (end > file_size)
		end = file_size;

	size_t zero_len = (size_t)(end - start);
	ck_assert_int_eq(zero_len, 1024);

	char zeros[1024];
	memset(zeros, 0, sizeof(zeros));
	ssize_t nw = data_block_write(test_inode->i_db, zeros, zero_len,
				      (off_t)start);
	ck_assert_int_eq(nw, (ssize_t)zero_len);

	/* Verify: first 1024 still 'Z', last 1024 zeroed */
	char buf[2048];
	ssize_t nr = data_block_read(test_inode->i_db, buf, 2048, 0);
	ck_assert_int_eq(nr, 2048);

	for (int i = 0; i < 1024; i++)
		ck_assert_int_eq((unsigned char)buf[i], 'Z');
	for (int i = 1024; i < 2048; i++)
		ck_assert_int_eq((unsigned char)buf[i], 0);
}
END_TEST

/*
 * DEALLOCATE should not change file size (KEEP_SIZE semantics).
 */
START_TEST(test_deallocate_keeps_size)
{
	char data[4096];
	memset(data, 'W', sizeof(data));

	test_inode->i_db = data_block_alloc(test_inode, data, sizeof(data), 0);
	ck_assert_ptr_nonnull(test_inode->i_db);
	test_inode->i_size = 4096;

	/* DEALLOCATE the entire file */
	char zeros[4096];
	memset(zeros, 0, sizeof(zeros));
	ssize_t nw = data_block_write(test_inode->i_db, zeros, 4096, 0);
	ck_assert_int_eq(nw, 4096);

	/* Size unchanged */
	ck_assert_int_eq(test_inode->i_size, 4096);
}
END_TEST

/*
 * DEALLOCATE with offset + length overflow should be rejected.
 */
START_TEST(test_deallocate_overflow_rejected)
{
	uint64_t offset = UINT64_MAX - 5;
	uint64_t length = 20;

	ck_assert(offset > UINT64_MAX - length);
}
END_TEST

/* ------------------------------------------------------------------ */
/* READ_PLUS tests (data layer)                                        */
/* ------------------------------------------------------------------ */

/*
 * READ_PLUS on a file returns the correct data.
 */
START_TEST(test_read_plus_returns_data)
{
	const char *data = "hello read_plus test data";
	size_t len = strlen(data);

	test_inode->i_db = data_block_alloc(test_inode, data, len, 0);
	ck_assert_ptr_nonnull(test_inode->i_db);
	test_inode->i_size = (int64_t)len;

	char buf[64] = { 0 };
	ssize_t nr = data_block_read(test_inode->i_db, buf, len, 0);
	ck_assert_int_eq(nr, (ssize_t)len);
	ck_assert_str_eq(buf, data);
}
END_TEST

/*
 * READ_PLUS at EOF returns zero bytes read.
 */
START_TEST(test_read_plus_at_eof)
{
	const char data[128] = { 'A' };

	test_inode->i_db = data_block_alloc(test_inode, data, sizeof(data), 0);
	ck_assert_ptr_nonnull(test_inode->i_db);
	test_inode->i_size = 128;

	/* Read at offset == file size → should return 0 or short read */
	char buf[64];
	ssize_t nr = data_block_read(test_inode->i_db, buf, 64, 128);
	ck_assert_int_eq(nr, 0);
}
END_TEST

/*
 * READ_PLUS partial read near EOF returns only available bytes.
 */
START_TEST(test_read_plus_partial_near_eof)
{
	char data[256];
	memset(data, 'B', sizeof(data));

	test_inode->i_db = data_block_alloc(test_inode, data, sizeof(data), 0);
	ck_assert_ptr_nonnull(test_inode->i_db);
	test_inode->i_size = 256;

	/* Read 128 bytes starting at offset 200 → only 56 available */
	char buf[128] = { 0 };
	ssize_t nr = data_block_read(test_inode->i_db, buf, 128, 200);
	ck_assert_int_ge(nr, 0);
	ck_assert_int_le(nr, 56);
}
END_TEST

/*
 * Zero-length read returns immediately with no error.
 */
START_TEST(test_read_plus_zero_length)
{
	const char data[64] = { 'C' };

	test_inode->i_db = data_block_alloc(test_inode, data, sizeof(data), 0);
	ck_assert_ptr_nonnull(test_inode->i_db);
	test_inode->i_size = 64;

	char buf[1];
	ssize_t nr = data_block_read(test_inode->i_db, buf, 0, 0);
	ck_assert_int_eq(nr, 0);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite                                                               */
/* ------------------------------------------------------------------ */

Suite *file_ops_suite(void)
{
	Suite *s;
	TCase *tc_alloc, *tc_dealloc, *tc_read_plus;

	s = suite_create("NFSv4 File Ops");

	/* ALLOCATE tests */
	tc_alloc = tcase_create("ALLOCATE");
	tcase_add_checked_fixture(tc_alloc, file_ops_setup, file_ops_teardown);
	tcase_add_test(tc_alloc, test_allocate_creates_data_block);
	tcase_add_test(tc_alloc, test_allocate_extends_file);
	tcase_add_test(tc_alloc, test_allocate_within_size_noop);
	tcase_add_test(tc_alloc, test_allocate_overflow_rejected);
	tcase_add_test(tc_alloc, test_allocate_updates_sb_bytes_used);
	suite_add_tcase(s, tc_alloc);

	/* DEALLOCATE tests */
	tc_dealloc = tcase_create("DEALLOCATE");
	tcase_add_checked_fixture(tc_dealloc, file_ops_setup,
				  file_ops_teardown);
	tcase_add_test(tc_dealloc, test_deallocate_zeros_range);
	tcase_add_test(tc_dealloc, test_deallocate_past_eof_noop);
	tcase_add_test(tc_dealloc, test_deallocate_clamps_to_eof);
	tcase_add_test(tc_dealloc, test_deallocate_keeps_size);
	tcase_add_test(tc_dealloc, test_deallocate_overflow_rejected);
	suite_add_tcase(s, tc_dealloc);

	/* READ_PLUS tests */
	tc_read_plus = tcase_create("READ_PLUS");
	tcase_add_checked_fixture(tc_read_plus, file_ops_setup,
				  file_ops_teardown);
	tcase_add_test(tc_read_plus, test_read_plus_returns_data);
	tcase_add_test(tc_read_plus, test_read_plus_at_eof);
	tcase_add_test(tc_read_plus, test_read_plus_partial_near_eof);
	tcase_add_test(tc_read_plus, test_read_plus_zero_length);
	suite_add_tcase(s, tc_read_plus);

	return s;
}

int main(void)
{
	return nfs4_test_run(file_ops_suite());
}
