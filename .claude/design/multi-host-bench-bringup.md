<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Multi-Host Bench Bringup (PS-encoder 4-variant, Option 1)

## Why this exists

`ps-encoder-bench-4variant-realnet.md` lists this as **prereq #3**
(2-3 days): the orchestration that gets the 3-host topology
standing so prereq #4 (the harness) and prereqs #5-6 (smoke +
full sweep) have something to drive.

## Scope clarification (revised after first-smoke 2026-06-03)

The initial design imagined this single bringup standing up the
full 4-variant topology in one shot.  First smoke surfaced a
genuine conflict: variants a/b/c need an MDS that speaks plain
TCP, but the proxy-server draft (and the implementation per
`proxy_registration.c`) require **TLS or RPCSEC_GSS** on the
PS-to-MDS session.  reffsd's `tls = true` makes the MDS listener
TLS-required (the AUTH_TLS STARTTLS path is gated on the client
initiating the upgrade, which ec_demo and the kernel client do
not).  Three viable resolutions:

| Option | Result |
|--------|--------|
| (a) Drop TLS on MDS | Variants a/b/c work; variant d **does not** -- `PROXY_REGISTRATION` rejects AUTH_SYS sessions |
| (b) Keep TLS on MDS | Variant d works; variants a/b/c blocked by handshake failure |
| (c) Code: dual-mode TLS listener | Both work.  Days of work in `lib/io/`. |
| (d) Two MDS instances on different ports | Both work.  Complex backend sharing problem. |

For this slice we land **(b)**: the realnet bringup ships as
**variant-d-only** today.  The plain-MDS bringup for variants
a/b/c is queued as a follow-up slice that either picks (c) or
runs two MDS containers.  The headline WG question
"client EC vs server EC" maps to variants c and d; variant d is
preserved, and the variant-c surrogate (`ec_demo` against a plain
MDS) can be measured on the existing single-host setup until the
plain-MDS realnet bringup lands.

The existing `run-ps-bench-bringup.sh` is single-host docker:
it brings up the docker-compose `mds + 10 DSes + N PSes` stack
on one Linux host and orchestrates everything via the
`--network=host` PS escape hatch.  That topology hides the
PS-to-MDS hop on loopback and cannot answer "where does EC
encoding belong" honestly (see the 2026-06-02 single-host bench
limitations documented in `ps-encoder-bench.md` Take 2).

This slice brings up **three real hosts** with real LAN
round-trips between every pair.

## Topology

Per user direction 2026-06-03, fixed host assignment:

| Role            | Host    | Notes                                       |
|-----------------|---------|---------------------------------------------|
| Client          | dreamer | M4 MacBook, Fedora 43 VM; kernel + ec_demo  |
| PS              | adept   | N100 AVX2; one standalone reffsd w/ `[[proxy_mds]]` |
| DS + MDS        | shadow  | Bench smoke host; docker-compose stack      |

shadow runs the docker-compose stack (1 MDS + 10 DSes).  Each
DS container publishes 2049 on a distinct shadow host port:

| Container | Container port | shadow host port |
|-----------|----------------|------------------|
| reffs-mds | 2049           | 2049             |
| reffs-ds0 | 2049           | 22049            |
| reffs-ds1 | 2049           | 22050            |
| reffs-ds2 | 2049           | 22051            |
| ...       | ...            | ...              |
| reffs-ds9 | 2049           | 22058            |

This is the key change from `docker-compose.yml`: the existing
file only publishes the MDS port (the DSes are only reachable
on the internal `bench` bridge network, by `reffs-dsN` service
name).  The realnet variant publishes every DS port so the
**client and PS** can reach each DS over the LAN.

Why per-DS port publishing and not macvlan?

- The `[[data_server]]` config has a `port` field
  (`lib/include/reffs/settings.h:140`) and the layout XDR
  carries (address, port) tuples per RFC 8881 § 19.  The
  NFSv3 dstore path already honours `ds_port` via
  `clnttcp_create` (`lib/nfs4/dstore/dstore.c:204,294`).
