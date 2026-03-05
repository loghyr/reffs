/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/*
 * fs_test_rename.c — reffs_fs_rename() correctness
 *
 * rename() changes a file's name and/or parent directory.  The critical
 * invariants (POSIX) are:
 *
 *   File identity: ino, uid, gid, size are unchanged.
 *   File timestamps: atime, mtime, ctime are unchanged on the moved entry.
 *   Source parent:   mtime/ctime advance; atime unchanged.
 *   Dest parent:     mtime/ctime advance; atime unchanged.
 *   Old path:        getattr returns -ENOENT.
 *   New path:        entry is visible with same identity.
 *
 * Tests:
 *
 *  Identity / no-op
 *    - rename(x, x) is a successful no-op; the entry still exists
 *
 *  File rename within one directory
 *    - file appears under new name, disappears under old name
 *    - identity (ino/uid/gid/size) and all timestamps are preserved
 *    - parent nlink is unchanged (files carry no ".." link)
 *
 *  File rename across directories — into a subdirectory
 *    - old path gone, new path has same identity and timestamps
 *    - source parent: nlink decrements; mtime/ctime advance; atime unchanged
 *    - dest   parent: nlink increments; mtime/ctime advance; atime unchanged
 *
 *  File rename across directories — out of a subdirectory (reverse)
 *    - same invariants in the opposite direction
 *
 *  File rename clobbers an existing file at the destination
 *    - atomic replacement; /dst gets the source inode and size
 *    - /src is gone; old destination inode is unreachable
 *
 *  Directory rename within one directory
 *    - directory appears under new name, disappears under old name
 *    - parent nlink unchanged (one ".." link moved, not added/removed)
 *    - renamed dir nlink unchanged (still 2 if empty)
 *
 *  Directory rename across directories
 *    - src parent loses one ".." back-link (nlink -1)
 *    - dst parent gains one ".." back-link (nlink +1)
 *    - moved dir nlink unchanged (children move with it)
 *    - non-empty source directory moves with all its children intact
 *    Note: dirent_parent_attach() increments the parent's nlink for
 *    every child entry including regular files, so a directory's nlink
 *    reflects 2 + number_of_children, not 2 + number_of_subdirs.
 *
 *  Destination already exists as a directory
 *    - implementation reparents src *into* that directory and renames it
 *      to nm_dst->nm_name (the dst basename), not the src basename
 *    - e.g. rename("/f", "/dstdir") → /dstdir/dstdir  (not /dstdir/f)
 *
 *  Known limitation (documented in test, not a failing assertion)
 *    - find_matching_directory_entry(LAST_COMPONENT_IS_NEW) silently
 *      succeeds when an intermediate path component is absent, making
 *      rename(x, "/missing/f") a no-op instead of returning -ENOENT
 *
 *  Error cases
 *    - rename("/", anything)            → -EFAULT
 *    - rename(anything, "/")            → -EFAULT
 *    - rename(nonexistent, anything)    → -ENOENT
 *    - rename(x, path/missing_parent)  → -ENOENT
 *    - rename(x, "<dir>/..")           → -ENOTEMPTY
 *
 * Deliberately NOT tested:
 *  - Moving a directory into its own subtree — the implementation has a
 *    "TODO: make sure the paths are not overlapped if dirs" comment and does
 *    not yet detect this; testing it would document a known defect, not a
 *    contract.
 *  - Cross-superblock rename — not supported.
 */

#include "fs_test_harness.h"

uid_t fs_test_uid;
gid_t fs_test_gid;

static void setup(void)
{
	fs_test_setup();
}
static void teardown(void)
{
	fs_test_teardown();
}

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

/*
 * Assert that stat fields that must survive a rename are equal.
 * Timestamps are verified separately by each caller so that the
 * direction of change (unchanged vs. advanced) can be stated precisely.
 */
static void assert_identity_preserved(const struct stat *before,
				      const struct stat *after)
{
	ck_assert_uint_eq(before->st_ino, after->st_ino);
	ck_assert_uint_eq(before->st_uid, after->st_uid);
	ck_assert_uint_eq(before->st_gid, after->st_gid);
	ck_assert_int_eq((int)before->st_size, (int)after->st_size);
}

/* Assert that a path exists (getattr succeeds). */
static void assert_exists(const char *path)
{
	struct stat st;
	ck_assert_int_eq(reffs_fs_getattr(path, &st), 0);
}

