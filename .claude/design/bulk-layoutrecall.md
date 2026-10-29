<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# On-demand CB_LAYOUTRECALL (FILE / FSID / ALL) test trigger

## Goal

Let a test fire CB_LAYOUTRECALL of type FILE, FSID or ALL at one
client on demand, resend it after every NFS4ERR_DELAY at a fixed
interval up to a time/count limit, finish on NFS4ERR_NOMATCHING_LAYOUT
or a covering LAYOUTRETURN, and record every send and reply so the
exact sequence can be read back.  The consumer is a Linux client
series that changes how the client answers bulk recalls while
flexfiles DS I/O is in flight.

## What exists today

- `nfs4_cb_layoutrecall_send()` / `_fnf()` (`lib/nfs4/server/cb.c`)
  hard-code `LAYOUTRECALL4_FILE`.
- The only DELAY-resend logic is in `nfs4_layout_barrier_resume()`
  (`layout.c`): FFv2 fixed-layout barrier only, tied to a paused
  compound (`cp_task`), exponential 0.5s..4s backoff, 30s deadline,
  fences on expiry.  Wrong shape for a probe-driven recall: no task,
  fixed interval wanted, no fencing wanted.
- `cb_timeout.c` already tracks cb_pending reply deadlines; on
  completion it calls `task_resume(cp->cp_task)`.
- Layout state: one `struct layout_stateid` per (inode, client),
  hashed in both `inode->i_stateids` and `client->c_stateids`;
  `ls_state` carries READ/RW iomode bits.  Layout type is not
  recorded per stateid.  fsid4 is `{ sb_id, 0 }` (`attr.c`).
- LAYOUTRETURN4_FSID / _ALL decode via XDR but the handler only
  ACKs them ("just acknowledge them without stateid tracking"); no
  state is dropped.
- LAYOUTGET returns RECALLCONFLICT only for the fixed-layout barrier.
- Probe op 39 NOTIFY_DEVICEID is the precedent for a probe-driven
  callback test trigger.  No probe op lists clients.

## Design

### Engine: `lib/nfs4/server/layout_recall.c` (new)

A small table of outstanding recalls (`struct layout_recall`), one
per (client, scope).  Each has: target clientid, type, sb_id / ino,
layout type, iomode, changed, interval, limits, state, send count,
and a bounded event log (256 entries).

A driver thread (started from the module's init, per the
module-init rule) owns all sends.  Completions from
`cb_reply_handler` / the timeout thread only record the result and
signal the driver.  The driver never holds its own mutex across a
send, so there is no ordering against `cb_timeout_mutex`.

Change to cb plumbing: add `void (*cp_complete)(struct cb_pending *)`
to `struct cb_pending`; `cb_pending_resume()` / `cb_submit_done()`
call it instead of `task_resume()` when set.  Existing callers leave
it NULL -- no behaviour change.

`nfs4_cb_layoutrecall_send()` grows a `layoutrecall4 *` (or a
recall-type + fsid argument) so FSID and ALL bodies can be encoded;
existing callers pass FILE as today.

### State machine

```
            trigger
  (none) ----------> SENDING --send fails--> FAILED
                        |
                    reply / timeout
                        v
   +---- DELAY -----> WAIT_RESEND --interval elapsed, within limit--> SENDING
   |                    |
   |              limit reached
   |                    v
   |                 EXPIRED            (log, stop; no revoke/fence)
   |
   +-- NOMATCHING_LAYOUT ---------------> DONE  (drop layouts in scope)
   +-- NFS4_OK --------> ACKED --covering LAYOUTRETURN--> DONE
   +-- other status / no reply ---------> FAILED (log, stop)
```

DONE/EXPIRED/FAILED records stay queryable until replaced by a new
trigger for the same client, or cancelled.

### Scope and completion

- FILE: (client, sb_id, ino).  FSID: (client, sb_id).  ALL: (client).
- NOMATCHING_LAYOUT: drop every layout stateid of that client in
  scope (unhash, same as the existing "remaining == 0" path), masked
  by iomode.
- LAYOUTRETURN4_ALL from the client completes any recall of that
  client; LAYOUTRETURN4_FSID completes an FSID recall on the same
  sb; a FILE return of the recalled inode completes a FILE recall.

### Protocol changes in the NFS ops

