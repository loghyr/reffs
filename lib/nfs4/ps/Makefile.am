# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

AM_CFLAGS = -Im4 -I.				\
	-I$(top_srcdir)/lib/include		\
	-I$(top_srcdir)/lib/nfs4/include	\
	-I$(top_srcdir)/lib/nfs4/client		\
	-I$(top_srcdir)/lib/nfs4/ps		\
	-I$(top_builddir)/lib/xdr		\
	$(TIRPC_CFLAGS)

noinst_LTLIBRARIES = libreffs_nfs4_ps.la
libreffs_nfs4_ps_la_SOURCES = \
	chunk_io.c \
	ds_io.c \
	ec_pipeline.c \
	ps_discovery.c \
	ps_mount_client.c \
	ps_sb.c \
	ps_state.c

SUBDIRS = tests
