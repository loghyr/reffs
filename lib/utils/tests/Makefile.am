# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

AM_CFLAGS = -Im4 @CHECK_CFLAGS@					\
	$(URCU_CFLAGS) $(URCU_CDS_CFLAGS) $(FUSE_CFLAGS)	\
	$(TIRPC_CFLAGS) $(ZLIB_CFLAGS) $(UUID_CFLAGS)		\
	$(JEMALLOC_CFLAGS) $(HDR_HISTOGRAM_CFLAGS)		\
	$(OPENSSL_CFLAGS)					\
	$(URING_CFLAGS)						\
	$(ZSTD_CFLAGS)						\
	-I$(top_srcdir)/lib/include				\
	-I$(top_srcdir)/lib/tests				\
	-I$(top_builddir)/lib/xdr

LDADD = \
	@CHECK_LIBS@					\
	$(top_builddir)/lib/tests/libreffs_test.la	\
	$(top_builddir)/lib/utils/libreffs_utils.la	\
	$(top_builddir)/lib/backends/libbackends.la	\
	$(top_builddir)/lib/fs/libreffs_fs.la		\
	$(top_builddir)/lib/trace/libreffs_trace.la	\
	$(top_builddir)/lib/xdr/libreffs_xdr.la		\
	-lxxhash					\
	-lpthread					\
	$(URCU_LIBS) $(URCU_CDS_LIBS) $(FUSE_LIBS)	\
	$(TIRPC_LIBS) $(ZLIB_LIBS) $(UUID_LIBS)		\
	$(OPENSSL_LIBS)					\
	$(URING_LIBS)					\
	$(ZSTD_LIBS)					\
	$(HDR_HISTOGRAM_LIBS) $(JEMALLOC_LIBS)

check_PROGRAMS = $(TESTS)

TESTS = \
	utf8string_component \
	utf8string_lifecycle \
	utf8string_compare \
	utf8string_id
