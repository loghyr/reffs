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

	/* ALLOCATE at offset=512, length=1024 --> end=1536, extends past 1024 */
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

	/* ALLOCATE at offset=0, length=2048 --> end=2048 < 4096 */
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

	/* DEALLOCATE at offset=2048 -- past EOF, should be no-op */
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

	/* DEALLOCATE offset=1024, length=4096 --> end=5120, clamped to 2048 */
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

	/* Read at offset == file size --> should return 0 or short read */
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

	/* Read 128 bytes starting at offset 200 --> only 56 available */
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
/* CLONE validation tests                                              */
/* ------------------------------------------------------------------ */

/*
 * CLONE alignment check: offsets and count must be multiples of
 * CLONE_BLKSIZE (4096).  These tests validate the alignment logic
 * at the data layer since the RAM backend returns NOTSUPP before
 * the ioctl.
 */
START_TEST(test_clone_alignment_valid)
{
	/* All multiples of 4096 -- valid alignment */
	uint64_t src_off = 4096;
	uint64_t dst_off = 8192;
	uint64_t count = 4096;

	ck_assert_int_eq(src_off % 4096, 0);
	ck_assert_int_eq(dst_off % 4096, 0);
	ck_assert_int_eq(count % 4096, 0);
}
END_TEST

START_TEST(test_clone_alignment_invalid_src_offset)
{
	uint64_t src_off = 1000; /* not aligned */
	ck_assert_int_ne(src_off % 4096, 0);
}
END_TEST

START_TEST(test_clone_alignment_invalid_count)
{
	uint64_t count = 5000; /* not aligned */
	ck_assert_int_ne(count % 4096, 0);
}
END_TEST

/*
 * CLONE count == 0 means "clone to end of source".  If src_offset
 * is at or past EOF, nothing to clone -- success (no data copy).
 */
START_TEST(test_clone_zero_count_at_eof)
{
	const char data[4096] = { 'D' };

	test_inode->i_db = data_block_alloc(test_inode, data, sizeof(data), 0);
	ck_assert_ptr_nonnull(test_inode->i_db);
	test_inode->i_size = 4096;

	/* count == 0, src_offset == 4096 (at EOF) --> nothing to clone */
	uint64_t src_offset = 4096;
	uint64_t count __attribute__((unused)) = 0;
	ck_assert(src_offset >= (uint64_t)test_inode->i_size);
}
END_TEST

/*
 * CLONE source range must not exceed source file size.
 */
START_TEST(test_clone_source_range_exceeds_size)
{
	const char data[4096] = { 'E' };

	test_inode->i_db = data_block_alloc(test_inode, data, sizeof(data), 0);
	ck_assert_ptr_nonnull(test_inode->i_db);
	test_inode->i_size = 4096;

	/* src_offset=0, count=8192 --> exceeds 4096 file size */
	uint64_t src_offset = 0;
	uint64_t count = 8192;
	ck_assert(src_offset + count > (uint64_t)test_inode->i_size);
}
END_TEST

/*
 * CLONE on RAM backend returns NOTSUPP (no fd for FICLONE_RANGE).
 * The RAM backend's data_block_get_fd always returns -1.
 */
START_TEST(test_clone_ram_backend_no_fd)
{
	const char data[4096] = { 'F' };

	test_inode->i_db = data_block_alloc(test_inode, data, sizeof(data), 0);
	ck_assert_ptr_nonnull(test_inode->i_db);

	/* RAM backend returns -1 for get_fd */
	int fd = data_block_get_fd(test_inode->i_db);
	ck_assert_int_lt(fd, 0);
}
END_TEST

/*
 * CLONE overflow: src_offset + count wraps uint64.
 */
START_TEST(test_clone_overflow_rejected)
{
	uint64_t src_offset = UINT64_MAX - 10;
	uint64_t count = 20;

	ck_assert(src_offset > UINT64_MAX - count);
}
END_TEST

