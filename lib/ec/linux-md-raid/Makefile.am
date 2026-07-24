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
# SIMD variants (avx2/avx512/sse2/neon and their recov_*) deferred
# to a later slice -- see NOTICE.md "What lands (planned)".

noinst_LTLIBRARIES = libreffs_linux_md_raid.la

# The generated .c files must be listed in nodist_ so automake does
# not try to distribute them.
libreffs_linux_md_raid_la_SOURCES = \
	algos.c \
	recov.c

nodist_libreffs_linux_md_raid_la_SOURCES = \
	int1.c int2.c int4.c int8.c int16.c int32.c \
	tables.c

# -I. lets sources find our vendored pq.h at linux/raid/pq.h
# via the standard #include <linux/raid/pq.h>.
AM_CPPFLAGS = -I$(srcdir)

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

# mktables is a userspace program that prints the GF-multiply +
# log/exp tables as a C source file.  Run it at build time to
# generate tables.c.  Using $(CC) (target compiler) since reffs
# does not cross-compile today; if that ever changes, swap for
# $(CC_FOR_BUILD).
mktables$(EXEEXT): mktables.c
	$(CC) -o $@ $(srcdir)/mktables.c

tables.c: mktables$(EXEEXT)
	./mktables$(EXEEXT) > $@

# Cleanup generated files.
CLEANFILES = int1.c int2.c int4.c int8.c int16.c int32.c \
	tables.c mktables$(EXEEXT)

EXTRA_DIST = NOTICE.md int.uc mktables.c unroll.awk linux/raid/pq.h
