# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Vendored SnapRAID raid/ library.  Sources carry Andrea Mazzoleni's
# GPL-2.0-or-later / GPL-3.0-or-later headers -- see NOTICE.md.
#
# All SIMD .c files are ifdef-guarded on CONFIG_X86 / CONFIG_X86_64
# / CONFIG_NEON (see internal.h's compiler-predefine autodetection)
# so they compile as empty translation units on non-matching targets.
# No per-file CFLAGS are needed: Andrea uses inline asm rather than
# intrinsic headers, so the compiler does not need -mavx2 / -mavx512
# / -mgfni / etc.

noinst_LTLIBRARIES = libreffs_snapraid_raid.la

libreffs_snapraid_raid_la_SOURCES = \
	raid.c \
	int.c \
	helper.c \
	memory.c \
	tables.c \
	module.c \
	check.c \
	avx2.c \
	avx512.c \
	ssse3.c \
	sse2.c \
	gfni.c \
	neon.c \
	raid.h \
	internal.h \
	gf.h \
	helper.h \
	memory.h \
	cpu.h \
	combo.h

# Andrea's sources use the GNU `asm` keyword directly.  reffs
# builds with -std=c11, which disables it -- but "-Dasm=__asm__"
# fixes that without fighting -std flag ordering (top-level
# CFLAGS puts -std=c11 last, so a -std=gnu11 here would lose).
# Same rationale for the -Wno-* set: silence warnings from vendored
# code without editing.
#
# -fsanitize-ignorelist: the CPUID probe in raid/cpu.h writes
# 3 uint32_t vendor-string chunks into a char[13] buffer at
# 1-byte-aligned offsets (harmless on x86 -- unaligned stores
# just work -- but strict-UB per the C standard).  UBSAN nightly
# builds reported this every time raid_init() ran; ignorelist
# suppresses the alignment check specifically for that file
# without editing Andrea's code.  Every other UBSAN check stays
# on for the vendored subtree.  See NOTICE.md "Build-scoped
# UBSAN carve-out" for the rationale and the flag-ordering
# reason we do not use -fno-sanitize=alignment (which loses to
# the later -fsanitize=undefined in top-level CFLAGS).
AM_CPPFLAGS = -Dasm=__asm__
AM_CFLAGS = -Wno-unused-parameter -Wno-unused-function \
	-fsanitize-ignorelist=$(srcdir)/ubsan-ignorelist.txt

EXTRA_DIST = NOTICE.md ubsan-ignorelist.txt
