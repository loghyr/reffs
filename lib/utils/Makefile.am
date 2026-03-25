# SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

SUBDIRS = tests

AM_CFLAGS = -Im4 @CHECK_CFLAGS@					\
	$(URCU_CFLAGS) $(URCU_CDS_CFLAGS) $(FUSE_CFLAGS)	\
	$(TIRPC_CFLAGS) $(ZLIB_CFLAGS) $(HDR_HISTOGRAM_CFLAGS)	\
	$(OPENSSL_CFLAGS) $(GSSAPI_CFLAGS) $(ZSTD_CFLAGS)	\
	-I$(top_srcdir)/lib/include				\
	-I$(top_builddir)/lib/xdr

AM_LDFLAGS = -rdynamic

lib_LTLIBRARIES = libreffs_utils.la
libreffs_utils_la_SOURCES = \
	cmp.c \
	context.c \
	identity.c \
	log.c \
	network.c \
	rcu.c \
	server.c \
	stack.c	 \
	utf8string.c

libreffs_utils_la_LIBADD = $(top_builddir)/lib/backends/libbackends.la
