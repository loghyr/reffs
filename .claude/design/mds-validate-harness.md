<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# MDS Client-Validation Run Harness

## Goal

A reproducible, committable way to run reffs as a Flex Files **MDS
only** (NFSv4.2, no rpcbind) pointing at **external, operator-provided
data servers**, as a stable mount target for validating a third-party
NFSv4.2 client.

reffs hosts no data in this mode.  The client mounts the MDS over
NFSv4.2, receives Flex Files layouts, and does data I/O directly to
the external DSes.

## Deliverables

- `examples/reffsd-mds-validate.toml` -- MDS config: `role = "mds"`,
  `register_with_rpcbind = false`, open `[[export.clients]]`,
  `layout_types`/`dstores` on the root export, and placeholder
  `[[data_server]]` blocks (RFC 5737 doc IPs) the operator edits.
- `Makefile.reffs` targets `run-mds-validate` / `stop-mds-validate`.
- `scripts/run_mds_validate.sh` -- bare-metal launcher (no Docker):
  builds reffsd in `build/` if needed, generates a runtime config from
  `--ds` arguments, and runs reffsd in the foreground.  Does not mount.

## Usage

Containerized (host networking, builds in the sandbox):

```
# Edit examples/reffsd-mds-validate.toml: replace the [[data_server]]
# placeholder addresses with your real DS IPs/paths.
make -f Makefile.reffs run-mds-validate
# Client: mount -t nfs -o vers=4.2 <host>:/ /mnt/point
```

Bare-metal (no Docker), passing reachable DSes on the command line:

```
scripts/run_mds_validate.sh --ds 192.168.2.105:/ds1 --ds 192.168.2.106:/ds2
# --dry-run prints the generated config without building or launching.
# With no --ds the MDS still boots and is mountable but issues no
# layouts.  The script never mounts -- do that from the client.
```

Knobs:
- `SAN` -- sanitizer flags for the sandbox build (default
  `--enable-asan --enable-ubsan`; set `SAN=""` for a plain build if a
  sanitizer abort would disrupt a long validation run).
- `MDS_VALIDATE_TOML` -- path (under the repo root, mounted at
  `/reffs`) to an alternate config.

## Why these specific choices

### Two independent "rpcbind" concerns

1. **MDS's own registration** with the local rpcbind.  Disabled by
   `register_with_rpcbind = false` (the config knob, see
   `no-rpcbind.md`).  NFSv4.2 uses the well-known port 2049 (RFC 8881
   S1.5) and needs no portmapper.  This is what makes the
   host-port-111 publish unnecessary.
2. **MDS -> DS outbound portmap lookups.**  The MDS is an NFSv3
   client to each external DS for the control plane (MOUNT / CREATE /
   GETATTR -- `mds.md`).  If a DS is on a standard port the MDS does a
   portmap lookup against *that DS's* rpcbind:111.  This is unaffected
   by the config knob; pin `port =` in the `[[data_server]]` block to
   bypass it.

### Host networking, not port publishing

`--network host` (no `-p` publishes) so (a) the MDS reaches external
DS IPs natively, (b) the client mounts 2049/20490 directly, and (c)
the rootless-podman/pasta failure to bind host UDP 111 never arises
(there is no 111 publish, and no host-port mapping at all).

Requirement: **host port 2049 must be free** on the box running this
target (host networking binds the real host 2049).

### Entrypoint bypass

The Dockerfile entrypoint runs `rpcbind -w` unconditionally.  The
target passes `--entrypoint /bin/bash`, so the container-local rpcbind
is never started -- and no image rebuild is needed.  (A complementary,
optional hygiene change would gate the entrypoint's rpcbind behind an
env var; not required for this target.)

### layout_types is mandatory (non-obvious)

`reffsd.c` applies pNFS config from `[[export]]` at startup
(`src/reffsd.c` ~982).  If `[[export]]` does not set `layout_types`,
the per-export layout gate **denies every LAYOUTGET** (reffsd logs a
loud warning, ~1005).  The shipped `examples/reffsd-mds.toml` omits
it; the validate config sets `layout_types = ["ffv1"]` + `dstores`.
`ffv1` (NFSv3 data path) works with any NFSv3 DS; `ffv2` requires
reffs DSes that speak CHUNK ops.

### Boots even when DSes are unreachable

`dstore_alloc(do_mount=true)` logs "mount failed ... continuing" and
marks the dstore unavailable rather than aborting
(`lib/nfs4/dstore/dstore.c` ~531); `dstore_load_config` continues past
per-DS failures and `reffsd.c` treats its non-zero return as a warning
(~956).  So the server boots and is mountable with placeholder DS IPs
-- but LAYOUTGET returns NFS4ERR_LAYOUTUNAVAILABLE until real,
reachable DSes are configured.  (Note: unreachable placeholder IPs can
stall startup on the per-DS connect timeout -- `clnt_create(...,"tcp")`
in `mount_get_root_fh` black-holes on a non-routable DS for ~2 min.)
`scripts/run_mds_validate.sh` sidesteps this by generating the config
only from the reachable DSes passed via `--ds` (or none at all), so it
never boots against the committed placeholder addresses.

## Deferred / not in scope

- AUTH_SYS only (`flavors = ["sys"]`).  krb5/TLS surfaces are a later
  addition.
- Gating the Dockerfile entrypoint's `rpcbind -w` behind an env var
  (optional hygiene; the target bypasses the entrypoint already).
- Dropping the `-p 111` publish from the standalone `run-image` target
  (separate concern / separate commit).
