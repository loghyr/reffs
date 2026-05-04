<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# trust-stateid slice 1.5: engage the DS trust check from NFSv3 dstores

Slice 1 (`trust-stateid-slice-1.md`) shipped the
trust-stateid mechanism and the conflict-recall path at
LAYOUTGET.  The 2026-05-04 v2 sweep against the bench docker
stack on adept showed slice 1 is correct but **structurally
bypassed** in that bench: ec_demo uses the anonymous stateid
for CHUNK ops by default, and the DS-side trust check at
`chunk.c:136` skips anonymous stateids -- so the trust table
maintained by slice 1 is never consulted on the data path.

This slice closes that bypass.

## The bypass, in detail

ec_demo (`lib/nfs4/ps/ec_pipeline.c`) sets the
`em_tight_coupled` bit per-mirror from the layout response's
`ffdv_tightly_coupled` flag.  When false, ec_demo passes
`stateid4_anonymous()` in `cwa_stateid` for every CHUNK_WRITE.
The DS-side check at `lib/nfs4/server/chunk.c:136`:

```c
if (!stateid4_is_special(&args->cwa_stateid)) {
    /* trust-table lookup + reject path */
}
```

Anonymous stateids ARE special, so the body is skipped.

`ffdv_tightly_coupled` is set by the MDS at GETDEVICEINFO
time (`lib/nfs4/server/layout.c`, in the deviceinfo builder):

```c
ver.ffdv_tightly_coupled = ds && ds->ds_tight_coupled;
```

`ds->ds_tight_coupled` is set per-dstore at allocation:

- **Local** dstore vtable (`dstore_ops_local.c`):
  hardcoded `true` (combined-mode reffsd is always tight-
  coupled because the trust table and the DS are the same
  process).
- **NFSv3** dstore vtable (`dstore_ops_nfsv3.c`): defaults
  `false`.  The NFSv3 wire protocol has no notion of
  trust-stateid; the DS at the other end is just an NFSv3
  server that can't enforce a trust table.
- **NFSv4** dstore vtable (`dstore_ops_nfsv4.c`): probes via
  `probe_tight_coupling()` on first connect.

So in the bench docker stack (10 separate reffsd-DS containers
talking NFSv3 to the MDS), every dstore has
`ds_tight_coupled = false` -> ec_demo gets
`ffdv_tightly_coupled = false` -> ec_demo uses the anonymous
stateid -> the DS-side trust check is a no-op.

## Why this matters

The 2026-05-04 v2 sweep dropped silent split-brain (MIXED) from
65 % to 20 % anyway, because the conflict-recall fires at the
MDS and the resulting on-disk inconsistency is caught by v2's
strict parity check on read (READ_FAILED instead of MIXED).
That is a real safety improvement.

But the slice plan predicted MIXED -> 0 with `b_won = 20`.
That prediction was based on the trust-stateid mechanism
actually engaging on the data path.  In the current bench
configuration, it does not engage, and 4 MIXED outcomes still
occur (the workload the mechanism was designed for, where the
mechanism never fires because the data path bypasses it).

To close the prediction, the trust path has to be reachable in
a configuration that:
- Does not use combined mode (combined-mode ec_demo segfaults
  on session teardown, see follow-up below).
