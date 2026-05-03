<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Experiment 5: Cross-PS Write-Read Coherence

Closes (or would close, when runnable) progress_report.md §3.4 /
§3.7: the PS-as-codec-layer claim that PSes are deployed
alongside DSes and a client connects to whichever is nearest.
This experiment measures the visibility latency when a write
goes through PS1 and the read goes through PS2.

## Status: BRING-UP UNBLOCKED 2026-05-02; visibility-latency test still TODO

The original PARTIAL stemmed from three blockers; all three are
now closed.  PS A and PS B come up cleanly against the bench MDS
on dreamer:

```
[bringup] MDS granted PS privilege 2 time(s) (expect 2)
[bringup] PS A: ready (no listener-dark, no registration failure)
[bringup] PS B: ready (no listener-dark, no registration failure)
[bringup] DONE.  PS A on 127.0.0.1:4098, PS B on 127.0.0.1:4099
```

What closed:

1. ~~mds_session_create_tls portmap path~~ -- closed by commit
   `6a06d4626772` (prior session).
2. ~~PROXY_REGISTRATION allowlist~~ -- closed by the bench
   scaffolding work (mds-tls.toml + ps-tls-{A,B}.toml + the
   per-PS cert minting in run-ps-bench-bringup.sh splicing
   fingerprints into [[allowed_ps]] blocks).  Two PSes register
   successfully; squat-guard works correctly between them.
3. ~~MOUNT3 export discovery via portmap~~ -- closed by commit
   `cbd135b544c4 ps_mount_client: explicit-port bypass for MOUNT3
   enumeration` (this slice).  ps_mount_fetch_exports now takes
   an explicit port; ps_discovery_run passes
   `pls->pls_upstream_port` and bypasses the host's rpcbind
   entirely.  Same shape as the bdde4f6539db pattern in ds_io.c
   and the mds_session explicit-port path.

What is still TODO for the experiment proper:

The bring-up surface is now reachable but a fresh blocker was
surfaced in the data path on first attempt (2026-05-02):

```
PS A trace:
  proxy open: listener=1 current_ino=1 claim=0 name=viz1
              (forwarded with PS creds)
  nfs4_op_open status=NFS4ERR_SERVERFAULT(10006) claim=0
              access=3 deny=0
  (561us elapsed)

ec_demo client: write failed: -121
```

The bench MDS log shows no incoming OPEN for the file -- the
PS rejects the request internally before forwarding it
upstream.  `ps_proxy_forward_open` (lib/nfs4/server/file.c
around line 561) is the call that produced the failing fret.
Likely the function returned an errno with no clean mapping
in `errno_to_nfs4` (default -> NFS4ERR_SERVERFAULT).  The
561us elapsed is consistent with "failed before any RPC went
out."

Cause not yet root-caused.  Suspect classes (in priority order):
1. PS-MDS session's per-PS export discovery missed the path,
   so PUTROOTFH+LOOKUP under PS creds returns an error that
   ps_proxy_forward_open relays as a non-mapped errno.
2. ps_proxy_forward_open's internal compound construction has
   a bug for the OPEN case specifically (CHUNK ops worked in
   the PS smoke).
3. PS creds forwarding has a dropped step (cred propagation
   into the upstream session).

Visibility-latency measurement is therefore still pending --
the harness exists (`/tmp/exp5_visibility.sh`) and runs
correctly against a working PS, but every iteration currently
returns WRITE_FAIL = -121.  Investigation of the SERVERFAULT
is the next slice.

NOT in this slice's commit; surfaced cleanly on the deck's
slide 17/18 honest-scope ("kernel-mount-through-PS still
pending; protocol path not yet end-to-end").

## Setup attempted

- adept (Intel N100, Fedora 43) running:
  - bench MDS + 10 DSes in docker (existing benchmark
    docker-compose stack).
  - PS1 (`/tmp/ps1.toml`): standalone reffsd in docker with
    `--network=host`, `[[proxy_mds]] port=4098, mds_port=2049,
    address=192.168.2.129`.
  - PS2: same as PS1 but listener port 4099, distinct probe
    port + state dirs.
- 5 second sleep; check `docker logs reffs-ps1 reffs-ps2`.

## Blocker chain (in order of discovery)

### 1. mds_session_create_tls portmap path (FIXED)

**Symptom**: PS log shows
`mds_session_create_tls failed: Connection refused -- listener
stays dark`.

**Root cause**: The no-cert fallback at port==2049 sent the
upstream connect through `mds_session_create(host)` with bare
host (no port).  After commit `a38cb6a0f7b7`,
`mds_session_clnt_open` routes bare-host through portmap.  The
host-side rpcbind on adept doesn't know about NFS_V4 (the
service is in a docker container with its own rpcbind), so the
portmap query returns nothing and the connect fails.

