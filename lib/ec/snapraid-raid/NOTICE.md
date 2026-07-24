<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Vendored: SnapRAID `raid/` library

This directory holds a verbatim vendor copy of the `raid/`
sub-tree from Andrea Mazzoleni's SnapRAID project.

## Upstream

- Project: **SnapRAID** by **Andrea Mazzoleni**
- Site: https://www.snapraid.it
- Repository: https://github.com/amadvance/snapraid
- Vendored at commit: `c41ac8bfba4518158dfff97465a45f259556cac3`
  (default branch as of 2026-07-22)
- Subtree vendored: `raid/` only (the parity/erasure library).
  The GPL-3.0 CLI / `cmdline/` / `os/` / `tommyds/` surfaces are
  NOT vendored; reffs has no dependency on them.

## Files vendored

- Headers: `raid.h`, `internal.h`, `gf.h`, `helper.h`,
  `memory.h`, `cpu.h`, `combo.h`
- Portable: `raid.c`, `int.c`, `helper.c`, `memory.c`,
  `tables.c`, `module.c`
- SIMD (per-arch, ifdef-guarded to compile as empty
  translation units on non-matching targets):
  `avx2.c`, `avx512.c`, `ssse3.c`, `sse2.c`, `gfni.c`, `neon.c`
- Self-check: `check.c`

## Files NOT vendored (from upstream `raid/`)

- `test.c`, `test.h`, `test/` — Andrea's standalone test
  harness.  reffs has its own tests in
  `lib/ec/tests/snapraid_test.c`.
- `mktables.c` — build-time table generator.  `tables.c`
  is checked in, so we don't need to regenerate at build.
  Add back if we ever need to change the field polynomial.

## Licenses

Per Andrea's per-file SPDX headers:

- **GPL-2.0-or-later**: `raid.h`, `internal.h`, `gf.h`,
  `helper.h`, `helper.c`, `memory.h`, `memory.c`, `cpu.h`,
  `combo.h`, `raid.c`, `int.c`, `tables.c`, `module.c`,
  `avx2.c`, `avx512.c`, `gfni.c`, `neon.c`, `check.c`
- **GPL-3.0-or-later**: `sse2.c`, `ssse3.c`

Both license tracks are compatible with reffs's overall
`AGPL-3.0-or-later` posture via the "-or-later" upgrade path
(GPLv2-or-later can be relicensed to GPLv3, which is compatible
with AGPLv3).  The GPL-3.0-or-later files stay under their
stricter track; do not weaken their SPDX headers.

Andrea's top-level `COPYING` states: "The RAID library is
provided under the GPL-2.0-or-later License."  The two
GPL-3.0-or-later files (`sse2.c`, `ssse3.c`) appear to be
an upstream inconsistency; they were retained here at their
declared license to avoid rewriting Andrea's headers.

## Refreshing from upstream

Two-step:

1. `git -C /tmp clone --depth 1 https://github.com/amadvance/snapraid.git`
2. `cp /tmp/snapraid/raid/{RAID_FILES_ABOVE} lib/ec/snapraid-raid/`

Then update the pinned SHA at the top of this file, and re-run
`lib/ec/tests/snapraid_test`.  The vendored files carry Andrea's
original SPDX + copyright headers; the refresh script must
preserve them.

## Field polynomial

Andrea's `raid.h:42-46` supports two GF(2^8) primitive polynomials
gated on a compile-time `USE_RAID_AES` define:

- **default (`USE_RAID_AES` undefined) -> `RAID_POLY = 0x1d`**
  (x^8 + x^4 + x^3 + x^2 + 1) -- the standard Linux md RAID
  polynomial and the same field reffs's ec_rs already uses.
- opt-in `-DUSE_RAID_AES -> RAID_POLY = 0x1b` (x^8 + x^4 + x^3 +
  x + 1) -- the AES polynomial.  Enables Intel GFNI's
  `vgf2p8mulb` single-instruction GF(2^8) multiply on Ice Lake
  and later CPUs, materially faster on GFNI hardware.  **Trade-
  off: parity generated with one polynomial cannot be decoded
  with the other**, and 0x1b is incompatible with every other
  RAID / erasure implementation on the planet.

reffs uses the default 0x1d.  We do NOT define `USE_RAID_AES`
anywhere; the `#ifdef USE_RAID_AES` hits in `tables.c` and
`gfni.c` are just Andrea's alternate branches sitting cold.

Any future move to 0x1b would need to be spelled out in the
IETF draft's on-wire encoding definition AND would break
interop with every non-reffs FFv2 implementation, so this is a
"never on-wire" flag for us.  If we ever want the GFNI single-
instruction perf, it stays inside a client-only side channel
that isn't part of the wire format.

## Build-scoped UBSAN carve-out

`Makefile.am` adds `-fno-sanitize=alignment` to this subtree's
`AM_CFLAGS`.  Reason: Andrea's CPUID probe at `cpu.h:44-54` casts
a `char[13]` vendor-string buffer to `(uint32_t *)` and writes
three `uint32_t` chunks at 1-byte-aligned offsets.  x86 handles
unaligned stores natively so this works fine in production but is
strict-UB per the C standard; without the flag, UBSAN reports the
alignment error every time `raid_init()` runs.

The carve-out is deliberately narrow -- only the `alignment`
check is disabled.  All other UBSAN checks (undefined-shift,
signed-overflow, null-deref, ...) stay on for Andrea's code, so
we still get the checks that catch real bugs.

The alternative would be to patch `cpu.h` to use `memcpy()` for
the vendor-string write.  That would be cleaner from a UB-standard
standpoint but modifies vendored code -- see the "Refreshing from
upstream" section: a refresh has to preserve Andrea's SPDX +
copyright headers AND leave the code byte-identical, so build-
scoped fixes go here and code-scoped fixes go upstream.

## Reffs-side additions

Files in this directory that are NOT from upstream:

- `NOTICE.md` (this file) — AGPL-3.0-or-later.
- `Makefile.am` — AGPL-3.0-or-later; builds the vendored
  sources as a `noinst_LTLIBRARIES` convenience library.

## Cross-references

- `~/Documents/reffs-docs/snapraid-evaluation.md` — the
  license + adoption evaluation that led to this vendor.
- `~/Documents/reffs-docs/christoph.md` Ask 3 — the reference
  Christoph named at 2026-07-22.