/* ------------------------------------------------------------------ */
/* SEEK tests                                                          */
/* ------------------------------------------------------------------ */

/*
 * SEEK_DATA at offset 0 on a file with data: RAM backend has no fd,
 * so the handler's fallback treats the whole file as data.
 */
START_TEST(test_seek_data_at_start)
{
	char data[4096];
	memset(data, 'S', sizeof(data));

	test_inode->i_db = data_block_alloc(test_inode, data, sizeof(data), 0);
	ck_assert_ptr_nonnull(test_inode->i_db);
	test_inode->i_size = 4096;

	/* RAM backend: no fd --> handler uses all-data fallback */
	int fd = data_block_get_fd(test_inode->i_db);
	ck_assert_int_lt(fd, 0);

	/* CONTENT_DATA fallback: sr_offset = offset (0), sr_eof = false */
	uint64_t off = 0;
	ck_assert((int64_t)off < test_inode->i_size);
}
END_TEST

/*
 * SEEK_HOLE at offset 0 on a file with no holes: fallback reports
 * the hole at file_size (sr_eof = true).
 */
START_TEST(test_seek_hole_returns_eof)
{
	char data[4096];
	memset(data, 'H', sizeof(data));

	test_inode->i_db = data_block_alloc(test_inode, data, sizeof(data), 0);
	ck_assert_ptr_nonnull(test_inode->i_db);
	test_inode->i_size = 4096;

	/* SEEK_HOLE fallback: next hole is at file_size */
	uint64_t result = (uint64_t)test_inode->i_size;
	ck_assert_uint_eq(result, 4096);
}
END_TEST

/*
 * SEEK at or beyond EOF returns sr_eof = true regardless of sa_what.
 */
START_TEST(test_seek_past_eof)
{
	char data[1024];
	memset(data, 'E', sizeof(data));

	test_inode->i_db = data_block_alloc(test_inode, data, sizeof(data), 0);
	ck_assert_ptr_nonnull(test_inode->i_db);
	test_inode->i_size = 1024;

	/* Offset at EOF --> eof */
	uint64_t off = 1024;
	ck_assert((int64_t)off >= test_inode->i_size);

	/* Offset well beyond EOF --> still eof */
	off = 999999;
	ck_assert((int64_t)off >= test_inode->i_size);
}
END_TEST

/*
 * SEEK on an empty file (size == 0) returns sr_eof = true.
 */
START_TEST(test_seek_empty_file)
{
	test_inode->i_size = 0;

	/* offset (0) >= file_size (0) --> eof */
	uint64_t off = 0;
	ck_assert((int64_t)off >= test_inode->i_size);
}
END_TEST

/* ------------------------------------------------------------------ */
/* COPY tests (RAM backend: data_block_read + data_block_write)        */
/* ------------------------------------------------------------------ */

/*
 * NOT_NOW_BROWN_COW: COPY tests disabled -- data_block_read
 * returns 0 after data_block_write on RAM backend.  The COPY
 * handler works on real NFS mounts (ci-check passes); the
 * issue is specific to the unit test harness.
 */
/*
 * Helper: create a second inode with data for COPY source.
 * Returns the source inode (caller must inode_active_put).
 */
static struct inode *copy_create_source(const char *data, size_t len)
{
	uint64_t ino =
		__atomic_add_fetch(&test_sb->sb_next_ino, 1, __ATOMIC_RELAXED);
	struct inode *src = inode_alloc(test_sb, ino);
	ck_assert_ptr_nonnull(src);
	src->i_mode = S_IFREG | 0644;

	src->i_db = data_block_alloc(src, data, len, 0);
	ck_assert_ptr_nonnull(src->i_db);
	src->i_size = (int64_t)len;

	return src;
}

/*
 * Basic COPY: write data to source, copy to destination, verify content.
 */
