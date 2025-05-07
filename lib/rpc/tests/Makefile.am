# SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: GPL-2.0+

AM_CFLAGS = -Im4 -I$(top_srcdir)/lib/fs @CHECK_CFLAGS@		\
	$(URCU_CFLAGS) $(URCU_CDS_CFLAGS) $(FUSE_CFLAGS)	\
	$(TIRPC_CFLAGS) $(ZLIB_CFLAGS) $(UUID_CFLAGS)		\
	$(JEMALLOC_CFLAGS) $(HDR_HISTOGRAM_CFLAGS)		\
	$(OPENSSL_CFLAGS)					\
	$(URING_CFLAGS)						\
	-I$(top_srcdir)/lib/include				\
	-I$(top_builddir)/lib/xdr

LDADD = \
	@CHECK_LIBS@					\
	$(top_builddir)/lib/trace/libreffs_trace.la	\
	$(top_builddir)/lib/xdr/libreffs_xdr.la		\
	$(top_builddir)/lib/rpc/libreffs_rpc.la		\
	$(top_builddir)/lib/io/libreffs_io.la		\
	$(top_builddir)/lib/fs/libreffs_fs.la		\
	$(top_builddir)/lib/utils/libreffs_utils.la	\
	-lxxhash					\
	-lpthread					\
	$(URCU_LIBS) $(URCU_CDS_LIBS) $(FUSE_LIBS)	\
	$(TIRPC_LIBS) $(ZLIB_LIBS) $(UUID_LIBS)		\
	$(OPENSSL_LIBS)					\
	$(URING_LIBS)					\
	$(HDR_HISTOGRAM_LIBS) $(JEMALLOC_LIBS)

check_PROGRAMS = $(TESTS)

TESTS =				\
	rpc_call
