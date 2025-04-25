# SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: GPL-2.0+

AM_CFLAGS = -Im4 @CHECK_CFLAGS@					\
	$(URCU_CFLAGS) $(URCU_CDS_CFLAGS) $(FUSE_CFLAGS)	\
	$(TIRPC_CFLAGS) $(ZLIB_CFLAGS)				\
	-I$(top_srcdir)/lib/include				\
	-I$(top_builddir)/lib/xdr

AM_LDFLAGS = -rdynamic

lib_LTLIBRARIES = libreffs_utils.la
libreffs_utils_la_SOURCES =	\
	cmp.c			\
	identity.c		\
	network.c		\
	rcu.c			\
	server.c		\
	trace.c
