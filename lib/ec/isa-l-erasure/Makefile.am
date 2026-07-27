# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Vendored Intel ISA-L erasure_code/ subset.  Sources carry
# Intel's BSD-3-Clause -- see NOTICE.md.
#
# Slice 7.1 landed portable-C only.  Slice 7.3 adds the x86
# SIMD tree behind HAVE_ISA_L_X86_SIMD (host is x86_64 AND
# NASM assembler is available).  aarch64 NEON is deferred to
# a follow-up sub-slice.

# Sources live under erasure_code/ and erasure_code/aarch64/;
# subdir-objects tells automake to put per-source objects into
# the corresponding build-dir subdirs (matching the source
# layout instead of flattening everything into the top-level).
AUTOMAKE_OPTIONS = subdir-objects

# Register .asm as a build suffix so make's VPATH search fires
# on .asm sources under $(srcdir)/erasure_code/ (out-of-tree
# builds otherwise fail to find them from the build tree).
SUFFIXES = .asm

noinst_LTLIBRARIES = libreffs_isa_l_erasure.la

# Portable-C baseline (Slice 7.1).  Always compiled.
libreffs_isa_l_erasure_la_SOURCES = \
	erasure_code/ec_base.c

# When SIMD is NOT active, ec_base_aliases.c aliases the public
# API (ec_encode_data, ec_init_tables, gf_vect_*) to their
# _base implementations.  When SIMD IS active, those aliases
# collide with the multibinary dispatch entries in
# ec_multibinary.asm / ec_highlevel_func.c, so drop them.
if !HAVE_ISA_L_X86_SIMD
libreffs_isa_l_erasure_la_SOURCES += erasure_code/ec_base_aliases.c
endif

# x86 SIMD path (Slice 7.3).  72 gf_*.asm files + the runtime
# dispatcher (ec_multibinary.asm) + the high-level function
# hooks (ec_highlevel_func.c).  Every .asm assembles through
# NASM (see .asm.lo rule below); the .c compiles as normal.
if HAVE_ISA_L_X86_SIMD
libreffs_isa_l_erasure_la_SOURCES += \
	erasure_code/ec_highlevel_func.c \
	erasure_code/gf_vect_mul_sse.asm \
	erasure_code/gf_vect_mul_avx.asm \
	erasure_code/gf_vect_mul_avx2_gfni.asm \
	erasure_code/gf_vect_dot_prod_sse.asm \
	erasure_code/gf_vect_dot_prod_avx.asm \
	erasure_code/gf_vect_dot_prod_avx2.asm \
	erasure_code/gf_vect_dot_prod_avx2_gfni.asm \
	erasure_code/gf_vect_dot_prod_avx512.asm \
	erasure_code/gf_vect_dot_prod_avx512_gfni.asm \
	erasure_code/gf_2vect_dot_prod_sse.asm \
	erasure_code/gf_2vect_dot_prod_avx.asm \
	erasure_code/gf_2vect_dot_prod_avx2.asm \
	erasure_code/gf_2vect_dot_prod_avx2_gfni.asm \
	erasure_code/gf_2vect_dot_prod_avx512.asm \
	erasure_code/gf_2vect_dot_prod_avx512_gfni.asm \
	erasure_code/gf_3vect_dot_prod_sse.asm \
	erasure_code/gf_3vect_dot_prod_avx.asm \
	erasure_code/gf_3vect_dot_prod_avx2.asm \
	erasure_code/gf_3vect_dot_prod_avx2_gfni.asm \
	erasure_code/gf_3vect_dot_prod_avx512.asm \
	erasure_code/gf_3vect_dot_prod_avx512_gfni.asm \
	erasure_code/gf_4vect_dot_prod_sse.asm \
	erasure_code/gf_4vect_dot_prod_avx.asm \
	erasure_code/gf_4vect_dot_prod_avx2.asm \
	erasure_code/gf_4vect_dot_prod_avx512.asm \
	erasure_code/gf_4vect_dot_prod_avx512_gfni.asm \
	erasure_code/gf_5vect_dot_prod_sse.asm \
	erasure_code/gf_5vect_dot_prod_avx.asm \
	erasure_code/gf_5vect_dot_prod_avx2.asm \
	erasure_code/gf_5vect_dot_prod_avx512.asm \
	erasure_code/gf_5vect_dot_prod_avx512_gfni.asm \
	erasure_code/gf_6vect_dot_prod_sse.asm \
	erasure_code/gf_6vect_dot_prod_avx.asm \
	erasure_code/gf_6vect_dot_prod_avx2.asm \
	erasure_code/gf_6vect_dot_prod_avx512.asm \
	erasure_code/gf_6vect_dot_prod_avx512_gfni.asm \
	erasure_code/gf_vect_mad_sse.asm \
	erasure_code/gf_vect_mad_avx.asm \
	erasure_code/gf_vect_mad_avx2.asm \
	erasure_code/gf_vect_mad_avx2_gfni.asm \
	erasure_code/gf_vect_mad_avx512.asm \
	erasure_code/gf_vect_mad_avx512_gfni.asm \
	erasure_code/gf_2vect_mad_sse.asm \
	erasure_code/gf_2vect_mad_avx.asm \
	erasure_code/gf_2vect_mad_avx2.asm \
	erasure_code/gf_2vect_mad_avx2_gfni.asm \
	erasure_code/gf_2vect_mad_avx512.asm \
	erasure_code/gf_2vect_mad_avx512_gfni.asm \
	erasure_code/gf_3vect_mad_sse.asm \
	erasure_code/gf_3vect_mad_avx.asm \
	erasure_code/gf_3vect_mad_avx2.asm \
	erasure_code/gf_3vect_mad_avx2_gfni.asm \
	erasure_code/gf_3vect_mad_avx512.asm \
	erasure_code/gf_3vect_mad_avx512_gfni.asm \
	erasure_code/gf_4vect_mad_sse.asm \
	erasure_code/gf_4vect_mad_avx.asm \
	erasure_code/gf_4vect_mad_avx2.asm \
	erasure_code/gf_4vect_mad_avx2_gfni.asm \
	erasure_code/gf_4vect_mad_avx512.asm \
	erasure_code/gf_4vect_mad_avx512_gfni.asm \
	erasure_code/gf_5vect_mad_sse.asm \
	erasure_code/gf_5vect_mad_avx.asm \
	erasure_code/gf_5vect_mad_avx2.asm \
	erasure_code/gf_5vect_mad_avx2_gfni.asm \
	erasure_code/gf_5vect_mad_avx512.asm \
	erasure_code/gf_5vect_mad_avx512_gfni.asm \
	erasure_code/gf_6vect_mad_sse.asm \
	erasure_code/gf_6vect_mad_avx.asm \
	erasure_code/gf_6vect_mad_avx2.asm \
	erasure_code/gf_6vect_mad_avx512.asm \
	erasure_code/gf_6vect_mad_avx512_gfni.asm \
	erasure_code/ec_multibinary.asm
