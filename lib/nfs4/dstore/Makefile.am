# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

SUBDIRS = tests

AM_CFLAGS = -I$(top_srcdir)/lib/include \
	-I$(top_srcdir)/lib/nfs4/include \
	-I$(top_srcdir)/lib/nfs4/client \
	$(TIRPC_CFLAGS) \
	-I$(top_builddir)/lib/xdr

# libreffs_dstore as a proper installed shared library.  Previously
# a noinst_LTLIBRARIES static convenience archive, but that caused
# ODR violations when multiple consumer dylibs
# (libreffs_nfs3, libreffs_nfs4_server, libreffs_probe1) each folded
# in their own copy of dstore_ops_{nfsv3,nfsv4,local} globals via
# LIBADD.  ASAN's odr-violation detector caught it; it was also a
# latent correctness bug since dstore code does `ds->ds_ops ==
# &dstore_ops_local` address comparisons that would silently fail
# across copies.  Single shared-lib instance fixes both.
lib_LTLIBRARIES = libreffs_dstore.la
libreffs_dstore_la_SOURCES = dstore.c dstore_fanout.c dstore_ops_nfsv3.c dstore_ops_nfsv4.c dstore_ops_local.c dstore_wcc.c runway.c ds_session.c
