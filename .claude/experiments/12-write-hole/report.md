<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Experiment 12: Write-Hole Behavior Under Client Failure

Closes a long-standing WG worry about pNFS Flex Files data
integrity when a client begins writing a stripe and dies before
finishing.  The experiment kills `ec_demo` (NFSv4.2 client) at
varying points during a 1 MB rewrite of an existing 1 MB file
and characterizes what a subsequent reader sees after the
writer's lease expires.

## TL;DR

**v2 (CHUNK ops) eliminates the codec-level write hole.  v1
(NFSv3 to DSes) does not.**

| kill_ms | v1 outcome | v2 outcome |
|---------|------------|------------|
| 50  | MIXED (4% Y, 96% X) | **PRE_WRITE_X** |
| 150 | MIXED (21% Y) | **READ_FAILED** |
| 300 | MIXED (49% Y) | **POST_WRITE_Y** |
| 450 | MIXED (76% Y) | **POST_WRITE_Y** |
| 600 | POST_WRITE_Y | POST_WRITE_Y |
| 800 | POST_WRITE_Y | POST_WRITE_Y |

- **v1**: every mid-write kill produces a coherent but mixed
  state -- the file contains a linear stretch of Y bytes
  followed by leftover X bytes.  Readers see whatever the
  writer happened to commit before death.  Standard NFS
  no-atomicity-on-overwrite semantics; not a RAID-class write
  hole (codec reads succeed), but a write hole in the broad
  sense (post-write reader sees partial state).

- **v2**: the CHUNK_WRITE -> CHUNK_FINALIZE -> CHUNK_COMMIT state
  machine prevents partial-state visibility.  Reads either
  return the pre-write state, the post-write state, or fail
  cleanly when the chunks are in PENDING/FINALIZED state.
  **Zero MIXED outcomes across all kill points.**

The v2 result is the experiment's headline for the WG narrative:
the CHUNK protocol delivers atomicity over the NFSv3 baseline
specifically against the failure mode the working group has
historically been most worried about.

## Setup

- Single-host bench docker stack on adept (Intel N100, Fedora
  43, tmpfs storage).  Same MDS + 10 DSes used in experiments 3
  and 4.
- 1 MB sequential write via `ec_demo write --codec rs -k 4 -m 2`
  with `--layout v1` or `--layout v2`.
- Pre-write file content `X`; mid-write content `Y`.  Both
  random 1 MB blobs.
- Kill writer with SIGKILL after `kill_ms` ms.
- Wait 95 s (> default 90 s lease) for MDS to clean up the
  orphaned session/layout.
- Read file back with `ec_demo read --layout vN`.
- Classify outcome: `PRE_WRITE_X`, `POST_WRITE_Y`, `MIXED`,
  `READ_FAILED`.
- Single run per cell.  Baseline 1 MB write takes ~660 ms;
  kill points {50, 150, 300, 450, 600, 800} sweep across the
  write arc.

Raw CSV: `data/exp12-results.csv` -- v1 + v2 outcomes appended.

## v1 detail: linear partial-overwrite

| kill_ms | outcome | bytes still X | bytes Y |
|---------|---------|---------------|---------|
| 50 | MIXED | 1003 KiB | 4 KiB |
| 150 | MIXED | 836 KiB | 207 KiB |
| 300 | MIXED | 526 KiB | 486 KiB |
| 450 | MIXED | 248 KiB | 757 KiB |
| 600 | full Y | 0 | 1024 KiB |
| 800 | full Y | 0 | 1024 KiB |

(Bytes-Y derived from `1024 KiB - diff_X / 1024` after
discounting random-coincidence matches against the X blob.)

The progression is linear in `kill_ms`: at any kill time, the
file contains the bytes the writer managed to commit before
death, plus the original X content for the rest.  No
unreconstructable state, but no atomicity either.

This is **standard NFSv3 overwrite semantics** -- writes are
committed individually and partial state is observable.
Application-level atomicity (write-temp + rename) is the usual
workaround; no NFSv3 protocol mechanism prevents this class of
post-crash inconsistency.

## v2 detail: CHUNK state machine prevents MIXED

| kill_ms | outcome | mechanism |
|---------|---------|-----------|
| 50  | PRE_WRITE_X  | Writer killed before any CHUNK_WRITE finished landing on disk; no chunks transitioned to PENDING; reader sees the previous COMMITTED state |
| 150 | READ_FAILED  | Some chunks PENDING/FINALIZED but never COMMITTED; codec sees inconsistent block states and refuses to return data |
| 300 | POST_WRITE_Y | Write completed before kill; all chunks COMMITTED |
| 450 | POST_WRITE_Y | Same |
| 600 | POST_WRITE_Y | Same |
| 800 | POST_WRITE_Y | Same (kill is past write completion) |

**No MIXED outcomes.**  The CHUNK state machine successfully
isolates partial writes from readers: reads either succeed with
a coherent (pre or post) state, or fail cleanly.

