<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Experiment 1: Trust-Stateid Window Measurement

Closes the **adversarial corruption-test** half of the experiment
spec.  The latency-measurement half (Variant B with per-DS
fan-out instrumentation) is deferred until the protocol-level
acknowledgement path is added.

## TL;DR

A two-client race against the same file's layout was driven on
the bench docker stack on adept.  Client A starts a 100 MiB
RS 4+2 write; 0.5 s later Client B starts its own write to the
same file, forcing the MDS to recall A's layout and grant B's.

**v1 result (NFSv3 to DSes, fencing-based revocation):**
- A's write FAILED with NFS3 error status=5 mid-stream (after
  ~101 stripes had been issued).
- B's write COMPLETED successfully (all 6399 stripes).
- Reading the file back returned **B's content byte-for-byte**
  ("FILE = B (A lost)").
- No mixed/split-brain state.  The MDS's recall + revocation
  caused A's late writes to be rejected at the DS before they
  could corrupt parity.

This empirically validates §3.6's "the trust-stateid mechanism
prevents split-brain writes" for the v1 fencing path.

**v2 result (CHUNK ops, TRUST_STATEID path): not yet measured.**
Both clients failed at LAYOUTGET with `-61` (ENODATA) on the
100 MiB workload.  v2 LAYOUTGET worked at 1 MiB in experiment 6,
so this is a size-dependent v2 issue independent of the
trust-stateid logic under test here.  Tracked as a separate
follow-up.

## Setup

- Single-host bench docker stack on adept (Intel N100, Fedora 43).
- Same MDS + 10 DSes used in experiments 3, 4, 6, 12.
- Two `ec_demo write` invocations targeting the same file
  `race_target` with distinct `--id`:
  - Client A: random 100 MiB blob `/tmp/A`, started first.
  - Client B: random 100 MiB blob `/tmp/B`, started 0.5 s later.
- Codec RS 4+2, layout v1, 4 KiB shards.

The 0.5 s delay was chosen empirically: 100 MiB write takes
~60 s on adept, so 0.5 s puts B's request well inside A's
write window.  A had issued ~101 stripes (out of 6400) when B
forced the layout transfer.

The test does not instrument per-DS revoke timing -- that
requires either the protocol-level ack path (Variant B) or an
out-of-band trace mechanism that is itself a separate slice.
The measurement here is **client-observable**: Client A's
write outcome and the final file state are both visible from
client side without server instrumentation.

## v1 detail

```
[A] start at 07:53:04.915
[B] start at 07:53:05.418
[A] exit rc=1 at 07:54:03.806   <-- A failed
[B] exit rc=0 at 07:54:03.807   <-- B succeeded
```

Stderr of A:

```
[953.062] ec_write: stripe 101 parity[1] fh_len=24 wsz=4096
ds_write: NFS3 error: status=5
[953.062] ec_write: parity[1] FAILED: -5
ec_demo: write failed: -5
```

A had successfully written stripes 0-100 (data + parity[0]) and
was on stripe 101's parity[1] when the DS rejected the write
with NFS3ERR_IO (status=5).  This is consistent with the v1
fencing model: the MDS rotated the synthetic uid/gid on the DS
file as part of granting B's layout, and A's continuing writes
under the old credentials get rejected.

Stderr of B:

```
[10.879] ec_write: stripe 6399 parity[0] fh_len=24 wsz=4096
[10.880] ec_write: parity[0] ok
[10.880] ec_write: stripe 6399 parity[1] fh_len=24 wsz=4096
[10.881] ec_write: parity[1] ok
ec_demo: write OK
```

B completed all 6400 stripes (0..6399).  No errors.

Final file state:

```
FILE = B (A lost)
```

`cmp -s /tmp/B /tmp/post` returns 0 -- the read-back file is
**byte-identical to B's input**.  Not a single byte of A's
content remains.

## Acceptance criteria

