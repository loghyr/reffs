<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Chunk-Collision Track 1b: ec_demo --offset / --length

## Goal

Add partial-range writes to `ec_demo` so the chunk-collision
harness can drive **sub-range** contention against the same
MDS file -- the surface that produces real byte-level interleave
and a **deterministic** post-state, per
`.claude/design/chunk-collision-validation.md` (Track 1b row).

This unblocks four chunk-collision sub-cases:

| Sub-case | Layout |
|----------|--------|
| `coll_t1b_disjoint` | 4 writers, non-overlapping byte ranges |
| `coll_t1b_overlap` | 4 writers, overlapping ranges, deterministic seed-per-rank |
| `coll_t1b_full_chunk_split` | 2 writers, each on a different half of one chunk |
| `coll_t1b_subchunk_interleave` | 2 writers, alternating 1 KiB stripes inside one 4 KiB block |

## Why this is small

The PS Phase 4b slice already shipped a per-stripe write
primitive `ec_write_stripe_with_file` (and its read mirror
`ec_read_stripe_with_file` for RMW prefix/suffix).  See
`lib/nfs4/client/ec_client.h:794-837`.  These functions:

- Acquire a LAYOUTGET / LAYOUTRETURN per stripe.
- Encode + CHUNK_WRITE / FINALIZE / COMMIT exactly that stripe's
  blocks.
- Compute DS-side block offsets from the file-level stripe
  number, so two concurrent writers on disjoint stripes do not
  clobber each other's bytes.

Track 1b just composes these primitives behind a new CLI surface
on the existing `write` / `verify` subcommands.

## CLI surface

Reuse the existing `write` and `verify` subcommands; add two new
long options:

```
  --offset OFF     Starting byte offset for partial-range I/O (default 0)
  --length LEN     Number of bytes to write/verify (default: full --input length)
```

Semantics:

- `--offset` and `--length` are **byte** ranges in the logical
  (decoded) file, not stripe or shard units.
- The bytes written are `input[0 .. LEN)`; they land at the MDS
  file's bytes `[OFF .. OFF+LEN)`.  This pairs naturally with the
  harness: each writer generates a `LEN`-byte payload distinct
  from every other writer's, then writes it into its assigned
  MDS range with `--offset $RANK_OFF --length $RANK_LEN`.
- `--length 0` (the default) means "use the entire input file
  length", so `--offset 0` alone (default) recovers today's
  full-file behaviour bit-for-bit.
- The range must satisfy `LEN <= input_file_size`.  Setting
  `--length` larger than the input is rejected with an error
  message (caller mismatch).
- For `verify`, the range is read back from the MDS bytes
  `[OFF .. OFF+LEN)` and compared byte-for-byte against
  `input[0 .. LEN)`.  Bytes outside the verify range are not
  read.

Both options are accepted on `write` and `verify` only.  The
plain `put` / `get` / `check` path is **not** extended -- it
goes through MDS InBand I/O, not CHUNK ops, so it does not
exercise the chunk-collision surface this slice cares about.

## Implementation

### Two new functions in lib/nfs4/client/ec_client.h

