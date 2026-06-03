<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Per-Export `default_coding` TOML Field

> **Plan-review pass 2026-06-03**: three BLOCKERs surfaced
> (constant value, runway interaction, cross-design
> constraint).  See "Plan-review findings applied" section
> at the end.  Changes integrated inline below; this header
> is the audit trail.

## Why

The MDS currently hard-codes `seg.ls_m = 0` at LAYOUTGET-time
layout-segment creation (`lib/nfs4/server/layout.c:1267`) and
uses `ls_k = nfiles` (line 1266) where `nfiles` is whatever
the runway managed to pop -- which silently degrades codec
geometry if the runway is short.  That selects
`FFV2_ENCODING_PASSTHROUGH` for every layout the MDS issues
for files created over a kernel mount (see
`lib/nfs4/server/layout.c:610-613`).

Wire protocol reference: FFv2 `ffm_coding_type` is defined in
`draft-haynes-nfsv4-flexfiles-v2` -- the user is the draft
author.  No NFSv4.2 protocol wire change in this slice;
`probe1_xdr.x` is internal.  ec_demo bypasses the
issue by passing `--codec rs/...` directly and ignoring the
wire `ffm_coding_type` (which is "advisory only" per the
comment at line 599-608).

For the WG-relevant Bucket 2 head-to-head (client EC vs
PS EC), the PS needs to receive an EC layout from the MDS --
no per-file negotiation mechanism exists yet
(`ffv2_layouthint4` is a NOT_NOW_BROWN_COW per the layout.c
comment).  Until the layouthint lands, the cheap interim is a
**per-export default codec** configured at the MDS:

- TOML `[[export]] default_coding = "rs:4+2"`
- Probe op `SB_SET_DEFAULT_CODING` for runtime modification
- Layout segments created on that export inherit the codec
  + (k, m) values, so any client (ec_demo, kernel, PS) gets
  the same coding type back from LAYOUTGET

This is the long-promised interim from the proxy-server design
(`.claude/design/proxy-server.md`) and the bucket-2 bench
design (`.claude/design/ps-encoder-bench-4variant-realnet.md`).
~50-100 LOC of production code + tests; no XDR change on the
NFSv4.2 wire (probe XDR only, which is internal).

## Tests first (per .claude/roles.md Planner discipline)

### Unit tests -- new

**`lib/config/tests/config_test.c`** (extend):

