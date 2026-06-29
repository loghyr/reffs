<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Explicit DS ports: separate mountd port + force-remote

## Problem

A per-`[[data_server]]` `port` is overloaded for three things:

1. the MDS's MOUNT (mountd) client (`mount_get_root_fh`, `dstore.c:230`)
2. the MDS's NFS (nfsd) client (`dstore.c:316`)
3. the port advertised to the pNFS client in GETDEVICEINFO
   (`layout.c:159`; when unset it hardcodes 2049)

This blocks using a real knfsd as a DS on a non-2049 port:

- knfsd runs mountd and nfsd on *separate* ports, so one `port` cannot
  drive both #1 and #2.
- Pinning `port` to the nfsd port makes the MDS's MOUNT hit nfsd and
  fail; leaving `port` unset advertises 2049 to the client even when
  nfsd is elsewhere.

Separately, `dstore_alloc` routes `127.0.0.1` / `localhost` / any of the
host's own interface IPs (`dstore_address_is_local`) to the *local VFS*
vtable (combined mode), so a same-host or link-local knfsd is never
contacted over the wire.

## Design

Two changes, gated on an explicit port:

1. **New `[[data_server]] mount_port`** (uint16).  The MOUNT (mountd)
   client uses `mount_port` when set, else falls back to `port`.  The
   NFS client and the GETDEVICEINFO uaddr keep using `port`.  A reffs
   DS (one port for both) sets only `port`; a knfsd DS sets both.

2. **Explicit `port` forces the remote vtable.**  When `port > 0`,
   `dstore_alloc` skips the local-address heuristic and selects the
   NFSv3 (or NFSv4, per `protocol`) vtable.  This lets a knfsd on a
   link-local / same-host address be treated as a real wire DS.
   Trigger is `port` alone (not "both ports"), so reffs DSes keep
   working with a single port.

Result: a fully no-rpcbind knfsd DS works -- pin knfsd's mountd and
nfsd ports, set `port`+`mount_port`, and the MDS reaches both directly.
The pNFS client never touches mountd (it gets per-file FHs from the
layout and nfsd:port from deviceinfo); only the MDS uses mountd.

## Backward compatibility

No deployed-format change.  The only configs that set a DS `port` today
(`deploy/benchmark/mds-realnet.toml`) use remote LAN IPs that are
already remote, so force-remote changes nothing for them.  No config
sets a `port` on a loopback/local DS, and combined-mode loopback DSes
set no port -- they stay on the local VFS vtable.

## Files

| File | Change |
|------|--------|
| `lib/include/reffs/settings.h` | `reffs_data_server_config.mount_port` |
| `lib/config/config.c` | parse `mount_port` |
| `lib/include/reffs/dstore.h` | `dstore.ds_mount_port`; `dstore_alloc` proto |
| `lib/nfs4/dstore/dstore.c` | `dstore_alloc` param + force-remote; `mount_get_root_fh` MOUNT port |
| `lib/nfs4/dstore/dstore_ops_nfsv4.c`, tests, `dstore_mock.c` | mechanical `dstore_alloc` arg |
| `scripts/run_mds_validate.sh` | `--ds ADDR:/PATH[:NFSPORT[:MOUNTPORT]]` |
| `examples/reffsd-mds-validate.toml` | document `mount_port` |

## Tests

- `lib/config/tests/config_test.c`: `mount_port` parses from
  `[[data_server]]`.
- `lib/nfs4/dstore/tests/dstore_test.c`: `dstore_alloc` with a local
  address + explicit `port` (do_mount=false) selects the NFSv3 vtable,
  not local; with `port == 0` it still selects local.
- Existing dstore tests: mechanical signature update only (force-remote
  fires only when `port > 0`; they pass `0`).
