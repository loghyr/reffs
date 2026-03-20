# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

AM_CFLAGS = @CHECK_CFLAGS@				\
	-I$(top_srcdir)/lib/include			\
	-I$(top_srcdir)/lib/tests			\
	-I$(top_builddir)/lib/xdr			\
	-I$(top_srcdir)/third_party/tomlc99		\
	$(TIRPC_CFLAGS)

LDADD = \
	@CHECK_LIBS@						\
	$(top_builddir)/lib/tests/libreffs_test.la		\
	$(top_builddir)/lib/config/libreffs_config.la		\
	$(top_builddir)/lib/utils/libreffs_utils.la		\
	$(top_builddir)/lib/trace/libreffs_trace.la		\
	$(top_builddir)/lib/fs/libreffs_fs.la			\
	$(top_builddir)/lib/backends/libbackends.la		\
	$(top_builddir)/lib/xdr/libreffs_xdr.la			\
	$(top_builddir)/third_party/tomlc99/libtomlc99.la	\
	$(URCU_LIBS) $(URCU_CDS_LIBS) $(FUSE_LIBS)		\
	$(TIRPC_LIBS)						\
	$(ZLIB_LIBS)						\
	$(UUID_LIBS)						\
	$(URING_LIBS)						\
	$(OPENSSL_LIBS)						\
	$(HDR_HISTOGRAM_LIBS)					\
	$(ZSTD_LIBS)						\
	$(JEMALLOC_LIBS)					\
	-lxxhash						\
	-lpthread

check_PROGRAMS = $(TESTS)

TESTS = config_test
