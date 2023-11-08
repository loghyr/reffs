# SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: GPL-2.0+

ACLOCAL_AMFLAGS = -Im4

AM_CFLAGS = \
	-I../lib/include \
	-I../lib/xdr
	-I$(TIRPC_CFLAGS)

LDADD = \
	../lib/xdr/libreffs_xdr.la \
	$(TIRPC_LIBS)

recho_ev_SOURCES = recho_ev.c
recho_ev_LDADD = $(LDADD)

recho_cl_SOURCES = recho_cl.c
recho_cl_LDADD = $(LDADD)

reffs_SOURCES = reffs.c
reffs_LDADD = $(LDADD)

bin_PROGRAMS = \
	recho_ev \
	recho_cl \
	reffs
