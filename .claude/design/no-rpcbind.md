<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Drop the rpcbind Dependency

## Context

reffsd registers ~22 (program, version, transport) tuples with the
local rpcbind/portmap daemon at startup, in `src/reffsd.c` lines
975-1061:

| Program | Versions | Transports | calls |
|---------|----------|------------|-------|
| NFSv4 | 1 | TCP | 1 |
| NFSv3 | 1 | TCP, UDP | 2 |
| MOUNT | v3 | TCP, UDP | 2 |
| NLM | v1, v3 (`NLM_VERSX`), v4 | TCP, UDP | 6 |
| NSM | 1 | TCP, UDP | 2 |

Plus 7 `pmap_unset()` cleanup calls at startup and 7 again at
shutdown.  Total ~26 round-trips to rpcbind on `127.0.0.1:111`
during boot.

### Why this is blocking CI coverage

`ci-review.md` traces nightly soak failures to two distinct causes:

- **RC1 (fixed in `276ff7a80ff8`)**: `local_soak.sh` polled
  `/dev/tcp/127.0.0.1/$NFS_PORT` to detect server readiness.  The
  TCP socket binds before `pmap_set()` finishes registering.  On
  garbo, the rpcbind round-trips can take seconds; the integration
  test mount fired before `io_handler_main_loop()` was running.
  Workaround: emit `LOG("reffsd ready: serving on port %d")` after
  the pmap registrations complete; scripts grep for that line.
  This is a band-aid -- the underlying ~22-call latency is still
  there, just hidden behind a log marker.

- **RC2 (open)**: kernel sunrpc xprt held in 15-30s reconnect
  backoff after force-unmount of `hard,timeo=600` integration
  mounts.  The doc proposes R1 (20s sleep), R2 (soft mounts), R3
  (`ss`-based drain), R4 (extend mount timeout).

Removing the rpcbind dependency entirely:

1. Eliminates RC1 at the root.  Startup is bounded by socket bind +
   protocol register, both microseconds.  The "reffsd ready" log
   line stays as belt-and-suspenders but isn't load-bearing.
2. Removes a class of intermittent CI flakes where rpcbind itself
   is slow, missing, or contended (containers, CI runners).
3. Removes the LSan suppression `detect_leaks=0` justification (TIRPC
   `pmap_set()` is the cited reason in `src/reffsd.c:103` and
   `.claude/standards.md`).  We could turn LSan on for `make check`
   integration runs after this.
4. Lets the soak test run on hosts without rpcbind installed at all.

### What rpcbind is actually used for

Two consumers in NFS:

- **NFSv3 / MOUNT / NLM / NSM clients**: discover (program,
  version, proto) -> port via `pmap_getport(host, prog, vers, proto)`
  on `host:111`.  Required for legacy NFSv3 clients that don't
  hardcode ports.
- **NFSv4 clients**: do NOT use rpcbind.  RFC 8881 mandates port
  2049 as the well-known NFSv4 port.  Clients connect directly.

Inside reffs, additional rpcbind use:

- **Probe protocol** (`probe1`): bound to fixed port 20490 via
  `[server] probe_port` config.  Does NOT use rpcbind.
- **PS to MDS** (`[[proxy_mds]]`): config block specifies
  `mds_port` (default 2049) and `mds_probe` (default 20490) directly.
  Does NOT use rpcbind.

So the only legitimate consumer of rpcbind on the server side is
"NFSv3 clients want to discover MOUNT/NLM/NSM ports."  For an
NFSv4-only server (the BAT and CI default targets), zero clients
need it.

## Goals

Primary: make `pmap_set` calls optional and disabled by default,
gated by a config knob and by an explicit set of conditions (NFSv3
support enabled, MOUNT/NLM/NSM advertised).

Secondary: simplify the soak harness and ci scripts that currently
must `pgrep rpcbind` / start it / wait for it.  Make rpcbind a soft
dependency that is only required for NFSv3-with-MOUNT deployments.