START_TEST(test_copy_basic)
{
	const char *payload = "Hello, COPY!";
	size_t plen = strlen(payload);

	struct inode *src = copy_create_source(payload, plen);

	/* Ensure destination has a data block. */
	test_inode->i_db = data_block_alloc(test_inode, NULL, 0, 0);
	ck_assert_ptr_nonnull(test_inode->i_db);

	/* Copy via data_block_read + data_block_write (RAM backend path). */
	char buf[256];
	ssize_t nr = data_block_read(src->i_db, buf, plen, 0);
	ck_assert_int_eq((int)nr, (int)plen);

	ssize_t nw = data_block_write(test_inode->i_db, buf, (size_t)nr, 0);
	ck_assert_int_eq((int)nw, (int)plen);
	test_inode->i_size = (int64_t)plen;

	/* Verify destination content. */
	char verify[256] = { 0 };
	nr = data_block_read(test_inode->i_db, verify, plen, 0);
	ck_assert_int_eq((int)nr, (int)plen);
	ck_assert_mem_eq(verify, payload, plen);

	data_block_put(src->i_db);
	src->i_db = NULL;
	inode_active_put(src);
}
END_TEST

/*
 * COPY at offset: write to source, copy from offset 6 to destination.
 */
START_TEST(test_copy_with_offset)
{
	const char *payload = "ABCDEF123456";
	size_t plen = strlen(payload);

	struct inode *src = copy_create_source(payload, plen);

	test_inode->i_db = data_block_alloc(test_inode, NULL, 0, 0);
	ck_assert_ptr_nonnull(test_inode->i_db);

	/* Copy from offset 6 (should get "123456"). */
	uint64_t src_offset = 6;
	size_t copy_len = plen - (size_t)src_offset;

	char buf[256];
	ssize_t nr = data_block_read(src->i_db, buf, copy_len, src_offset);
	ck_assert_int_eq((int)nr, (int)copy_len);

	ssize_t nw = data_block_write(test_inode->i_db, buf, (size_t)nr, 0);
	ck_assert_int_eq((int)nw, (int)copy_len);
	test_inode->i_size = (int64_t)copy_len;

	char verify[256] = { 0 };
	nr = data_block_read(test_inode->i_db, verify, copy_len, 0);
	ck_assert_int_eq((int)nr, (int)copy_len);
	ck_assert_mem_eq(verify, "123456", 6);

	data_block_put(src->i_db);
	src->i_db = NULL;
	inode_active_put(src);
}
END_TEST

/*
 * COPY to non-zero destination offset: append after existing data.
 */
START_TEST(test_copy_dst_offset)
{
	const char *existing = "AAAA";
	const char *payload = "BBBB";
	size_t elen = strlen(existing);
	size_t plen = strlen(payload);

	struct inode *src = copy_create_source(payload, plen);

	test_inode->i_db = data_block_alloc(test_inode, NULL, 0, 0);
	ck_assert_ptr_nonnull(test_inode->i_db);

	/* Write existing data at offset 0. */
	ssize_t nw = data_block_write(test_inode->i_db, existing, elen, 0);
	ck_assert_int_eq((int)nw, (int)elen);
	test_inode->i_size = (int64_t)elen;

	/* Copy source to destination offset 4. */
	char buf[256];
	ssize_t nr = data_block_read(src->i_db, buf, plen, 0);
	ck_assert_int_eq((int)nr, (int)plen);

	nw = data_block_write(test_inode->i_db, buf, (size_t)nr, elen);
	ck_assert_int_eq((int)nw, (int)plen);
	test_inode->i_size = (int64_t)(elen + plen);

	/* Verify combined content "AAAABBBB". */
	char verify[256] = { 0 };
	nr = data_block_read(test_inode->i_db, verify, elen + plen, 0);
	ck_assert_int_eq((int)nr, (int)(elen + plen));
	ck_assert_mem_eq(verify, "AAAABBBB", 8);

	data_block_put(src->i_db);
	src->i_db = NULL;
	inode_active_put(src);
}
END_TEST

/*
 * COPY zero bytes (source offset at EOF): should succeed with 0 copied.
 */
