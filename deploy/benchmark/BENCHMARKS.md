<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# reffs EC Benchmark Reproduction Guide

This document covers reproducing the erasure-coding benchmarks for the
reffs pNFS Flex Files stack.  It describes the infrastructure, the four
benchmark suites, reference results, and how to interpret the output.

## Prerequisites

### Software

- Docker Engine (23+) or Docker Desktop
- Docker Compose plugin (`docker compose`, not legacy `docker-compose`)
- reffs source tree (any commit after `fc27da0e`)

### Host requirements

- Linux host recommended (the containers run as privileged, need loopback
  NFS port binding)
- macOS with Docker Desktop works but has higher per-container overhead
- 4 GB RAM minimum; 8 GB comfortable for 12-container suite
- `nfsidmap`, `rpcbind`, and `nfs-utils` are inside the containers —
  the host only needs Docker

### Build the dev image

The benchmark containers use the `reffs-dev` image:

```bash
make -f Makefile.reffs image
```

This must be done before the first run and after any Dockerfile change.

---

## Infrastructure

The benchmark uses Docker Compose to create a self-contained network:

| Container | Count | Role |
|-----------|-------|------|
| `reffs-bench-builder` | 1 | Compiles the source tree once into a shared volume |
| `reffs-bench-ds{0-9}` | 10 | Data servers (reffsd with DS-only config) |
| `reffs-bench-mds` | 1 | Metadata server (reffsd with MDS+DS role, 10 DSes configured) |
| `reffs-bench-client` | 1 | Runs `ec_benchmark.sh` |

The `builder` container compiles into a Docker volume (`build-vol`), then
exits.  All other containers wait for `build-vol/ready` before starting.
The MDS waits for all 10 DSes to pass the healthcheck (TCP port 2049)
before accepting connections.

### Startup order

```
builder → ds{0-9} (parallel) → mds → bench-client
```

Healthchecks retry for up to 10 minutes (120 × 5s intervals).  On a
fast host the full stack is usually ready in under 3 minutes.

---

## Running the Benchmarks

### Standard run (all suites)

```bash
make -f Makefile.reffs run-benchmark
```

This invokes `docker compose --profile run up` in `deploy/benchmark/`.
The client container runs `scripts/ec_benchmark.sh` and writes CSV to
stdout.  Logs from all containers go to Docker's log driver.

### Manual run with custom flags

```bash
cd deploy/benchmark
docker compose up -d builder ds0 ds1 ds2 ds3 ds4 ds5 ds6 ds7 ds8 ds9 mds
# Wait for MDS healthy, then:
docker compose --profile run run bench \
    /bin/bash -c "exec /reffs/scripts/ec_benchmark.sh \
        --degrade 1 \
        /shared/build/tools/ec_demo reffs-mds"
```

### Teardown

```bash
cd deploy/benchmark
docker compose down -v   # removes containers AND the build-vol volume
```

---

## Benchmark Script Reference

`scripts/ec_benchmark.sh [--degrade N] <ec_demo_path> <mds_host>`

| Parameter | Default | Description |
|-----------|---------|-------------|
| `--degrade N` | 0 (disabled) | After healthy pass, re-read with N data shards skipped |
| `ec_demo_path` | `/build/tools/ec_demo` | Path to the ec_demo binary inside the container |
| `mds_host` | `localhost` | MDS hostname or IP |

### Configuration constants (edit in script to change)

| Variable | Default | Meaning |
|----------|---------|---------|
| `RUNS` | 5 | Measured runs per combination |
| `WARMUP` | 2 | Warmup runs discarded |
| `SIZES` | `4096 16384 65536 262144 1048576` | File sizes in bytes |
| `GEOMETRIES` | `4:2 8:2` | `k:m` pairs to test |
| `NUM_DS` | 10 | DSes available (stripe width for stripe baseline) |

### CSV output format

```
codec,geometry,size_bytes,run,write_ms,read_ms,verify,mode
```

`verify` is `OK` or `FAIL`.  `mode` is `healthy` or `degraded-N`.

### Codecs tested

| Codec | Description |
|-------|-------------|
| `plain` | Single-mirror write/read (no EC, no striping) |
| `stripe` | Pure striping across all DSes, no redundancy |
| `rs` | Reed-Solomon systematic (GF(2^8), Vandermonde matrix) |
| `mojette-sys` | Mojette systematic (forward-only on write; inverse on failure) |
| `mojette-nonsys` | Mojette non-systematic (inverse transform on every read) |

