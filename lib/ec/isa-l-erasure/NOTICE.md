<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Vendored (planned): Intel ISA-L erasure code

This directory will hold a vendor copy of the `erasure_code/`
subtree from Intel's ISA-L (Intelligent Storage Acceleration
Library).

Corresponds to `FFV2_ENCODING_ISA_L_RS = 0x9` in the encoding menu.

## Upstream

- Project: **Intel(R) Intelligent Storage Acceleration Library**
- Repository: <https://github.com/intel/isa-l>
- Subtree: `erasure_code/`
- License: **BSD-3-Clause** (most permissive of any encoding
  candidate on the menu)
- Copyright: Intel Corporation 2011-2024

## What lands (planned)

Full `erasure_code/` subtree (mirrors the SnapRAID vendor
pattern):

- **Portable C**: `ec_base.c`, `ec_base_aliases.c`,
  `ec_highlevel_func.c`, headers `ec_base.h`, `erasure_code.h`.
  Defines `ec_init_tables_base()`, `gf_gen_rs_matrix()`
  (Vandermonde with generator=2), `gf_gen_cauchy1_matrix()`
  (Cauchy 1/(i XOR j)), `gf_invert_matrix()`, and the
  `ec_encode_data_*` family.
- **NASM SIMD**: 40+ `.asm` files covering AVX / AVX-2 /
  AVX-512 with GFNI variants + SSE + aarch64 NEON.  Each
  `_gfni.asm` sibling is Intel Ice Lake+ / AMD Zen 4+ only.
- **Build machinery**: `Makefile.am` (upstream provides
  autotools already) + `ec_multibinary.asm` runtime
  dispatcher.

## Build-tooling note

Unlike Andrea's SnapRAID raid/ (which uses inline `asm
volatile` blocks the C compiler handles directly), ISA-L's
SIMD implementations are separate `.asm` files that require
NASM (Netwide Assembler) to assemble.  Adding NASM as a build
dep is a new tooling requirement for reffs:

- Fedora / RHEL: `dnf install nasm`
- Debian / Ubuntu: `apt install nasm`
- macOS: `brew install nasm`

`configure.ac` will need an `AC_CHECK_PROG([NASM], [nasm], ...)`
+ conditional so the ISA-L build path is skipped cleanly when
NASM is absent (or falls back to portable-C only).

## Wrapper (planned)

`lib/ec/isa_l.c` -- reffs's `ec_encoding` vtable over ISA-L's
`ec_encode_data*` + `gf_gen_*` + `gf_invert_matrix()`.  Shape
mirrors `lib/ec/snapraid.c`:

- `ec_isa_l_create(int k, int m)` returns a `struct
  ec_encoding *`.  Caps: k in [1, 254], m in [1, 254 - k].
- Choice of matrix construction: expose `gf_gen_rs_matrix`
  (Vandermonde, default) OR `gf_gen_cauchy1_matrix` (Cauchy).
  If the WG picks one variant to standardize, the wrapper
  can either pin to that or expose both via
  `ec_isa_l_create_rs(k, m)` + `ec_isa_l_create_cauchy(k, m)`.
  Design open until the WG discussion converges.
- `encode()` -- prep `g_tbls` via `ec_init_tables()`, then
  `ec_encode_data(size, k, m, g_tbls, data_ptrs,
  parity_ptrs)`.
- `decode()` -- build decode-matrix from k rows of the encode
  matrix corresponding to present shards, invert via
  `gf_invert_matrix`, apply.  Reference impl at
  `erasure_code/erasure_code_test.c` for the pattern.
- Global init: none required beyond the multibinary
  dispatcher initializing itself lazily.

## Wire-compat (from scoping 2026-07-24)

- Same field as RS_VANDERMONDE / SNAPRAID_CAUCHY / LINUX_MD_RAID
  (GF(2^8) with 0x1d).
- NEITHER matrix (Vandermonde nor Cauchy) matches our
  RS_VANDERMONDE (0x4) at the coefficient level -- ISA-L uses
  a different generator sequence.
- ISA-L Cauchy also does NOT match SnapRAID's Cauchy -- different
  x_i, y_j point choice.
- So ISA-L needs its own enum value even though it uses the
  same field as three other candidates.

## Refresh procedure (once populated)

`git clone --depth 1 https://github.com/intel/isa-l.git /tmp/isa-l`
+ `cp /tmp/isa-l/erasure_code/* lib/ec/isa-l-erasure/`.  Pin the
upstream commit SHA at the top of this file for each refresh.

## Status: SCOPING (2026-07-24)

Placeholder for the actual vendor.  Next slice: vendor the
sources + wire up autotools + `AC_CHECK_PROG(NASM)` + wrapper +
tests.  See `~/Documents/reffs-docs/ffv2-encoding-menu.md`
FFV2_ENCODING_ISA_L_RS for the menu-level scope.

## Cross-references

- <https://github.com/intel/isa-l/tree/master/erasure_code>
- `lib/ec/snapraid-raid/NOTICE.md` -- vendor discipline
  template
- `~/Documents/reffs-docs/ffv2-encoding-menu.md`
  FFV2_ENCODING_ISA_L_RS (proposed 0x9)
