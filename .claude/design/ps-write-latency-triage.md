<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# PS-write latency triage (2026-06-04)

## Trigger

Prereq #4 first-smoke CSV showed every variant-d cell costing ~62s
on writes (and ~10s on reads), independent of size:

```
d,rs,4+2,4096,1,61849,10438,OK,...,2026-06-04T17:26:40Z
d,rs,4+2,1048576,1,61979,10445,OK,...,2026-06-04T17:27:53Z
```

Both 4 KB and 1 MB writes finished within ~130 ms of each other.
Uniform-cost write that ignores byte count = fixed-overhead per
file lifecycle, not per-byte cost.  Prereq #5/#6 would mostly
measure that fixed cost, not codec/geometry/size effects, so this
gates further bench work.

## Root cause (definitive, instrumented)

**The 60 s per dd is the Linux NFS kernel client retrying
OP_DESTROY_CLIENTID once per second against each of the 6 RS-4+2
mirror DSes, after each gets NFS4ERR_STALE_CLIENTID.  10 retries
x 6 DSes x ~1 s = 60 s.  Not a PS code issue, not an MDS code
issue, not network routing, not TLS.**

It happens on every dd, including consecutive ones after
`umount -f` + remount.

## Evidence trail

### Wire decode (the smoking gun)

`tshark` decode of a single 4K dd captured on shadow:

| Op | Count | Per-DS count |
|----|-------|--------------|
| OP_DESTROY_CLIENTID | 60 | 10 each |
| OP_EXCHANGE_ID      | 24 | 4 each |
| SEQUENCE + PUTFH    | 12 | 2 each |
| OP_CREATE_SESSION   | 12 | 2 each |
| SEQUENCE+RECLAIM_COMPLETE | 6 | 1 each |
| OP_DESTROY_SESSION  | 6  | 1 each |

DESTROY_CLIENTID arrival pattern (one DS shown):
```
10.750 -> ds  9
11.762 -> ds  9   (Δ 1.012 s)
12.787 -> ds  9   (Δ 1.025 s)
13.810 -> ds  9   (Δ 1.023 s)
... 10 attempts, then move to next DS ...
20.979 -> ds 11   (first attempt on next DS)
21.003 -> ds 11   (Δ 1.024 s)
...
```

That cadence -- ~1 s between each DESTROY_CLIENTID on the same
DS, 10 attempts per DS, sequential across DSes -- adds up to
exactly the 60 s wait we measure.

### DS handler is RFC-compliant

`lib/nfs4/server/session.c:958` (`nfs4_op_destroy_clientid`):

```c
nc = nfs4_client_find(args->dca_clientid);
if (!nc) {
    *status = NFS4ERR_STALE_CLIENTID;
    goto out;
}
```

Per RFC 8881 §18.50.3, returning NFS4ERR_STALE_CLIENTID for an
unknown clientid is correct behaviour.  The Linux NFS client
treats this as a hint to do state recovery and retries.

### What this is NOT

- **Not PS code**: PS-side trace shows OPEN+LAYOUTGET+
  GETDEVICEINFOx7+LAYOUTRETURN+CLOSE complete in ~657 ms.
- **Not network routing**: same 62 s from cross-subnet client
  (dreamer) and same-subnet client (shadow).
- **Not TLS handshake retries**: 927 SSL handshake failures
  every minute in MDS logs were attributable to a stale
  reffsd PS process (pid 1711497, 2 days old, deleted config).
  Killing it stopped the SSL errors -- dd still takes 63 s.
- **Not stale client state at the PS or MDS**: `umount -f` +
  remount on shadow gives the kernel a fresh clientid for the
  PS, yet both dds after remount still take ~63 s.

### Same-subnet dd timing (confirms it isn't size-dependent)

From shadow as client (same /24 as adept), three consecutive
dds via the same mount:

| Size | Wall (s) | Effective B/s |
|------|----------|---------------|
| 4 KB | 62.256 | 66 |
| 1 MB | 62.093 | 16,900 |
| 4 MB | 61.975 | 67,800 |

Byte rate scales with size; latency is fixed -- the 60 s wait is
per-file-lifecycle, not per-byte.

### Data does land correctly on the DSes

The bench's CSV "verify=OK" outcome is genuine: DS-side `.dat`
files (e.g., `ino_19.dat` for one of the 4 KB writes) hash to
`ad7facb2...` which is SHA256 of 4096 zero bytes -- exactly
what `dd if=/dev/zero` wrote.  The 60 s is dead-air after the
data is already on the wire, before dd returns.

