# SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: GPL-2.0+

AM_CFLAGS = -I$(top_srcdir)/lib/include -I$(top_builddir)/lib/xdr $(TIRPC_CFLAGS) $(URCU_CFLAGS)

noinst_LTLIBRARIES = libnsm.la
libnsm_la_SOURCES = nsm_server.c
