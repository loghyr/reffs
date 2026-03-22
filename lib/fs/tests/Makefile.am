# SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

AM_CFLAGS = -Im4 -I$(top_srcdir)/lib/fs @CHECK_CFLAGS@		\
	$(URCU_CFLAGS) $(URCU_CDS_CFLAGS) $(FUSE_CFLAGS)	\
	$(UUID_CFLAGS)						\
	$(TIRPC_CFLAGS)						\
	$(OPENSSL_CFLAGS)					\
	$(ZSTD_CFLAGS)						\
	$(JEMALLOC_FLAGS)					\
	-I$(top_srcdir)/lib/include				\
	-I$(top_srcdir)/lib/tests				\
	-I$(top_builddir)/lib/xdr

AM_LDFLAGS = -rdynamic					\
	$(ASAN_LDFLAGS) $(LSAN_LDFLAGS)			\
	$(UBSAN_LDFLAGS) $(TSAN_LDFLAGS)

lib_LTLIBRARIES = libreffs_fs_test.la

libreffs_fs_test_la_SOURCES =				\
	posix_recovery.c \
	fs_test_harness.c

noinst_HEADERS = \
	fuse_harness.h \
	lock_test.h \
	fs_test_harness.h \
	posix_recovery.h

LDADD = \
	@CHECK_LIBS@					\
	$(top_builddir)/lib/tests/libreffs_test.la	\
	$(top_builddir)/lib/fs/tests/libreffs_fs_test.la	\
	$(top_builddir)/lib/fs/libreffs_fs.la		\
	$(top_builddir)/lib/rpc/libreffs_rpc.la		\
	$(top_builddir)/lib/io/libreffs_io.la		\
	$(top_builddir)/lib/utils/libreffs_utils.la	\
	$(top_builddir)/lib/trace/libreffs_trace.la	\
	$(top_builddir)/lib/backends/libbackends.la	\
	$(top_builddir)/lib/xdr/libreffs_xdr.la		\
	-lxxhash					\
	-lpthread					\
	$(URCU_LIBS) $(URCU_CDS_LIBS) $(FUSE_LIBS)	\
	$(TIRPC_LIBS)					\
	$(OPENSSL_LIBS)					\
	$(URING_LIBS)					\
	$(ZSTD_LIBS)					\
	$(UUID_LIBS) $(JEMALLOC_LIBS)

TESTS = \
	fuse_1 \
	fuse_2 \
	fuse_3 \
	fuse_4 \
	fuse_5 \
	fuse_6 \
	fuse_7 \
	fuse_8 \
	fuse_9 \
	inode_hashing \
	inode_hashing_no_check \
	lock_range_1 \
	lock_conflict_2 \
	lock_remove_3 \
	lock_share_4 \
	fs_test_access \
	fs_test_chmod_chown \
	fs_test_create \
	fs_test_create_length \
	fs_test_vfs_unchecked_create \
	fs_test_vfs_ctime_capture \
	fs_test_getattr \
	fs_test_lru \
	fs_test_mkdir \
	fs_test_mkdir_timestamps \
	fs_test_nlink_semantics \
	fs_test_ns_teardown \
	fs_test_permissions \
	fs_test_rename \
	fs_test_setattr_permissions \
	fs_test_unlink \
	fs_test_unlink_open \
	fs_test_utimensat \
	test_rmdir_leak \
	fs_test_write_read \
	fs_test_owner_write \
	dirent_lifecycle \
	vfs_integrity \
	posix_recovery_1 \
	posix_recovery_2 \
	posix_recovery_3 \
	posix_recovery_4 \
	posix_recovery_5 \
	posix_recovery_6 \
	posix_recovery_7 \
	posix_recovery_8 \
	posix_recovery_9 \
	posix_recovery_10 \
	posix_recovery_11 \
	posix_recovery_12 \
	posix_recovery_13 \
	posix_recovery_14 \
	posix_recovery_15 \
	posix_force_load \
	fs_usage_ram \
	fs_usage_posix \
	layout_segment_test

posix_force_load_SOURCES = posix_force_load.c
posix_force_load_LDADD = $(LDADD)

check_PROGRAMS = $(TESTS)
