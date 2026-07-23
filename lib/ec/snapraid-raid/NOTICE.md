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
