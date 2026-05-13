<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# PS Phase 4b: Client WRITE through pipeline -- per-stripe RMW

Active-slice plan for the second half of `proxy-server.md` Phase 4.
Predecessor: `proxy-server-phase4a.md` (whole-file buffer, COMMIT-flush).
This slice ships the spec-compliant write path: per-stripe dirty
tracking, partial-stripe RMW, and the verifier composition that
together unblock chunk-collision-validation Track 2 (IOR `-F 0`
shared file).

## Status (planning, 2026-05-12)

Phase 4a left three classes of incorrectness on the table:

| Problem | 4a behaviour | 4b fix |
|---------|--------------|--------|
| Multi-writer shared file (distinct stateids, same FH) | Second COMMIT zeros out stripes the first writer touched (whole-buffer encode) | Per-stripe dirty bitmap; commit only writes stripes the local buffer actually touched |
| Partial-stripe WRITE | Whole-buffer encode writes zeros into the stripe's untouched bytes | Read missing prefix/suffix from DS, merge in buffer bytes, encode whole stripe |
| MDS restart between WRITE and COMMIT | PS-listener verifier hides the change; bytes silently lost (Risk #3a from 4a) | Mix upstream MDS verifier into the PS verifier; client sees mismatch on next COMMIT |
| COMMIT range request | Flushes entire buffered prefix regardless of `args->offset/count` | Honour the requested range, intersected with dirty stripes |
| `stable = FILE_SYNC4` / `DATA_SYNC4` | Reply with `committed = UNSTABLE4` regardless (spec-permitted downgrade) | Synchronous flush on WRITE if client asks; reply with the requested level when flush succeeds |

The shipped Phase 4a code paths (`ps_proxy_pipeline_write` /
`_commit` / `_close`, the buffer table, the quiesce protocol) are
reused.  4b replaces the *encode* step at flush time and grows the
buffer structure with per-stripe dirty tracking.  No XDR change to
NFSv4.2.  No on-disk format change.

## Goal

Wire spec-compliant client WRITE on proxy-SB files end-to-end so
that:

- IOR `-F 0 -W -R -C` from N MPI ranks (one PS per rank) produces
  a file whose bytes match each rank's writes in each rank's
  exclusive byte range (chunk-collision-validation Track 2).
- A single-rank partial-stripe write (e.g., `dd if=/dev/urandom
  of=ps-mount/f bs=1024 seek=7`) produces a file whose first 7 KiB
  reads back as the existing data and bytes 7K-8K read back as the
  written bytes -- *not* zeros in the rest of the stripe.
- A client requesting `FILE_SYNC4` on WRITE observes
  `committed = FILE_SYNC4` and a verifier valid for COMMIT against
  the same listener generation; the bytes are durable on the DSes
  before the WRITE reply.
- An MDS restart between WRITE and COMMIT surfaces to the client
  as a verifier mismatch on COMMIT, triggering a clean client-side
  rewrite (the standard NFS recovery path), not silent data loss.

This closes Risk #1 (4b is the chunk-collision Track 2 gate),
Risk #3a (MDS-verifier mix), Risk #4 (multi-writer shared file),
and the DATA_SYNC4/FILE_SYNC4 deviation from 4a.

## Tests first

All tests live in `lib/nfs4/ps/tests/ps_write_buffer_rmw_test.c`
(NEW) and `lib/nfs4/ps/tests/ps_proxy_pipeline_rmw_test.c` (NEW).
Existing 4a tests must still pass without modification, with the
exception of `test_concurrent_writes_distinct_stateids_documented_loss`
which is **deleted and replaced** by
`test_concurrent_writes_distinct_stateids_disjoint_stripes_ok` (see
table below) -- that flip is the 4b acceptance criterion.

### Group A: dirty-stripe bitmap on the buffer

| Test | Intent |
|------|--------|
| `test_dirty_bitmap_initial_all_clean` | A fresh buffer has zero dirty bits. |
| `test_dirty_bitmap_full_stripe_write_sets_one_bit` | A WRITE covering exactly stripe `[k*shard, (k+1)*shard)` sets exactly bit `k` and the partial-mask for `k` is "full". |
| `test_dirty_bitmap_partial_stripe_write_sets_partial_mask` | WRITE of 1 KiB at offset 0 sets stripe 0's bit + partial mask `[0, 1KiB)`. |
| `test_dirty_bitmap_cross_stripe_write_sets_two_bits` | WRITE spanning end of stripe 0 + start of stripe 1 sets both bits with correct partial masks on each. |
| `test_dirty_bitmap_overwrite_same_stripe_partial_widens_mask` | Two writes to disjoint sub-ranges of stripe 0 leave both ranges covered in the mask; the union is recorded. |
| `test_dirty_bitmap_full_stripe_overwrite_clears_partial_mask` | After a partial-stripe write, a follow-up full-stripe write to the same stripe collapses the partial mask to "full" (no RMW needed at flush). |
| `test_dirty_bitmap_high_water_independent_of_dirty` | Sparse write at high offset bumps `pwb_high_water` but only sets the bit for the stripe actually touched -- untouched stripes between low and high water stay clean. |

### Group B: per-stripe RMW at COMMIT

| Test | Intent |
|------|--------|
| `test_commit_only_full_stripes_no_chunk_read` | Buffer with only full-stripe dirty bits triggers zero CHUNK_READ; only CHUNK_WRITE for dirty stripes is issued. |
| `test_commit_partial_stripe_reads_existing` | Buffer with one partial-stripe dirty bit triggers CHUNK_READ for that stripe before CHUNK_WRITE; the post-merge bytes match new-bytes-in-mask + old-bytes-elsewhere. |
| `test_commit_partial_stripe_decode_uses_quorum` | CHUNK_READ for the RMW prefix succeeds with `k` of `k+m` shards (the rest unreachable); decode reconstructs the missing bytes. |
| `test_commit_skips_clean_stripes` | Buffer with `pwb_high_water = 8 MiB` but only stripes 3 and 7 dirty: COMMIT issues CHUNK_WRITE for stripes 3 and 7 only. |
| `test_commit_dirty_stripe_overwrites_zeros` | Pin the 4a-broken multi-writer case: two `(stateid, fh)` buffers, distinct stateids, dirty disjoint stripes, both COMMIT; verify each writer's bytes survive in its own stripes (no zero-overwrite). |
| `test_commit_returns_unstable_verifier` | Same as 4a -- the WRITE/COMMIT verifier still carries the listener identity. |
| `test_commit_clears_dirty_bits_on_success` | After successful flush, all dirty bits are cleared (buffer ready for re-COMMIT no-op). |
| `test_commit_keeps_dirty_bits_on_partial_failure` | If CHUNK_WRITE fails on a subset of dirty stripes, the bits for the failed stripes stay set; success-stripe bits clear; COMMIT returns NFS4ERR_IO; client retry flushes only the failed remainder. |
| `test_commit_range_intersects_dirty` | COMMIT with `args->offset = 0`, `args->count = stripe_size` and dirty bits {0, 5}: only stripe 0 is flushed; stripe 5 remains dirty until a later wider-range COMMIT. |
| `test_commit_range_zero_count_flushes_all` | `args->count = 0` is the "commit everything" sentinel per RFC 8881 S18.3.4 (a sample of real servers also honour this); all dirty stripes flush. |

### Group C: MDS-verifier mix

| Test | Intent |
|------|--------|
| `test_listener_verf_combines_mds_and_pls` | `nfs4_write_verf()` (or the new `ps_compose_write_verf`) folds the MDS-returned verifier byte-string into the PS-listener verifier; both halves contribute to the output 8 bytes. |
| `test_listener_verf_change_on_mds_restart` | First COMMIT captures MDS-verifier V1; simulate MDS restart (capture V2 ≠ V1) on the next codec invocation; the composed verifier returned for subsequent WRITEs changes. |
| `test_commit_returns_listener_verf_v1_then_v2` | Sequenced flush across an MDS-verifier change: first flush returns composed(listener, V1); second flush returns composed(listener, V2); a client comparing the two sees a mismatch and rewrites. |
| `test_listener_verf_mds_unknown_falls_back` | If the MDS reply does not carry a verifier (legacy or error path), the composed verf is `(listener || zero_mds)` -- documented; deterministic; does NOT block the response. |

### Group D: stable-flag honouring

| Test | Intent |
|------|--------|
| `test_write_unstable_buffers_only` | `stable = UNSTABLE4` -- pure 4a behaviour: buffer + reply with UNSTABLE4 verifier. |
| `test_write_file_sync_flushes_inline` | `stable = FILE_SYNC4` -- the WRITE handler invokes the pipeline flush of all dirty stripes (which now include this write) before replying; reply has `committed = FILE_SYNC4`. |
| `test_write_data_sync_flushes_inline` | `stable = DATA_SYNC4` -- same as FILE_SYNC4 (reffs does not distinguish data vs metadata sync on the DS side). |
| `test_write_file_sync_failure_returns_io` | Synchronous flush fails on `k > m` DSes mid-write; WRITE returns NFS4ERR_IO; the dirty bits for the just-written range stay set so a client retry can re-flush. |
| `test_write_file_sync_other_buffer_unaffected` | Synchronous flush only touches *this* WRITE's stripes; concurrent buffered writes for the same stateid on a *different* range stay buffered. |

### Group E: collision-counter observability (regression for chunk-collision)

| Test | Intent |
|------|--------|
| `test_multi_ps_disjoint_stripes_no_collisions` | Two simulated PS clientids COMMIT disjoint stripes of the same upstream FH; DS-side `cs_pending_displaced` stays at zero (no collision because the per-stripe writes never overlap). |
| `test_multi_ps_overlap_stripe_increments_displaced` | Two PSes both flush stripe 5 (same byte range); DS-side `cs_pending_displaced` increments at least once (last-writer-wins is documented POSIX behaviour; the counter just confirms contention happened). |

### Test impact on existing tests

| File | Impact |
|------|--------|
| `lib/nfs4/ps/tests/ps_proxy_pipeline_write_test.c` (4a) | **DELETE** `test_concurrent_writes_distinct_stateids_documented_loss`; replace with `test_commit_dirty_stripe_overwrites_zeros` (Group B) which asserts the *correct* multi-writer behaviour.  Other 4a tests still pass unchanged. |
| `lib/nfs4/ps/tests/ps_write_buffer_test.c` (4a) | PASS unchanged -- 4a's whitebox lifecycle tests don't probe the dirty bitmap or the encode step. |
| `lib/backends/tests/proxy_data_test.c` (4a) | PASS unchanged. |
| All other `make check` tests | PASS unchanged. |

**Functional test** (`scripts/ci_ps_phase4b_test.sh`, NEW): IOR
`-F 0 -W -R -C` across two simulated ranks (two PSes,
single-machine), expecting both ranks' bytes to survive end-to-end.
Bench-integration gating is deferred to the bench-integration arc
(same NOT_NOW_BROWN_COW as 4a step 10).

## State / data structures

Extend `struct ps_write_buffer` (the 4a struct) with dirty-stripe
tracking:

```c
/* Stripe coverage geometry, snapshot at first WRITE on this buffer.
 * Encoded geometry comes from the file's layout (or the listener
 * default if no layout cached yet).  Once set, never changes -- a
 * geometry change forces a buffer drop + NFS4ERR_STALE on next
 * COMMIT.  The post-Phase-3 layout cache makes this a hash lookup,
 * not an RPC. */
struct ps_write_buffer_geom {
    uint32_t pwbg_k;            /* data shards */
    uint32_t pwbg_m;            /* parity shards */
    uint32_t pwbg_shard_size;   /* bytes per data shard */
    uint32_t pwbg_stripe_size;  /* k * shard_size */
    layouttype4 pwbg_layout_type; /* LAYOUT4_FLEX_FILES_V2 today */
    ec_codec_type_t pwbg_codec; /* EC_CODEC_RS today */
};

/* Per-stripe dirty state.  Allocated lazily as stripes are touched.
 * Indexed by stripe number = offset / stripe_size. */
struct ps_dirty_stripe {
    uint32_t pds_stripe_no;     /* hash key */
    uint8_t *pds_partial_mask;  /* NULL if fully dirty; else bitmap, one bit per shard_size/8 bytes */
    uint32_t pds_partial_mask_bits; /* bit count when allocated */
    struct cds_lfht_node pds_ht_node;
};

struct ps_write_buffer {
    /* ...existing 4a fields... */

    /* 4b additions */
    struct ps_write_buffer_geom pwb_geom;       /* snapshot at first WRITE */
    bool                        pwb_geom_set;   /* false until first WRITE */
    struct cds_lfht           *pwb_dirty_ht;    /* stripe_no -> ps_dirty_stripe */
    uint8_t                    pwb_mds_verf[NFS4_VERIFIER_SIZE]; /* last MDS verf seen */
    bool                       pwb_mds_verf_set;
};
```

`pwb_dirty_ht` is keyed by stripe number; entries are added on
WRITE under `pwb_mutex` and walked during flush.  The hash table
is `cds_lfht` for size-flexibility (sparse writes), not for
concurrency -- all dirty-bitmap mutations happen under `pwb_mutex`
(leaf-most, same as 4a).  No urcu_ref refcount on
`ps_dirty_stripe`: the table is owned by the buffer and lives and
dies with it.  RCU read-side is still used for the walk so the
"iterate under rcu_read_lock" pattern from
patterns/ref-counting.md Rule 6 holds; the ref discipline is
"buffer ref held by caller covers all stripe entries".

### Why a hash table, not a bitmap

A linear bitmap "bit per stripe" sized to `pwb_high_water /
stripe_size` would be simpler.  For dense write patterns (IOR
single rank writing 256 MiB) the bitmap is tiny (256 MiB / 16 KiB =
16 K bits = 2 KiB).  But sparse writes (`dd seek=1G` at the end of
a 1 TiB file) would need a 1 TiB / 16 KiB = 64 Mbit = 8 MiB
bitmap.  The hash table grows to the *number of touched stripes*,
not the file's extent.  For the workloads we care about (chunk-
collision Track 2 + partial-stripe RMW) the touched stripe count
stays in the hundreds.

