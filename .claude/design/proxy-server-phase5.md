<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Proxy-Server Phase 5: Short-Circuit for Co-Resident DS

## Goal

When the PS is fanning out a per-stripe CHUNK_WRITE / CHUNK_READ (or
v3 WRITE / READ) to a mirror whose `ec_device.ed_host` resolves to
one of the PS's *own* bound addresses, bypass the RPC layer and call
the local DS sb's `db_read` / `db_write` directly.  This shaves a full
loopback NFS RPC off every per-stripe I/O on a co-resident DS+PS
deployment — the same node Tier-2/Tier-3 benchmark setups run on.

## Design rules

1. **Per-mirror flag**, not per-pipeline: a layout can mix local +
   remote mirrors (Tier-2 deploys 1 DS per host, so a 2-mirror layout
   on the same host has both local; a 6-DS Tier-3 may have 1 local
   + 5 remote).  The short-circuit decision is per `ctx_devs[i]`.
2. **No new credential model**: the short path uses the forwarded
   client cred exactly as the RPC path would.  The local DS sb's
   per-file synthetic-uid/gid check must run.  If the check rejects
   on the RPC path, it must reject on the short path.
3. **No new stateid model**: once trust-stateid lands (NOT_NOW_BROWN_COW
   on the PS), the same trust-table lookup that the DS does inside its
   CHUNK_WRITE handler must run on the short path.  Until then,
   anonymous stateid works the same on both paths.
4. **No new wire format**: probe surface stays additive (a single
   `pls_shortcircuit_total` counter so benchmarks can prove the
   short path actually fired).
5. **No new dstore vtable**: the local DS sb is the existing
   `dstore_ops_local` vtable; we just plumb its `data_block` access
   through a small helper.

## Address detection

The PS reads its own bound addresses from `pls_upstream` + whatever
the listener accepts on.  At PS-listener boot (after the
`bind`+`listen` pair) we capture the set of local `sockaddr_in*`
addresses in a small array on `pls`:

```c
struct ps_local_addr {
    struct sockaddr_storage la_ss;
    socklen_t                la_len;
};
#define PS_MAX_LOCAL_ADDRS 8
struct ps_listener_state {
    /* ... existing fields ... */
    struct ps_local_addr pls_local_addrs[PS_MAX_LOCAL_ADDRS];
    uint32_t             pls_nlocal_addrs;
};
```

Population happens once at listener start: `getifaddrs(3)` enumerates
the host's interfaces, filtered to the family the listener bound on
(AF_INET + AF_INET6).  Loopback (127.0.0.1 / ::1) is always included.

Matching is by-numeric-address only: `getaddrinfo(ed_host, AI_NUMERICHOST)`
+ memcmp against `pls_local_addrs[].la_ss.sin_addr`.  Hostnames that
resolve to a non-local IP take the remote path (the RPC layer will
do the same resolution and connect there).

Tests:

| Test | Intent |
|------|--------|
| `test_local_addr_seed_from_getifaddrs` | After ps_listener init, pls_local_addrs contains at least 127.0.0.1 |
| `test_local_addr_match_loopback` | "127.0.0.1" matches; "127.0.0.2" does not |
| `test_local_addr_match_external_ipv4` | The host's real IPv4 (read via getifaddrs in the test) matches |
| `test_local_addr_match_ipv6_loopback` | "::1" matches when AF_INET6 listener is up |
| `test_local_addr_no_match_remote` | "203.0.113.5" (TEST-NET-3) does not match |
| `test_local_addr_full_table` | Filling pls_local_addrs to PS_MAX_LOCAL_ADDRS truncates cleanly, no overflow |

## Short-circuit dispatch

Two call sites in `lib/nfs4/ps/ec_pipeline.c`:

1. `ec_chunk_write` / `ec_chunk_read` (v2/CHUNK path)
2. `ds_write` / `ds_read` via `ds_io.c` (v1/NFSv3 path)

Per-mirror, after deviceinfo resolution: set `em->em_local = true`
when `ec_device.ed_host` matches `pls_local_addrs`.  In both call
sites, check `em->em_local`:

- If true: call `ps_shortcircuit_write` / `ps_shortcircuit_read`
  (new helpers in `lib/nfs4/ps/ps_shortcircuit.c`).  These resolve
  the local DS sb (via `super_block_find` on the ds_id encoded in the
  layout), check the per-file synthetic uid/gid against the forwarded
  cred, and call `sb->sb_ops->db_read` / `db_write` against the
  `data_block` for the file's inode.
- If false: existing RPC path.

Counter: `_Atomic uint64_t pls_shortcircuit_total` bumped on every
short-path call, surfaced in `probe_ps_stats1`.

Tests (`lib/nfs4/ps/tests/ps_shortcircuit_test.c`, new file):

