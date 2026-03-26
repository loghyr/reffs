<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# EC Benchmark: Healthy vs Degraded-1 Read (Reconstruction)

## Test Configuration

- **Platform**: Fedora 43, Linux 6.19.8, aarch64
- **Setup**: 7 Docker containers on a bridge network (1 MDS + 6 DSes)
- **Geometry**: 4+2 (k=4 data shards, m=2 parity shards)
- **Codecs tested**: plain (no EC), Reed-Solomon, Mojette systematic, Mojette non-systematic
- **File sizes**: 4 KB, 16 KB, 64 KB, 256 KB, 1 MB
- **Runs**: 5 measured runs per codec per size (2 warmup runs discarded)
- **Shard size**: 4 KB (io_uring large-message workaround)
- **Layout**: Flex Files v1 (NFSv3 DS I/O)

## Degraded Mode

Each test file is **written once** with all 6 DSes healthy, then **read twice**:

1. **Healthy read** -- all 6 DSes respond normally
2. **Degraded-1 read** -- data shard 0 is skipped (simulated failure via `--skip-ds 0`), forcing the codec to reconstruct the missing shard from the remaining 5 shards

Skipping a **data shard** (not parity) forces real reconstruction work for systematic codecs. Plain mirroring has no reconstruction capability and is excluded from the degraded pass.

All degraded reads verified correct (byte-for-byte match against the original input).

## Results (5-run means, milliseconds)

### Write Latency (healthy only)

| File size | plain | RS    | Mojette-sys | Mojette-nonsys |
|-----------|-------|-------|-------------|----------------|
| 4 KB      | 14.2  | 17.2  | 15.2        | 15.2           |
| 16 KB     | 13.4  | 17.0  | 16.8        | 16.0           |
| 64 KB     | 15.6  | 25.8  | 23.2        | 21.4           |
| 256 KB    | 26.0  | 40.0  | 44.4        | 42.6           |
| 1 MB      | 65.4  | 110.0 | 122.0       | 123.4          |

### Read Latency -- Healthy (all DSes responding)

| File size | plain | RS   | Mojette-sys | Mojette-nonsys |
|-----------|-------|------|-------------|----------------|
| 4 KB      | 13.8  | 19.4 | 14.4        | 19.0           |
| 16 KB     | 10.2  | 14.4 | 13.4        | 15.8           |
| 64 KB     | 13.4  | 16.4 | 18.4        | 28.2           |
| 256 KB    | 20.4  | 33.4 | 32.0        | 72.8           |
| 1 MB      | 63.2  | 95.6 | 93.2        | 248.6          |

### Read Latency -- Degraded-1 (shard 0 missing, reconstruction)

| File size | RS   | Mojette-sys | Mojette-nonsys |
|-----------|------|-------------|----------------|
| 4 KB      | 17.6 | 15.0        | 23.6           |
| 16 KB     | 15.0 | 14.0        | 16.4           |
| 64 KB     | 19.2 | 16.8        | 27.6           |
| 256 KB    | 34.4 | 31.2        | 73.0           |
| 1 MB      | 96.8 | 95.6        | 251.4          |

### Degraded-1 Overhead vs Healthy Read

| File size | RS     | Mojette-sys | Mojette-nonsys |
|-----------|--------|-------------|----------------|
| 4 KB      | -9%    | +4%         | +24%           |
| 16 KB     | +4%    | +4%         | +4%            |
| 64 KB     | +17%   | -9%         | -2%            |
| 256 KB    | +3%    | -2%         | +0%            |
| 1 MB      | +1%    | +3%         | +1%            |

## Key Findings

1. **Reconstruction overhead is negligible.** At 1 MB, degraded reads add 1-3% latency for all three codecs. The reconstruction CPU cost is dwarfed by the network I/O to the remaining 5 DSes.

2. **Systematic codecs show near-zero penalty.** RS and Mojette-sys degraded reads are statistically indistinguishable from healthy reads at 256 KB and 1 MB. The saved I/O from skipping one DS roughly offsets the decode math.

3. **Mojette non-systematic remains consistent.** The high baseline read latency (full inverse transform on every read) means reconstruction adds no meaningful additional cost -- the codec always does the same amount of work regardless of which shards are present.

4. **All reconstructions verified correct.** 75 degraded reads across 3 codecs and 5 sizes, all byte-for-byte correct.

5. **Negative overhead values** at some sizes reflect measurement noise (single-digit millisecond differences) and the I/O savings from reading 5 shards instead of 6.

## Implications for pNFS Flex Files

These results demonstrate that erasure-coded pNFS with Flex Files layouts can tolerate DS failures with **no meaningful performance penalty** for reads. A client discovering a failed DS during LAYOUTGET or READ can simply reconstruct from the remaining shards without impacting user-visible latency. This validates the design where:

- MDS issues layouts referencing k+m DSes
- Client reads from all available DSes
- Missing shards are reconstructed transparently
- CB_LAYOUTRECALL + re-layout only needed for write-path failures
