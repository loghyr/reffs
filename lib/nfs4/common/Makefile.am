# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

AM_CFLAGS = \
	-I$(top_srcdir)/lib/include \
	-I$(top_srcdir)/lib/nfs4/include \
	-I$(top_builddir)/lib/xdr \
	$(TIRPC_CFLAGS)

noinst_LTLIBRARIES = libreffs_nfs4_common.la

libreffs_nfs4_common_la_SOURCES = \
	ffv2_hint.c
