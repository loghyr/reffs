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
	ps_local_addr.c \
	ps_mount_client.c \
	ps_owner.c \
	ps_proxy_ops.c \
	ps_renewal.c \
	ps_sb.c \
	ps_security.c \
	ps_state.c \
	ps_write_buffer.c

# ps_sb_alloc.c and ps_shortcircuit.c need lib/fs symbols
# (super_block_alloc, mkdir_p_for_listener, super_block_find,
# inode_find, data_block_*).  Keeping them in their own convenience
# library means ec_demo and other lib/nfs4/ps consumers that do NOT
# link lib/fs still build cleanly -- libreffs_nfs4_ps.la stays
# free of cross-archive references into lib/fs.
#
# The Phase 5 dispatch hook in ec_pipeline.c calls into
# ps_shortcircuit via function pointers stashed on
# ps_listener_state (installed by ps_shortcircuit_install, which
# only reffsd / the PS unit tests invoke).  This indirection is
# what lets the symbol-graph split stand.
libreffs_nfs4_ps_sb_la_SOURCES = \
	ps_sb_alloc.c \
	ps_shortcircuit.c
libreffs_nfs4_ps_sb_la_CFLAGS = $(AM_CFLAGS) $(UUID_CFLAGS)

SUBDIRS = tests
