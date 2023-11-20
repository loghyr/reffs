# SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: GPL-2.0+

AM_CFLAGS = -Im4 -I.. @CHECK_CFLAGS@ \
	-I$(top_srcdir)/lib/include \
	-I$(top_srcdir)/lib/xdr

LDADD = \
	@CHECK_LIBS@ \
	$(top_srcdir)/lib/nfs4/libreffs_nfs4.la \
	$(top_srcdir)/lib/utils/libreffs_utils.la

check_PROGRAMS = $(TESTS)

TESTS = \
	errors

