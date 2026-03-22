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
See `lib/nfs4/` for current state.

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

### 3. Flex Files MDS — NEW

- LAYOUTGET / LAYOUTCOMMIT / LAYOUTRETURN handlers
- Flex Files v1 device info and layout encoding (proof of concept)
- Flex Files v2 layout encoding
- Data server registry and health tracking

### 4. Erasure Coding Demo Client — NEW

Minimal userspace tool (not a general-purpose NFS client) that:
- Sends LAYOUTGET to the MDS, parses Flex Files layout
- Reed-Solomon encodes writes, distributes data+parity to data servers
- Reads back from data servers, RS-decodes / reconstructs

Purpose: prove the MDS+DS architecture works with pluggable encoding.
RS is the proof-of-concept codec.  Having a clean-room RS demonstrates the
architecture is encoding-agnostic.

- RS codec: clean-room GF(2^8) Vandermonde, no external deps,
  no SIMD (see standards.md patent rules).  Correctness over speed.
- Client talks NFSv4.2 just enough for LAYOUTGET + direct DS I/O
- Codec interface should be designed for swappability from the start
- Later: port logic into the Linux kernel NFS client for production use

### 5. CHUNK ops
All 11 CHUNK_* operations in `lib/nfs4/chunk.c`.

## Deferred / Out of Scope (initially)

- Copy/clone ops
- Extended attributes
- Lease renewal / expiry enforcement
- CB_GETATTR for authoritative timestamps (stub: fall back to server-side values)
