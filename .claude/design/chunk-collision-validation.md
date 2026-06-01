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

**2026-06-01 -- BADSESSION localized to the DSes, not the MDS.**

Counts of `NFS4ERR_BADSESSION` returns in each container's
trace file (`/build/reffsd.log`) after the 4-mode batch run:

| Container | BADSESSION count |
|-----------|------------------|
| reffs-bench-mds | **0** |
| reffs-bench-ds0 | 48 |
| reffs-bench-ds1 | 34 |
| reffs-bench-ds2 | 34 |
| reffs-bench-ds3 | 34 |
| reffs-bench-ds4 | 34 |
| reffs-bench-ds5 | 34 |
| reffs-bench-ds6 | 32 |
| reffs-bench-ds7 | 32 |
| reffs-bench-ds8 | 32 |
| reffs-bench-ds9 | 32 |

The DS-side traces show the failing pattern explicitly:

```
[FS] dispatch_compound:269: dispatch op=OP_SEQUENCE(53) curr_op=0 ss=...
[FS] dispatch_compound:295: dispatch done op=OP_SEQUENCE(53) curr_op=0 ss=...
[NFS] dispatch_compound:296: compound=... c_op=0 op=OP_SEQUENCE status=NFS4ERR_BADSESSION(10052) ...
[FS] dispatch_compound:335: dispatch op=OP_SEQUENCE(53) FAILED status=NFS4ERR_BADSESSION(10052)
[FS] rpc_protocol_op_call:711: op 100003/1 ret=10052 xid=...
```

Per `lib/nfs4/server/session.c:776-797` (`nfs4_op_sequence`),
`NFS4ERR_BADSESSION` returns from exactly one place: the
`nfs4_session_find(...)` lookup returns NULL.  So whatever
sessionid the client is sending on these failing SEQUENCEs,
the DS's `ss_session_ht` doesn't have it.

The misleading wire signature is that the client log shows
`tag="reclaim_complete"` / `tag="chunk_write"` failing -- both
of those tags route through `mds_compound_send` regardless of
whether the destination is the MDS or a DS, because the same
client library serves both endpoints (`mds_session` is shared
between MDS and DS connections; the "mds" prefix in the
function name is a misnomer once the layout client uses it for
DS sessions too).

Revised triage targets, narrower:

1. **DS-side `nfs4_session_find` lookup miss** -- the only path
   to `NFS4ERR_BADSESSION`.  Why is the sessionid being sent by
   the client not in the DS's `ss_session_ht`?
   - DS-side `nfs4_session_alloc` returns NULL on a session-id
     hash collision (`err_put_client`), and the client logs
     `mds_log_nfs4_err("CREATE_SESSION", ...)` -- look for that
     log on the client side and the corresponding DS-side
     CREATE_SESSION trace to confirm session-id collision is
     the trigger.
   - DS-side session being destroyed between `CREATE_SESSION`
     return and the client's next op.  Candidate triggers:
     * `nfs4_session_destroy_zombies` in
       `nfs4_op_create_session` -- destroys all
       `NFS4_SESSION_IS_ZOMBIE` sessions for the same client.
       But zombies only exist via `nfs4_session_reparent_for_replace`
       (RFC 8881 case 7), which only fires when EXCHANGE_ID
       returns an existing client with a different
       verifier or principal.  Each ec_demo writer uses a
       distinct `--id`, so this shouldn't fire.  Worth
       confirming by adding a TRACE in
       `nfs4_session_destroy_zombies` showing what it unhashed.
     * Lease reaper destroying the session via
       `nfs4_session_destroy_for_client` --> `client_expire`
       path.
     * A bug in `nfs4_op_destroy_session` that races with
       `nfs4_op_sequence`: SEQUENCE looks up the session ref,
       DESTROY_SESSION fires from another connection, unhashes
       the session, and SEQUENCE on the now-unhashed-but-still-
       reachable session takes a stale path.  This is unlikely
       since `nfs4_session_find` checks the hash table and the
       URCU mechanics serialize.

2. **Why does `--mode disjoint` PASS while the RMW modes
   FAIL?**  Disjoint exercises full-stripe writes only -- no
   `ec_read_stripe_with_file` (no CHUNK_READ).  The failing
   modes always invoke RMW.  So the RMW path on the client
   (`ec_read_stripe_with_file` --> `ds_chunk_read`) might be
   the only path that triggers whatever destroys the DS
   session.  Worth comparing one passing and one failing run
   trace side-by-side to see the difference in DS session
   creation/destroy events.

