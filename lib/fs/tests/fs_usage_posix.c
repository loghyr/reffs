/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#define _GNU_SOURCE
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <fcntl.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdint.h>
#include <check.h>
#include <stdbool.h>
#include <errno.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

#include "posix_recovery.h"
#include "reffs/fs.h"
#include "reffs/test.h"
#include "reffs/super_block.h"
#include "reffs/inode.h"
#include "fs_test_harness.h"
#include <urcu/call-rcu.h>

/* Required by fs_test_harness.h (declared extern there) */

/* --------------------------------------------------------------------------
 * User namespace helpers
 * -------------------------------------------------------------------------- */

/*
 * write_map / deny_setgroups: helpers for user namespace uid/gid setup.
 *
 * The real uid/gid MUST be captured before entering the new user namespace
 * (before clone()), because inside the new namespace getuid() returns 65534
 * until the maps are written.  Pass them in explicitly via mount_child_args.
 */
static int write_map(const char *path, uid_t inner, uid_t outer)
{
	char buf[64];
	int fd, n, ret = 0;

	fd = open(path, O_WRONLY);
	if (fd < 0)
		return -errno;
	n = snprintf(buf, sizeof(buf), "%u %u 1", inner, outer);
	if (write(fd, buf, n) != n)
		ret = -errno;
	close(fd);
	return ret;
}

static int deny_setgroups(void)
{
	int fd, ret = 0;

	fd = open("/proc/self/setgroups", O_WRONLY);
	if (fd < 0)
		return -errno;
	if (write(fd, "deny", 4) != 4)
		ret = -errno;
	close(fd);
	return ret;
}

/*
 * mount_child_fn / mount_child_args
 *
 * Called by the clone()d child which is already in new user+mount namespaces.
 * Writes uid/gid maps, mounts a size-limited tmpfs, signals the parent, then
 * blocks until the parent signals done.
 *
 * We use clone(CLONE_NEWUSER|CLONE_NEWNS) rather than fork() because fork()
 * triggers pthread_atfork() handlers -- specifically call_rcu_after_fork_child,
 * which creates a worker thread.  A process with >1 threads cannot call
 * unshare(CLONE_NEWUSER) (EINVAL).  clone() does not fire atfork handlers,
 * so the child is always single-threaded when it sets up its namespaces.
 */
struct mount_child_args {
	const char *mountpoint;
	size_t size_mb;
	uid_t real_uid; /* captured in parent before clone() */
	gid_t real_gid;
	int sync_fd; /* child writes "R" when mount is ready */
	int done_fd; /* child reads; EOF = parent is done */
	int done_pipe_write_fd; /* write end: must be closed in child */
};

static int mount_child_fn(void *arg)
{
	struct mount_child_args *a = arg;
	char options[64];
	int ret;

	/*
	 * Close the write end of done_pipe in the child.  The child inherited
	 * it from the parent via clone(), so without this close() the child
	 * holds done_pipe[1] open itself -- its own read(done_fd) would never
	 * get EOF even after the parent closes its copy.
	 */
	close(a->done_pipe_write_fd);

	ret = write_map("/proc/self/uid_map", 0, a->real_uid);
	if (ret < 0)
		goto fail;
	ret = deny_setgroups();
	if (ret < 0)
		goto fail;
	ret = write_map("/proc/self/gid_map", 0, a->real_gid);
	if (ret < 0)
		goto fail;

	if (mkdir(a->mountpoint, 0755) < 0 && errno != EEXIST)
		goto fail;

	snprintf(options, sizeof(options), "size=%zuM", a->size_mb);
	if (mount("tmpfs", a->mountpoint, "tmpfs", 0, options) < 0)
		goto fail;

	if (write(a->sync_fd, "R", 1) != 1)
		goto fail;
	close(a->sync_fd);

	{
		char buf[1];
		read(a->done_fd, buf, 1);
	}
	close(a->done_fd);
	return 0;

fail:
	close(a->sync_fd);
	close(a->done_fd);
	return 1;
}

/* --------------------------------------------------------------------------
 * Tests
 * -------------------------------------------------------------------------- */