### Why every dd?

Each file OPEN on the PS triggers a fresh LAYOUTGET ->
GETDEVICEINFO sequence -- the layout the MDS issues lists the
6 mirror DSes by their docker container IPs (172.19.0.x).  The
kernel pNFS client tears down its prior per-DS state on each
LAYOUTRETURN/CLOSE and re-bootstraps on the next OPEN; each
re-bootstrap drives the DESTROY_CLIENTID -> EXCHANGE_ID ->
CREATE_SESSION dance, and DESTROY_CLIENTID hits the 10-retry
storm because the DS doesn't have the clientid the kernel
remembers from a prior incarnation.

The 60 s appears per cell, not just per mount.

## Fix options

| Option | Effort | What it buys |
|--------|--------|--------------|
| **Make DS DESTROY_CLIENTID return NFS4_OK for unknown clientid (Recommended)** | ~10 LOC reffs change | RFC-compliant interpretation: "the clientid is gone" can be expressed as NFS4_OK (already-not-there) instead of NFS4ERR_STALE_CLIENTID.  Linux kernel will stop retrying.  Per-dd cost drops to actual codec/IO time.  Lowest-risk single-file change. |
| Find / disable the Linux kernel retry behaviour | unknown | Maybe a /proc/sys/sunrpc tunable, maybe not.  Worth trying as a configuration workaround before a code change.  E.g., `/sys/module/nfs/parameters/`, `sysctl sunrpc.tcp_*`. |
| Use NFSv3 DSes instead of NFSv4 DSes for the bench | medium | NFSv3 has no DESTROY_CLIENTID concept.  Bench docker-compose's DSes are reffsd configured to register NFSv3 unconditionally (per memory note); a layout pointing at NFSv3 ds-port would avoid the storm.  But variant-d's whole point is the FFv2 tightly-coupled path which is NFSv4-CHUNK on DSes -- swapping breaks the variant. |
| Pre-bench-cell EXCHANGE_ID handshake the kernel will accept | unknown | Hard to engineer without kernel cooperation. |

The DS-side fix is the right one.  It's a single-file change in
`lib/nfs4/server/session.c`, with a one-line standards interpretation
("DESTROY_CLIENTID is idempotent: NFS4_OK if already gone").  The
RFC says SHOULD return NFS4ERR_STALE_CLIENTID, but Linux kernel's
retry behaviour makes that effectively a denial-of-service against
the bench measurement.  An opt-in `tolerant` mode or a build-time
flag for the bench-only DSes would land in 30 minutes plus reviewer.

## Recommended next step

A small slice that:

1. Changes `nfs4_op_destroy_clientid` in `lib/nfs4/server/session.c`
   to return NFS4_OK when the clientid is not found.
2. Adds a unit test for the idempotent behaviour
   (`test_destroy_clientid_unknown_is_ok`).
3. Re-runs the bench: a single 4 KB dd from shadow should drop
   from 62 s to sub-second.  If yes, prereq #5/#6 unblocks.
4. Documents the RFC interpretation in
   `.claude/patterns/nfs4-protocol.md`.

This is a clean reviewer-gating slice (RFC interpretation change,
single op handler, single test).

## Deferred / NOT_NOW_BROWN_COW

- **Why does the kernel DESTROY_CLIENTID at all on every open?**
  Could be a session-recycling artefact of the kernel pNFS
  implementation per-LAYOUTGET.  Investigate after the latency
  fix unblocks the bench.
- **PS-uptime / stability**: the bench PS exited cleanly with no
  coredump after ~14 h of idle (separate finding).  Cause unknown.
- **Stale client 192.168.2.67 hammering :4098**: identify the
  host (ARP MAC `e2:d0:80:5f:48:88`), umount, prevent re-mount
  during bench runs.  Adds tcpdump noise but doesn't affect dd
  timing.
- **`ec_write_stripe_with_file` per-stripe ctx churn**:
  `lib/nfs4/ps/ps_proxy_ops.c:2641,2661` pass `ctx_in_out = NULL`
  and defeat the shared-ctx fast path documented at
  `lib/nfs4/ps/ec_pipeline.c:1193-1201`.  Will matter once
  per-dd cost is in the seconds range, not minutes.  Covered by
  existing memory note `project_t1b_badsession_ds_lease_reaper.md`.