If profiling later shows the per-stripe `ps_dirty_stripe` malloc
dominates, swap to an interval tree or to a hierarchical bitmap.
Not in 4b scope.

### Partial mask granularity

The partial mask granularity is "bytes per shard" wide: one bit
per shard-aligned region.  For the default `shard_size = 4 KiB`
and `k = 4`, a partial mask has at most 4 bits.  A WRITE of 1 KiB
at offset 0 in a stripe marks bit 0 of the partial mask "dirty"
because it touches the first shard; bits 1-3 stay clean.

Why shard-granularity, not byte-granularity?  Erasure coding
recomputes parity from all `k` data shards together.  If any byte
of a shard is dirty, the entire shard must be re-encoded from
"new-bytes-in-buffer || old-bytes-read-from-DS".  Finer than
shard-level adds bookkeeping cost for no fidelity gain at the
encode step.

When a partial mask has all bits set (every shard touched), it
collapses to "full" -- the entry's `pds_partial_mask` pointer is
freed and set to NULL.  Callers check `pds_partial_mask == NULL`
to mean "fully dirty, no RMW read needed".

## RFC compliance

Per `draft-haynes-nfsv4-flexfiles-v2-proxy-server` and RFC 8881:

- **Partial-stripe RMW model.**  When a client writes a sub-stripe
  range, the PS reads the existing stripe shards via CHUNK_READ
  with the same layout stateid the WRITE-side flush will use,
  decodes to recover the stripe's plaintext, merges in the new
  bytes by shard, encodes via the registered codec, and writes
  back via CHUNK_WRITE+FINALIZE+COMMIT.  This matches the
  draft's section on partial writes.  When all `k` data shards are
  fully dirty the read step is skipped (encode-from-buffer).

