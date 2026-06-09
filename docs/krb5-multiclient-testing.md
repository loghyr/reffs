<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Multi-Client Kerberos NFS Testing

A guide for QA engineers. No prior reffs knowledge assumed.

## What is reffs?

reffs is an NFS server — it speaks NFSv3 and NFSv4.2 to NFS clients,
and also implements pNFS Flex Files (a way of striping file data
across several data servers). For this test you only need to know
that reffs is "the server under test."

## What does this test do?

`nfs_krb5_multiclient` stress-tests reffs with **many Kerberos-
authenticated NFS clients at once**. Each simulated client logs in
with its own Kerberos identity, mounts the server, writes a file,
reads it back, verifies a checksum and the file's owner, and
disconnects.

Why this matters: a single Kerberos client exercises very little of
the server's authentication machinery. Running 50 or 200 of them
concurrently shakes out bugs in the server's GSS (Kerberos) context
cache, its per-client state tables, and its lease handling — the
code paths that only light up under client-count pressure.

The test is **self-contained**. It starts everything it needs:

- an embedded Kerberos KDC (the ticket-granting server),
- a reffs server configured to require Kerberos,
- N client workers, each with its own Kerberos identity.

You do not set up a KDC, you do not configure a server, you do not
run `kinit`. One command does all of it.

## Running it — the easy way

From the top of the reffs source tree:

```sh
make -f Makefile.ci krb5
```

This builds reffs inside a Docker container that already has the
Kerberos tools installed, then runs the test. **Requirements:**

