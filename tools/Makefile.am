# SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

AM_CFLAGS = \
	-I$(top_srcdir)/lib/include		\
	-I$(top_srcdir)/lib/ec			\
	-I$(top_srcdir)/lib/nfs4/client		\
	-I$(top_builddir)/lib/xdr		\
	$(TIRPC_CFLAGS)

NFS4_CLIENT_LIBS = \
	$(top_builddir)/lib/nfs4/client/libreffs_nfs4_client.la	\
	$(top_builddir)/lib/xdr/libreffs_xdr.la			\
	$(TIRPC_LIBS) $(GSSAPI_LIBS)

ec_demo_SOURCES = ec_demo.c
ec_demo_LDADD = $(NFS4_CLIENT_LIBS) \
	$(top_builddir)/lib/ec/libreffs_ec.la

nfs_krb5_test_SOURCES = nfs_krb5_test.c
nfs_krb5_test_LDADD = $(NFS4_CLIENT_LIBS)

TLS_CLIENT_SRC = $(top_srcdir)/lib/tls/tls_client.c

nfs_tls_test_SOURCES = nfs_tls_test.c $(TLS_CLIENT_SRC)
nfs_tls_test_CFLAGS = $(AM_CFLAGS) $(OPENSSL_CFLAGS)
nfs_tls_test_LDADD = $(OPENSSL_LIBS)

bin_PROGRAMS = ec_demo nfs_krb5_test nfs_tls_test
