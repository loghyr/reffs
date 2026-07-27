<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Vendored: Intel ISA-L erasure code (portable-C subset)

Vendor copy of the portable-C subset of Intel ISA-L's
`erasure_code/`, snapshotted 2026-07-24 from upstream
`8b7e3b28a2b7b816483d3f9e80162e11cebeaa70` (2026-07-22).

Corresponds to `FFV2_ENCODING_ISA_L_RS = 0x9` in the FFv2
encoding menu.

## Upstream

- Project: **Intel(R) Intelligent Storage Acceleration Library**
- Repository: <https://github.com/intel/isa-l>
- Subtree: `erasure_code/` (portable-C only)
- License: **BSD-3-Clause** (most permissive of any encoding
  candidate on the menu)
- Copyright: Intel Corporation 2011-2024 -- upstream `LICENSE`
  is reproduced verbatim in this directory.
- Pinned commit: `8b7e3b28a2b7b816483d3f9e80162e11cebeaa70`
  (2026-07-22 `erasure_code: optimize SVE/SVE2 dot product...`)

## What landed (Slice 7.1)

**Portable-C only** -- 5 files, no NASM required:

- `erasure_code/ec_base.c` -- `_base` implementations of every
  `gf_*` / `ec_*` primitive (`ec_init_tables_base`,
  `gf_gen_rs_matrix`, `gf_gen_cauchy1_matrix`,
  `gf_invert_matrix`, `ec_encode_data_base`, and the
  supporting `gf_vect_*_base` primitives).
- `erasure_code/ec_base_aliases.c` -- thin wrappers that
  expose the public-API symbols (`ec_encode_data`,
  `ec_init_tables`, ...) as aliases for the `_base`
  implementations.  This is the file that makes
  portable-C-only builds tenable without the runtime
  multibinary dispatcher.
- `erasure_code/ec_base.h` -- the GF(2^8) log/antilog tables
  (4.5 kLOC of const data).
- `include/erasure_code.h` -- the public API header.
- `include/gf_vect_mul.h` -- dependency header.

## What was deliberately NOT vendored (Slice 7.3 scope)

- `erasure_code/ec_highlevel_func.c` -- x86 SIMD dispatch
  wrappers.  Redundant once `ec_base_aliases.c` is in the
  build.
- `erasure_code/ec_multibinary.asm` -- runtime dispatcher.
- Every `erasure_code/gf_*_avx*.asm` / `gf_*_sse.asm` /
  `gf_*_gfni.asm` -- x86 SIMD sources (~40 files).
- `erasure_code/aarch64/*.S` -- NEON / SVE sources.
- Any NASM tooling requirement.

Slice 7.3 will re-vendor the SIMD tree and add the
`configure.ac` `AC_CHECK_PROG(NASM)` gate.  Design open --
either drop the entire SIMD tree in one go and gate the whole
build path on NASM, or land per-architecture (NEON first,
since NASM is not required for aarch64 assembly).

## Field

- GF(2^8), primitive polynomial 0x1d.  Same field as reffs's
  RS_VANDERMONDE (0x4), SNAPRAID_CAUCHY (0x6), and
  LINUX_MD_RAID (0x8).
- **Matrix construction is NOT wire-compat with 0x4 or 0x6:**
  ISA-L's `gf_gen_rs_matrix` uses generator `2^(i*j)`,
  reffs's rs.c uses `2^(2^i * j)`.  ISA-L's
  `gf_gen_cauchy1_matrix` uses `1 / (i XOR j)` with a
  different point-choice than SnapRAID.  Slice 7.2's tests
  will verify this cross-check numerically.

## Wrapper (Slice 7.2)

`lib/ec/isa_l.c` -- reffs's `ec_encoding` vtable over
`ec_encode_data*` + `gf_gen_rs_matrix` + `gf_invert_matrix()`.
Shape mirrors `lib/ec/snapraid.c`:

- `ec_isa_l_create(int k, int m)` returns a `struct
  ec_encoding *`.  Caps: k in [1, 254], m in [1, 254 - k].
- Default matrix: **Vandermonde**
  (`gf_gen_rs_matrix`).  Cauchy variant deferrable to a
  follow-up `ec_isa_l_create_cauchy(k, m)` if the WG asks
  for it.
- `encode()` -- prep `g_tbls` via `ec_init_tables()`, then
  `ec_encode_data(size, k, m, g_tbls, data_ptrs, parity_ptrs)`.
- `decode()` -- rebuild decoding matrix from k rows of the
  encode matrix corresponding to present shards, invert via
  `gf_invert_matrix`, apply.

## Refresh procedure

```
git clone --depth 1 https://github.com/intel/isa-l.git /tmp/isa-l
cp /tmp/isa-l/erasure_code/{ec_base.c,ec_base.h,ec_base_aliases.c} \
   lib/ec/isa-l-erasure/erasure_code/
cp /tmp/isa-l/include/{erasure_code.h,gf_vect_mul.h} \
   lib/ec/isa-l-erasure/include/
cp /tmp/isa-l/LICENSE lib/ec/isa-l-erasure/
```

Bump the pinned SHA above and re-run `make check`.

## Cross-references

- <https://github.com/intel/isa-l/tree/master/erasure_code>
- `lib/ec/snapraid-raid/NOTICE.md` -- vendor discipline
  template
- `lib/ec/linux-md-raid/NOTICE.md` -- portable-C-first
  vendor pattern (same rationale)
- `~/Documents/reffs-docs/ffv2-encoding-menu.md`
  FFV2_ENCODING_ISA_L_RS (proposed 0x9)