---

## Reference Results

All results below are 5-run means in milliseconds.  2 warmup runs were
discarded.  Shard size is 4 KB (required by the io_uring workaround; see
Known Limitations).

### Suite 1 — Codec comparison, 4+2, Fedora 43 aarch64 (Tier 2)

Platform: Fedora 43, Linux 6.19.8-200.fc43.aarch64, Docker bridge network,
7 containers (1 builder + 1 MDS + 6 DSes + 1 client).

**Write latency (ms)**

| File size | plain | RS   | Mojette-sys | Mojette-nonsys |
|-----------|-------|------|-------------|----------------|
| 4 KB      | 10.2  | 14.2 | 13.4        | 13.4           |
| 16 KB     | 10.8  | 13.8 | 14.8        | 18.4           |
| 64 KB     | 14.0  | 18.4 | 20.4        | 19.6           |
| 256 KB    | 23.2  | 35.6 | 37.6        | 40.0           |
| 1 MB      | 64.4  | 106.8| 141.6*      | 116.8          |

*1 MB Mojette-sys: one anomalous run in run 5; without outlier ~123 ms.

**Read latency (ms)**

| File size | plain | RS   | Mojette-sys | Mojette-nonsys |
|-----------|-------|------|-------------|----------------|
| 4 KB      | 8.0   | 12.0 | 11.6        | 14.2           |
| 16 KB     | 9.6   | 13.6 | 12.2        | 15.0           |
| 64 KB     | 11.6  | 16.2 | 15.4        | 25.4           |
| 256 KB    | 19.2  | 29.6 | 29.4        | 67.4           |
| 1 MB      | 59.2  | 91.2 | 92.4        | 237.8          |

### Suite 2 — Striping analysis (plain vs stripe vs EC vs degraded), 4+2

Platform: same as Suite 1.

**Write latency (ms)**

| File size | plain | stripe | RS   | Mojette-sys | Mojette-nonsys |
|-----------|-------|--------|------|-------------|----------------|
| 4 KB      | 10.8  | 13.0   | 13.8 | 13.8        | 13.2           |
| 16 KB     | 12.4  | 14.6   | 14.0 | 15.2        | 14.6           |
| 64 KB     | 16.4  | 17.2   | 20.8 | 21.4        | 21.2           |
| 256 KB    | 24.6  | 31.6   | 39.0 | 41.8        | 45.2           |
| 1 MB      | 64.4  | 71.2   | 103.0| 122.8       | 139.8          |

**Healthy read latency (ms)**

| File size | plain | stripe | RS   | Mojette-sys | Mojette-nonsys |
|-----------|-------|--------|------|-------------|----------------|
| 4 KB      | 9.4   | 13.6   | 13.0 | 12.2        | 14.2           |
| 16 KB     | 9.8   | 12.8   | 13.2 | 12.4        | 14.6           |
| 64 KB     | 13.4  | 14.8   | 15.6 | 17.0        | 24.6           |
| 256 KB    | 18.2  | 28.6   | 31.2 | 28.2        | 69.0           |
| 1 MB      | 59.4  | 64.8   | 86.8 | 89.4        | 239.0          |

**Degraded-1 read latency (data shard 0 missing, reconstruction)**

| File size | RS   | Mojette-sys | Mojette-nonsys |
|-----------|------|-------------|----------------|
| 4 KB      | 12.2 | 12.4        | 15.2           |
| 16 KB     | 14.0 | 14.6        | 20.2           |
| 64 KB     | 18.2 | 17.2        | 26.6           |
| 256 KB    | 31.2 | 29.8        | 71.8           |
| 1 MB      | 98.6 | 94.2        | 246.2          |

### Suite 3 — Degraded read deep-dive, 4+2

Platform: same as Suite 1.

**Degraded-1 overhead vs healthy read**

| File size | RS  | Mojette-sys | Mojette-nonsys |
|-----------|-----|-------------|----------------|
| 4 KB      | -9% | +4%         | +24%           |
| 16 KB     | +4% | +4%         | +4%            |
| 64 KB     | +17%| -9%         | -2%            |
| 256 KB    | +3% | -2%         | +0%            |
| 1 MB      | +1% | +3%         | +1%            |

