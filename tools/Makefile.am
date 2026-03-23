# SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

AM_CFLAGS = \
	-I$(top_srcdir)/lib/include		\
	-I$(top_srcdir)/lib/nfs4/client		\
	-I$(top_builddir)/lib/xdr		\
	$(TIRPC_CFLAGS)

ec_demo_SOURCES = ec_demo.c
ec_demo_LDADD = \
	$(top_builddir)/lib/nfs4/client/libreffs_nfs4_client.la	\
	$(top_builddir)/lib/ec/libreffs_ec.la			\
	$(top_builddir)/lib/xdr/libreffs_xdr.la			\
	$(TIRPC_LIBS)

bin_PROGRAMS = ec_demo