3. **First DS-side BADSESSION timing.**  Pull the timestamp of
   the first BADSESSION on each DS and correlate to the
   client's first DS session creation.  If `[CREATE_SESSION
   OK]` --> `[BADSESSION immediately]` with no other events in
   between, the bug is in CREATE_SESSION itself (returning a
   session that isn't actually in the hash table); if there's
   a `[CREATE_SESSION OK]` --> `[some destroy event]` -->
   `[BADSESSION]` sequence, the destroy is the smoking gun.

The probe-client LD path issue (separate item in the
outstanding list) blocks publishing INV-1 counter deltas
alongside this analysis, but doesn't block the triage of the
DS-side session lifecycle itself.

**2026-06-01 -- lease-reaper hypothesis FALSIFIED.**

Tested by patching `lib/nfs4/server/lease_reaper.c`:
`LEASE_EXPIRE_FACTOR_NUM` 3 -> 100, making the effective
expiry ~37 minutes instead of ~67 seconds.  Rebuilt reffsd,
restarted MDS + DSes, waited for `health=healthy`, re-ran all
four sub-modes.

Result: the failure pattern is unchanged:

| Mode | Result |
|------|--------|
| disjoint | PASS (unchanged) |
| chunk-split | FAIL: same BADSESSION cascade |
| overlap | FAIL: same BADSESSION cascade |
| subchunk | FAIL: same BADSESSION cascade |

The writer-0 log on `--mode chunk-split` shows the same wire
signature as the unpatched run -- 8x `tag="reclaim_complete"`
BADSESSION immediately after CREATE_SESSION, then
`tag="chunk_write"` BADSESSION, then `[596.123]
ec_write_stripe: stripe 0 data[0] FAILED: -121`, then 8x
`tag="destroy_session"` BADSESSION on shutdown.  Timing
remains seconds, not minutes -- the reaper isn't the
mechanism.

Patch reverted before any further work.

Narrowed hypothesis space:

- **DS-side session destroy from a non-reaper path.**  The
  remaining candidates are
  `nfs4_session_destroy_for_client` (called from
  `nfs4_client_expire`),
  `nfs4_session_destroy_zombies` (called from
  `nfs4_op_create_session`), or a manual unhash on a
  client-eviction path that doesn't go through the reaper.
  Need TRACE instrumentation on the unhash sites to see
  which one fires under RMW load.
- **DS-side sessionid mismatch.**  The client may be
  encoding the sessionid into the `SEQUENCE` differently
  than what the DS hashed.  A direct correlation between
  the bytes in the client's `ms->ms_sessionid` and the bytes
  in the DS's `ns->ns_sessionid` for the same xid would
  rule this in or out.
- **Concurrent EXCHANGE_ID -> CREATE_SESSION race on the
  DS.**  When 4 writers each issue `EXCHANGE_ID +
  CREATE_SESSION` to the same DS within a few ms, a server-
  side race in `nfs4_client_alloc_or_find` or
  `nfs4_session_alloc` might leave the wrong client/session
  bound to the wrong sessionid, with a valid-looking
  CREATE_SESSION response.  This would explain why the
  client's SEQUENCE doesn't match anything in the hash
  table.

Next slice's lightest probe: a fprintf in
`nfs4_session_unhash`, `nfs4_session_destroy_zombies`, and
`nfs4_client_expire` that prints the sessionid (or the
ns_sessionid + nc clientid pair).  After a rebuild + re-run,
correlate any DS unhash event with the BADSESSION the client
sees right after it.

**2026-06-01 -- instrumentation ran, the real primary failure
is BAD_STATEID on CHUNK_READ, not BADSESSION.**

Added LOGs in `nfs4_session_alloc` (post-add), `nfs4_session_unhash`,
and `nfs4_session_destroy_zombies` on a throwaway
`wip-t1b-session-trace` branch.  Rebuild + restart + re-run on
`--mode chunk-split`.

What the DS-side LOGs showed:

- ds0 / ds1: ONE `session_alloc` per writer + ONE
  `destroy_zombies` (entered after CREATE_SESSION; nothing to
  unhash since this is each writer's first session on this DS).
- **No `session_unhash` events on the DSes during the smoke
  window.**  Sessions stay in the hash table.  The
  earlier "session destroyed" narrative is wrong.

What the writer log shows once the head of the log is read
(prior `tail -25` was truncating it):

```
ec_demo: writing 8192 bytes ... at offset 0 (4+2, shard=4096, range mode)
mds_compound_send: COMPOUND tag="chunk_read" op[2]=OP_CHUNK_READ(83) status=NFS4ERR_BAD_STATEID(10025)
mds_compound_send: COMPOUND tag="chunk_read" op[2]=OP_CHUNK_READ(83) status=NFS4ERR_BAD_STATEID(10025)
mds_compound_send: COMPOUND tag="reclaim_complete" op[0]=OP_SEQUENCE(53) status=NFS4ERR_BADSESSION(10052)   (x6)
mds_compound_send: COMPOUND tag="chunk_write" op[0]=OP_SEQUENCE(53) status=NFS4ERR_BADSESSION(10052)
ds_chunk_write: COMPOUND failed status=10052 (resarray_len=1)
[141.103] ec_write_stripe: stripe 0 data[0] FAILED: -121
mds_compound_send: COMPOUND tag="destroy_session" op[0]=OP_SEQUENCE(53) status=NFS4ERR_BADSESSION(10052)   (x6)
[141.123] ec_write_range: write stripe 0 failed: -121
```

The **first** failure on the wire is `CHUNK_READ ->
NFS4ERR_BAD_STATEID`, twice (client retry).  This is the
tight-coupling stateid validation path: the DS-side
`nfs4_op_chunk_read` looks the layout stateid up in the trust
table; if not found, returns BAD_STATEID.

What the MDS log shows in the same window:

```
[NFS] LAYOUTERROR: trust gap healed for ino=2
      (BAD_STATEID re-registered on 6 DS(es))
[NFS] LAYOUTERROR: trust gap healed for ino=2
      (BAD_STATEID re-registered on 6 DS(es))
