# SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: GPL-2.0+

SUBDIRS = tests

AM_CFLAGS = -Im4 -I.			\
	-I$(top_srcdir)/lib/include	\
	-I$(top_builddir)/lib/xdr	\
	$(TIRPC_CFLAGS)

lib_LTLIBRARIES = libreffs_nfs4.la
libreffs_nfs4_la_SOURCES = \
	dispatch.c \
	errors.c \
	filehandle.c \
	lock.c \
	nfs4_server.c \
	ops.c

reffsincludedir = $(includedir)/reffs
reffsinclude_HEADERS = \
	errors.h \
	nfs4_internal.h \
	ops.h
