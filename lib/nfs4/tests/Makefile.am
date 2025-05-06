# SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: GPL-2.0+

AM_CFLAGS = -Im4 -I$(top_srcdir)/lib/nfs4 @CHECK_CFLAGS@	\
	$(URCU_CFLAGS) $(URCU_CDS_CFLAGS) $(FUSE_CFLAGS)	\
	$(TIRPC_CFLAGS) $(ZLIB_CFLAGS) $(UUID_CFLAGS)		\
	$(JEMALLOC_CFLAGS) $(HDR_HISTOGRAM_CFLAGS)		\
	-I$(top_srcdir)/lib/include				\
	-I$(top_builddir)/lib/xdr

LDADD = \
	$(top_builddir)/lib/io/libreffs_io.la		\
	$(top_builddir)/lib/nfs4/libreffs_nfs4.la	\
	$(top_builddir)/lib/utils/libreffs_utils.la	\
	@CHECK_LIBS@ $(TIRPC_LIBS)			\
	-lxxhash -lpthread				\
	$(URCU_LIBS) $(URCU_CDS_LIBS) $(FUSE_LIBS)	\
	$(TIRPC_LIBS) $(ZLIB_LIBS) $(UUID_LIBS)		\
	$(HDR_HISTOGRAM_LIBS) $(JEMALLOC_LIBS)

check_PROGRAMS = $(TESTS)

TESTS = \
	errors