- **Multi-writer shared file.**  Distinct stateids touching
  disjoint stripes is now correct -- each `(stateid, fh)` buffer
  flushes only its own dirty stripes.  Overlapping-stripe
  contention between writers is "last writer wins per stripe",
  which matches POSIX `pwrite(2)` semantics for unsynchronized
  concurrent writers.  Atomicity guarantees apply within a single
  WRITE call (RFC 8881 S18.32.4); 4b does not strengthen that.

- **Composed write verifier.**  RFC 8881 S18.32.4 says the verifier
  identifies the server boot epoch.  For PS, the relevant boot
  epoch is the *combination* of (PS listener generation,
  upstream MDS verifier) -- both must match between WRITE and
  COMMIT for the bytes to be considered durable.  The PS-listener
  half tracks the PS's own buffer survival; the MDS-verifier half
  tracks whether the bytes that actually reached the DSes are
  still considered durable by the MDS.  Composition is a non-
  cryptographic mix (XOR of the 8-byte listener verf with the
  first 8 bytes of the MDS verf, padded if shorter); collisions
  are tolerable because they only cause a missed-mismatch
  (false-clean), which degrades to "wait for the next COMMIT" --
  not a correctness violation.  Documented as a deviation in
  Risks.

- **Stable flag honouring.**  Per RFC 8881 S18.32 the server MAY
  downgrade FILE_SYNC4 to UNSTABLE4 with a verifier; 4a took that
  liberty.  4b honours FILE_SYNC4 / DATA_SYNC4 when the client
  asks, by issuing the per-stripe flush synchronously inside the
  WRITE handler before reply.  UNSTABLE4 retains the 4a deferred-
  flush path.  The `committed` field in the reply matches what
  was actually achieved on the wire: FILE_SYNC4 only when the
  flush succeeded.

