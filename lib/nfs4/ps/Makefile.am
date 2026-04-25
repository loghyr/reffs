# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

AM_CFLAGS = -Im4 -I.				\
	-I$(top_srcdir)/lib/include		\
	-I$(top_srcdir)/lib/nfs4/include	\
	-I$(top_srcdir)/lib/nfs4/client		\
	-I$(top_srcdir)/lib/nfs4/ps		\
	-I$(top_builddir)/lib/xdr		\
	$(TIRPC_CFLAGS)

noinst_LTLIBRARIES = libreffs_nfs4_ps.la libreffs_nfs4_ps_sb.la
libreffs_nfs4_ps_la_SOURCES = \
	chunk_io.c \
	ds_io.c \
	ec_pipeline.c \
	ps_discovery.c \
	ps_inode.c \
	ps_mount_client.c \
	ps_owner.c \
	ps_proxy_ops.c \
	ps_sb.c \
	ps_state.c

# ps_sb_alloc.c needs lib/fs symbols (super_block_alloc, mkdir_p_for_listener,
# etc.) that the other PS unit tests deliberately do NOT link.  Pulling it
# into libreffs_nfs4_ps.la would force every PS test to pull in the full
# fs + urcu + uuid dep graph.  Keep it in its own convenience library so
# only reffsd (and the dedicated ps_sb_alloc_test) pay that cost.
libreffs_nfs4_ps_sb_la_SOURCES = \
	ps_sb_alloc.c
libreffs_nfs4_ps_sb_la_CFLAGS = $(AM_CFLAGS) $(UUID_CFLAGS)

SUBDIRS = tests
