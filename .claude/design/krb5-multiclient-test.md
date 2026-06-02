<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Multi-Client Kerberos NFS Test Driver

## Context

We want to stress reffsd (and, pointed manually, any NFSv4.2 server) with
**many independent krb5-authenticated clients at once** -- to exercise the
paths that only light up under client-count pressure:

- the RPCSEC_GSS context cache in `lib/rpc/`
- the `nfs4_client` table and per-client session tables
- the lease reaper and grace handling when many clients come and go
- credential parsing + GSS INIT concurrency

The kernel NFS client cannot do this cheaply on one host: each distinct
client identity needs its own network namespace plus a per-namespace
`rpc.gssd` and `idmapd`.  A userspace client sidesteps all of that --
one process is one client, full stop.

## What already exists (the building blocks)

This plan is mostly *composition*, not new machinery:

| Piece | Where | What it gives us |
|-------|-------|-----------------|
| `mini_kdc` fixture | `lib/tests/mini_kdc.{c,h}` | Starts a real `krb5kdc` in realm `TEST.REFFS` with a service principal + one test user; hands back keytab + ccache paths; returns -1 (skip) if krb5kdc absent. |
| `nfs_krb5_test` | `tools/nfs_krb5_test.c` | Standalone single-client krb5 NFS tester: `mds_session_create_sec` -> `mds_file_write` -> `mds_file_read` -> CRC compare -> `mds_session_destroy`. |
| `ec_client` library | `lib/nfs4/client/` | The session / compound / file-I/O library both tools link.  RPCSEC_GSS krb5/krb5i/krb5p already wired (`mds_session.c` `authgss_create_default`). |
| Per-instance owner | `mds_session_set_owner()` | Distinct `co_ownerid` -> distinct `clientid4` at the server. |
| ccache selection | libgssapi honours `KRB5CCNAME` | `authgss_create_default` is called with `GSS_C_NO_CREDENTIAL`, so the initiator identity is whatever `KRB5CCNAME` points at. |

## What is missing

1. `mini_kdc` provisions exactly one test user (`testuser@TEST.REFFS`).
   N clients need N principals, each kinit'd into its **own** ccache.
2. No driver that runs N clients concurrently and collects per-client
   results.
3. `nfs_krb5_test`'s single-client body is `main()`-local; it needs to
   be a callable function so the multi-client driver can reuse it
   instead of duplicating the session/write/read/CRC logic.

Note: `ec_demo` was the first candidate, but `nfs_krb5_test` is the
better base -- it is already the krb5-focused single-client tool.
`ec_demo` is the pNFS / erasure-coding workload client; its extra
machinery (layouts, encodings, DS fan-out) is noise for a krb5
client-table stress test.

## Architecture

The driver owns the **entire** lifecycle -- KDC, the reffsd under
test, and the N client workers.  `mini_kdc` generates a fresh realm
(`mkdtemp`, new keytab) every run, so reffsd has to be (re)started
pointed at *that* keytab; the cleanest way to keep KDC and server
consistent is for one process to own both.  This makes
`nfs_krb5_multiclient --clients N` a single self-contained command.

```
  tools/nfs_krb5_multiclient.c   (new, links mini_kdc + ec_client)
    |
    |  1. mini_kdc_start("nfs","localhost")  -- KDC, realm TEST.REFFS;
    |                                          sets KRB5_CONFIG +
    |                                          KRB5_KDC_PROFILE in env
    |  2. write a krb5 reffsd TOML; fork+exec reffsd:
    |        child sets KRB5_KTNAME=<mini_kdc keytab>, inherits
    |        KRB5_CONFIG; reffsd's root export is flavors=["krb5"]
    |  3. poll connect() to localhost:<port> until reffsd listens
    |  4. mini_kdc_add_user() x N            -- N principals, N ccaches
    |  5. fork() x N workers:
    |        child sets KRB5CCNAME=<ccache_i> in its own env,
    |        runs krb5_client_once() (the extracted nfs_krb5_test core)
    |  6. waitpid() x N                      -- per-worker exit code
    |  7. SIGTERM reffsd, waitpid it
    |  8. mini_kdc_stop()                    -- kill KDC, rm temp trees
    v
  reffsd  (krb5 root export, spawned child)  <--  N concurrent clients
```