- **COMMIT range honouring.**  4a flushed the entire buffered
  prefix; spec permits this but it is wasteful for chunked clients.
  4b restricts the flush to dirty stripes whose stripe range
  intersects `[args->offset, args->offset + args->count)` (or to
  all dirty stripes if `args->count == 0`).  Stripes outside the
  range stay dirty and flush on a later COMMIT.

## State machines

### Per-stripe RMW flush

The 4a `ps_proxy_pipeline_commit` shim is restructured so the
"encode the whole buffer" step becomes "iterate dirty stripes,
RMW each".  Pseudocode for one stripe:

```
flush_stripe(buffer, stripe_no, stripe_geom, layout, creds):
    /* stripe byte range in the buffer's coordinate system */
    base = stripe_no * stripe_geom.stripe_size
    end  = base + stripe_geom.stripe_size

    dirty = lookup(buffer.pwb_dirty_ht, stripe_no)
    assert(dirty)         /* we wouldn't be flushing if clean */

    /* assemble the encode input: k data shards */
    if (dirty.pds_partial_mask == NULL):
        /* fully dirty -- read directly from buffer */
        encode_input = buffer.pwb_data + base
    else:
        /* RMW: read missing shards from DS, merge with buffer */
        encode_input = malloc(stripe_geom.stripe_size)
        /* For each of k shards: */
        for shard in 0..k:
            shard_off = base + shard * stripe_geom.shard_size
            if dirty.pds_partial_mask bit shard set:
                memcpy(encode_input + shard*shard_size,
                       buffer.pwb_data + shard_off,
                       shard_size)
            else:
                /* Read this shard's data from the DS via
                 * CHUNK_READ on the existing layout.  Quorum:
                 * k-of-(k+m) shards readable; reconstruct if
                 * needed via the codec's decode. */
                ret = ec_read_stripe_shard(ms, mf,
                                           stripe_no, shard,
                                           encode_input +
                                              shard*shard_size,
                                           creds)
                if ret < 0:
                    free(encode_input)
                    return ret

    /* Encode + write via the same per-stripe primitive
     * Phase 4a used, but driven by stripe_no rather than
     * "whole file". */
    ret = ec_write_stripe_with_file(ms, mf, stripe_no,
                                    encode_input,
                                    stripe_geom.k, stripe_geom.m,
                                    stripe_geom.codec,
                                    stripe_geom.layout_type,
                                    stripe_geom.shard_size,
                                    creds,
                                    &mds_verf_out)
    if encode_input is not buffer.pwb_data + base:
        free(encode_input)

    if (ret == 0):
        /* Capture the MDS verifier for the composed-verf mix */
        if mds_verf_out_set:
            memcpy(buffer.pwb_mds_verf, mds_verf_out,
                   NFS4_VERIFIER_SIZE)
            buffer.pwb_mds_verf_set = true

    return ret
```

The outer `ps_proxy_pipeline_commit` walks the dirty hash table
(under `pwb_mutex`), intersects each stripe with the COMMIT-range
window, and calls `flush_stripe()` per intersecting stripe.  On
success per stripe, the stripe entry is removed from the dirty
table.  On failure of any stripe, COMMIT returns NFS4ERR_IO with
the successful stripes' bits cleared and the failed ones still
dirty for client retry.

### WRITE-time dirty mark

WRITE handler in `ps_proxy_pipeline_write`:

```
on WRITE arrival:
    /* ...4a path: lookup-or-allocate buffer, take refs, lock mutex... */
    if (!buffer.pwb_geom_set):
        snapshot geometry from layout cache or listener defaults
        buffer.pwb_geom_set = true

    /* Copy bytes into buffer (4a behaviour). */
    memcpy(buffer.pwb_data + offset, args.data, count)
    buffer.pwb_high_water = max(buffer.pwb_high_water, offset + count)

    /* Mark stripes dirty.  Iterate stripes from
     * (offset / stripe_size) to ((offset+count-1) / stripe_size). */
    for stripe_no in covered_stripes:
        stripe_base = stripe_no * stripe_size
        stripe_end  = stripe_base + stripe_size
        write_lo    = max(offset, stripe_base)
        write_hi    = min(offset + count, stripe_end)

        dirty = lookup_or_alloc(buffer.pwb_dirty_ht, stripe_no)
        if write_lo == stripe_base and write_hi == stripe_end:
            /* Full stripe -- collapse partial mask */
            free(dirty.pds_partial_mask)
            dirty.pds_partial_mask = NULL
        else:
            /* Partial -- ensure mask exists, set shard bits */
            if dirty.pds_partial_mask == NULL and
                  dirty was newly allocated:
                /* fresh entry, partial write -- alloc mask */
                alloc_partial_mask(dirty, k)
            elif dirty.pds_partial_mask == NULL:
                /* Already fully dirty -- a partial write doesn't
                 * demote it.  Leave as full. */
                pass
            else:
                set_shard_bits(dirty.pds_partial_mask,
                               write_lo - stripe_base,
                               write_hi - stripe_base,
                               shard_size)
                if all bits set:
                    free(dirty.pds_partial_mask)
                    dirty.pds_partial_mask = NULL

    /* 4a path: unlock mutex, drop find ref, reply with
     * UNSTABLE4 + composed verifier */
    reply: count=count, committed=UNSTABLE4,
           verf=compose_verf(buffer)
```

