<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Two-writer race harness — design

## Why this exists

The existing `exp01_race_v1.sh` / `exp01_race_v2.sh` are
single-shot manual drivers.  They produce one observation per
invocation and have no machine-readable output.  The IETF 126
deck claim — "the trust-stateid mechanism prevents split-brain
writes" — is currently backed by **one** v1 race observation
(`report.md` §"v1 result").

`REVIEW_RESPONSE_PLAN.md` and the codex review both flagged this:
the cheapest item that materially defends against
single-observation pushback is multi-run averaging.  The harness
delivers that, and is the missing prerequisite for the v2 re-run
once the TLS+PS-bench trigger is isolated.

## What the harness measures (per round)

Per round, the harness records:

| Field | Source | Meaning |
|---|---|---|
| `round` | loop counter | 1..N |
| `layout` | --layout flag | v1 or v2 |
| `codec` | --codec flag | rs / mojette-sys / mojette-nonsys |
| `k`, `m` | --k / --m | shard counts |
| `size_mb` | --size | client buffer size |
| `delay_ms` | --delay-ms | gap between A start and B start |
| `a_rc`, `b_rc` | wait $PID exit codes | success / failure of A and B |
| `a_stripes_ok` | grep A.err for "ok" lines | how many of A's writes succeeded |
| `a_first_fail_stripe` | grep A.err for first "FAILED:" | stripe index where A was rejected |
| `b_stripes_ok` | grep B.err for "ok" lines | sanity (B should complete) |
| `final_winner` | cmp /tmp/post against /tmp/A,/tmp/B | A / B / MIXED / READ_FAILED |
| `mixed_diff_a` | `cmp -l /tmp/A /tmp/post \| wc -l` | only set when MIXED |
| `mixed_diff_b` | `cmp -l /tmp/B /tmp/post \| wc -l` | only set when MIXED |

The deck-relevant invariant is `final_winner != MIXED`.  Across
N runs:

- If `final_winner = MIXED` happens **even once**, the
  "no split-brain" claim is empirically falsified.
- If `final_winner = MIXED` is `0/N`, the claim holds for the
  measured workload.
- `a_first_fail_stripe` documents how quickly A noticed the
  recall (lower = tighter mechanism response).

`a_first_fail_stripe = N/A` with `a_rc = 0` is itself a finding:
it means A wrote the entire file before B's request landed —
either the delay was too long, or the codec was too fast.  The
sweep script can flag this and suggest a smaller `--delay-ms`.

## Two scripts, mirroring exp 12

- `exp01_race_harness.sh` (single round)
  - Replaces both `exp01_race_v1.sh` and `exp01_race_v2.sh`.
  - Takes `--layout v1|v2`, `--delay-ms M`, `--size-mb S`,
    `--codec C`, `--k K`, `--m M`, `--round N`,
    `--mds HOST[:PORT]`.
  - Emits one `RESULT` line on stdout, parseable by the sweep
    script.  Format:
    ```
    RESULT round=$N layout=$L codec=$C k=$K m=$M size_mb=$S \
           delay_ms=$D a_rc=$ARC b_rc=$BRC \
           a_stripes_ok=$AS a_first_fail_stripe=$AFS \
           b_stripes_ok=$BS final_winner=$FW \
           mixed_diff_a=$MDA mixed_diff_b=$MDB
    ```
  - Cleans up `/tmp/A_$N`, `/tmp/B_$N`, `/tmp/post_$N` between
    rounds so concurrent rounds (future) don't collide.

- `exp01_race_sweep.sh` (multi-round CSV)
  - Loops the harness N times, emits CSV header + per-round row.
  - CSV columns match the table above exactly.
  - Default N=20 (the codex review's lower bound for "materially
    defends against single-observation pushback").
  - Single CSV file per layout in `data/`:
    `data/race_v1_N20_4+2_rs_100MB.csv`,
    `data/race_v2_N20_4+2_rs_100MB.csv`.

## v1 vs v2 readiness

- **v1 path**: ready today against the bench docker stack on
  adept (or the standalone reffsd setup used for the
  2026-05-03 bisect).  No prerequisites.
- **v2 path**: harness writes the script; the script blocks on
  the same TLS+PS-bench trigger isolation that exp01 itself is
  blocked on.  Until `build_ffv2_layout4` is instrumented
  against the live PS bench, `--layout v2` will fail at
  LAYOUTGET against the TLS+PS bench (the only environment that
  reproduces it; combined-mode bisect on 2026-05-03 ruled out a
  size threshold).  The v2 row of the deck stays "asserted,
  validation pending" until both are done.

The harness commits in this slice; the v2 measurement waits on
the bench-trigger isolation work tracked in
`ietf126.experiments.md` §1 of the P1 priority queue.

## What this slice does not deliver

- Per-DS revoke-ack latency (Variant B).  Still requires the
  protocol-level acknowledgement path; not in scope.
- Cross-host topology (exp 6 path).  Loopback-only for now;
  documented as a caveat the same way exp 1 v1 documented it.
- Statistical confidence intervals.  N=20 is "tight enough to
  defend against pushback", not "tight enough to publish a
  rate".  If the v1 sweep finds even one MIXED outcome, that's
  a real bug, not a statistical artefact, and the slice
  reframes around root cause.

## Test impact

No reffs source change.  No CI change.  The harness scripts live
under `.claude/experiments/01-trust-stateid-window/` and emit
CSV into `data/`, which is **tracked** in git (one CSV per sweep
configuration, named `race_<layout>_N<runs>_<k>plus<m>_<codec>_<size>MB.csv`),
consistent with exp 12.  The v1 sweep runs on adept against the
existing bench docker stack with no code changes.

## Implementation order

1. Write `exp01_race_harness.sh` — single round, parameterized.
2. Verify on adept against bench stack: `--layout v1 --round 1`
   reproduces the existing v1 result (`report.md` §v1 detail).
3. Write `exp01_race_sweep.sh` — multi-round CSV emitter.
4. Run `--layout v1 --runs 20 --size-mb 100 --codec rs` on adept,
   commit the CSV under `data/`.
5. Append the multi-run result to `report.md` (replacing the
   "Single race observation; no statistical distribution" caveat
   in §Caveats with the actual N=20 numbers).
6. Once the TLS+PS-bench trigger is isolated (separate slice):
   run `--layout v2` and append v2 results.

`exp01_race_v1.sh` and `exp01_race_v2.sh` stay in place until
step 4 completes — the new harness must reproduce the prior
manual result before the old scripts are deleted.

## Open questions

- **Should `--delay-ms` adapt to file size?**  100 MiB takes
  ~60s on adept (per `report.md`); 0.5s puts B inside A's
  window cleanly.  At 10 MiB, 0.5s might already be past A's
  window.  For now: keep `--delay-ms` user-controlled, add a
  warning if `a_first_fail_stripe = N/A AND a_rc = 0` (A
  finished before B's request landed).
- **Should the sweep parallelise rounds?**  No — the bench
  stack has shared MDS state and concurrent rounds would
  cross-contaminate.  Sequential rounds, accepted cost.
- **What about the read-back step?**  Each round has to clean
  the file before the next round; the simplest is to use a
  per-round file name `race_target_$N` so rounds are
  independent (no need to delete between rounds).  Adopted.
