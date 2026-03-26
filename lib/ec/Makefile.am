# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

SUBDIRS = tests

AM_CFLAGS = -I$(top_srcdir)/lib/include

noinst_LTLIBRARIES = libreffs_ec.la
libreffs_ec_la_SOURCES = gf.c matrix.c mojette.c mojette_codec.c rs.c stripe.c

# Enable AVX2 for the Mojette forward transform SIMD fast path.
# configure detects -mavx2 support and sets AVX2_CFLAGS accordingly.
mojette.lo: AM_CFLAGS += @AVX2_CFLAGS@
