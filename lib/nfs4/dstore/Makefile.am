# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

SUBDIRS = tests

AM_CFLAGS = -I$(top_srcdir)/lib/include \
	$(TIRPC_CFLAGS) \
	-I$(top_builddir)/lib/xdr

noinst_LTLIBRARIES = libreffs_dstore.la
libreffs_dstore_la_SOURCES = dstore.c dstore_fanout.c dstore_ops_nfsv3.c dstore_ops_local.c dstore_wcc.c runway.c