| Test | Intent |
|------|--------|
| `test_config_default_coding_passthrough` | `default_coding = "passthrough"` parses to (codec=PASSTHROUGH, k=stripe_count, m=0) |
| `test_config_default_coding_rs_4_2` | `default_coding = "rs:4+2"` parses to (codec=RS_VANDERMONDE, k=4, m=2) |
| `test_config_default_coding_mojette_sys_8_2` | `default_coding = "mojette-sys:8+2"` parses to (codec=MOJETTE_SYSTEMATIC, k=8, m=2) |
| `test_config_default_coding_invalid_codec` | `default_coding = "bogus:4+2"` -> parse error |
| `test_config_default_coding_invalid_format` | `default_coding = "rs"` (missing geometry) -> parse error; `"rs:4"` (missing m) -> parse error; `"rs:0+2"` (k=0) -> parse error |
| `test_config_default_coding_absent` | No `default_coding` field -> defaults to PASSTHROUGH with k = layout_width, m = 0 (today's behaviour) |
| `test_config_default_coding_max_k_m` | `"rs:128+64"` -> parse error (exceeds LAYOUT_SEG_MAX_FILES = 16 -- enforce sb cap) |

**`lib/fs/tests/sb_persistence_test.c`** (extend):

| Test | Intent |
|------|--------|
| `test_registry_default_coding_persisted` | Set default_coding=(RS, 4, 2), save, destroy, load -- coding matches |
| `test_registry_default_coding_absent_legacy` | Load a registry entry with sre_default_coding all-zero (legacy) -> sb default coding is PASSTHROUGH (today's behaviour) |

**`lib/nfs4/tests/`** (new file `layoutget_default_coding_test.c`):

| Test | Intent |
|------|--------|
| `test_layoutget_passthrough_default` | sb.default_coding = (PASSTHROUGH, k, 0) -> LAYOUTGET returns segment with ls_m=0 and ffm_coding_type=FFV2_ENCODING_PASSTHROUGH |
| `test_layoutget_rs_4_2_default` | sb.default_coding = (RS, 4, 2) -> LAYOUTGET returns ls_k=4, ls_m=2, ffm_coding_type=FFV2_ENCODING_RS_VANDERMONDE |
| `test_layoutget_mojette_sys_8_2_default` | sb.default_coding = (MOJETTE_SYSTEMATIC, 8, 2) -> ls_k=8, ls_m=2, ffm_coding_type=FFV2_ENCODING_MOJETTE_SYSTEMATIC |
| `test_layoutget_runway_target_drives_k_m` | sb.default_coding = (RS, 4, 2) target = 6.  Runway pops 6 FHs.  ls_k = 4 (from config), ls_m = 2 (from config), NOT ls_k = nfiles.  Catches the today's-code bug that conflates target with nfiles. |
| `test_layoutget_insufficient_dstores` | sb.default_coding wants k+m=10 but only 6 dstores available -> **NFS4ERR_LAYOUTUNAVAILABLE** (today's code silently uses ls_k=6, ls_m=0 -- this test pins the corrected behaviour) |
| `test_layoutget_dstore_count_drops` | default_coding set successfully when dstore_count=10, then one dstore goes offline (count=8 effective); a fresh LAYOUTGET with k+m=10 returns NFS4ERR_LAYOUTUNAVAILABLE, not a degraded layout |
| `test_layoutget_file_layout_rejects_ec_default` | sb.sb_layout_types includes SB_LAYOUT_FILE and default_coding = (RS, 4, 2) -> setter (not LAYOUTGET) refuses; cross-validation per `per-export-dstore.md` "file layouts = single DS" rule |

### Test impact on existing tests

| File | Impact |
|------|--------|
| `sb_lifecycle_test.c` | PASS -- default_coding is a new field, zero-initialized; lifecycle unaffected |
| `sb_mount_crossing_test.c` | PASS -- mount-crossing logic untouched |
| `sb_security_test.c` | PASS -- security flavors orthogonal |
| `sb_path_conflict_test.c` | PASS -- path-conflict logic untouched |
| `config_test.c` | **EXTEND** -- add tests above; existing `[[export]]` tests still pass |
| `sb_persistence_test.c` | **EXTEND** -- registry round-trip extended with new field |
| ec_demo bench tests | PASS -- ec_demo passes `--codec` directly; new field is opt-in via TOML |
| Track 1b chunk-collision tests | PASS -- those use ec_demo too |
| `bat_export_setup.sh` | PASS -- existing bat exports do not set `default_coding`; default behavior is unchanged |

### Functional tests

After the unit tests pass:

1. **Single-codec kernel mount**: configure MDS with
   `default_coding = "rs:4+2"`.  Restart MDS.  Kernel-mount
   MDS, write 1 MB.  Verify DS0..5 see the WRITE traffic (via
   `--op gather`).  Verify file reads back correctly.

2. **Restart preservation**: configure default_coding via TOML
   AND via probe op `SB_SET_DEFAULT_CODING`; restart MDS;
   verify both persist correctly (registry-loaded sbs keep the
   probe value; root sb re-reads from TOML).

3. **Mixed exports**: two `[[export]]` blocks with different
   `default_coding` settings; verify LAYOUTGET on a file in
   each returns the export-appropriate codec.

### CI tests

`ci_integration_test.sh`: add a variant that sets
`default_coding = "rs:4+2"` on the root export, mounts via
kernel client, clones the source repo to the mount, builds it,
md5sums configure.ac.  Exercises kernel-pNFS path with
real-EC writes end-to-end.

## State machine / persistence

No new state machine -- this is configuration that drives
LAYOUTGET-time decisions.

Persistence: extend `sb_registry_entry` with
`sre_default_coding` (codec, k, m as fixed-size fields).
SB_REGISTRY_VERSION stays at 1 (no deployed persistent
storage per CLAUDE.md); zero-initialized = PASSTHROUGH legacy.

## Admin interface (per .claude/roles.md Planner #9)

Three admin paths:

1. **TOML**: `[[export]] default_coding = "rs:4+2"` at server
   boot.  Applied to root sb only (per sb-registry-v3
   `[[export]]`-is-root-only rule).  Non-root sbs come from
   probe.

2. **Probe op `SB_SET_DEFAULT_CODING`** (new op 28):

   ```
   struct SB_SET_DEFAULT_CODING1args {
       unsigned hyper            scda_id;
       probe_coding_spec1        scda_coding;
   };
   struct probe_coding_spec1 {
       unsigned int  pcs_codec_type;  /* PASSTHROUGH/RS/MOJ-SYS/MOJ-NONSYS */
       unsigned int  pcs_k;
       unsigned int  pcs_m;
   };
   ```

   Validation:
   - `pcs_k > 0`
   - `pcs_k + pcs_m <= LAYOUT_SEG_MAX_FILES` (32, per
     `lib/include/reffs/layout_segment.h:28`)
   - codec known
   - **If `sb_layout_types & SB_LAYOUT_FILE`**: refuse any
     non-PASSTHROUGH codec OR any `m > 0` -- file layouts
     require single-DS per the per-export-dstore.md design
     rule "file layouts = single DS per export".  Return
     `PROBE1ERR_INVAL`.

3. **Probe op `SB_GET_DEFAULT_CODING`** (new op 29): takes
   `sb_id`, returns `probe_coding_spec1`.  Symmetric with the
   existing `SB_SET_STRIPE_UNIT` / `SB_SET_CHECKSUM_ALGORITHM`
   shape in `probe1_server.c`.  Integration test in step 9
   uses this directly rather than walking `sb-get` output.

4. **Probe `SB_GET` / `SB_LIST`**: extend `probe_sb_info1`
   with `psi_default_coding` so admins can inspect current
   setting.

Both setter and getter persist to the registry.

## Implementation steps (dependency-ordered)

| # | Step | Files | LOC | Tests |
|---|------|-------|-----|-------|
| 1 | `struct reffs_coding_spec` + constants in `lib/include/reffs/ec.h` (or a new `coding_spec.h`) | header | ~30 | n/a |
| 2 | TOML parser for `default_coding` string + 5 test cases | `lib/config/config.c`, `config_test.c` | ~60 | 5 unit tests |
| 3 | Add `sb_default_coding` field to `struct super_block` + setter `super_block_set_default_coding()` | `lib/include/reffs/super_block.h`, `lib/fs/super_block.c` | ~30 | mechanical (covered by tests below) |
| 4 | Registry persistence: add `sre_default_coding` to `sb_registry_entry`; save/load round-trip + 2 test cases | `lib/include/reffs/sb_registry.h`, `lib/fs/sb_registry.c`, `sb_persistence_test.c` | ~40 | 2 unit tests |
| 5 | LAYOUTGET dispatch (real shape per plan-review B2): (a) compute `target = sb->sb_default_coding.k + m` upfront (before `lines 1190-1195` runway-pop count derivation); (b) if `nfiles < target` after runway pop, return `NFS4ERR_LAYOUTUNAVAILABLE` instead of accepting degraded `nfiles` as `ls_k`; (c) set `ls_k = sb->sb_default_coding.k` and `ls_m = sb->sb_default_coding.m` from config, NOT from `nfiles`; (d) drive `ffm_coding_type` selection at `layout.c:610-613` from `sb_default_coding.codec` instead of the `ls_m==0` heuristic.  Step-5 tests can call `super_block_set_default_coding()` directly without depending on probe wiring (steps 6+); flag this in the test setup. | `lib/nfs4/server/layout.c`, `layoutget_default_coding_test.c` | ~80 (was 50; +30 LOC for runway-target work) | 6 unit tests (passthrough, rs 4+2, moj-sys 8+2, runway-target, insufficient, dstore-drop) |
| 6 | Probe XDR: `probe_coding_spec1`, `SB_SET_DEFAULT_CODING1args/res`, op 28, extend `probe_sb_info1` with `psi_default_coding` | `lib/xdr/probe1_xdr.x` | ~30 | XDR-only; tests via C/Python clients below |
| 7 | Probe server handlers `probe1_op_sb_set_default_coding` (op 28) AND `probe1_op_sb_get_default_coding` (op 29) + validation (incl. file-layout-single-DS check per B3) | `lib/probe1/probe1_server.c` | ~80 (was 50; +30 for getter + file-layout check) | one Python integration test |
| 8 | Probe C client wrappers (setter + getter) + CLI flag | `lib/probe1/probe1_client.c`, `src/probe1_client.c` | ~60 (was 40; +20 for getter) | covered by integration test |
| 9 | Probe Python client wrappers (setter + getter) + CLI subcommands | `scripts/reffs/probe_client.py.in`, `scripts/reffs-probe.py.in`, `scripts/test_sb_probe.py` | ~40 (was 30; +10 for getter) | extend Python integration test (uses sb-get-default-coding) |
| 10 | Bench config: set `default_coding = "rs:4+2"` in `deploy/benchmark/mds.toml` (optional default for variant d) | `mds.toml` | ~1 line | functional test |

Total: ~470 LOC production (was ~400) + ~170 LOC tests (was
~150); **~12 working days** (was ~10; +2 day buffer per
plan-review N1 for runway-target interaction risk and the
follow-on getter/file-layout work).

## Persistence inventory addition

Per `rocksdb-persistence.md` Server-Wide table -- the registry
already has `superblocks.registry`; the new field threads into
that existing path.  RocksDB namespace DB (when active) needs
`sre_default_coding` in its `registry:sbreg:<id>` value too;
that update happens automatically because the value is the
same `sb_registry_entry` struct.

## Coding spec format

The TOML string format is `"<codec>:<k>+<m>"`:

- `"passthrough"` (alone, no `:<k>+<m>`) -- legacy / explicit
  PASSTHROUGH; k = `ss_layout_width` (server-wide), m = 0
- `"rs:K+M"` -- Reed-Solomon Vandermonde, k=K, m=M
- `"mojette-sys:K+M"` -- Mojette systematic
- `"mojette-nonsys:K+M"` -- Mojette non-systematic

K must be in [1, LAYOUT_SEG_MAX_FILES]; M must be in
[0, LAYOUT_SEG_MAX_FILES - K].  k+m total cannot exceed
LAYOUT_SEG_MAX_FILES = 32 (per
`lib/include/reffs/layout_segment.h:28`; **not** 16 as an
earlier draft of this design said).

Implementation note: pair the TOML parser's codec-name lookup
with a `_Static_assert` linking the parser's string table to
the `enum ec_codec_type` values in `lib/include/reffs/ec.h`,
so a future codec addition cannot silently desync.

## Backward compatibility

- TOML without `default_coding` -> PASSTHROUGH with k =
  `server_state.ss_layout_width` (the server-wide knob, NOT
  per-sb; default 6 for RS(4,2)).  Today's behavior,
  unchanged.  A per-sb override of `layout_width` is a
  separate follow-up not in this slice.
- Registry entries from before this slice -> zero
  `sre_default_coding` interpreted as PASSTHROUGH (legacy).
  `srh_version` stays at 1; do NOT bump (per CLAUDE.md
  no-deployed-storage rule).  `test_registry_default_coding_absent_legacy`
  also asserts `srh_version == 1`.
- ec_demo `--codec rs/mojette-sys/...` still drives codec
  client-side regardless of MDS default (ec_demo ignores the
  layout's coding type per existing convention)
- PS / `ec_pipeline`: when the MDS issues a non-PASSTHROUGH
  default-coding layout, the PS receives it at LAYOUTGET
  (proxy-server design §"Client WRITE on a proxied file").
  PS's data path then encodes accordingly -- the entire point
  of this slice for the Bucket 2 bench.  No PS code change
  is needed; the PS already honours the layout's coding type.
- bat_export_setup.sh, all existing TOML examples -> no change
  needed

## Deferred / NOT_NOW_BROWN_COW

- **`fattr4_layout_hint` SETATTR (RFC 8881 attr 63)** -- the
  per-file mechanism that supersedes per-export default.  This
  slice is the interim; the layout-hint work proceeds
  independently and superscribes default_coding on a per-file
  basis when it lands.
- **Per-byte-range coding** (different codecs for different
  parts of a file).  Out of scope.
- **Runway pool that pre-allocates k+m FHs per default_coding**
  -- the current runway is per-dstore; for k+m allocation it
  walks the dstore list at LAYOUTGET time.  May need a runway
  warmup pass for non-default codecs in steady state; not in
  this slice.

## Reviewer agent gating

Per CLAUDE.md gating:
- **Cross-layer boundary addition** (`lib/config/` ->
  `lib/fs/super_block.h` -> `lib/nfs4/server/layout.c` ->
  `lib/probe1/`)
- **Slice over ~150 LOC** of substantive change (~400 LOC total)
- **Persistent-state change** (sb_registry_entry grows)

These trigger the reviewer agent per CLAUDE.md
"Review before commit (gated)".  Run `/review` BEFORE the
landing commit, not after.  Per-step commits on a topic branch
can stay inline-reviewed; the final main-bound commit gets the
agent.

Probe XDR changes go through `probe1_xdr.x` which is internal
(client + server ship together) -- not a BLOCKER under the
reviewer's "XDR file review" rule 9.

## Reference

- `lib/nfs4/server/layout.c:610-613` -- the
  `ls_m`-keyed PASSTHROUGH / RS_VANDERMONDE selection
- `lib/nfs4/server/layout.c:1261-1272` -- the layout_segment
  struct construction that hardcodes `.ls_m = 0` and uses
  `.ls_k = nfiles` (the runway pop count, not a per-sb target)
- `lib/nfs4/server/layout.c:599-608` -- the NOT_NOW_BROWN_COW
  comment naming `fattr4_layout_hint` as the long-term mechanism
- `lib/include/reffs/layout_segment.h:28` -- `LAYOUT_SEG_MAX_FILES = 32`
- `.claude/design/proxy-server.md` -- where the interim was
  first proposed
- `.claude/design/ps-encoder-bench-4variant-realnet.md` --
  consumer of this prereq (prereq #2 in the dep table)
- `.claude/design/per-export-dstore.md` -- file-layout-single-DS
  cross-validation constraint (this design's B3 fix)

## Plan-review findings applied (2026-06-03)

A planner-completeness review against `.claude/roles.md`
responsibilities #1-10 surfaced three BLOCKERs and five
WARNINGs before any code was written.  All are integrated
above.  The diff vs the original v1 design:

| Tag | Finding | Where applied |
|-----|---------|---------------|
| B1 | `LAYOUT_SEG_MAX_FILES` is 32, not 16 | Coding-spec section, Reference list |
| B2 | Step 5 conflates target with `nfiles`; today's code silently degrades codec geometry when runway is short | Step 5 table row rewritten (~80 LOC, was ~50); 2 new test cases (`test_layoutget_runway_target_drives_k_m`, `test_layoutget_dstore_count_drops`); `test_layoutget_insufficient_dstores` reframed from "existing behavior" to "corrected behavior" |
| B3 | Missing cross-validation: file layouts require single DS per `per-export-dstore.md` | Admin Interface op 28 validation; new test `test_layoutget_file_layout_rejects_ec_default`; Step 7 LOC bumped +30 for the file-layout check |
| W1 | `layout_width` is server-wide (`ss_layout_width`), not per-sb | Backward-compatibility section corrected |
| W2 | Add `SB_GET_DEFAULT_CODING` op for symmetry + integration testing | New op 29 in Admin Interface; Steps 7-9 LOC bumped (+30/+20/+10) |
| W4 | PS / `ec_pipeline` interaction unmentioned | Backward-compatibility section names the PS path |
| W5 | Test impact should cover registry header version unchanged | `test_registry_default_coding_absent_legacy` extended to assert `srh_version == 1` |
| N1 | +2 day buffer realistic | Total estimate now 12 working days (was 10) |
| N2 | `_Static_assert` linking parser to ec.h codec enum | Coding-spec format section |
| N3 | RFC compliance citation | "Why" section adds draft-haynes-nfsv4-flexfiles-v2 reference |
| N4 | `test_layoutget_dstore_count_drops` edge case | Added to LAYOUTGET unit test set |

Verdict: APPROVE-WITH-CHANGES -> all changes applied.
Estimate revised from ~400 LOC + ~150 LOC tests / 10 days
to ~470 LOC + ~170 LOC tests / 12 days.  Plan is now ready
for implementation -- the cheap moment for plan feedback is
closed.
