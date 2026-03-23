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
- **NFSv4.2 client**: Reed-Solomon erasure coding across data servers
  referenced by Flex Files layouts

## Near-Term Demo Target (by 2026-04-12)

Demonstrate Flex Files v2 + erasure coding end-to-end:

1. MDS issues Flex Files v2 layout pointing at N data servers
2. Client writes data with Reed-Solomon encoding, distributing
   data + parity chunks across data servers
3. Client reads back and reconstructs from any sufficient subset

Requires:
- Flex Files v1/v2 MDS in reffs (LAYOUTGET returning ff_layout_v2)
- NFSv4.2 client with layout awareness
- Unencumbered Reed-Solomon implementation for encoding/decoding

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

### 4. Erasure Coding Demo Client — DONE (basic)

`lib/nfs4/client/` + `tools/ec_demo`:
- MDS session (EXCHANGE_ID, CREATE_SESSION, SEQUENCE)
- COMPOUND builder (build argarray, clnt_call, parse results)
- File ops (PUTROOTFH + OPEN + GETFH, CLOSE)
- Layout ops (LAYOUTGET + ff_layout4 decode, GETDEVICEINFO + uaddr parse,
  LAYOUTRETURN)
- DS I/O (NFSv3 READ/WRITE with synthetic AUTH_SYS credentials)
- RS codec integration (ec_write/ec_read with stripe padding)
- Plain (non-EC) put/get/check for single-mirror testing
- 15 unit tests (compound builder + stripe math)

**2026-03-23: plain put/get/check verified against run-combined.**
EC commands exist but need multi-dstore testing.

### 5. CHUNK ops
All 11 CHUNK_* operations in `lib/nfs4/server/chunk.c`.

## Deferred / Out of Scope (initially)

- Copy/clone ops
- Extended attributes
- Lease renewal / expiry enforcement
- CB_GETATTR for authoritative timestamps (stub: fall back to server-side values)