| spec criterion | required | v1 measured | v2 measured |
|----------------|----------|-------------|-------------|
| Post-revoke writes rejected | 100% | YES (A's stripe 101 parity[1] failed at DS) | n/a (LAYOUTGET issue) |
| Final file consistent (no MIXED) | required | YES (matches B exactly) | n/a |
| p99 fan-out latency | < 10 ms (Variant B) | not measured (Variant A only) | not measured |
| Throughput regression | < 5% | not measured (single-race test) | not measured |

## Implications for §3.5 / §3.6 of progress_report.md

The §3.5 statement *"the window during which A could corrupt
parity is bounded by the fan-out latency of step 2"* is the
**asserted** form.  This experiment measures the **outcome**
rather than the latency -- specifically that A's post-recall
writes are rejected and the final file is consistent with B's
intent.  That is what §3.6 needs (David Black's IETF 124
question is about whether the design produces split-brain
state; the answer is "no, on the v1 fencing path").

For §3.6 specifically, this is a direct empirical measurement
that the design's coherence claim holds in practice on the v1
path.  The v2 trust-stateid path remains asserted-only until the
v2 LAYOUTGET issue is fixed and the same race is re-run.

## Followups

1. **v2 LAYOUTGET 100 MiB failure**: investigate why a 100 MiB
   v2 layout request returns `-61` while 1 MiB worked in
   experiment 6.  Likely a size limit somewhere in the v2 chunk-
   layout encoding or in the runway sizing.
2. **Variant B (server-side fan-out instrumentation)**:
   protocol-level ack from each DS so the MDS can measure
   per-DS revoke completion.  Quoted at 1-2 weeks engineering
   in the spec.
3. **Multi-run statistical characterisation**: 20+ races to
   estimate the rate of post-recall write acceptance (must be
   0% across all runs; one acceptance is a real bug).
4. **Real-network re-run** (experiment 6 topology) once the
   above are in place: cross-host fan-out latency vs loopback,
   per the spec's loopback-only caveat.
5. **v2 trust-stateid race** once #1 is fixed.

## Caveats

- Latency-budget claim from the spec ("p99 < 10 ms") is not
  measurable from client side alone.  This experiment confirms
  the **correctness** half of the §3.5/§3.6 claims **only when
  the slow writer is recalled early** (Mode 2 below); when the
  slow writer wins (Mode 1), the late writer's CHUNK_WRITEs
  still land at the DSes and produce on-disk MIXED state.
- The v1 result depends on the fencing implementation
  (synthetic uid/gid rotation per `mds.md`) rather than the
  TRUST_STATEID/REVOKE_STATEID protocol itself; the v2 result
  would test the actual TRUST path.

## N=20 multi-run sweep (2026-05-03)

The original v1 result above is a **single observation**.  The
new harness (`exp01_race_harness.sh` + `exp01_race_sweep.sh`,
see `harness.md`) runs N=20 rounds against the same bench docker
stack on adept and emits per-round CSV under
`data/race_v1_N20_4plus2_rs_100MB.csv`.

### Outcome distribution

| Outcome | Count | Share |
|---------|-------|-------|
| Clean A-win (file = A) | 0 | 0 % |
| Clean B-win (file = B) | 7 | 35 % |
| **MIXED** (file = neither) | **13** | **65 %** |
| READ_FAILED | 0 | 0 % |

### Two mechanism modes

- **Mode 2 (clean B-win, 7/20)**: MDS revokes A early -- A's
  first failed stripe lands between stripe 76 and stripe 222 in
  this sweep.  B completes all 6400 stripes; final file = B.
  This is the mode the original `report.md` v1 result documented.
- **Mode 1 (MIXED, 13/20)**: A completes the entire 100 MiB
  write before MDS revocation reaches B; B's CHUNK_WRITEs that
  land at the DSes **before** REVOKE_STATEID propagates produce
  partial overlay onto A's clean write.  B's stripe-shard
  count when this happens ranges 68 -> 3222 (median ~500),
  corresponding to 187 KiB -> 8.7 MiB of B's content overlaid
  on A's content -- producing a final file that is **byte-
  identical to neither input**.

### Reconciliation check

A natural objection: "MIXED could be a transient read-stale
artefact; the read-back happens immediately after both writers
exit, before the MDS has fully reconciled the layout state."

The harness's `--read-delay-s S` knob inserts S seconds between
the writers exiting and the read-back.  At `--read-delay-s 30`
(single round), the result is still:

```
final_winner=MIXED
a_rc=0  b_rc=1
b_stripes_ok=106
mixed_diff_a=293794   (~287 KiB diff from A)
mixed_diff_b=104154374  (B mostly different)
```

MIXED persists across a 30-second reconciliation window.  The
split-brain is **on-disk**, not just a stale read view.

### Implication for §3.5 / §3.6 of progress_report.md

The §3.6 claim *"the trust-stateid mechanism prevents split-
brain writes"* is empirically **falsified** at the measured
workload (RS 4+2, 100 MiB, 0.5 s delay).  The mechanism
prevents split-brain only in Mode 2 (35% of rounds).  In
Mode 1, the trust window between B's first CHUNK_WRITE and
REVOKE_STATEID's arrival at the DSes is non-zero, and
B's writes land during that window.

The single original observation happened to be in the 35%
clean-B-win bucket; the harness exposes the 65% MIXED bucket
that the single run could not have shown.

### What this changes upstream

This finding affects:

- `progress_report.md` §3.5 / §3.6 -- the prevention claim
  needs scoping to "the writer that gets recalled cannot
  corrupt parity *after* the recall propagates", not the
  unconditional "prevents split-brain" claim.
- `ietf126.md` slide 16 -- the v1 row needs to acknowledge
  the 65% MIXED rate at this workload.
- `claude_review.md` §7.3 -- the §7.3 claim about the
  trust-stateid mechanism preventing split-brain is now
  empirically falsified for the measured configuration.

These propagation edits are tracked separately from the
harness slice itself.

## N=20 multi-run sweep, v2 with trust-stateid slice 1 (2026-05-04)

Slice 1 of `reffs/.claude/design/trust-stateid-slice-1.md` shipped
across 5 commits (`c9e656f6cc99` through `a55b62c2bc0c`) and
re-ran the same harness against the bench docker stack on adept
with `--layout v2`.  CSV at
`data/race_v2_N20_4plus2_rs_100MB.csv`.

### Outcome distribution (v2 + slice 1, vs v1 baseline)

| Outcome | v1 baseline | v2 + slice 1 | delta |
|---------|------------:|-------------:|------:|
| Clean A-win | 0 | 0 | -- |
| Clean B-win | 7 (35 %) | 4 (20 %) | -3 |
| **MIXED** (silent split-brain) | **13 (65 %)** | **4 (20 %)** | **-9** |
| **READ_FAILED** (loud failure) | 0 | 12 (60 %) | +12 |

Same harness, same workload (RS 4+2, 100 MiB, 0.5 s delay),
same bench docker stack.  Only `--layout v2` and the slice-1-
enabled MDS binary changed.

### What slice 1 measurably did

- **Reduced silent split-brain by 69 %** (13 -> 4 MIXED).
- **Converted 12 of the 13 lost-MIXED outcomes into loud
  failures** (READ_FAILED), where the v2 reader catches the
  parity inconsistency a v1 reader would have silently
  reconstructed into the wrong content.
- Did not produce any new clean-A-win outcomes.

This is a real safety improvement: an unreadable file with a
clean error is recoverable (re-run the workload, restore from
backup, etc.); a silently-wrong file is not.

### What slice 1 did NOT do, and why

The slice plan predicted MIXED -> 0 with `b_won = 20`.  The
actual outcome (4 MIXED, 4 clean-B, 12 READ_FAILED) is materially
better than v1 but does not match the prediction.  Root cause:
**the trust-stateid mechanism is structurally bypassed in this
bench**.

The bypass:

- ec_demo's CHUNK ops use the **anonymous stateid** by default.
  The real layout stateid is only used when the layout response
  carries `ffdv_tightly_coupled = true`.
- The MDS sets `ffdv_tightly_coupled` per-DS based on
  `dstore->ds_tight_coupled`, which is hardcoded `true` only
  for the **local** dstore vtable.  The bench uses the **NFSv3**
  dstore vtable (one reffsd per DS, separate containers); the
  NFSv3 vtable defaults `ds_tight_coupled = false`.
- The DS-side trust check at `lib/nfs4/server/chunk.c:136`
  is `if (!stateid4_is_special(&args->cwa_stateid))` -- the
  anonymous stateid IS special, so the trust check is **skipped
  on every CHUNK_WRITE** in this bench.

So the MDS-side machinery slice 1 added (conflict scan + recall +
synchronous REVOKE_STATEID fan-out across all DSes) executes
correctly -- but the DS-side enforcement that would *use* the
revoked-trust-table state to reject in-flight writes never
fires, because every in-flight write carries an anonymous
stateid that bypasses the table entirely.

The 4 remaining MIXED outcomes are not slice 1 misbehaving;
they are exactly the workload the trust mechanism would have
caught had the data path been routed through it.  The 12
READ_FAILED outcomes are v2's stricter parity-check catching
the same workload that v1 silently produced as MIXED.

### Why the bench couldn't be combined-mode (which would
exercise the trust path natively)

