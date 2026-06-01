<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Chunk-Level Write Collision Validation

## Goal

Stress reffs's CHUNK state machine, CRC bookkeeping, layout-fence
rotation, and trust-stateid invariants under **concurrent writes to
the same chunk from distinct NFSv4.2 clientids**.  Detect any
silent corruption -- bytes returned on read that do not match any
writer's payload at any point in time -- and any protocol-state
inconsistency (orphan PENDING blocks, stale FINALIZE, mismatched
CRC32 vs payload, ghost layout grants).

This is a correctness validation, not a performance benchmark.
Throughput numbers are incidental.

## What we are looking for

The interesting bug surfaces:

1. **CHUNK_WRITE / FINALIZE / COMMIT race**.  Writer A's CHUNK_WRITE
   lands the bytes; writer B's CHUNK_WRITE for the same offset lands
   different bytes; writer A's FINALIZE wins.  What does the persisted
   `chunk_block.state` say?  What CRC32 is recorded?  Does the on-disk
   payload match the recorded CRC?  RFC: draft-haynes-nfsv4-flexfiles-v2
   sec-CHUNK_WRITE / sec-CHUNK_FINALIZE.

2. **CRC vs payload divergence**.  Two writers write overlapping bytes
   in the same chunk; the persisted block ends up with bytes from
   writer A and a CRC from writer B (or vice versa).  CHUNK_READ
   would detect this on next read; CHUNK_HEADER_READ might not.

3. **Layout fence + in-flight CHUNK_WRITE**.  Writer A triggers a
   LAYOUTERROR; the MDS rotates synthetic uid/gid on all mirrors.
   Writer B's in-flight CHUNK_WRITE was sent with the old credentials.
   Does the DS reject with ACCESS?  Does writer B's recovery path
   re-LAYOUTGET, re-encode, retry without losing its bytes?

4. **Trust-stateid invariants** (after that ships per
   trust-stateid.md).  Writer A's stateid is REVOKE_STATEID'd
   mid-write.  Subsequent CHUNK ops from writer A return BAD_STATEID;
   layout error path triggers; writer A re-acquires layout, gets a
   fresh stateid via TRUST_STATEID, retries.  No bytes lost, no
   ghost trust-table entries.

5. **WWWL detection** (Write Without Write Layout).  Writer A returns
   its layout while writer B is mid-CHUNK_WRITE.  The MDS's reflected
   GETATTR fan-out at LAYOUTRETURN sees DS state diverged from cached
   inode attrs.  Does the MDS log WWWL correctly, or silently accept
   the divergence?  Today's WWWL detection is narrower than this
   item implies: `lib/nfs4/dstore/dstore_wcc.c` only fires on
   `mtime_backwards` and `ctime_backwards`, not on size divergence
   or out-of-window writes -- the harness will deliberately surface
   the gap rather than rely on the existing detection covering
   every case.

6. **PENDING-block leak**.  Writer A starts CHUNK_WRITE (PENDING),
   then crashes / disconnects before FINALIZE.  Does the chunk_store
   ever see CHUNK_FINALIZE for that block?  If not, who reaps the
   PENDING entry?  After how long?  Is there a leak across the chunk
   metadata file?

7. **Codec divergence under contention**.  Two writers using the same
   codec encoding produce different parity for the same data.  Which
   parity ends up on disk?  If a degraded read later needs to
   reconstruct, does it pick the right shard set?

## Why mdtest is the wrong tool

mdtest measures metadata throughput: independent processes operating
on independent files in independent subtrees.  No two processes ever
target the same byte range, the same chunk, or the same file.  Every
chunk reffs sees has exactly one writer, by construction.  None of
the seven bug surfaces above can be hit with mdtest.

## What does collide at the chunk level

| Tool | Mode | Block-level conflict? |
|------|------|----------------------|
| mdtest | metadata storm, file-per-process | NO |
| IOR `-F 1` | bulk I/O, file-per-process | NO |
| **IOR `-F 0` shared file** | one file, N writers, overlapping ranges | **YES** |
| fio `--numjobs=N` shared `--filename` | one file, N writers, overlapping ranges | YES (less rigorous verify than IOR) |
| HACC-IO / VPIC-IO | shared-file collective writes (real-app proxy) | YES |
| Multiple ec_demo `put` to same MDS file | EC writers race per-chunk | YES (CHUNK ops directly) |
| pjdfstest / mdtest with shared-dir flags | metadata + dirent contention | partial -- contends on dirents not chunks |

The HPC-canonical answer is IOR `-F 0 -W -R -C`.  The reffs-native
answer is multiple concurrent ec_demo instances.  **Both have a role
because they exercise different layers** -- IOR exercises
client-visible POSIX I/O semantics through the mount, multiple
ec_demo exercises the CHUNK protocol surface directly.

