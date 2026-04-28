# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

AM_CFLAGS = -I$(top_srcdir)/lib/include		\
	-I$(top_srcdir)/lib/nfs4/client		\
	-I$(top_builddir)/lib/xdr		\
	$(OPENSSL_CFLAGS)			\
	$(TIRPC_CFLAGS)				\
	@CHECK_CFLAGS@

LDADD = @CHECK_LIBS@						\
	$(top_builddir)/lib/nfs4/client/libreffs_nfs4_client.la	\
	$(top_builddir)/lib/tls/libreffs_tls.la			\
	$(top_builddir)/lib/xdr/libreffs_xdr.la			\
	$(OPENSSL_LIBS)						\
	$(TIRPC_LIBS)

check_PROGRAMS = $(TESTS)
LOG_COMPILER = $(top_srcdir)/scripts/timed-test.sh

TESTS = \
	host_port_test \
	mds_compound_test \
	mds_tls_xprt_test \
	proxy_reg_status_test
