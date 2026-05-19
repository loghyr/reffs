<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# krb5 FFv1 Stress Driver (external server, ec_demo)

## What and why

A multi-client Kerberos stress driver for any already-running
NFSv4.2 server that issues Flex Files v1 layouts.

Distinct from `nfs_krb5_multiclient`: that driver owns the entire
lifecycle (it spawns reffsd, runs an embedded mini-KDC, and each
worker does plain MDS write/read via `mds_file_*`).  This driver
assumes the NFSv4.2 server and the KDC already exist, takes N krb5
identities the operator provides, and drives the **FFv1 layout
path** -- `LAYOUTGET` followed by direct DS I/O -- because the
intended target is a production NFSv4.2 server that speaks FFv1
only and does not proxy inbound I/O cleanly, so plain MDS I/O is not
a fair stress of it.

The work is therefore a thin shell driver, not a new tool.
`ec_demo` (`tools/ec_demo`) already provides every per-client piece:

| Piece | Source |
|-------|--------|
| `--sec krb5` (wires `mds_session_create_sec`) | `tools/ec_demo.c:112-126` |
| `--id ID` (per-instance client owner) | usage at line 729 |
| Multi-component path opens (PUTROOTFH + LOOKUP\* + OPEN) | `lib/nfs4/client/mds_file.c` |
| The FFv1 layout workload (LAYOUTGET, DS I/O) | `ec_demo write` / `read` / `verify` |

## Shape

`scripts/krb5_ffv1_stress.sh`:

    krb5_ffv1_stress.sh --server <host[:port]>
                        --path <dir-on-server>
                        --clients <N>
                        --principals <file>
                        [--ec-demo <path>] [--input <file>]
                        [--size <bytes>] [--k <K> --m <M>]
                        [--codec rs|mojette-sys|mojette-nonsys|stripe]
                        [--sec krb5|krb5i|krb5p]

Flow:

1.  Validate args; locate `ec_demo` (default
    `./build/tools/ec_demo`).
2.  Read the principals file: one `<principal> <password>` pair per
    line, blank lines and `#` comments skipped, principals fully
    qualified (`user@REALM`).  Need at least N entries.
3.  `mktemp -d` a private run directory.  `kinit` each of the first
    N principals into `<run-dir>/ccaches/cc_<i>` (password on
    stdin; never on the command line).  Fail fast on any kinit
    error.
4.  Either use the operator's `--input` file, or generate
    `<run-dir>/input.bin` of `--size` bytes from `/dev/urandom`
    (default 10 MB).
5.  Fork N workers, each in a subshell:
    *   `KRB5CCNAME=<run-dir>/ccaches/cc_<i>`
    *   `ec_demo write --mds <server>
        --file <path>/krb5stress_<i> --input <input>
        --sec <sec> --layout v1 --codec <codec> --k <K> --m <M>
        --id krb5stress_<i>`
    *   then `exec ec_demo verify ...` (same args) to read back
        and compare.

    `ec_demo verify` reads + compares only -- it does NOT write
    first.  So the worker writes the file in one invocation and
    verifies the readback in a second invocation, both with the
    same per-worker krb5 ccache, which together exercise the full
    FFv1 round-trip under one identity.

    Each worker's stdout / stderr go to
    `<run-dir>/logs/worker_<i>.log`.
6.  `wait` every worker; tally pass / fail.  On failure dump each
    failing worker's last 40 log lines to stderr (the run dir is
    cleaned on success, so otherwise the logs vanish).
7.  Print `PASS N/N` or `FAIL k/N (m failed)`; exit 0 / 1.
8.  Cleanup: remove the run dir on success; keep it on failure (and
    print the path) so the operator can inspect.

## What the operator provides

| Item | What |
|------|------|
| NFSv4.2 server speaking FFv1 | the server under test |
| A directory path on that server | the workers write into `<path>/krb5stress_<i>` |
| A reachable krb5 realm | the test host has working `kinit` against it |
| `--principals` file | one `<principal> <password>` per line, N test users with known passwords |
| `ec_demo` build | the reffs client binary, by default at `./build/tools/ec_demo` |

The test host must be configured so the krb5 principals resolve to
distinct uids on it -- typically sssd + `ldap_id_mapping = False` for
an AD realm.  Otherwise the server side sees every worker as
`nobody`; that precondition is unchanged from
`docs/krb5-multiclient-testing.md`.

## Out of scope

- Provisioning the server, the KDC, or the principals.
- Renewing tickets mid-run -- workers must complete inside the TGT
  lifetime.
- Asserting per-file ownership at the server -- ec_demo's exit code
  covers data integrity (write + read + compare).  An ownership
  assertion needs a separately-authenticated stat from a known
  identity; deferred.
- A cross-identity check (A writes, B reads).

## Key files

| File | Change |
|------|--------|
| `scripts/krb5_ffv1_stress.sh` | NEW |
| `docs/krb5-multiclient-testing.md` | add a "Stressing an external FFv1 server" section |
| `.claude/design/krb5-ffv1-stress.md` | this doc |