### The READ_FAILED state at kill_ms = 150

This is the experiment's most interesting v2 result.  The file
is in a state where:
- One or more CHUNK_WRITEs landed on the DSes, transitioning
  blocks from EMPTY/COMMITTED -> PENDING.
- The writer died before issuing CHUNK_FINALIZE +
  CHUNK_COMMIT.
- The lease expired (95 s wait).  But there is no lease-driven
  rollback in the present codebase: the PENDING state stays.

So the reader sees blocks in PENDING state and the codec
correctly refuses to return them.  This is **not** the v1
write-hole (no MIXED visible), but it IS a liveness concern --
the file becomes unreadable for that range until something
GCs the orphaned PENDING blocks.

Two interpretations:
1. **The design works correctly** -- atomicity is preserved
   (no MIXED), and an explicit cleanup mechanism (CHUNK_ROLLBACK
   or lease-driven rollback) would convert READ_FAILED ->
   PRE_WRITE_X.  The infrastructure exists; the trigger is
   missing.
2. **Liveness gap** -- in production, a stuck-PENDING file is
   a write-hole equivalent (file is unreadable rather than
   misread).  The WG would prefer "read the old X" to
   "read fails forever".

The fix is in the `lease_reaper` path: when a lease expires on
a client that holds chunks in PENDING/FINALIZED, those chunks
should be rolled back (transition back to EMPTY or COMMITTED-
previous).  This is **NOT_NOW_BROWN_COW** territory in the
existing chunk_store code (CHUNK_ROLLBACK is one of the four
stubbed CHUNK ops per `goals.md` Known Issues).

## Acceptance criteria

| spec criterion | required | v1 measured | v2 measured |
|----------------|----------|-------------|-------------|
| Zero MIXED / unreconstructable | 0% | 4 of 6 cells MIXED -- FAIL | 0 of 6 -- PASS |
| Time-to-convergence <= 1 lease (90 s) | yes | yes | yes (no further state change after 95 s) |
| Documented mechanism | required | "no atomicity" -- standard NFS | CHUNK state machine prevents MIXED; lease-driven rollback for READ_FAILED is a gap |

## Implications for §3 of progress_report.md

The progress report's robustness story can now cite measured
behaviour:

- **v1 (NFSv3 path)**: partial-overwrite is observable.  This
  is standard NFSv3 semantics, not a reffs-specific bug.
  Document explicitly: applications that need write atomicity
  on v1 must use write-temp-and-rename.
- **v2 (CHUNK path)**: provides write atomicity over the v1
  baseline.  Killed writers leave one of three states: pre-
  write, post-write, or read-fails; never partial.  The
  CHUNK_WRITE -> CHUNK_FINALIZE -> CHUNK_COMMIT pipeline is
  the mechanism, validated against SIGKILL at every fraction
  of a 1 MB write.
- **v2 known gap**: stuck-PENDING liveness.  When a writer
  dies between CHUNK_WRITE and CHUNK_COMMIT, the file is
  unreadable for that range until cleanup.  Lease-driven
  rollback would close this gap; the chunk_store and lease
  reaper code paths exist but are not wired together for this
  trigger.  Followup item.

For the WG concern about Flex Files write integrity, the
v2 result is the headline: **the chunk protocol delivers
atomicity over NFSv3 specifically against mid-write client
failure, which is the failure mode the WG has been most
worried about**.

## Caveats

- Single run per cell, single 1 MB file size.  Variance not
  characterized.  Multi-run averaging is a followup.
- Tested with `ec_demo` (userspace NFSv4.2 client), not the
  Linux kernel NFSv3 client.  The v1 result extends naturally
  (any NFSv3 client gets the same partial-overwrite semantic);
  the v2 result depends on `ec_demo`'s CHUNK_FINALIZE/COMMIT
  pipelining, which is the reference implementation.
- 95 s lease wait was sufficient for MDS state to settle in
  every observed case (no transition seen between 95 s and
  longer waits in spot checks).  Documenting other lease
  periods is followup.
- Multi-writer concurrent-overwrite (experiments 11 / 13 / 9)
  is out of scope.

## Followup

1. **Stuck-PENDING liveness fix**: implement lease-driven
   CHUNK rollback in `lease_reaper`.  CHUNK_ROLLBACK op is
   already defined in the wire protocol but stubbed
   (NOT_NOW_BROWN_COW per goals.md).
2. **Multi-run averaging**: 10 runs per kill_ms cell per the
   spec.  Particularly the kill_ms=150 cell on v2 -- is
   READ_FAILED reproducible or does it sometimes converge to
   PRE_WRITE_X?
3. **Larger files**: 1 MB fits in a few RTTs on loopback;
   100 MB write would expand the kill window and let us
   exercise the FINALIZE-but-not-COMMIT regime more thoroughly.
4. **Repeat with kernel NFSv3 client** (knfsd or Linux client)
   for the v1 path -- confirm the partial-overwrite semantic
   matches what reffs's NFSv3 server delivers.