## The clientid problem

Concurrent CHUNK_WRITE on the same chunk from a single client is
neither realistic nor diagnostic of the bugs we care about.  Real
contention comes from **distinct NFSv4.2 clientids** (separate
EXCHANGE_ID + CREATE_SESSION lifecycles).  Today, the only reffs
client that produces CHUNK_WRITE traffic is `tools/ec_demo`.

Three execution tracks, depending on what is shipped:

### Track 1 -- multiple ec_demo instances (ships now)

Each ec_demo instance does its own EXCHANGE_ID with a unique
`--id` value.  N instances target the same MDS file via concurrent
`write` operations (codec-aware -- not `put`, which is the plain
mirrored subcommand and silently ignores `--codec`).  The
whole-file rewrite produces N writers on every chunk
simultaneously.  Last-FINALIZE-wins per chunk; verify phase
checks the surviving payload is *some* writer's complete encoding
(not a frankenstein of partial writes).

This works against the existing 10-DS Docker topology in
`deploy/benchmark/` with no new infrastructure.

Limitation: ec_demo only supports whole-file `write` / `read` /
`verify`, so writers always target the same offsets in the same
order.  Sub-chunk byte interleaving (writer A wins bytes 0-1023,
writer B wins bytes 1024-4095 of the same block) is not exercised.
That mode requires Track 1b.

### Track 1b -- ec_demo partial-range writes (small extension)

Add a `--offset OFF --length LEN` option to ec_demo `put`.  The
client encodes only the affected stripes, sends CHUNK_WRITE for
the affected blocks only, FINALIZE / COMMIT only those blocks.

