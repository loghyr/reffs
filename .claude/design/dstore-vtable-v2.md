<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Dstore Vtable v2: Protocol-Agnostic DS Operations

## Problem

The dstore vtable has two implementations:
- `dstore_ops_local` -- local VFS (combined mode)
- `dstore_ops_nfsv3` -- remote NFSv3 RPC

There is no NFSv4.2 vtable.  File layout DSes speak NFSv4.2 but
the MDS control plane (runway, fence, GETATTR) uses the NFSv3
vtable, which fails against an NFSv4.2-only DS.

Additionally, InBand I/O (MDS proxying data for non-pNFS clients)
needs protocol conversion: an NFSv3 client reading a file whose
data lives on an NFSv4.2 DS requires the MDS to read from the DS
via NFSv4.2 and return the data to the client via NFSv3.

## Requirements

### 1. DS Control Plane

The MDS needs these operations against any DS, regardless of
what protocol the DS speaks:

| Operation | Purpose |
|-----------|---------|
| CREATE | Runway pool file creation |
| REMOVE | File cleanup |
| GETATTR | Reflected GETATTR (size, mtime after write) |
| SETATTR(size) | Truncate fan-out |
| SETATTR(mode) | chmod after fence |
| SETATTR(uid,gid) | Fencing (credential rotation) |
| LOOKUP | Runway restart recovery |

These are currently in `struct dstore_ops`.  Each protocol
provides its own implementation.

### 2. InBand I/O (MDS Proxy)

