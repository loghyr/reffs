# SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: GPL-2.0+

ACLOCAL_AMFLAGS = -Im4

AM_CFLAGS = \
	-I../lib/include \
	-I../lib/xdr

LDADD = \
	../lib/xdr/libreffsxdr.la \
	-ljemalloc \
	-lpthread

reffs_SOURCES = reffs.c
reffs_LDADD = $(LDADD)

bin_PROGRAMS = \
	reffs
