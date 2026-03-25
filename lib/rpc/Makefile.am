# SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

SUBDIRS = tests

AM_CFLAGS = -Im4 -I$(top_srcdir)/lib/fs @CHECK_CFLAGS@		\
	$(URCU_CFLAGS) $(URCU_CDS_CFLAGS) $(FUSE_CFLAGS)	\
	$(TIRPC_CFLAGS) $(ZLIB_CFLAGS) $(HDR_HISTOGRAM_CFLAGS)	\
	$(OPENSSL_CFLAGS) $(GSSAPI_CFLAGS) $(NFSIDMAP_CFLAGS)	\
	$(ZSTD_CFLAGS)						\
	-I$(top_srcdir)/lib/include				\
	-I$(top_builddir)/lib/xdr

AM_LDFLAGS = -rdynamic

lib_LTLIBRARIES = libreffs_rpc.la
libreffs_rpc_la_SOURCES = \
	gss_context.c \
	rpc.c

libreffs_rpc_la_LIBADD = \
	$(top_builddir)/lib/io/libreffs_io.la \
	$(GSSAPI_LIBS) $(NFSIDMAP_LIBS)
