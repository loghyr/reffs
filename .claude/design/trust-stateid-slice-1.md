<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# trust-stateid slice 1: close the harness gap

The comprehensive design lives in [`trust-stateid.md`](trust-stateid.md).
This document is the **slice-1 plan**: the minimum work that
turns the exp 01 harness's measured 13/20 MIXED rate on v1 into
a v2 measurement against the same harness with TRUST_STATEID
mitigation enabled.

Reviewer-requested framing: be honest about what is already
shipped vs what is genuinely new.  The earlier draft of this
plan listed a five-day day-by-day grid for work that is mostly
on the branch already; this rewrite separates the two.

## What ships

### Already on `xdr-proxy-stateid` (and on `main` after today's push)

The DS-side and most of the MDS-side trust-stateid mechanism is
in tree.  Verified at the time of writing against the actual
files:

| Piece | Location |
|-------|----------|
| `nc_exchgid_flags` field on `struct nfs4_client` | `lib/nfs4/include/nfs4/client.h:49` |
| TRUST_STATEID / REVOKE_STATEID / BULK_REVOKE_STATEID XDR + handlers | `lib/xdr/nfsv42_xdr.x`, `lib/nfs4/server/trust_stateid.{c,h}` |
| In-memory trust table (Rule-6 lifecycle) | `lib/nfs4/server/trust_stateid.c` |
| CHUNK_WRITE / CHUNK_READ ingress trust check | `lib/nfs4/server/chunk.c:136-167`, `:319-350` |
| Dispatch registration | `lib/nfs4/server/dispatch.c` |
| `dstore_ops` extension (`probe_tight_coupling`, `trust_stateid`, `revoke_stateid`, `bulk_revoke_stateid`) | `lib/include/reffs/dstore_ops.h:81-97, 208-212` |
| Local-dstore impl (direct trust-table calls, no RPC) | `lib/nfs4/dstore/dstore_ops_local.c:272-369` |
| `FANOUT_TRUST_STATEID` op + synchronous LAYOUTGET fan-out (three call sites) | `lib/nfs4/dstore/dstore_fanout.h:45,62`; `lib/nfs4/server/layout.c:1240,1421,1723` |
| ec_pipeline tight-coupling stateid switch | `lib/nfs4/ps/ec_pipeline.c:139,224,266` (passed through to ec_demo) |
| Unit-test coverage | `lib/nfs4/tests/trust_stateid_test.c` -- 42 START_TEST cases (Groups A-J, including the J group's renewal cases that are formally out-of-slice) |

### Genuinely new in this slice

Two pieces, in order of dependency:

1. **Conflict-driven CB_LAYOUTRECALL + REVOKE_STATEID at LAYOUTGET.**
   Today, `nfs4_op_layoutget` does **not** check whether other
   clients hold layout stateids on the same inode.
   `nfs4_cb_layoutrecall_fnf()` exists (`lib/nfs4/server/cb.c:295`)
   but is only called from `migration_record.c:898`.  The new
   wiring:
   - On a LAYOUTGET that would grant a writable layout to a new
     client, scan the inode's existing layout stateids.  For each
     stateid held by a *different* client:
       - Fire `nfs4_cb_layoutrecall_fnf()` to that client.
       - In parallel (per-DS fan-out) issue REVOKE_STATEID for
         the prior client's stateid.  This is the load-bearing
         step: the local-DS trust table entry for the prior
         client must be removed *before* the new client's
         TRUST_STATEID is issued, otherwise both stateids
         co-exist in the trust table and the gap stays open.
     `task_pause` until either every DS has acked the REVOKE or
     a bounded timeout fires.
   - Only after the REVOKE fan-out completes, proceed with the
     existing TRUST_STATEID fan-out for the new client and
     return the layout.

   Conservative scope for slice 1: synchronous wait, single
   timeout knob (default = lease period / 4), no
   delegation/layout interaction beyond what already exists.
   No client-state recovery if the recall ack never arrives --
   we revoke regardless and let the prior client see
   `NFS4ERR_BAD_STATEID` on its next CHUNK op.

2. **Harness Friday plumbing.**
   - Smoke `--layout v2` against the bench docker stack on
     adept (single round, RS 4+2, 100 MiB).  This is the
     gating unknown for the slice -- the 2026-05-03 bisect
     showed v2 LAYOUTGET succeeds on standalone reffsd with 6
     dstores at 10 / 50 / 100 MiB clean, but the bench docker
     stack with 10 dstores has not been independently
     verified.
   - Sweep wrapper invocation parameterised to write
     `data/race_v2_N20_4plus2_rs_100MB.csv` (as opposed to
     the v1 file already in tree).
   - Append the v2 result to
     `.claude/experiments/01-trust-stateid-window/report.md`.

## What does not ship

- **NFSv4 dstore vtable (Phase 2 Step 2.2 of `trust-stateid.md`)**:
  bench is single-host; local vtable suffices.
- **Capability probe (Phase 2 Step 2.4)**: hardcoded
  `ds_tight_coupled = true` for local dstores.  Per-DS
  probing is for the cross-host slice.
- **`ffdv_tightly_coupled` per-DS resolution at GETDEVICEINFO
  (Phase 2 Step 2.5)**: hardcoded `true` for local-DS devices.
- **MDS-to-DS session flag (`USE_NON_PNFS` vs `USE_PNFS_MDS`)**:
  known NOT_NOW_BROWN_COW from `dstore-vtable-v2.md`.
  Irrelevant for combined mode (local vtable bypasses the
  wire); will surface as `NFS4ERR_PERM` at every DS the moment
  the harness extends to a real cross-host MDS+DS.  Tracked
  separately.
- **Kerberos principal binding (`te_principal`)**: AUTH_SYS
  only; existing trust table already supports the empty-string
  principal case.
- **TRUST_STATEID renewal (Phase 2 Step 2.8)**: out of scope.
  Harness uses fresh client-ids per round; lease never expires.
- **Trust-table persistence across restarts**: in-memory only
  (Open Question 4 of `stateids.md`).
- **v1 path (NFSv3 fencing)**: unchanged.  The v1 baseline IS
  the comparison.
- **ec_demo refactor into `ec_pipeline` (`proxy-server.md`
  Phase 0)**: separate slice.
- **PS path measurement**: separate slice.  See PS section
  below.
- **Cross-host correctness measurement**: exp 6 territory; not
  in slice 1.
- **Heavy-conflict performance characterisation**: separate
  measurement, same harness apparatus, varied parameters.

## The gap that needs closing

The exp 01 harness measures, on the single-host bench docker
stack on adept (RS 4+2, 100 MiB, 0.5 s delay):

| Outcome | Count | Share |
|---------|-------|-------|
| Clean A-win | 0 | 0 % |
| Clean B-win (file = B) | 7 | 35 % |
| **MIXED (on-disk split-brain)** | **13** | **65 %** |

MIXED persists across a 30 s read-back delay
(`--read-delay-s 30`).  This is on-disk corruption, not a
stale-read artefact.

### Mechanism, corrected

The earlier draft of this plan asserted "B's LAYOUTGET arrives;
MDS rotates the file's synthetic uid on the DSes (fencing) and
returns LAYOUTGET to B."  That is **not** what
`nfs4_op_layoutget` currently does.  Cross-checking
`lib/nfs4/server/layout.c:870-985`:

- `dstore_data_file_fence()` is invoked only on the **first**
  LAYOUTGET that has to populate `i_layout_segments` from the
  runway pool.
- Once `lss->lss_count > 0`, the fast path at `:888-891` reuses
  the existing segments and never re-fences.
- Subsequent LAYOUTGETs (B, in the harness) reuse A's
  `ldf_uid` / `ldf_gid`.
- v1 fencing in the current implementation only re-fires on
  LAYOUTERROR (`layout.c:1786 layouterror_fence_and_revoke`)
  and on CB_RECALL timeout.

So v1 has **no per-LAYOUTGET fencing at all** in the current
code on the conflict path.  The only sites that do fence are
the first-LAYOUTGET runway-pop branch (`layout.c:969`),
LAYOUTERROR-driven `layouterror_fence_and_revoke`
(`layout.c:1786`), and CB_RECALL timeout.  None of those fire
just because B's LAYOUTGET arrives.

This raises -- but does not yet answer -- two questions the
harness CSV poses:

- In Mode 2 (35 % of rounds), Client A demonstrably fails at
  a specific stripe (`a_first_fail_stripe` 76-222 in the v1
  N=20 sweep).  Some mechanism rejects A.  Candidates: an
  earlier LAYOUTERROR from one of A's writes returning an
  error that triggers `layouterror_fence_and_revoke`, a
  CB_RECALL-timeout fence, or some path the slice has not yet
  enumerated.  The slice does not pre-commit to which.
- In Mode 1 (65 % of rounds), A completes its full write and
  B errors out (`b_rc=1`) after writing some partial number
  of stripes.  The MIXED diff_a values range 187 KiB to
  8.7 MiB.  An "in-flight CHUNK_WRITEs during a hypothetical
  REVOKE arrival" model predicts at most ~24 KiB per round
  (6 DSes x one in-flight 4 KiB shard each); the observed
  range is three orders of magnitude wider.  Either the
  in-flight bound is much higher than that simple model
  predicts (TCP send-buffer queueing, multi-stripe pipelining
  inside the client, etc.), or the overlay is being produced
  by a mechanism unrelated to in-flight-during-revoke
  (runway-pool collisions on the shared `race_target_$N`
  path, chunk-state collisions, retry amplification, etc.).
  The slice does not pre-commit to which.

Both questions are the Mon-AM instrumentation half-day's
job to answer before any code changes land.  If the answers
reframe the gap, the slice gets revised before charging into
the implementation.

The TRUST_STATEID mechanism closes this by adding what is
currently absent: an **explicit, per-LAYOUTGET conflict
detection** at the MDS, an **explicit per-stateid trust table**
at each DS, and an **explicit synchronous REVOKE_STATEID**
fan-out at the moment of conflict.  None of these are present
in the v1 path.  The v2 path has the trust table and the
TRUST_STATEID fan-out today, but it does not have the
conflict-detection step at LAYOUTGET -- which is item 1 of
the slice's "genuinely new" list above.

This is why the slice is necessary even though so much of the
trust-stateid machinery is already in tree: the
conflict-detection step is the load-bearing piece, and it is
the only piece missing.

## Slice plan, day-by-day

| Day | Work | Tests |
|-----|------|-------|
| Mon AM | **Instrumentation half-day before any code lands.**  Two questions to answer first: (a) what NFS3ERR_* code is rejecting Client A in the 35 % "clean B-win" rounds, and which code path fires it (`layouterror_fence_and_revoke`, CB_RECALL timeout, runway exhaustion, or something else); (b) what produces the overlay-size distribution in MIXED rounds (187 KiB-8.7 MiB observed; the in-flight-during-REVOKE model predicts ~24 KiB).  Capture: a 5-round mini-sweep with DS-side log of CHUNK_WRITE accept/reject by stateid + per-stripe error code; per-DS LAYOUTERROR receipts; runway-pool acquire/release log.  Falsification criteria: question (a) is "answered" when one of the candidate code paths is observed firing in at least 4 of 5 rounds (or a previously-unconsidered candidate is identified).  Question (b) is "answered" when the overlay-size distribution maps to one of these signatures: (i) overlay <= ~24 KiB and B's accepted-stripe timestamps fall within ~RTT of B's LAYOUTGET = pure in-flight-during-REVOKE; (ii) overlay scales with B's LAYOUTGET-to-error wall time = retry amplification or runway-collision; (iii) overlay matches the size of B's first error-burst suffix = chunk-state collision on shared FH.  If neither (a) nor (b) is answered by the mini-sweep, extend Mon-AM to a full Mon-day before Tue starts. | n/a (measurement) |
| Mon PM | Conflict detection scan in `nfs4_op_layoutget`: enumerate sibling layout stateids on the inode; identify the prior-client set. | unit test for the scan: setup multiple stateids on one inode, verify the new-client branch returns the right prior-client set. |
| Tue | CB_LAYOUTRECALL fan-out to prior clients via the existing `nfs4_cb_layoutrecall_fnf()`; REVOKE_STATEID fan-out via the existing `dstore_revoke_stateid` vtable op + `dstore_fanout`; `task_pause` until completion or bounded-timeout. | unit test: prior client gets recall and trust-table entry is gone before TRUST_STATEID for new client fires. |
| Wed | Lease-period / 4 timeout; stateid-not-acked-by-DS handling (continue regardless and document the gap); LAYOUTGET-during-LAYOUTGET race handling (lock ordering: confirm `i_attr_mutex` covers the conflict scan). | lifecycle test under TSAN: two concurrent LAYOUTGETs on the same inode; no UAF; observed serialisation order matches the lock contract. |
| Thu | Full reviewer pass on the new code (lock ordering, RCU, ref-counting); fix-style; license; `ci-check`. | existing `make check` must pass. |
| Fri | `--layout v2 --runs 1` smoke against bench docker stack on adept; if clean, full N=20 sweep; otherwise pivot per decision matrix. | n/a (measurement) |

LOC budget: ~250-400 in `nfs4_op_layoutget` + helpers, ~80
in tests.  Lock ordering and lifecycle make this reviewer-
agent territory (per `standards.md`'s gating rules: "lock-
ordering / locking-discipline changes" and "RCU or
ref-counting lifecycle changes" both trigger reviewer).

The day-by-day grid is approximate.  Wed-Thu may telescope
or extend depending on what surfaces during the lifecycle
work.  The Friday measurement does not slip later than one
calendar week from Mon.

## Test impact on existing tests

For the actually-changing surface only:

| Test | Impact | Why |
|------|--------|-----|
| All `make check` tests including `chunk_test`, `trust_stateid_test`, `dstore_*_test`, `layout_test` | Should PASS unchanged | The new code adds a path inside `nfs4_op_layoutget` that fires only when sibling layout stateids exist; existing single-client tests do not exercise it.  Verify on Thursday before Friday measurement. |
| LAYOUTGET tests using `dstore_mock` | Verify, do not assume | The new conflict path adds a synchronous fanout call.  Mocks must complete the fanout synchronously; if the mock is fire-and-forget, the new test path will hang. |
| `reflected_getattr_test` | Spot-check | Reflected GETATTR fan-out shares the `dstore_fanout` machinery; the new REVOKE fan-out should not collide, but worth verifying on Thursday. |

Existing trust-stateid Group A-E unit tests (now in tree) are
not affected -- the slice does not change the trust-table API
or the op handlers; it only adds a new caller of those APIs
inside `nfs4_op_layoutget`.

## Decision point at end of Friday

Smoke first, sweep only on smoke pass.

| Result | Action |
|--------|--------|
| `--layout v2 --runs 1` smoke FAILS | Pivot to v2 LAYOUTGET trigger isolation against the bench docker stack.  Defer the sweep.  Estimate: +1 week. |
| Smoke passes; full sweep `mixed = 0`, `b_won >= 18` (allowing 1-2 read-failed for noise) | Slice succeeded.  Append `data/race_v2_N20_4plus2_rs_100MB.csv` to the experiment dir; update `report.md` with the v2 result AND correct the v1 TL;DR (currently still describes the original single-observation v1 result as "no MIXED state"; the N=20 sweep contradicts it); the `ietf126.md` slide-16 v2-row update is a separate slice (deck-affecting). |
| Smoke passes; full sweep `1 <= mixed <= 5` | Conflict-recall path is **mostly** load-bearing.  Most likely cause: REVOKE fan-out completes before all in-flight CHUNK_WRITEs from the prior client have been processed by the DSes; the trust-table check at the DS sees the entry already removed but the prior-client write was already past the check.  Add 2-3 days for instrumentation (DS-side log of accepted-vs-rejected CHUNK_WRITEs by stateid) + fix. |
| Smoke passes; full sweep `6 <= mixed <= 12` | Conflict-recall path is **partially** load-bearing -- the fix is doing something but not enough.  More likely cause: a second mechanism beyond in-flight-during-revoke (re-entry of the prior client's writes after a timeout, runway-pool overlap, or chunk-state collision still active).  Pivot to root-cause: extend Mon-AM's instrumentation to a full DS-side per-write log across the failing rounds, identify the second mechanism, and decide whether to extend the slice or open a follow-up. |
| Smoke passes; full sweep `mixed >= 13` (no improvement vs v1 baseline) | The conflict-recall path is **not** load-bearing for the gap as it currently exists.  The Mon-AM instrumentation must already have surfaced what produces the MIXED overlay; cross-check whether the implemented mechanism (REVOKE on conflict at LAYOUTGET) actually addresses that producer.  If not, the slice's premise was wrong; convene to re-plan. |

## Files that will change in this slice

- `lib/nfs4/server/layout.c` -- conflict scan + recall + REVOKE
  fan-out in `nfs4_op_layoutget`.
- `lib/nfs4/include/nfs4/layout.h` (or equivalent) -- any new
  helper signature.
- `lib/nfs4/tests/layout_*` -- new conflict-recall tests.
- A v2 invocation of `exp01_race_sweep.sh` (the wrapper
  already takes `--layout`); a thin Friday-runner can either
  call it directly or wrap it with the bench-stack-specific
  docker-compose plumbing pattern that `_wrap_sweep.sh`
  established for the v1 run.  No changes to
  `exp01_race_harness.sh` or `exp01_race_sweep.sh` needed.
- `.claude/experiments/01-trust-stateid-window/report.md` --
  Friday-evening append of v2 result.
- `.claude/experiments/01-trust-stateid-window/data/race_v2_N20_4plus2_rs_100MB.csv`
  -- Friday measurement output.

XDR files: not touched.  Probe protocol: not touched.  On-disk
format: not touched.  Wire-affecting changes: none.

## PS interaction (out of scope for slice 1, forward-looking)

The harness uses ec_demo directly against MDS+DSes.  No PS in
the data path.  The interaction model when the PS path is
exercised in a future slice:

1. **PS as trust-stateid consumer.**  When an end-client writes
   through the PS, the PS does its own LAYOUTGET against the
   MDS carrying the end-client's forwarded credentials.  The
   MDS issues the PS a layout stateid and fans out
   TRUST_STATEID for that stateid -- same machinery as any
   other client.  The DS trust-table check at CHUNK_WRITE
   ingress does not need to know whether the caller is a PS
   or an end-client; it just checks the stateid.
2. **PROXY_REGISTRATION does not pre-trust the PS** per
   `proxy-server.md`'s design intent.  Slice 1 does not
   implement that policy; it is on the proxy-server slice.
   The slice-1 trust table is the *enabler* for the future PS
   short-circuit safety check (the
   `trust_stateid_find()`-before-`db_write` path described in
   `proxy-server.md` Phase 5,
   `test_shortcircuit_rejects_unknown_stateid`).
3. **Cross-PS coherence** is the same problem one level up.
   Two PSes proxying the same MDS each get their own layout
   stateids; MDS REVOKE_STATEID against either propagates
   normally; the same gap-close holds.  Latency at scale is
   exp 5 territory.

When the PS path is bench-runnable, the harness extends with
a `--via-ps HOST[:PORT]` flag (one-line ec_demo config change)
and the same v2 sweep runs against the PS path.  Expected null
hypothesis: same MIXED-rate behaviour as the direct path,
since trust-stateid sits below the PS in the architecture.
If they differ, that is the finding.

## Companion document

The framing of this slice in terms of which IETF reviewers'
recurring questions it answers, and the standards-body framing
of "wire contract vs implementation", is kept in a private
companion document outside this repo.  This file stays
mechanism / measurement / RFC; the named-reviewer framing is
not in the public reffs repo.
