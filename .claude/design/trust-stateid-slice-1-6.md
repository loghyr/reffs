<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Slice 1.6: ec_demo retry-on-BAD_STATEID + re-LAYOUTGET

## Context

Slice 1.5 closed the trust-stateid safety property: the DS-side
trust table eliminates silent split-brain across the N=20 v2
race sweep (0/20 MIXED, vs the v1 baseline's 13/20).  But the
matching availability cost is loud: 20/20 READ_FAILED, because
neither writer in the harness reaches CHUNK_FINALIZE / COMMIT
when its in-flight CHUNK_WRITEs are rejected with
NFS4ERR_BAD_STATEID at the trust check.

The slice 1.5 plan called this out as the
"`mixed = 0, read_failed > 0`" decision-matrix row and named
the follow-on as an ec_demo retry-path slice, not a protocol
change.  This is that slice.

## What this slice ships

**The OUTER retry layer that the existing inner retry in
`ec_chunk_write` was designed to feed into.**

`ec_chunk_write` (in `ec_pipeline.c`) already retries
NFS4ERR_BAD_STATEID three times with 50/100/200 ms backoff,
reporting LAYOUTERROR to the MDS each time, and on exhaustion
returns -ESTALE.  The header comment is explicit: "After 3
failed attempts, return -ESTALE so the caller can LAYOUTRETURN."
That caller (the stripe loop in `ec_write_codec`) does NOT
currently catch -ESTALE -- it just propagates the failure up
and aborts the whole write.

When the trust entry has actually been revoked (slice 1.5's
race-harness scenario), no amount of inner retry helps -- the
stateid stays BAD_STATEID until the client gets a fresh
layout from the MDS.  That's slice 1.6's job.

When the per-stripe write or read returns -ESTALE (i.e., the
inner retry exhausted), the codec layer:

1. Releases the current layout state (LAYOUTRETURN-equivalent
   client-side cleanup; we do NOT send LAYOUTRETURN to the
   MDS because the MDS already revoked the layout when the
   conflict-recall fired -- sending LAYOUTRETURN now would
   be a no-op at best and a stale-stateid error at worst).
2. Backs off a random 50-200 ms.
3. Re-issues `mds_layout_get` with the same iomode.  If the
   MDS grants a new layout, the new stateid is fanned out to
   the DSes via TRUST_STATEID and the client retries the
   failed stripe with the fresh stateid.
4. If `mds_layout_get` fails (NFS4ERR_RECALLCONFLICT,
   NFS4ERR_LAYOUTUNAVAILABLE, etc.), the client gives up
   cleanly with the original error.
5. Maximum 3 retry attempts per stripe; after that the write
   fails with the original BAD_STATEID errno, no infinite
   loop.

The re-resolve of mirrors (`ec_resolve_mirrors`) runs after
each successful re-LAYOUTGET because the layout MAY name
different DSes after the recall (in practice it usually
doesn't, but the protocol allows it).

## What this slice does NOT ship

- **No protocol change.**  TRUST_STATEID / REVOKE_STATEID /
  the trust table are unchanged.  This is purely a client-side
  retry policy.
- **No NFSv4 dstore vtable debug.**  The Phase 2 work to wire
  `protocol = "nfsv4"` end-to-end remains a separate slice.
- **No re-encode optimisation.**  We re-encode the entire stripe
  on retry.  The encoder is deterministic and per-stripe cost
  is small relative to the re-LAYOUTGET RPC, so the simpler
  "drop everything, retry" path is sufficient.
- **No "resume from last committed stripe" optimisation.**  We
  retry the stripe that failed, not the entire write.  The
  CHUNK_WRITE op is idempotent (same offset, same data, same
  CRC), so re-writing prior stripes would also be safe but is
  not needed.
- **No server-side priority.**  If both racing writers retry
  simultaneously, the MDS's own conflict-recall logic on
  LAYOUTGET breaks the tie (the later writer wins; the earlier
  writer's re-LAYOUTGET hits NFS4ERR_RECALLCONFLICT and gives
  up cleanly).  No client-side "I'm older than that other guy"
  arbitration.

## Test plan

### Integration test (acceptance criterion)

Re-run the slice 1.5 N=20 v2 sweep harness with the retry path
in place.  The decision matrix:

| Outcome                                  | Verdict |
|------------------------------------------|---------|
| `mixed = 0, b_won >= 18, read_failed = 0`| Slice closed.  Trust-stateid property holds AND availability is restored. |
| `mixed = 0, b_won < 18` | Some races still produce read_failed.  Investigate which retry exhaustion mode dominates. |
| `mixed > 0` (any) | Retry path introduces a new corruption window.  Revert and re-think. |

The harness is unchanged.  Same docker bench, same RS 4+2,
same 100 MiB blobs, same 0.5 s delay between A and B.

CSV path:
`.claude/experiments/01-trust-stateid-window/data/race_v2_N20_4plus2_rs_100MB_slice16.csv`

### Unit test (best-effort, not blocking)

If the existing `lib/nfs4/ps/tests/` harness already mocks
`mds_layout_get` + `ds_write`, add a unit test:

| Test | Intent |
|------|--------|
| `test_ec_write_retries_on_bad_stateid` | Mock returns NFS4ERR_BAD_STATEID on stripe 5; mds_layout_get mock returns success on retry; verify the write completes and all stripes are written. |
| `test_ec_write_gives_up_on_persistent_bad_stateid` | Mock returns BAD_STATEID on every retry; verify the write fails after 3 attempts. |
| `test_ec_write_propagates_recallconflict` | Mock returns BAD_STATEID; mds_layout_get retry returns NFS4ERR_RECALLCONFLICT; verify the write fails immediately with the original errno (no further retries). |

If the existing harness does NOT mock these (likely it doesn't,
since `ec_pipeline.c` is system-bench-only today), defer the
unit tests; integration is the acceptance bar.

### Existing tests affected

| File | Impact |
|------|--------|
| `lib/nfs4/ps/tests/ec_pipeline_test.c` (if it exists) | None unless retry path is unit-tested |
| `tools/ec_demo` happy-path runs (`make run-bench`) | PASS -- single-writer never hits BAD_STATEID; retry path is a no-op |
| Slice 1.5 N=20 v2 sweep CSV in tree | Stays as the slice 1.5 reference; new CSV is the slice 1.6 evidence |

## Implementation steps

### Step 1: Confirm the errno contract from ec_chunk_write

`ec_chunk_write` (lib/nfs4/ps/ec_pipeline.c:218) returns
-ESTALE after 3 inner retries.  The matching `ec_chunk_read`
does the same.  The retry predicate is therefore `ret == -ESTALE`,
not -EBADF.  No code change here -- this is just documenting
the contract the slice 1.6 outer retry consumes.

### Step 2: Factor the per-stripe write into its own helper

The current `ec_write_codec` stripe loop is ~80 lines inline.
Extract `ec_write_one_stripe(struct ec_context *ctx, ...)`
to make the retry-around-the-stripe wrapping clean.  No
behavior change in this step alone.

### Step 3: Wrap with retry-on-BAD_STATEID

```c
for (size_t s = 0; s < nstripes; s++) {
    int retries = 0;
    while (1) {
        ret = ec_write_one_stripe(&ctx, s, ...);
        if (ret != -ESTALE || retries >= MAX_RETRIES)
            break;
        retries++;
        ec_log("ec_write: stripe %zu BAD_STATEID, "
               "retry %d/%d after re-LAYOUTGET\n",
               s, retries, MAX_RETRIES);
        usleep(50000 + (rand_r(&seed) % 150000));
        ec_layout_release(&ctx.ctx_layout);
        ret = mds_layout_get(ms, &ctx.ctx_file,
                             LAYOUTIOMODE4_RW, layout_type,
                             &ctx.ctx_layout);
        if (ret) {
            ec_log("ec_write: re-LAYOUTGET failed: %d "
                   "(giving up after %d retries)\n",
                   ret, retries);
            break;
        }
        ret = ec_resolve_mirrors(&ctx);
        if (ret)
            break;
    }
    if (ret)
        break;
}
```

### Step 4: Mirror the same retry on the read path

`ec_read_codec` has the symmetric structure.  Reads can also
hit BAD_STATEID if the layout is recalled mid-read.  Apply the
same retry wrapper.

### Step 5: Run the integration test

On dreamer with the slice 1.5 bench config (`tight_coupling = true`,
NFSv3 dstores), run the N=20 v2 sweep and capture the CSV.

### Step 6: Document the result

Append a new section to
`.claude/experiments/01-trust-stateid-window/report.md` and
update `progress_report.md` §3.6 with the slice 1.6 row in
the outcome table.

## Decision matrix at end of step 5

| Result | Action |
|--------|--------|
| mixed = 0 AND read_failed = 0 AND b_won >= 18 | Slice closed.  Update upstream docs. |
| mixed = 0 AND 0 < read_failed < 5 | Mostly closed; investigate the residual cells (probably MDS not granting B's re-LAYOUTGET fast enough); document as "follow-on" if the cells are well-understood. |
| mixed = 0 AND read_failed >= 5 | Retry policy is wrong (probably the back-off is too short, causing ping-pong, OR the RECALLCONFLICT path isn't firing).  Iterate on retry policy. |
| mixed > 0 | New corruption window.  Revert.  The retry path must not weaken the safety property slice 1.5 just established. |

## Cross-references

- `trust-stateid-slice-1-5.md` -- slice that lands the safety
  property; slice 1.6 picks up the availability follow-on it
  scoped.
- `.claude/experiments/01-trust-stateid-window/report.md` --
  the slice 1.5 measurement that this slice's test re-runs.
- `lib/nfs4/ps/ec_pipeline.c` -- the implementation site.
- `lib/nfs4/client/mds_session.c` -- BAD_STATEID errno mapping
  reference.

## Followups (out of slice 1.6 scope)

- Move the retry policy out of ec_demo's pipeline and into a
  reusable client-library helper, once a real Linux NFSv4.2
  client implementation lands the same retry shape.  Today it
  lives in ec_pipeline.c only.
- Server-side priority arbitration (later-writer-wins) at
  LAYOUTGET if the MDS's existing conflict-recall logic turns
  out to need explicit hint-passing for fairness.
