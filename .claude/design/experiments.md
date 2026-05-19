<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# reffs Experiments & Investigations Index

A living tracker for measurement-driven investigations and
validation harnesses.  This work spans three places -- the IETF
draft (`draft-haynes-nfsv4-flexfiles-v2`), the reffs reference
implementation, and the `deploy/benchmark/` suite -- and an index
keeps the threads linked so a question raised on the WG list can
be traced to the harness that answers it.

Each entry: **status**, the design doc, the origin (why it
exists), and what would close it.

Status legend: PLANNED / DESIGNED / IN PROGRESS / SHIPPED /
BLOCKED.

---

## Group A: chunk-collision validation

Stress the CHUNK state machine, CRC bookkeeping, layout-fence
rotation, and trust-stateid invariants under concurrent writes to
the same chunk from distinct NFSv4.2 clientids.  Master design:
[`chunk-collision-validation.md`](chunk-collision-validation.md).

| ID | What it does | Design | Status |
|----|--------------|--------|--------|
| **T1** | N concurrent `ec_demo write/verify` instances, one MDS file, distinct `--id` per writer | `chunk-collision-validation.md` | **SHIPPED** -- harness `deploy/benchmark/run_chunk_collision.sh`; per-sb chunk counters + probe surface landed (BLOCKER 2); reviewer verdict #2 done |
| **T1b** | `ec_demo --offset/--length` partial-range writes -- sub-chunk byte interleave | `chunk-collision-validation.md` (Track 1b) | **PLANNED** -- ~150 LOC `ec_demo` extension; not started |
| **T2** | IOR shared-file write+verify via N Proxy Server containers; N PSes = N clientids on one shared MDS file | [`chunk-collision-track2.md`](chunk-collision-track2.md) | **IN PROGRESS** -- harness + image done; staggered bringup verified clean; run 4 (2026-05-19) reached IOR but IOR write+verify fails in the PS proxy path -- see INV-6 |
| **T3** | Linux NFS client direct-to-MDS, as a no-EC-conflict sanity baseline | `chunk-collision-validation.md` (Track 3) | **PARTIAL** -- covered by existing CI git-clone-over-NFS |

---

## Group B: IETF -04 review-driven investigations

Origin: the `draft-haynes-nfsv4-flexfiles-v2-04` adoption thread on
the IETF nfsv4 WG list, 2026-04-22 through 2026-05-19.  Christoph
Hellwig is the blocking reviewer; Thomas Haynes conceded on-list
(msg 12) that "the draft is not going forward" until the
client-EC-awareness disagreement is resolved.  Actor-stance
detail and full provenance live in the private
`reffs-docs/christoph.md`; this index tracks only the items that
the **reffs implementation** can answer with measurement.

| ID | Question | Origin | Status |
|----|----------|--------|--------|
| **INV-1** | What I/O pattern does a partial-stripe write produce on the DSes, and how much fragmentation does it cause (DS filesystem + SSD FTL)? Can partial-stripe writes be reduced to full-stripe writes cheaply? | Hellwig msg 5 (in-place update semantics) + msg 9 (NFS block size); Haynes committed msg 13 to "answer the questions you have raised" using the reference implementation's POSIX-DS interfaces | **IN PROGRESS** -- see "INV-1 plan" below |
| **INV-2** | How much of the control path is exposed to clients, and can the EC-aware client be kept thin? | Hellwig msg 5 objection 1 ("wide exposure of the control path to the clients") | **PLANNED** -- design-level, not a measurement; needs a written answer, not a harness |
| **INV-3** | Should block-level at-rest checksums be a core NFS feature instead of a layout-type detail? | Hellwig msg 2 + msg 6; Chuck Lever msg 7 agrees integrity-at-rest should be core | **PLANNED** -- draft-restructure question; tracked in `reffs-docs` |
| **INV-4** | Are the `CHUNK_*` ops a de-facto new storage protocol that should not be presented as a general NFSv4.1+ filesystem? | Hellwig msg 6 | **PLANNED** -- draft-framing question; Haynes msg 12 open to defining them inside the layout type |

