<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# PS-Mount Nightly Smoke Test

## Context

The 2026-04-30 LAYOUTGET regression in `make run-benchmark` (commit
`05b9ae22d58f` added the per-export layout-policy gate, the benchmark
TOML never set `layout_types`, every LAYOUTGET silently denied) lived
in main for ~3 weeks before anyone noticed.  Nightly CI was green
throughout because nightly drives the kernel NFS client, which does
not request layouts -- only the userspace `ec_demo` client does, and
`ec_demo` only runs from the bench docker stack.

Adding `ec_demo` to nightly is unattractive: it is being absorbed
into `lib/nfs4/ps/ec_pipeline` (proxy-server phase 0), and adding
docker/compose to nightly_ci.sh introduces new failure dimensions
and slow startup for a test that runs every night.

The PS role is the better long-term test target: it lives in-repo
as a real role going forward, and it exercises the same userspace
NFSv4.2 client surface (LAYOUTGET via `lib/nfs4/ps/ec_pipeline`)
that broke -- but as a **server** that nightly's existing
kernel-mount harness can drive without containers.

## Goal

Catch the next "userspace-NFSv4.2-client path silently broken" class
of regression in nightly CI.  Specifically: any change to LAYOUTGET
encoding/decoding, layout-policy gating, dstore selection, or the
ec_pipeline orchestration should fail nightly the morning after it
lands.

## Approach

Bare-metal, no containers.  Use the same machine nightly already
runs on (garbo or similar).

```
   reffsd role=combined    <-- existing nightly target (MDS + DS)
        ^                       on port 2049
        |
   reffsd role=ps           <-- NEW: PS proxying the combined
        ^                       reffsd, on port 4098
        |
   mount -t nfs4 localhost:/<path> /mnt/ps   <-- nightly client
        |
   small workload (cp / cmp / readdir)
```

Steps:

1. After the existing combined-mode reffsd is up, start a second
   reffsd with `role=ps`, `[[proxy_mds]] address = "localhost"
   port = 4098`, bound to `127.0.0.1:4098`.  Wait for it to
   PROXY_REGISTRATION-handshake with the upstream (the combined
   reffsd).
2. `mount -t nfs4 localhost:/<discovered-path> /mnt/reffs-ps`.
3. Run a tiny smoke workload: write 64KiB, read it back, `cmp`,
   `readdir`, `unlink`.
4. `umount /mnt/reffs-ps`, signal the PS reffsd to shut down,
   collect stderr/log into the nightly artefact directory.
5. PASS only if the cmp matches and no LSan/ASan/UBSan output
   appears in either reffsd's log.

## Why this catches the LAYOUTGET-policy class

The PS issues LAYOUTGET to the upstream MDS to discover the
underlying DS layout for each proxied file.  Any regression that
makes LAYOUTGET deny (the bug we just fixed) will surface as
"PS cannot serve the file" -- the kernel client mount fails or
hangs on first I/O.  The existing nightly cleanup (fuser + umount
-l) handles the hang case.

The PS path also exercises:

- `lib/nfs4/ps/ec_pipeline` (the libified ec_demo orchestration)
- userspace NFSv4.2 client COMPOUND building / parsing
- LAYOUT4_FLEX_FILES_V2 layout decode
- Per-export layout-policy gate (the specific bug class)
- PROXY_REGISTRATION over loopback (RPCSEC_GSS or mTLS depending
  on what the proxy-server.md spec settles on for nightly)

## What this does NOT cover

- Codec correctness (the underlying ec_pipeline tests cover that).
- DS failover / degraded reads (separate slice).
- Cross-codec interoperability (separate slice).
- True multi-host topologies (still requires the docker bench).

## Prerequisites

This slice is BLOCKED until:

1. PS phase 1 (second listener + empty proxy namespace) lands,
   so the second reffsd actually accepts mounts.
2. PS phase 2 (showmount discovery + proxy SB allocation) lands,
   so the discovered path resolves to an upstream filehandle.
3. PS phase 3 (`REFFS_DATA_PROXY` backend, client READ) lands,
   so the cmp succeeds.

Phases 1-3 are tracked in `.claude/design/proxy-server.md`.  Once
phase 3 ships, this slice is ~half a day of wiring + nightly
integration.

## Implementation Notes (for the future implementer)

- New script: `scripts/nightly_ps_smoke.sh` -- callable from
  `nightly_ci.sh` between unit tests and the kernel-mount
  integration tests.
- The PS reffsd uses the same build artefact as the combined
  reffsd; only the TOML differs (`role = "ps"` plus a single
  `[[proxy_mds]]` block pointing at `localhost:2049`).
- For PROXY_REGISTRATION authentication, prefer mTLS (mini-CA
  fixture is already in `lib/io/tests/tls_test.c`) over Kerberos
  to avoid pulling a KDC into nightly.
- Capture both reffsds' stderr to separate log files in the
  nightly artefact dir.  The post-test step greps both for ASan/
  LSan/UBSan markers and fails the run if any appear -- same
  pattern as `ci_integration_test.sh`.
- Set `[ASAN_OPTIONS, UBSAN_OPTIONS] = halt_on_error=0,
  detect_leaks=0` for the same `pmap_set` / pthread-stack
  process-lifetime LSan false-positive reasons documented in
  `.claude/standards.md` "CI Integration Tests".

## Why not rely on the startup WARN added in `06207c884084`

The WARN catches the specific `role=mds + data_servers + no
layout_types` config-drift class.  It does NOT catch:

- A regression in the LAYOUTGET handler itself (e.g. someone
  refactors and accidentally always returns LAYOUTUNAVAILABLE).
- A regression in `ec_pipeline` LAYOUTGET encoding/decoding.
- Per-export-dstore-binding regressions.
- Layout4_FLEX_FILES_V2 decode regressions.
- PS PROXY_REGISTRATION regressions.

All of those would silently break ec_demo / PS while passing the
WARN.  The PS-mount nightly is the proper end-to-end coverage.
