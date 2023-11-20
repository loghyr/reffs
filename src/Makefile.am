# SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: GPL-2.0+

ACLOCAL_AMFLAGS = -Im4 

AM_CFLAGS = \
	-I$(top_srcdir)/lib/include \
	-I$(top_srcdir)/lib/xdr
	-I$(TIRPC_CFLAGS)

LDADD = \
	$(top_srcdir)/lib/xdr/libreffs_xdr.la \
	$(top_srcdir)/lib/fs/libreffs_fs.la \
	$(top_srcdir)/lib/utils/libreffs_utils.la \
	$(TIRPC_LIBS)

recho_ev_SOURCES = recho_ev.c
recho_ev_LDADD = $(LDADD)

recho_cl_SOURCES = recho_cl.c
recho_cl_LDADD = $(LDADD)

reffs_SOURCES = reffs.c
reffs_LDADD = $(LDADD)

reffs_fuse_SOURCES = reffs_fuse.c
reffs_fuse_LDADD = $(LDADD)

bin_PROGRAMS = \
	recho_cl \
	recho_ev \
	reffs_fuse \
	reffs
