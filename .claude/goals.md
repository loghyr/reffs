<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# reffs — Project Goals

## End Goal

A **pNFS Flex Files data server** implementing NFSv4.2 with full CHUNK op support.

- NFSv3 clients create and populate data instance objects
- NFSv4.2 clients use CHUNK_* ops for high-performance I/O to those instances
- The two protocol families operate independently — no cross-protocol state sharing

## Milestones

### 1. Basic NFSv4.2 op set — DONE
Session, filehandle, file I/O, directory, attributes, locking, basic delegation.
See `lib/nfs4/` for current state.

### 2. Pre-CHUNK infrastructure

Work required before CHUNK ops can be implemented, in priority order:

1. **RocksDB backend** — storage backend alongside existing POSIX backend
2. **io_uring file I/O** — replace/augment file I/O with io_uring
   - Open question: separate submission queues?
3. **Config file** — structured configuration (format TBD: toml/yaml/libconfig)
   - Must express: server type, io_uring tunings, export options
4. **Per-op NFSv4.2 stats** — per-operation statistics at global/per-sb/per-client scope
5. **NFSv4.2 error tracking** — errors globally, per super-block, per client
6. **Callback (CB) infrastructure** — CB channel for CB_RECALL, CB_GETATTR, etc.
   - Do not stack many CB_GETATTR requests
   - Need a compound state machine for pause/resume while waiting on CB
7. **RFC 9754 support** — delegating timestamps; OPEN XOR delegation stateid
   (depends on CB infrastructure)
8. **Grace lifecycle bug** *(open bug)* — server never leaves `SERVER_GRACE_STARTED`
9. **Full client recovery** — grace period handling, client state reclaim

### 3. CHUNK ops — end goal
All 11 CHUNK_* operations in `lib/nfs4/chunk.c`.

## Deferred / Out of Scope (initially)

- Layout support (LAYOUTGET, LAYOUTCOMMIT, etc.)
- Copy/clone ops
- Extended attributes
- Lease renewal / expiry enforcement
- CB_GETATTR for authoritative timestamps (stub: fall back to server-side values)
