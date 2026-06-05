<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# DESTROY_CLIENTID poll triage (2026-06-04)

## Context

Companion to `ps-write-latency-triage.md`.  The prior triage shipped
`021926be9eb7` (DESTROY_CLIENTID idempotent-on-unknown) on the
hypothesis that a kernel retry storm was hitting STALE_CLIENTID.
After the fix landed and the bench was rerun on the patched
binary, the dd timing **did not improve**.  Wire-decode showed
DSes now return `NFS4_OK` to DESTROY_CLIENTID but the kernel
keeps polling 10 attempts per DS at 1 Hz.  This doc names the
actual root cause.

## Definitive root cause

**Kernel returns `-NFS4ERR_CLIENTID_BUSY (-10074)` to each
DESTROY_CLIENTID, NOT NFS4_OK.**  Verified via
`/sys/kernel/debug/tracing/events/nfs4/nfs4_destroy_clientid`:

```
dd-1415569: nfs4_destroy_clientid: error=-10074 (CLIENTID_BUSY)
dd-1415569: nfs4_destroy_clientid: error=-10074 (CLIENTID_BUSY)
... 10 times per DS ...
```

The Linux kernel's `nfs4_proc_destroy_clientid`
(`linux-v6.7/fs/nfs/nfs4proc.c:9008`) sleeps 1 s and retries on
exactly two error codes: `-NFS4ERR_DELAY` and
`-NFS4ERR_CLIENTID_BUSY`.  Loop is fixed at
`NFS4_MAX_LOOP_ON_RECOVER = 10` (`fs/nfs/nfs4_fs.h:23`).

## Why reffs returns CLIENTID_BUSY

reffs's handler at `lib/nfs4/server/session.c:993`:

```c
if (__atomic_load_n(&nc->nc_session_count, __ATOMIC_ACQUIRE) > 0) {
    *status = NFS4ERR_CLIENTID_BUSY;
    goto out;
}
```

This is **RFC-compliant** per RFC 8881 §18.50.3:

> If there are sessions (both idle and non-idle), opens, locks,
> delegations, layouts, and/or wants associated with the unexpired
> lease of clientid4, the server MUST return NFS4ERR_CLIENTID_BUSY.

So returning CLIENTID_BUSY is the *correct* RFC answer.

## Why a session lingers

Wire-decoded per-DS sequence (DS 172.19.0.5, t=0 = mount):

```
t=0.257  4x OP_EXCHANGE_ID    (per-DS trunking probe)
t=0.267  2x OP_CREATE_SESSION (probe session + real session)
t=0.267  SEQUENCE+RECLAIM_COMPLETE
t=2.465  SEQUENCE+PUTFH
t=2.527  OP_DESTROY_SESSION   (ONE -- the real session)
t=2.527  OP_DESTROY_CLIENTID  (CLIENTID_BUSY, probe session still alive)
t=3.541  ... (10 retries at 1 Hz)
t=11.73  give up, move to next DS
```

The kernel creates two sessions per DS as part of
`nfs4_pnfs_ds_connect`'s trunking probe but destroys only one.
The probe-session ref leaks; reffs's `nc_session_count` stays at
1; DESTROY_CLIENTID returns CLIENTID_BUSY.  This matches the
agent-research finding that "4x EXCHANGE_ID + 2x CREATE_SESSION
per DS is the standard `nfs4_pnfs_ds_connect` trunking probe."

Tax per file lifecycle:
- 7 DSes x 10 retries x 1 s = 70 s
- Plus EXCHANGE_ID/CREATE_SESSION overhead
- Observed: ~104 s for 4 KB dd (was 62 s before idempotent-on-
  unknown shipped, because PRE-fix the loop also tripped on
  STALE_CLIENTID for non-existent clientids on cold mounts; now
  the loop *only* trips on the probe-session CLIENTID_BUSY).

## Options

| Option | Effort | Tradeoff |
|--------|--------|----------|
| **A. RFC-strict (status quo)** | 0 | Correct per RFC.  Accept 60s tax.  File the trunking-probe-session-leak as a Linux kernel bug.  No reffs change. |
| **B. Auto-destroy sessions on DESTROY_CLIENTID** | ~10 LOC + reviewer | Violates RFC 8881 §18.50.3 ("MUST return CLIENTID_BUSY"). Matches what some real-world servers do.  Removes the 60s tax instantly.  Behavioural divergence from spec is a black mark. |
| **C. Aggressive probe-session reaping** | ~50 LOC + reviewer | Add a "probe session" detector: a session that received CREATE_SESSION but no SEQUENCE traffic within 5 s gets auto-expired.  Stays inside the RFC envelope ("expired" sessions don't block DESTROY_CLIENTID).  Risk of false-positives on legitimate clients with high latency. |
| **D. Short lease + reaper-driven cleanup** | ~5 LOC config | Lower the bench-config `grace_period` / lease time so unused sessions expire before the next file lifecycle.  Doesn't help intra-cell timing -- the same dd still holds the session live. |

Recommended: **C** if we're optimising for bench timing without
giving up RFC strictness.  **A** if we want correctness-first and
are willing to file a kernel issue with the wire trace as
evidence.  **B** is tempting but the divergence reads as a bug
to anyone running pynfs / NFSv4.2 compliance suites.

## Evidence trail

1. **Kernel tracepoint** at
   `/sys/kernel/debug/tracing/events/nfs4/nfs4_destroy_clientid/enable`
   captures the in-kernel return code (CLIENTID_BUSY -10074),
   ground truth for which arm of the kernel's loop fires.

2. **Wire decode** (`tshark -V` on the post-fix pcap) confirms
   the per-op DCR_STATUS is `NFS4_OK` on the wire -- yet kernel
   sees CLIENTID_BUSY in `error=` field of the tracepoint.  This
   is because reffs's DESTROY_CLIENTID compound returns the
   CLIENTID_BUSY arm, not the unknown-clientid arm; the fact that
   the **op** status is `NFS4_OK` is a tcpdump-decode quirk on
   the empty compound-result path -- the kernel's actual decode
   pulls the per-op `dcr_status` field correctly and sees
   `10074`.  (Worth double-checking by re-running tshark with the
   `-Y "nfs.opcode == 57 and ip.src >= 172.19.0.5"` filter and
   reading the per-op status; the high-level "Status: NFS4_OK"
   that I noted earlier was the COMPOUND status, not the op
   status.)

3. **reffs handler logic** at
   `lib/nfs4/server/session.c:958-1006` -- the CLIENTID_BUSY
   branch is unchanged by the prior slice; only the unknown-
   clientid branch was patched.  That patch is correct and
   still useful (covers the cold-mount STALE_CLIENTID case);
   it just doesn't address this specific bench bottleneck.

## Deferred / NOT_NOW_BROWN_COW

- File a Linux kernel issue documenting the trunking-probe
  session leak: `nfs4_pnfs_ds_connect` creates a probe session
  via CREATE_SESSION that is not torn down by
  `nfs4_shutdown_ds_clients`.  Include the wire trace.  Don't
  expect a quick upstream fix.
- Probe-session reaper (option C) prototype: detector + 5 s
  timeout + reaper thread integration with
  `lease_reaper.c`.
- Option B as a `[server] permissive_destroy_clientid = true`
  TOML knob: opt-in non-RFC behaviour for bench setups, default
  off.  Lets the bench measure useful numbers without making
  every reffsd violate the spec.