- **The NFSv4 dstore path did not, prior to this slice.**
  `lib/nfs4/dstore/ds_session.c` was calling
  `mds_session_create(ms, ds->ds_address)` with the bare
  address; libtirpc fell through to portmap and the MDS-to-DS
  session failed with `RPC: Program not registered`.  First
  smoke caught this; the fix lives in this slice (snprintf a
  `host:port` suffix when `ds_port > 0`, mirroring the v3
  path's behaviour).  10 LOC, single file, no XDR/RCU/on-disk
  format -- inline-review eligible per CLAUDE.md gating.
- macvlan needs lab-subnet DHCP allocation and per-host
  bridge configuration; port-publish needs one yaml edit.
- The shared-fsync caveat (Option 1 con per the 4-variant
  design) is unchanged either way.  Option 2 (N+3 distinct
  VMs) is the follow-on that closes it.

## Wire-up

```
+-----------+        NFSv4.2 LAN          +-----------+
|  dreamer  | --------------------------> |   adept   |
|  client   |   variant d (PS-encoded)    |    PS     |
+-----------+                              +-----+-----+
      |                                         |
      |  variants a/b/c (direct to MDS)         |  PS -> MDS, PS -> DS
      |                                         v
      +-----> shadow:2049 (MDS)         shadow:2049 (MDS)
              shadow:22049 (DS0)        shadow:22049 (DS0)
              shadow:22050 (DS1)        shadow:22050 (DS1)
              ... shadow:22058 (DS9)    ... shadow:22058 (DS9)
```

Every (client, PS, MDS, DS) hop is a real network round-trip.
No loopback shortcuts.

## Config artefacts

### `deploy/benchmark/docker-compose-realnet.yml` (NEW)

Derived from `docker-compose.yml`.  Two changes:

1. Every `dsN` service adds a `ports:` block:
   ```yaml
   ports:
     - "22049:2049"   # ds0 ; +1 per index for ds1..ds9
     - "22049:2049/udp"
   ```
2. The MDS no longer depends on docker-bridge DNS for DS
   discovery -- it reads `mds-realnet.toml` which uses
   shadow's LAN IP + per-DS ports.  We still mount the toml
   via a volume; the only difference is which toml we mount.

### `deploy/benchmark/mds-realnet.toml` (NEW)

Identical to `mds.toml` except every `[[data_server]]` entry
uses `address = "<SHADOW_LAN_IP>"` and `port = 22049 + (id-1)`.

The `SHADOW_LAN_IP` placeholder is substituted at bringup
time (the orchestrator script discovers shadow's LAN IP via
`ip -4 addr` after SSH).  Carries `default_coding = "rs:4+2"`
from prereq #2.

### `deploy/benchmark/ps-realnet.toml` (NEW)

Derived from `ps-tls.toml`.  Two changes:

1. `[[proxy_mds]] address` placeholder substitutes shadow's
   LAN IP (not 127.0.0.1).
2. `tls_ca` placeholder substitutes the CA path on adept (not
   shadow's `/tmp/reffs_ps_tls`).

Otherwise identical: standalone role, mTLS-allowed PS cert,
NFS port 4098.

## Orchestration: `deploy/benchmark/run-realnet-bringup.sh`

Reuses the proven patterns from `run-ps-bench-bringup.sh`:

1. Mint mini-CA + one PS cert via
   `deploy/sanity/setup-mini-ca.sh`.  Stamp the PS fingerprint
   into a working `mds-realnet.toml` copy.
2. **SSH to shadow**:
   - `rsync` the working dir (TLS materials + tomls + the
     realnet docker-compose) to shadow under `/tmp/reffs_realnet/`.
     Per `feedback_lab_use_worktrees_not_rsync.md`, do NOT use
     `~/reffs-main` (that's the nightly's worktree); use a
     temp dir.
   - `git worktree add /tmp/reffs_realnet_repo` from
     `/home/loghyr/reffs-main` (the canonical reffs clone) at
     `origin/main`, so the docker-compose builder has a clean
     source tree.
   - Run the docker-compose builder + bring up MDS + DSes
     with the per-DS port-publishing yaml.
   - Wait for `reffsd ready` in MDS logs (same readiness signal
     as the single-host bringup).
3. **SSH to adept**:
   - `rsync` the same working dir to adept under
     `/tmp/reffs_realnet/`.
   - `git worktree add /tmp/reffs_realnet_repo` and build
     reffsd (it's the same git clone pattern -- adept has the
     same `~/reffs-main` convention).  Actually skip the
     full build; if adept already has a fresh reffsd binary
     under `/home/loghyr/reffs-main/build/src/reffsd`, reuse it.
     If not, build it.
   - Launch `reffsd --config=/tmp/reffs_realnet/ps-realnet.toml`
     with `ASAN_OPTIONS=detect_leaks=0:halt_on_error=0` (the
     same env the soak script uses).
4. **Verify**:
   - Poll shadow MDS logs for one `PROXY_REGISTRATION:
     client granted PS privilege` line (same check as the
     single-host bringup).
   - Poll adept PS logs for `reffsd ready` and no
     `PROXY_REGISTRATION failed`.
5. **Print the topology**:
   ```
   variant a/b/c clients point at SHADOW_LAN_IP:2049
   variant d clients point at ADEPT_LAN_IP:4098
   ```

Steps 2 and 3 run sequentially.  Step 3 depends on step 2
(the PS can't register until the MDS is up); parallelising
buys nothing because step 2 is the long pole (docker-compose
builder + MDS readiness).

### Tear-down

Companion `run-realnet-teardown.sh` (separate file, keep
the bringup focused):

1. SSH to adept: `pkill -f 'reffsd.*ps-realnet'`, clean
   `/tmp/ps_0_{data,state}`.
2. SSH to shadow: `docker compose -f
   /tmp/reffs_realnet/docker-compose-realnet.yml down -v`,
   `git worktree remove /tmp/reffs_realnet_repo`,
   `rm -rf /tmp/reffs_realnet`.

NOT_NOW_BROWN_COW: bringup teardown is idempotent -- the
script can be re-run safely.  The teardown is for the
explicit-cleanup-after-run case.

## Why SSH (not ansible / containers-everywhere)

- Ansible is more state than this slice needs.  Five files
  on each remote host, two `docker compose up` calls -- shell
  is the right grain.
- Putting the orchestrator itself in a container adds a
  layer (the orchestrator needs to SSH out from its
  container).  Run it on the local laptop instead.
- This script ssh's to lab hosts that are already in the
  user's `~/.ssh/config`; no new credential setup.

## Test plan

### Existing tests affected: NONE

This slice is bench infrastructure.  No code in `lib/` moves,
no XDR changes, no probe ops.  The only `.x` / `.h` / `.c`
files referenced are the existing ones that already encode
(address, port) tuples.

### Smoke tests (per prereq #5 of the 4-variant design)

- SSH to dreamer, run `ec_demo --layout v2 --codec rs --k 4
  --m 2 --mds SHADOW_LAN_IP --size 1MB` and verify the
  written file reads back byte-identical.  This is the
  variant-c surrogate.
- SSH to dreamer, `mount -t nfs4.2 -o sec=sys
  ADEPT_LAN_IP:/ /mnt/realnet`, write a 1MB file via
  `dd`, read back with `cmp`.  This is variant d.

Both run as separate manual checks after bringup completes.
Wiring them into the bringup script would make it slow and
opinionated; the harness (prereq #4) will own the actual
data-collection.

### Failure modes covered

| Failure                              | Detection                          |
|--------------------------------------|------------------------------------|
| shadow's LAN IP not discoverable    | `ip -4 addr` returns empty -> fail-fast |
| MDS never ready                      | 120s `reffsd ready` poll timeout   |
| PS never registers                   | MDS grant count == 0 (authoritative) |
| PS upstream session never up         | grep PS log for `listener stays dark` |
| docker-compose port conflict on shadow | `docker compose up` exits non-zero |
| SSH connect failure                  | `ssh ... exit 0` smoke before any work |
| Firewalld blocks DS publish ports    | `firewall-cmd --add-port=22049-22058/{tcp,udp}` step (transient) |
| Stale PROXY_REGISTRATION retry       | Squat-DELAY on second registration is treated as benign as long as MDS recorded >=1 grant -- the PS occasionally re-registers under contention with a fresh `registration_id` and gets squat-DELAY'd; the first grant is the topology-up signal |

## Deferred / NOT_NOW_BROWN_COW

- **Plain-MDS realnet bringup for variants a/b/c.**  This slice
  is variant-d-only by design (see scope clarification above).
  Follow-up slice picks one of: (i) dual-mode TLS listener
  (`lib/io/` code change, days of work), or (ii) two MDS
  containers on different ports sharing a backend mount, or
  (iii) just re-use the existing single-host bench for a/b/c
  measurement and only do realnet for variant d.  The
  4-variant deck slide (slide 22 on `ietf126.md`) can land
  with variant d on realnet topology and variants a/b/c on
  single-host until the follow-up resolves.
- **Why we did not investigate dual-mode TLS in this slice.**
  The reffs RPC layer registers AUTH_TLS as a flavor
  (`lib/rpc/rpc.c:1276`) but the actual STARTTLS upgrade is
  client-initiated; ec_demo and the Linux kernel client do
  not currently issue the AUTH_TLS NULL probe before their
  first NFSv4 op.  Making the listener accept plain TCP and
  upgrade on STARTTLS-request, while rejecting non-STARTTLS
  plain TCP traffic from the PS specifically, would touch
  authentication negotiation in a way that warrants its own
  design slice.
- **Option 2 topology** (N+3 distinct hosts).  Same bringup
  script with different host inventory; deferred until
  per-DS-storage measurement is needed.
- **Multi-PS on adept** (NPS > 1).  Single-host bringup
  already does this; the realnet variant defaults to NPS=1
  for the encoder bench.  The harness can call this script
  multiple times against different `--ps-host` if needed.
- **Pre-built binaries cache**.  Currently each bringup
  rebuilds reffsd via docker-compose builder.  A keep-warm
  build cache would shave 5-10 minutes off iteration time;
  not on the prereq-3 critical path.
- **Companion teardown** -- separate slice if it ever needs
  to be more than a trivial pkill + docker-compose down.

## Reference

- `.claude/design/ps-encoder-bench-4variant-realnet.md` --
  the 4-variant design this slice unblocks (prereq #3).
- `deploy/benchmark/run-ps-bench-bringup.sh` -- the
  single-host bringup whose patterns this slice extends.
- `lib/include/reffs/settings.h:140` -- `port` field on
  `[[data_server]]` that makes per-DS port-publish viable
  with no code change.
- `feedback_lab_use_worktrees_not_rsync.md` -- do not pollute
  `~/reffs-main` on lab hosts; the orchestrator uses
  `/tmp/reffs_realnet_repo` as a fresh worktree on each
  remote host.