Tertiary: removes one of the two cited LSan suppression rationales
in `src/reffsd.c:103` and `.claude/standards.md` (the `pmap_set()`
TIRPC internals).  The other cited rationale (pthread stack
process-lifetime allocations) is unaffected; flipping
`detect_leaks=1` in CI integration runs is therefore a separate
follow-up that requires its own pthread audit pass.

## Non-goals

- Removing the C `pmap_set/pmap_unset` calls from the source entirely.
  They stay in the codebase and are exercised when the config opts
  in.  An NFSv3 deployment that uses MOUNT discovery still needs
  them.
- Switching from rpcbind to rpcbind v3 (`rpcb_set`).  The TIRPC
  `pmap_*` shims internally call `rpcb_set` already; the issue is
  whether to call them at all, not which call to use.
- Adding a "discover MOUNT port via mDNS" or other replacement
  discovery mechanism.  Out of scope.
- Changing the wire format or the listener port assignments.  This
  slice is purely about the rpcbind side-channel.

## Design

### Config schema

New `[server]` knob:

```toml
[server]
# Register reffsd's NFS programs with the local rpcbind/portmap
# daemon at startup.  Must be true for NFSv3 clients that
# auto-discover MOUNT/NLM/NSM ports via rpcbind (the standard
# Linux/BSD kernel NFSv3 mount path does this).  Set to false
# for NFSv4-only deployments and for soak/CI runs where the
# ~22 rpcbind round-trips at startup cause flaky readiness
# detection.  Default true (preserve historical behaviour).
register_with_rpcbind = true
```

The default is **`true`**.  Rationale: an admin who upgrades into
an existing NFSv3-MOUNT environment without reading the changelog
must not silently break MOUNT auto-discovery.  Today every
deployment gets registration; that stays the safe default.