```

So the trust-gap recovery path fires (`stateids.md` step 2.7,
`trust-stateid.md`).  It re-registers TRUST_STATEID on the 6
DSes carrying this file's mirrors and returns NFS4_OK for the
LAYOUTERROR.  But by then the client's retry path on the DS
session has already torn it down or moved off it, and the
subsequent client compounds (the RECLAIM_COMPLETE retry on
re-create, then the CHUNK_WRITE, then DESTROY_SESSION) all hit
BADSESSION.

So the BADSESSION cascade is **downstream** of a BAD_STATEID
on CHUNK_READ.  The earlier "DS lease reaper destroys
sessions" hypothesis was wrong because the primary failure
isn't a session-destroy at all -- it's a trust-stateid lookup
miss on the DS during RMW.

Revised triage targets, narrower again:

1. **Why does CHUNK_READ get BAD_STATEID under multi-writer
   RMW?**  Candidates:
   - LAYOUTGET's TRUST_STATEID fan-out finishes after the
     client's CHUNK_READ arrives (race on layout completion).
   - Two writers share the same `stateid.other` (per-inode
     key) in the trust table, and one writer's LAYOUTRETURN /
     LAYOUTERROR removes the shared entry while the other is
     mid-read.
   - The trust table entry was registered correctly but the
     in-flight CHUNK_READ uses a different stateid (different
     seqid) that lookups against `stateid.other` should still
     match, unless the lookup is keyed on the full stateid.
2. **Why does the BAD_STATEID retry path trash the DS
   session?**  After the second BAD_STATEID, the client
   surfaces -ESTALE up to `ec_layout_refresh`, which presumably
   tears down DS sessions and re-establishes.  The re-establish
   path may be on a different connection that the DS doesn't
   associate with the existing session, leaving CREATE_SESSION
   on the new connection orphaned from the running compound.
3. **MDS-side trust-gap healing is firing twice in this run.**
   That's the design path responding to the BAD_STATEID.  If it
   succeeds (and the log line says "healed"), why does the
   client's CHUNK_READ still fail on retry?

Substantial finding: the trust-stateid implementation under
concurrent partial-stripe RMW is the bug surface.  Disjoint
passes because full-stripe writes don't invoke CHUNK_READ.
The bug is NOT a generic NFSv4.1 session lifecycle issue.

**2026-06-01 -- root cause locked: trust-table key is per-inode,
shared by concurrent writers.**

Extended the WIP instrumentation with LOG in
`trust_stateid_register`, `trust_stateid_revoke`, and
`trust_stateid_find`.  Re-ran `--mode chunk-split` against the
patched MDS+DSes.  ds1's trust-event timeline (xtimes /
sessionid context redacted):

```
[+ 0.000] register  other=0500...e136a16c  iomode=1 (READ)  ino=3
[+ 0.002] revoke    other=0500...e136a16c  hit=Y            ino=3
[+ 0.114] find      other=0500...e136a16c  hit=N -- BAD_STATEID
[+ 0.116] register  other=0500...e136a16c  iomode=2 (RW)    ino=3
[+ 0.153] revoke    other=0500...e136a16c  hit=Y            ino=3
[+ 0.176] find      other=0500...e136a16c  hit=N -- BAD_STATEID
[+ 0.178] register  other=0500...e136a16c  iomode=2 (RW)
[+ 0.289] find      other=0500...e136a16c  hit=Y
```

The exact same 12-byte `other` (`0500000003000000e136a16c`)
gets registered, revoked, re-registered, revoked, re-registered
multiple times within hundreds of milliseconds.  The MDS log
shows the matching `LAYOUTERROR: trust gap healed for ino=2
(BAD_STATEID re-registered on 6 DS(es))` events firing twice
in response.

The trust table key is `stateid.other` (12 bytes -- see
`trust_match` in `lib/nfs4/server/trust_stateid.c`).  When two
writers hold layouts on the same MDS file simultaneously,
their stateids share the same `other`.  Writer A's
LAYOUTRETURN fires `trust_revoke` keyed on `other`; the
operation deletes the shared trust entry; writer B's next
CHUNK op calls `trust_stateid_find` and gets NULL ->
BAD_STATEID.

The MDS's "trust gap healed" path then re-registers, and
writer B's retry MAY succeed -- but if writer A revokes again
before writer B's retry actually issues, the thrash repeats.
Each thrash cycle the client retries up to 3x per
`ec_pipeline.c` ("ec_chunk_write retries NFS4ERR_BAD_STATEID
three times"), then bails up to `ec_layout_refresh` which
tears down + recreates the DS session.  That recreate path is
where the BADSESSION cascade originates -- not because the DS
destroyed the session, but because the client tore it down
and the recreate is racing with the trust thrash.

`disjoint` PASSES because each writer covers one full stripe
with no `ec_read_stripe_with_file` call -- the entire READ-half
LAYOUTGET / CHUNK_READ / LAYOUTRETURN cycle never fires, so
the per-inode trust entry isn't being shared between
concurrent operations.

`trust-stateid.md` anticipated the in-place update risk in the
NOT_NOW_BROWN_COW comment on `te_iomode`.  The deeper issue is
**entry lifetime**: not just the iomode field, but the entry
itself can't be shared between writers under multi-writer RMW.

Fix space (out of scope for the current triage slice):
- Change the trust table key from `stateid.other` to the full
  stateid (other + seqid).  Each writer's stateid becomes its
  own entry with its own lifetime.  Cleanest, matches NFSv4.1
  per-layout-stateid semantics.
- Or: reference-count the trust entry on register, only
  unhash when the count drops to zero on revoke.
- Or: track per-writer iomode lists on the same entry and
  revoke only when every writer's reference is gone.

**2026-06-01 -- the (other + seqid) fix is a NO-OP for this
workload; the real bug is in MDS exclusive-layout enforcement.**

Shipped a (other + seqid) trust-table re-key (commit
`8f442303574a` on the topic branch) on the working hypothesis
that concurrent writers shared `stateid.other`.  Re-instrumented
with the fix in place + a `seqid=...` field added to every
trust_register / revoke / find LOG.  Re-ran `--mode chunk-split`.

Trace on ds1:

```
[+ 0.000] register other=03000000030000... seqid=1 iomode=RW   ino=3
[+ 0.135] find     other=03000000030000... seqid=1 hit=Y
[+ 0.314] register other=05000000030000... seqid=1 iomode=READ ino=3
[+ 0.316] revoke   other=05000000030000... seqid=1
[+ 0.319] register other=07000000030000... seqid=1 iomode=READ ino=3
[+ 0.451] register other=05000000030000... seqid=1 iomode=RW   ino=3
[+ 0.453] find     other=07000000030000... seqid=1 hit=Y
[+ 0.491] revoke   other=05000000030000... seqid=1
...
```

Three observations falsify the "shared other" hypothesis:

1. **Every layout stateid has `seqid=1`.**  The MDS doesn't bump
   seqid across LAYOUTGET cycles in this codebase -- each
   `layout_stateid_alloc` mints a fresh stateid that starts at
   `seqid=1`.  Adding seqid to the trust table key changes
   nothing: with seqid identical across all entries, the
   composite key collapses to `other` alone.
2. **Writers DO have distinct `other` values.**  `0300...`,
   `0500...`, `0700...`, `0800...`, `0900...` all appear --
   different `s_id` per `layout_stateid_alloc` call.  So
   per-writer trust entries already existed before the rekey.
3. **The revoke targets the *correct* per-writer stateid.**
   Each revoke removes the specific stateid that the MDS just
   told it to remove.  No collateral damage on neighboring
   entries.

The real mechanism is in `nfs4_layoutget_check_conflicts`
(`lib/nfs4/server/layout.c:855`).  When writer B does
LAYOUTGET on an inode that already has another client's layout
stateid, the MDS:

1. Sends CB_LAYOUTRECALL_FNF to that client (fire-and-forget).
2. **Immediately fans out FANOUT_REVOKE_STATEID to all DSes
   for that client's stateid**, deleting the DS trust entries
   for the writer that was still using them.
3. Returns the new layout to writer B.

The code's own comment captures the intent:
> The recall is best-effort and fire-and-forget -- the MDS does
> not wait for the client's DELEGRETURN/LAYOUTRETURN.
> Correctness comes from the synchronous REVOKE_STATEID below,
> not from the client honoring this recall.

The MDS is enforcing **exclusive layouts**: only one client
holds a layout on a file at a time, and the next LAYOUTGET
forcibly evicts the prior holder via REVOKE_STATEID rather
than waiting for LAYOUTRETURN.  For partial-stripe RMW
workloads with N writers, each writer's LAYOUTGET cycle (and
RMW does at least one READ-layout + one RW-layout cycle per
stripe) triggers an immediate REVOKE on the other writer.
The other writer's in-flight CHUNK ops hit BAD_STATEID; the
client retries up to 3x; if still failing, `ec_layout_refresh`
tears down + recreates the DS session and the BADSESSION
cascade we've been chasing surfaces.

`disjoint` PASSES because each writer's range maps to a
different set of stripes, so per-stripe LAYOUTGET on writer A
doesn't touch the inode-layouts the other writers hold on
THEIR stripes.  The MDS's exclusivity check is per-inode --
if all writers were on the same single-stripe file there
would be the same thrash regardless of whether they overlap
at the byte level.  (Worth re-checking: do the disjoint
writers actually all hit `nfs4_layoutget_check_conflicts`
treating each other as conflicts and just happen to win the
race?  Or is the conflict check checking iomode such that
non-overlapping read+read passes?)

Reverting the no-op (other + seqid) fix (commit
`8d9e039c9d7f`).  The test
`test_register_same_other_distinct_seqid` is dropped along
with the code -- with the old key on just `other`, the
invariant the test asserts doesn't hold and the test would
fail.  If we eventually want that invariant for forward
compatibility (e.g. when LAYOUTGET seqid-bumps land), we can
re-introduce both code and test together.

Real fix space:

1. **MDS-side: don't immediately revoke; wait for LAYOUTRETURN
   with timeout.**  Send CB_LAYOUTRECALL, set a recall
   deadline, complete the new LAYOUTGET on the recalled
   client's LAYOUTRETURN or after the timeout.  This is the
   RFC 8881 §12.5.5.1 recall semantics.  The downside is
   added latency on every contended LAYOUTGET.
2. **MDS-side: allow concurrent layouts of compatible
   iomodes.**  Multiple READ layouts can coexist; one writer
   plus N readers if the underlying chunk-store can
   atomically serialize the writes.  RFC 8881 §12.5.4
   actually permits this for the file layout type; FFv2's
   own design needs to decide whether it does too.
3. **Client-side: on BAD_STATEID, drive a fresh LAYOUTGET
   before the next CHUNK retry.**  Today the client's
   ec_chunk_write retries 3x with the same stateid before
   bailing to `ec_layout_refresh`.  If a single BAD_STATEID
   triggered an immediate fresh LAYOUTGET, the writer would
   recover faster -- but the next LAYOUTGET also evicts the
   other writer, so the thrash continues.  Doesn't fix it.

Option 1 is the architecturally correct path.  Option 2 is
appealing for FFv2 (the chunk-store enforces ordering at the
write level, so multiple concurrent layouts is genuinely
safe), but needs design discussion.  Option 3 is a Band-Aid.

**2026-06-01 -- option 2 partial fix shipped, exposes a
second revoke mechanism.**

Gated `nfs4_layoutget_check_conflicts` on layout type --
`LAYOUT4_FLEX_FILES_V2` skips the FANOUT_REVOKE_STATEID
exclusivity check (commit `1e2fde440e2e` on the topic branch).
v1 and file layouts keep the exclusivity.

Result on the 4-mode smoke against the patched build:

| Mode | Result | Notes |
|------|--------|-------|
| `disjoint` | PASS | unchanged |
| `overlap` | **NEW SIGNAL** | `ec_read_stripe: stripe 2 decode failed: -5` -- this is the actual concurrent-write surface Track 1b was designed for (RMW prefix reads see inconsistent shards). |
| `chunk-split` | FAIL | same wire signature as before (RECLAIM_COMPLETE BADSESSION x9 -> CHUNK_WRITE BADSESSION -> -121) |
| `subchunk` | FAIL | same |

Server-side BADSESSION counts after the run:

| Container | BADSESSION count |
|-----------|------------------|
| reffs-bench-mds | 0 |
| reffs-bench-ds0 | 60 |
| reffs-bench-ds1 | 96 |

So the fix is correct in direction (the `overlap` mode now
reaches the real collision surface instead of dying at
LAYOUTGET conflict) but **incomplete** for `chunk-split` and
`subchunk`.  A second mechanism is still revoking
something the DSes hold.  Candidates:

1. Another FANOUT_REVOKE_STATEID call site
   (`grep -rn FANOUT_REVOKE_STATEID` shows only the
   conflict-check, but the writer logs show the BADSESSION
   cascade still fires -- so either there's an indirect
   path, or the DS-side session table is losing sessions
   for a separate reason).
2. The DS-side session table itself losing sessions, not
   the trust table.  Earlier session-lifecycle LOGs (before
   this slice) showed NO `nfs4_session_unhash` events during
   the smoke window on DSes, but those LOGs were on the
   pre-fix build -- worth re-running with both the fix AND
   the session-lifecycle instrumentation in place to
   confirm.
3. The OPEN / CLOSE state machine on the DS file mistreating
   concurrent OPENs.  CHUNK ops are gated on a server-side
   OPEN of the DS file; under multi-writer load some OPEN
   path might be evicting the prior OPEN's state.

The `overlap` decode failure is meaningful regardless of
whether `chunk-split` / `subchunk` are also fixed: the
harness is producing actual concurrent-write corruption
signal on the wire, which is the entire point of Track 1b.
The focus shifts from "BADSESSION cascade" (a downstream
symptom that turned out to be two mechanisms stacked) to
"RMW shard inconsistency under multi-writer" (the real
surface the validation is meant to detect).

Next slice candidates:
- Re-instrument with both layout-type fix AND
  session-lifecycle + trust-table LOGs at once, re-run
  chunk-split, identify the second mechanism.
- Or pivot to the `overlap` decode failure as the primary
  bug surface and triage why concurrent writers produce
  inconsistent shards.

**2026-06-01 -- second mechanism identified: per-stripe
DS-session create/destroy churn in `ec_pipeline.c`.**

Re-instrumented `nfs4_session_unhash` with caller markers
(`destroy_for_client`, `destroy_zombies`, `DESTROY_SESSION_op`,
`session_release`).  Re-ran chunk-split.  ds1 trace shows the
writer's own session lifetime is only ~200ms, torn down by a
client-initiated `DESTROY_SESSION_op`, then subsequent
compounds the writer queued on the dying session hit
BADSESSION:

```
[17:24:31.803] trace_unhash from=destroy_zombies clid=658
[17:24:31.996] trace_unhash from=DESTROY_SESSION_op
[17:24:31.996] session_unhash sid=0200... clid=658
[17:24:32.288] OP_SEQUENCE FAILED BADSESSION (compound on dead sid)
```

Root cause: `ec_pipeline.c`.  `ec_read_stripe_with_file` and
`ec_write_stripe_with_file` each construct a fresh
`struct ec_context`, call `ec_resolve_mirrors` (EXCHANGE_ID +
CREATE_SESSION + RECLAIM_COMPLETE on every DS), do I/O, then
call `ec_disconnect_all` which sends DESTROY_SESSION +
DESTROY_CLIENTID to every DS.  For chunk-split RMW: 1 stripe
x (RMW = 2 calls) x N mirrors = 2*N full DS-session lifecycle
events per writer per stripe.  Under concurrent N writers,
the storm produces the BADSESSION cascade.

`ec_write_codec_with_file` (whole-file path) already does the
right thing -- one ctx, one resolve_mirrors, loop over stripes.
`ec_write_codec_range` (the partial-range RMW added for
Track 1b) does NOT -- it calls the per-stripe wrappers in a
loop, each with its own setup/teardown.

Hypothesis-test (NOT MERGED) confirmed the mechanism by
commenting out the `mds_session_destroy` call in
`ec_disconnect_all`.  Result: the BADSESSION cascade
disappeared from DS traces.  BUT: `LAYOUTGET` started
returning only 1 mirror instead of the required 6
(`need 6 mirrors, got 1`), so even disjoint failed at prefill.
Leaked client/session state breaks MDS-side bookkeeping; the
naive bypass is not viable.

Proper fix is the `ec_pipeline.c` **refactor**: factor the
per-stripe inner loops so a caller can supply a pre-populated
ctx and share DS sessions across stripes.
`ec_write_codec_range` and `ec_read_codec_range` then do ONE
setup at the top, ONE teardown at the bottom, with the inner
loop reusing the ctx -- exactly the structure
`ec_write_codec_with_file` already uses for the whole-file
path.  Scope: ~480 lines of complex existing code to refactor
without regressing the whole-file and PS-proxy callers,
including the BAD_STATEID retry and layout-refresh paths.
1-2 hours of careful work + smoke.

**Pivoted to the `overlap` decode failure as the next slice
per the "find or pivot" directive.**  The session-churn fix
stays on the followup list -- it's a real architectural bug,
but it's tangential to the validation thesis.  The overlap
decode is the actual concurrent-write surface Track 1b was
designed to detect (RMW shards mismatching on read after
multi-writer concurrent CHUNK_WRITE).

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

**2026-06-01 -- ec_pipeline refactor SHIPPED, but did NOT fix the
chunk-split / overlap / subchunk modes.**

Commit `efaff665437b` on main:
`nfs4/ps/ec_pipeline: ec_write_codec_range shares ctx across stripes`.

Pattern shipped:

- New optional `void *ctx_in_out` last parameter on
  `ec_write_stripe_with_file` and `ec_read_stripe_with_file`.
  NULL preserves today's setup/teardown semantics (PS-proxy
  callers, `ec_read_codec_range`).  Non-NULL points at a caller-
  owned `struct ec_context`: the per-stripe function copies state
  in, skips `ec_resolve_mirrors` / `ec_disconnect_all` /
  LAYOUTGET / LAYOUTRETURN, does I/O, copies state back out.
- `ec_write_codec_range` rewritten: build one shared ctx at the
  top (codec + LAYOUTGET(RW) + ec_resolve_mirrors), thread it
  through every per-stripe call, tear down once at the bottom.
  ASAN/UBSAN-clean build via shadow's docker builder.

Re-ran all four sub-modes on shadow (`MDS=127.0.0.1:2049`,
clean container restart + healthy MDS).

| Mode | Result | First failure |
|------|--------|---------------|
| `disjoint` | PASS | -- |
| `chunk-split` | FAIL | `stripe 0 data[0] FAILED: -121` |
| `overlap` | FAIL | `stripe 2 data[0] FAILED: -121` |
| `subchunk` | FAIL | `stripe 0 data[0] FAILED: -121` |

Per-writer log (writer-0.log on chunk-split, distilled):

```
ec_demo: connecting to MDS 127.0.0.1:2049 (owner shadow:writer0, ...)
ec_demo: writing 8192 bytes ... at offset 0 (4+2, shard=4096, range mode)
mds_compound_send: COMPOUND tag="reclaim_complete" status=NFS4ERR_BADSESSION  (x10)
mds_compound_send: COMPOUND tag="chunk_write"     status=NFS4ERR_BADSESSION
ds_chunk_write: COMPOUND failed status=10052 (resarray_len=1)
[806.544] ec_write_stripe: stripe 0 data[0] FAILED: -121
mds_compound_send: COMPOUND tag="destroy_session" status=NFS4ERR_BADSESSION  (x10)
ec_demo: write failed: -121
```

Same wire signature as the pre-refactor build.  The refactor was
necessary but not sufficient.  Mechanism #3 is still active.

Diagnostic: the BADSESSION cascade now starts with the VERY FIRST
post-CREATE_SESSION compound (RECLAIM_COMPLETE).  That rules out
the in-stripe per-DS-session churn the refactor targeted (a
ctx-rebuild would have shown success on RECLAIM_COMPLETE and only
failed mid-loop).  Something earlier kills the session.

The earlier `nfs4_layoutget_check_conflicts` fix is still in
place (commit `1e2fde44`) -- gating on layout type so FFv2 skips
FANOUT_REVOKE_STATEID exclusivity.  Confirmed present in shadow's
checkout.

Remaining hypotheses:

1. A second `FANOUT_REVOKE_STATEID` call site (or equivalent
   session-invalidator) that fires under concurrent
   LAYOUTGET / OPEN to the same file.  The earlier grep for
   FANOUT_REVOKE_STATEID showed only the conflict-check, but
   maybe `client_expire` or another path tears down sessions
   for "old" clients when a new EXCHANGE_ID for the same
   client_owner arrives with a different verifier.
2. EXCHANGE_ID verifier collision.  Each writer has a distinct
   `--id` (`shadow:writer0`, `shadow:writer1`).  If their
   verifiers happen to coincide (e.g. all derived from the
   same boot epoch) AND there's a server-side path that treats
   the second writer's CREATE_SESSION as a "replace_client"
   that tears down the first writer's session, that fits the
   pattern -- the FIRST writer's RECLAIM_COMPLETE would hit
   BADSESSION the moment the SECOND writer's CREATE_SESSION
   completes.
3. Per-stripe `LAYOUTGET(RW)` issued by `ec_write_codec_range`
   on each writer still racing with the OTHER writer's
   LAYOUTGET, and some non-conflict-check path is triggered
   under multi-client OPEN-on-same-file.

The disjoint PASS is now critically informative: disjoint
writers each open and write a DISTINCT chunk range with no
overlap.  The failing modes have writers OPENing and LAYOUTGETing
on the SAME chunk range simultaneously.  The differentiator
between disjoint and the others is "concurrent OPEN/LAYOUTGET on
the same file region", not "RMW".

Next slice candidates:

- Add MDS-side LOG to every code path that invokes
  `nfs4_session_destroy_for_client`, `nfs4_session_unhash`, or
  any inline session-teardown to find which one fires during
  the failing modes.  Run the chunk-split smoke against the
  instrumented build, see what fires and why.
- Move sideways: triage the `overlap` decode failure that
  surfaced briefly under the previous slice.  That's the real
  Track 1b validation signal; the BADSESSION cascade is still
  swallowing the test on these other modes.

The refactor itself is a clean architectural fix that stays in
mainline regardless -- per-stripe DS-session churn was a real
scaling defect (2*N session lifecycle events per writer per
stripe) and the new shared-ctx pattern is the right shape.  It
just doesn't fix THIS bug.

**2026-06-01 -- BADSESSION cascade was a stale-build artifact;
real signal is now exposed.**

While instrumenting the client side to find the stale-sessionid
source (Path A from the previous slice), I added a per-mirror
LOG to `ec_resolve_mirrors` printing `ed_host` / `ed_port`.  To
get the LOG into the binary I had to:

1. `touch lib/nfs4/ps/ec_pipeline.c`
2. `make -C lib/nfs4/ps` (rebuild the library)
3. `make -C tools` (rebuild ec_demo, picking up the new .so)

After that rebuild, the chunk-split run produced:

| Writer | Result |
|--------|--------|
| writer-0 | **`ec_demo: write OK`** -- no errors |
| writer-1 | `chunk_read NFS4ERR_NOENT` x6 retries -> `stripe 0 decode failed: -5` |

The BADSESSION cascade is **gone**.  All the
session-lifecycle and trust-table triage on the previous slice
was chasing a phantom: the ec_pipeline.c refactor (commit
`efaff665437b`, "ec_write_codec_range shares ctx across stripes")
was committed and on shadow's source tree, but the DEPLOYED
binary `/home/loghyr/reffs/build/tools/ec_demo` was built on
2026-05-31 and never picked up the refactor.

The benchmark docker `builder` container writes to
`/shared/build` (a docker volume).  The DSes / MDS mount that
volume read-only and use the in-container binary.  But the host
runs ec_demo from `/home/loghyr/reffs/build/tools/ec_demo`,
which is a SEPARATE build tree, populated by the host's own
`make` -- never by the docker builder.  Subsequent re-runs after
`git pull` invoked `docker compose run builder` (which updated
the in-container build), but DID NOT re-run `make -C tools` on
the host.  So every smoke ever since the refactor landed was
actually testing pre-refactor ec_demo against post-refactor
reffsd.

This invalidates the entire chain of triage downstream of the
refactor commit:

- "Path A: client-side stale sessionid cache" -- there is no
  such cache in the refactored code; the stale state was a
  stale binary.
- "Mechanism #3: ec_layout_refresh tears down DS sessions
  preserving stale pointers" -- the actual refactored code
  reuses sessions cleanly across ec_layout_refresh.
- "10 reclaim_complete + 1 chunk_write + 10 destroy_session
  BADSESSION pattern" -- this was the PRE-refactor per-stripe
  setup/teardown churn the refactor was designed to remove.

The trust-table and session-lifecycle LOGs added on
wip-t1b-unhash-trace did show signal correctly -- it just
happened to be the OLD ec_demo's signal, not the refactored
one's.

**Going forward**: the actual Track 1b validation surface is
the writer-1 failure on this run -- two concurrent partial-
stripe writers, one succeeds, the other hits NFS4ERR_NOENT
on CHUNK_READ during the RMW prefix.  Hypothesis: writer-0's
CHUNK_WRITEs leave blocks in PENDING state until FINALIZE;
writer-1's CHUNK_READ for the RMW prefix doesn't see PENDING
blocks (server returns NOENT) and the decode fails because
too few shards are present.  This is exactly the kind of
ordering bug a chunk-collision harness exists to expose.

The wip-t1b-unhash-trace branch stays on origin for the
next slice's instrumentation if needed.  Real fix work
starts from a clean main, with the discipline check:
**always `make -C tools` on shadow after any pull that
touches lib/nfs4/ps or lib/nfs4/client**.

## Triage: chunk-store sub-stripe atomicity (last-FINALIZE-wins)

With the BADSESSION-cascade noise gone (the stale-build artifact),
the harness exposes exactly the bug the chunk-split / overlap /
subchunk modes were designed to find:

```
mode=chunk-split (k=4 m=2 shard=4KB stripe=16KB file=16KB N=2)
prefill: full-file write of 16384 bytes (bytes = 0x00)
writer-0: write 8192 B at offset 0      (bytes = 0x10 marker)
writer-1: write 8192 B at offset 8192   (bytes = 0x11 marker)
verify writer-0 range 0..8191:    expected 0x10, got 0x00 -- FAIL
verify writer-1 range 8192..16383: expected 0x11, got 0x11 -- PASS
(next run: roles flip -- writer-1 loses while writer-0 wins)
```

The loser's bytes are zeroed (pre-fill value), not garbled.  That
pinpoints the mechanism:

### Why the bytes go to zero, not to the other writer

Each writer does a full-stripe RMW:

1. CHUNK_READ all 4 data shards (sees pre-fill = `0x00 ...`).
2. Overwrite the writer's own half of the buffer in memory.
3. RS-encode the entire stripe -> 4 data shards + 2 parity shards.
4. CHUNK_WRITE all 6 shards to the 6 DSes.
5. CHUNK_FINALIZE the stripe's blocks on every DS.

Crucially, writer-N's local stripe buffer at encode time looks like:

  shards 0..1 = pre-fill if writer-N is writing 8192..16383, else N's marker
  shards 2..3 = pre-fill if writer-N is writing 0..8191,     else N's marker

So writer-0 emits `{wr0, wr0, prefill, prefill}` + parity.
Writer-1 emits `{prefill, prefill, wr1, wr1}` + parity.

At each DS, `chunk_store_write` (`lib/nfs4/server/chunk_store.c:197`)
does `cs->cs_blocks[offset] = *blk;` -- a flat assignment.  No
merge, no rejection, no per-writer queue.  The payload .dat is
overwritten too.

So the contended shards are the data shards in the OTHER writer's
range, where one writer is stamping pre-fill bytes ON TOP of the
other writer's just-written real bytes.  Whichever CHUNK_WRITE
lands last on each DS wins that DS.

Reads succeed (CHUNK_READ pulls k surviving shards and decodes)
but the decoded bytes in the loser's range are pre-fill = `0x00`.

### What the chunk-store actually has

The chunk_block already tracks per-block owner identity
(`cb_gen_id`, `cb_client_id`, `cb_owner_id`, `cb_writer_clientid`,
`cb_payload_id`) -- `lib/nfs4/server/chunk.c:359-385`.  The
INV-1 `cs_pending_displaced` counter
(`lib/nfs4/server/chunk.c:407-413`) already INCREMENTS when a
PENDING block from a different owner gets overwritten.  So the
chunk-store has the contention signal -- it just doesn't act on
it.  The ec_demo client also has no atomicity primitive to take.

### Fix space

Two viable architectures.  Both keep the per-block ownership the
chunk-store already records; they differ on where the
serialisation happens.

**Option A: per-stripe write lock on the MDS (CB_LAYOUTRECALL-like).**

Add a per-inode-stripe lock the MDS hands out to a single writer
at a time before granting a LAYOUTGET(RW) on that stripe.  Other
writers either block on LAYOUTGET, or get a partial-stripe layout
that excludes the contended stripe.  Same drumbeat as RFC 8881
S12.5.5.1 layout recall, but at per-stripe granularity.

Pros: simple correctness; matches existing layout-stateid model;
chunk-store and ec_demo unchanged.

Cons: kills the parallel-writes property the FFv2 chunk-store
arbitration was supposed to provide.  Disjoint workloads
(currently PASS) stay fast; sub-stripe contention serializes.
Worst case = one writer at a time on a hot stripe.

**Option B: sub-shard write tracking + delta-parity in the
chunk-store.**

Each CHUNK_WRITE carries the byte range it actually modified
within the shard (a `[off, len]` annotation on the existing
`cwa_chunks` payload, or a new wire field).  The chunk-store
maintains a "dirty range" per pending block and accepts multiple
writers' PENDING contributions as long as their dirty ranges are
DISJOINT within the shard.  CHUNK_FINALIZE merges contributions
shard-by-shard.

Parity is the hard bit: each writer must send a delta-parity (the
XOR of "old bytes -> new bytes" pushed through the RS matrix), and
the chunk-store XORs it into the on-disk parity instead of
clobbering.  Same trick mainline software RAID-5 uses for RMW.

Pros: keeps both writers running in parallel; correct for
arbitrary disjoint sub-stripe RMW; the disjoint-mode PASS
generalises to chunk-split/overlap/subchunk.

Cons: substantial work -- wire-format change (ASCII XDR additions
to `lib/xdr/nfsv42_xdr.x` and the v2 draft), ec_demo client
rewrite to compute delta-parity instead of full-stripe parity,
chunk-store XOR-merge for parity shards + range-tracking for data
shards, FINALIZE rewrite to coordinate per-writer per-shard
completion.  Touches the FFv2 draft.

**Option C (informational): widen `cs_pending_displaced` into a
loud failure.**

When CHUNK_WRITE arrives at a block with a PENDING entry from a
different owner whose dirty range overlaps, return
NFS4ERR_DELAY (or a new NFS4ERR_CHUNK_BUSY).  Client retries
after the prior writer COMMITs.  Simpler than A or B but
collapses to serialisation under contention with worse
worst-case latency (the prior writer must FINALIZE+COMMIT
before the retry will succeed).

Useful as an intermediate step: ships visibility before either
deeper fix lands, prevents silent data loss in the meantime.

### Recommendation

Ship **Option C** as a defensive correctness gate first (single
slice, ~50 LOC in chunk.c + a probe counter + a harness mode that
re-runs chunk-split with the expectation of NFS4ERR_CHUNK_BUSY
+ retry success instead of last-FINALIZE-wins).  That removes
the silent corruption.

Then choose between **A** (correct, simple, single-writer
performance ceiling) and **B** (correct, complex, parallel
performance ceiling) as a follow-on, based on whether IOR `-F 0`
shared-file performance matters more than implementation cost.
For the BAT demo: Option A is the lower-risk path.  For long-
term FFv2 viability against HPC shared-file workloads: Option B
is the architecturally correct one.

Track 2 (PS-based IOR `-F 0`) is what would actually exercise B
at scale; until that's ramped, A's serialisation cost is the
same as not having the bug at all.

## Option C first cut: PENDING-PENDING gate shipped, doesn't fire

Commit `d8a09448671b` adds the defensive gate: refuse CHUNK_WRITE
with NFS4ERR_DELAY when the target block already holds a PENDING
entry from a different (co_id, cg_client_id) writer.  Binary
deployed clean on shadow (`strings reffsd | grep cs_chunk_busy_delay`
returns 1 in both the host and in-container `.libs/reffsd`).

Re-ran all four modes against the gated build.  Result is
unchanged from before the gate: chunk-split / overlap / subchunk
still show last-FINALIZE-wins corruption, disjoint still PASSes.
**The `cs_chunk_busy_delay` counter on every DS is zero.**  The
gate is correct code that the test workload never reaches.

Diagnosis: the two writers don't actually overlap at the
PENDING-PENDING stage.  Writer-0's full per-block lifecycle
(CHUNK_WRITE -> PENDING -> CHUNK_FINALIZE -> FINALIZED ->
CHUNK_COMMIT -> COMMITTED) completes faster than writer-1's
gap between its own per-block CHUNK_WRITEs.  So writer-1's
CHUNK_WRITE finds the block COMMITTED, not PENDING, and the gate
short-circuits.  Then the COMMITTED bytes get clobbered by
writer-1's stale-RMW payload.

The real surface is **COMMITTED-overwrite with stale-read**:
writer-1 RMW-read happened BEFORE writer-0's CHUNK_COMMIT made
its bytes visible, so writer-1's local encode used the
pre-fill payload for writer-0's shards; writer-1's CHUNK_WRITE
then stamps that stale payload onto the already-committed
writer-0 bytes.

### The protocol already has the CAS primitive

`cwa_guard` (write_chunk_guard4 in lib/xdr/nfsv42_xdr.x:3586)
is exactly the right field:

```
union write_chunk_guard4 switch (bool cwg_check) {
    case TRUE:  chunk_guard4 cwg_guard;
    case FALSE: void;
};
```

The semantic is "fail this write if the block's existing
{cg_gen_id, cg_client_id} does not match `cwg_guard`".  The
writer presents the version it read; the server CAS-compares
on write.  This is the standard RAID-5-RMW / log-structured
atomicity primitive, already in the FFv2 draft and already in
reffs's XDR.

ec_demo today never uses it -- `lib/nfs4/ps/chunk_io.c:80`
hardcodes `cwa->cwa_guard.cwg_check = FALSE` for every
CHUNK_WRITE.  And every writer stamps the constant
`{cg_gen_id=1, cg_client_id=1}` (lines 75-76), so even if the
guard were enabled, every writer's "version" looks the same to
the server.

### Full Option C scope (next slice)

Five touch points to make Option C actually enforce sub-stripe
RMW correctness:

1. **ec_demo: per-write monotonic cg_gen_id.** Initialize a
   per-process counter at startup (e.g., pid << 32 | counter,
   or just incrementing from a randomised base); bump on every
   CHUNK_WRITE so each write stamps a unique version.

2. **ds_chunk_write: pass cb_gen_id/cb_client_id captured from
   the matching CHUNK_READ as cwa_guard.cwg_guard, with
   cwg_check=TRUE.**  When the writer is doing RMW, the version
   it read MUST be the version it expects to overwrite.

3. **ds_chunk_read: return cr_owner.co_guard so the client
   captures the version.**  Today CHUNK_READ already includes
   cr_owner per the XDR (line 3623 of nfsv42_xdr.x); the client-
   side decode needs to surface co_guard up to the caller.

4. **ec_pipeline RMW path: thread the captured guard from
   read-time through to write-time.**  Currently the RMW loop
   throws away the CHUNK_READ response after copying the bytes;
   the {gen_id, client_id} per shard must travel from
   ec_read_stripe_with_file into ec_write_stripe_with_file.

5. **chunk.c: enforce cwa_guard.cwg_check=TRUE.** Look up the
   existing block at the offset; if its {cb_gen_id, cb_client_id}
   differ from the wire guard, return NFS4ERR_DELAY without
   writing.  Same code path as the PENDING-PENDING gate, just
   broadened to cover COMMITTED.

This makes Option C protocol-correct (uses the existing draft
mechanism rather than bolting on a side-channel check) and
covers the actual stale-RMW corruption path.  Estimated scope:
~150 LOC across ec_demo + chunk_io.c + ec_pipeline.c + chunk.c.

The PENDING-PENDING gate already shipped stays in place -- it's
correct, narrow, and fires for the (real, just rare) racing-
within-PENDING case that the broader CAS gate also has to cover.

Pinning the BAT decision: the deeper Option B (delta-parity)
remains the right long-term answer for parallel performance.
Option C with full CAS is the correct semantics; the BAT demo
runs it serialised at acceptable cost as long as the IOR `-F 0`
throughput target lives in Track 2 (PS-based) rather than direct
ec_demo, which it does.
