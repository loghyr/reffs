# Mojette at 24 KiB Physical Shards Through the PS Pipeline

## Context

`EC_SHARD_SIZE` in `lib/nfs4/ps/ec_pipeline.c` is hardcoded at 4 KiB
since the io_uring large-message workaround.  That cap is now stale:
the underlying io_uring fragmentation bug was fixed on 2026-03-27
(see `.claude/goals.md`).  The cap remains because nothing has
exercised the path at a different shard size end-to-end.

`deploy/sanity/run-ps-demo-codecs.sh` documents the gap directly:
Mojette through the PS uses a 16 KiB payload (4 KiB physical shards)
because larger-shard Mojette through the PS pipeline + CHUNK_FINALIZE
hasn't been verified.  Issue #147 closed the FINALIZE total_blocks
ceiling-divide arithmetic; this slice closes the per-shard-size
parameterisation needed to actually run the path.

The codec itself is now pinned at 24 KiB by
`lib/ec/tests/mojette_codec_test.c` (commit `a24c5ba882c5`).  The
remaining work is the pipeline plumbing.

## Geometry at 24 KiB Mojette-sys k=4 m=2

| Shard            | Size      | Notes                                |
|------------------|-----------|--------------------------------------|
| data[0..3]       | 24576 B   | grid rows (P=3072 columns)           |
| parity[0] (p=2)  | 49168 B   | `2*(P-1) + (k-1) + 1` = 6146 elem    |
| parity[1] (p=3)  | 73736 B   | `3*(P-1) + (k-1) + 1` = 9217 elem    |
| ds_stride        | 73736 B   | `max(shard sizes)` per ec_pipeline   |

With `chunk_sz = 4096` (the MDS-issued `ffm_striping_unit_size`):

| Shard            | wsz       | blocks written    | nblk on read     |
|------------------|-----------|-------------------|------------------|
| data[0..3]       | 24576     | 6                 | 6                |
| parity[0]        | 49168     | 13 (`ceil(49168/4096)`) | 13         |
| parity[1]        | 73736     | 19 (`ceil(73736/4096)`) | 19         |
| per-stripe stride| -         | 19                | -                |

For a 96 KiB payload: nstripes = `96 KiB / (k * 24 KiB)` = 1.
total_blocks = `1 * 19` = 19.  FINALIZE and COMMIT both cover
blocks 0..18 on every mirror (matching #147's ceiling-divide).

This is the same arithmetic shape as the 4 KiB case (#147 fix), just
with bigger numbers.  No new arithmetic; a new test pin verifies the
24 KiB numbers explicitly.

## Plan

### Slice A: parameterize shard_size in ec_pipeline + ec_demo

**Goal**: make `shard_size` a per-call argument to
`ec_write_codec` / `ec_read_codec`, default 4096 via thin wrappers
to keep all existing callers unchanged.

**Changes**:

1. `lib/nfs4/ps/ec_pipeline.c`
   - Drop the file-local `EC_SHARD_SIZE` macro.
   - Add `size_t shard_size` parameter to `ec_write_codec`/`ec_read_codec`.
     Internal `EC_SHARD_SIZE` symbol becomes the function argument.
   - For the io_uring per-RPC cap (`plain_write`/`plain_read`),
     keep a separate const (e.g. `PLAIN_RPC_CHUNK = 4096`) so the
     PS plain-path stays the same -- it uses a single-mirror NFSv3
     write loop, not the EC pipeline, and isn't part of this slice.

2. `lib/nfs4/client/ec_client.h`
   - `ec_write_codec` / `ec_read_codec` signatures grow `shard_size`.
   - `ec_write` / `ec_read` (the back-compat RS-default wrappers)
     keep their old signatures and pass 4096.

3. `tools/ec_demo.c`
   - Add `--shard-size` CLI flag, default 4096.
   - Thread through to `ec_write_codec` / `ec_read_codec`.
   - `cmd_write`, `cmd_read`, `cmd_verify` gain `shard_size` parameter.

4. `lib/nfs4/ps/tests/ec_pipeline_stripe_test.c`
   - Add `test_finalize_total_blocks_mojette_24k`: pin the 24 KiB
     Mojette numbers (ds_stride=73736, chunk_sz=4096, per-stripe
     stride=19, nstripes=1, total_blocks=19).

**Test impact**:
- Existing tests pass unchanged (back-compat wrappers preserve the
  old 4 KiB behaviour).
- New stripe-math test pins the 24 KiB geometry explicitly.

### Slice B: exercise PS-demo Mojette at 24 KiB end-to-end

**Goal**: enable the previously-gated Mojette 24 KiB path in
`deploy/sanity/run-ps-demo-codecs.sh`.

**Changes**:

1. `deploy/sanity/run-ps-demo-codecs.sh`
   - Drop the `PAYLOAD_MJ_SIZE = 16K` carve-out.
   - Use the same 96 KiB payload as the other codecs.
   - Pass `--shard-size 24576` for the Mojette case.
   - Update the gating comment.

2. Smoke-test on the docker sanity rig (or dreamer if a local rig
   is available); confirm round-trip byte-identical on read.

**Test impact**:
- This is a deploy-script change.  Verification is end-to-end
  rather than unit-pinned -- the sanity matrix either passes or
  fails on real PS-MDS-DS containers.

## Risks

- **`ec_write_codec` signature change is API-visible.**  Internal
  to the project (no external consumers), but every caller in
  `ec_demo.c` must be updated in the same commit.  Back-compat
  `ec_write` / `ec_read` wrappers keep the RS happy-path callers
  free of touch.
- **Per-stripe block stride at chunk_sz=4096 is 19 -- larger than
  any prior test case.**  Same arithmetic as #147; the new
  stripe-math test is the unit pin.  If the production path
  diverges from the unit test math, the divergence is the bug.
- **The PS pipeline encodes whole stripes in memory.**  At 24 KiB
  shards k=4 m=2 the per-stripe footprint is ~316 KiB (k*24 +
  m*max_proj).  For a 1 MB payload nstripes=11, total transient
  alloc ~3.5 MB.  Trivial.

## Deferred / NOT_NOW_BROWN_COW

- Lifting `ffm_striping_unit_size` past 4096 on the MDS side.  Not
  needed for the Mojette 24 KiB demo; the variable-shard arithmetic
  already handles `wsz / chunk_sz != integer` (see #147).
- Variable shard size per codec instance (e.g. RS at one size,
  Mojette at another in the same export).  Out of scope; one shard
  size per call.
- Reed-Solomon at 24 KiB shards through the PS demo.  Same plumbing
  works trivially since RS shard sizes are uniform; add to the
  matrix in slice B if it composes cleanly.

## Verification

1. `make check` passes; new `test_finalize_total_blocks_mojette_24k`
   passes.
2. `make -f Makefile.reffs style && ... license` clean.
3. `tools/ec_demo --codec mojette-sys --k 4 --m 2 --shard-size 24576`
   round-trips a 96 KiB payload byte-identically against a combined
   reffs.
4. PS-demo sanity matrix passes the Mojette case at the 24 KiB
   geometry on the dreamer container rig.