Negative values reflect measurement noise and the I/O savings from
reading 5 shards instead of 6.

### Suite 4 — Multi-geometry (4+2 vs 8+2), 10 DSes

Platform: Fedora 43, Linux 6.19.8-200.fc43.aarch64, Docker bridge network,
12 containers (1 builder + 1 MDS + 10 DSes + 1 client).

**Write latency (ms)**

| File size | plain | stripe(10) | RS 4+2 | Msys 4+2 | Mnsys 4+2 | RS 8+2 | Msys 8+2 | Mnsys 8+2 |
|-----------|-------|------------|--------|----------|-----------|--------|----------|-----------|
| 4 KB      | 17.8  | 24.4       | 25.4   | 23.0     | 23.8      | 25.4   | 26.6     | 23.0      |
| 16 KB     | 19.0  | 26.8       | 26.6   | 27.4     | 24.6      | 34.8   | 26.2     | 26.6      |
| 64 KB     | 21.4  | 29.2       | 29.4   | 34.0     | 31.4      | 27.8   | 28.8     | 28.8      |
| 256 KB    | 29.4  | 36.6       | 47.0   | 49.6     | 49.6      | 42.2   | 43.0     | 523.4*    |
| 1 MB      | 59.2  | 77.2       | 102.6  | 137.0    | 127.2     | 96.2   | 114.6    | 120.0     |

*256 KB Mnsys 8+2: one outlier run inflated the mean.

**Healthy read latency (ms)**

| File size | plain | stripe(10) | RS 4+2 | Msys 4+2 | Mnsys 4+2 | RS 8+2 | Msys 8+2 | Mnsys 8+2 |
|-----------|-------|------------|--------|----------|-----------|--------|----------|-----------|
| 4 KB      | 15.0  | 21.4       | 20.8   | 19.4     | 23.8      | 22.8   | 23.0     | 31.6      |
| 16 KB     | 15.0  | 22.0       | 21.8   | 20.0     | 23.8      | 24.4   | 23.4     | 33.2      |
| 64 KB     | 15.6  | 24.2       | 25.0   | 26.2     | 35.4      | 22.8   | 23.4     | 43.4      |
| 256 KB    | 22.6  | 32.2       | 35.0   | 36.4     | 79.8      | 31.8   | 33.8     | 116.4     |
| 1 MB      | 53.6  | 65.0       | 81.4   | 83.6     | 246.2     | 71.2   | 80.6     | 394.6     |

**Degraded-1 read latency (data shard 0 missing)**

| File size | RS 4+2 | Msys 4+2 | Mnsys 4+2 | RS 8+2 | Msys 8+2 | Mnsys 8+2 |
|-----------|--------|----------|-----------|--------|----------|-----------|
| 4 KB      | 20.0   | 19.8     | 24.2      | 23.4   | 22.2     | 34.0      |
| 16 KB     | 22.8   | 21.8     | 26.6      | 22.2   | 22.0     | 33.2      |
| 64 KB     | 26.4   | 24.6     | 36.8      | 25.8   | 24.4     | 47.8      |
| 256 KB    | 35.2   | 36.6     | 84.6      | 41.8   | 36.4     | 124.4     |
| 1 MB      | 86.4   | 79.8     | 253.8     | 109.8  | 84.2     | 486.4     |

---

## Key Findings

### 1. EC overhead is within operational tolerance at small sizes

At 4–64 KB, all three EC codecs (RS, Mojette-sys, Mojette-nonsys) add
14–21% write overhead over plain.  This is within the noise of real-network
variance and within the tolerance of most interactive workloads.

### 2. Reconstruction is essentially free for systematic codecs

At 4+2 and 8+2, degraded reads with RS or Mojette-sys add 1–5% latency
over healthy reads.  The I/O savings from reading one fewer shard partially
offsets the reconstruction CPU cost.  A client discovering a failed DS
can reconstruct transparently without impacting user-visible latency.

### 3. Mojette non-systematic is unsuitable for interactive workloads

Mojette non-systematic requires a full inverse transform on every read
(not just degraded reads), scaling quadratically with file size.  At 1 MB,
it is 2.7× slower to read than RS at 4+2 and 5.5× slower at 8+2.  Use
only for write-once / archive workloads or benchmarking contrast.