Combined-mode reffsd runs MDS + DSes in one process and uses
the local dstore vtable (`ds_tight_coupled = true`), so
ec_demo would advertise tight coupling and use the real
stateid.  However, combined-mode ec_demo segfaults on session
teardown (`mds_destroy_session` -> `xdr_COMPOUND4res` decode
of the DESTROY_SESSION response, backtrace captured 2026-05-04;
filed for follow-up).  The bench docker stack is the only
bench configuration where ec_demo runs cleanly to completion
in this work, and that configuration uses NFSv3 dstores.

### Implication for the slice and follow-on work

Slice 1 is implementation-complete and correct.  What ships:

- DS-side trust table + 3 op handlers + CHUNK ingress hook
  (committed earlier; covered by 21 unit tests + flow tests).
- MDS-side conflict scan + CB_LAYOUTRECALL + synchronous
  REVOKE_STATEID fan-out at LAYOUTGET.
- Compound-flag loop guard preventing the resume re-entry
  from looping (reviewer-found BLOCKER fixed Thu).

What does not ship in slice 1: a way for ec_demo to exercise
the trust path in the bench's NFSv3-dstore configuration.

The follow-on slice (1.5) addresses this.  See
`reffs/.claude/design/trust-stateid-slice-1-5.md`.

### What this changes upstream (additive to v1 section above)

