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
| **INV-5** | Concurrent mTLS session establishment from multiple PSes to one MDS races `mds_session_create_tls` and most sessions fail with `EIO` (`Input/output error -- listener stays dark`).  With 4 PSes cold-starting together, Track 2 runs lost 1-of-4 and 3-of-4 sessions on two of three attempts. | Track 2 ([`chunk-collision-track2.md`](chunk-collision-track2.md)), 2026-05-19 | **OPEN.**  Harness staggered (`run-ps-bench-bringup.sh`, 4 s between PS launches) as a stopgap so Track 2 can proceed.  Real fix -- making `mds_session_create_tls` / the MDS TLS-accept path concurrency-safe -- is a separate slice.  Also bites production PSes reconnecting together after an MDS restart. |
| **INV-6** | A real kernel NFS client running IOR shared-file write+verify through the PS proxy path fails: `fsync()` warns "failed" and `stat()` on the proxied file errors (`aiori-POSIX.c:866`), aborting IOR.  The PS proxy data/metadata path does not survive an IOR `-w -r -W -R -e` shared-file workload. | Track 2 run 4, 2026-05-19 -- the first run to reach IOR (topology + harness now clean) | **OPEN.**  Suspects: COMMIT-through-proxy (the `fsync`) and/or cross-PS GETATTR visibility -- a file written via PS 0 then `stat`'d via PS 3.  Needs the PS + MDS container logs from the run (the harness leaves the containers up).  Distinct from INV-5, which was bringup-time. |

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

**Stage 1 repro (next):** a multithreaded
`lib/io/tests/conn_lifecycle_race_test.c` built with
`--enable-tsan` -- a thread pool doing register / get (read
`ci_ssl`) / unregister on a small rotating set of fd numbers with
real `SSL` objects from a test `SSL_CTX`.  Expect a TSAN
data-race report on `ci_ssl` and an ASAN UAF on the SSL object.
That red test anchors the fix.

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