### INV-1 plan (the load-bearing measurement)

INV-1 is the one Christoph objection that reffs can answer with
numbers rather than prose, and it is the explicit on-list
commitment.  The vehicle already exists:

- **chunk-collision T1b and T2 exercise exactly the partial-stripe
  write path.**  T1b drives sub-chunk byte interleave directly;
  T2 drives `IOR -F 0` shared-file writes through N PSes, whose
  per-stripe RMW path (proxy-server Phase 4b) is the partial-stripe
  write under test.
- The missing piece is **DS-side instrumentation**: during a T1b
  or T2 run, measure the write pattern the DSes actually receive
  -- offset/length distribution, how often a CHUNK_WRITE is a
  full chunk vs a partial overwrite, and the resulting on-disk
  fragmentation of the DS data files.
- Closing INV-1 = a short measured report (write-pattern
  histogram + fragmentation delta) that can be cited back to the
  WG list, plus the prose answer on whether variable stripe
  geometry or MDS hand-off reduces partial writes to full writes.

INV-1 is therefore **gated on T2's first clean run** (it reuses
the same harness + topology) and on adding the DS-side
write-pattern instrumentation.  When T2 ships, INV-1's
measurement step is a thin add-on.

---

## Group C: bugs surfaced by the harnesses

Findings the validation harnesses turned up -- the payoff of
running them.  Each is a real defect, tracked here until fixed.

| ID | Defect | Surfaced by | Status |
|----|--------|-------------|--------|
| **INV-5** | Concurrent mTLS session establishment from multiple PSes to one MDS races `mds_session_create_tls` and most sessions fail with `EIO` (`Input/output error -- listener stays dark`).  With 4 PSes cold-starting together, Track 2 runs lost 1-of-4 and 3-of-4 sessions on two of three attempts. | Track 2 ([`chunk-collision-track2.md`](chunk-collision-track2.md)), 2026-05-19 | **CLOSED 2026-05-19 by Stage 3 Slice 1.**  Stage 4 re-run on dreamer (NPS=4, even with the 4-second stagger still in place) shows 0-of-4 lost sessions and MDS grants matching expected `NPS`.  Verified with the safe-API repro `conn_lifecycle_race_test` green under TSAN.  The 4-second stagger in `run-ps-bench-bringup.sh` is now belt-and-braces -- consider revisiting once Slice 3 lands. |
| **INV-6** | A real kernel NFS client running IOR shared-file write+verify through the PS proxy path fails: `fsync()` warns "failed" and `stat()` on the proxied file errors (`aiori-POSIX.c:866`), aborting IOR.  The PS proxy data/metadata path does not survive an IOR `-w -r -W -R -e` shared-file workload. | Track 2 run 4, 2026-05-19 -- the first run to reach IOR (topology + harness now clean) | **NARROWED again by Slice 3, STILL OPEN (Stage 4 run 6, 2026-05-19).**  Affected-PS count narrows monotonically across slices: run 4 (no fix) 4-of-4, run 5 (Slice 1) 2-of-4, run 6 (Slice 3) 1-of-4.  Zero ASAN/UBSAN across all runs since Slice 1.  But IOR still aborts with the same `fsync(15) failed` + `stat()` cascade, and the MDS log still shows `Connection not tracked for fd=N` AND the same `SSL_ERROR_ZERO_RETURN` (alert error:00000000) sequence.  The "Track 2 run 6 timeline" subsection below shows the PS sees EIO BEFORE any close_notify -- the close_notify is the consequence of the PS deciding its upstream is dead, not the cause.  Slice 4 (`CONN_CLOSING` + generation-check extension) is the natural next step: the "Connection not tracked for fd=N" line is exactly the symptom Slice 4 was designed to address. |

### INV-5 characterization (2026-05-19, first dig)

From `mds_session_create_tls` (`lib/nfs4/client/mds_session.c:1018`):

- The 4 PSes are separate processes -- INV-5 is **not** a
  shared-memory race within one reffsd.
- The TLS *transport* comes up: `mds_session_clnt_open_tls`
  returns a non-NULL client (a handshake failure there returns
  `-ECONNREFUSED`, not the observed `-EIO`).