Each forked child has its own address space and environment, so
`KRB5CCNAME` set in the child before the GSS INIT picks the right
identity with no cross-talk.  The child calls `krb5_client_once`
**directly** -- fork-and-call, no `exec`.  One binary, no argv
marshalling, and the child inherits the already-provisioned KDC
state from the parent.

Fork-per-client is also what sidesteps the known libtirpc
connection-sharing bug (`goals.md`): multiple `clnt_create` to the
same `host:port` *within one process* hangs.  N separate processes
means N independent libtirpc instances -- no sharing, no hang.  This
is the structural reason the "in-process N-session" mode stays
deferred (see below): that mode would walk straight into the bug.

## Component 1: mini_kdc N-principal extension

`lib/tests/mini_kdc.{c,h}` -- add one function, no change to the
existing `mini_kdc_start` / `mini_kdc_stop` contract:

```c
/*
 * Provision an additional user principal <name>@TEST.REFFS on an
 * already-started KDC and kinit it into a dedicated ccache.
 *
 * ccache_out receives the path (FILE: ccache) for this principal;
 * pass it to the worker as KRB5CCNAME.
 *
 * Returns 0 on success, -1 on failure.
 */
int mini_kdc_add_user(struct mini_kdc *kdc, const char *name,
                      char *ccache_out, size_t ccache_sz);
```

Implementation mirrors the existing `testuser` path in `mini_kdc.c`:
`kadmin.local ... addprinc -pw`, then `kinit -c <ccache>` into a
per-principal cache under `kdc->kdc_dir`.  All caches sit inside the
temp tree so `mini_kdc_stop` already cleans them up.

## Component 2: extract the single-client core

`tools/nfs_krb5_test.c` -- pull the body of `main()` (lines ~163-250:
session create, write, read, CRC compare, teardown) into:

```c
/* tools/krb5_client_core.{c,h} -- shared by nfs_krb5_test and
 * nfs_krb5_multiclient. */
struct krb5_client_args {
    const char *server;
    enum ec_sec_flavor sec;
    const char *owner;        /* co_ownerid */
    const char *file;
    uint32_t block_size;
    uint32_t nblocks;
};

/* Run one client: session + write + read + CRC.  Returns 0 PASS,
 * non-zero FAIL.  KRB5CCNAME must already be set in the environment. */
int krb5_client_once(const struct krb5_client_args *a);
```

`nfs_krb5_test.c` becomes a thin CLI wrapper over `krb5_client_once`.
Behaviour is unchanged -- this is a mechanical extraction.

## Component 3: the multi-client driver

`tools/nfs_krb5_multiclient.c` (new `bin_PROGRAMS` entry):

```
nfs_krb5_multiclient --reffsd <path> [--clients <N>] [--port <n>]
                     [--sec krb5|krb5i|krb5p] [--same-principal]
```

- `--reffsd <path>`    -- reffsd binary to spawn (e.g.
                          `build/src/reffsd`).  The driver owns its
                          lifecycle.
- `--clients N`        -- spawn N workers (default 2).
- `--port n`           -- NFS port for the spawned reffsd (default
                          22049).  Not 2049 (privileged, collides
                          with a real NFS server) and not 20490
                          (reffsd's own probe-protocol listener --
                          using it for NFS is a same-process
                          EADDRINUSE).  Workers connect to
                          `localhost:<port>` -- the explicit-port
                          path in `mds_session_clnt_open` bypasses
                          portmap.