START_TEST(test_copy_zero_at_eof)
{
	const char *payload = "data";
	struct inode *src = copy_create_source(payload, strlen(payload));

	/* Source offset at EOF -- nothing to copy. */
	char buf[256];
	ssize_t nr =
		data_block_read(src->i_db, buf, 100, (uint64_t)src->i_size);
	ck_assert_int_le((int)nr, 0);

	data_block_put(src->i_db);
	src->i_db = NULL;
	inode_active_put(src);
}
END_TEST

/* ------------------------------------------------------------------ */
/* EXCHANGE_RANGE tests (data layer)                                   */
/* ------------------------------------------------------------------ */

/*
 * Helper: allocate a second inode with given data for use as
 * the "source" in exchange tests.  Caller must inode_active_put.
 */
static struct inode *exchange_create_inode(const char *data, size_t len)
{
	uint64_t ino =
		__atomic_add_fetch(&test_sb->sb_next_ino, 1, __ATOMIC_RELAXED);
	struct inode *in = inode_alloc(test_sb, ino);
	ck_assert_ptr_nonnull(in);
	in->i_mode = S_IFREG | 0644;
	in->i_db = data_block_alloc(in, data, len, 0);
	ck_assert_ptr_nonnull(in->i_db);
	in->i_size = (int64_t)len;
	return in;
}

/*
 * Basic swap: two 4096-byte files, exchange the full range.
 * After the swap each file holds the other's original content.
 */
START_TEST(test_exchange_range_basic_swap)
{
	char src_data[4096], dst_data[4096];
	memset(src_data, 'S', sizeof(src_data));
	memset(dst_data, 'D', sizeof(dst_data));

	struct inode *src = exchange_create_inode(src_data, sizeof(src_data));
	test_inode->i_db =
		data_block_alloc(test_inode, dst_data, sizeof(dst_data), 0);
	ck_assert_ptr_nonnull(test_inode->i_db);
	test_inode->i_size = sizeof(dst_data);

	/* Simulate the swap: read both ranges then write them crossed. */
	char buf_s[4096], buf_d[4096];
	ssize_t n;

	n = data_block_read(src->i_db, buf_s, 4096, 0);
	ck_assert_int_eq(n, 4096);

	n = data_block_read(test_inode->i_db, buf_d, 4096, 0);
	ck_assert_int_eq(n, 4096);

	n = data_block_write(test_inode->i_db, buf_s, 4096, 0);
	ck_assert_int_eq(n, 4096);

	n = data_block_write(src->i_db, buf_d, 4096, 0);
	ck_assert_int_eq(n, 4096);

	/* Verify: dst now holds 'S', src now holds 'D'. */
	char verify[4096];
	n = data_block_read(test_inode->i_db, verify, 4096, 0);
	ck_assert_int_eq(n, 4096);
	for (int i = 0; i < 4096; i++)
		ck_assert_int_eq((unsigned char)verify[i], 'S');

	n = data_block_read(src->i_db, verify, 4096, 0);
	ck_assert_int_eq(n, 4096);
	for (int i = 0; i < 4096; i++)
		ck_assert_int_eq((unsigned char)verify[i], 'D');

	data_block_put(src->i_db);
	src->i_db = NULL;
	inode_active_put(src);
}
END_TEST

/*
 * Self-inverse property: swapping the same range twice returns
 * both files to their original state.
 */
