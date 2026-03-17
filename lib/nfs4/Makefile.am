# SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

SUBDIRS = tests

AM_CFLAGS = -Im4 -I.			\
	-I$(top_srcdir)/lib/include	\
	-I$(top_srcdir)/lib/nfs4/include \
	-I$(top_builddir)/lib/xdr	\
	$(TIRPC_CFLAGS)

lib_LTLIBRARIES = libreffs_nfs4.la
libreffs_nfs4_la_SOURCES = \
	attr.c \
	client.c \
	client_persist.c \
	compound.c \
	copy.c \
	delegation.c \
	dir.c \
	dispatch.c \
	errors.c \
	file.c \
	filehandle.c \
	layout.c \
	lock.c \
	ops.c \
	register.c \
	session.c \
	stateid.c \
	xattr.c

reffsincludedir = $(includedir)/reffs/nfs4
reffsinclude_HEADERS = \
	include/nfs4/attr.h \
	include/nfs4/client.h \
	include/nfs4/client_persist.h \
	include/nfs4/compound.h \
	include/nfs4/errors.h \
	include/nfs4/ops.h \
	include/nfs4/stateid.h
