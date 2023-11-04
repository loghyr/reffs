# SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: GPL-2.0+

ACLOCAL_AMFLAGS = -Im4

AM_CFLAGS = \
	-I../lib/include \
	-I../lib/xdr

LDADD = \
	../lib/xdr/libreffsxdr.la

echo_SOURCES = echo.c
echo_LDADD = $(LDADD)

reffs_SOURCES = reffs.c
reffs_LDADD = $(LDADD)

bin_PROGRAMS = \
	echo \
	reffs