- Docker installed and runnable (the command auto-uses `sudo` if
  your user can't reach the Docker daemon).
- Roughly 5–15 minutes the first time (it builds a container image
  and compiles reffs); faster on later runs.

You do not need Kerberos, a build toolchain, or anything else on
your own machine — it all happens inside the container.

## Reading the result

The test prints one of three outcomes.

### PASS

```
=== ci_krb5_multiclient: proof (N=2) ===

PASS 2/2 krb5 clients
--- proof (N=2) PASS ---
=== ci_krb5_multiclient: scale (N=50) ===

PASS 50/50 krb5 clients
--- scale (N=50) PASS ---
...
ci_krb5_multiclient: all cases passed
```

The command exits 0. Every Kerberos client authenticated and did
its I/O correctly. This is the expected result.

### SKIP

```
SKIP: krb5kdc not available -- skipping krb5 multiclient test
```

The command exits 0. The Kerberos KDC software was not present in
the environment. Inside the `make -f Makefile.ci krb5` Docker path
this should **not** happen — the CI image ships the KDC. If you see
SKIP there, the container image is wrong or stale; rebuild it with
`make -f Makefile.ci image`.

### FAIL

```
FAIL 47/50 krb5 clients (3 failed)
...
nfs_krb5_multiclient: run failed -- reffsd work dir kept: /tmp/krb5mc_a1b2c3
FATAL: scale (N=50) failed (driver rc=1)
```

The command exits non-zero. At least one client failed, or the
server itself reported a problem. See **Troubleshooting** below.

## Running at larger scale

By default the test runs three cases: 2 clients (a quick proof),
50 clients, and 50 clients sharing a single Kerberos identity.

To also run the 200-client case (the heavy/nightly scale point):

```sh
KRB5MC_BIG=1 make -f Makefile.ci krb5
```

## Running the tool directly

If you already have a reffs build (for example you ran the steps in
the project's build instructions and have a `build/` directory),
you can run the driver yourself without Docker — provided your
machine has the Kerberos server tools installed
(`krb5-kdc` + `krb5-admin-server` on Debian/Ubuntu, `krb5-server`
on Fedora/RHEL):

```sh
./build/tools/nfs_krb5_multiclient --reffsd ./build/src/reffsd --clients 10
```

### Driver options

| Option | Meaning |
|--------|---------|
| `--reffsd <path>` | Path to the `reffsd` binary to launch (required). |
| `--clients <N>` | Number of concurrent client workers (default 2). |
| `--port <n>` | TCP port for the test server (default 22049). Change it only if 22049 is busy. |
| `--sec <krb5\|krb5i\|krb5p>` | Kerberos protection level: authentication only (`krb5`), + integrity (`krb5i`), or + encryption (`krb5p`). Default `krb5`. |
| `--same-principal` | All workers share one Kerberos identity (still distinct clients to the server). The default gives every worker its own identity. |
| `--server-host <host>` | Hostname the workers connect to, and the GSS service name. Default `localhost`. In external-KDC mode set it to match the service keytab's `nfs/<host>` principal. |
| `--external-kdc` | Use an external, pre-provisioned KDC instead of the embedded one. See "Running against your own KDC" below. |
| `--krb5-conf <path>` | (external-KDC) `krb5.conf` for the realm. |
| `--service-keytab <path>` | (external-KDC) keytab holding reffsd's `nfs/<host>` service principal. |
| `--principals <path>` | (external-KDC) file of `<principal> <password>` lines, one identity per worker. |
| `--expect-map <path>` | (external-KDC) file of `<principal> <owner>` lines; asserts each created file's owner. |
| `--keep-going` | Reserved; currently all workers always run regardless. |
| `--help` | Print usage. |

### Exit codes

| Code | Meaning |
|------|---------|
| 0 | All workers passed. |
| 1 | A worker failed, or the server reported a problem. |
| 2 | Bad command-line arguments. |
| 77 | Skipped — the Kerberos KDC software is not installed. |

## Running against your own KDC

The default mode stands up its own throwaway KDC. To stress reffs
against a **real Kerberos realm** -- for example an MIT KDC joined
to Active Directory -- run the driver in *external-KDC mode*.

This mode is run directly (not through `make -f Makefile.ci krb5`):
the test host needs network access to the KDC, and you supply the
realm's krb5 artifacts.

### What you provide

| Artifact | What it is |
|----------|-----------|
| A `krb5.conf` | The realm configuration the test should use. AD realms usually want `dns_lookup_kdc = true`. |
| A **service keytab** | A keytab holding the `nfs/<host>` service principal for the host reffsd runs on. Whoever administers the KDC/AD registers that SPN and exports the keytab. |
| A **principals file** | One `<principal> <password>` pair per line -- the test accounts the workers authenticate as. Principals are fully qualified (`user@REALM`); blank lines and `#` comments are ignored. |
| An **expect-map** (optional) | One `<principal> <owner>` pair per line. When given, the test asserts that each file's owner -- as the server reports it -- matches the expected value for the worker that wrote it. |

Treat the service keytab and the principals file as secrets: they
are credentials. Keep them mode `0600` and never commit them.

A principals file looks like:

```
# principals.txt -- one test identity per line
testuser1@AD.EXAMPLE.COM  Passw0rd-one
testuser2@AD.EXAMPLE.COM  Passw0rd-two
```

### Running it

```sh
./build/tools/nfs_krb5_multiclient \
    --reffsd ./build/src/reffsd \
    --clients 50 \
    --external-kdc \
    --krb5-conf /path/to/krb5.conf \
    --service-keytab /path/to/nfs.keytab \
    --principals /path/to/principals.txt \
    --server-host reffs-host.example.com
```

`--server-host` must be the hostname the workers connect to **and**
the hostname in the service keytab's `nfs/<host>` principal -- they
have to match or the Kerberos handshake fails. `--principals` must
list at least `--clients` identities (one per worker); with
`--same-principal` only the first is used.

### Checking identity mapping

Each worker GETATTRs the file it created and checks the owner the
server reports. Without `--expect-map` it only asserts the owner is
non-empty. To assert it exactly:

1. Run once without `--expect-map`. Each worker prints a line like
   `TEST 4: GETATTR owner of <file> ... PASS (owner=<X>)`.
2. If those owner values are what your realm's identity mapping
   should produce, codify them: write an expect-map file of
   `<principal> <owner>` lines.
3. Re-run with `--expect-map /path/to/expect-map.txt`. Now a worker
   fails if its file's owner does not match -- including the file
   showing up owned by `nobody`, which means the server's identity
   mapping is wrong for that principal.

## Stressing an external FFv1 server (any NFSv4.2 server)

The modes above stand up reffs themselves and stress it.  If you
want to point N krb5 clients at an **already-running** NFSv4.2
server that issues Flex Files v1 layouts -- for instance, the AD-
joined NFS server in your QA environment -- use the companion
script:

```
scripts/krb5_ffv1_stress.sh
```

It does not provision the server, the KDC, or the users.  It just
`kinit`s N principals you supply, launches N `ec_demo verify`
instances in parallel (each as a distinct krb5 identity, hitting
the FFv1 layout path -- `LAYOUTGET` + direct DS I/O -- exactly the
workload the server is designed for), and tallies pass / fail.

### What you provide

- a reffs build (steps below) so `./build/tools/ec_demo` exists,
- the server's `host[:port]`,
- a directory path on the server the workers can write into,
- a *principals* file: one `<principal> <password>` per line,
  principals fully qualified (`user@REALM`); blank lines and `#`
  comments are ignored.

You also need `kinit` on the test host and a reachable krb5 realm
(your AD or MIT KDC).  The script does not install or configure
those.

### Getting a reffs build

On a Linux host with autotools and a C toolchain:

```sh
git clone https://github.com/loghyr/reffs.git
cd reffs
mkdir -p m4 && autoreconf -fi
mkdir build && cd build
../configure
make -j$(nproc)
```

`configure` will name any missing development packages.  On
Fedora / RHEL the usual set is `gcc autoconf automake libtool
pkgconfig libtirpc-devel openssl-devel krb5-devel libuuid-devel
userspace-rcu-devel libev-devel libcap-devel`; on Ubuntu use the
same names with `-dev`.  If you would rather use a containerised
build, `make -f Makefile.reffs image` builds a development image
with everything pinned (Dockerfile lives at the top of the tree).

The `make -j$(nproc)` invocation builds the whole tree; the
script needs `build/tools/ec_demo` to exist, but the full build
is what dependency-tracks correctly across `git pull`s.  Don't
try `make -j tools/ec_demo` -- the parallel-build target graph
for that file pulls in libraries that aren't reachable from a
single-target invocation, and the build will fail.

### Running it

```sh
./scripts/krb5_ffv1_stress.sh \
    --server nfs.example.com:2049 \
    --path /your/share/stress \
    --clients 8 \
    --principals /path/to/principals.txt
```

Each of the 8 workers writes `/your/share/stress/krb5stress_<i>`,
reads it back, and compares -- exiting 0 on a clean round trip.
The script exits `PASS N/N` or `FAIL k/N (m failed)`; on any
failure it keeps its run directory and dumps each failing worker's
log tail to stderr.

Pass the port (`:2049`) explicitly.  Omitting it can hit a
session-create round trip that ends with `session create failed:
-111` (ECONNREFUSED) on the first attempt against some targets,
because the no-port form takes a different code path through
libtirpc's portmapper that some servers (notably the Hammerspace
Anvil) handle inconsistently.

**Server hostname resolution.**  The `--server` value must
resolve to the server's IP from the test client.  AD-attached
server names (e.g. `*.ad.local`, `*.win.ad.test`) often don't
appear in public DNS; add an `/etc/hosts` entry on the test
client:

```
10.200.107.69  ae58rfme5g080w.ad.local
```

`getent hosts <hostname>` from the test client should return the
right IP before you run the script.

Useful options:

| Option | Meaning |
|---|---|
| `--size <bytes>` | Bytes per file (default 10 MB) |
| `--k <K> --m <M>` | EC geometry; default `1 0` with the `mirror` codec (matches one-DS-per-share targets).  For RS 4+2 pass `--codec rs --k 4 --m 2` and the target share must back the layout with at least 6 DSes. |
| `--codec rs\|mojette-sys\|mojette-nonsys\|stripe\|mirror` | Erasure-coding codec. `mirror` is the default. |
| `--nconnect <N>` | Number of TCP transports per session (NFSv4 multi-pathing).  Default 1.  See "Running at scale" below. |
| `--source-ip <IP>` | Bind outgoing sockets to a specific local IP.  Useful when the test client has multiple addresses and you need to spread workers across them.  See "Running at scale" below. |
| `--sec <flavor>` | Security flavor (default `krb5`). |
| `--ec-demo <path>` | Where the `ec_demo` binary lives (default `./build/tools/ec_demo`). |
| `--help` | Full option list. |

### Running at scale

The test client opens a fresh source-port pair per worker per
transport.  When the target enforces strict reserved-port
checking (the default NFSv4 stance, and the Anvil default),
clients are limited to the privileged port range -- about 500
usable ports per source IP after TIME_WAIT slack.  Per-source-IP
worker ceiling, before connect or `bindresvport` start failing:

| `--nconnect` | Max workers per source IP |
|---|---|
| 1            | ~400 |
| 2            | ~200 |
| 4            | ~100 |
| 8            | ~50 (hard wall ~53) |

For back-to-back runs at any meaningful concurrency, enable
TIME_WAIT slot reuse on the test client:

```sh
sudo sysctl -w net.ipv4.tcp_tw_reuse=1
```

Without it, the second run typically fails with
`session create failed: -99` (EADDRNOTAVAIL) or
`bindresvport failed (Address already in use)`.  Make this
permanent by adding the line to `/etc/sysctl.d/99-nfs-stress.conf`
if you run this regularly.

To exceed the per-IP ceiling, configure additional source IPs on
the test client and fan workers out across them with
`--source-ip`:

```sh
./scripts/krb5_ffv1_stress.sh --source-ip 10.1.1.5 \
    --server ... --clients 50 --nconnect 8 ... &
./scripts/krb5_ffv1_stress.sh --source-ip 10.1.1.6 \
    --server ... --clients 50 --nconnect 8 ... &
./scripts/krb5_ffv1_stress.sh --source-ip 10.1.1.7 \
    --server ... --clients 50 --nconnect 8 ... &
wait
```

Each `--source-ip` value gets its own per-IP ceiling, so three
local IPs lift the nconnect=8 wall from ~50 to ~150 concurrent
workers.

### The precondition (server side)

NFSv4 with krb5 maps a principal to a Unix uid **on the server**, so
for N distinct krb5 clients to appear as N distinct file owners the
**server's** sssd/idmap must resolve your realm's users into
distinct uids.  For AD, that's typically the server host AD-joined
with sssd configured `ldap_id_mapping = False` so it reads AD's
`uidNumber` / `gidNumber` attributes.  If that is not the case the
test still runs and each worker still verifies its own data, but
every file lands owned by the server's "nobody" uid -- you are no
longer stressing per-identity handling.

Treat the principals file as a credential: mode `0600`, never
commit it.

## Troubleshooting

**The run failed — where do I look?**

On failure the driver prints a line like:

```
nfs_krb5_multiclient: run failed -- reffsd work dir kept: /tmp/krb5mc_a1b2c3
```

That directory is **kept** (on success it is deleted). Inside it:

- `reffsd.log` — everything the test server printed. Start here.
  Look for `ERROR:`, `AddressSanitizer`, a crash backtrace, or
  authentication rejections.
- `reffsd.toml` — the server's generated configuration.
- `reffsd.trc` — the server's trace file.

Note: under `make -f Makefile.ci krb5` the test runs inside a
container that is removed when the command finishes, so the kept
directory disappears with it. To inspect it, reproduce the failure
by running the driver directly (see "Running the tool directly").

**Some clients failed but not all (e.g. `47/50`).**

A partial failure usually points at a concurrency bug in the server
— a race that only trips when many clients hit it at once. Re-run;
if the failing count varies between runs, that confirms a race.
Capture `reffsd.log` from a direct run and file it with the bug.

**All clients failed (`0/N`).**

Setup, not concurrency. Either the server never came up, or no
client could authenticate. `reffsd.log` will say which. A common
cause is a stale container image — rebuild with
`make -f Makefile.ci image`.

**It says SKIP under `make -f Makefile.ci krb5`.**

The container image lacks the Kerberos KDC. Rebuild it:
`make -f Makefile.ci image`.

**Server reported a sanitizer finding.**

The CI build compiles reffs with AddressSanitizer. If the server
hits a memory bug, it aborts; the driver detects the non-zero exit
and fails the run with a message like
`reffsd exited N (sanitizer finding or error)`. The details are in
`reffsd.log`. This is a real server bug — file it.

## What happens under the hood

Knowing the moving parts helps when reading a failure:

1. The driver starts a throwaway Kerberos KDC (realm `TEST.REFFS`)
   in a temporary directory.
2. It writes a small reffs server configuration whose only export
   requires Kerberos, then launches `reffsd` pointed at the KDC's
   service key.
3. It waits for the server to start listening.
4. It creates N Kerberos user accounts and obtains a ticket for
   each.
5. It forks N worker processes. Each worker picks up one ticket,
   connects to the server with Kerberos, writes a file, reads it
   back, checks the checksum, GETATTRs the file and checks its
   owner, and exits 0 (pass) or non-zero (fail).
6. It collects every worker's result, shuts the server down, tears
   the KDC down, and reports the tally.

In the default (embedded) mode everything is ephemeral — temporary
directories, a throwaway realm, a server on a non-standard port.
Nothing touches a real KDC or a production server, and nothing is
left behind on a successful run. External-KDC mode is the exception:
it uses the realm you point it at (the embedded KDC steps above are
replaced by the artifacts you supply), and still spawns its own
throwaway reffsd.

## `ec_demo burst` — Kerberos identmap stress generator

`nfs_krb5_multiclient` runs N forked workers, one process per
Kerberos identity.  That covers the "N distinct identities, each
its own client process" axis: per-worker `KRB5CCNAME`, per-worker
clientid, per-worker session.  It is the right tool for credential-
state coverage on a small N.

`ec_demo burst` is the other tool — a configurable wire-shape
generator that drives the server-side Kerberos identmap path at
scale.  Three independent axes:

| Axis | Flag | What it varies | Server-side load |
|---|---|---|---|
| Sessions | `--nsessions N` | N parallel mds_sessions (threaded by default; forked under `--ccache-dir`) | N EXCHANGE_ID + CREATE_SESSION pairs |
| Transports | `--nconnect M` | M TCP transports per session, kernel-style; transport 0 carries EXCHANGE_ID + CREATE_SESSION, transports 1..M-1 land via BIND_CONN_TO_SESSION | N × M RPCSEC_GSS contexts (M GSS_INIT per session) |
| Initiator identities | `--ccache-dir DIR` | per-worker `KRB5CCNAME` from a directory of pre-baked ccaches; rotation `ccaches[i % len]` | N distinct initiator principals → N distinct server-side identmap lookups |

These compose orthogonally.  Total wire transports for a `burst`
run is `nsessions × nconnect`; total distinct initiator identities
on the wire is `min(nsessions, ccache-count)` or 1 if `--ccache-dir`
is unset.  The customer load shape this harness is designed to
reproduce is 32 concurrent K8s pod mounts with `nconnect=8` — 256
parallel `gss_accept_sec_context` calls, each triggering a server-
side identmap upcall.  Set `--nsessions 32 --nconnect 8` to match.

### Running it — the common shapes

**Single-identity handshake burst** (threaded, same KRB5CCNAME on
every worker):

```sh
ec_demo burst --mds anvil.example.com --sec krb5 --nsessions 32
```

**Kernel-style multi-transport per session** (one identity, but
each session has M concurrent transports doing GSS_INIT):

```sh
ec_demo burst --mds anvil.example.com --sec krb5 \
              --nsessions 32 --nconnect 8
```

**Multi-identity, multi-transport** (K8s pod model — N pods, each
with its own pre-baked ccache, each mounting with nconnect=8):

```sh
ec_demo burst --mds anvil.example.com --sec krb5 \
              --nsessions 32 --nconnect 8 \
              --ccache-dir /var/run/krb5/pod-ccaches
```

`--ccache-dir` switches to forked-worker mode (each child has its
own envp and its own libkrb5 context) and decorates each worker's
NFSv4 clientowner with the worker index, so the server resolves N
distinct (clientowner, krb5 principal) tuples.  The directory
should contain one regular file per identity; whatever provisions
your pod ccaches in production is the right tool to provision
this directory in test.

**SPN rotation** (the target-service axis, orthogonal to the
above; useful when the server hosts multiple service principals):

```sh
ec_demo burst --mds anvil.example.com --sec krb5 --nsessions 32 \
              --spn-list nfs/h0,nfs/h1,nfs/h2,...,nfs/h31
```

### Reading the output

Aggregate counts in threaded mode:

```
ec_demo burst: opening 32 parallel mds_sessions to anvil.example.com (sec=krb5)
ec_demo burst: 32 passed, 0 failed in 187 ms total (handshake min=42 max=181 avg=98 ms)
```

Forked mode (when `--ccache-dir` is set) reports aggregate
pass/fail + wall-clock only; per-worker timing goes to each
child's stderr.

### Reading the failures

The central log in `mds_compound_send` surfaces both NFS4-layer
and RPC-layer failures symbolically.

**NFS4 layer** — failing op + symbolic `NFS4ERR_` name:

```
mds_compound_send: COMPOUND tag="exchange_id" op[0]=OP_EXCHANGE_ID(42) status=NFS4ERR_PERM(13)
ec_demo burst: worker 7 FAIL ret=-121 (Remote I/O error)
```

**RPC-AUTH layer** — `auth_stat` decoded against the RFC 5531
enum.  The customer "Auth Bogus Credentials (seal broken)"
symptom from the multi-mount stress lives in this group:

```
mds_compound_send: clnt_call returned rpc_stat=1 (RPC can't decode result) \
    re_status=5 auth_stat=RPCSEC_GSS_CTXPROBLEM(14) tag=exchange_id
ec_demo burst: worker 12 FAIL ret=-13 (Permission denied)
```

The `auth_stat` value names the wire-level failure mode directly:

| auth_stat | What the server is telling the client |
|---|---|
| `AUTH_REJECTEDCRED` | Client credential rejected (e.g. expired ticket past server's clock skew) |
| `AUTH_BADVERF` / `AUTH_REJECTEDVERF` | RPC verifier integrity check failed |
| `RPCSEC_GSS_CREDPROBLEM` | Client's GSS credential is stale; rpc.gssd should re-init |
| `RPCSEC_GSS_CTXPROBLEM` | Server-side GSS context broken (MIC mismatch, sequence window exceeded, identmap chain returned ENOENT in the middle of `nfs4_cred_auth_gss_construct_token`) — this is the canonical "seal broken" footprint |

### Comparison with `nfs_krb5_multiclient`

| Driver | Workers | Identities | Transports per worker | Use when |
|---|---|---|---|---|
| `nfs_krb5_multiclient` | forked | N distinct (default mode) | 1 (mount per process) | small-N credential-state coverage, per-pod identity, kernel-mount path |
| `ec_demo burst` | pthreads (default) or forked (`--ccache-dir`) | 1 or N (depending on `--ccache-dir`) | 1 or M (depending on `--nconnect`) | userspace handshake bursting at scale, axis-independent stressing, reproducing the K8s multi-mount load |

The two are complementary; a full credential + identmap stress
matrix runs `nfs_krb5_multiclient` for the per-process kernel-mount
shape and `ec_demo burst` for everything that needs scale or
axis-independent variation.

### What `burst` does NOT do

- **End-to-end I/O after the handshake.**  `burst` opens, runs the
  full EXCHANGE_ID + CREATE_SESSION (+ M-1 BIND_CONN_TO_SESSION if
  `--nconnect > 1`) sequence, then calls `mds_session_destroy`.
  The stress surface this harness is built for lives at handshake
  time; READ / WRITE coverage is the job of the other ec_demo
  subcommands.
- **`kinit` / keytab handling.**  `--ccache-dir` consumes
  pre-baked ccaches; the harness does not acquire credentials
  itself.  Whatever your production deployment uses to provision
  pod ccaches is the right tool for the test side too.

## See also

- `docs/security-test-tools.md` — `nfs_krb5_test`, the single-client
  Kerberos tester this multi-client driver is built on.
- `.claude/design/krb5-multiclient-test.md` — design rationale for
  the forked driver.
- `.claude/design/krb5-stress-multi-xprt.md` — design rationale for
  `ec_demo burst` and the `--spn` / `--spn-list` / `--nsessions` /
  `--nconnect` / `--ccache-dir` flag family.