/* Assert that a path does NOT exist (getattr returns -ENOENT). */
static void assert_not_exists(const char *path)
{
	struct stat st;
	ck_assert_int_eq(reffs_fs_getattr(path, &st), -ENOENT);
}

/* Return the inode number for a path. */
static ino_t get_ino(const char *path)
{
	struct stat st;
	ck_assert_int_eq(reffs_fs_getattr(path, &st), 0);
	return st.st_ino;
}

/* Return st_nlink for a path. */
static nlink_t get_nlink(const char *path)
{
	struct stat st;
	ck_assert_int_eq(reffs_fs_getattr(path, &st), 0);
	return st.st_nlink;
}

/* ------------------------------------------------------------------ */
/* Identity / no-op                                                     */
/* ------------------------------------------------------------------ */

/*
 * POSIX: "If old and new are the same existing file, rename() shall
 * return successfully and perform no other action."
 */
START_TEST(test_rename_same_path_noop)
{
	ck_assert_int_eq(reffs_fs_create("/f", S_IFREG | 0644), 0);
	ck_assert_int_eq(reffs_fs_rename("/f", "/f"), 0);
	assert_exists("/f");
	ck_assert_int_eq(reffs_fs_unlink("/f"), 0);
}
END_TEST

/* ------------------------------------------------------------------ */
/* File rename within one directory                                     */
/* ------------------------------------------------------------------ */

/*
 * Rename within the same directory: the file's name changes but its
 * parent does not change, nlink is unaffected, and all timestamps on
 * the file itself are preserved.
 */
START_TEST(test_rename_file_same_dir)
{
	struct stat st_pre, st_post, st_parent_before, st_parent_after;

	ck_assert_int_eq(reffs_fs_create("/f", S_IFREG | 0644), 0);
	ck_assert_int_eq(reffs_fs_write("/f", "data", 4, 0), 4);
	ck_assert_int_eq(reffs_fs_getattr("/f", &st_pre), 0);
	ck_assert_int_eq(reffs_fs_getattr("/", &st_parent_before), 0);

	ck_assert_int_eq(reffs_fs_rename("/f", "/g"), 0);

	assert_not_exists("/f");
	ck_assert_int_eq(reffs_fs_getattr("/g", &st_post), 0);
	assert_identity_preserved(&st_pre, &st_post);
	ck_assert_timespec_eq(st_pre.st_atim, st_post.st_atim);
	ck_assert_timespec_eq(st_pre.st_mtim, st_post.st_mtim);
	ck_assert_timespec_eq(st_pre.st_ctim, st_post.st_ctim);

	/* Parent nlink unchanged — files carry no ".." link */
	ck_assert_int_eq(reffs_fs_getattr("/", &st_parent_after), 0);
	ck_assert_uint_eq(st_parent_before.st_nlink, st_parent_after.st_nlink);

	ck_assert_int_eq(reffs_fs_unlink("/g"), 0);
}
END_TEST

/* ------------------------------------------------------------------ */
/* File rename across directories                                       */
/* ------------------------------------------------------------------ */

/*
 * Move a file from the root into a subdirectory.
 *
 * File identity and all three timestamps must be unchanged.
 * Source parent (root): nlink decrements, mtime/ctime advance,
 *                       atime is untouched.
 * Dest   parent (/d):   nlink increments, mtime/ctime advance,
 *                       atime is untouched.
 *
 * Note: because files carry no ".." link the nlink change here is due
 * to the implementation updating parent nlink when reparenting; verify
 * against what the code actually does rather than pure POSIX theory.
 */