endif

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

# NASM assembly rule.  ISA-L's .asm sources use NASM syntax with
# ELF64 (Linux) or MACHO64 (macOS) output formats.  Automake does
# not natively support NASM; drive it manually and hand the
# resulting .o off to libtool through the -.libs/-copy trick that
# lets libtool consume a pre-existing object.
if HAVE_ISA_L_X86_SIMD
NASM_INCLUDES = -I$(srcdir)/include/ -I$(srcdir)/erasure_code/
if HOST_DARWIN
NASM_FORMAT = -f macho64 --prefix _
else
NASM_FORMAT = -f elf64
endif
NASM_FLAGS = $(NASM_FORMAT) $(NASM_INCLUDES) -DHAVE_AS_KNOWS_AVX512 \
	-DAS_FEATURE_LEVEL=10

# Assemble .asm -> .lo.  Libtool's --tag=CC --mode=compile does
# not directly drive NASM, so build the .o ourselves and stage
# it under .libs/ + write the .lo pseudo-file so libtool's link
# stage picks it up.
# libtool's .lo pseudo-file format requires a specific shape --
# match what libtool itself writes for a normal CC compile so the
# link stage accepts the NASM-produced .o.  Both pic_object and
# non_pic_object must be present; setting non_pic_object='none'
# is rejected by libtool 2.5+.  We point both at the same .o
# (no separate PIC / non-PIC builds needed here since the sub-
# library is always linked into another static or shared target
# and the NASM output is position-independent already).
.asm.lo:
	@mkdir -p $(@D)/.libs
	$(NASM) $(NASM_FLAGS) -o $(@D)/.libs/$(*F).o $<
	@echo "# $@ - a libtool object file" > $@
	@echo "# Generated by libtool (GNU libtool) 2.5.4" >> $@
	@echo "#" >> $@
	@echo "# Please DO NOT delete this file!" >> $@
	@echo "# It is necessary for linking the library." >> $@
	@echo "" >> $@
	@echo "# Name of the PIC object." >> $@
	@echo "pic_object='.libs/$(*F).o'" >> $@
	@echo "" >> $@
	@echo "# Name of the non-PIC object" >> $@
	@echo "non_pic_object='$(*F).o'" >> $@
	@echo "" >> $@
	@cp $(@D)/.libs/$(*F).o $(@D)/$(*F).o
endif

EXTRA_DIST = NOTICE.md LICENSE \
	erasure_code/ec_base.h \
	erasure_code/gf_vect_gfni.inc \
	include/erasure_code.h \
	include/gf_vect_mul.h \
	include/isal_api.h \
	include/reg_sizes.asm \
	include/multibinary.asm \
	include/aarch64_multibinary.h \
	include/memcpy.asm \
	include/crc.inc