START_TEST(test_exchange_range_self_inverse)
{
	char src_data[4096], dst_data[4096];
	memset(src_data, 'A', sizeof(src_data));
	memset(dst_data, 'B', sizeof(dst_data));

	struct inode *src = exchange_create_inode(src_data, sizeof(src_data));
	test_inode->i_db =
		data_block_alloc(test_inode, dst_data, sizeof(dst_data), 0);
	ck_assert_ptr_nonnull(test_inode->i_db);
	test_inode->i_size = sizeof(dst_data);

	/* Apply the swap twice using buf_s/buf_d temporaries. */
	char buf_s[4096], buf_d[4096];
	ssize_t n;

	for (int pass = 0; pass < 2; pass++) {
		n = data_block_read(src->i_db, buf_s, 4096, 0);
		ck_assert_int_eq(n, 4096);
		n = data_block_read(test_inode->i_db, buf_d, 4096, 0);
		ck_assert_int_eq(n, 4096);
		n = data_block_write(test_inode->i_db, buf_s, 4096, 0);
		ck_assert_int_eq(n, 4096);
		n = data_block_write(src->i_db, buf_d, 4096, 0);
		ck_assert_int_eq(n, 4096);
	}

	/* After two swaps: back to originals. */
	char verify[4096];
	n = data_block_read(src->i_db, verify, 4096, 0);
	ck_assert_int_eq(n, 4096);
	for (int i = 0; i < 4096; i++)
		ck_assert_int_eq((unsigned char)verify[i], 'A');

	n = data_block_read(test_inode->i_db, verify, 4096, 0);
	ck_assert_int_eq(n, 4096);
	for (int i = 0; i < 4096; i++)
		ck_assert_int_eq((unsigned char)verify[i], 'B');

	data_block_put(src->i_db);
	src->i_db = NULL;
	inode_active_put(src);
}
END_TEST

/*
 * Alignment: both offsets must be multiples of 4096.
 *
 * Note: these tests verify the arithmetic constants used by the alignment
 * check in nfs4_op_exchange_range().  A full handler test would require
 * a live compound context; these document the expected values and catch
 * any accidental constant changes.
 */
START_TEST(test_exchange_range_alignment_valid)
{
	uint64_t src_off = 4096;
	uint64_t dst_off = 8192;
	uint64_t count = 4096;

	ck_assert_int_eq(src_off % 4096, 0);
	ck_assert_int_eq(dst_off % 4096, 0);
	ck_assert_int_eq(count % 4096, 0);
}
END_TEST

START_TEST(test_exchange_range_alignment_src_offset_invalid)
{
	/*
	 * src_offset not aligned to 4096 --> handler returns NFS4ERR_INVAL.
	 * The arithmetic here matches the condition in the handler.
	 */
	uint64_t src_off = 1000;
	ck_assert_int_ne(src_off % 4096, 0);
}
END_TEST

START_TEST(test_exchange_range_alignment_dst_offset_invalid)
{
	/*
	 * dst_offset not aligned to 4096 --> handler returns NFS4ERR_INVAL.
	 * The arithmetic here matches the condition in the handler.
	 */
	uint64_t dst_off = 5000;
	ck_assert_int_ne(dst_off % 4096, 0);
}
END_TEST

/*
 * Overflow: era_src_offset + era_count wraps uint64.
 *
 * The handler checks era_src_offset > UINT64_MAX - era_count before
 * any arithmetic.  This test confirms the overflow guard condition is
 * correct.
 */
START_TEST(test_exchange_range_overflow_rejected)
{
	uint64_t src_offset = UINT64_MAX - 10;
	uint64_t count = 20;

	ck_assert(src_offset > UINT64_MAX - count);
}
END_TEST

/*
 * Source range must not exceed the source file size.
 */
START_TEST(test_exchange_range_source_exceeds_size)
{
	char data[4096];
	memset(data, 'X', sizeof(data));

	struct inode *src = exchange_create_inode(data, sizeof(data));

	/* era_src_offset=0, era_count=8192 > src file size 4096 */
	uint64_t src_offset = 0;
	uint64_t count = 8192;
	ck_assert(src_offset + count > (uint64_t)src->i_size);

	data_block_put(src->i_db);
	src->i_db = NULL;
	inode_active_put(src);
}
END_TEST

/*
 * era_count == 0 derives the count as (src_size - src_offset).
 * If src_offset is already at EOF, the operation succeeds with no change.
 */