- Uses NFSv3 dstores (the bench docker stack's shape).

That requires turning on tight-coupling for NFSv3 dstores.

## What this slice ships

### 1. Per-dstore tight-coupling toggle in `[[data_server]]` config

```toml
[[data_server]]
id      = 1
address = "192.168.2.105"
port    = 2049
path    = "/foo"
tight_coupling = true   # NEW: opt in to the trust path even
                        # though this is an NFSv3 vtable
```

When the toggle is set, the dstore-alloc path overrides the
vtable's default `ds_tight_coupled` and sets it to `true`.
The MDS then advertises `ffdv_tightly_coupled = true` in
GETDEVICEINFO for that dstore, and ec_demo uses the real
layout stateid in `cwa_stateid`.

The toggle is opt-in because not every NFSv3 dstore is a
reffsd that knows about trust-stateid -- it's only safe when
the operator knows the DS is reffsd (and not, e.g., a generic
NFSv3 server).  The bench docker stack is exactly that case.

### 2. Bench docker stack uses the toggle

`deploy/benchmark/mds.toml`:

```toml
[[data_server]]
id = 1
address = "ds0"
port = 2049
path = "/"
tight_coupling = true   # bench-only: every DS is reffsd

# ... ditto for ds1..ds9
```

Same change to `mds-tls.toml` if it lives.

### 3. Tests

| File | What |
|------|------|
| `lib/nfs4/dstore/tests/dstore_test.c` | Add a test that creates an NFSv3 dstore with `tight_coupling=true` config and verifies `ds->ds_tight_coupled == true`. |
| `lib/nfs4/server/tests/...` | A test that asserts the GETDEVICEINFO builder respects `ds->ds_tight_coupled` when emitting `ffdv_tightly_coupled` (probably exists; verify, extend if needed). |
| `lib/nfs4/tests/layout_conflict_scan_test.c` | Optional: extend Group B to also assert the test path of a tight-coupled DS receiving a real-stateid CHUNK_WRITE (today the mock has trust/revoke counters but not a CHUNK_WRITE-with-stateid assertion). |

### 4. Re-run the v2 N=20 sweep against the bench docker stack
   with tight-coupling ON

Same harness, same workload.  Predicted outcome:

| Outcome | Predicted (slice 1.5) |
|---------|----------------------:|
| Clean A-win | 0 |
| Clean B-win | 18-20 |
| MIXED | 0 |
| READ_FAILED | 0-2 (residual race-window if any) |

If MIXED stays > 0, the trust path has a hole that the slice
plan didn't anticipate; root-cause and decide whether to
extend slice 1.5 or open a follow-on.  If READ_FAILED is also
0, the slice closes.

## What this slice does NOT ship

- Combined-mode ec_demo segfault fix.  Backtrace from
  2026-05-04 captured (`mds_destroy_session` ->
  `xdr_COMPOUND4res` decode of DESTROY_SESSION response,
  `xdr_array` -> `xdr_nfs_resop4` -> `xdr_int` reading from a
  likely-freed buffer).  Tracked separately; combined-mode is
  not on the slice 1.5 critical path because the bench docker
  stack with tight-coupling toggle is the friendlier
  measurement environment.
- Probe-protocol toggle for live setting of `ds_tight_coupled`.
  Slice 1.5 only adds the static toml config.
- NFSv4 dstore vtable (Phase 2 Step 2.2 of `trust-stateid.md`).
  Per-dstore probing of tight-coupling at session setup is
  separate work for NFSv4 dstores; slice 1.5 only addresses
  NFSv3.

## Slice plan, day-by-day

| Day | Work | Tests |
|-----|------|-------|
| Day 1 AM | Add `tight_coupling` field to `reffs_data_server_config` (settings.h); parse in TOML loader; thread to `dstore_alloc` via a new param. | Config test verifies the field round-trips through the loader. |
| Day 1 PM | In `dstore_alloc`, after the vtable-default `ds_tight_coupled` is set, override with the config value if non-default.  Existing local-dstore tests must still pass (they don't use the config path). | New dstore_test case: NFSv3 dstore with `tight_coupling=true` -> `ds->ds_tight_coupled == true`. |
| Day 2 | Update `deploy/benchmark/mds.toml` to set `tight_coupling = true` on all 10 dstores.  Re-run the bench bringup so the new config is picked up. | None directly; the v2 sweep is the integration test. |
| Day 3 | Run `--layout v2 --runs 20` against the updated bench.  Append to `report.md` with the new outcome distribution. | n/a (measurement) |
| Day 4 | Reviewer pass + style + license + push.  Update `progress_report.md` §3.6 with the measured close. | All green from prior days. |

## Decision matrix at end of Day 3

| Result | Action |
|--------|--------|
| `mixed = 0` and `b_won >= 18` | Slice closed.  Trust-stateid measurably prevents split-brain in the configuration where it engages.  Update slide 16 v2 row to "measured: 0/20 MIXED". |
| `mixed = 0` and `read_failed > 0` | Mechanism prevents silent split-brain but a race window between recall and re-grant produces unreadable file in some rounds.  Investigate the race window; may need to extend the synchronous-revoke wait. |
| `mixed > 0` (any) | The trust path has a hole.  Most likely cause: ec_demo retries on `NFS4ERR_BAD_STATEID` by re-LAYOUTGETing, and the re-LAYOUTGET fires the conflict-recall against itself somehow.  Instrument the DS-side trust check (count rejected CHUNK_WRITEs) to confirm it fires; trace ec_demo's retry path. |

## Cross-references

- `trust-stateid-slice-1.md` -- slice 1's design and what it
  shipped.
- `.claude/experiments/01-trust-stateid-window/report.md`
  "N=20 multi-run sweep, v2 with trust-stateid slice 1
  (2026-05-04)" -- the measurement that motivated this slice.
- `lib/include/reffs/dstore.h` -- `ds_tight_coupled` field
  declaration.
- `lib/nfs4/dstore/dstore_ops_nfsv3.c` -- the NFSv3 vtable
  that today defaults `ds_tight_coupled = false`.
- `lib/nfs4/ps/ec_pipeline.c` -- ec_demo's per-mirror
  `em_tight_coupled` decision.