START_TEST(test_fs_usage_posix_basic)
{
	struct test_context ctx;
	struct reffs_fs_usage_stats stats;
	const char *file1 = "/file1";
	char buf[1024];

	ck_assert_int_eq(test_setup(&ctx), 0);

	/* Check initial usage: root directory exists */
	ck_assert_int_eq(reffs_fs_usage(&stats), 0);
	ck_assert_uint_eq(stats.used_bytes, 0);
	ck_assert_uint_eq(stats.used_files, 1); /* Root directory */

	/* Create and write to file1 */
	ck_assert_int_eq(reffs_fs_create(file1, 0644), 0);
	memset(buf, 'A', 512);
	ck_assert_int_eq(reffs_fs_write(file1, buf, 512, 0), 512);

	/* Check usage after file1 */
	ck_assert_int_eq(reffs_fs_usage(&stats), 0);
	ck_assert_uint_eq(stats.used_bytes, 512);
	ck_assert_uint_eq(stats.used_files, 2); /* Root + file1 */

	/* Delete file1 */
	ck_assert_int_eq(reffs_fs_unlink(file1), 0);

	/* Check usage after unlink */
	ck_assert_int_eq(reffs_fs_usage(&stats), 0);
	ck_assert_uint_eq(stats.used_bytes, 0);
	ck_assert_uint_eq(stats.used_files, 1);

	test_teardown(&ctx);
}
END_TEST

