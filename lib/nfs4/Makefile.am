# SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

SUBDIRS = tests

AM_CFLAGS = -Im4 -I.			\
	-I$(top_srcdir)/lib/include	\
	-I$(top_builddir)/lib/xdr	\
	$(TIRPC_CFLAGS)

lib_LTLIBRARIES = libreffs_nfs4.la
libreffs_nfs4_la_SOURCES = \
	attr.c \
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
	nfs4_client.c \
	nfs4_stateid.c \
	ops.c \
	register.c \
	session.c \
	xattr.c

reffsincludedir = $(includedir)/reffs
reffsinclude_HEADERS = \
	attr.h \
	compound.h \
	errors.h \
	nfs4_client.h \
	nfs4_stateid.h \
	ops.h
