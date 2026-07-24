# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Vendored Linux md/raid6 library.  Sources carry H. Peter Anvin's
# GPL-2.0-or-later headers -- see NOTICE.md.
#
# int1.c ... int32.c are generated at build time from int.uc by
# unroll.awk (Linux's own preprocessor).  tables.c is generated
# by mktables (a build-time helper that emits the GF tables as C).
# Both patterns are lifted verbatim from Linux's own
# lib/raid6/test/Makefile so the userspace-portable subset builds
# without any kernel headers.
#
# Slice 6.3 adds x86 (SSE2 / AVX2 / AVX-512) and aarch64 (NEON)
# SIMD variants behind HAVE_MD_X86_SIMD / HAVE_MD_NEON_SIMD
# conditionals from configure.ac.

noinst_LTLIBRARIES = libreffs_linux_md_raid.la

# The generated .c files must be listed in nodist_ so automake does
# not try to distribute them.
libreffs_linux_md_raid_la_SOURCES = \
	algos.c \
	recov.c

nodist_libreffs_linux_md_raid_la_SOURCES = \
	int1.c int2.c int4.c int8.c int16.c int32.c \
	tables.c

# Slice 6.3 x86 SIMD (SSE2 / AVX2 / AVX-512) -- 8 files + x86.h.
# Per-file arch flags below.
if HAVE_MD_X86_SIMD
libreffs_linux_md_raid_la_SOURCES += \
	mmx.c sse1.c sse2.c avx2.c avx512.c \
	recov_ssse3.c recov_avx2.c recov_avx512.c
endif

# Slice 6.3 aarch64 NEON -- neon.c dispatcher + recov_neon.c +
# recov_neon_inner.c, plus neon{1,2,4,8}.c generated from neon.uc.
if HAVE_MD_NEON_SIMD
libreffs_linux_md_raid_la_SOURCES += \
	neon.c recov_neon.c recov_neon_inner.c
nodist_libreffs_linux_md_raid_la_SOURCES += \
	neon1.c neon2.c neon4.c neon8.c
endif

# -I. lets sources find our vendored pq.h at linux/raid/pq.h
# via the standard #include <linux/raid/pq.h>.
#
# When NEITHER x86 nor NEON SIMD is available on this host,
# define __arch_um__=1 so algos.c's arch-specific dispatch
# blocks (`#if defined(__x86_64__) && !defined(__arch_um__)`)
# gate OUT their references to SIMD symbols we did not build.
# When SIMD IS built for this arch, leave __arch_um__ undefined
# so the arch block activates and picks up the new SIMD entries.
# `asm` as a keyword is a GNU extension not accepted under -std=c11;
# alias to __asm__ globally on the sub-lib (matches the SnapRAID
# vendor's identical carve-out).  Vendored source unchanged.
if HAVE_MD_X86_SIMD
# On x86_64 algos.c's `#if defined(__x86_64__)` block activates
# naturally on the compiler-defined arch macro.  The `x4`
# variants (raid6_sse2x4, raid6_avx2x4, raid6_avx512x4)
# referenced from that block are defined inside
# `#ifdef CONFIG_X86_64` guards in sse2.c / avx2.c / avx512.c
# (a Kconfig symbol that would always be set on x86_64 in the
# kernel).  Define it here so the x4 variants get built.
AM_CPPFLAGS = -I$(srcdir) -Dasm=__asm__ -DCONFIG_X86_64=1
else
if HAVE_MD_NEON_SIMD
# aarch64 has no compiler-defined arch macro that matches Linux
# md's NEON block; the kernel gates on CONFIG_KERNEL_MODE_NEON.
# Define it here so algos.c and recov.c both wire in the NEON
# dispatch entries we just built.
AM_CPPFLAGS = -I$(srcdir) -DCONFIG_KERNEL_MODE_NEON=1 -Dasm=__asm__
else
AM_CPPFLAGS = -I$(srcdir) -D__arch_um__=1 -Dasm=__asm__
endif
endif

# Silence all warnings for the vendored subtree via -w.  Linux md
# uses kernel coding conventions (signed int loop counters, KR-style
# prototypes in places) that reffs's top-level -Werror -Wextra treats
# as fatal.  A per-target -Wno-* would lose to the top-level -Werror
# via CFLAGS-comes-after-AM_CFLAGS ordering, so use the nuclear -w
# and lean on Anvin's own kernel-side CI for correctness on Linux
# md's code -- we do not own it.
AM_CFLAGS = -w

# Regenerate the .uc-derived C sources.  Each is int.uc with the
# UNROLL loop-factor N burned in via awk.
int1.c: int.uc unroll.awk
	$(AWK) -f $(srcdir)/unroll.awk -vN=1 < $(srcdir)/int.uc > $@