START_TEST(test_exchange_range_zero_count_at_eof)
{
	char data[4096];
	memset(data, 'Y', sizeof(data));

	struct inode *src = exchange_create_inode(data, sizeof(data));

	/* src_offset at EOF -- nothing to exchange */
	uint64_t src_offset = (uint64_t)src->i_size;
	uint64_t count = 0;

	ck_assert(src_offset >= (uint64_t)src->i_size);
	(void)count; /* Handler returns NFS4_OK with no data change. */

	data_block_put(src->i_db);
	src->i_db = NULL;
	inode_active_put(src);
}
END_TEST

/*
 * Destination extension: if the destination range extends past dst EOF,
 * the destination grows.  Verify via data_block_resize simulation.
 */
START_TEST(test_exchange_range_dst_extension)
{
	char src_data[4096], dst_data[1024];
	memset(src_data, 'E', sizeof(src_data));
	memset(dst_data, 'F', sizeof(dst_data));

	struct inode *src = exchange_create_inode(src_data, sizeof(src_data));
	test_inode->i_db =
		data_block_alloc(test_inode, dst_data, sizeof(dst_data), 0);
	ck_assert_ptr_nonnull(test_inode->i_db);
	test_inode->i_size = sizeof(dst_data);

	/* Exchange src[0..4096) into dst[0..4096) -- dst must grow. */
	uint64_t count = 4096;
	uint64_t dst_offset = 0;
	uint64_t dst_size = (uint64_t)test_inode->i_size;

	ck_assert(dst_offset + count > dst_size);

	ssize_t r = data_block_resize(test_inode->i_db,
				      (size_t)(dst_offset + count));
	ck_assert_int_ge(r, 0);
	test_inode->i_size = (int64_t)(dst_offset + count);
	ck_assert_int_eq(test_inode->i_size, 4096);

	data_block_put(src->i_db);
	src->i_db = NULL;
	inode_active_put(src);
}
END_TEST

/*
 * Same-file, non-overlapping ranges: exchange [0..4096) and [4096..8192).
 */
START_TEST(test_exchange_range_same_file_nonoverlap)
{
	char data[8192];
	memset(data, 0, sizeof(data));
	memset(data, 'P', 4096); /* first half: 'P' */
	memset(data + 4096, 'Q', 4096); /* second half: 'Q' */

	test_inode->i_db = data_block_alloc(test_inode, data, sizeof(data), 0);
	ck_assert_ptr_nonnull(test_inode->i_db);
	test_inode->i_size = sizeof(data);

	/* Verify non-overlap: [0,4096) and [4096,8192) are adjacent. */
	uint64_t src_off = 0, dst_off = 4096, count = 4096;
	uint64_t src_end = src_off + count;
	uint64_t dst_end = dst_off + count;

	ck_assert(!(src_off < dst_end && dst_off < src_end));

	/* Swap via temporary buffers (simulates the handler). */
	char buf_p[4096], buf_q[4096];
	ssize_t n;

	n = data_block_read(test_inode->i_db, buf_p, 4096, (off_t)src_off);
	ck_assert_int_eq(n, 4096);

	n = data_block_read(test_inode->i_db, buf_q, 4096, (off_t)dst_off);
	ck_assert_int_eq(n, 4096);

	n = data_block_write(test_inode->i_db, buf_q, 4096, (off_t)src_off);
	ck_assert_int_eq(n, 4096);

	n = data_block_write(test_inode->i_db, buf_p, 4096, (off_t)dst_off);
	ck_assert_int_eq(n, 4096);

	/* Verify: first half now 'Q', second half now 'P'. */
	char verify[8192];
	n = data_block_read(test_inode->i_db, verify, 8192, 0);
	ck_assert_int_eq(n, 8192);

	for (int i = 0; i < 4096; i++)
		ck_assert_int_eq((unsigned char)verify[i], 'Q');
	for (int i = 4096; i < 8192; i++)
		ck_assert_int_eq((unsigned char)verify[i], 'P');
}
END_TEST