START_TEST(test_rename_file_into_subdir)
{
	struct stat st_file_pre, st_file_post;
	struct stat st_src_pre, st_src_post;
	struct stat st_dst_pre, st_dst_post;

	ck_assert_int_eq(reffs_fs_mkdir("/d", 0755), 0);
	ck_assert_int_eq(reffs_fs_create("/f", S_IFREG | 0644), 0);
	ck_assert_int_eq(reffs_fs_write("/f", "hello", 5, 0), 5);

	ck_assert_int_eq(reffs_fs_getattr("/f", &st_file_pre), 0);
	ck_assert_int_eq(reffs_fs_getattr("/", &st_src_pre), 0);
	ck_assert_int_eq(reffs_fs_getattr("/d", &st_dst_pre), 0);

	usleep(1000);
	ck_assert_int_eq(reffs_fs_rename("/f", "/d/f"), 0);

	/* old path gone */
	ck_assert_int_eq(reffs_fs_getattr("/f", &st_file_post), -ENOENT);

	/* new path: file identity and timestamps preserved */
	ck_assert_int_eq(reffs_fs_getattr("/d/f", &st_file_post), 0);
	assert_identity_preserved(&st_file_pre, &st_file_post);
	ck_assert_timespec_eq(st_file_pre.st_atim, st_file_post.st_atim);
	ck_assert_timespec_eq(st_file_pre.st_mtim, st_file_post.st_mtim);
	ck_assert_timespec_eq(st_file_pre.st_ctim, st_file_post.st_ctim);

	/* source parent: nlink unchanged, mtime/ctime advanced, atime unchanged */
	ck_assert_int_eq(reffs_fs_getattr("/", &st_src_post), 0);
	ck_assert_uint_eq(st_src_pre.st_nlink, st_src_post.st_nlink);
	ck_assert_timespec_eq(st_src_pre.st_atim, st_src_post.st_atim);
	ck_assert_timespec_lt(st_src_pre.st_mtim, st_src_post.st_mtim);
	ck_assert_timespec_lt(st_src_pre.st_ctim, st_src_post.st_ctim);

	/* dest parent: nlink unchanged, mtime/ctime advanced, atime unchanged */
	ck_assert_int_eq(reffs_fs_getattr("/d", &st_dst_post), 0);
	ck_assert_uint_eq(st_dst_pre.st_nlink, st_dst_post.st_nlink);
	ck_assert_timespec_eq(st_dst_pre.st_atim, st_dst_post.st_atim);
	ck_assert_timespec_lt(st_dst_pre.st_mtim, st_dst_post.st_mtim);
	ck_assert_timespec_lt(st_dst_pre.st_ctim, st_dst_post.st_ctim);

	ck_assert_int_eq(reffs_fs_unlink("/d/f"), 0);
	ck_assert_int_eq(reffs_fs_rmdir("/d"), 0);
}
END_TEST

/* Move a file back out of a subdirectory to the root — reverse direction. */
START_TEST(test_rename_file_out_of_subdir)
{
	struct stat st_file_pre, st_file_post;
	struct stat st_src_pre, st_src_post;
	struct stat st_dst_pre, st_dst_post;

	ck_assert_int_eq(reffs_fs_mkdir("/d", 0755), 0);
	ck_assert_int_eq(reffs_fs_create("/d/f", S_IFREG | 0644), 0);
	ck_assert_int_eq(reffs_fs_write("/d/f", "hello", 5, 0), 5);

	ck_assert_int_eq(reffs_fs_getattr("/d/f", &st_file_pre), 0);
	ck_assert_int_eq(reffs_fs_getattr("/d", &st_src_pre), 0);
	ck_assert_int_eq(reffs_fs_getattr("/", &st_dst_pre), 0);

	usleep(1000);
	ck_assert_int_eq(reffs_fs_rename("/d/f", "/f"), 0);

	ck_assert_int_eq(reffs_fs_getattr("/d/f", &st_file_post), -ENOENT);

	ck_assert_int_eq(reffs_fs_getattr("/f", &st_file_post), 0);
	assert_identity_preserved(&st_file_pre, &st_file_post);
	ck_assert_timespec_eq(st_file_pre.st_atim, st_file_post.st_atim);
	ck_assert_timespec_eq(st_file_pre.st_mtim, st_file_post.st_mtim);
	ck_assert_timespec_eq(st_file_pre.st_ctim, st_file_post.st_ctim);

	ck_assert_int_eq(reffs_fs_getattr("/d", &st_src_post), 0);
	ck_assert_uint_eq(st_src_pre.st_nlink, st_src_post.st_nlink);
	ck_assert_timespec_eq(st_src_pre.st_atim, st_src_post.st_atim);
	ck_assert_timespec_lt(st_src_pre.st_mtim, st_src_post.st_mtim);
	ck_assert_timespec_lt(st_src_pre.st_ctim, st_src_post.st_ctim);

	ck_assert_int_eq(reffs_fs_getattr("/", &st_dst_post), 0);
	ck_assert_uint_eq(st_dst_pre.st_nlink, st_dst_post.st_nlink);
	ck_assert_timespec_eq(st_dst_pre.st_atim, st_dst_post.st_atim);
	ck_assert_timespec_lt(st_dst_pre.st_mtim, st_dst_post.st_mtim);
	ck_assert_timespec_lt(st_dst_pre.st_ctim, st_dst_post.st_ctim);

	ck_assert_int_eq(reffs_fs_unlink("/f"), 0);
	ck_assert_int_eq(reffs_fs_rmdir("/d"), 0);
}
END_TEST

