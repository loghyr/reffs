# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

AM_CFLAGS = -I$(top_srcdir)/lib/d2store -I$(top_srcdir)/lib/d1store
AUTOMAKE_OPTIONS = serial-tests
LDADD = $(top_builddir)/lib/d2store/libd2store.la \
	$(top_builddir)/lib/d1store/libd1store.la -lpthread

TESTS = d2_format_test
check_PROGRAMS = $(TESTS)
d2_format_test_SOURCES = d2_format_test.c