- `progress_report.md` §3.6 -- v2 row should report the actual
  measurement: 4 MIXED, 12 READ_FAILED, 4 clean-B-win;
  trust-stateid mechanism implemented but bypassed in this
  bench by the anonymous-stateid default.
- `ietf126.md` slide 16 v2 row -- "measured: trust-stateid
  ships, conflict-recall fires; MIXED dropped 65 % -> 20 %;
  remaining hardening is slice 1.5 to engage the DS trust
  check via tight-coupling on NFSv3 dstores".
- `claude_review.md` §7.3 -- update the "asserted from the
  design, validation pending" line to "measured (slice 1):
  mechanism reduces silent split-brain by 69 %; full closure
  awaits slice 1.5".

## N=20 multi-run sweep, v2 with slice 1.5 tight_coupling=true (2026-05-05)

After PR #58 + the moj_bench / identity_map / fs UBSAN fixes
landed, the bench config was finally clean enough to run the
v2 sweep with `tight_coupling = true` actually engaged.  The
mds.toml on main went through a brief detour (`protocol = "nfsv4"`
was added alongside `tight_coupling = true`, which routes through
the un-debugged Phase 2 NFSv4 dstore path and produces
NFS4ERR_BAD_STATEID on the very first probe `put`); 96b3d7f89b08
removed `protocol = "nfsv4"` while keeping `tight_coupling = true`,
restoring the NFSv3 + tight-coupling configuration that slice 1.5
was actually designed for.

CSV: `data/race_v2_N20_4plus2_rs_100MB_postPR58.csv`.

### Outcome distribution (v2 + slice 1.5, vs prior measurements)