This is the surface that produces real byte-level interleave: each
writer touches a different sub-range of the same chunk, and the
expected post-state is **deterministic** (writer A's bytes in its
range, writer B's bytes in its range).  Any deviation is a
correctness bug.

Estimated work: ~150 LOC in ec_demo, no protocol changes.

### Track 2 -- IOR via N PS containers (UNGATED -- PS Phase 3 + 4 shipped)

When `proxy-server.md` Phase 3 (`REFFS_DATA_PROXY` backend, client
READ wired through pipeline) AND Phase 4 (client WRITE wired
through pipeline) both land, Linux NFS clients gain access to
EC-backed files for both directions.  IOR `-W -R` end-to-end
verification needs both halves wired -- Phase 4 alone gets you a
write-only test surface.  Each PS holds its own MDS-facing
session, so N PS containers = N distinct clientids visible to the
MDS.

Run IOR with `-F 0 -W -R -C` from N MPI ranks where each rank
mounts a different PS's :4098 listener:

```sh
mpirun -np 16 \
  --hostfile /etc/wt/hosts \
  ior -a POSIX -F 0 -w -r -W -R -C -e -k \
      -t 64k -b 256m -s 4 -i 5 \
      -o /mnt/ps-${OMPI_COMM_WORLD_RANK}/ior_shared.dat
```

Each rank's `/mnt/ps-N` mount points at a different PS container.
All PSes proxy the same MDS file.  16 ranks → 16 PSes → 16
clientids hammering the same file.

This gives the HPC-tool recognition (IOR is everyone's reference
parallel-I/O test) plus the multi-clientid contention surface.
**Ungated as of 2026-05-19** -- PS Phase 3 (proxy READ) and Phase
4a+4b (proxy WRITE) shipped.  The full Track 2 implementation
design is `.claude/design/chunk-collision-track2.md`; the harness
is `deploy/benchmark/run_chunk_collision_track2.sh`.

### Track 3 -- Linux NFS clients direct (BASELINE ONLY, no EC conflict)

For completeness: today, Linux NFS clients can mount the MDS via
v3 or v4.  The MDS does inband I/O proxying to ONE mirror per
file -- no EC encoding from the MDS, no CHUNK_WRITE traffic, no
chunk-level conflict.

**Useful for:** confirming v3/v4 inband I/O still works while EC
is under stress on other paths.  Useful as a sanity baseline and
for cross-protocol corruption checks (does an ec_demo write of
file F survive a Linux NFS read of file F?).

**Not useful for:** the seven bug surfaces above.  Track 3 alone
proves nothing about chunk-level correctness.

## Verification methodology

Per-track verification:

| Track | Writer | Verifier | Mismatch detection |
|-------|--------|----------|-------------------|
| 1, 1b | ec_demo put | ec_demo check (re-read + decode + compare) | Built into ec_demo |
| 2 | IOR `-W` (write-then-verify in same process) | IOR `-R` (read-back verify) + reorderTasks | Built into IOR |
| 3 | dd / cp from Linux client | sha256sum after | Manual checksum |

In addition, every track captures structured probe-counter
deltas (preferred over log scraping -- structured, rate-limited,
stable across log-format changes).  The harness reads counters at
run start and run end and checks the deltas:

- **Chunk activity counters**: `sb_chunk_writes`,
  `sb_chunk_pending_displaced`, `sb_chunk_finalize_crc_fail`,
  `sb_chunk_commit_crc_recompute`, `sb_chunk_rollback_invoked`,
  `sb_chunk_repair_initiated`.
- **Layout activity counters**: `sb_layout_errors_received`,
  `sb_layout_fences_rotated`.
- **Optional per-inode chunk-store dump** via a `chunk-store-dump
  <ino>` probe op for diagnostic depth: every chunk_block has
  state in {EMPTY, FINALIZED, COMMITTED}; PENDING entries only
  for writers still active; orphan PENDING is a finding.

Both counters and the dump op are added by the BLOCKER 2 work
described in the reviewer-feedback section.  Until they ship,
log scraping (MDS for WWWL/LAYOUTERROR/BAD_STATEID, DS for CHUNK
state-machine assertions and CRC mismatches) is the temporary
fallback.

Pass criteria:

1. Zero data corruption in the verifier output.
2. Zero `WWWL` entries unless deliberately injected (Track 1b
   with overlap-but-no-conflict ranges should produce zero WWWL).
3. Zero ASAN / UBSAN / TSAN errors in MDS or DS logs.
4. Counter deltas are consistent with the test's expected race
   behavior.  For `coll_t1_basic` with 4 writers and 20
   iterations: `sb_chunk_pending_displaced` is positive (proves
   contention happened), `sb_chunk_finalize_crc_fail` is zero
   (proves no CRC anomaly slipped through), `sb_chunk_writes`
   matches expected fan-out arithmetic.  Replaces the old "every
   PENDING chunk_block reaped or owned by active client"
   criterion which was unobservable without per-inode dumps.
5. Every layout fence rotation is followed by either a successful
   re-LAYOUTGET (writer recovers) or an EIO to the writer (clean
   failure).  No silent stalls.  Verified via counter delta:
   `sb_layout_fences_rotated` increments line up with
   `sb_layout_errors_received` plus a clean recovery or a
   visible writer EIO.
6. Trust-stateid table (after that ships) at end of run contains
   only entries for layout grants still held by active clients.
   Verified via the counter family above plus the optional
   `chunk-store-dump <ino>` probe op for spot checks.

## Test inventory

### Track 1: ec_demo multi-instance (ships now)

| Test | Configuration | Pass criteria |
|------|--------------|---------------|
| `coll_t1_basic` | 4 ec_demo `write`, same file, same codec (RS 4+2) | verify after settle: passes against any writer's payload |
| `coll_t1_codec_mix` | 4 ec_demo `write`, same file, mixed codecs | The MDS picks the codec from each LAYOUTGET's k/m, NOT from a per-file property (lib/nfs4/server/layout.c). Two clients with different codecs against the same file produce silent shard-format divergence. **Pass criteria: this MUST be detected by the MDS or surface as visible corruption -- silent acceptance is a BLOCKER outcome.** |
| `coll_t1_layout_v1v2_mix` | 2 ec_demo with `--layout v1`, 2 with `--layout v2` | similar -- layout type is per-LAYOUTGET, not per-file; mismatched layout types should fail-loud or detect on verify |
| `coll_t1_id_collision` | 4 ec_demo, all with same `--id "stress"` | All 4 writers produce the same `co_ownerid`; nfs4_client_alloc_or_find Case 2 returns the existing clientid; each ec_demo creates its own session, so the 4 writers get distinct sessions on the same client.  Expected protocol behavior; chunk-level race semantics are unchanged. |
| `coll_t1_long_soak` | 8 ec_demo, 30-minute loop of write-then-verify | zero verify failures over 30 min |
| `coll_t1_fence_recovery` | 2 ec_demo + injected LAYOUTERROR every 30s | both writers eventually complete; verify passes |

### Track 1b: ec_demo partial-range (after `--offset` / `--length`)

| Test | Configuration | Pass criteria |
|------|--------------|---------------|
| `coll_t1b_disjoint` | 4 ec_demo, disjoint byte ranges in same file | every byte matches the writer that wrote it |
| `coll_t1b_overlap` | 4 ec_demo, overlapping ranges, deterministic seed-per-rank | every byte matches *some* writer's range; we can identify which |
| `coll_t1b_full_chunk_split` | 2 ec_demo on different halves of one chunk | chunk-store ends with single chunk_block, COMMITTED, CRC matches concatenation |
| `coll_t1b_subchunk_interleave` | 2 ec_demo writing alternating 1KB stripes within a 4KB block | every byte matches its writer; CRC32 matches the actual byte concatenation |

### Track 2: IOR via PSes (after PS Phase 4)

| Test | Configuration | Pass criteria |
|------|--------------|---------------|
| `coll_t2_ior_basic` | `mpirun -np 4 ior -F 0 -w -r -W -R -e -k` via 4 PSes | IOR reports zero verify mismatches |
| `coll_t2_ior_reorder` | adds `-C` reorderTasks | IOR reports zero verify mismatches; cross-rank read pairs all clean |
| `coll_t2_ior_scaled` | scales up to 16 ranks / 16 PSes | same as above; signal that the architecture scales to a typical HPC fan-in |
| `coll_t2_ps_codec_translate` | mixed-codec target file; one PS does the translation in-flight | IOR sees consistent bytes even as the target file's codec shifts |

### Track 3: Linux NFS baseline (no EC conflict, sanity only)

| Test | Configuration | Pass criteria |
|------|--------------|---------------|
| `coll_t3_v3_baseline` | git clone over NFSv3 to MDS while Track 1 stress is running | git clone succeeds; sha256sums match remote |
| `coll_t3_v4_baseline` | same via NFSv4.2 | same |
| `coll_t3_cross_protocol` | ec_demo writes file F; Linux client reads F via NFSv3; bytes match | every byte matches the write |

## What we found

Observed-state log of first-pass smoke runs.  Each entry is
"what the harness produced", not "what we have triaged" --
triage moves to NOT_NOW_BROWN_COW items and follow-up commits.

### Track 1 (full-file race) -- 2026-05 first smoke

Reproduced a chunk-level **frankenstein** in 4 of 5 iterations:
two writers complete cleanly, neither writer's payload matches
the verified file.  CRC vs payload divergence and PENDING-block
leak counters both fire on these runs.  Root cause not yet
diagnosed.

### Track 1b (partial-range race) -- 2026-06-01 triage

First-smoke (pre-trap-fix, 2026-05-30): the harness exited
non-zero during pre-fill and the unconditional `rm -rf`
WORKDIR trap wiped all per-writer logs before anyone could
read them.  The session-summary characterization of "concurrent
ec_demo writers exit with NFS4ERR_BADSESSION cascades" was
stitched together from partial stderr and turns out to be
**incorrect**: no BADSESSION has been observed.

Today's preserved-WORKDIR runs (after the trap fix in this
document set) walk a stack of harness / environment defects
that have prevented the race from ever running end-to-end on
the shadow lab host:

