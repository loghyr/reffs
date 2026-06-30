<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Erasure Coding Design

## Encoding Architecture

The encoding interface (`lib/include/reffs/ec.h`) is designed for
swappability.  Reed-Solomon is the proof-of-concept encoding to
demonstrate that the pNFS architecture is encoding-agnostic.

```c
struct ec_encoding {
    const char *ec_name;
    int ec_k;           /* data shards */
    int ec_m;           /* parity shards */
    int (*ec_encode)(...);
    int (*ec_decode)(...);
    void *ec_private;
};
```

Any encoding that satisfies this interface can be plugged in.

## Reed-Solomon Implementation

Clean-room GF(2^8) Vandermonde RS.  See `lib/ec/`.

- No external dependencies
- No SIMD (patent-safe — see standards.md)
- Correctness over speed
- References only pre-2000 textbook sources

## Demo Client

Minimal userspace tool (not a general-purpose NFS client):

1. LAYOUTGET from MDS → parse Flex Files layout
2. RS-encode write data → distribute data+parity to DSes via NFSv3
3. Read from DSes → RS-decode / reconstruct → verify

The client talks NFSv4.2 to the MDS (just enough for OPEN, LAYOUTGET,
LAYOUTRETURN, CLOSE) and NFSv3 to the DSes (READ, WRITE).

## Patent Rules

See `.claude/standards.md` for the full patent-safe implementation
rules.  Summary: no Plank/Jerasure/GF-Complete, no SIMD GF tricks,
no ISA-L GF internals.  US 8,683,296 (StreamScale) is the specific
patent to avoid.
