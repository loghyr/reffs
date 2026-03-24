<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# reffs — Project Goals

## End Goal

A complete **pNFS Flex Files** stack: MDS + data servers + erasure-coding client.

- **reffs as data server**: NFSv3/NFSv4.2 with CHUNK ops for data instances
- **reffs as MDS**: Flex Files v2 metadata server issuing layouts that
  reference reffs data servers (may do v1 first as proof of concept)
- **NFSv4.2 client**: erasure coding (Reed-Solomon, Mojette systematic,
  Mojette non-systematic) across data servers referenced by Flex Files layouts

## Near-Term Demo Target (by 2026-04-12) — ACHIEVED

Flex Files v2 + erasure coding demonstrated end-to-end (2026-03-24):

1. MDS issues Flex Files v1/v2 layout pointing at 6 data servers
2. Client writes data with RS/Mojette encoding (4+2), distributing
   data + parity chunks across data servers via NFSv3 or CHUNK ops
3. Client reads back and reconstructs from any sufficient subset
4. Tier 1 benchmark data collected (4 codecs × 5 file sizes)

Remaining for the demo deadline:
- Tier 2 benchmarks (data center VMs with real network latency)
- Multi-geometry runs (2+1, 8+2, 8+4) to validate scaling model

## Milestones

### 1. Basic NFSv4.2 op set — DONE
Session, filehandle, file I/O, directory, attributes, locking, basic delegation.
See `lib/nfs4/server/` for current state.

### 2. Pre-CHUNK infrastructure — MOSTLY DONE

1. ~~io_uring file I/O~~ — DONE
2. ~~Config file~~ — DONE (TOML)
3. ~~Per-op NFSv4.2 stats~~ — DONE
4. ~~NFSv4.2 error tracking~~ — DONE
5. ~~CB_RECALL~~ — DONE (fire-and-forget)
6. ~~RFC 9754 support~~ — DONE (XOR delegation, timestamps, unit tested)
7. **RocksDB backend** — storage backend alongside existing POSIX backend
8. **Grace lifecycle bug** *(open bug)* — server never leaves `SERVER_GRACE_STARTED`
9. **Full client recovery** — grace period handling, client state reclaim
10. **CB_GETATTR** — requires CB response handling (deferred)

### 3. Flex Files MDS — DONE

All layout operations implemented in `lib/nfs4/server/layout.c`:
- LAYOUTGET / LAYOUTCOMMIT / LAYOUTRETURN / GETDEVICEINFO / LAYOUTERROR
- Dstore vtable (NFSv3 + local VFS) in `lib/nfs4/dstore/`
- Runway (pre-created file pool), async fan-out, reflected GETATTR
- Compound-level dedup (`COMPOUND_DS_ATTRS_REFRESHED`)
- Fencing (synthetic uid/gid rotation)

### 4. Erasure Coding Demo Client — DONE

`lib/nfs4/client/` + `tools/ec_demo`:
- MDS session (EXCHANGE_ID, CREATE_SESSION, SEQUENCE, RECLAIM_COMPLETE)
- COMPOUND builder (build argarray, clnt_call, parse results)
- File ops (PUTROOTFH + OPEN + GETFH, CLOSE)
- Layout ops (LAYOUTGET + ff_layout4/ffv2_layout4 decode, GETDEVICEINFO,
  LAYOUTRETURN, LAYOUTERROR)
- DS I/O: NFSv3 READ/WRITE with synthetic AUTH_SYS credentials
- DS I/O: NFSv4.2 CHUNK_WRITE/READ/FINALIZE/COMMIT (v2 path)
- Codecs: Reed-Solomon, Mojette systematic, Mojette non-systematic
- RS codec integration (ec_write/ec_read with stripe padding)
- Mojette clean-room from published papers (no RozoFS code — GPL-2.0 incompatible)
- Plain (non-EC) put/get/check for single-mirror testing
- --codec flag: rs, mojette-sys, mojette-nonsys
- --layout flag: v1 (NFSv3), v2 (CHUNK ops)
- --id flag: unique client owner per concurrent instance
- DS connection dedup: mirrors on same host share one connection
- Variable shard stride for Mojette non-systematic projections
- 29 unit tests (compound builder, stripe math, Mojette transform,
  Mojette codec, RS codec)

**2026-03-23: plain put/get/check verified against run-combined.**
**2026-03-23: full git CI test passed against Flex Files v1.**
**2026-03-24: all 4 codecs verified end-to-end on combined mode:**
  plain, RS 4+2, Mojette-sys 4+2, Mojette-nonsys 4+2.

### 4a. CB Response Infrastructure — DONE

- CB response handling: pause compound, send CB, wait for reply, resume
- cb_pending with atomic CAS (cb_pending_try_complete) for race safety
- cb_timeout thread with joinable shutdown
- CB_GETATTR: integrated into GETATTR handler for delegated timestamps
  (round-trip works; fattr4 decode/merge is TODO)
- CB_LAYOUTRECALL: per-file layout recall with wait-for-reply
  (fence+revoke on timeout integration is TODO)
- 7 unit tests for cb_pending lifecycle and race safety

### 4b. Lease Renewal and Client Recovery — DONE

- RECLAIM_COMPLETE in ec_demo client
- SEQ4_STATUS_RESTART_RECLAIM_NEEDED during grace
- nc_last_renew_ns lease renewal timestamp in SEQUENCE
- NFS4ERR_GRACE enforcement on OPEN, CREATE, REMOVE, RENAME, LINK, SETATTR
- Lease reaper thread (30s scan, 1.5x lease expiry)

