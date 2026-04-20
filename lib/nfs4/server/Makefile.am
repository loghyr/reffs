# SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

AM_CFLAGS = -Im4 -I.			\
	-I$(top_srcdir)/lib/include	\
	-I$(top_srcdir)/lib/nfs4/include \
	-I$(top_builddir)/lib/xdr	\
	$(TIRPC_CFLAGS)

lib_LTLIBRARIES = libreffs_nfs4_server.la
libreffs_nfs4_server_la_SOURCES = \
	attr.c \
	cb.c \
	cb_timeout.c \
	client.c \
	client_persist.c \
	compound.c \
	chunk.c \
	chunk_store.c \
	copy.c \
	delegation.c \
	dir.c \
	dispatch.c \
	errors.c \
	file.c \
	filehandle.c \
	layout.c \
	lease_reaper.c \
	lock.c \
	ops.c \
	owner.c \
	security.c \
	register.c \
	session.c \
	stateid.c \
	trust_stateid.c \
	trust_stateid_ops.c \
	xattr.c

# libreffs_dstore is a shared library (installed); this dylib depends
# on its dstore_ops_{nfsv3,nfsv4,local} globals and dstore_*/runway_*
# functions.  Must be a shared (not static-convenience) dep, otherwise
# each consumer dylib carries its own copy of the ops structs and
# `ds->ds_ops == &dstore_ops_local` address comparisons fail across
# copies (ASAN also flags it as an ODR violation under -fsanitize=address).
libreffs_nfs4_server_la_LIBADD = \
	$(top_builddir)/lib/nfs4/dstore/libreffs_dstore.la