**Fix**: commit `6a06d4626772` -- only skip host:port encoding
when port == 0 (caller didn't pick).  For any explicit port
including 2049, build "host:port" and let
`mds_session_clnt_open` use direct connect.

### 2. PROXY_REGISTRATION allowlist (NOT FIXED)

**Symptom**: PS log shows
`proxy_mds[1]: PROXY_REGISTRATION failed: Operation not
permitted -- PS will operate without registered-PS privilege`.

**Root cause**: The MDS's `[[allowed_ps]]` allowlist (per
`proxy-server.md` "Guard against registration squatting") has
no entry for the PS instance.  Without registration, the PS
won't get the `nc_is_registered_ps` privilege bit, which means
it can't run the namespace-discovery LOOKUP/GETFH operations
that bypass per-export filtering.

**Fix needed**: extend `deploy/benchmark/mds.toml` with
`[[allowed_ps]]` blocks for each PS identity.  For the
bench/lab topology this is plain AUTH_SYS uid-allowlist (mTLS
in `proxy-server.md` is the production form).  Implementation
of the AUTH_SYS allowlist branch in the MDS may also be
incomplete -- needs source review.

### 3. MOUNT3 export discovery via portmap (NOT FIXED)

**Symptom**: PS log shows
`ps[1]: MOUNT3 export enumeration against 192.168.2.129
failed: -111`.

**Root cause**: PS discovery uses NFSv3 MOUNT to enumerate
exports on the upstream MDS.  `lib/nfs4/ps/ps_mount_client.c`
does `clnt_create(host, MOUNT_PROGRAM, MOUNT_V3, "tcp")` --
portmap-driven.  Same issue as blocker 1: host-side rpcbind
on 192.168.2.129 has no MOUNT_V3 service registered (the
bench MDS in docker has its own rpcbind that does, but
visible only inside the container's network).

**Fix needed**: `ps_mount_client.c` needs the same
explicit-port bypass as `mds_session_clnt_open` and
`ds_io.c`.  When the [[proxy_mds]] block sets `mds_port`,
discover MOUNT_V3 directly at `<mds_address>:<mds_port>` (or
add a separate `mds_mount_port` knob) instead of going through
portmap.

## Implications -- the experiment can be unblocked with
~half a day of focused work

The blocker chain is concrete:

1. ~~mds_session_create_tls port path~~ -- fixed.
2. Add `[[allowed_ps]]` to `deploy/benchmark/mds.toml` (or
   verify the AUTH_SYS allowlist code path; if missing, add
   it -- ~50 LOC in `lib/nfs4/server/`).
3. Patch `ps_mount_client.c` for explicit-port MOUNT3
   (similar to `bdde4f6539db`'s ds_io fix; ~30 LOC).
4. Re-run the discovery + visibility test.

This is the same shape of cross-host port-bypass work that
experiment 6 surfaced.  Each component (MDS-DS connect, v1
NFSv3 client, v2 NFSv4.2 client, MDS upstream connect) had its
own portmap path that needed fixing.  The PS MOUNT3 discovery
path is the next one.

## What this experiment would measure once unblocked

The spec's hypothesis stands: a read at PS2 issued after a
successful write at PS1 sees the new content within one MDS
round-trip (≈ 1 ms p99 on loopback).  The harness exists in
shape (two `ec_demo` against two PS listeners on 4098 and
4099); the missing piece is the PS-MDS handshake completing
cleanly so PS1 and PS2 actually have a working namespace.

## Followup tasks (filed for whoever picks this up)

1. **`deploy/benchmark/mds.toml`** -- add `[[allowed_ps]]` for
   the test PS identities.  Verify AUTH_SYS allowlist code
   path actually exists; if not, implement it.
2. **`lib/nfs4/ps/ps_mount_client.c`** -- explicit-port MOUNT3
   bypass when the proxy_mds config sets the upstream port.
3. **`deploy/benchmark/`** -- add a `docker-compose-ps.yml`
   overlay or extend the existing compose with two PS
   services so the bring-up is reproducible.
4. **Run the visibility test** with the fixed topology;
   produce the spec's measurement (p99 visibility latency,
   zero mixed reads).

## Caveats

- The experiment is measurement-friendly once the topology
  works -- the test harness is straightforward.  The blocker
  is plumbing, not protocol.
- The PROXY_REGISTRATION + MOUNT3 issues both stem from the
  same root cause as experiments 1 / 6 / 12 portmap fixes:
  reffs's NFSv4 client paths default to portmap when no
  explicit port is given, but reffs's NFSv4 server doesn't
  reliably register with the host's rpcbind in
  containerised deployments.  A blanket "always-prefer-
  explicit-port-when-set" sweep across `lib/nfs4/client/` and
  `lib/nfs4/ps/` would close this class of issue once.
