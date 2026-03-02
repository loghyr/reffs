# SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: GPL-2.0+

AM_CFLAGS = -Im4 -I$(top_srcdir)/lib/fs @CHECK_CFLAGS@		\
	$(URCU_CFLAGS) $(URCU_CDS_CFLAGS) $(FUSE_CFLAGS)	\
	$(UUID_CFLAGS)						\
	$(OPENSSL_CFLAGS)					\
	$(ZSTD_CFLAGS)						\
	$(JEMALLOC_FLAGS)					\
	-I$(top_srcdir)/lib/include				\
	-I$(top_builddir)/lib/xdr

AM_LDFLAGS = -rdynamic					\
	$(ASAN_LDFLAGS) $(LSAN_LDFLAGS)			\
	$(UBSAN_LDFLAGS) $(TSAN_LDFLAGS)

lib_LTLIBRARIES = libreffs_fs_test.la

libreffs_fs_test_la_SOURCES =				\
	posix_recovery.c

noinst_HEADERS =					\
	posix_recovery.h				\
	lock_test.h

LDADD = \
	@CHECK_LIBS@					\
	$(top_builddir)/lib/fs/tests/libreffs_fs_test.la	\
	$(top_builddir)/lib/fs/libreffs_fs.la		\
	$(top_builddir)/lib/trace/libreffs_trace.la	\
	$(top_builddir)/lib/utils/libreffs_utils.la	\
	-lxxhash					\
	-lpthread					\
	$(URCU_LIBS) $(URCU_CDS_LIBS) $(FUSE_LIBS)	\
	$(OPENSSL_LIBS)					\
	$(ZSTD_LIBS)					\
	$(UUID_LIBS) $(JEMALLOC_LIBS)

TESTS = \
	fuse \
	inode_hashing \
	inode_hashing_no_check \
	lock_range_1 \
	lock_conflict_2 \
	lock_remove_3 \
	lock_share_4 \
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
	posix_recovery_13

check_PROGRAMS = $(TESTS)