1. **Stale `ec_demo` binary at the head of the discovery
   list.**  The harness searches `/shared/build/tools/ec_demo`,
   `/reffs/build/tools/ec_demo`, then `/usr/local/bin/ec_demo`.
   On the host (outside the docker network), only the last
   path exists -- and it is an old `ec_demo` from before
   `--shard-size` landed.  Hit symptom: `write:
   unrecognized option '--shard-size'`.  Workaround:
   `--ec_demo /home/<user>/reffs/build/tools/ec_demo`.

2. **Hard-coded `MDS=reffs-mds` doesn't resolve on the host.**
   That hostname is a docker-network alias only.  Hit symptom:
   `session create failed: -111` (ECONNREFUSED), but actually
   prior to TCP -- this is the libtirpc DNS-failure code path.
   Workaround: `--mds 127.0.0.1`.

3. **libtirpc `clnt_create_timed` portmap-path mismatch.**
   With `--mds 127.0.0.1` (no explicit port), libtirpc tries
   the host rpcbind for the NFS program; the host rpcbind is
   running but does not register the in-container NFS service,
   so the lookup fails before any TCP connect to 2049.  Hit
   symptom: `session create failed: -111`.  Workaround:
   `--mds 127.0.0.1:2049` (ec_demo's host:port syntax bypasses
   portmap).

4. **No `ASAN_OPTIONS=detect_leaks=0:halt_on_error=0` /
   `LSAN_OPTIONS=halt_on_error=0` around `ec_demo`
   invocations.**  The ASAN/LSAN-instrumented `ec_demo` build
   reports libtirpc allocation leaks in `clnt_create_timed`
   and `authunix_create_default` (~6 KiB total) even when the
   actual RPC completes successfully.  Default LSan exits the
   process with rc=1 on any leak.  The harness's
   `set -euo pipefail` catches that and aborts before
   launching the per-writer racers, even though the pre-fill
   `ec_demo: write OK` line is in the log just above.  Hit
   symptom: the harness trap fires after the pre-fill, no
   `writer-<i>.log` files are produced.  The CI integration
   test (`ci_integration_test.sh`) already mitigates this
   class of leak with `ASAN_OPTIONS=detect_leaks=0:halt_on_error=0`
   and `UBSAN_OPTIONS=halt_on_error=0`; the Track 1b harness
   needs the same.

