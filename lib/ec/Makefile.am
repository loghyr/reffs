# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

SUBDIRS = snapraid-raid linux-md-raid isa-l-erasure tests

AM_CFLAGS = -I$(top_srcdir)/lib/include

noinst_LTLIBRARIES = libreffs_ec.la
libreffs_ec_la_SOURCES = gf.c isa_l.c linux_md.c matrix.c mirror.c mojette.c mojette_encoding.c rs.c snapraid.c stripe.c xor.c
libreffs_ec_la_LIBADD = snapraid-raid/libreffs_snapraid_raid.la \
	linux-md-raid/libreffs_linux_md_raid.la \
	isa-l-erasure/libreffs_isa_l_erasure.la

# isa_l.c needs to see the vendored ISA-L public header.
isa_l.lo: AM_CFLAGS += -I$(top_srcdir)/lib/ec/isa-l-erasure/include

# Enable AVX2 for the Mojette forward transform SIMD fast path.
# configure detects -mavx2 support and sets AVX2_CFLAGS accordingly.
mojette.lo: AM_CFLAGS += @AVX2_CFLAGS@

# --enable-noscalar-vec applies -fno-tree-vectorize -fno-slp-vectorize
# to the encoding source files so the "scalar" path used by the
# SIMD-vs-scalar honest comparison cannot be auto-vectorized by the
# compiler.  When the flag is absent NOSCALAR_VEC_CFLAGS is empty so
# this is a no-op for normal builds.  Used by the
# experiments/03-larger-shards SIMD-vs-scalar arm (see
# experiments/03-larger-shards.md).
gf.lo:            AM_CFLAGS += @NOSCALAR_VEC_CFLAGS@
matrix.lo:        AM_CFLAGS += @NOSCALAR_VEC_CFLAGS@
mojette.lo:       AM_CFLAGS += @NOSCALAR_VEC_CFLAGS@
mojette_encoding.lo: AM_CFLAGS += @NOSCALAR_VEC_CFLAGS@
stripe.lo:        AM_CFLAGS += @NOSCALAR_VEC_CFLAGS@

# rs.lo needs @NOSCALAR_VEC_CFLAGS@ (like the peers above) AND on
# x86_64 also -mssse3 so Slice R.2's pshufb intrinsic path
# activates (__SSSE3__ is only defined when the flag is on the
# compile line).  Automake warns if the same target is assigned
# in multiple conditional branches, so build the assignment
# in one place.
if HAVE_MD_X86_SIMD
rs.lo:            AM_CFLAGS += @NOSCALAR_VEC_CFLAGS@ -mssse3
else
rs.lo:            AM_CFLAGS += @NOSCALAR_VEC_CFLAGS@
endif
