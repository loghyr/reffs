<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Vendored (planned): Linux md RAID-6 (P+Q double parity)

This directory will hold a vendor copy of the `lib/raid6/` subtree
from the Linux kernel, adapted for userspace compilation the same
way `lib/raid6/test/` in the kernel tree does it.

Corresponds to `FFV2_ENCODING_LINUX_MD_RAID = 0x8` in the encoding
menu -- **double parity only** (m=2).  Single-parity RAID-5 (m=1)
is already covered by `FFV2_ENCODING_XOR_PARITY = 0x7`, whose wire
format is identical to Linux md's P-only case; landing LINUX_MD_RAID
at m=1 would be redundant.

## Upstream

- Project: **Linux kernel md/raid6** library
- Path in kernel: `lib/raid6/` (plus `include/linux/raid/pq.h`)
- Copyright: H. Peter Anvin 2002 onward, GPL-2.0-or-later
- Local reference: `~/linux/lib/raid6/` (Tom's Linux checkout)

## What lands (planned)

Bare-minimum userspace-portable set, following the pattern of
kernel `lib/raid6/test/Makefile`:

- **Headers**: adapted `pq.h` (from `include/linux/raid/pq.h`) --
  minimal kernel-shim so the raid6 API compiles without `<linux/*>`
  includes.  Provides `raid6_call.gen()`, `raid6_2data_recov()`,
  `raid6_datap_recov()`.
- **Portable C**: `int.uc` + build-time awk expansion to
  `int1.c` through `int32.c` (per-unroll variants).  Or ship
  one pre-generated `int8.c` if we do not want the awk step.
- **Dispatch + table**: `algos.c` (adapt to strip
  `#ifdef __KERNEL__`), `mktables.c` (run at build to generate
  `tables.c`), `recov.c` (portable recovery).
- **SIMD** (deferred to a follow-up slice): `avx2.c`,
  `avx512.c`, `sse2.c`, `neon.c`, and their `recov_*.c` peers.
  Each of these depends on kernel-side arch-detection macros
  (`CONFIG_X86_64`, etc.) that the userspace test harness
  in `lib/raid6/test/Makefile` sets via `-DCONFIG_X86_64` on
  the compile command.  Vendor them the same way; document
  the arch-flag requirements in this file when they land.
- **Skipped**: kernel-only files (`raid5*.c`, `raid5-*.h` from
  `drivers/md/`) -- those handle stripe cache, disk state
  machine, resync workqueue, etc.  Not needed for the
  P+Q-generation encoding library.

## Wrapper (planned)

`lib/ec/linux_md.c` -- reffs's `ec_encoding` vtable over Linux
md's `raid6_call.gen()` + `raid6_2data_recov()` +
`raid6_datap_recov()`.  Shape mirrors `lib/ec/snapraid.c`:

- `ec_linux_md_create(int k)` returns a `struct ec_encoding *`
  with `ec_m = 2` (hard-coded; the encoding is double-parity
  by definition).  Rejects m != 2 at create time.
- `encode()` -- assemble the pointer array `dataptrs[k+2]`
  (data first, then P, Q), call `raid6_call.gen(k+2, size,
  dataptrs)`.
- `decode()` -- classify missing shards: {none, one data, one
  parity, two data, one-data-one-parity, two parity}.
  Dispatch to `raid6_2data_recov` / `raid6_datap_recov`
  accordingly.  Two-parity-both-missing = re-run `gen`.
  More than two = `-EIO`.
- `destroy()` -- free the encoding struct.
- Global init: `raid6_select_algo()` in a `pthread_once` so
  the dispatch table is populated before any encode/decode.

## Wire-compat verification (owed at land time)

Cross-check that the P+Q bytes produced by `raid6_call.gen(k+2,
...)` match SnapRAID's first-two-Cauchy-row output byte-for-byte
at the same (k, m=2).  Christoph asserted at IETF-126 that they
DO match; the sanity check is a one-shot unit test in the
`linux_md_test.c` that comes with the wrapper.

## Refresh procedure (once populated)

Two-step, same as the SnapRAID vendor:

1. `cp ~/linux/lib/raid6/{RAID6_FILES} lib/ec/linux-md-raid/`
2. `cp ~/linux/include/linux/raid/pq.h lib/ec/linux-md-raid/`

Then re-run `lib/ec/tests/linux_md_test`.  Pin the Linux kernel
SHA at the top of this file for each refresh; document any
adaptations away from upstream (kernel-shim additions in `pq.h`,
`__KERNEL__` fences stripped from `algos.c`) so a future refresh
reviewer knows what to preserve.

## Status: SCOPING (2026-07-24)

This file is currently a scoping placeholder.  Next slice: actual
vendor of the portable-C sources + `pq.h` + wrapper.  See
`~/Documents/reffs-docs/ffv2-encoding-menu.md`
FFV2_ENCODING_LINUX_MD_RAID (proposed 0x8) for the menu-level
scope and the follow-ups map.

## Cross-references

- `~/linux/lib/raid6/` -- upstream source
- `~/linux/lib/raid6/test/Makefile` -- userspace-compile template
- `lib/ec/snapraid-raid/NOTICE.md` -- the vendor-discipline
  template this file follows
- `~/Documents/reffs-docs/ffv2-encoding-menu.md` -- menu entry
  for LINUX_MD_RAID
