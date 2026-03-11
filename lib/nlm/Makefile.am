# SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

AM_CFLAGS = -Im4 -I. \
	-I$(top_srcdir)/lib/include \
	-I$(top_builddir)/lib/xdr \
	$(TIRPC_CFLAGS) \
	$(URCU_CFLAGS) \
	$(URCU_CDS_CFLAGS)

AM_LDFLAGS = -rdynamic \
	$(ASAN_LDFLAGS) $(LSAN_LDFLAGS) \
	$(UBSAN_LDFLAGS) $(TSAN_LDFLAGS)

noinst_LTLIBRARIES = libnlm.la
libnlm_la_SOURCES = nlm_server.c nlm_lock.c

SUBDIRS = tests
