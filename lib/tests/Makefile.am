# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

AM_CFLAGS = -Im4 @CHECK_CFLAGS@					\
	$(URCU_CFLAGS) $(URCU_CDS_CFLAGS) $(FUSE_CFLAGS)	\
	$(TIRPC_CFLAGS) $(ZLIB_CFLAGS) $(HDR_HISTOGRAM_CFLAGS)	\
	$(OPENSSL_CFLAGS)					\
	$(ZSTD_CFLAGS)						\
	-I$(top_srcdir)/lib/include				\
	-I$(top_builddir)/lib/xdr

noinst_LTLIBRARIES = libreffs_test.la
libreffs_test_la_SOURCES = \
	libreffs_test.c \
	libreffs_test.h \
	mini_kdc.c \
	mini_kdc.h

libreffs_test_la_LIBADD = @CHECK_LIBS@