The default-`false` argument was tempting (CI is NFSv4-only and
that's where the pain is) but it makes the upgrade hostile to the
exact use case the plan acknowledges as legitimate.  CI / soak
configs and dev TOMLs opt OUT explicitly to `false` and gain the
fast-startup and rpcbind-independence benefits.

The cleanup `pmap_unset()` calls at startup also depend on the
knob: with `false`, we don't emit the unset calls either.  That
is correct -- if the operator just flipped from true to false,
their previous registrations get garbage-collected by rpcbind's
own staleness mechanisms.  We should not, with the knob off,
make a network call to clean up state we're not registering.

The slice does NOT add per-program conditional registration
(e.g., "skip pmap_set(NFS3...) if NFSv3 is disabled").  Today
NFSv3 / MOUNT / NLM / NSM are unconditionally compiled in and
registered together; either the whole rpcbind block runs or
none of it does.  Per-program control is deferred to the future
`enable_nfsv3 = false` slice (NOT_NOW_BROWN_COW below) which
would also gate the program register/init calls themselves, not
just the rpcbind side.

### Code changes

`src/reffsd.c`:

1. Read `cfg.register_with_rpcbind` (new field, default `false`)
   into a local `bool register_pmap`.
2. Wrap the `pmap_unset(...)` cleanup block (lines 975-982) and
   the `pmap_set(...)` registration block (lines 984-1061) in
   `if (register_pmap) { ... }`.
3. Wrap the symmetric shutdown `pmap_unset(...)` block (lines
   1136-1142) in the same guard.
4. The `LOG("reffsd ready: serving on port %d", port)` line at
   1076 stays as-is.  It now fires <1ms after the listener binds
   instead of after rpcbind round-trips.

`lib/include/reffs/settings.h` / `lib/config/config.c`:

1. Add `bool register_with_rpcbind` to `struct reffs_server_config`
   (existing struct under `[server]`).  Default `false`.
2. Parse `register_with_rpcbind` in `config.c` with the existing
   `toml_bool_in()` helper.

`src/reffsd.c:103` comment block:

- Update the LSAN justification comment.  The TIRPC `pmap_set`
  leak source is gone when the knob is off; `detect_leaks=0` may
  still be needed for pthread stack false positives but the pmap
  rationale is obsolete.
- Add a one-line note: "When `register_with_rpcbind = true`,
  TIRPC's pmap_set leaks process-lifetime; LSan suppression file
  in scripts/lsan-pmap-suppress.txt covers it."

`scripts/lsan-pmap-suppress.txt` (new, only if needed for the
opt-in path):

```
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# TIRPC's pmap_set internals leak process-lifetime allocations.
# Only relevant when [server] register_with_rpcbind = true.
leak:pmap_set
leak:rpcb_set
leak:__rpcb_set
```

Whether this file is needed depends on whether we surface a
"register_with_rpcbind = true under LSan" CI scenario.  For the
initial slice we don't -- the integration tests already use
`detect_leaks=0` per the ci_integration_test.sh ASAN_OPTIONS
block.  File creation is gated on a follow-up that flips
`detect_leaks=1` for those tests.

### Script changes

Three buckets, by what each script does:

**Bucket A -- NFSv4-only soaks.  Opt OUT to `false`, drop rpcbind
prerequisite entirely.**

| Script | Mount profile | Change |
|--------|--------------|--------|
| `scripts/local_soak.sh` | NFSv4 only | reffsd TOML: `register_with_rpcbind = false`; remove `pgrep -x rpcbind` / `sudo rpcbind` startup block |
| `scripts/ci_soak_test.sh` | NFSv4 only | Same |
| `scripts/ci_ps_soak_test.sh` | NFSv4 only | Same |

These get the full benefit of the slice: rpcbind no longer needs
to be running on the host at all, and reffsd startup is
microseconds-bounded.

**Bucket B -- NFSv3-mount integration tests.  Keep default
`true`, no script change needed, BUT the kernel NFSv3 client
still needs host-side rpcbind to do `pmap_getport()` for MOUNT
discovery.**

| Script | Mount profile | Change |
|--------|--------------|--------|
| `scripts/ci_integration_test.sh` | NFSv3 + NFSv4 | None to reffsd config (default `true`); rpcbind startup block stays for the kernel client |
| `scripts/ci_pjdfstest.sh` | NFSv3 | None |
| `scripts/ci_cthon04_test.sh` | NFSv3 + NFSv4 | None |
| `scripts/ci_sigmund.sh` | NFSv3 | None |
| `scripts/local_ps_test.sh` | NFSv4 PS + NFSv3 client | None to reffsd config; rpcbind block stays for the kernel client side |
| `scripts/_nfs_ctime_compare_inner.sh` | NFSv3 + NFSv4 | None |

These scripts work as-is.  They get a tiny startup speedup (the
~22-call latency moves from the readiness window into normal
startup, but RC1's racing-mount issue is moot because they wait
for the "reffsd ready" log marker that fires AFTER pmap registers
when the knob is true).  The ~22-call cost remains -- we did not
remove rpcbind use here, just gated it.

**Bucket C -- nightly_ci.sh (the orchestrator).**

| Script | Change |
|--------|--------|
| `scripts/nightly_ci.sh` | None directly.  Inherits Bucket A/B fixes via the test scripts it invokes.  RC2 (kernel xprt reuse) is independent of this slice and addressed separately per ci-review.md R1/R2/R3/R4. |

The clear win: any script that does NFSv4-only work no longer
sees the rpcbind round-trip cost or needs rpcbind installed at
all.  Any script that does NFSv3-MOUNT work keeps its existing
behaviour with no surprise.

### Documentation

`docs/admin-rpcbind.md` (new):

- Explains when rpcbind is needed (NFSv3 client + MOUNT auto-discovery)
- Explains how to disable it (config knob)
- Notes the tradeoff: NFSv4-only clients work fine; NFSv3 clients
  must mount with `port=...,mountport=...,mountproto=tcp` if rpcbind
  is unavailable.

`CLAUDE.md`:

- ADD a new "Service registration" section (the existing
  "Deployment Status" block is about persistent storage and is
  not the right place to extend).  Content: "reffsd registers
  its NFS programs with rpcbind by default for NFSv3 MOUNT
  auto-discovery compatibility.  Soak/CI configs and any
  NFSv4-only deployment should set `[server]
  register_with_rpcbind = false` -- this avoids ~22 startup
  round-trips that have caused readiness-race flakes (see
  `.claude/design/no-rpcbind.md`)."

`examples/reffsd.toml` and `examples/reffsd-bat.toml`:

- Show the knob commented-out at default (false) with a one-line
  explanation.

## State machine

None.  This slice is a single boolean gating an existing block of
calls.  No new lifecycle, no new persistence, no wire change.

## Persistence

None.  No on-disk format change.

## Security model

- `register_with_rpcbind = true` is exactly today's behaviour --
  nothing changes for deployments that opt back in.
- `register_with_rpcbind = false` removes a passive disclosure: a
  remote `rpcinfo -p <reffsd-host>` query that previously listed
  reffsd's services now returns only what other services on the
  host registered.  This is a security improvement (less surface
  for a network attacker), not a regression.
- No change to authentication, authorization, or wire format.
- No change to NFS4ERR_DELAY / WRONGSEC behaviour.

## Test impact analysis

### Existing tests affected

With the default flipped to `true`, the integration scripts in
Bucket B require zero changes: their behaviour is unchanged.
Only the Bucket A (NFSv4-only soak) scripts get edits, and those
get strictly less complex (rpcbind prerequisite drops away).

| Test | Bucket | Impact | Reason |
|------|--------|--------|--------|
| All `make check` unit tests | -- | PASS -- no change | Unit tests do not exercise rpcbind |
| `lib/io/tests/tls_test.c` | -- | PASS -- no change | TLS unit tests are pure-loopback |
| `lib/nfs4/ps/tests/ps_mount_client_test.c` | -- | PASS -- no change | Already comments that test runs without rpcbind |
| `scripts/ci_integration_test.sh` | B | PASS -- no change | Default `true` preserves NFSv3 MOUNT discovery |
| `scripts/ci_pjdfstest.sh` | B | PASS -- no change | Same |
| `scripts/ci_cthon04_test.sh` | B | PASS -- no change | Same |
| `scripts/ci_sigmund.sh` | B | PASS -- no change | Same |
| `scripts/local_ps_test.sh` | B | PASS -- no change | Same |
| `scripts/_nfs_ctime_compare_inner.sh` | B | PASS -- no change | Same |
| `scripts/local_soak.sh` | A | **SIMPLIFY** | reffsd TOML: `register_with_rpcbind = false`; drop `pgrep rpcbind` / start block (NFSv4 only) |
| `scripts/ci_soak_test.sh` | A | **SIMPLIFY** | Same |
| `scripts/ci_ps_soak_test.sh` | A | **SIMPLIFY** | Same; this script exists on `origin/main` since `3bb0861bc4a4` |
| `scripts/nightly_ci.sh` | -- | PASS -- no change | Calls into the above test scripts; their fixes propagate |

The integration scripts (Bucket B) continue to exercise the
rpcbind path with the default-on knob -- so the existing
pmap_set/pmap_unset code stays under CI coverage.  We don't lose
that coverage; we gain coverage of the don't-register path via
the soak scripts (Bucket A).

### New unit tests

**`lib/config/tests/config_test.c`** (extend existing):

| Test | Intent |
|------|--------|
| `test_register_with_rpcbind_default_true` | TOML without the knob -> `cfg.register_with_rpcbind == true` (regression-proof for the default that protects upgrade compatibility) |
| `test_register_with_rpcbind_explicit_true` | `register_with_rpcbind = true` in TOML -> field set to true |
| `test_register_with_rpcbind_explicit_false` | `register_with_rpcbind = false` in TOML -> field set to false |

These are pure config-parser tests, no reffsd startup.

### New functional tests

**`scripts/ci_no_rpcbind_test.sh`** (new -- header includes
SPDX block per `.claude/standards.md`):

| Step | Intent |
|------|--------|
| 1. Stop rpcbind on the host (or run in a namespace with no rpcbind) | Prove server starts cleanly without rpcbind |
| 2. Start reffsd with `[server] register_with_rpcbind = false` config | Should reach "reffsd ready" within 100ms |
| 3. Mount NFSv4 (`vers=4.2,port=12049`) | Should succeed |
| 4. Run a small workload (write, read, unlink) | Should succeed |
| 5. `rpcinfo -p localhost` (if rpcbind absent: skip; if present: assert reffsd not listed) | Confirms no registration leaked |
| 6. Teardown | Should be clean (no AddressSanitizer / LeakSanitizer hits) |

Run from `ci-full`.  Skipped (exit 77) on hosts where rpcbind
suppression isn't possible (e.g., shared-host CI runners).

### CI pipeline impact

- `ci-full` adds the new functional test.  Net add: ~5 seconds.
- `ci-check` (unit tests only) gains the 3 new config_test cases.
  Net add: <1 second.
- Soak tests get faster startup (eliminates the 0.5-3s pmap window
  per restart cycle).  At 5-min restart cadence over a 30-min CI
  soak that's ~6 cycles * up to 3s = up to 18s of test wall-clock
  saved per run.  Negligible but measurable.

### LSan opportunity (limited)

The `pmap_set()` LSan suppression rationale goes away in the
soak path (Bucket A, knob = false).  But the integration tests
(Bucket B) keep the knob on and still need the suppression for
both `pmap_set` AND pthread stacks.  And `.claude/standards.md`
explicitly cites pthread stacks as a separate process-lifetime
leak source unaffected by this slice.

So flipping `detect_leaks=1` is NOT unlocked end-to-end by this
slice.  It's only unlocked for the Bucket A scripts, which today
already don't run under LSan because the soak harness wasn't
built that way.  Adding LSan to the soaks would be the actual
follow-up; it's worth doing but it's a separate slice with its
own pthread audit.  Marked NOT_NOW_BROWN_COW.

## Implementation steps

1. **Config knob**: add `bool register_with_rpcbind` to
   `struct reffs_server_config`; parse in `config.c`; default
   `true`.  Add the 3 config_test cases (TDD: tests first, fail,
   then add field).
2. **Server gating**: wrap the three `pmap_*` blocks in
   `src/reffsd.c` (startup unset, startup set, shutdown unset)
   in `if (cfg.server.register_with_rpcbind)`.  Build, run unit
   tests.
3. **Soak script simplification (Bucket A)**: write
   `register_with_rpcbind = false` into the reffsd TOML config
   used by `local_soak.sh`, `ci_soak_test.sh`,
   `ci_ps_soak_test.sh`; remove the rpcbind startup block from
   each.
4. **Bucket B verification**: confirm no script changes needed
   (default `true` preserves behaviour); spot-run
   `ci_integration_test.sh` to prove zero regression.
5. **Functional test**: add `scripts/ci_no_rpcbind_test.sh`
   (with SPDX header), wire into the `ci-full` target in
   `Makefile.reffs`.
6. **Documentation**: add `docs/admin-rpcbind.md` (with SPDX
   header); ADD a new "Service registration" section to
   `CLAUDE.md`; update the example TOMLs to include the knob
   commented at default; update the LSan-comment block in
   `src/reffsd.c:103`.
7. **Verify on macOS**: `make check` clean, `make -f
   Makefile.reffs license` clean, `make -f Makefile.reffs style`
   clean.
8. **Verify on Linux (reffs.ci)**: full `make check` + the new
   `ci_no_rpcbind_test.sh`; spot run an integration script to
   confirm Bucket B unchanged.
9. **Soak rerun on whatever's available**: prefer reffs.ci or
   witchie; if dreamer is free per
   `feedback_check_dreamer_busy.md`, run there too -- but ASK
   FIRST.  Confirm the RC1 symptom is gone in the Bucket A
   scripts: startup is instant and the initial mount succeeds
   within 1s instead of 30s.

## Verification

| Check | Expected |
|-------|----------|
| `make -j$(nproc)` | zero errors, zero warnings |
| `make check` | all existing + 3 new config tests pass |
| `make -f Makefile.reffs license` | clean (one new file with SPDX) |
| `make -f Makefile.reffs style` | clean |
| `scripts/ci_no_rpcbind_test.sh` | exit 0 (or 77 if host can't suppress rpcbind) |
| `scripts/ci_integration_test.sh` | pass (knob explicitly flipped on) |
| Soak: time from reffsd start to first mount success | <1s (was: up to 30s under RC1) |
| `rpcinfo -p localhost` after reffsd start (default config) | NFSv4, NFSv3, MOUNT, NLM, NSM listed (preserves today's behaviour) |
| `rpcinfo -p localhost` with `register_with_rpcbind = false` (Bucket A soak config) | no NFS programs listed; reffs's reffsd is rpcinfo-invisible |

## Deferred / NOT_NOW_BROWN_COW

- **LSan flip in CI integration runs**: separate audit slice once
  the rpcbind leak source is gone by default.
- **`enable_nfsv3 = false` config knob**: full removal of NFSv3 /
  MOUNT / NLM / NSM service registration when the deployment
  doesn't need it.  Slot in alongside the existing
  `protocol_was_registered()` per-program guards.
- **Removing NLM v1 / v3 (`NLM_VERSX`) registration entirely**:
  Linux NFSv3 clients use NLMv4 only; v1/v3 are legacy.  Can be
  unconditionally dropped after a deprecation cycle.
- **NSM (`SM_PROG`) on a separate process**: traditionally
  rpc.statd runs separately and the kernel NLM uses it.  reffs's
  in-process NSM is unusual; a future slice could move it to a
  separate process to match the kernel architecture.
- **Replacing rpcbind discovery with a probe-protocol query** for
  PS->MDS and admin tooling.  Already half-done -- the probe
  protocol bypasses rpcbind -- could be extended to fully replace
  rpcinfo for reffs-internal admin tooling.

## Key files

| File | Change |
|------|--------|
| `lib/include/reffs/settings.h` | Add `register_with_rpcbind` to `struct reffs_server_config` |
| `lib/config/config.c` | Parse the new knob |
| `lib/config/tests/config_test.c` | 3 new test cases |
| `src/reffsd.c` | Gate three `pmap_*` blocks; update LSan comment |
| `scripts/local_soak.sh` | Remove rpcbind startup |
| `scripts/ci_soak_test.sh` | Remove rpcbind startup |
| `scripts/ci_ps_soak_test.sh` | Remove rpcbind startup |
| `scripts/ci_integration_test.sh` | TOML config: `register_with_rpcbind = true` |
| `scripts/ci_pjdfstest.sh` | Same |
| `scripts/ci_cthon04_test.sh` | Same |
| `scripts/ci_sigmund.sh` | Same |
| `scripts/local_ps_test.sh` | Same |
| `scripts/_nfs_ctime_compare_inner.sh` | Same |
| `scripts/ci_no_rpcbind_test.sh` | NEW -- functional test |
| `Makefile.reffs` | Wire `ci_no_rpcbind_test.sh` into `ci-full` |
| `docs/admin-rpcbind.md` | NEW -- admin guide |
| `CLAUDE.md` | One-line note about the new default |
| `examples/reffsd.toml` | Commented example of the knob |
| `examples/reffsd-bat.toml` | Same |

## RFC references

- RFC 8881 §1.5: NFSv4 well-known port 2049, no rpcbind required.
- RFC 1057 Appendix A: SunRPC port mapper (`PMAP_PROG = 100000`,
  the protocol rpcbind v2 speaks; `pmap_set()` is its client API).
- RFC 1833 §3 (rpcbind v3) and §4 (rpcbind v4): the modern entry
  point `rpcb_set()`; TIRPC's `pmap_set()` shim internally calls
  `rpcb_set` on RFC 1833-capable systems, so gating the
  `pmap_set()` call site disables both.
- `draft-haynes-nfsv4-flexfiles-v2-data-mover` (PS): the
  `[[proxy_mds]] mds_port`/`mds_probe` config already bypasses
  rpcbind; this slice extends that pattern to the server's own
  service registration.

## Why this is the right time

The user has flagged rpcbind-induced startup slowness as the root
cause of nightly soak failures since 2026-04-23.  The current
workaround (`reffsd ready` log marker, `276ff7a80ff8`) hides the
symptom but the underlying ~22-call latency remains and continues
to cause flakes when rpcbind is contended.  The slice is small (one
config knob, three guarded blocks, six script tweaks) and the
correctness story is clean (NFSv4 doesn't need rpcbind; NFSv3
deployments opt back in).  It unblocks the "100% green soak"
goal and opens the door to LSan-clean integration runs.
