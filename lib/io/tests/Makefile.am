# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

AM_CFLAGS = -Im4 @CHECK_CFLAGS@				\
	$(URCU_CFLAGS) $(URCU_CDS_CFLAGS)			\
	$(TIRPC_CFLAGS) $(OPENSSL_CFLAGS) $(ZSTD_CFLAGS)	\
	$(UUID_CFLAGS) $(JEMALLOC_CFLAGS)			\
	$(HDR_HISTOGRAM_CFLAGS) $(URING_CFLAGS)			\
	-I$(top_srcdir)/lib/include				\
	-I$(top_srcdir)/lib/tests				\
	-I$(top_builddir)/lib/xdr

AM_LDFLAGS = -rdynamic					\
	$(ASAN_LDFLAGS) $(LSAN_LDFLAGS)			\
	$(UBSAN_LDFLAGS) $(TSAN_LDFLAGS)

LDADD = \
	@CHECK_LIBS@					\
	$(top_builddir)/lib/tests/libreffs_test.la	\
	$(top_builddir)/lib/io/libreffs_io.la		\
	$(top_builddir)/lib/utils/libreffs_utils.la	\
	$(top_builddir)/lib/backends/libbackends.la	\
	$(top_builddir)/lib/fs/libreffs_fs.la		\
	$(top_builddir)/lib/trace/libreffs_trace.la	\
	$(top_builddir)/lib/rpc/libreffs_rpc.la		\
	$(top_builddir)/lib/xdr/libreffs_xdr.la		\
	-lxxhash					\
	-lpthread					\
	$(URCU_LIBS) $(URCU_CDS_LIBS)			\
	$(TIRPC_LIBS) $(OPENSSL_LIBS)			\
	$(URING_LIBS) $(ZSTD_LIBS) $(UUID_LIBS)		\
	$(FUSE_LIBS)					\
	$(HDR_HISTOGRAM_LIBS) $(JEMALLOC_LIBS)

check_PROGRAMS = $(TESTS)
LOG_COMPILER = $(top_srcdir)/scripts/timed-test.sh

TESTS = \
	tls_test \
	tls_write_count_test
