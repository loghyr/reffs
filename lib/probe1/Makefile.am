# SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

AM_CFLAGS = -Im4 -I. \
	-I$(top_srcdir)/lib/include	\
	-I$(top_builddir)/lib/xdr	\
	$(PTHREAD_CFLAGS)		\
	$(URCU_CFLAGS)			\
	$(URCU_CDS_CFLAGS)		\
	$(TIRPC_CFLAGS)			\
	$(OPENSSL_CFLAGS)		\
	$(ZSTD_CFLAGS)			\
	$(FUSE_CFLAGS)

lib_LTLIBRARIES = libreffs_probe1.la
libreffs_probe1_la_SOURCES =	\
	probe1_client.c		\
	probe1_server.c
