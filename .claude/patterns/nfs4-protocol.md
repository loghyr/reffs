<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# NFSv4 Protocol Patterns in reffs

## clientid4 partitioning

```
[ boot_seq (16 bits) | incarnation (16 bits) | slot (32 bits) ]
```

Do not compare clientid4 as an opaque integer without extracting fields.
Use accessor macros. Mixing up field positions produces silent correctness bugs
after server restart.

## EXCHANGE_ID decision tree

nfs4_client_alloc_or_find() owns the full decision tree:

1. New clientid (no existing record) → allocate, assign clientid4
2. Same principal, same verifier → return existing clientid4 (idempotent retry)
3. Same principal, new verifier → new incarnation, invalidate old state
4. Different principal, same nii_name → CLID_INUSE
5. Confirmed vs. unconfirmed state transitions

All five cases must be handled. Falling through to case 1 for cases 3-5 produces
state corruption under client restart.

## DESTROY_CLIENTID idempotent semantics

RFC 8881 §18.50.3 says the server SHOULD return
NFS4ERR_STALE_CLIENTID for an unknown clientid and MUST return
NFS4ERR_CLIENTID_BUSY when sessions / opens / locks / delegations
/ layouts exist.  Both responses make the Linux NFS client retry
DESTROY_CLIENTID 10 times at one-per-second cadence per peer
(linux-v6.7/fs/nfs/nfs4proc.c:9008,
`NFS4_MAX_LOOP_ON_RECOVER`).  In a pNFS fan-out (Flex Files v2,
k+m mirrors), the kernel runs that retry storm against each DS in
the layout, adding `10 * (k+m)` seconds per file lifecycle.

reffs's `nfs4_op_destroy_clientid` (`lib/nfs4/server/session.c`)
diverges from the RFC's letter on both branches to avoid that
penalty:

- **Unknown clientid → NFS4_OK** (idempotent).  No client record
  means the destroy has effectively already happened; no caller
  can observe a state transition.  Covers cold-mount stale-state
  cases.

- **Known clientid → unconditional `nfs4_client_expire`** (lenient
  teardown).  The handler does not check `nc_session_count`; it
  calls `nfs4_client_expire`, which tears down sessions, layouts,
  locks, opens, and delegations in one shot, then returns
  NFS4_OK.  Covers the Linux trunking-probe session-leak (issue
  #64): the kernel's `nfs4_pnfs_ds_connect` emits 2x
  CREATE_SESSION per DS but only 1x DESTROY_SESSION, leaving one
  session per DS still hashed at DESTROY_CLIENTID time.  Real-
  world Linux clients also pay a brief RCU grace period before
  `nfs4_session_free_rcu` decrements `nc_session_count`, which
  can keep the count above zero for the kernel's first one or
  two retries even after a clean DESTROY_SESSION.

The net wire outcome matches the spec: the client has no extant
references after the call returns.  The behavioural difference
is that reffs forgives common client teardown sloppiness rather
than charging the 10x retry tax.

The probe-session reaper (`lib/nfs4/server/lease_reaper.c`
`lease_reaper_sweep_probe_sessions`, fires every 1 s on
never-SEQUENCE'd sessions older than 2 s) complements this: it
keeps `nc_session_count` honest for *non*-DESTROY_CLIENTID code
paths that still need the strict count, and reaps trunking-
probe sessions whose owning client is still healthy.

Tests pinning the contract are in
`lib/nfs4/tests/nfs4_session.c`:
`test_destroy_clientid_unknown_is_ok`,
`test_destroy_clientid_with_session_expires_client`,
`test_session_alloc_seeds_timestamps`,
`test_reaper_sweeps_aged_probe_session`,
`test_reaper_leaves_used_session_alone`,
`test_reaper_leaves_young_probe_alone`.

## utf8string validation

All string fields off the wire (client owner, server owner, nii_name, path
components) must go through utf8string_validate() before use:

```c
if (utf8string_validate(&cs->clientowner) < 0)
    return NFS4ERR_INVAL;
```

Do not access .utf8string_val directly on unvalidated input.

## nfstime4 overflow

nseconds must be < 1e9; wire values >= 1e9 are invalid, reject them.
Use nfstime4_to_timespec() and timespec_to_nfstime4(). Do not open-code.

## Persistent state write pattern

```c
/* CORRECT: write temp, fsync, rename */
fd = open(path_tmp, O_WRONLY|O_CREAT|O_TRUNC, 0600);
write(fd, &state, sizeof(state));
fsync(fd);
close(fd);
rename(path_tmp, path_final);
```

Direct overwrite of the final path is not crash-safe.

## bitmap4 operations

Use bitmap4.h helpers. Open-coded bit manipulation fails when bitmaps span
multiple uint32 words (word index off-by-one is the common mistake).

```c
bitmap |= (1U << attr_id);      /* WRONG for attr_id >= 32 */
bitmap4_set(&bm, attr_id);      /* CORRECT */
```
