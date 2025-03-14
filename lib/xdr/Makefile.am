# SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: GPL-2.0+

ACLOCAL_AMFLAGS = -Im4 -I.

AM_CFLAGS = $(TIRPC_CFLAGS)

GEN_SOURCES = \
	ref_cp_xdr.h \
	ref_cp_xdr.c \
	reffs_echo_xdr.h \
	reffs_echo_xdr.c \
	nfsv42_xdr.h \
	nfsv42_xdr.c \
	mntv3_xdr.h \
	mntv3_xdr.c \
	nfsv3_xdr.h \
	nfsv3_xdr.c

CLEANFILES = $(GEN_SOURCES)

lib_LTLIBRARIES = libreffs_xdr.la
libreffs_xdr_la_SOURCES = $(GEN_SOURCES) ref_cp_xdr.x reffs_echo_xdr.x nfsv42_xdr.x mntv3_xdr.x nfsv3_xdr.x rpc_xdr.x

%.c: %.x %.h
	rpcgen -C -c $< > $@
	./insert_pragmas.sh $@

%.h: %.x
	rpcgen -C -h $< > $@
