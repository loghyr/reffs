<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# ec_demo — Erasure-Coding Demo Client

`ec_demo` is a minimal NFSv4.2 client that demonstrates pNFS Flex Files
layouts with erasure-coded I/O.  It talks NFSv4.2 to the MDS (for
session setup, OPEN, LAYOUTGET, LAYOUTRETURN, CLOSE) and NFSv3 or
NFSv4.2 CHUNK ops to the data servers (DS) for the actual data path.

## Build

`ec_demo` is built as part of the normal reffs build:

```
mkdir build && cd build
../configure --enable-asan --enable-ubsan
make
```

The binary appears at `build/tools/ec_demo`.

## Subcommands

### Plain I/O (no erasure coding)

These operate directly on the MDS with no layout.  The MDS stores the
data; no DS involvement.

```
ec_demo put   --mds HOST --file NAME --input FILE
ec_demo get   --mds HOST --file NAME --output FILE [--size N]
ec_demo check --mds HOST --file NAME --input FILE
```

`put` writes the local `FILE` to the server as `NAME`.
`get` reads `NAME` from the server and writes it to the local `FILE`.
`check` reads `NAME` from the server and verifies it byte-for-byte
against the local `FILE`.

### Erasure-coded I/O

These use pNFS Flex Files: the MDS issues a layout describing k+m data
server targets; the client stripes encoded data across them.

```
ec_demo write  --mds HOST --file NAME --input FILE  [--k K] [--m M] [--codec TYPE] [--layout TYPE]
ec_demo read   --mds HOST --file NAME --output FILE [--k K] [--m M] [--size N]    [--codec TYPE] [--layout TYPE] [--skip-ds LIST]
ec_demo verify --mds HOST --file NAME --input FILE  [--k K] [--m M] [--codec TYPE] [--layout TYPE] [--skip-ds LIST]
```

`write` encodes the local `FILE` with the chosen codec and writes the
shards to the data servers.
`read` reads shards from the data servers, decodes, and writes the
result to the local `FILE`.
`verify` reads and decodes, then compares the result byte-for-byte
against the local `FILE`.  Use `--skip-ds` to test degraded-mode
reconstruction.

### Identity commands

```
ec_demo getowner  --mds HOST --file NAME
ec_demo setowner  --mds HOST --file NAME --input OWNER
```

`getowner` prints the NFSv4 `owner` and `owner_group` attributes.
`setowner` sets the `owner` attribute to the string given by `--input`
(format: `user@domain`).

## Options

| Option | Default | Description |
|--------|---------|-------------|
| `--mds HOST` | required | MDS hostname or IP address |
| `--file NAME` | required | Filename on the server (root of MDS export) |
| `--input FILE` | — | Local file to read from (put/write/verify/setowner) |
| `--output FILE` | — | Local file to write to (get/read) |
| `--k K` | 4 | Number of data shards |
| `--m M` | 2 | Number of parity shards |
| `--size N` | 16777216 | Expected read size in bytes |
| `--codec TYPE` | `rs` | Codec: `rs`, `mojette-sys`, `mojette-nonsys`, `stripe` |
| `--layout TYPE` | `v1` | Layout type: `v1` (NFSv3 DS), `v2` (CHUNK ops) |
| `--skip-ds LIST` | — | Comma-separated DS indices to skip (degraded read) |
| `--force-scalar` | off | Disable SIMD in Mojette transforms |
| `--id ID` | PID | Client owner string; must be unique per concurrent instance |
| `--sec FLAVOR` | `sys` | RPC security flavor: `sys`, `krb5`, `krb5i`, `krb5p` |

## Codecs

| Codec | Description |
|-------|-------------|
| `rs` | Reed-Solomon (Vandermonde, GF(2^8), scalar) |
| `mojette-sys` | Mojette systematic (best read latency) |
| `mojette-nonsys` | Mojette non-systematic (slow reconstruction, not recommended for reads) |
| `stripe` | Plain stripe with no parity (`--m 0`); no reconstruction |

Recommended for interactive workloads: `mojette-sys` with 8+2
(see `.claude/goals.md` benchmark findings).

## Layout types

`v1` uses the standard pNFS Flex Files v1 layout: the client does
direct NFSv3 READ/WRITE to the data servers.

`v2` uses Flex Files v2: the client sends CHUNK\_WRITE / CHUNK\_READ /
CHUNK\_FINALIZE / CHUNK\_COMMIT to NFSv4.2 data servers.  Each chunk
write is CRC32-verified and persisted atomically on the DS.

## Examples

### Write and read back a 1 MB file with Reed-Solomon 4+2

```bash
dd if=/dev/urandom of=/tmp/test.bin bs=1M count=1

ec_demo write --mds 192.168.2.128 --file test.bin \
              --input /tmp/test.bin --k 4 --m 2 --codec rs

ec_demo read  --mds 192.168.2.128 --file test.bin \
              --output /tmp/out.bin --k 4 --m 2 --codec rs \
              --size 1048576

diff /tmp/test.bin /tmp/out.bin
```

### Degraded read (skip DS 1)

```bash
ec_demo read --mds 192.168.2.128 --file test.bin \
             --output /tmp/out.bin --k 4 --m 2 \
             --skip-ds 1 --size 1048576
```

### Flex Files v2 with CHUNK ops

```bash
ec_demo write --mds 192.168.2.128 --file test.bin \
              --input /tmp/test.bin --k 4 --m 2 \
              --codec rs --layout v2

ec_demo read  --mds 192.168.2.128 --file test.bin \
              --output /tmp/out.bin --k 4 --m 2 \
              --codec rs --layout v2 --size 1048576
```

### Plain put/get (no layouts, no EC)

```bash
ec_demo put --mds 192.168.2.128 --file hello.txt --input /etc/hostname
ec_demo get --mds 192.168.2.128 --file hello.txt --output /tmp/hello.txt
ec_demo check --mds 192.168.2.128 --file hello.txt --input /etc/hostname
```

### Run two concurrent clients with distinct IDs

```bash
ec_demo write --mds HOST --file a.bin --input /tmp/a.bin --id client-1 &
ec_demo write --mds HOST --file b.bin --input /tmp/b.bin --id client-2 &
wait
```

## See also

- `tools/ec_demo.c` — source
- `lib/nfs4/client/` — client library (`mds_*`, `ec_*`, `ds_*` functions)
- `docs/er_demo.md` — atomic file update tool using EXCHANGE_RANGE
- `.claude/goals.md` — benchmark results and codec comparison
- `.claude/design/mds.md` — MDS architecture and Flex Files design
