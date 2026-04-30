# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

SUBDIRS = tests

AM_CFLAGS = -I$(top_srcdir)/lib/include

noinst_LTLIBRARIES = libreffs_ec.la
libreffs_ec_la_SOURCES = gf.c matrix.c mojette.c mojette_codec.c rs.c stripe.c

# Enable AVX2 for the Mojette forward transform SIMD fast path.
# configure detects -mavx2 support and sets AVX2_CFLAGS accordingly.
mojette.lo: AM_CFLAGS += @AVX2_CFLAGS@

# --enable-noscalar-vec applies -fno-tree-vectorize -fno-slp-vectorize
# to the codec source files so the "scalar" path used by the
# SIMD-vs-scalar honest comparison cannot be auto-vectorized by the
# compiler.  When the flag is absent NOSCALAR_VEC_CFLAGS is empty so
# this is a no-op for normal builds.  Used by the
# experiments/03-larger-shards SIMD-vs-scalar arm (see
# experiments/03-larger-shards.md).
gf.lo:            AM_CFLAGS += @NOSCALAR_VEC_CFLAGS@
matrix.lo:        AM_CFLAGS += @NOSCALAR_VEC_CFLAGS@
mojette.lo:       AM_CFLAGS += @NOSCALAR_VEC_CFLAGS@
mojette_codec.lo: AM_CFLAGS += @NOSCALAR_VEC_CFLAGS@
rs.lo:            AM_CFLAGS += @NOSCALAR_VEC_CFLAGS@
stripe.lo:        AM_CFLAGS += @NOSCALAR_VEC_CFLAGS@
