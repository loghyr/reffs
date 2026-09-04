# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

AM_CFLAGS = \
	-I$(top_srcdir)/lib/nfs4/include \
	-I$(top_srcdir)/third_party/blake3 \
	-I$(top_builddir)/lib/xdr \
	$(OPENSSL_CFLAGS) \
	$(TIRPC_CFLAGS)

noinst_LTLIBRARIES = libreffs_nfs4_common.la

libreffs_nfs4_common_la_SOURCES = checksum.c
libreffs_nfs4_common_la_LIBADD = \
	$(top_builddir)/third_party/blake3/libblake3.la \
	$(OPENSSL_LIBS) \
	$(ZLIB_LIBS)
