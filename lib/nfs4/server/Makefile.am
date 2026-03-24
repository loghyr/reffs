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
	register.c \
	session.c \
	stateid.c \
	xattr.c