### FILE_SYNC4 / DATA_SYNC4 inline flush

If `args.stable != UNSTABLE4`, the WRITE handler does the dirty-
marking step above, then immediately walks the just-touched
stripes and runs `flush_stripe()` for each before reply.  On
success, reply with `committed = args.stable` and the composed
verifier; on failure, reply with NFS4ERR_IO (dirty bits stay set
for any unflushed stripes -- the client retries).

The inline flush holds `pwb_mutex` across the per-stripe RPCs,
serialising other WRITEs to the same buffer.  This is acceptable
because (a) RFC 8881 already serialises ops per `(stateid, fh)`
at the client kernel level (see 4a's COMMIT empty-buffer
analysis), and (b) FILE_SYNC4 callers are not the latency-
sensitive path.  Profiling may later motivate a "drop the lock
around the RPC, take it back to update dirty bits" pattern, but
that's an optimisation, not a correctness change.

### Composed verifier

`ps_compose_write_verf(buffer, listener)` returns an 8-byte
verifier:

```
compose_verf(buffer, listener):
    base = listener.pls_listener_verf       /* 8 bytes from 4a */
    if (!buffer.pwb_mds_verf_set):
        return base                          /* no MDS verf yet */
    /* XOR the first 8 bytes of the MDS verf into the listener
     * verf; this is non-cryptographic, but the verifier comparison
     * is "equal vs not-equal", so XOR-mix is sufficient.
     * Documented in RFC compliance bullet above. */
    return base XOR buffer.pwb_mds_verf[0..8]
```

Listener teardown does **not** invalidate composed verifiers --
the listener verf incorporates `pls_boot_gen`, which already
changes on listener restart per 4a.  A buffer that survives across
a listener restart (it shouldn't -- 4a's gen check drops stale
buffers) would compose a verifier the client cannot match anyway.

## Persistence

None.  Same as 4a: write buffers + dirty bitmaps live in PS RAM
and are lost on listener restart or PS process exit.  The bigger
buffer (dirty stripe table + RMW intermediate state) is a small
absolute increase; the 1 GiB `REFFS_PS_WRITE_BUFFER_MAX` cap from
4a stays unchanged.

The `pwb_mds_verf` field is intentionally transient.  A future
upstream-restart-aware design that survives PS restart would need
to persist the (stateid, fh, MDS verf) tuple -- explicitly NOT in
scope.

## Security model

Unchanged from 4a:

- GSS compounds refused with NFS4ERR_WRONGSEC; AUTH_NONE refused
  at the op handler.
- Forwarded creds (`compound->c_ap`) threaded through both the
  CHUNK_READ (RMW prefix) and CHUNK_WRITE (flush) compounds.
- Per-listener buffer table; one client's buffers are not visible
  to another.
- Buffer cap (`REFFS_PS_WRITE_BUFFER_MAX`) plus the new
  per-buffer dirty hash table; the dirty table is bounded by
  number of touched stripes which is bounded by buffer size /
  stripe size.  No new DoS surface.

The RMW path issues CHUNK_READ for stripes not fully overwritten.
A client cannot use this to extract bytes it never had access to:
the layout stateid the PS uses for CHUNK_READ is the same layout
stateid the upstream MDS issued *to the PS's pipeline*.  The PS
already authenticated the client for WRITE on this FH; the RMW
prefix read is the PS's own internal read of bytes it is about to
overwrite.

## Deferred / NOT_NOW_BROWN_COW

- **Async RMW pipeline.**  The COMMIT path is still synchronous --
  the client thread blocks while RMW reads + encodes + writes
  proceed.  An async dispatch with completion polling is a perf
  follow-on; correctness path stays sync.
- **Cross-stateid coordination on overlapping stripes.**  Two
  writers' last-writer-wins-per-stripe is POSIX-correct but a
  client doing strict ordering (`write_a; write_b; commit; expect
  both`) gets undefined ordering for overlapping ranges.  A
  per-stripe lock-via-MDS protocol op (`CHUNK_LOCK`?  layout-
  recall?) would close this but requires a draft change.  Defer.
- **CHUNK_LOCK / CHUNK_UNLOCK wire ops.**  Today stubs returning
  NFS4ERR_NOTSUPP; would let PSes serialise RMW on contended
  stripes explicitly rather than rely on POSIX semantics.  Defer.
- **Per-stripe MDS verifier capture.**  The composed verifier
  captures the *last* MDS verifier seen on any flush of this
  buffer.  A precise design would carry one verifier per stripe.
  Defer (the "did MDS restart anywhere" signal is sufficient for
  the client-rewrite recovery path).
- **Hierarchical / interval-tree dirty representation.**  The
  per-stripe `cds_lfht` is the simple choice; profiling may
  motivate a more compact representation for very sparse writes.
