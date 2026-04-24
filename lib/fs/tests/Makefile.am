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
libreffs_fs_test_la_LIBADD =				\
	$(top_builddir)/lib/tests/libreffs_test.la

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

check_PROGRAMS = $(TESTS)
LOG_COMPILER = $(top_srcdir)/scripts/timed-test.sh

# FUSE-backed tests do not build on Darwin: lib/fs/fuse.c is gated
# off (no macFUSE integration yet) and reffs/fuse.h can't provide
# prototypes for fuse_fill_dir_t / fuse_file_info without the
# system <fuse/fuse.h>.  Skip the fuse_* harness tests on Darwin;
# the rest of the fs test-suite runs against the in-process fs API.
if HOST_DARWIN
FUSE_TESTS =
else
FUSE_TESTS = \
	fuse_1 \
	fuse_2 \
	fuse_3 \
	fuse_4 \
	fuse_5 \
	fuse_6 \
	fuse_7 \
	fuse_8 \
	fuse_9
endif

# fs_usage_posix relies on Linux user + mount namespaces
# (CLONE_NEWUSER / CLONE_NEWNS) to isolate per-test mounts and
# exercise size/inode accounting.  Darwin has no equivalent, so
# skip this test there.  fs_usage_ram runs unchanged.
if HOST_DARWIN
FS_USAGE_POSIX_TEST =
else
FS_USAGE_POSIX_TEST = fs_usage_posix
endif

TESTS = \
	$(FUSE_TESTS) \
	inode_hashing \
	inode_hashing_no_check \
	lock_range_1 \
	lock_conflict_2 \
	lock_remove_3 \
	lock_share_4 \
	fs_test_access \
	fs_test_chmod_chown \
	fs_test_create \
	fs_test_evictor \
	fs_test_create_length \
	fs_test_vfs_unchecked_create \
	fs_test_vfs_ctime_capture \
	fs_test_getattr \
	fs_test_lru \
	fs_test_listener_scoped \
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
	posix_recovery_16 \
	posix_force_load \
	fs_usage_ram \
	$(FS_USAGE_POSIX_TEST) \
	layout_segment_test \
	sb_lifecycle_test \
	sb_listener_id_test \
	sb_mount_crossing_test \
	sb_mkdir_p_test \
	sb_path_conflict_test \
	sb_persistence_test \
	sb_security_test \
	client_match_test \
	compose_test

if HAVE_ROCKSDB
TESTS += rocksdb_test
endif

posix_force_load_SOURCES = posix_force_load.c
posix_force_load_LDADD = $(LDADD)
