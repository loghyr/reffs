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

## Design Rules

- dstores are round-robin'd when fewer than k+m are available
  (multiple data files on the same DS is allowed)
- No root squashing from MDS IP to DSes; no access enforcement
  on the DS side (trust the MDS)
- DSes must be available over NFSv3
- Layouts come from the set of configured dstore pairs