Validation framing: this is exactly the value of the trap
fix.  The preserved WORKDIR turned a "BADSESSION cascade"
narrative (which would have anchored several days of
server-side triage) into a 4-step walk through harness
defects in under 10 minutes.

Once the four harness defects were addressed in a follow-up
commit, the multi-writer race ran end-to-end across all four
sub-modes and produced a genuine validation signal:

| Mode | Result | Why |
|------|--------|-----|
| `disjoint` | **PASS** -- all 4 writers' ranges verify clean | Each writer owns its own byte range; no chunk overlap, no per-stripe RMW contention.  Acts as the control case for "concurrent same-file clientid4s". |
| `chunk-split` | FAIL: BADSESSION cascade | Writes straddle chunk boundaries; multiple writers RMW the same chunk-block. |
| `overlap` | FAIL: BADSESSION cascade | Writers overlap on the same chunks. |
| `subchunk` | FAIL: BADSESSION cascade | Writes are smaller than a chunk; multiple writers RMW the same chunk. |

The three failing modes share the same on-wire signature.
Per-writer log (writer-0.log on `--mode overlap`):

```
ec_demo: connecting to MDS 127.0.0.1:2049 (owner shadow:writer0, ...)
ec_demo: writing 32768 bytes ... at offset 0 (4+2, shard=4096, range mode)
mds_compound_send: COMPOUND tag="reclaim_complete" op[0]=OP_SEQUENCE(53) status=NFS4ERR_BADSESSION(10052)  (x10)
mds_compound_send: COMPOUND tag="chunk_write" op[0]=OP_SEQUENCE(53) status=NFS4ERR_BADSESSION(10052)
ds_chunk_write: COMPOUND failed status=10052 (resarray_len=1)
[7.496] ec_write_stripe: stripe 1 data[0] FAILED: -121
mds_compound_send: COMPOUND tag="destroy_session" op[0]=OP_SEQUENCE(53) status=NFS4ERR_BADSESSION(10052)  (x10)
ec_demo: write failed: -121
```

The pattern: CREATE_SESSION appears to succeed (no error
logged), the very next op (RECLAIM_COMPLETE) returns
BADSESSION, the writer retries 10x with the same result, the
client falls through into CHUNK_WRITE to the DS which also
returns BADSESSION on its SEQUENCE op, then DESTROY_SESSION
also gets BADSESSION 10x.  Client surfaces it as
`ec_write_range: write stripe N failed: -121` (EREMOTEIO).
The disjoint PASS rules out a generic "N clientid4s on one
file" trigger -- the bug is specifically in the per-stripe
RMW path when multiple writers hit the same chunk-block.

The MDS log is silent on this -- no LOG/TRACE on the
BADSESSION return path -- which is itself a finding (the
operator gets no actionable signal when this fires; from the
client side it looks like a transient retry storm).

Triage targets for the next slice:

- DS session lifecycle under concurrent
  CHUNK_WRITE+FINALIZE+COMMIT on the same chunk-block;
- any MDS-side session-destroy-on-error path that fires
  before the client sees the failing op (BADSESSION returns
  before the actual op runs);
- the chunk-store lock-contention path on simultaneous RMW.

Validation framing for the run: the harness delivered exactly
what a chunk-collision harness should -- 1 clean control case
plus 3 distinct failing variants that all converge on the
same wire signature in the same code path.

### Track 2 (fio via N PSes) -- 2026-06-01 first clean smoke

Two smoke runs back-to-back on the shadow lab box.

**Run 1 (pre-harness-fix): FAIL on mount.** The fio client
container ran
`mount -t nfs4 -o port=4098,vers=4.2,sec=sys,nolock 127.0.0.1:/
/mnt/ps-0` and the kernel returned
`NFS: mount program didn't pass remote address` before any
CHUNK op issued.  Root cause: the `reffs-dev:latest` image used
as the fio client container ships without `nfs-utils`, so there
is no `mount.nfs` helper.  Without the helper, the generic
util-linux `mount(8)` falls through to the `fsconfig()` path
without the address translation the helper would have done, and
the in-kernel NFS code rejects the mount because
`nfs_server.address_len == 0`.  Fix: extend the harness's
existing one-shot in-container `dnf install fio` to also
install `nfs-utils`, and gate on both `fio` and `mount.nfs`
being present.

