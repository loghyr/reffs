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

## See also

- `docs/security-test-tools.md` — `nfs_krb5_test`, the single-client
  Kerberos tester this multi-client driver is built on.
- `.claude/design/krb5-multiclient-test.md` — the design rationale,
  for developers extending the test.
