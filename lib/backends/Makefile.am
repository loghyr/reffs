# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

AM_CFLAGS = -Im4 -I.			\
	-I$(top_srcdir)/lib/include	\
	-I$(top_builddir)/lib/xdr	\
	$(TIRPC_CFLAGS)

lib_LTLIBRARIES = libbackends.la

libbackends_la_SOURCES = \
	client_persist.c \
	driver.c \
	ram.c \
	ram_data.c \
	posix.c \
	posix_data.c \
	server_persist.c

if HAVE_ROCKSDB
libbackends_la_SOURCES += rocksdb.c
libbackends_la_CFLAGS = $(AM_CFLAGS) $(ROCKSDB_CFLAGS)
libbackends_la_LIBADD = $(ROCKSDB_LIBS)
endif