- LAYOUTRETURN FSID / ALL: actually drop that client's layout
  stateids in scope (current FH's sb for FSID), honouring
  lora_iomode.  RFC 8881 S18.44.3.
- LAYOUTGET: NFS4ERR_RECALLCONFLICT if the caller has an outstanding
  recall (SENDING / WAIT_RESEND / ACKED) whose scope covers the file.
  RFC 8881 S12.5.5.2.1.5.
- LAYOUTRETURN while a bulk recall is outstanding: S12.5.5.2.1.5
  also says FILE returns (with a stateid) and returns of a different
  type get RECALLCONFLICT; enforced.

### Trigger: probe ops 41 / 42 / 43

- `LAYOUTRECALL` (41): clientid (0 = match by IPv4 address string;
  both empty = the single client, error if more than one), recall
  type, layout type (0 = FLEX_FILES), iomode, changed, sb_id, ino
  (FILE), offset/length (FILE), interval_ms (default 1000),
  limit_ms (default 60000), limit_count (0 = no count limit).
  Returns recall id and resolved clientid.  Returns immediately.
- `LAYOUTRECALL_STATUS` (42): recall id (0 = latest) -> state, sends,
  and the event log.
- `LAYOUTRECALL_CANCEL` (43): stop resending, drop the record.

C CLI (`reffs_probe1_clnt --op layoutrecall ...`) and Python
(`reffs-probe.py layoutrecall ...`) ship together.  Every event is
also emitted to the trace file (`REFFS_TRACE_CAT_NFS`, prefix
`layoutrecall:`), one line per event:

```
layoutrecall: id=1 clid=0x... type=FSID sb=10 SEND n=1
layoutrecall: id=1 clid=0x... REPLY n=1 cb=OK op=NFS4ERR_DELAY
layoutrecall: id=1 clid=0x... SEND n=2
layoutrecall: id=1 clid=0x... REPLY n=2 cb=OK op=NFS4ERR_DELAY
layoutrecall: id=1 clid=0x... SEND n=3
layoutrecall: id=1 clid=0x... REPLY n=3 cb=OK op=NFS4ERR_NOMATCHING_LAYOUT
layoutrecall: id=1 clid=0x... DONE dropped=4
```

(CLOCK_REALTIME timestamps come from the trace line prefix and are
also returned by STATUS.)

## Tests (written first)

New `lib/nfs4/tests/layout_recall_test.c`, using a send seam
(engine-level function pointer) and an injected clock, so no
network and no sleeps:

| Test | Intent |
|------|--------|
| `test_fsid_delay_delay_nomatching` | FSID recall: SEND, DELAY, resend after interval, DELAY, resend, NOMATCHING -> DONE; 3 sends; layouts on that sb dropped, other sb untouched; event log exact |
| `test_all_encodes_all` | ALL recall puts LAYOUTRECALL4_ALL on the wire with chosen iomode/changed |
| `test_file_recall_delay_then_ok_return` | FILE recall: DELAY, resend, OK -> ACKED; FILE LAYOUTRETURN -> DONE |
| `test_resend_not_before_interval` | tick before interval elapses sends nothing |
| `test_limit_time_expires` | DELAY forever: EXPIRED at limit_ms, no further sends, layouts NOT dropped |
| `test_limit_count_expires` | same with limit_count |
| `test_layoutget_recallconflict` | LAYOUTGET in scope while outstanding -> RECALLCONFLICT; out of scope (other sb / other client) -> proceeds; after DONE -> proceeds |
| `test_layoutreturn_all_completes` | OK reply then LAYOUTRETURN4_ALL -> DONE, stateids freed |
| `test_layoutreturn_fsid_drops_state` | LAYOUTRETURN4_FSID frees only that sb's stateids for that client |
| `test_cancel` | cancel stops resends |

Existing tests: none should change.  `cp_complete` is NULL for all
existing callers; the send-signature change is mechanical.

## Deliberately not done

- Revoke / fence on expiry (log-and-stop only; NOT_NOW_BROWN_COW).
- Per-stateid layout-type tracking: clora_type is sent as requested,
  but drops apply to all layout stateids in scope.
- Byte-range matching for FILE (whole-file only for completion).
- Device-ID mapping invalidation on bulk return (S20.3.3) -- reffs
  keeps no per-client deviceid mapping state other than notify
  subscriptions, which are left alone.
- Backchannel slot contention: all CBs use slot 0; a concurrent CB
  to the same client during a resend can draw SEQ_MISORDERED.
  Pre-existing.
- FFv2 fixed-layout barrier interaction: a bulk drop does not run
  the barrier's commit/return bookkeeping.
- Persistence: recalls are in-memory only.