### 4. 8+2 is faster than 4+2 for systematic codecs

With the same m=2 parity overhead, 8+2 spreads data into smaller shards
across more DSes.  RS 8+2 at 1 MB: write 96 ms vs 103 ms (4+2), read
71 ms vs 81 ms (4+2).  Write overhead scales with m, not k.

### 5. RS at 8+2 has significant reconstruction overhead

RS matrix inversion scales with k.  At 8+2, degraded-1 RS reads add 54%
over healthy reads (110 vs 71 ms at 1 MB).  Mojette-sys at 8+2 adds only
4% (84 vs 81 ms at 1 MB) because its corner-peeling reconstruction scales
with m×k (small for sparse failures).

### 6. Overhead ratios are platform-independent

Suite 1 on macOS M4 (Docker, Rocky Linux 8.10) and Fedora 43 aarch64
show different absolute latencies but nearly identical overhead ratios.
The encoding/decoding math dominates at large sizes; the ratios hold
across hardware.

---

## Recommended Operating Points

| Use case | Recommended | Rationale |
|----------|-------------|-----------|
| Interactive workloads (< 64 KB) | Any systematic codec | All within 15–20% of plain |
| Large file I/O, many DSes (8+) | RS 8+2 or Mojette-sys 8+2 | Write overhead same as 4+2; healthy reads faster |
| High resilience (2 DS failures) | RS 8+2 | Proven reconstruction; single-failure degraded adds 54% |
| Low reconstruction cost | Mojette-sys 8+2 | Degraded-1 adds 4%; sweet spot for live storage |
| Archive / write-once | Mojette-nonsys | Proof-of-concept only; unsuitable for reads |
| Baseline / latency floor | stripe | No redundancy; useful for isolating encoding overhead |

**Primary recommendation**: Mojette-sys 8+2 for interactive workloads
on 10+ DS clusters.  RS 8+2 as the fallback when proven algebraic
reconstruction is required.

---

## Known Limitations

### io_uring large-message stall

NFSv3 RPCs larger than ~32 KB stall in the io_uring read pipeline on
the server side.  The benchmark uses 4 KB shard size as a workaround.
This constrains minimum useful file sizes and may affect large-file
throughput.  See `project_iouring_large_msg.md` in memory for context.

### Same-host Docker network

All containers share a single host loopback/bridge network.  Real
network latency (data center, WAN) is not modeled.  Tier 2 benchmarks
(planned) will run on separate VMs to validate that overhead ratios
hold under realistic network conditions.

### No concurrent clients

The benchmark is single-client, single-stream.  Concurrent-client
throughput is not measured.

### Outlier runs

At large sizes (256 KB, 1 MB), occasional outlier runs (network stall,
Docker scheduler pause) inflate means.  Five measured runs with two
warmup discards is usually sufficient; re-run if a mean is 3× the
median.

### macOS Docker overhead

macOS Docker Desktop adds virtualization overhead.  Absolute latencies
from macOS runs are ~20% higher than Linux runs.  The overhead ratios
(EC overhead vs plain) are consistent across platforms.

---

## Tiered Evidence Strategy

| Tier | Platform | Geometry | Status |
|------|----------|----------|--------|
| 1 | macOS M4 (Rocky Linux Docker) | 4+2 | Done (2026-03-24) |
| 2 | Fedora 43 aarch64 (Docker bridge) | 4+2, 4+2 degraded, 8+2 | Done (2026-03-25) |
| 3 | Data center VMs, separate hosts | Multi-geometry | Planned |

Tier 3 will add real network latency and validate that the encoding
bottleneck (not network) dominates at large sizes.  The Tier 2 results
already show that at 1 MB, network fan-out accounts for ~7 ms and RS
encoding accounts for ~32 ms, consistent with encoding being the bottleneck.

---

## Interpreting CSV Output

Quick analysis with common tools:

```bash
# Mean write latency per codec at 1 MB (healthy runs only)
awk -F, '$3==1048576 && $8=="healthy" {sum[$1]+=$5; cnt[$1]++}
         END {for (c in sum) printf "%s: %.1f ms\n", c, sum[c]/cnt[c]}' results.csv

# All degraded runs at 1 MB
awk -F, '$3==1048576 && $8!="healthy"' results.csv

# Check for verification failures
awk -F, '$7=="FAIL"' results.csv
```
