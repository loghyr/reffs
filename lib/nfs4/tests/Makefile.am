# SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: GPL-2.0+

AM_CFLAGS = -Im4 -I.. \
	@CHECK_CFLAGS@ \
	-I$(top_srcdir)/lib/include \
	-I$(top_srcdir)/lib/xdr

LDADD = \
	$(top_srcdir)/lib/nfs4/libreffs_nfs4.la \
	@CHECK_LIBS@

check_PROGRAMS = $(TESTS)

TESTS = \
	check_errors

check_errors_SOURCES = errors.c
