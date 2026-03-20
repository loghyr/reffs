# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

SUBDIRS = tests

AM_CFLAGS = \
	-I$(top_srcdir)/lib/include		\
	-I$(top_builddir)/lib/xdr		\
	-I$(top_srcdir)/third_party/tomlc99	\
	$(TIRPC_CFLAGS)

lib_LTLIBRARIES = libreffs_config.la

libreffs_config_la_SOURCES = config.c

libreffs_config_la_LIBADD = \
	$(top_builddir)/third_party/tomlc99/libtomlc99.la \
	$(top_builddir)/lib/utils/libreffs_utils.la
