<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# er_demo — Atomic File Update via EXCHANGE_RANGE

`er_demo` demonstrates the atomic file update pattern enabled by the
`EXCHANGE_RANGE` NFSv4.2 operation (draft-haynes-nfsv4-swap).

The core idea: rather than overwriting a file in place (which exposes
readers to partial writes), clone the file, write the new content into
the clone, then atomically swap the clone and the original.  The server
performs the swap as a single indivisible operation — readers see either
the old content or the new content, never a mix.

## Build

`er_demo` is built as part of the normal reffs build:

```
mkdir build && cd build
../configure --enable-asan --enable-ubsan
make
```

The binary appears at `build/tools/er_demo`.

## Subcommands

```
er_demo put    --mds HOST --file NAME --data TEXT
er_demo get    --mds HOST --file NAME
er_demo update --mds HOST --file NAME --data TEXT
```

### put

Create or overwrite `NAME` on the server with the literal string `TEXT`.

```bash
er_demo put --mds 192.168.2.128 --file greeting.txt --data "hello world"
```

### get

Read `NAME` from the server and print it to stdout.

```bash
er_demo get --mds 192.168.2.128 --file greeting.txt
```

### update

Atomically replace `NAME` with `TEXT` using the CLONE + EXCHANGE_RANGE
workflow:

1. Open the target file.
2. Create and open a temporary file named `<NAME>.__er_clone__`.
3. Copy the entire target into the clone (`CLONE`, count=0 = full file).
4. Write the new content (`TEXT`) into the clone.
5. Atomically swap the full byte range of target and clone
   (`EXCHANGE_RANGE`).  The target now holds the new content; the clone
   holds the old content.
6. Close both files.
7. Delete the clone.

```bash
er_demo update --mds 192.168.2.128 --file greeting.txt --data "goodbye world"
```

The EXCHANGE\_RANGE NFS compound is:

```
SEQUENCE + PUTFH(clone) + SAVEFH + PUTFH(target) + EXCHANGE_RANGE
```

The clone becomes `SAVED_FH` (source); the target becomes `CURRENT_FH`
(destination).  After the swap, target holds the new content.

## Options

| Option | Default | Description |
|--------|---------|-------------|
| `--mds HOST` | required | MDS hostname or IP address |
| `--file NAME` | required | Filename on the server (root of MDS export) |
| `--data TEXT` | required for put/update | Content to write |
| `--id OWNER` | `er_demo` | Client owner string |

## Self-inverse property

`EXCHANGE_RANGE` is self-inverse.  Running `update` twice with the same
`--data` returns the file to its previous state:

```bash
er_demo put    --mds HOST --file f.txt --data "version 1"
er_demo update --mds HOST --file f.txt --data "version 2"
er_demo get    --mds HOST --file f.txt   # prints: version 2
er_demo update --mds HOST --file f.txt --data "version 2"
er_demo get    --mds HOST --file f.txt   # prints: version 1
```

This follows from RFC 8881 S5.11.1: `change_info4.before` lets a client
detect whether a self-inverse operation ran an even or odd number of
times during a window.

## Error behaviour

If any step of `update` fails after the clone file is created, the tool
removes the clone before returning the error.  On success, the clone is
always removed.  The target file is never left in a partially-updated
state because the swap is atomic.

If `update` fails before `EXCHANGE_RANGE` is sent, the target is
unchanged.  If `EXCHANGE_RANGE` itself fails, both files retain their
pre-swap content.

## See also

- `tools/er_demo.c` — source
- `lib/nfs4/client/mds_file.c` — `mds_file_clone()`, `mds_file_exchange_range()`
- `docs/ec_demo.md` — erasure-coding demo tool
- `lib/nfs4/server/copy.c` — server-side CLONE and EXCHANGE_RANGE handlers
- draft-haynes-nfsv4-swap — protocol specification