- The `-EIO` propagates out of `mds_exchange_id()` or
  `mds_create_session()` -- the **first RPC(s) over the freshly
  established TLS session fail**, not the handshake itself.
- So under N concurrent STARTTLS connects the TLS session
  establishes but EXCHANGE_ID / CREATE_SESSION over it dies.
  Prime suspect: the **MDS-side RPC-over-TLS accept path**
  (`lib/io/tls.c` plus the io_uring accept loop) racing
  per-connection TLS state when several handshakes land at once.

Root-causing the MDS accept path -- and the fix -- is a separate
slice; this records the finding and the first dig.

### INV-6 characterization (2026-05-19, dig)

Run-4 logs (PS-0 / PS-3 + MDS) show INV-6 is a cascade -- and the
same fault family as INV-5.

Timeline (run 4):

- `17:45:56`  IOR begins its write phase through the 4 PS mounts.
- `17:46:00`  MDS io layer: `CQE error for op=WRITE, fd=24:
  Broken pipe`, plus `SSL error 6` (`SSL_ERROR_ZERO_RETURN` --
  TLS connection closed) on the PS connections.  **The PS<->MDS
  TLS connections break ~4 s into IOR's write load.**
- `17:46:00-02`  each PS: `upstream session is dead (errno=EIO)`
  -> forces a reconnect.
- All 4 PSes reconnect at once -> the **INV-5** concurrent-mTLS
  race -> reconnect fails repeatedly (`Input/output error`) for
  ~90 s (exp backoff 0/1/2/4/8/16 s).
- `17:46:38`  MDS: `io_rpc_trans_cb: Connection not tracked for
  fd=21` -- an fd-lifecycle / connection-tracking confusion.
- `17:47:45-53`  PSes finally reconnect -- far too late; IOR
  aborted long before.

So INV-6 is two faults:

1. **The established PS<->MDS RPC-over-TLS connection breaks
   under IOR write load** (~4 s in).  MDS side: an io_uring
   `WRITE` CQE returns `EPIPE` and the TLS read sees a closed
   connection.  Root cause is in the MDS `lib/io/` TLS +
   io_uring connection path; the `Connection not tracked for fd`
   line points at an fd-lifecycle bug.
2. **INV-5 then amplifies it**: a connection blip that should
   cost one quick reconnect instead costs ~90 s because all PSes
   reconnect simultaneously and lose the mTLS race.

Net: **INV-5 and INV-6 are one problem with two faces** -- the
PS<->MDS RPC-over-TLS path is not robust, neither at concurrent
establishment (INV-5) nor under sustained write load (INV-6).
The fix is an `lib/io/` TLS + io_uring connection-lifecycle
slice; fixing INV-5 alone would shorten the outage but not stop
the mid-workload disconnect.

### INV-5/INV-6 root cause (2026-05-19, lib/io read)

Read of `lib/io/conn_info.c`, `io_internal.h`, `accept.c`,
`handlers.c`.  The connection table is

```
static struct conn_info *connections[MAX_CONNECTIONS];  /* 65536 */
... connections[fd % MAX_CONNECTIONS] ...
```

Since accepted fds are always far below 65536, `fd % 65536 == fd`
-- the table is a **direct fd-indexed array**.  The "Connection
slot collision" branch in `io_conn_register()` is therefore dead
code in practice; slot collision is NOT the INV-5 cause.  The
`conn_info` struct itself is malloc'd once per fd number and
reused -- never freed until `io_conn_cleanup()` -- so there is no
use-after-free on the `conn_info` pointer.  The defects are
narrower and real:

- **Defect 1 -- `ci_ssl` is an unguarded pointer.**
  `io_conn_unregister()` / `io_conn_destroy()` do
  `SSL_shutdown` + `SSL_free` + NULL on `ci_ssl` under
  `conn_mutex`.  But every consumer holding a `struct conn_info *`
  (returned by `io_conn_get()`, which drops the lock before
  returning) reads `ci->ci_ssl` and uses the SSL *outside* the
  lock -- `handlers.c:281` is the live example: it caches `ci`,
  tests `ci->ci_ssl`, then calls `io_do_tls()` which uses it.  A
  concurrent unregister `SSL_free`s that object -> UAF.
  `handlers.c:99` already admits the hazard in a comment:
  "ci->ci_ssl = NULL at any time after an SSL error."  This is
  the prime suspect for INV-6's mid-load disconnect: one worker
  `SSL_write`s on an SSL another worker just freed.

