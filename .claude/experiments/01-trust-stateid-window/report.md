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

- Single race observation; no statistical distribution.
- Latency-budget claim from the spec ("p99 < 10 ms") is not
  measurable from client side alone.  This experiment confirms
  the **correctness** half of the §3.5/§3.6 claims; the
  **bound** half awaits Variant B instrumentation.
- The v1 result depends on the fencing implementation
  (synthetic uid/gid rotation per `mds.md`) rather than the
  TRUST_STATEID/REVOKE_STATEID protocol itself; the v2 result
  would test the actual TRUST path.