START_TEST(test_fs_usage_posix_tmpfs)
{
	char tmp_mount[] = "/tmp/reffs_tmpfs_XXXXXX";
	char proc_mount[256];
	struct reffs_fs_usage_stats stats;
	struct super_block *sb;
	struct statvfs sv;
	pid_t mount_pid;
	int sync_pipe[2];
	int done_pipe[2];
	static char child_stack[65536];

	if (!mkdtemp(tmp_mount))
		ck_abort_msg("Failed to create temp mount point");

	if (pipe(sync_pipe) < 0 || pipe(done_pipe) < 0) {
		rmdir(tmp_mount);
		ck_abort_msg("pipe failed: %m");
	}

	/*
	 * Spawn mount child via clone(CLONE_NEWUSER|CLONE_NEWNS).
	 * Real uid/gid captured here (parent) and passed via args because
	 * inside the new user namespace getuid()/getgid() return 65534.
	 */
	struct mount_child_args args = {
		.mountpoint = tmp_mount,
		.size_mb = 10,
		.real_uid = getuid(),
		.real_gid = getgid(),
		.sync_fd = sync_pipe[1],
		.done_fd = done_pipe[0],
		.done_pipe_write_fd = done_pipe[1],
	};
	mount_pid = clone(mount_child_fn, child_stack + sizeof(child_stack),
			  CLONE_NEWUSER | CLONE_NEWNS | SIGCHLD, &args);
	if (mount_pid < 0) {
		close(sync_pipe[0]);
		close(sync_pipe[1]);
		close(done_pipe[0]);
		close(done_pipe[1]);
		rmdir(tmp_mount);
		ck_abort_msg("clone failed: %m");
	}

	close(sync_pipe[1]);
	close(done_pipe[0]);

	/* Wait for mount child to signal ready. */
	{
		char buf[1];
		ssize_t n = read(sync_pipe[0], buf, 1);
		if (n != 1 || buf[0] != 'R') {
			close(sync_pipe[0]);
			close(done_pipe[1]);
			waitpid(mount_pid, NULL, 0);
			rmdir(tmp_mount);
			ck_abort_msg("mount child failed to mount tmpfs");
		}
		close(sync_pipe[0]);
	}

	/*
	 * The tmpfs is in the child's private mount namespace.
	 * Access via /proc/<pid>/root/<path> -- Linux exposes each process's
	 * mount namespace root through procfs, readable without joining it.
	 */
	snprintf(proc_mount, sizeof(proc_mount), "/proc/%d/root%s", mount_pid,
		 tmp_mount);

	/*
	 * Explicitly restart the call_rcu worker thread.  pthread_atfork
	 * should have done this already when libcheck forked the test child,
	 * but call it again to be certain -- it is idempotent if the worker
	 * is already running.
	 */
	call_rcu_after_fork_child();

	fs_test_setup();

	/*
	 * Create a POSIX superblock backed by the tmpfs.
	 *
	 * reffs_fs_usage() aggregates all superblocks.  fs_test_setup()
	 * created sb id=1 (RAM backend) which contributes:
	 *   sb_bytes_max  = SIZE_MAX
	 *   sb_inodes_max = SIZE_MAX
	 *   sb_bytes_used = 0  (no data written)
	 *   sb_inodes_used = 1 (root inode)
	 *
	 * sb id=10 (POSIX/tmpfs) contributes:
	 *   sb_bytes_max  = f_blocks * f_frsize  (from statvfs on mount)
	 *   sb_inodes_max = f_files
	 *   sb_bytes_used = 0
	 *   sb_inodes_used = 1 (root dirent)
	 *
	 * uint64 arithmetic: SIZE_MAX + N == N - 1 (wraps by 1).
	 */
	sb = super_block_alloc(10, "/mnt/tmpfs", REFFS_STORAGE_POSIX,
			       proc_mount);
	ck_assert(sb != NULL);
	super_block_dirent_create(sb, NULL, reffs_life_action_birth);

	ck_assert_int_eq(statvfs(proc_mount, &sv), 0);
	ck_assert_int_eq(reffs_fs_usage(&stats), 0);

	TRACE("reffs says total_bytes = %" PRIu64 ", used_bytes = %" PRIu64
	      ", free_bytes = %" PRIu64 ", total_files = %" PRIu64
	      ", used_files = %" PRIu64 ", free_files = %" PRIu64,
	      stats.total_bytes, stats.used_bytes, stats.free_bytes,
	      stats.total_files, stats.used_files, stats.free_files);
	TRACE("sb has %zu", sb->sb_bytes_max);

	/* total_bytes: SIZE_MAX + (f_blocks*f_frsize) wraps to (f_blocks*f_frsize - 1) */
	ck_assert_uint_eq(stats.total_bytes,
			  (uint64_t)sv.f_blocks * sv.f_frsize - 1);
	/* total_files: SIZE_MAX + f_files wraps to (f_files - 1) */
	ck_assert_uint_eq(stats.total_files, (uint64_t)sv.f_files - 1);
	/* used_bytes: 0 (sb1) + 0 (sb10) */
	ck_assert_uint_eq(stats.used_bytes, 0);
	/* used_files: 1 root (sb1) + 1 root (sb10) */
	ck_assert_uint_eq(stats.used_files, 2);

#ifdef NOT_NOW_BROWN_COW
	super_block_dirent_release(sb, reffs_life_action_death);
	super_block_put(sb);
	rcu_barrier();
#endif

	fs_test_teardown();

	/* Release mount child; its namespace and tmpfs vanish on exit. */
	close(done_pipe[1]);
	waitpid(mount_pid, NULL, 0);
	rmdir(tmp_mount);
}
END_TEST

/* --------------------------------------------------------------------------
 * Suites and main
 * -------------------------------------------------------------------------- */

Suite *fs_usage_posix_suite(void)
{
	Suite *s = suite_create("fs: usage (POSIX)");
	TCase *tc = tcase_create("Core");
	tcase_add_test(tc, test_fs_usage_posix_basic);
	suite_add_tcase(s, tc);
	return s;
}

/*
 * The tmpfs test uses clone(CLONE_NEWUSER|CLONE_NEWNS) to create the mount
 * child, bypassing pthread_atfork() handlers.  The libcheck CK_FORK child
 * (the test process) never calls unshare(), so call_rcu / rcu_barrier work
 * normally for teardown.
 */
Suite *fs_usage_posix_ns_suite(void)
{
	Suite *s = suite_create("fs: usage (POSIX, namespace)");
	TCase *tc = tcase_create("Namespace");
	tcase_add_test(tc, test_fs_usage_posix_tmpfs);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	return fs_test_run(fs_usage_posix_suite());
}