Mirror of `ec_write_codec` / `ec_read_codec` (`verify` reuses
`ec_read_codec`'s by-FH variant under the hood), with explicit
`offset` and `length`:

```c
/*
 * Partial-range variant of ec_write_codec.  Writes
 * data[0 .. length) into the MDS file at byte offset `offset`.
 * Computes the stripe span covered by [offset, offset+length),
 * walks fully-dirty stripes via ec_write_stripe_with_file, and
 * RMW-merges the prefix / suffix stripes via
 * ec_read_stripe_with_file + overwrite + ec_write_stripe_with_file
 * when the range does not align to stripe boundaries.
 *
 * `offset + length` MUST NOT exceed the MDS file's existing size
 * for the prefix / suffix RMW path to succeed (sparse RMW is
 * NOT_NOW_BROWN_COW per ec_read_stripe_with_file).
 */
int ec_write_codec_range(struct mds_session *ms, const char *path,
                         const uint8_t *data, size_t length,
                         uint64_t offset, int k, int m,
                         enum ec_codec_type codec_type,
                         layouttype4 layout_type,
                         size_t shard_size);

/*
 * Partial-range variant of ec_read_codec.  Reads `length` bytes
 * from the MDS file starting at `offset` into `buf`.
 */
int ec_read_codec_range(struct mds_session *ms, const char *path,
                        uint8_t *buf, size_t length, uint64_t offset,
                        int k, int m, enum ec_codec_type codec_type,
                        layouttype4 layout_type,
                        uint64_t skip_ds_mask, size_t shard_size);
```

Internally each one:

1. OPENs the MDS file (via `mds_file_open`).
2. Computes:
   - `stripe_data_size = k * shard_size`
   - `first_stripe = offset / stripe_data_size`
   - `last_stripe  = (offset + length - 1) / stripe_data_size`
3. For the **prefix stripe** (if `offset % stripe_data_size != 0`):
   - `ec_read_stripe_with_file(first_stripe, ...)` to fetch.
   - Overwrite the dirty range in the stripe buffer with bytes
     from `data`.
   - `ec_write_stripe_with_file(first_stripe, ...)` to flush.
4. For each **interior fully-dirty stripe** (`first_stripe+1 .. last_stripe-1`):
   - Copy `data[stripe_offset .. stripe_offset+stripe_data_size)`
     into a stripe buffer.
   - `ec_write_stripe_with_file(stripe_no, ...)` to flush.
5. For the **suffix stripe** (if last stripe is partial, i.e.
   `(offset + length) % stripe_data_size != 0` AND the suffix
   stripe is not the same as the prefix stripe):
   - Same RMW pattern as step 3.
6. Special case: prefix and suffix are the same stripe (range
   contained in one stripe) -- one RMW round-trip total.
7. CLOSE the file.

For `ec_read_codec_range`: walk the covered stripes via
`ec_read_stripe_with_file`, then `memcpy` only the
`[offset, offset+length)` byte slice into `buf`.

### ec_demo.c changes

- Two new `long_options` entries: `--offset` -> `'O'`,
  `--length` -> `'L'`.
- Two new `getopt_long` cases.
- Bounds check at the top of `cmd_write` / `cmd_verify`: if
  `offset` is 0 AND `length` is 0, fall through to the existing
  full-file path.  Otherwise call the new range function.
- `cmd_verify` reads `[offset, offset+length)` and `memcmp`s
  against `input[offset .. offset+length)` from the local file.
- Help text mentions the two new options under `write` /
  `verify`.

### Test inventory

New unit tests in `lib/nfs4/client/tests/ec_range_test.c` (or
extend the existing PS Phase 4b test file if one exists):

| Test | Intent |
|------|--------|
| `test_range_aligned_single_stripe` | `offset=0, length=stripe_data`: writes one stripe; verify reads it back |
| `test_range_aligned_multi_stripe` | `offset=0, length=4*stripe_data`: walks 4 stripes; verify reads back |
| `test_range_prefix_unaligned` | `offset=shard/2, length=stripe_data`: RMW the prefix stripe, fully-write the interior stripe |
| `test_range_suffix_unaligned` | `offset=stripe_data, length=stripe_data/2`: RMW the suffix stripe only |
| `test_range_single_partial_stripe` | `offset=shard/4, length=shard/2`: prefix == suffix, one RMW round-trip |
| `test_range_two_writers_disjoint` | Writer A writes `[0, stripe_data)`, writer B writes `[stripe_data, 2*stripe_data)`; verify both ranges intact |
| `test_range_subchunk_interleave` | Writer A `[0, 1KiB)`, writer B `[1KiB, 2KiB)` on a 4 KiB block; verify byte-exact for each writer's range |

All tests use the combined-mode test harness (single reffsd
listening as MDS+DS+PS) so the LAYOUTGET / CHUNK ops fire
locally without docker setup.  Tests run in `make check` and
must complete in under 2 seconds each per standards.md.

### Harness changes

Extend `deploy/benchmark/run_chunk_collision.sh` (or add a
sibling `run_chunk_collision_t1b.sh` if the existing script is
too coupled to the full-file path):

- New `--mode disjoint|overlap|chunk-split|subchunk` option
  selecting the four sub-cases.
- Per sub-case, compute per-rank `OFFSET` / `LENGTH` and pass to
  each ec_demo writer.
- Verification phase: each writer calls `ec_demo verify --offset
  $RANK_OFF --length $RANK_LEN` against its own range.  Every
  writer's range must verify clean -- any mismatch is a
  correctness bug.
- For `coll_t1b_overlap`: define a deterministic
  byte -> rank mapping (lowest rank wins each overlap byte, or
  highest -- the choice is local to the harness as long as the
  expected bytes match what we assert against).

Each new sub-case is one short block in the harness; total
expected harness delta is ~100 LOC.

### Counter assertions

Reuse the existing `--inv1-report` flag.  Track 1b adds an
expected pattern:

- `sb_chunk_writes` is `>= 0`, exactly equal to the sum of stripes
  written across all sub-case writers (since each stripe is one
  CHUNK_WRITE per mirror).
- `sb_chunk_pending_displaced` is **zero** for `coll_t1b_disjoint`
  (no two writers touch the same block) and **positive** for
  `coll_t1b_overlap` and `coll_t1b_subchunk_interleave` (sub-shard
  RMW means concurrent PENDING blocks on the same offset).
- `sb_chunk_finalize_crc_fail` is **zero** for all four sub-cases
  (no CRC anomaly is expected).

The harness prints these deltas alongside the verify result.

## Test impact

This slice adds new code and new tests.  No existing test is
modified.  Specifically:

- `ec_write_codec` / `ec_read_codec` keep their full-file
  signatures.  ec_demo `write` with no `--offset` / `--length`
  still routes to them, bit-for-bit identical to today.
- `ec_write_stripe_with_file` / `ec_read_stripe_with_file` are
  already shipped (PS Phase 4b) and remain unchanged.  Their
  existing tests in `lib/nfs4/ps/tests/` (or wherever they live)
  are unaffected.
- `run_chunk_collision.sh` keeps the existing four Track 1
  sub-cases working; Track 1b cases are additive behind the new
  `--mode` flag.

## Deferred / NOT_NOW_BROWN_COW

- **Sparse RMW** (writing into a stripe that has no prior DS
  bytes).  `ec_read_stripe_with_file` documents this as out of
  scope; if the harness needs it, the Track 1b sub-cases will
  pre-fill the file with `ec_demo write` at full size first.
- **Range LAYOUTGET** (asking the MDS for a layout segment that
  covers only `[offset, offset+length)` rather than the entire
  file).  The current per-stripe primitives already do one
  LAYOUTGET per stripe; range-LAYOUTGET would consolidate that
  into one grant per range.  Performance optimisation, not
  correctness.
- **Concurrent prefix / suffix RMW** on the same partial stripe
  from two writers.  Today the RMW is non-atomic at the client
  level (read, modify locally, write).  Two writers on the same
  partial stripe will race in a last-FINALIZE-wins pattern; the
  harness's `coll_t1b_overlap` sub-case will surface this and
  the expected counter delta records that race (positive
  `sb_chunk_pending_displaced`).  Adding optimistic-concurrency
  retry to the RMW is a follow-up.

## RFC references

- RFC 8881 §18.36 (LAYOUTGET / LAYOUTRETURN / LAYOUTCOMMIT)
- draft-haynes-nfsv4-flexfiles-v2 sec-CHUNK_WRITE,
  sec-CHUNK_FINALIZE