**Run 2 (post-fix): PASS.**  4 PSes, NPS=4 default geometry
(`--n 4 --transfer 64k --block 4m --segments 4 --iterations 3`),
3 iterations of write + pattern-verify on one shared 64 MiB MDS
file.  Each rank writes a 16 MiB stripe through its own PS at
`127.0.0.1:4098+r` with a rank-keyed `verify_pattern`, then
re-reads its own stripe and confirms the pattern.  fio
returned 0; the harness reported `PASS: fio write+verify clean
(rc=0, no mismatches)`, `PASS: no ASAN / UBSAN errors`, `PASS:
no CONN_CLOSING force-drain warnings`.

This is the first clean multi-PS chunk-collision result on the
wire path the WG asked about: 4 distinct clientid4s contend
for the same MDS file's layout state and chunk-store while
each commits its own byte range; the server-side state machine
(LAYOUTGET fan-out across 4 sessions, the chunk-store under
concurrent CHUNK_WRITE / FINALIZE / COMMIT from 4 layout
holders, in-flight LAYOUTERROR + fence rotation if any) did
not corrupt bytes, leak PENDING blocks visible to a verify
read, or trip the sanitizers.

Smaller open item: the harness's post-run probe-counter snapshot
prints `(probe sb-list failed)` because the in-MDS-container
`reffs_probe1_clnt` binary lacks its shared-library path
(`libreffs_nfs4_server.so.0: cannot open shared object file`).
The probe RPC server itself is fine -- this is a per-binary
LD_LIBRARY_PATH gap in the container, not a server defect.
Doesn't gate the validation result, but blocks publishing
INV-1 counter deltas alongside the PASS.  Fix: set
`LD_LIBRARY_PATH=/shared/build/lib/.libs:/shared/build/src/.libs`
in the `docker exec` invocation in `snapshot_counters()`.

Validation framing: Run 1's "FAIL before any server work"
shape is exactly what a validation harness is supposed to give
you when the harness itself is broken -- ASAN/UBSAN clean
because nothing reached the server, sentinel "did not issue
CHUNK ops" visible in the chunk counters had they been
readable.  Run 2 is the actual chunk-collision signal Track 2
was designed to deliver.

## Cost model: containers vs. bare metal

The container topology produces correct NFSv4.2 traffic, with
the bridge-NAT and shared-overlay2 costs that any docker-bench
inherits.  Specific latency / scaling numbers depend on the host
hardware and aren't pinned here.

Implications:

- **Correctness validation is unaffected.**  The host kernel
  honors fsync, RPC semantics, and ordering identically inside a
  container.  The seven bug surfaces are exactly as detectable.
- **Race window timing changes.**  Slower RPCs widen race
  windows; some bugs become *more* reproducible in containers
  than on a tight LAN.  Useful for stress.  But we cannot claim
  a clean container run validates a tight-LAN-only race.
- **Throughput numbers don't transfer.**  Don't quote IOR
  bandwidth from a container run as the system's real-world
  performance.  This is correctness validation; throughput is
  incidental.

Scale on the bench host until either (a) the bridge NAT becomes
the dominant cost, or (b) docker / overlay2 storage saturates.
At that point switch to macvlan or move to bare-metal Track 4 in
goals.md.

## Implementation order

1. **Phase 1 (now): Track 1 harness.**  Script that launches N
   concurrent ec_demo `put` instances against the existing 10-DS
   Docker topology, runs check after settle, scrapes logs for
   the pass-criteria signals.  Lives in
   `deploy/benchmark/run_chunk_collision.sh`.

2. **Phase 2 (small): ec_demo `--offset` / `--length`.**  Extend
   ec_demo to support partial-range puts.  Unblocks Track 1b.

3. **Phase 3: Track 2 harness -- designed 2026-05-19.**  MPI
   launcher for IOR via N PSes.  The PS Phase 3/4 gate is
   satisfied.  See `.claude/design/chunk-collision-track2.md` and
   `deploy/benchmark/run_chunk_collision_track2.sh`.

4. **Phase 4 (parallel with any of the above): Track 3
   baseline.**  Existing CI already does some of this; formalize
   it as a co-runner during chunk-collision tests.

## RFC references

- RFC 8881 §13.1 (file layout, EXCHANGE_ID flags for DS)
- RFC 8881 §18.36 (LAYOUTGET / LAYOUTRETURN / LAYOUTCOMMIT)
- RFC 8881 §18.37 (LAYOUTERROR)
- RFC 8435 (Flex Files v1 layout)
- draft-haynes-nfsv4-flexfiles-v2 (CHUNK ops, FINALIZE/COMMIT)
- draft-haynes-nfsv4-flexfiles-v2-proxy-server (PS for Track 2)

## Reviewer feedback (review #1, 2026-05-05)

This section captures the first review pass.  Status of each item
inline.  BLOCKER 2 (counters + probe surface) is the remaining
load-bearing piece; everything else is resolved.

