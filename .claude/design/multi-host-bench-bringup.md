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

- The `[[data_server]]` config already has a `port` field
  (`lib/include/reffs/settings.h:140`) and the layout XDR
  carries (address, port) tuples per RFC 8881 § 19.  No code
  change required.
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
| PS never registers                   | grep MDS log for grant count != 1  |
| PS upstream session never up         | grep PS log for `listener stays dark` |
| docker-compose port conflict on shadow | `docker compose up` exits non-zero |
| SSH connect failure                  | `ssh ... exit 0` smoke before any work |

## Deferred / NOT_NOW_BROWN_COW

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