| Test | Intent |
|------|--------|
| `test_shortcircuit_dispatch_local` | em_local=true → write goes through db_write, no ds_chunk_write call |
| `test_shortcircuit_dispatch_remote` | em_local=false → write goes through ds_chunk_write, db_write not called |
| `test_shortcircuit_counter_increments` | After N local writes, pls_shortcircuit_total == N |
| `test_shortcircuit_read_roundtrip` | Local write followed by local read returns the same bytes (no RPC in between) |

## Credential enforcement

The local DS sb stores its files with synthetic uid/gid (fenced range,
default 1024-2048).  On the RPC path, this is enforced by the DS's
NFSv4 access check before `db_write` is called.  The short path must
replicate the check, otherwise a forwarded cred that the RPC path
would reject (e.g., wrong gid for a fenced file) silently succeeds.

Helper: `ps_shortcircuit_check_creds(sb, ino, forwarded_uid,
forwarded_gid)`.  Reads the local file's stored synthetic uid/gid
and applies the same accept rule the RPC path's `nfs4_check_access`
applies (read=open-for-read, write=open-for-write).

Tests:

| Test | Intent |
|------|--------|
| `test_shortcircuit_reject_wrong_uid` | Forwarded uid that doesn't match synthetic uid → -EACCES, no db_write call |
| `test_shortcircuit_accept_matching_uid` | Forwarded uid matches → write proceeds |
| `test_shortcircuit_root_squash` | Forwarded uid=0 with root_squash → squashed to nobody, then checked |

## Trust-stateid coupling (gated)

Test 5 from `proxy-server.md` says "Once tight-coupling trust tables
land, short-circuit also rejects a forwarded layout stateid absent
from the local DS's trust table."  Trust-stateid is fully designed
(`.claude/design/trust-stateid.md`) but not yet implemented on the
PS side.

Slice plan: implement the trust-table hook NOW in
`ps_shortcircuit_check_stateid`, gated on whether the local DS sb
has a trust table populated.  Until trust-stateid Phase 1 ships
on the PS, the table is empty and the check accepts (matching
today's behavior).  When Phase 1 ships, the check becomes
load-bearing.

Test:

| Test | Intent |
|------|--------|
| `test_shortcircuit_rejects_unknown_stateid_when_trust_armed` | With trust table populated, forwarded layout stateid absent from table → -EBADSTATE.  Skipped (CK_PASS short-circuit) when trust table is empty. |

## Partial layout (mix local + remote)

When some mirrors are local and others are remote, each mirror takes
its own path independently.  Reconstruction (RS or Mojette decode)
proceeds from whatever data lands in `ctx_buf[]` — the encoder
doesn't care whether a given shard came from a short-circuit memcpy
or an RPC.

Test:

| Test | Intent |
|------|--------|
| `test_shortcircuit_partial_2_mirrors` | 2-mirror layout, mirror 0 local + mirror 1 remote; both write paths fire; verify pls_shortcircuit_total == 1 and ds_chunk_write call_count == 1 |
| `test_shortcircuit_partial_decode_after_local_failure` | If the local write fails (db_write returns -EIO), the per-mirror retry kicks in on the remote-only mirrors; CSM still satisfied |

## Probe surface

Add to `probe_ps_stats1` (wire-additive, internal-only):

```
unsigned hyper ps_shortcircuit_total;
```

CLI shows it as `SC` column next to existing per-listener stats.

## Slice plan

| # | Title | Touches | LOC est |
|---|-------|---------|---------|
| 5.1 | local-addr table + match primitive | ps_state.h, ps_state.c, new ps_local_addr.c, 6 tests | ~250 |
| 5.2 | em_local plumbing + dispatch hook (both paths, no creds yet) | ec_pipeline.c, ec_client.h, new ps_shortcircuit.c, 4 tests | ~300 |
| 5.3 | cred enforcement on short path | ps_shortcircuit.c, 3 tests | ~150 |
| 5.4 | trust-stateid hook (gated) | ps_shortcircuit.c, 1 test | ~80 |
| 5.5 | partial layout (mix) + counter probe surface | ec_pipeline.c, probe1_xdr.x, probe1_server.c, 2 tests | ~120 |

Total: ~900 LOC, 16 new tests.

Each slice is independently testable, reviewer-gated (cross-layer
addition + new vtable plumbing — meets the reviewer trigger).
Cross-verify on dreamer after slices 5.2 and 5.5 (the two with the
most surface area).

## Deferred (NOT_NOW_BROWN_COW)

- IPv6 zone-id matching (`fe80::1%en0`) — getifaddrs gives us
  zone-id-stripped addresses; rare in practice.
- DS-side trust table sync from the local DS sb's MDS view — when
  trust-stateid Phase 1 ships, this hook becomes load-bearing
  automatically.
- `getifaddrs` refresh on interface up/down — capture-once is fine
  for the BAT demo; production may want a netlink listener.

## Out of scope

- The PS's own RPC listener address discovery: already done by
  `getsockname` at bind time; this slice adds *peer* address
  enumeration (the set of addresses the *kernel* would route to
  the local host).
- Cross-host setups where the DS is on a different physical node:
  these never match `pls_local_addrs`, the existing RPC path runs.