- **Defect 2 -- fd reuse only partly guarded.**  On reconnect the
  kernel hands back the same fd number; `io_conn_register()`
  reuses the *same* slot, `memset`s it, bumps `ci_generation`.
  `ci_generation` is checked in exactly one place --
  `io_conn_write_done()`, the write gate.  `ci_ssl`,
  `ci_tls_enabled`, `ci_state`, `ci_xid` are NOT generation-
  checked, so a stale in-flight op for the *old* connection N
  reads/mutates the *new* connection's state once fd N is reused.

- **Defect 3 -- no CLOSING state.**  The code flags this gap
  itself: `io_conn_check_timeouts()` comment (conn_info.c lines
  ~778-783) -- "the fix is a per-slot CLOSING state to block
  reuse; for now the simplicity is worth it."  Between
  `io_conn_unregister()` (sets `ci_fd = -1`) and the next
  `io_conn_register()`, an in-flight CQE resolves `io_conn_get()`
  to NULL ("Connection not tracked for fd=N" -- the INV-6 log
  line) or, after reuse, to the WRONG connection.

So "Connection not tracked" is the *benign* face -- a NULL lookup
that just drops a reply.  The dangerous face is the reuse/teardown
window where `ci_ssl` is freed under an active user or
`io_conn_get()` returns a connection that is not the caller's.
INV-5 (concurrent establishment) and INV-6 (mid-load teardown)
are the same defect class: `conn_info` has no lifecycle
discipline.

Fix-slice shape (separate work, own branch off main):
1. Refcount `ci_ssl` (or hold `conn_mutex` across every SSL use)
   so consumers cannot use-after-free.
2. Add a `CONN_CLOSING` state that blocks slot reuse until
   in-flight ops on the old fd drain.
3. Extend the generation check beyond the write gate -- have
   `io_conn_get()` hand back `(ci, generation)` and require
   callers to revalidate, or key every accessor by `(fd, gen)`.

**Stage 1 repro (DONE 2026-05-19, commit 8cd8149b50e1):**
`lib/io/tests/conn_lifecycle_race_test.c` -- churn threads
register/unregister a small set of fds with real `SSL` objects
while reader threads run the `handlers.c:281` pattern
(`io_conn_get()` then read `ci->ci_ssl` outside `conn_mutex`).
Built as a `check_PROGRAM` but kept out of `TESTS`, so `make
check` only compiles it; it is run by hand under a sanitizer
build.

TSAN on dreamer (`--enable-tsan`) confirms the diagnosis RED --
two data races, exit 66:

- `conn_lifecycle_race_test.c:92` write of `ci->ci_ssl`
  (8 bytes) in churn vs `:122` read in reader -- the unguarded
  `ci_ssl` pointer, Defect 1.
- `:93` write of `ci->ci_tls_enabled` (1 byte) in churn vs
  `:122` read in reader -- same defect class, the
  non-generation-checked TLS state of Defect 2.

Both races are on the `conn_info` struct (heap block of 360
bytes allocated by `io_conn_register` at `conn_info.c:67`); the
struct is stable, the *fields* are raced.  This is the root
cause confirmed: every reader of an `io_conn_get()` return value
touches `ci_ssl` / `ci_tls_enabled` with no lock.  The test
terminates cleanly (no hang, no deadlock) -- it is purely a
sanitizer-RED repro, not a crash.

An `--enable-asan` run of the same binary on dreamer was clean
(exit 0).  The `conn_info`-field race is what fires
deterministically; the downstream use-after-free on the freed
`SSL` object itself needs the reader descheduled between caching
`ci_ssl` and dereferencing it -- a window the tight reader loop
rarely hits.  A one-line `sched_yield()` in the reader would
widen it to demonstrate the UAF directly (the literal INV-6
"SSL_write on a just-freed SSL" mechanism); not done yet, since
the TSAN field-race already confirms the diagnosis.

