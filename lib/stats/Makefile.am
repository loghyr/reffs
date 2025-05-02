# SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: GPL-2.0+

AM_CFLAGS = -Im4 -I. \
	-I$(top_srcdir)/lib/include	\
	-I$(top_builddir)/lib/xdr	\
	$(PTHREAD_CFLAGS)		\
	$(URCU_CFLAGS)			\
	$(URCU_CDS_CFLAGS)		\
	$(TIRPC_CFLAGS)			\
	$(FUSE_CFLAGS)

lib_LTLIBRARIES = libreffs_stats.la
libreffs_stats_la_SOURCES = \
	stats1_server.c