When a client does READ/WRITE through the MDS (no layout, or
the client doesn't support pNFS), the MDS must proxy:

| Operation | Purpose |
|-----------|---------|
| READ | Read from DS, return to client |
| WRITE | Accept from client, write to DS |
| COMMIT | Commit on DS |

These are NOT in the current `dstore_ops`.  Today, the MDS
only does proxy I/O for the local VFS vtable (combined mode).
Remote DSes don't have proxy support -- if a client does MDS
I/O on a file with a remote layout, the MDS returns EIO.

### 3. Protocol Combinations

| DS Protocol | Control Plane | InBand I/O | Layout Data Path |
|------------|---------------|------------|------------------|
| Local VFS | `dstore_ops_local` | direct VFS read/write | N/A (combined) |
| NFSv3 | `dstore_ops_nfsv3` | NFSv3 READ/WRITE to DS | Client-->DS NFSv3 (flex files) |
| NFSv4.2 | `dstore_ops_nfsv4` (NEW) | NFSv4.2 READ/WRITE to DS | Client-->DS NFSv4.1 session (file layout) |

## Design

### New vtable: `dstore_ops_nfsv4`

```c
/* lib/nfs4/dstore/dstore_ops_nfsv4.c (NEW) */
```

Implements the same `struct dstore_ops` interface as the NFSv3
vtable, but using NFSv4.2 COMPOUND operations over a session.

The MDS establishes its own NFSv4.1 session to the DS at
startup (during `dstore_alloc` for remote NFSv4.2 DSes).  This
session is used for both control plane and InBand I/O.

#### MDS-->DS Session: EXCHANGE_ID Flags

The MDS acts as an NFSv4.2 **client** to the DS.  Per RFC 8881
§18.35, the MDS uses `EXCHGID4_FLAG_USE_NON_PNFS` -- it is a
plain NFSv4 client doing control-plane operations and proxy I/O.
It does NOT set `USE_PNFS_MDS` (that would mean "I want you to
be my metadata server", which is the opposite of the intent).

```c
struct dstore {
    ...
    /* NFSv4.2 DS session (MDS-->DS control + proxy I/O) */
    struct nfs4_mds_ds_session *ds_v4_session;
};
```

Session setup at `dstore_alloc` time:
- EXCHANGE_ID with `EXCHGID4_FLAG_USE_NON_PNFS`
- CREATE_SESSION (single slot -- serializes ops per DS,
  see limitations)
- PUTROOTFH + GETFH to get the DS root FH

#### Stateids for InBand I/O

The MDS needs valid stateids to READ/WRITE on the DS.

**Control plane** (CREATE, REMOVE, SETATTR): Uses stateids
from the OPEN that created the runway files.  The runway
already OPENs files; store the stateid in `runway_entry`.

**InBand I/O** (READ/WRITE/COMMIT on behalf of a client):
The MDS must OPEN the DS file before doing I/O.  Two approaches:

1. **Lazy OPEN**: OPEN on first InBand I/O, cache the stateid
   in the `layout_data_file`.  CLOSE when the client closes the
   MDS file (or on layout expiry).  Adds one round-trip on
   first I/O but amortizes over subsequent ops.

2. **Anonymous stateid**: Use the special all-zero stateid
   `{seqid=0, other=0}`.  RFC 8881 §8.2.3: "represents the
   current stateid ... if there is no current stateid, [it]
   is treated as a special anonymous stateid."  This works for
   READ on files with no mandatory locking.  For WRITE, the DS
   must allow writes with the anonymous stateid -- which our DS
   does (no lock enforcement for BAT).

For BAT: **use the anonymous stateid** (option 2).  No OPEN
round-trip needed.  This works because our DS does not enforce
lock semantics for anonymous access.  Full stateid management
(option 1) is NOT_NOW_BROWN_COW.

#### Credentials for DS Operations

| Operation class | Credentials |
|----------------|-------------|
| Control plane (CREATE, REMOVE, fence) | uid=0, gid=0 (MDS service identity) |
| InBand I/O (READ, WRITE, COMMIT) | Synthetic fenced uid/gid from `ldf_uid`/`ldf_gid` |

The InBand I/O vtable signature includes credential info:

```c
ssize_t (*read)(struct dstore *ds, const uint8_t *fh,
                uint32_t fh_len, void *buf, size_t len,
                uint64_t offset,
                uint32_t uid, uint32_t gid);
ssize_t (*write)(struct dstore *ds, const uint8_t *fh,
                 uint32_t fh_len, const void *buf,
                 size_t len, uint64_t offset,
                 uint32_t uid, uint32_t gid);
int     (*commit)(struct dstore *ds, const uint8_t *fh,
                  uint32_t fh_len, uint64_t offset,
                  uint32_t count);
```

The NFSv3 vtable sets AUTH_SYS with the provided uid/gid.
The NFSv4 vtable sets the AUTH_SYS header on the compound.
The local vtable ignores credentials (VFS access is direct).

#### Control Plane Operations

Each `dstore_ops` function maps to an NFSv4.2 compound:

| dstore_ops | NFSv4.2 Compound |
|-----------|-----------------|
| `create` | SEQ + PUTFH(dir) + OPEN(CREATE) + GETFH |
| `remove` | SEQ + PUTFH(dir) + REMOVE(name) |
| `getattr` | SEQ + PUTFH(fh) + GETATTR |
| `truncate` | SEQ + PUTFH(fh) + SETATTR(size) |
| `chmod` | SEQ + PUTFH(fh) + SETATTR(mode) |
| `fence` | SEQ + PUTFH(fh) + SETATTR(uid,gid) |
| `lookup` | SEQ + PUTFH(dir) + LOOKUP(name) + GETFH |

#### InBand I/O Operations

| dstore_ops | NFSv4.2 Compound |
|-----------|-----------------|
| `read` | SEQ + PUTFH(fh) + READ(stateid, offset, len) |
| `write` | SEQ + PUTFH(fh) + WRITE(stateid, offset, data) |
| `commit` | SEQ + PUTFH(fh) + COMMIT(offset, count) |

Stateid: anonymous `{0, 0}` for BAT (see above).

### InBand I/O Detection

An inode's data lives on a remote DS when:

```c
inode->i_layout_segments != NULL &&
inode->i_layout_segments->lss_count > 0
```

This is true even after the client returns the layout -- the
layout segments persist on the MDS inode because they describe
WHERE the data is, not whether a client currently holds a
layout grant.

The `layout_data_file` within the segment has `ldf_dstore_id`
which identifies the DS.  If `ldf_dstore_id` maps to the local
VFS (dstore 1 in combined mode), the existing `data_block`
path is used.  If it maps to a remote DS, InBand I/O is used.

Decision tree in `nfs4_op_read` / `nfs3_op_read`:

```c
struct layout_segments *lss = inode->i_layout_segments;
if (lss && lss->lss_count > 0) {
    struct layout_data_file *ldf = &lss->lss_segs[0].ls_files[0];
    struct dstore *ds = dstore_find(ldf->ldf_dstore_id);
    if (ds && ds->ds_ops->read) {
        /* Remote DS: proxy through dstore vtable */
        ret = ds->ds_ops->read(ds, ldf->ldf_fh, ldf->ldf_fh_len,
                               buf, len, offset,
                               ldf->ldf_uid, ldf->ldf_gid);
        dstore_put(ds);
    } else if (ds && ds->ds_ops == &dstore_ops_local) {
        /* Local VFS: use existing data_block path */
        dstore_put(ds);
        /* fall through to data_block_read */
    } else {
        dstore_put(ds);
        ret = -EIO;  /* DS not available */
    }
} else {
    /* No layout: use local data_block */
    ret = data_block_read(inode->i_db, buf, len, offset);
}
```

**DS unavailable during InBand I/O**: Return -EIO to the client.
The client sees `NFS4ERR_IO` (NFSv4) or `NFS3ERR_IO` (NFSv3).
This is correct -- the data is unreachable.

### Vtable Selection

In `dstore_alloc()`, the vtable is selected based on config:

```c
if (address is local)
    ds->ds_ops = &dstore_ops_local;
else if (ds_config->protocol == DS_PROTO_NFSV3)
    ds->ds_ops = &dstore_ops_nfsv3;
else if (ds_config->protocol == DS_PROTO_NFSV4)
    ds->ds_ops = &dstore_ops_nfsv4;
```

#### Config

```toml
[[data_server]]
id       = 1
address  = "192.168.2.128"
path     = "/"
protocol = "nfsv3"          # flex files DS (default)

[[data_server]]
id       = 2
address  = "192.168.2.129"
path     = "/"
protocol = "nfsv4"          # file layout DS
```

The `protocol` field is NEW.  Default: `"nfsv3"` (backward
compatible -- existing configs without this field work unchanged).

### Async InBand I/O

The MDS READ/WRITE handlers currently use io_uring for local
file I/O (via the backend ring).  For remote DS I/O, the
operations are blocking RPCs.

For BAT: **synchronous** (worker thread blocks on DS RPC).
This is simple and correct.

**Worker starvation risk**: With 8 workers and a 10-second DS
RPC timeout, 8 concurrent InBand I/O operations to a slow DS
exhausts all workers, stalling non-proxy operations.

Mitigation for BAT: use a short DS RPC timeout (3 seconds)
and document that InBand I/O to remote DSes is latency-bound.
NOT_NOW_BROWN_COW: async proxy I/O with task_pause/resume
using a separate thread pool for DS RPCs.

### Filehandle Size

NFSv3 FHs are up to 64 bytes (`FHSIZE3`).  NFSv4 FHs are up
to 128 bytes (`NFS4_FHSIZE`).

The `layout_data_file` struct has `ldf_fh[RUNWAY_MAX_FH]`
where `RUNWAY_MAX_FH` = 64.  This is insufficient for NFSv4
DSes.  Increase to 128 for NFSv4 compatibility.

The on-disk `.layouts` format stores FHs.  No deployed
persistent storage exists, so no migration needed.

### Stride / Offset Mapping (File Layouts)

File layouts with striping: the client uses
`(offset / stripe_unit) % nfl_fh_list_len` to pick the stripe.
The MDS must do the same mapping for InBand I/O.

With a single DS and 1 FH (current BAT config), all offsets
map to FH[0] on DS[0].  No offset translation needed for
SPARSE mode.

NOT_NOW_BROWN_COW: DENSE mode offset translation
(DS offsets = MDS offsets / stripe_count).

## Test Plan

### Existing tests affected

- `dstore_ops.h` changes (adding read/write/commit) require
  `dstore_ops_local` and `dstore_ops_nfsv3` to provide the new
  function pointers (or NULL).  Existing tests that use the
  vtable indirectly (via runway, layout, etc.) must still pass.
- `nfs4_op_read/write` in `file.c` and `nfs3_op_read/write` in
  `server.c` gain InBand detection.  Existing CI integration
  tests (git clone over NFS) exercise these handlers -- any
  regression is caught.
- `layout_data_file.ldf_fh` size change from 64-->128 bytes
  affects the on-disk `.layouts` format.  No deployed storage,
  so no migration.  Persistence tests round-trip within the
  same format.

### Unit tests

**Session lifecycle** (`lib/nfs4/dstore/tests/`):
- `test_nfsv4_session_create`: MDS-->DS session succeeds
- `test_nfsv4_session_create_ds_down`: DS unreachable --> dstore
  state is DISCONNECTED, not crash
- `test_nfsv4_session_reconnect`: DS comes back --> session
  re-establishes on next operation

**Control plane** (`lib/nfs4/dstore/tests/`):
- `test_nfsv4_create`: MDS creates file on NFSv4 DS
- `test_nfsv4_getattr`: MDS gets size from NFSv4 DS
- `test_nfsv4_fence`: MDS sets uid/gid on NFSv4 DS file
- `test_nfsv4_truncate`: MDS truncates file on NFSv4 DS

**InBand I/O** (`lib/nfs4/dstore/tests/`):
- `test_inband_read_local`: InBand READ on local VFS
- `test_inband_read_nfsv3_ds`: InBand READ from NFSv3 DS
- `test_inband_read_nfsv4_ds`: InBand READ from NFSv4.2 DS
- `test_inband_write_nfsv4_ds`: InBand WRITE to NFSv4.2 DS
- `test_inband_ds_down`: DS unreachable --> EIO to client
- `test_inband_detection`: Inode with layout_segments -->
  uses InBand path; inode without --> uses data_block

**Config** (`lib/config/tests/`):
- `test_config_ds_protocol_default`: missing protocol --> nfsv3
- `test_config_ds_protocol_nfsv4`: protocol = "nfsv4"
- `test_config_ds_protocol_invalid`: protocol = "foo" --> error

**Credentials** (`lib/nfs4/dstore/tests/`):
- `test_inband_uses_fenced_creds`: InBand WRITE uses
  ldf_uid/ldf_gid, not root

### Functional tests

- NFSv3 client reads file written by NFSv4.2 file layout client
- NFSv3 client writes file, NFSv4.2 client reads via file layout
- `du`/`df`/`stat` correct after InBand write to remote DS
- Truncate through MDS propagates to NFSv4.2 DS
- `ci_space_test.sh` passes on `/files` export

## Implementation Order

1. **FH size**: increase `RUNWAY_MAX_FH` to 128
2. **Config**: add `protocol` field to `[[data_server]]`
3. **NFSv4.2 MDS-->DS session** using ec_demo client library
   (EXCHANGE_ID `USE_NON_PNFS` + CREATE_SESSION, single slot)
4. **`dstore_ops_nfsv4`** -- control plane ops
5. **InBand vtable extension** (read/write/commit with creds)
6. **Local VFS InBand** -- already works via data_block
7. **NFSv3 InBand** -- NFSv3 READ/WRITE/COMMIT RPCs
8. **NFSv4 InBand** -- SEQ+PUTFH+READ/WRITE/COMMIT
9. **InBand detection** in nfs3_op_read/write, nfs4_op_read/write
10. **Test: NFSv3 client reads file on NFSv4.2 DS**

## Deferred / NOT_NOW_BROWN_COW

- Async proxy I/O (task_pause/resume for DS RPCs, separate
  thread pool to avoid worker starvation)
- DENSE mode offset translation
- DS session failover / reconnection (session expiry during
  long idle causes all ops to fail -- the session has no renewal
  thread; consider adding one if idle DSes are common)
- DS session renewal thread
- Full stateid management (OPEN/CLOSE DS files for InBand I/O
  instead of anonymous stateid)
- CHUNK ops vtable (FFv2 DS protocol)
- Multi-DS striping for InBand I/O (parallel fan-out)
- Write-behind / read-ahead caching on the MDS proxy path
- AUTH_SYS --> RPCSEC_GSS credential mapping for DS RPCs
- Multi-slot DS session (concurrent InBand I/O to same DS;
  current single-slot serializes all operations)
- Probe visibility for DS protocol and session state

## Key Files

| File | Change |
|------|--------|
| `lib/nfs4/dstore/dstore_ops_nfsv4.c` | NEW -- NFSv4.2 vtable |
| `lib/include/reffs/dstore_ops.h` | Add read/write/commit with creds |
| `lib/include/reffs/runway.h` | Increase RUNWAY_MAX_FH to 128 |
| `lib/nfs4/dstore/dstore.c` | Protocol-based vtable selection |
| `lib/config/config.c` | Parse `protocol` field |
| `lib/include/reffs/settings.h` | `ds_protocol` enum/field |
| `lib/nfs3/server.c` | InBand READ/WRITE via dstore ops |
| `lib/nfs4/server/file.c` | InBand READ/WRITE via dstore ops |
| `lib/nfs4/dstore/dstore_ops_nfsv3.c` | Add read/write/commit |
| `lib/nfs4/dstore/dstore_ops_local.c` | Add read/write/commit |
| `lib/nfs4/dstore/tests/` | New unit tests |
| `lib/config/tests/config_test.c` | Protocol config tests |