Note: sanitizers do not run on the macOS dev box (Darwin 25.5 /
macOS 26 -- TSAN segfaults and ASAN deadlocks in their own
runtime init before `main()`); the repro is exercised on dreamer
(Fedora aarch64).  macOS still builds the tree fine.

That red test anchors the Stage 3 fix.

### Track 2 re-run after Slice 1 (2026-05-19)

Stage 3 Slice 1 (commits `1c7aae9a6276` + `50572a93024d`) refcounted
`ci_ssl` and added the lock-safe TLS-flag accessors.  Slice 2
(`5ad0b516b8c3` + `078b0c9d70fa`) promoted the lifecycle-race test
into `TESTS` and verified it GREEN under TSAN.  Stage 4 re-ran
Track 2 (`NPS=4`, basic IOR `-w -r -W -R -e -k`, no `--reorder`) on
dreamer to assess INV-5/INV-6 status with the safe API live.

**Outcome:**

| Symptom | Run 4 (pre-Slice-1) | Run 5 (post-Slice-1) |
|---------|---------------------|----------------------|
| INV-5 -- bringup mTLS race -- lost sessions | 1-of-4 and 3-of-4 on two of three attempts | **0-of-4: clean** -- all 4 PSes registered, MDS granted 4 grants |
| Mid-IOR disconnect trigger | ~4 s into IOR, all 4 PSes lost session | ~6 s into IOR, **2 of 4 PSes** lost session (PS-0 and PS-2) |
| MDS log line on the break | `Connection not tracked for fd=N` | same line, same call site (`io_rpc_trans_cb:307`) |
| Reconnect outage | ~90 s (INV-5 amplified the break) | ~95 s on PS-0 / PS-2 (exp backoff 0/1/2/4/8/16 s) |
| IOR end state | aborted at `fsync(15) failed` + `stat()` failed | **same abort, same `fsync(15) failed` + `stat()` failed** |
| Memory-safety symptoms (ASAN / UBSAN) | -- | **none** -- harness scan of MDS/DS/PS logs reports `PASS: no ASAN/UBSAN errors` |

**Interpretation.**  INV-5 and INV-6 are *not* one bug with two
faces -- the earlier hypothesis from the lib/io read was incomplete.
Slice 1 demonstrably closes INV-5 (the bringup-time symptom) and
removes the memory-safety attack surface around `ci_ssl` (no
ASAN/UBSAN in this run), but the mid-workload PS<->MDS disconnect
trigger fires unchanged.  The `Connection not tracked for fd=N`
line still appears, which is exactly what Slice 3's `CONN_CLOSING`
+ generation-check extension is designed to address (per
`.claude/design/io-conn-lifecycle.md`, Defects 2 and 3 in the root
cause).  Slice 1 addressed Defect 1 (`ci_ssl` UAF); Defects 2 and 3
are still live.

