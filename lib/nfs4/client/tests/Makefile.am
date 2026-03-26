# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

AM_CFLAGS = -I$(top_srcdir)/lib/include		\
	-I$(top_srcdir)/lib/nfs4/client		\
	-I$(top_builddir)/lib/xdr		\
	$(TIRPC_CFLAGS)				\
	@CHECK_CFLAGS@

LDADD = @CHECK_LIBS@						\
	$(top_builddir)/lib/nfs4/client/libreffs_nfs4_client.la	\
	$(top_builddir)/lib/xdr/libreffs_xdr.la			\
	$(TIRPC_LIBS)

check_PROGRAMS = $(TESTS)
LOG_COMPILER = $(top_srcdir)/scripts/timed-test.sh

TESTS = \
	mds_compound_test \
	ec_io_test
