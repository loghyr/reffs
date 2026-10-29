<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# One-shot NFSv4 error injection (test trigger)

## Goal

Make a Linux client invalidate one file's layout stateid without a
LAYOUTRETURN (`pnfs_destroy_layout()`), on demand, while it still has
flexfiles DS I/O outstanding.  The client does that when an MDS
operation on the file gets NFS4ERR_BADHANDLE (or ESTALE) through the
generic v4 exception handler, LAYOUTSTATS or LAYOUTERROR.  The
consumer tests how the client answers a FILE CB_LAYOUTRECALL in that
state (see `bulk-layoutrecall.md`).

## Design

- One injection slot, in memory: `lib/nfs4/server/error_inject.c`,
  `lib/nfs4/include/nfs4/error_inject.h`.
- Arm: sb_id + ino (required), optional op set (up to 16 op numbers),
  optional client (clientid, else IPv4 address, else any client),
  status (default NFS4ERR_BADHANDLE).  Arming replaces any previous
  injection.
- Match point: `dispatch_compound()`, just before an op's handler
  runs, on the *current* filehandle (`compound->c_inode`).  So in
  `SEQUENCE, PUTFH(f), GETATTR` the GETATTR matches; PUTFH does not
  (its current FH is the previous one).
- Empty op set = any op whose current FH is the file, except the ops
  that only set the current FH or the session: SEQUENCE, PUTFH,
  PUTPUBFH, PUTROOTFH, SAVEFH, RESTOREFH.
- On match: the op's status is set to the armed status, the compound
  stops there (as for any failed op), and the injection disarms under
  its mutex, so exactly one op fires even with concurrent compounds.
- Fast path: one relaxed atomic load when nothing is armed.
- Ops on the file from the matching client that the op set excludes
  are counted (`skipped`) so a test can see the client did reach the
  server.
- Observability: STATUS returns state (ARMED / FIRED / DISARMED), arm
  and fire times (CLOCK_REALTIME), the op, its index in the compound,
  the RPC xid and the clientid it fired on.  Trace lines
  (`REFFS_TRACE_CAT_NFS`, prefix `errinject:`) on ARM, FIRE and
  DISARM; FIRE is also LOGged.
- Server state is not touched.  The layout stateid stays; the client's
  next LAYOUTGET (with its open stateid) finds it again through
  `layout_stateid_find_or_create()`.
- Probe ops 44 ERRINJECT (arm), 45 ERRINJECT_STATUS,
  46 ERRINJECT_DISARM; C (`reffs_probe1_clnt --op errinject*`) and
  Python (`reffs-probe.py errinject*`).

## Client facts checked (Linux tree bulk-recall-busy-delay, 90c9c3365063)

- `nfs4_proc_getattr()` builds its `nfs4_exception` without `.inode`,
  so the BADHANDLE case in `nfs4_do_handle_exception()` does not call
  `pnfs_destroy_layout()` for a GETATTR.  GETATTR is matchable here,
  but it does not trigger the layout invalidation on this client.
- `nfs_getattr()` calls `filemap_write_and_wait()` on a regular file
  when the statx mask asks for mtime/ctime/change, so a plain `stat`
  blocks behind stuck DS WRITEs before any GETATTR is sent.
- `nfs4_close_done()` (CLOSE and OPEN_DOWNGRADE) sets
  `exception.inode` and sends errors through
  `nfs4_async_handle_exception()`, which does call
  `pnfs_destroy_layout()`.  A read-only open and close by a second
  process, while the writer holds the file O_WRONLY and no delegation
  is held, sends OPEN then OPEN_DOWNGRADE without flushing.
- reffs grants a delegation only when the OPEN carries a
  WANT_*_DELEG bit (`file.c`); the Linux client sets none by default.

## Tests

`lib/nfs4/tests/error_inject_test.c`, through `dispatch_compound()`:
PUTFH+GETATTR fires once then passes; op-set filter and `skipped`;
other inode / other sb / other client do not match; SEQUENCE/PUTFH
never match in "any op" mode; disarm; re-arm replaces; arm validation.

## Not done

- No PUTFH-argument matching (BADHANDLE on the PUTFH that names the
  file); the client handles a failed GETATTR/CLOSE the same way.
- One slot, not a list; no count > 1; not persisted.
