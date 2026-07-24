# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Vendored Intel ISA-L erasure_code/ subset (portable-C only,
# no NASM).  Sources carry Intel's BSD-3-Clause -- see NOTICE.md.
#
# Slice 7.3 will add the SIMD tree + configure.ac AC_CHECK_PROG(NASM)
# gate.  For now the build is portable-C uniformly.

noinst_LTLIBRARIES = libreffs_isa_l_erasure.la

libreffs_isa_l_erasure_la_SOURCES = \
	erasure_code/ec_base.c \
	erasure_code/ec_base_aliases.c

# -I$(srcdir)/include lets sources find <erasure_code.h> and
# <gf_vect_mul.h>.  -I$(srcdir)/erasure_code lets ec_base.c
# find its sibling ec_base.h (the GF tables).
#
# reffs's own lib/ec/gf.c exports `gf_mul` and `gf_inv` too, and
# they end up in libreffs_ec.a alongside ISA-L's copies via
# LIBADD -- the linker rejects the collision.  Rename ISA-L's
# copies via -D so the vendored source stays untouched and the
# refresh procedure keeps its diff-of-zero property.  reffs's
# own gf_mul / gf_inv operate over the identical GF(2^8) (poly
# 0x11d, generator 2) so any external consumer of ISA-L's
# public API still gets bit-identical answers -- and none of
# the shipped public API (ec_encode_data, ec_init_tables,
# gf_gen_rs_matrix, gf_invert_matrix) needs gf_mul on its hot
# path anyway.
AM_CPPFLAGS = -I$(srcdir)/include \
	-I$(srcdir)/erasure_code \
	-Dgf_mul=isa_l_gf_mul \
	-Dgf_inv=isa_l_gf_inv

# Silence all warnings for the vendored subtree via -w.  ISA-L
# uses upstream coding conventions that reffs's top-level
# -Werror -Wextra treats as fatal (signed loop counters, KR-style
# prototypes).  A per-target -Wno-* would lose to the top-level
# -Werror via CFLAGS-comes-after-AM_CFLAGS ordering, so use the
# nuclear -w and lean on Intel's own upstream CI for correctness.
AM_CFLAGS = -w

EXTRA_DIST = NOTICE.md LICENSE \
	erasure_code/ec_base.h \
	include/erasure_code.h \
	include/gf_vect_mul.h \
	include/isal_api.h