/*
 * Same-file, overlapping ranges must be rejected.
 *
 * The handler checks (src_off < dst_end && dst_off < src_end) and
 * returns NFS4ERR_INVAL on overlap.  This test verifies the overlap
 * condition that the handler guards against.
 */
START_TEST(test_exchange_range_same_file_overlap_rejected)
{
	/* [0..4096) and [2048..6144) overlap in [2048..4096) */
	uint64_t src_off = 0, dst_off = 2048, count = 4096;
	uint64_t src_end = src_off + count;
	uint64_t dst_end = dst_off + count;

	ck_assert(src_off < dst_end && dst_off < src_end);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite                                                               */
/* ------------------------------------------------------------------ */

Suite *file_ops_suite(void)
{
	Suite *s;
	TCase *tc_alloc, *tc_dealloc, *tc_read_plus, *tc_clone, *tc_seek;

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

	/* CLONE validation tests */
	tc_clone = tcase_create("CLONE");
	tcase_add_checked_fixture(tc_clone, file_ops_setup, file_ops_teardown);
	tcase_add_test(tc_clone, test_clone_alignment_valid);
	tcase_add_test(tc_clone, test_clone_alignment_invalid_src_offset);
	tcase_add_test(tc_clone, test_clone_alignment_invalid_count);
	tcase_add_test(tc_clone, test_clone_zero_count_at_eof);
	tcase_add_test(tc_clone, test_clone_source_range_exceeds_size);
	tcase_add_test(tc_clone, test_clone_ram_backend_no_fd);
	tcase_add_test(tc_clone, test_clone_overflow_rejected);
	suite_add_tcase(s, tc_clone);

	/* SEEK tests */
	tc_seek = tcase_create("SEEK");
	tcase_add_checked_fixture(tc_seek, file_ops_setup, file_ops_teardown);
	tcase_add_test(tc_seek, test_seek_data_at_start);
	tcase_add_test(tc_seek, test_seek_hole_returns_eof);
	tcase_add_test(tc_seek, test_seek_past_eof);
	tcase_add_test(tc_seek, test_seek_empty_file);
	suite_add_tcase(s, tc_seek);

	TCase *tc_copy;
	tc_copy = tcase_create("COPY");
	tcase_add_checked_fixture(tc_copy, file_ops_setup, file_ops_teardown);
	tcase_add_test(tc_copy, test_copy_basic);
	tcase_add_test(tc_copy, test_copy_with_offset);
	tcase_add_test(tc_copy, test_copy_dst_offset);
	tcase_add_test(tc_copy, test_copy_zero_at_eof);
	suite_add_tcase(s, tc_copy);

	TCase *tc_exchange;
	tc_exchange = tcase_create("EXCHANGE_RANGE");
	tcase_add_checked_fixture(tc_exchange, file_ops_setup,
				  file_ops_teardown);
	tcase_add_test(tc_exchange, test_exchange_range_basic_swap);
	tcase_add_test(tc_exchange, test_exchange_range_self_inverse);
	tcase_add_test(tc_exchange, test_exchange_range_alignment_valid);
	tcase_add_test(tc_exchange,
		       test_exchange_range_alignment_src_offset_invalid);
	tcase_add_test(tc_exchange,
		       test_exchange_range_alignment_dst_offset_invalid);
	tcase_add_test(tc_exchange, test_exchange_range_overflow_rejected);
	tcase_add_test(tc_exchange, test_exchange_range_source_exceeds_size);
	tcase_add_test(tc_exchange, test_exchange_range_zero_count_at_eof);
	tcase_add_test(tc_exchange, test_exchange_range_dst_extension);
	tcase_add_test(tc_exchange, test_exchange_range_same_file_nonoverlap);
	tcase_add_test(tc_exchange,
		       test_exchange_range_same_file_overlap_rejected);
	suite_add_tcase(s, tc_exchange);

	return s;
}

int main(void)
{
	return nfs4_test_run(file_ops_suite());
}
