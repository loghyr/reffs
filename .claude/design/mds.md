<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# MDS Design

## Terminology

- **dstore**: a DS address + export path pair (e.g., `192.168.2.105:/foo`).
  The atomic unit of backing storage in the MDS configuration.
- **runway**: pre-created pool of empty data files on each dstore,
  ready for immediate assignment at LAYOUTGET time.

## Architecture

The MDS is a standard NFSv4.2 server that additionally implements
LAYOUTGET / LAYOUTCOMMIT / LAYOUTRETURN.  Data servers are generic
NFSv3 servers — no proprietary control protocol.  The MDS acts as an
NFSv3 *client* to each dstore for control-plane operations (CREATE,
REMOVE, GETATTR).

```
┌─────────────┐      NFSv4.2 + layouts      ┌─────────────┐
│  EC Client   │◄──────────────────────────►│     MDS      │
└──────┬───┬──┘                             └──────┬──────┘
       │   │                                       │
       │   │  NFSv3 direct I/O                     │ NFSv3 control
       │   │  (data path)                          │ (create/remove/getattr)
       │   │                                       │
  ┌────▼───▼────┐                            ┌─────▼──────┐
  │  dstore 0   │    ...                     │  dstore N  │
  │ (any NFSv3) │                            │ (any NFSv3)│
  └─────────────┘                            └────────────┘
```

## Configuration

```toml
[server]
role = "mds"
uuid = "auto"           # generated on first boot, persisted

[[data_server]]
address = "192.168.2.105"
path = "/foo"

[[data_server]]
address = "192.168.2.106"
path = "/bar"
```

## File Runway (Pre-Created Pool)

File creation is the bottleneck.  The MDS never blocks LAYOUTGET on
NFSv3 CREATE:

1. **Startup**: connect to each dstore via NFSv3, create a batch of
   empty files at `<export_path>/<mds_uuid>/pool/<seqnum>.dat`,
   stash their FHs in memory.
2. **LAYOUTGET**: pop k+m FHs from the pool (round-robin across
   dstores when fewer dstores than k+m), assign to the inode.
3. **Background replenisher** (post-demo): keep pool above a
   low-water mark by creating files async.
4. **REMOVE / LAYOUTRETURN**: return FHs to pool or delete on DS.

File naming is deterministic from dstore + MDS UUID + sequence number,
so only the dstore reference and FH need to be stored per-inode.

## MDS Inode On-Disk Format

Designed for striping and continuations from the start, even though
the demo uses full byte-range layouts.

```
MDS inode
├── standard attrs (mode, uid, gid, size, times)
└── layout_segments[]              ← supports continuations
    ├── byte_range (offset, length)    ← LAYOUT4_ALL_FILE for demo
    ├── stripe_unit                    ← full file for demo
    ├── stripe_type (k, m, codec)      ← e.g., RS(4,2)
    └── data_files[]               ← per-stripe-index DS mapping
        ├── dstore_id              ← index into dstore table
        ├── ds_fh                  ← NFSv3 filehandle (opaque)
        └── cached_attrs           ← refreshed at LAYOUTRETURN
            ├── size
            ├── mtime, ctime, atime
            └── mode, uid, gid
```

## GETATTR Aggregation

**Deferred for demo.**  When the MDS receives GETATTR for a file with
an outstanding RW layout, it should PAUSE the compound, fan out NFSv3
GETATTR to all DSes, aggregate (max times, effective size), and resume.

For the demo: return cached values from the last LAYOUTRETURN.

## Dstore Control-Plane Operations

The MDS acts as an NFSv3 client to each dstore for file lifecycle
and attribute management.  These are **not** data-path operations
and do **not** count as WRITE layouts.

### File lifecycle

- `dstore_data_file_create(ds, path)` — NFSv3 CREATE
- `dstore_data_file_remove(ds, path)` — NFSv3 REMOVE
- `dstore_data_file_chmod(ds, fh)` — NFSv3 SETATTR: owner rw,
  group r, no other perms (0640)
- `dstore_data_file_truncate(ds, fh, size)` — NFSv3 SETATTR(size).
  Called when the MDS receives SETATTR from a client.  The MDS must
  PAUSE the compound, fan out truncates to all DSes, wait for all
  to complete (write-like: all must succeed), then RESUME.

### Fencing

- `dstore_data_file_fence(ds, fh)` — NFSv3 SETATTR(uid, gid) to
  rotate synthetic IDs within a configured range (default 1024-2048).
  Bumps both uid and gid atomically, wrapping within the range.
  Called in response to client I/O errors to fence a misbehaving
  client off the data file.

### GETATTR aggregation

- `dstore_data_file_getattr(ds, fh)` — NFSv3 GETATTR
- When the MDS receives a client GETATTR and there is an active
  WRITE layout, the MDS must:
  1. PAUSE the compound
  2. Fan out NFSv3 GETATTR to ALL DSes holding mirrors
  3. Wait for responses:
     - ALL mirrors must respond, or return NFS4ERR_DELAY
     - If a mirror doesn't respond, mark it stale in the inode's
       per-dstore state
  4. Update the inode's cached data_file attrs from the responses
  5. If atime/mtime/size changed from the DS values, set the
     inode's atime/mtime/ctime to NOW (clocks are not in sync)
  6. RESUME the compound with the updated attributes

### Write vs read semantics for DS operations

- **Write operations** (truncate, fence, create, remove, chmod):
  ALL DSes in the mirror set must succeed, or the operation fails.
  This matches Client Side Mirroring (CSM) semantics.
- **Read operations** (getattr): ALL mirrors must respond for
  the GETATTR to succeed.  If any mirror is stale (marked from a
  previous failed getattr), don't update the inode's state if that
  mirror advances later without the other mirrors also advancing.

### Per-dstore stale tracking

Each data_file in the inode's layout_segment has a stale flag.
Example with CSM=3: if 2 of 3 mirrors respond to GETATTR, mark
the 3rd as stale.  If the stale mirror later responds with newer
values but the other two don't advance, don't update the inode —
wait for all non-stale mirrors to agree.

## Design Rules

- dstores are round-robin'd when fewer than k+m are available
  (multiple data files on the same DS is allowed)
- No root squashing from MDS IP to DSes; no access enforcement
  on the DS side (trust the MDS)
- DSes must be available over NFSv3
- Layouts come from the set of configured dstore pairs
- Dstore control-plane operations do NOT count as WRITE layouts
