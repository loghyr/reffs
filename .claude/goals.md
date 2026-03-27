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

### 5. CHUNK ops — DONE (happy path + persistence)

`lib/nfs4/server/chunk.c` + `lib/nfs4/server/chunk_store.c`:
- CHUNK_WRITE: validate CRC32, store data + per-block metadata (PENDING)
- CHUNK_READ: return FINALIZED/COMMITTED blocks with data + CRC,
  recompute CRC from disk for bit rot detection
- CHUNK_FINALIZE: PENDING → FINALIZED transition + persist metadata
- CHUNK_COMMIT: FINALIZED → COMMITTED transition + persist + disk sync
- Persistent chunk store per inode: `<state_dir>/chunks/<ino>.meta`,
  write-temp/fdatasync/rename, crash-safe.  Per-block: state, flags
  (CHUNK_BLOCK_LOCKED), chunk_owner4, CRC32, chunk_size.
  RocksDB-ready (fixed-size records indexed by offset).
- Client CRC verification on read (network corruption detection)
- Remaining 7 CHUNK ops return NFS4ERR_NOTSUPP (stubs)

### 5a. Flex Files v2 Layout — DONE

- MDS supports both LAYOUT4_FLEX_FILES (v1) and LAYOUT4_FLEX_FILES_V2 (v2)
- Client-driven selection (Linux kernel → v1, ec_demo → v1 or v2)
- v2 layout: encoding type, chunk size, FFV2_DS_FLAGS per mirror
- Both layout types advertised in FS_LAYOUT_TYPES

### 5b. Benchmark Infrastructure — DONE (4 tiers)

`deploy/benchmark/` + `scripts/ec_benchmark.sh` + `scripts/ec_benchmark_full.sh`:
- 13-container Docker setup (1 MDS + 10 DSes + builder + client)
- Single-run full benchmark: v1 SIMD, v1 scalar, v2 SIMD, v2 scalar,
  each with healthy + degraded-1 reads.  CSV output with layout/simd/cpu columns.
- SIMD: AArch64 NEON, x86_64 AVX2, x86_64 SSE2 (fallback), forced-scalar
- `--force-scalar` flag on ec_demo bypasses SIMD for A/B comparison
- `--layout v2` exercises CHUNK_WRITE/FINALIZE/COMMIT with CRC + persistence
- Report: `deploy/benchmark/results/ec_benchmark_full_report.html`
  (self-contained, email-safe).  Generated by `scripts/gen_benchmark_report.py`.

#### Tiered Evidence Strategy

| Tier | Platform | Geometry | Status |
|------|----------|----------|--------|
| 1 | dreamer: M4 MacBook, Fedora 43 VM (VMware Fusion) | 4+2 | Done (2026-03-24) |
| 2 | dreamer: same | 4+2 degraded, stripe, 8+2 | Done (2026-03-25) |
| 3 | mana (M4 NEON), kanigix (i9 AVX2), adept (N100 AVX2), garbo (Ryzen 7 AVX2) | 4+2, 8+2, SIMD+scalar+degraded | Done (2026-03-26) |
| 4 | adept + garbo | v1 vs v2 CHUNK (RS), CRC read verification | Done (2026-03-27) |
| 5 | Data center VMs, separate hosts | Multi-geometry with real network latency | Planned |

#### Key Benchmark Findings (2026-03-27)

**v1 (NFSv3) codec comparison at 1 MB, mana M4 NEON:**

| Codec | 4+2 write | 4+2 read | 8+2 write | 8+2 read |
|-------|-----------|----------|-----------|----------|
| Plain | 86 ms | 58 ms | — | — |
| RS | 108 ms | 89 ms | 99 ms | 81 ms |
| Mojette-sys | 104 ms | 92 ms | 91 ms | 83 ms |
| Mojette-nonsys | 109 ms | 237 ms | 111 ms | 391 ms |

**v2 (CHUNK) overhead vs v1 (RS at 1 MB):**
- Write: +7–22% (compound RPC + FINALIZE/COMMIT round-trips + CRC + persistence)
- Read: +2–10% (CRC recompute on server + CRC verify on client)

