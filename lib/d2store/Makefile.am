# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

SUBDIRS = tests

AM_CFLAGS = -I$(srcdir) -I$(top_srcdir)/lib/d1store

noinst_LTLIBRARIES = libd2store.la
libd2store_la_SOURCES = d2_files.c d2_format.c
libd2store_la_LIBADD = $(OPENSSL_LIBS) $(ZLIB_LIBS)

noinst_HEADERS = d2_files.h d2_format.h