- `--same-principal`   -- all N workers share one principal (N
                          clientids, one GSS identity: stresses GSS
                          context-cache sharing).  Default: N distinct
                          principals (stresses idmap + per-principal
                          GSS contexts).

The flat-fork driver always runs every worker and reports the full
tally, so there is no first-failure stop to gate -- a `--keep-going`
flag would be vacuous and is not provided.

Flow: `mini_kdc_start` -> write a krb5 reffsd TOML (root export
`flavors = ["krb5"]`, posix backend in a temp dir) -> fork+exec
reffsd with `KRB5_KTNAME` set to the mini_kdc service keytab ->
poll `connect()` until it listens -> `mini_kdc_add_user` x N (or x1
for `--same-principal`) -> fork N workers, each sets `KRB5CCNAME`
and calls `krb5_client_once` with a distinct owner id -> parent
`waitpid`s all -> SIGTERM reffsd -> `mini_kdc_stop` -> print
`PASS k/N` / `FAIL`.

reffsd's GSS server side calls `gss_acquire_cred(GSS_C_NO_NAME,
...)` (`lib/rpc/gss_context.c`), which picks up the default keytab
-- so setting `KRB5_KTNAME` in reffsd's environment to the
mini_kdc service keytab is all the server-side wiring needed.  The
krb5 root export is a plain TOML field; no probe op required.

Exit non-zero if any worker failed; the per-worker tally is always
printed.

The spawned-reffsd path is the self-contained CI mode.  Pointing
the workers at an external, separately-configured server is
deferred -- see below.

## What this actually stresses (server side)

With reffsd as the target, N concurrent krb5 workers exercise:

- **GSS context cache** (`lib/rpc/`): N GSS INIT handshakes, N cached
  contexts; `--same-principal` checks the cache keys correctly on
  context, not just principal.
- **`nfs4_client` table**: N distinct `clientid4` from N
  `co_ownerid`s; EXCHANGE_ID / CREATE_SESSION / DESTROY_SESSION churn.
- **Lease reaper**: workers exit without DESTROY_SESSION in the
  crash-y case; reaper must drain them.
- **Concurrency**: N simultaneous compounds through the credential
  parse + dispatch path.

## Test plan

### Unit tests

No new libcheck unit tests.  Per `standards.md` the unit-test budget
is 2 s/test; a krb5 KDC handshake alone can exceed that, and the
multi-client driver is inherently a multi-process integration
exercise.  The krb5 path stays covered at the functional/CI layer,
as `nfs_krb5_test` already is.

`mini_kdc_add_user` is exercised transitively by the functional test
(Component 1 has no standalone unit test -- it is a fixture helper,
same status as the rest of `mini_kdc`).

### Functional test

New `scripts/ci_krb5_multiclient.sh`.  Because the driver owns the
whole lifecycle (KDC + reffsd + workers, option A), the script is
thin -- it does not start reffsd itself:

1. `nfs_krb5_multiclient --reffsd build/src/reffsd --clients 2`
   -- proof.
2. `--clients 50`, then `--clients 200`              -- scale.
3. `--same-principal --clients 50`                   -- GSS
   cache-sharing variant.
4. Assertions:
   - the driver exits 0 (all workers PASS with matching CRC);
   - the spawned reffsd is gone after the run (driver SIGTERMs it);
   - ASAN/LSAN clean (build reffsd with the sanitizer build);
   - run completes within the CI time budget at N=50 (N=200 is the
     manual/nightly scale point).

The driver spawns reffsd, so there is no script-side KDC/reffsd
ordering to get right -- the driver sequences KDC-before-reffsd
internally.

### Scale target

N=200 is the committed ceiling.  A 1000-client target is an open
ask; if it lands, the driver needs explicit resource handling that
N=200 does not:

- **Process count**: 1000 forked workers vs `ulimit -u`.  The driver
  should check `RLIMIT_NPROC` up front and fail fast with a clear
  message rather than fork-bombing into `EAGAIN`.
- **File descriptors**: each worker holds a TCP connection + GSS
  context; the *parent* holds N `waitpid` children but few fds, so
  the fd pressure is per-worker, not aggregate -- N=1000 is fine
  per-process.  No parent-side fd ceiling concern.
- **Batching**: at N=1000 the driver may need to cap concurrency
  (e.g. 200 in flight, refill as they exit) rather than a literal
  1000-wide fork.  Add a `--max-inflight` knob if the 1000 ask
  is confirmed; at N<=200 a flat fork is fine.

These are deferred until the 1000 ask resolves -- phases 1-6 target
N=200 with a flat fork.

### CI integration

New `krb5` target in `Makefile.ci` (`make -f Makefile.ci krb5`)
running `ci_krb5_multiclient.sh`.  Builds in the CI image
(`Dockerfile.ci`, which ships `krb5-kdc` / `krb5-admin-server`) with
`--enable-asan --enable-ubsan` and `ASAN_OPTIONS=detect_leaks=0`
(LSan leak detection stays off -- the libtirpc / pthread
process-lifetime false positives documented in
`ci_integration_test.sh` would otherwise swamp it).  Skips cleanly
(not fails) when `krb5kdc` is unavailable, matching `mini_kdc`'s
existing skip-not-fail contract.  Fold into the `full` target once
stable.

QA-facing usage and troubleshooting live in
`docs/krb5-multiclient-testing.md`.

### Test impact on existing tests

`nfs_krb5_test.c` is refactored to call `krb5_client_once`.  Its
**contract is the exit code and the CLI flags**, both preserved:

- `scripts/ci_integration_test.sh:348` runs `nfs_krb5_test --sec krb5`
  and checks `|| die` -- exit code only, no stdout parsing.
- `docs/security-test-tools.md` documents the CLI surface
  (`--server`, `--sec`, `--file`, `--id`).

The `PASS` / `FAIL` printf strings are human-readable and parsed by
nothing; `krb5_client_once` keeps emitting them.  The extraction is
mechanical: the CLI wrapper keeps flag parsing, the core moves into
`krb5_client_once`, the exit code maps from the function's return.
All other tests are untouched (new files only).

## Implementation phases

| Phase | Deliverable |
|-------|-------------|
| 1 | `krb5_client_core.{c,h}` extraction; `nfs_krb5_test` rewired over it; `make check` still green. |
| 2 | `mini_kdc_add_user()`. |
| 3 | `tools/nfs_krb5_multiclient.c` + `bin_PROGRAMS` wiring; proof at N=2. |
| 4 | `scripts/ci_krb5_multiclient.sh`; scale to N=50/200; server-side assertions. |
| 5 | `--same-principal` mode -- absorbed into phase 3 (the flag) + phase 4 (the script exercises it). |
| 6 | `krb5` target in `Makefile.ci`; `docs/krb5-multiclient-testing.md`; fold into `full` once stable. |

## Security model

- The KDC is test-only: ephemeral realm `TEST.REFFS`, torn down with
  the temp tree.  Never shipped, never reachable off-box.
- Keytabs and ccaches live under `mini_kdc`'s temp dir; `mini_kdc_stop`
  removes them.
- Happy-path only for phases 1-6.  Negative paths (expired ticket,
  wrong principal, KDC unreachable mid-run) are deferred -- see below.

## External-KDC mode (scoping)

Phases 1-6 use the embedded `mini_kdc`: a throwaway MIT realm with no
AD and no identity story.  That proves the RPCSEC_GSS plumbing, but
it does **not** exercise the thing a real QA environment cares about
-- AD principals and ID mapping.  External-KDC mode points the same
harness at an externally provisioned KDC (the QA team's MIT KDC
joined to Windows AD), with reffsd still spawned and owned locally.

This is distinct from the "external server" deferred item below:
here *our* reffsd is the server under test; only the KDC and the
identities come from outside.

### Requirements (krb5 NFS scenario coverage)

The general requirements a krb5 NFS test must cover, and where each
lands.  External-KDC mode is what makes the AD/identity rows
reachable; the rest already apply to embedded mode too.  Listing
them in one matrix gives later phases a checklist.

| Requirement | Status |
|-------------|--------|
| krb5 / krb5i / krb5p each: GSS session + I/O | embedded today (`--sec`) |
| Concurrency: many clients, same- and distinct-principal | embedded today |
| Flavor enforcement: a flavor outside the export policy gets NFS4ERR_WRONGSEC; SECINFO negotiation | embedded-capable; add an assertion |
| Identity: authenticated principal maps to the correct uid/gid; file ownership reflects it; distinct principals -> distinct owners | E3 |
| Unmapped principal squashes to nobody (65534) -- asserted as the expected outcome, not a silent pass | E3 |
| Broken-backend graceful degradation: KDC unreachable / keytab missing -> NFS4ERR_DELAY, never WRONGSEC (standards.md "Security Flavor Graceful Degradation") | negative-path; deferred |
| Misconfiguration is diagnosable: wrong/missing service keytab, SPN mismatch, enctype mismatch, DNS / reverse-DNS failure -> a clean error, no hang or crash | negative-path; deferred |
| Ticket lifecycle: expired ticket mid-session, clock skew beyond tolerance | negative-path; deferred |

The negative-path rows match the existing "Deferred" item below;
the identity rows are the E3 deliverable.

### Why embedded mode cannot cover this

`mini_kdc` issues principals like `ecuser0@TEST.REFFS`.  reffsd maps
a GSS principal to a uid in `gss_ctx_map_to_unix()`
(`lib/rpc/gss_context.c`): split at `@`, try the persistent reffs_id
table, else fall back to libnfsidmap (`nfs4_owner_to_uid`, driven by
`/etc/idmapd.conf`), else uid 65534 (nobody).  For `TEST.REFFS`
principals that resolves to nobody -- and the embedded test passes
anyway, because each worker only writes then reads back *its own*
file and compares a CRC.  Ownership is never asserted.  A uid/gid
mapping bug is invisible to the current test.

External-KDC mode exists to make ID mapping observable and asserted.

### Identity mapping against a real AD environment

reffs maps a GSS principal to a uid by **name** only:
`gss_ctx_map_to_unix` splits the principal at `@`, tries the
reffs_id table, then falls back to libnfsidmap `nfs4_owner_to_uid`.
It does not interpret the AD group / SID authorization data carried
in the Kerberos ticket.  Against a real AD-joined environment that
means:

- The principal *name* must resolve to a uid through the reffs
  host's nsswitch -- the reffs host has to run sssd (or an
  equivalent) joined to or trusting the AD domain.  With no such
  resolver every AD principal maps to 65534 (nobody).
- SID-based and group-aware mapping is not something reffs does
  today; it is the unimplemented Phase 2 of
  `.claude/design/identity.md` (the reffs_id / RFC 2307bis design).

External-KDC mode therefore exercises and asserts reffs's
**name-based** mapping against AD.  That is the realistic measure of
reffs's current behaviour; SID/group-aware mapping is explicitly out
of scope because reffs has no such support.

### The artifact contract

The harness gets **no** `kadmin` / AD-admin access.  The KDC/AD
administrator provides, out of band:

| Artifact | Purpose |
|----------|---------|
| Service keytab for `nfs/<fqdn>@REALM` | reffsd's GSS acceptor identity; the SPN must be registered in AD (`setspn` / `ktpass`) or the MIT KDC. |
| Client identities | One of: a client keytab the harness `kinit`s each principal from; pre-obtained ccaches; or principal+password pairs. |
| `krb5.conf` (or realm + KDC host) | Realm config.  AD typically needs `dns_lookup_kdc = true` and correct enctypes. |
| Expected principal -> uid/gid table | So the harness can assert reffsd mapped each identity correctly. |

### Driver changes

1. **Abstract the krb5 environment.**  The driver currently calls
   `mini_kdc_start` / `mini_kdc_add_user` / `mini_kdc_kinit` /
   `mini_kdc_stop` directly.  Introduce a `struct krb5_env` with two
   providers -- `embedded` (wraps `mini_kdc`, current behaviour) and
   `external` (consumes the provided artifacts) -- so the
   provision/run/teardown loop is provider-agnostic.
2. **New flags:** `--external-kdc`, `--realm`, `--krb5-conf`,
   `--service-keytab`, `--service-principal` (or derive from
   `--server-host`), a client-identity source
   (`--client-keytab` | `--ccache-dir` | `--principals <file>`), and
   `--expect-map <file>` (principal -> uid:gid).
3. **`--server-host`.**  The GSS service name the worker requests is
   `nfs@<host>` derived from the host it connects to.  Embedded mode
   uses `localhost` (`mini_kdc` issues `nfs/localhost`); external
   mode must connect to the fqdn that matches the SPN in the service
   keytab, that name must resolve (DNS or `/etc/hosts`), and reffsd
   must serve on an interface reachable as it.
4. **reffsd spawn.**  Still local, but `KRB5_KTNAME` = provided
   service keytab and `KRB5_CONFIG` = provided `krb5.conf`.  reffsd's
   idmap must be configured for the AD domain -- accept
   `--idmapd-conf <path>` and set it for the reffsd child, or
   document that the operator configures the host's
   `/etc/idmapd.conf` + nsswitch (sssd).

### ID-mapping verification (the new assertion)

The point of external mode.  `krb5_client_once` (or the driver after
join) gains an ownership check:

- After a worker writes its file, GETATTR it and assert owner
  uid/gid == the expected mapping for that worker's principal.
- **Cross-identity check:** worker A writes; worker B (a different
  principal) reads and sees the file owned by A's uid -- not B's,
  not nobody.
- A uid/gid of 65534 (nobody) is the canonical mapping-failure
  signal; assert against it explicitly.

Because reffs maps by name, the "expected" uid for a principal is
whatever the reffs host's sssd/nsswitch returns for that principal
name -- not an AD SID or a `uidNumber` read from the ticket.  The
`--expect-map` table records the *name-resolved* uid/gid, and the
reffs host must run sssd (or equivalent) joined to or trusting the
AD domain for any non-nobody result.  Group membership beyond the
primary group is not exercised -- reffs does not read the ticket's
AD authorization data.

### How it is run

External mode needs egress to the KDC/AD, the keytab + `krb5.conf`
mounted, and working DNS -- the pure `make -f Makefile.ci krb5`
`--rm` container (no network, no mounts) does not fit.  Plan:

- **v1:** run the driver directly on a host already in the realm:
  `./build/tools/nfs_krb5_multiclient --external-kdc ...`.
- **optional:** a separate `krb5-ad` make target with
  `--network=host` + bind mounts, operator-supplied paths via env
  vars.  Deferred to phase E4.

### Test impact

- Embedded mode (`make -f Makefile.ci krb5`) is unchanged and must
  stay green; the `krb5_env` abstraction is a refactor with no
  behaviour change for the embedded provider.
- No new libcheck unit tests -- external mode needs a real KDC, same
  rationale as the rest of the krb5 path.
- External mode is opt-in (`--external-kdc`); the default stays
  embedded.

### Security model

The service keytab and any client keytab/passwords are real
credentials: read-only to the harness, file perms 0600, never
logged, never copied into world-readable temp.  The provided
artifacts belong to the operator -- the harness consumes them and
never renews or writes them.

### Runtime inputs (supplied per run, not scoping blockers)

These are deployment facts the operator passes to the harness via
the flags above; they do not block the design:

- The realm name, KDC / AD-DC host(s), and the test account
  principals -- supplied as `--realm` / `--krb5-conf` /
  `--principals`.
- The service keytab for `nfs/<fqdn>` and the per-principal
  expected uid/gid -- supplied as `--service-keytab` and
  `--expect-map`.

One point to confirm with whoever owns the environment: whether the
KDC issuing tickets is the AD domain controller itself or a
separate MIT KDC in a trust.  It does not change reffs's behaviour
-- reffs maps by name either way -- but it bounds what the
environment can exercise.

### Phases

| Phase | Deliverable |
|-------|-------------|
| E1 | `krb5_env` abstraction; embedded provider == current behaviour; `make -f Makefile.ci krb5` stays green. |
| E2 | External provider: consume service keytab + client identities + `krb5.conf`; `--external-kdc` and related flags; `--server-host` / SPN handling. |
| E3 | ID-mapping verification: per-principal expected uid/gid, ownership assertion, cross-identity check. |
| E4 | Docs: a "Running against your own KDC" section in `docs/krb5-multiclient-testing.md`; optional `krb5-ad` make target. |

### Reference

- `.claude/design/identity.md` -- reffs's reffs_id / RFC 2307bis
  identity design (Phase 2, unimplemented); the long-term home for
  AD / SID-aware mapping inside reffsd.

### Deferred within external mode

- Automated SPN / AD account provisioning -- the AD admin's job.
- krb5i / krb5p against AD reuse the existing `--sec` flag; no
  separate work.
- The containerised `krb5-ad` target (phase E4, optional).

## Deferred

- **Negative-path coverage**: expired ticket, wrong principal,
  KDC-unreachable-mid-run.  These are the shapes most krb5 bugs take
  (NFS4ERR_WRONGSEC vs NFS4ERR_ACCESS vs hang); worth a follow-up
  plan once the happy-path driver exists.
- **Metadata workload mode**: phases 1-6 reuse the write/read/CRC
  body.  A lighter OPEN/GETATTR/CLOSE loop for sustained client-table
  pressure (rather than one-shot) is a later addition if one-shot
  proves insufficient.
- **Pointing the driver at an external server**: option A has the
  driver spawn its own reffsd.  Aiming the workers at a separately
  configured external server (a remote reffsd, or a different NFS
  server) needs a `--server <host>` mode that skips the reffsd spawn and
  assumes the server already trusts some realm.  This is a step
  beyond External-KDC mode above (which keeps reffsd ours and takes
  only the KDC from outside): here the whole server is external.
  Cross-project, no shared build; deferred.
- **In-process N-session mode**: one process holding N krb5 identities
  via explicit `gss_cred_id_t` per session (instead of N forked
  processes).  Smaller footprint, but needs `mds_session` to accept an
  explicit credential instead of `GSS_C_NO_CREDENTIAL`.  Forked
  processes are simpler and more faithful to "N clients"; revisit only
  if process count becomes the bottleneck.

## Key files

| File | Change |
|------|--------|
| `lib/tests/mini_kdc.h` | declare `mini_kdc_add_user()` |
| `lib/tests/mini_kdc.c` | implement `mini_kdc_add_user()` |
| `tools/krb5_client_core.h` | NEW -- `krb5_client_args`, `krb5_client_once()` |
| `tools/krb5_client_core.c` | NEW -- extracted single-client core |
| `tools/nfs_krb5_test.c` | rewire `main()` over `krb5_client_once()` |
| `tools/nfs_krb5_multiclient.c` | NEW -- the N-client driver; spawns KDC + reffsd + N workers |
| `tools/Makefile.am` | add `nfs_krb5_multiclient` to `bin_PROGRAMS`; new `krb5_client_core` sources; link `mini_kdc` (`libreffs_test.la`) |

The driver generates reffsd's krb5 TOML inline, so no static
`examples/reffsd-krb5.toml` is needed.
| `scripts/ci_krb5_multiclient.sh` | NEW -- functional/CI driver |
| `Makefile.reffs` | NEW `ci-krb5` target |