**Cross-platform:** Overhead ratios consistent across 4 machines, 2 ISAs
(aarch64 NEON, x86_64 AVX2).  SIMD vs scalar within noise at 4 KB shards.

**Degraded reads:** Mojette-sys 8+2 reconstruction adds +2% — essentially free.

**Recommendation:** Mojette-sys 8+2 for interactive workloads (best read
latency, lowest storage overhead, near-zero reconstruction).  RS 4+2 as
conservative fallback.  Mojette-nonsys unsuitable for reads.

### 6. Flex Files v2 IETF Draft — IN PROGRESS (2026-03-27)

Draft: `draft-haynes-nfsv4-flexfiles-v2` at `~/ws/flexfiles-v2/`

**Review tooling**: Created `~/ws/ietf-review-prompts/` (CC0 licensed)
with Claude Code skills for automated IETF draft review.  Covers
RFC 2119, XDR, IANA, security, editorial, and idnits checks.  Detects
Martin Thomson's i-d-template Makefile and runs `make idnits` locally.

**Draft fixes applied (2026-03-27):**
- 38 review findings resolved across all categories (STRUCTURE, RFC 2119,
  XREF, XDR, IANA, Security, Editorial)
- XREF: fixed 8 stale v1 field name references in prose
  (ffl_stripe_unit, ffs_mirrors, ffm_data_servers, ffv2ds_fh_vers,
  ffv2ds_stateid → correct v2 field names)
- XDR: defined ffv2_key4; added encoding types to ffv2_coding_type4
  enum; fixed write_chunk_guard4 missing `switch`; renamed colliding
  field prefixes (cra_ → crb_ for ROLLBACK, ccr_ → cfr_ for FINALIZE
  resok, cwr_status → cwr_block_status); added cwa_flags +
  CHUNK_WRITE_FLAGS_ACTIVATE_IF_EMPTY + cwr_block_activated
- IANA: fixed layout type value 0x6 → 0x5; corrected intro sentence
- Security: added CRC32 scope, chunk lock/lease, error disclosure sections
- idnits: eliminated 15 non-ASCII chars; reformatted 5 wide tables;
  converted ops-and-errors table to ASCII art
- Submitted as -03

**XDR sync**: `lib/xdr/nfsv42_xdr.x` updated to match all draft XDR
changes.  `lib/nfs4/server/chunk.c` and `lib/nfs4/client/chunk_io.c`
updated for renamed fields.

### Known Issues

- **io_uring large message stall**: NFSv3 RPCs >~32KB stall in the
  server's io_uring read pipeline. Workaround: EC_SHARD_SIZE = 4KB.
  SIMD benefit will appear when shard sizes increase to 64KB+.
- **TIRPC connection sharing**: multiple clnt_create to same host:port
  causes hangs. Workaround: DS connection dedup in ec_resolve_mirrors.
- **v2 CHUNK + combined mode**: DS session multiplexing needed for
  single-host combined mode.  Multi-container Docker works fine.
- **Mojette + v2 CHUNK mismatch**: Mojette projections produce
  variable-sized outputs per direction (B = |p|(Q-1) + |q|(P-1) + 1).
  The v2 CHUNK path uses fixed chunk_size.  Fails at small file sizes
  where projection size doesn't divide evenly.  RS works correctly.
  Fix: per-shard chunk_size or pad projections to common size.
- **CHUNK_HEADER_READ / CHUNK_LOCK / CHUNK_UNLOCK / CHUNK_ROLLBACK**:
  stubbed as NFS4ERR_NOTSUPP.  Lock flag infrastructure (cb_flags with
  CHUNK_BLOCK_LOCKED) is in place but no handler.

## Deferred / Out of Scope (initially)

- Copy/clone ops
- Extended attributes
- RocksDB backend
- LAYOUTRETURN body parsing (ff_layoutreturn4 error reports)
- DS session multiplexing for v2/CHUNK combined mode
- Mojette + v2 CHUNK variable chunk size fix
- x86_64 AVX2 benchmarks at larger shard sizes (blocked on io_uring fix)