- **Direct-to-disk stripe-aligned WRITE path** (also deferred in
  4a).  A WRITE whose offset + count is exactly stripe-aligned
  could bypass buffering entirely and encode-and-stripe inline.
  Defer.

## Admin interface

Probe op `ps-write-buffer-stats` (4a) gains two additional
per-listener counters:

- `dirty_stripes_total` -- sum of dirty entries across all live
  buffers.  Operator signal that "stuff is buffered and waiting".
- `rmw_reads_total` -- count of CHUNK_READ issued for partial-
  stripe RMW prefixes.  Workload health indicator (high values
  on a non-RMW-heavy workload mean clients are doing unexpected
  partial-stripe patterns).
- `rmw_read_failures_total` -- CHUNK_READ failures during RMW;
  surfaces DS degradation that the WRITE-side counters miss.

No new probe op.  No XDR change to nfsv42.x.  The probe XDR
extension is wire-additive (internal-only, client+server ship
together -- same rule as 4a's probe extension).

## Implementation steps

Each step is **tests-first** per `roles.md`: write the failing
unit test from the "Tests first" table, then the production code,
then verify the test passes before moving on.  Per-slice quality
gates (`license`, `style`, `make check`, reviewer when the slice
triggers the workflow rule) run BEFORE every commit.

The slices are designed to flip a single semantic at a time so
each can be committed and reverted in isolation.

### 4b.1: dirty-stripe bitmap data structures + WRITE-time marking

Tests unblocked: Group A (all 7 tests).

- Add `ps_write_buffer_geom`, `ps_dirty_stripe`, the `pwb_dirty_ht`
  hash table, and the partial-mask helpers (set/clear/test-shard-
  bits, allocate, free) to `ps_write_buffer.c` / `_internal.h`.
- Extend `ps_proxy_pipeline_write` to snapshot geometry on first
  WRITE and mark dirty stripes after the buffer memcpy.
- Geometry snapshot reads from the per-listener layout cache; if
  no cache hit yet, lazy-fetch the layout (existing pipeline
  helper) and store.  Subsequent WRITEs on the same buffer reuse
  the cached geometry.
- **No flush-side change yet** -- COMMIT still calls 4a's whole-
  buffer encode path.  This slice ships pure data-structure
  plumbing.

### 4b.2: per-stripe flush primitive (full-stripe only)

Tests unblocked: `test_commit_only_full_stripes_no_chunk_read`,
`test_commit_skips_clean_stripes`,
`test_commit_clears_dirty_bits_on_success`,
`test_commit_keeps_dirty_bits_on_partial_failure`.

- Add `ec_write_stripe_with_file(ms, mf, stripe_no, stripe_bytes,
  k, m, codec, layout_type, shard_size, creds, *mds_verf_out)`
  in `lib/nfs4/ps/ec_pipeline.c`.  Sibling to 4a's
  `ec_write_codec_with_file`, but takes a single stripe and
  returns the MDS verifier.
- Replace the body of `ps_proxy_pipeline_commit` with: walk the
  dirty hash table, call `ec_write_stripe_with_file` per dirty
  stripe whose `pds_partial_mask == NULL`, on success remove the
  stripe entry.  Stripes with non-NULL partial masks **return
  NFS4ERR_IO for now** (4b.3 will RMW them).  This slice flips
  the multi-writer-shared-file behaviour for fully-aligned
  workloads (IOR `-F 0 -W` with stripe-aligned ranks).

### 4b.3: partial-stripe RMW (read missing shards from DS)

Tests unblocked: `test_commit_partial_stripe_reads_existing`,
`test_commit_partial_stripe_decode_uses_quorum`,
`test_commit_dirty_stripe_overwrites_zeros`.

- Add `ec_read_stripe_shard(ms, mf, stripe_no, shard, dst,
  creds)` in `ec_pipeline.c`.  Reads the shard via CHUNK_READ
  from the layout's resolved mirrors; decodes via the codec's
  `decode` if quorum is below all-shards-readable.
- In `flush_stripe()`, when `pds_partial_mask != NULL`, allocate
  a stripe-sized scratch buffer, populate dirty shards from the
  PS buffer, clean shards from `ec_read_stripe_shard`, then
  encode + write back.
- Free the scratch buffer on the way out (success or failure).

### 4b.4: composed write verifier (MDS-verifier mix)

Tests unblocked: Group C (all 4 tests).

- Add `pwb_mds_verf` / `_set` to `ps_write_buffer`.
- Capture the MDS verifier from `ec_write_stripe_with_file`'s
  out-parameter into `pwb_mds_verf` after each successful flush.
- Replace 4a's `nfs4_write_verf()` call sites in
  `ps_proxy_pipeline_write` / `_commit` reply paths with
  `ps_compose_write_verf(buffer, listener)`.  The function lives
  in `ps_write_buffer.c`.
- Default for "buffer has never flushed yet" is `pls_listener_verf`
  unmodified (verifier is still valid; subsequent COMMIT will
  see the same listener verf and succeed).

### 4b.5: COMMIT range honouring

Tests unblocked: `test_commit_range_intersects_dirty`,
`test_commit_range_zero_count_flushes_all`.

- In `ps_proxy_pipeline_commit`, when iterating the dirty hash
  table, compute the byte range `[base, base+stripe_size)` for
  each stripe and skip it if it doesn't intersect
  `[args->offset, args->offset + args->count)`.
- `args->count == 0` means "commit everything" (RFC 8881 S18.3.4
  permissive interpretation, matched by Linux NFSv4 server).
- Stripes outside the range remain in `pwb_dirty_ht` for a later
  COMMIT.

### 4b.6: FILE_SYNC4 / DATA_SYNC4 inline flush on WRITE

Tests unblocked: Group D (all 5 tests).

- In `ps_proxy_pipeline_write`, after the buffer memcpy + dirty-
  mark, check `args->stable`.  If `UNSTABLE4`, reply (4a path).
- Else: walk the stripes just touched by this WRITE, call
  `flush_stripe()` for each, reply with
  `committed = args->stable` on success or NFS4ERR_IO on
  failure.  Hold `pwb_mutex` across the per-stripe RPCs.

### 4b.7: probe stats extension + collision-counter observability tests

Tests unblocked: Group E.

- Extend `ps-write-buffer-stats` (the 4a probe op) with the new
  counters listed in "Admin interface".
- Add the two Group E tests against a single-process test harness
  that drives two pipeline flushes to the same upstream FH and
  reads back the DS-side `cs_pending_displaced` via the existing
  chunk-collision probe surface.
- This slice does NOT add new wire ops; the chunk-collision
  validation tooling already shipped.

### 4b.8: delete the 4a "documented loss" test + wrap-up

- **DELETE** `test_concurrent_writes_distinct_stateids_documented_loss`
  from `ps_proxy_pipeline_write_test.c` -- the replacement
  Group B test `test_commit_dirty_stripe_overwrites_zeros`
  asserts the correct behaviour.
- Update `proxy-server.md` Phase 4 status to "DONE (4a + 4b)".
- Update `proxy-server-phase4a.md` cross-reference to point at
  the finalised 4b doc.
- Final `make check` + reviewer pass.

## Files to change

| File | Change |
|------|--------|
| `lib/nfs4/ps/ps_state.h`, `ps_state.c` | Add per-listener dirty-stripe / RMW counters; no new locks |
| `lib/nfs4/ps/ps_write_buffer.c` | Add `pwb_dirty_ht`, `ps_dirty_stripe`, partial-mask helpers, `ps_compose_write_verf` |
| `lib/nfs4/ps/ps_write_buffer_internal.h` | Whitebox surface for Group A + Group B + Group C tests |
| `lib/nfs4/ps/ps_proxy_ops.c` | WRITE-time dirty marking, FILE_SYNC4 inline flush, per-stripe flush walk in `ps_proxy_pipeline_commit` |
| `lib/nfs4/ps/ec_pipeline.c` | `ec_write_stripe_with_file`, `ec_read_stripe_shard`; refactor 4a's `ec_write_codec_with_file` to be a thin caller of the per-stripe primitive |
| `lib/nfs4/client/ec_client.h` | Declare new per-stripe primitives |
| `lib/nfs4/ps/tests/ps_write_buffer_rmw_test.c` | NEW -- Groups A + B + C |
| `lib/nfs4/ps/tests/ps_proxy_pipeline_rmw_test.c` | NEW -- Groups D + E |
| `lib/nfs4/ps/tests/ps_proxy_pipeline_write_test.c` | DELETE `test_concurrent_writes_distinct_stateids_documented_loss`; existing 4a tests stay |
| `lib/nfs4/ps/tests/Makefile.am` | Wire new test files |
| `lib/xdr/probe1_xdr.x` | Extend `ps-write-buffer-stats` (wire-additive) |
| `lib/probe1/probe1_server.c`, `probe1_client.c` | New counter fields |
| `scripts/reffs/probe_client.py.in`, `scripts/reffs-probe.py.in` | Counter formatters |
| `scripts/ci_ps_phase4b_test.sh` | NEW (NOT_NOW_BROWN_COW until bench arc) |
| `.claude/design/proxy-server.md` | Phase 4 status -> DONE (both 4a + 4b) |
| `.claude/design/proxy-server-phase4a.md` | Cross-reference to finalised 4b doc |

## Reviewer checklist (planner self-review)

Per `.claude/standards.md`:

- **Rule 1 (tests-first)**: full test surface listed before any
  implementation step; each slice cites the tests it unblocks.
- **Rule 2 (design compliance)**: slice scope is "per-stripe RMW +
  verifier mix"; cross-stateid coordination + async RMW + CHUNK_LOCK
  are explicitly out of scope.
- **Rule 3 (atomics)**: no new atomics on shared state -- dirty-
  bitmap mutations are all under `pwb_mutex`.  Composed-verifier
  fields (`pwb_mds_verf`, `pwb_mds_verf_set`) are read/written
  under `pwb_mutex` too.  The 4a `pls_state` / `pls_active_buffer_refs`
  ordering stays unchanged.
- **Rule 4 (lock ordering)**: order unchanged from 4a:
  `pls_session_rwlock` > `rcu_read_lock` (buffer table lookup,
  dirty-table walk) > `pwb_mutex` (buffer + dirty-table mutation).
  The dirty hash table is RCU-walked at flush but not
  concurrently-mutated (mutex held), so no new lock layer.
- **Rule 5 (forward declarations)**: `ps_dirty_stripe` stays
  internal to `ps_write_buffer.c`; whitebox header exposes only
  the helper signatures.
- **Rule 6 (error paths)**: per-slice tests cover the partial-
  failure cases (`test_commit_keeps_dirty_bits_on_partial_failure`,
  `test_write_file_sync_failure_returns_io`).  RMW scratch buffer
  freed on every exit.
- **Rule 7 (logging)**: TRACE on per-stripe flush start/end with
  stripe number + dirty-stripe count; LOG only for operator-
  actionable events (none new in 4b -- composed verifier mismatch
  is a client-side concern, not operator).
- **Rule 8 (probe surface)**: additive counters on
  `ps-write-buffer-stats`; no new probe op.
- **Rule 9 (XDR)**: probe XDR is internal-only (additive);
  nfsv42_xdr.x unaffected.  No protocol op renumbering.  No on-
  disk format change.
- **Rule 10 (UUID stability)**: no new long-lived objects.

The slice touches RCU lifecycle (the new dirty hash table is RCU-
walked at flush time), per-stripe encode/decode, and the verifier
contract -- all reviewer-gate triggers per `.claude/CLAUDE.md`
"Workflow rules".  **Reviewer agent runs on every slice in this
arc**, not just the wrap-up.

## Risks

1. **`pwb_mutex` held across CHUNK_READ + CHUNK_WRITE RPCs.**
   The RMW path issues network I/O under the buffer's leaf mutex.
   If a CHUNK_READ takes seconds (slow DS), other WRITEs to the
   same `(stateid, fh)` block.  Mitigation: the kernel client
   already serialises ops per `(stateid, fh)` (4a's serialisation
   analysis still applies), so the only callers waiting are
   already in line behind the in-flight WRITE -- no new
   serialisation surface.  An async RMW path is the proper fix;
   tracked as NOT_NOW_BROWN_COW.

2. **Partial-mask granularity is shard-sized, not byte-sized.**
   A WRITE of 1 byte at the start of a shard marks the entire
   shard as "dirty from buffer".  The RMW does NOT read the
   shard's pre-existing bytes; the buffer's zero-filled tail of
   that shard wins.  Mitigation: the buffer is initialised
   carefully -- the memcpy in WRITE only touches `[offset,
   offset+count)`; the remaining bytes of the shard are zeros
   from `calloc`.  This is **wrong** for the "1-byte WRITE
   followed by COMMIT" case.  **Fix**: when allocating a partial
   mask for a partially-touched shard, ALSO read the shard's
   existing bytes from the DS and prefill the buffer's shard-
   range with them.  This adds one CHUNK_READ to the first
   sub-shard WRITE on a fresh stripe; subsequent WRITEs to the
   same shard touch the already-populated buffer.  **Slice 4b.3
   owns this** -- the RMW path is the natural home for the
   prefill.  Tests `test_commit_partial_stripe_reads_existing`
   and `test_commit_partial_stripe_decode_uses_quorum` cover it.

3. **Geometry mismatch between buffer snapshot and current
   layout.**  If the MDS reissues a layout with a different
   `(k, m, shard_size)` between WRITE and COMMIT, the buffer's
   geometry snapshot is stale.  Mitigation: on geometry mismatch
   at flush time (compare buffer's snapshot to the layout at
   flush), drop the buffer and return NFS4ERR_STALE -- the
   client rewrites under the new geometry.  Same idiom as 4a's
   `pls_boot_gen` mismatch.

4. **MDS verifier byte-string size.**  The MDS verifier is up to
   8 bytes (NFS4_VERIFIER_SIZE); composing it into the listener
   verifier via XOR-mix into 8 bytes is fine.  If a future MDS
   variant returns a longer verifier, the mix truncates to the
   first 8 bytes -- acceptable, since the verifier comparison
   is equality, not exact-byte-stream-fidelity.

5. **`cs_pending_displaced` is observational, not protective.**
   Concurrent overlapping-stripe writes from two PSes still race
   at the DS; "last writer wins per stripe" matches POSIX, but a
   client expecting both writers' bytes to coexist sees one of
   them lost.  This is **documented expected behaviour**, not a
   bug -- POSIX makes no ordering promise.  The test
   `test_multi_ps_overlap_stripe_increments_displaced` pins the
   counter signal so a future protocol change (CHUNK_LOCK) can
   add real protection without losing the diagnostic.

6. **FILE_SYNC4 latency under DS load.**  An inline FILE_SYNC4
   WRITE blocks the client thread for the full RMW + encode +
   per-DS CHUNK_WRITE+FINALIZE+COMMIT round-trip storm.  For a
   stripe-aligned WRITE that's roughly the same cost as 4a's
   deferred COMMIT.  For a partial-stripe WRITE the extra
   CHUNK_READ + decode is added.  Documented in operator notes;
   no mitigation needed (FILE_SYNC4 callers asked for durability,
   they get the latency cost).

7. **MDS verifier capture race during inline FILE_SYNC4.**  Two
   concurrent FILE_SYNC4 WRITEs to the same buffer flush
   different stripes; each captures the MDS verifier into
   `pwb_mds_verf`.  Last-writer-wins per buffer is fine because
   the MDS verifier is monotonic per MDS-boot-epoch -- both
   writes see the same MDS verifier unless an MDS restart
   happened between them, in which case the later writer's
   verifier is the correct one to keep.  No mitigation needed.

## Cross-references

- Parent: `.claude/design/proxy-server.md` Phase 4
- Predecessor wiring: `.claude/design/proxy-server-phase4a.md`
  (whole-file buffer + COMMIT-flush; the dirty bitmap layers on top)
- DS-side collision observability:
  `.claude/design/chunk-collision-validation.md` Track 1 + Track 2
  (Track 2 is gated on this slice landing)
- Per-stripe encode prior art: `lib/nfs4/ps/ec_pipeline.c`
  `ec_write_codec_with_file` (4a) -- 4b factors the per-stripe
  inner loop out and exposes it as a primitive
- DS-side per-block state: `lib/nfs4/server/chunk.c` (`cb_state`,
  `cb_gen_id`, `cb_client_id`, `cb_owner_id`, `cs_pending_displaced`)
- Trust-stateid plumbing: `.claude/design/trust-stateid.md` (the
  RMW CHUNK_READ uses the same layout stateid the WRITE-flush
  side uses; no new trust-table interaction)