/* ------------------------------------------------------------------ */
/* File rename clobbers an existing file at the destination             */
/* ------------------------------------------------------------------ */

/*
 * POSIX: "If new names an existing file, it shall be removed and old
 * renamed to new."  The removal is atomic from the caller's perspective.
 * After the rename /dst must carry the source's inode and size; /src
 * must be gone; the old destination inode must be unreachable.
 * If the destination was multiply linked, its ctime must be updated.
 */
START_TEST(test_rename_file_clobbers_existing_file)
{
	struct stat st_src_pre, st_post, st_dst_pre, st_dstlnk_post;

	ck_assert_int_eq(reffs_fs_create("/src", S_IFREG | 0644), 0);
	ck_assert_int_eq(reffs_fs_write("/src", "source", 6, 0), 6);
	ck_assert_int_eq(reffs_fs_getattr("/src", &st_src_pre), 0);

	ck_assert_int_eq(reffs_fs_create("/dst", S_IFREG | 0644), 0);
	ck_assert_int_eq(reffs_fs_write("/dst", "old", 3, 0), 3);
	ck_assert_int_eq(reffs_fs_link("/dst", "/dstlnk"), 0);
	ck_assert_int_eq(reffs_fs_getattr("/dst", &st_dst_pre), 0);
	ck_assert_uint_eq(st_dst_pre.st_nlink, 2);

	/* Sanity: distinct inodes before the rename */
	ck_assert(st_src_pre.st_ino != get_ino("/dst"));

	/* Ensure some time passes for ctime change */
	usleep(100000);

	ck_assert_int_eq(reffs_fs_rename("/src", "/dst"), 0);

	/* /src is gone */
	ck_assert_int_eq(reffs_fs_getattr("/src", &st_post), -ENOENT);

	/* /dst now has the source file's identity and size */
	ck_assert_int_eq(reffs_fs_getattr("/dst", &st_post), 0);
	ck_assert_uint_eq(st_src_pre.st_ino, st_post.st_ino);
	ck_assert_int_eq(st_post.st_size, 6);
	ck_assert_uint_eq(st_post.st_nlink, 1);

	/* /dstlnk still exists, its nlink is now 1, and its ctime was updated */
	ck_assert_int_eq(reffs_fs_getattr("/dstlnk", &st_dstlnk_post), 0);
	ck_assert_uint_eq(st_dstlnk_post.st_ino, st_dst_pre.st_ino);
	ck_assert_uint_eq(st_dstlnk_post.st_nlink, 1);
	ck_assert_timespec_lt(st_dst_pre.st_ctim, st_dstlnk_post.st_ctim);

	ck_assert_int_eq(reffs_fs_unlink("/dst"), 0);
	ck_assert_int_eq(reffs_fs_unlink("/dstlnk"), 0);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Directory rename within one directory                                */
/* ------------------------------------------------------------------ */

/*
 * Renaming a directory within its parent: one ".." link moves with it
 * so the parent's nlink must be unchanged, and the directory itself
 * remains at nlink 2.
 */
START_TEST(test_rename_dir_same_dir)
{
	ino_t dir_ino;
	nlink_t root_nlink_before, root_nlink_after;

	ck_assert_int_eq(reffs_fs_mkdir("/srcd", 0755), 0);
	dir_ino = get_ino("/srcd");
	root_nlink_before = get_nlink("/");

	ck_assert_int_eq(reffs_fs_rename("/srcd", "/dstd"), 0);

	assert_not_exists("/srcd");
	ck_assert_uint_eq(get_ino("/dstd"), dir_ino);

	/* One ".." link moved — parent nlink must be unchanged */
	root_nlink_after = get_nlink("/");
	ck_assert_uint_eq(root_nlink_before, root_nlink_after);

	/* Renamed empty directory retains nlink 2 */
	ck_assert_uint_eq(get_nlink("/dstd"), 2);

	ck_assert_int_eq(reffs_fs_rmdir("/dstd"), 0);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Directory rename across directories — nlink accounting              */
/* ------------------------------------------------------------------ */

/*
 * Moving /srcdir/child → /dstdir/child:
 *   /srcdir loses the ".." back-link → nlink from N to N-1
 *   /dstdir gains the ".." back-link → nlink from M to M+1
 *   child itself retains nlink == 2
 */
START_TEST(test_rename_dir_cross_dir_nlink)
{
	nlink_t src_before, src_after;
	nlink_t dst_before, dst_after;
	ino_t child_ino;

	ck_assert_int_eq(reffs_fs_mkdir("/srcdir", 0755), 0);
	ck_assert_int_eq(reffs_fs_mkdir("/dstdir", 0755), 0);
	ck_assert_int_eq(reffs_fs_mkdir("/srcdir/child", 0755), 0);

	child_ino = get_ino("/srcdir/child");
	src_before = get_nlink("/srcdir");
	dst_before = get_nlink("/dstdir");

	ck_assert_int_eq(reffs_fs_rename("/srcdir/child", "/dstdir/child"), 0);

	assert_not_exists("/srcdir/child");
	assert_exists("/dstdir/child");
	ck_assert_uint_eq(get_ino("/dstdir/child"), child_ino);

	src_after = get_nlink("/srcdir");
	dst_after = get_nlink("/dstdir");

	ck_assert_uint_eq(src_after, src_before - 1);
	ck_assert_uint_eq(dst_after, dst_before + 1);
	ck_assert_uint_eq(get_nlink("/dstdir/child"), 2);

	ck_assert_int_eq(reffs_fs_rmdir("/dstdir/child"), 0);
	ck_assert_int_eq(reffs_fs_rmdir("/dstdir"), 0);
	ck_assert_int_eq(reffs_fs_rmdir("/srcdir"), 0);
}
END_TEST

/* A non-empty directory must move with all its children intact. */
START_TEST(test_rename_nonempty_dir_cross_dir)
{
	ck_assert_int_eq(reffs_fs_mkdir("/srcdir", 0755), 0);
	ck_assert_int_eq(reffs_fs_mkdir("/dstdir", 0755), 0);
	ck_assert_int_eq(reffs_fs_mkdir("/srcdir/sub", 0755), 0);
	ck_assert_int_eq(reffs_fs_create("/srcdir/sub/file", S_IFREG | 0644),
			 0);

	ck_assert_int_eq(reffs_fs_rename("/srcdir/sub", "/dstdir/sub"), 0);

	assert_not_exists("/srcdir/sub");
	assert_exists("/dstdir/sub");
	assert_exists("/dstdir/sub/file");

	/*
	 * sub's nlink is 2: the initial 2 (from mkdir).
	 * dirent_parent_attach correctly no longer increments for regular files.
	 */
	ck_assert_uint_eq(get_nlink("/dstdir/sub"), 2);

	ck_assert_int_eq(reffs_fs_unlink("/dstdir/sub/file"), 0);
	ck_assert_int_eq(reffs_fs_rmdir("/dstdir/sub"), 0);
	ck_assert_int_eq(reffs_fs_rmdir("/dstdir"), 0);
	ck_assert_int_eq(reffs_fs_rmdir("/srcdir"), 0);
}
END_TEST

/*
 * Destination already exists as a directory
 */

/*
 * POSIX: "If the new argument points to an existing directory, it shall
 * fail." (when src is a file). Linux rename(2) specifies -EISDIR.
 */
START_TEST(test_rename_file_into_existing_dir)
{
	ck_assert_int_eq(reffs_fs_mkdir("/dstdir", 0755), 0);
	ck_assert_int_eq(reffs_fs_create("/f", S_IFREG | 0644), 0);

	ck_assert_int_eq(reffs_fs_rename("/f", "/dstdir"), -EISDIR);

	assert_exists("/f");
	assert_exists("/dstdir");

	ck_assert_int_eq(reffs_fs_unlink("/f"), 0);
	ck_assert_int_eq(reffs_fs_rmdir("/dstdir"), 0);
}
END_TEST

/*
 * POSIX: "If new points to an existing directory, old shall also point
 * to an existing directory... the directory new is removed and the
 * directory old is renamed to new."
 */
START_TEST(test_rename_dir_into_existing_dir)
{
	ino_t child_ino;
	nlink_t root_before, root_after;

	ck_assert_int_eq(reffs_fs_mkdir("/dstdir", 0755), 0);
	ck_assert_int_eq(reffs_fs_mkdir("/child", 0755), 0);
	child_ino = get_ino("/child");
	root_before = get_nlink("/");

	ck_assert_int_eq(reffs_fs_rename("/child", "/dstdir"), 0);

	assert_not_exists("/child");
	ck_assert_uint_eq(get_ino("/dstdir"), child_ino);

	/* root lost one link (the 'child' entry) */
	root_after = get_nlink("/");
	ck_assert_uint_eq(root_after, root_before - 1);

	ck_assert_int_eq(reffs_fs_rmdir("/dstdir"), 0);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Error cases                                                          */
/* ------------------------------------------------------------------ */

START_TEST(test_rename_root_src_efault)
{
	ck_assert_int_eq(reffs_fs_rename("/", "/anything"), -EFAULT);
}
END_TEST

START_TEST(test_rename_root_dst_efault)
{
	ck_assert_int_eq(reffs_fs_create("/f", S_IFREG | 0644), 0);
	ck_assert_int_eq(reffs_fs_rename("/f", "/"), -EFAULT);
	ck_assert_int_eq(reffs_fs_unlink("/f"), 0);
}
END_TEST

START_TEST(test_rename_nonexistent_src_enoent)
{
	ck_assert_int_eq(reffs_fs_rename("/no_such_entry", "/dst"), -ENOENT);
}
END_TEST

/*
 * NOT_NOW_BROWN_COW: rename(x, "/missing/f") where "/missing" does not
 * exist ought to return -ENOENT per POSIX.  However,
 * find_matching_directory_entry() with LAST_COMPONENT_IS_NEW breaks out
 * of the walk early when a non-terminal component is absent, leaving
 * nm_dirent pointing at root and nm_name == "f".  The rename therefore
 * succeeds as a same-inode no-op (renames /f to /f).
 *
 * This test documents the current (incorrect) behaviour so that fixing
 * find_matching_directory_entry() will be caught as a test change.
 */
START_TEST(test_rename_dst_parent_missing_enoent)
{
	ck_assert_int_eq(reffs_fs_create("/f", S_IFREG | 0644), 0);
	/* Now correctly returns -ENOENT because find_matching_directory_entry is fixed */
	ck_assert_int_eq(reffs_fs_rename("/f", "/missing/f"), -ENOENT);
	/* /f must still exist */
	assert_exists("/f");
	ck_assert_int_eq(reffs_fs_unlink("/f"), 0);
}
END_TEST

/*
 * rename(x, "<dir>/..") — the implementation explicitly returns
 * -ENOTEMPTY when nm_dst->nm_name is "..".
 */
START_TEST(test_rename_dst_dotdot_enotempty)
{
	ck_assert_int_eq(reffs_fs_mkdir("/d", 0755), 0);
	ck_assert_int_eq(reffs_fs_create("/f", S_IFREG | 0644), 0);
	ck_assert_int_eq(reffs_fs_rename("/f", "/d/.."), -ENOTEMPTY);
	ck_assert_int_eq(reffs_fs_unlink("/f"), 0);
	ck_assert_int_eq(reffs_fs_rmdir("/d"), 0);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite                                                                */
/* ------------------------------------------------------------------ */

Suite *fs_rename_suite(void)
{
	Suite *s = suite_create("fs: rename");
	TCase *tc = tcase_create("Core");

	tcase_add_checked_fixture(tc, setup, teardown);

	/* Identity */
	tcase_add_test(tc, test_rename_same_path_noop);

	/* File renames */
	tcase_add_test(tc, test_rename_file_same_dir);
	tcase_add_test(tc, test_rename_file_into_subdir);
	tcase_add_test(tc, test_rename_file_out_of_subdir);
	tcase_add_test(tc, test_rename_file_clobbers_existing_file);

	/* Directory renames */
	tcase_add_test(tc, test_rename_dir_same_dir);
	tcase_add_test(tc, test_rename_dir_cross_dir_nlink);
	tcase_add_test(tc, test_rename_nonempty_dir_cross_dir);

	/* rename where dst already exists as a directory */
	tcase_add_test(tc, test_rename_file_into_existing_dir);
	tcase_add_test(tc, test_rename_dir_into_existing_dir);

	/* Error cases */
	tcase_add_test(tc, test_rename_root_src_efault);
	tcase_add_test(tc, test_rename_root_dst_efault);
	tcase_add_test(tc, test_rename_nonexistent_src_enoent);
	tcase_add_test(tc, test_rename_dst_parent_missing_enoent);
	tcase_add_test(tc, test_rename_dst_dotdot_enotempty);

	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int failed;
	fs_test_global_init();
	SRunner *sr = srunner_create(fs_rename_suite());
	srunner_set_fork_status(sr, CK_NOFORK);
	srunner_run_all(sr, CK_NORMAL);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	fs_test_global_fini();
	return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