### 5. CHUNK ops — DONE (happy path)

`lib/nfs4/server/chunk.c` + `lib/nfs4/server/chunk_store.c`:
- CHUNK_WRITE: validate CRC32, store data + per-block metadata (PENDING)
- CHUNK_READ: return FINALIZED/COMMITTED blocks with data + CRC
- CHUNK_FINALIZE: PENDING → FINALIZED transition
- CHUNK_COMMIT: FINALIZED → COMMITTED transition + disk sync
- In-memory chunk store per inode (grows dynamically)
- Remaining 7 CHUNK ops return NFS4ERR_NOTSUPP (stubs)

### 5a. Flex Files v2 Layout — DONE

- MDS supports both LAYOUT4_FLEX_FILES (v1) and LAYOUT4_FLEX_FILES_V2 (v2)
- Client-driven selection (Linux kernel → v1, ec_demo → v1 or v2)
- v2 layout: encoding type, chunk size, FFV2_DS_FLAGS per mirror
- Both layout types advertised in FS_LAYOUT_TYPES

### 5b. Benchmark Infrastructure — TIER 1 DONE

`deploy/benchmark/` + `scripts/ec_benchmark.sh`:
- 7-container Docker setup (1 MDS + 6 DSes + builder + client)
- Builder container compiles once, shares via Docker volume
- Healthcheck-based container synchronization
- ec_benchmark.sh: tests all 4 codecs at 5 file sizes, CSV output

**2026-03-24: Tier 1 benchmark complete (same-host Docker, 4+2).**

#### Tier 1 Results (M4 MacBook Pro, same-host Docker, 4+2, 5-run means)

Write latency (ms):

| File size | plain | RS    | Mojette-sys | Mojette-nonsys |
|-----------|-------|-------|-------------|----------------|
| 4 KB      | 13.4  | 15.8  | 16.2        | 16.2           |
| 16 KB     | 15.0  | 17.6  | 18.0        | 17.6           |
| 64 KB     | 22.0  | 25.0  | 25.2        | 25.2           |
| 256 KB    | 27.8  | 37.4  | 37.8        | 38.6           |
| 1 MB      | 66.8  | 103.2 | 102.4       | 108.4          |

Read latency (ms):

| File size | plain | RS   | Mojette-sys | Mojette-nonsys |
|-----------|-------|------|-------------|----------------|
| 4 KB      | 10.4  | 14.2 | 13.4        | 16.2           |
| 16 KB     | 11.8  | 14.8 | 14.6        | 18.2           |
| 64 KB     | 16.4  | 21.8 | 22.2        | 32.8           |
| 256 KB    | 23.0  | 32.2 | 32.6        | 67.2           |
| 1 MB      | 58.2  | 89.2 | 94.8        | 245.4          |

Key findings:
- At 4–64 KB, all EC codecs within 14–22% write overhead — noise vs
  real-network variance.  Kills "EC is too expensive for NFS" objection.
- RS and Mojette-sys statistically indistinguishable on writes.
- RS has ~10% lower read overhead than Mojette-sys at 1 MB.
- Mojette-nonsys read overhead accelerates (321% at 1 MB, not plateauing).
  Useful as contrast, not as a recommendation.
- Systematic codecs: healthy reads are pure parallel I/O (near-plain),
  reconstruction only on failure.

#### Tiered Evidence Strategy

- **Tier 1** (done): Same-host Docker, 4+2.  Proves feasibility, kills
  "impossible" objections.
- **Tier 2** (next): Data center VMs, 4+2.  Adds real network latency.
  Write overhead should stay similar.  Validates model.
- **Tier 3** (reserve): Performance rig, multiple geometries (2+1, 4+2,
  8+2, 8+4).  Validates scaling model empirically.

#### Scaling Model (projected from 4+2 baseline)

Write overhead scales with Y (spare count) only, independent of X
(active count).  Systematic healthy reads are near-plain (parallel
I/O, no projection math).  Key projections at 1 MB:

| Config | Write overhead | Sys healthy read | Sys reconstruction |
|--------|----------------|------------------|--------------------|
| 2+1    | ~27%           | near-plain       | ~16%               |
| 4+2    | 53% (measured) | 63% (measured)   | —                  |
| 8+1    | ~27%           | near-plain       | ~63%               |
| 8+2    | ~53%           | near-plain       | ~126%              |
| 8+4    | ~106%          | near-plain       | ~252%              |

8+1 has same reconstruction cost as 4+2 (Y×X = 8) but half the write
overhead — interesting point in the design space.

### Known Issues

- **io_uring large message stall**: NFSv3 RPCs >~32KB stall in the
  server's io_uring read pipeline. Workaround: EC_SHARD_SIZE = 4KB.
- **TIRPC connection sharing**: multiple clnt_create to same host:port
  causes hangs. Workaround: DS connection dedup in ec_resolve_mirrors.
- **v2 CHUNK path**: DS session multiplexing needed for single-host
  combined mode. Deferred; --layout v1 (NFSv3) is the default.

## Deferred / Out of Scope (initially)

- Copy/clone ops
- Extended attributes
- RocksDB backend
- LAYOUTRETURN body parsing (ff_layoutreturn4 error reports)
- DS session multiplexing for v2/CHUNK path
