# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

AM_CFLAGS = \
	-I$(top_srcdir)/lib/include	\
	$(OPENSSL_CFLAGS)

noinst_LTLIBRARIES = libreffs_tls.la
libreffs_tls_la_SOURCES = tls_client.c
# tls_client.c picks up source_bind.h's inline bindresvport_sa call
# via the --source-ip path; propagate libtirpc through libtool's .la
# linkage so consumers (nfs_tls_test, lib/nfs4/tests/*, ...) do not
# each have to re-list TIRPC_LIBS.
libreffs_tls_la_LIBADD = $(TIRPC_LIBS)
