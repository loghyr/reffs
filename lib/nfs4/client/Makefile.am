# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

AM_CFLAGS = -Im4 -I.				\
	-I$(top_srcdir)/lib/include		\
	-I$(top_srcdir)/lib/nfs4/include	\
	-I$(top_srcdir)/lib/nfs4/client		\
	-I$(top_builddir)/lib/xdr		\
	$(OPENSSL_CFLAGS)			\
	$(TIRPC_CFLAGS)

noinst_LTLIBRARIES = libreffs_nfs4_client.la
libreffs_nfs4_client_la_SOURCES = \
	mds_compound.c \
	mds_session.c \
	mds_file.c \
	mds_layout.c \
	mds_tls_xprt.c

SUBDIRS = tests