int2.c: int.uc unroll.awk
	$(AWK) -f $(srcdir)/unroll.awk -vN=2 < $(srcdir)/int.uc > $@
int4.c: int.uc unroll.awk
	$(AWK) -f $(srcdir)/unroll.awk -vN=4 < $(srcdir)/int.uc > $@
int8.c: int.uc unroll.awk
	$(AWK) -f $(srcdir)/unroll.awk -vN=8 < $(srcdir)/int.uc > $@
int16.c: int.uc unroll.awk
	$(AWK) -f $(srcdir)/unroll.awk -vN=16 < $(srcdir)/int.uc > $@
int32.c: int.uc unroll.awk
	$(AWK) -f $(srcdir)/unroll.awk -vN=32 < $(srcdir)/int.uc > $@

# neon.uc is the aarch64 NEON template.  Same unroll pattern as
# int.uc; NEON needs unroll factors 1, 2, 4, 8 (matches the
# raid6_neonx{1,2,4,8} dispatch entries in neon.c).
neon1.c: neon.uc unroll.awk
	$(AWK) -f $(srcdir)/unroll.awk -vN=1 < $(srcdir)/neon.uc > $@
neon2.c: neon.uc unroll.awk
	$(AWK) -f $(srcdir)/unroll.awk -vN=2 < $(srcdir)/neon.uc > $@
neon4.c: neon.uc unroll.awk
	$(AWK) -f $(srcdir)/unroll.awk -vN=4 < $(srcdir)/neon.uc > $@
neon8.c: neon.uc unroll.awk
	$(AWK) -f $(srcdir)/unroll.awk -vN=8 < $(srcdir)/neon.uc > $@

# mktables is a userspace program that prints the GF-multiply +
# log/exp tables as a C source file.  Run it at build time to
# generate tables.c.  Using $(CC) (target compiler) since reffs
# does not cross-compile today; if that ever changes, swap for
# $(CC_FOR_BUILD).
mktables$(EXEEXT): mktables.c
	$(CC) -o $@ $(srcdir)/mktables.c

tables.c: mktables$(EXEEXT)
	./mktables$(EXEEXT) > $@

# Per-file arch flags for the x86 SIMD variants.  Kernel Makefile
# uses `CFLAGS_avx2.o += -mavx2` etc.; the automake analogue is
# target-specific AM_CFLAGS on the .lo target.  These fire only
# when the corresponding source is in SOURCES, so they are safe
# to keep unconditionally.
sse1.lo:        AM_CFLAGS += -msse
sse2.lo:        AM_CFLAGS += -msse2
recov_ssse3.lo: AM_CFLAGS += -mssse3
avx2.lo:        AM_CFLAGS += -mavx2
recov_avx2.lo:  AM_CFLAGS += -mavx2
avx512.lo:      AM_CFLAGS += -mavx512f -mavx512bw
recov_avx512.lo: AM_CFLAGS += -mavx512f -mavx512bw
# mmx.c uses MMX intrinsics via <mmintrin.h>; on x86_64 MMX is
# implicitly available but the .c uses -mmmx explicitly in Linux.
mmx.lo:         AM_CFLAGS += -mmmx

# neon1..8.c compile with -march=armv8-a+simd on aarch64.
neon.lo:              AM_CFLAGS += -march=armv8-a+simd
neon1.lo:             AM_CFLAGS += -march=armv8-a+simd
neon2.lo:             AM_CFLAGS += -march=armv8-a+simd
neon4.lo:             AM_CFLAGS += -march=armv8-a+simd
neon8.lo:             AM_CFLAGS += -march=armv8-a+simd
# recov_neon.c calls __raid6_{2data,datap}_recov_neon() (defined in
# recov_neon_inner.c) but only #includes their forward-decl header
# (neon.h) under `#ifdef __KERNEL__`.  In userspace the calls hit
# implicit-declaration -Werror.  Force-include neon.h to satisfy
# the decls without touching the vendored source.
recov_neon.lo:        AM_CFLAGS += -march=armv8-a+simd \
	-include $(srcdir)/neon.h
recov_neon_inner.lo:  AM_CFLAGS += -march=armv8-a+simd

# Cleanup generated files.
CLEANFILES = int1.c int2.c int4.c int8.c int16.c int32.c \
	neon1.c neon2.c neon4.c neon8.c \
	tables.c mktables$(EXEEXT)

EXTRA_DIST = NOTICE.md int.uc neon.uc mktables.c unroll.awk \
	linux/raid/pq.h neon.h x86.h