Open hypothesis for the trigger itself (what makes the SSL session
break ~6 s into IOR's write load):

1. **Concurrent `SSL_read` vs worker `SSL_write` on one `SSL`** --
   flagged as Deferred / NOT_NOW_BROWN_COW in the lifecycle plan
   ("Mechanism A makes it memory-safe; full serialisation is a
   separate slice and is not required to unblock Track 2").  Run 5
   shows that "not required to unblock Track 2" no longer holds:
   memory safety alone is insufficient for IOR to complete.
2. **The fd-reuse hazard of Defect 2** -- the SSL error on fd=21 /
   fd=23 in the MDS log keeps repeating after the original break,
   suggesting state from a torn-down conn is being read on a
   reused or stale slot.  This is Slice 3's territory.
3. **A real protocol-level error** at the first heavy write that
   the SSL machinery is mis-reporting as an SSL alert.  Lower
   prior; the same line "SSL error 6, alert: error:00000000" is
   `SSL_ERROR_ZERO_RETURN` with no real alert payload, consistent
   with a peer-closed underlying socket rather than a protocol
   fault.

INV-1 DS instrumentation remains gated on a clean Track 2 run,
which Slice 1 alone does not deliver.

### Track 2 run 6 timeline (Slice 3 live, 2026-05-19)

Stage 3 Slice 3 (commit 23dfe116108c) added per-SSL serialisation
via `SSL_set_ex_data` + a `pthread_mutex_t` taken around every
`SSL_` and `BIO_` call on a slot-owned SSL.  Track 2 re-ran with
the slice live; outcome:

- Bringup clean (same as run 5).
- Only **1 of 4 PSes** (PS-0) lost its session vs 2 in run 5 and
  4 in run 4 -- INV-6's amplification narrows monotonically across
  slices.
- IOR still aborts at `fsync(15) failed` + `stat() failed`,
  same as runs 4 and 5.
- Zero ASAN/UBSAN, same as run 5.

Per-thread timeline (extracted from `docker logs reffs-bench-mds`
and `docker logs reffs-ps-0`):

| Time | Actor | Event |
|------|-------|-------|
| 22:43:16 | IOR | start (rank 0 on PS-0) |
| 22:43:22.086 | PS-0 (renewal_tick) | `upstream session is dead (errno=Input/output error sr_status=0) -- forcing reconnect` |
| 22:43:22.091 | MDS (io_rpc_trans_cb:331) | `Connection not tracked for fd=21` |
| 22:43:22.106 | MDS (io_handle_read) | `SSL error 6` on fd=25 (SSL_ERROR_ZERO_RETURN, peer close_notify) |
| 22:43:37 -> 22:45:07 | PS-0 | exponential-backoff reconnect attempts (0/1/2/4/8/16/30 s) |
| 22:45:07 | PS-0 | reconnect succeeds |

Two facts the run-5 analysis got wrong, now corrected:

1. **PS sees EIO BEFORE any close_notify.**  The PS's renewal
   thread detects `errno=Input/output error sr_status=0` from
   the TIRPC layer and forces a reconnect.  Only then does the
   PS send close_notify to the MDS.  The MDS's SSL_ERROR_ZERO_
   RETURN is the *consequence* of the PS already deciding to
   tear down -- not the trigger.

2. **`Connection not tracked for fd=N` precedes the SSL error
   by 15 ms.**  The MDS event loop took an io_rpc_trans_cb
   completion for a fd that `io_conn_get()` had just lost --
   classic stale-CQE / fd-reuse symptom.  This is exactly what
   Defects 2 (no generation check beyond write gate) and 3 (no
   `CONN_CLOSING` state) in the lifecycle plan target.

The original hypothesis ("event-loop SSL_read races worker
SSL_write under sustained load") was plausible but wrong as a
*primary* cause; the data points at fd-lifecycle, not SSL state.
Slice 3 still contributes -- the affected-PS count went 4 -> 2 -> 1
across runs and zero ASAN/UBSAN persists -- but it does not close
INV-6 on its own.

The natural next slice is Slice 4 (CONN_CLOSING + generation
extension) as originally planned in `.claude/design/io-conn-lifecycle.md`.

---

## How the groups connect

The chunk-collision harnesses (Group A) were built to find
correctness bugs.  The IETF review (Group B) asks a *performance
characterisation* question about the same code path.  T1b/T2 are
the shared vehicle: the partial-stripe write that Christoph is
worried about is exactly the write that chunk-collision contends
on.  Instrumenting those runs answers INV-1 without a separate
harness -- which is why this index exists, so the connection is
not lost.

## Cross-references

- [`chunk-collision-validation.md`](chunk-collision-validation.md)
  -- Group A master design
- [`chunk-collision-track2.md`](chunk-collision-track2.md)
  -- T2 implementation design
- [`proxy-server.md`](proxy-server.md) -- PS phases; Phase 4b is
  the partial-stripe RMW path INV-1 measures
- `reffs-docs/christoph.md` (private) -- Christoph Hellwig
  actor-stance notes, full IETF-thread provenance
- `reffs-docs/experiments/` (private) -- earlier experiment
  write-ups (01-..) and reports
