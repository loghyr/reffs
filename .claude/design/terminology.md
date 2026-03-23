<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# pNFS MDS Terminology

## Data Structures

- **dstore**: DS address + export path pair. Config `[[data_server]]`.
- **instance** (mirror instance): a data file on a DS backing an MDS
  file. Stored as `layout_data_file` with dstore_id, FH, cached attrs.
- **runway**: pre-created pool of empty data files per dstore. LAYOUTGET
  pops FHs from the runway instead of blocking on CREATE.
- **layout segment**: byte range + stripe config + array of instances.
  Persisted in `.layouts` file. Designed for striping/continuations.

## Operations

- **reflected GETATTR**: MDS sends NFSv3 GETATTR to all DSes to
  refresh cached attrs. Triggered by write LAYOUTRETURN. Times set
  to NOW (clock sync not assumed).
- **fan-out**: parallel async dstore operations (one pthread per DS).
  Last thread calls task_resume(). Used for truncate and reflected
  GETATTR. Deduped per compound via `COMPOUND_DS_ATTRS_REFRESHED`.
- **fencing**: rotating synthetic uid/gid on a DS data file to
  invalidate a client's credentials. Triggers: LAYOUTERROR with
  access error, CB_RECALL timeout, initial runway assignment.
- **guarded truncate**: GETATTR for ctime, then SETATTR with
  sattrguard3. Retry on NFS3ERR_NOT_SYNC.

## Semantics

- **CSM** (Client Side Mirroring): client writes to ALL mirrors.
  Write ops must succeed on all or fail.
- **WWWL** (Write Without Write Layout): LOG error when WCC data
  shows DS file changed without an outstanding write layout.
- **synthetic uid/gid**: AUTH_SYS credentials for DS data files.
  Configurable range (default 1024-2048). Carried in layout as
  `ffds_user`/`ffds_group`.

## Architecture

- **vtable**: `dstore_ops_nfsv3` (remote), `dstore_ops_local`
  (combined role, same server). Future: `dstore_ops_nfsv42`.
- **combined role**: single reffsd as MDS+DS. Two super_blocks:
  sb_id=1 (MDS namespace), sb_id=2 (DS namespace). Local dstores
  use VFS vtable, no loopback RPC.
- **compound flags** (`c_flags`): per-compound state bits.
  `COMPOUND_DS_ATTRS_REFRESHED` prevents redundant DS fan-outs.