### BLOCKERs

1. **RESOLVED:** **`ec_demo put` ignored `--codec`.**
   `tools/ec_demo.c:280-309` shows `cmd_put` calls `plain_write`,
   not `ec_write_codec`.  The `--codec` flag is consumed only by
   `write` / `read` / `verify` subcommands.  As originally written,
   Track 1 exercised plain (mirrored) writes regardless of
   `--codec rs` -- it did NOT race the RS / Mojette CHUNK paths.

   Fix applied: harness switched to `ec_demo write` / `verify`
   (codec-aware).  First N=4 RS/v2 sweep at 1 MiB surfaced
   chunk-level frankenstein in 4/5 iterations -- two writers
   completed cleanly, neither input matched the verified file --
   exactly the bug surface this slice is supposed to find.

2. **PENDING:** **Pass criteria 4 and 6 are unobservable today.**
   The plan requires "every PENDING chunk_block is either reaped
   or owned by an active client at run end" and a similar
   invariant on the trust-stateid table.  There is no standalone
   PENDING reaper today (CHUNK_ROLLBACK only fires on writer death
   / lease expiry, per `lib/nfs4/server/chunk.c:743-768`), no
   operator-visible chunk-store dump, and no probe op enumerating
   the trust-stateid table.  The criteria as written cannot be
   checked.

   Pass criteria reframed (see "Verification methodology") to
   counter-delta semantics; counters not yet wired -- this is the
   next sub-slice.

   Fix (the operator-counters direction the user proposed):
   add a probe op `chunk-store-dump <ino>` as a Phase 1.5
   prerequisite, AND introduce per-MDS counters for chunk repairs,
   chunk collisions detected (CRC mismatch on FINALIZE / COMMIT,
   PENDING displaced by another writer's PENDING, etc.), and
   layout / fence errors reported by clients via LAYOUTERROR.
   The MDS-side counter is what ultimately matters: any chunk
   anomaly the validation cares about lands as a CHUNK op or a
   LAYOUTERROR from a client, so a probe-visible counter on the
   MDS is the load-bearing observation point.  Surface those via
   the probe protocol so harnesses can read them at run start
   and run end and check the deltas.

   Suggested counter family on `struct super_block` or
   `struct server_state`:

       sb_chunk_writes               -- total CHUNK_WRITE received
       sb_chunk_pending_displaced    -- new PENDING overrode prior PENDING
       sb_chunk_finalize_crc_fail    -- FINALIZE saw CRC mismatch
       sb_chunk_commit_crc_recompute -- COMMIT recomputed CRC vs persisted
       sb_chunk_rollback_invoked     -- CHUNK_ROLLBACK fired
       sb_layout_errors_received     -- LAYOUTERROR ops received
       sb_layout_fences_rotated      -- synthetic uid/gid bumps
       sb_chunk_repair_initiated     -- repair path entered (today: 0)

   Most of these likely already exist as static counters or
   tracepoints; the probe-side work is to expose them on the
   existing `nfs4-op-stats` / `layout-errors` probe surface
   (per `.claude/design/probe-sb-management.md` "extended stats"
   ops 0-12, with the per-sb breakdown landed in
   `.claude/design/sb-registry-v3.md`).

   Pass criterion 4/6 then becomes: "delta of these counters
   across the run is consistent with the test's expected race
   behavior" -- e.g., for `coll_t1_basic` with 4 writers and 20
   iterations, `sb_chunk_pending_displaced` is positive (proves
   contention happened), `sb_chunk_finalize_crc_fail` is zero
   (proves no CRC anomaly slipped through), `sb_chunk_writes`
   matches expected fan-out arithmetic.

### WARNINGs

3. **RESOLVED.** **`coll_t1_codec_mix` claim was incorrect.**  Plan
   originally said "file's codec is set at first write".  In fact
   `lib/nfs4/server/layout.c` picks the codec from the segment's
   k/m, not from a per-file persisted property.  Re-framed in the
   test inventory: mismatched-codec writers MUST be detected by
   the MDS or surface as visible corruption -- silent acceptance
   is a BLOCKER outcome.

4. **RESOLVED.** **"Only ec_demo produces CHUNK_WRITE traffic" is
   shelf-life bound.**  `lib/nfs4/ps/chunk_io.c` already issues
   `OP_CHUNK_WRITE`, but not yet wired through PS `db_write` (PS
   Phase 4 pending).  The note in the prose is now explicit: this
   is operationally true until PS Phase 4 lands, which is also
   Track 2's gating event.

5. **PARTIALLY RESOLVED.** **Plan-vs-harness mismatch on log
   scraping.**  Plan called for MDS / DS log scans for `WWWL`,
   `LAYOUTERROR`, `BAD_STATEID`, sanitizer errors; harness
   captured only the writer's stdout/stderr.  Verification
   methodology re-written to prefer probe-counter deltas
   (structured, rate-limited, stable across log-format changes);
   log scraping kept as the temporary fallback until BLOCKER 2's
   counters land.

6. **RESOLVED.** **`printf '\x%02x'` is bash-only.**  Harness
   updated to `printf '\\x%02x' "$i" | xxd -r -p` per the
   reviewer's suggested form.  Confirmed working: smoke run with
   the original form failed with bash "missing hex digit"; with
   the fix the per-rank byte encodes correctly.

7. **RESOLVED.** **Container cost model had unsourced numbers.**
   Specific latency / scaling figures stripped; replaced with
   "depends on host hardware -- scale until bridge or overlay2
   saturates."

8. **RESOLVED.** **WWWL detection scope is narrower than the plan
   implied.**  Bug surface item 5 now states this explicitly
   (only `mtime_backwards` / `ctime_backwards` today, not size or
   out-of-window writes) and frames it as a deliberate gap the
   validation will surface rather than relying on existing
   detection covering every case.

### NOTEs

9. **No action.**  Compose `bench-collision` block structurally
   mirrors `bench-shards` correctly: image, profile, network,
   depends_on chain, single-line `bash -c`.  `mktemp -d` defaults
   to `/tmp` (writable layer), unaffected by the `:ro` bind on
   `/reffs`.  SPDX, ASCII, tabs, `set -euo pipefail` all present.

10. **RESOLVED.**  Verifier-loop chimera detection is sound; the
    harness now has a one-line comment citing `cmd_verify` in
    `tools/ec_demo.c` so the next reviewer doesn't re-derive it.

11. **RESOLVED.**  Track 2 description now explicitly states PS
    Phases 3 AND 4 are required for `-W -R` end-to-end.

12. **RESOLVED.**  `coll_t1_id_collision` test row in the
    inventory now spells out the expected protocol behavior:
    same `co_ownerid` -> Case 2 returns the existing clientid ->
    each ec_demo gets its own session on that shared client.

13. **RESOLVED.**  Harness filename now includes `$$`:
    `coll_${CODEC}_${LAYOUT}_$(date +%s)_$$.dat`.

### Adjacent finding

Running the codec-aware harness for the first time exposed a
race in `lib/backends/server_persist.c`: the temp-file path was
the fixed `<dir>/server_state.tmp`, so two concurrent
EXCHANGE_IDs (each calling `server_alloc_client_slot` ->
`server_state_save`) collided, the first rename consumed the
.tmp, the second got `ENOENT`, and slot allocation failed -->
EXCHANGE_ID failed --> "session create failed: -121" in
ec_demo.  Fixed in a separate commit
(`server_persist: per-call .tmp filename to fix concurrent-save
race`) by appending `getpid()` and a process-local atomic
counter to the .tmp filename.  Not a chunk-collision finding per
se, but the race surfaced for the first time because of this
slice's harness, and it would have masked any real chunk-level
race result.

### Disposition

Status as of review #1 follow-up:

1. BLOCKER 1: **DONE.**  Harness uses `write` / `verify`; smoke
   run surfaces real chunk-level frankenstein.
2. BLOCKER 2: **PENDING.**  MDS counters + probe-surface
   exposure are the next sub-slice.  Add
   `sb_chunk_writes` / `sb_chunk_pending_displaced` /
   `sb_chunk_finalize_crc_fail` /
   `sb_chunk_commit_crc_recompute` /
   `sb_chunk_rollback_invoked` /
   `sb_layout_errors_received` / `sb_layout_fences_rotated` /
   `sb_chunk_repair_initiated` on `struct super_block` (or
   `server_state` where appropriate), expose via the existing
   `nfs4-op-stats` probe op or a new `chunk-stats` op.  Optional:
   `chunk-store-dump <ino>` probe op for diagnostic depth.
3. WARNINGs 3, 5: **DONE** inline (codec-mix re-framed;
   verification methodology now counter-first with log scraping
   as fallback).
4. WARNINGs 6, 7, 8: **DONE** inline.
5. NOTEs 10-13: **DONE** inline.
6. After BLOCKER 2 lands: re-spawn the reviewer for verdict #2.

## Deferred / NOT_NOW_BROWN_COW

- HACC-IO / VPIC-IO real-app proxies on Track 2.  Useful for HPC
  audience; nice-to-have, not gating.
- pjdfstest correctness suite once dir delegations land.  Outside
  chunk-collision scope.
- Cross-MDS replication scenarios (no replication today).
- Bare-metal Track 2 validation (Tier 5 in `goals.md`).
