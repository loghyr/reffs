# SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: GPL-2.0+

ACLOCAL_AMFLAGS = -Im4 -I.

AM_CFLAGS = -Wno-unused-variable $(TIRPC_CFLAGS)

GEN_SOURCES = \
	ref_cp_xdr.h \
	ref_cp_xdr.c \
	nfsv42_xdr.h \
	nfsv42_xdr.c \
	mntv3_xdr.h \
	mntv3_xdr.c \
	nfsv3_xdr.h \
	nfsv3_xdr.c

CLEANFILES = $(GEN_SOURCES)

lib_LTLIBRARIES = libreffsxdr.la
libreffsxdr_la_SOURCES = $(GEN_SOURCES)
libreffsxdr_la_LDFLAGS = -rdynamic

ref_cp_xdr.c: ref_cp_xdr.x ref_cp_xdr.h
	rpcgen -C -c ref_cp_xdr.x > $@

ref_cp_xdr.h: ref_cp_xdr.x
	rpcgen -C -h ref_cp_xdr.x > $@

nfsv42_xdr.c: nfsv42_xdr.x nfsv42_xdr.h
	rpcgen -C -c nfsv42_xdr.x > $@

nfsv42_xdr.h: nfsv42_xdr.x
	rpcgen -C -h nfsv42_xdr.x > $@

mntv3_xdr.c: mntv3_xdr.x mntv3_xdr.h
	rpcgen -C -c nfsv42_xdr.x > $@

mntv3_xdr.h: mntv3_xdr.x
	rpcgen -C -h mntv3_xdr.x > $@

nfsv3_xdr.c: nfsv3_xdr.x nfsv3_xdr.h
	rpcgen -C -c nfsv3_xdr.x > $@

nfsv3_xdr.h: nfsv3_xdr.x
	rpcgen -C -h nfsv3_xdr.x > $@