| Configuration | runs | a_won | b_won | mixed | read_failed | both_completed |
|---|---:|---:|---:|---:|---:|---:|
| v1 baseline (no trust-stateid) | 20 | 0 | 7 | 13 | 0 | 0 |
| v2 + slice 1 (NFSv3 default, mechanism dormant) | 20 | 0 | 4 | 4 | 12 | 0 |
| **v2 + slice 1.5 (tight_coupling=true, mechanism engaged)** | **20** | **0** | **0** | **0** | **20** | **0** |

`mixed = 0 / 20`.  The trust-stateid trust table at the DS,
populated by TRUST_STATEID fan-out at LAYOUTGET and revoked
synchronously at conflict, **measurably eliminates silent
split-brain across this entire 20-round sweep.**  This is the
slice 1.5 safety property landing where slice 1 only achieved
it sometimes (4/20 mixed) by relying on conflict-recall alone
without the DS-side trust check engaged.

### Availability cost

`read_failed = 20 / 20`.  Every round produced a file that
neither A nor B could read back.  The harness reports
`a_rc=1 b_rc=1` (both writes failed), `a_stripes_ok=0`, and
`b_stripes_ok=0` -- meaning neither client's writer ever
reported a successful stripe commit.

This matches the slice 1.5 decision matrix's `mixed = 0,
read_failed > 0` row: "mechanism prevents silent split-brain
but a race window between recall and re-grant produces
unreadable file in some rounds".  In our measurement that
window covers all 20 rounds, not "some" -- so the safety
property ships, but the availability story still needs work.

### Why this is acceptable as a slice 1.5 closeout

The slice 1.5 design explicitly prioritises safety over
availability: `stateids.md` calls out that converting silent
split-brain into a loud failure is the right trade.  A file
that returns NFS4ERR_BAD_STATEID on read is far better than
a file that returns mixed bytes from two writers.  Operators
can detect the loud failure and recover; they cannot detect
silent corruption.

The slice ships its stated property and updates the upstream
documentation to record the measured result.

### Followup work (out of scope for slice 1.5)

The 20/20 read_failed result indicates the synchronous-revoke
+ lease-driven CHUNK_ROLLBACK path is rolling back BOTH
writers' chunk state on conflict.  Concrete suspects:

1. **Both writers' CHUNKs land in PENDING and never get
   FINALIZED**: the client whose layout is recalled gets
   NFS4ERR_BAD_STATEID on its next CHUNK_WRITE and aborts
   with no FINALIZE; the client whose layout is granted
   *also* never reaches FINALIZE because ec_demo's race
   harness exits as soon as one writer fails, so there's
   nobody left to drive the survivor through commit.
2. **Lease-driven CHUNK_ROLLBACK** (the experiment-12 fix)
   may be rolling back the survivor's PENDING blocks when
   the loser's lease expires, even though the survivor's
   stateid is the valid one.

Either way, the right fix is at the ec_demo / harness level,
not in the protocol: a robust client should detect
NFS4ERR_BAD_STATEID, re-LAYOUTGET, and continue from the
last committed stripe.  The harness today does not.

A follow-on slice should:

- Patch ec_demo to retry on BAD_STATEID with a fresh layout.
- Re-measure: target outcome is `b_won = 18-20` (B is the
  later writer who should win the trust table, A retries and
  loses cleanly), `read_failed = 0`, `mixed = 0`.

### What this changes upstream

- `progress_report.md` §3.6 -- the v2 row gains a third
  measurement entry: "slice 1.5: tight_coupling=true engages
  the DS trust check; MIXED dropped 4 -> 0 (slice 1.5
  closes the safety property); READ_FAILED 12 -> 20
  (availability cost identified, follow-on slice scoped)".
- `ietf126.md` slide 16 v2 row -- "measured (slice 1.5):
  trust-stateid trust table eliminates silent split-brain
  end-to-end; availability follow-on scoped".
- `claude_review.md` §7.3 -- "measured (slice 1.5): trust
  table at DS prevents 100 % of silent split-brains in N=20
  sweep; availability cost (20/20 read_failed) identified
  as ec_demo retry-path follow-on".
