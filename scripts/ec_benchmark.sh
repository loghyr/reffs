#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# EC Benchmark -- compare encoding overhead across codecs and geometries.
#
# Usage: ec_benchmark.sh [OPTIONS] <ec_demo_path> <mds_host>
#
# Runs write/verify cycles for each codec at several file sizes,
# measuring wall-clock time.  Output is CSV to stdout.
#
# --degrade N      After each healthy read, re-read with N data shards
#                  skipped to measure reconstruction overhead.
# --force-scalar   Pass --force-scalar to ec_demo (disable SIMD).
# --layout TYPE    Layout type: v1 (NFSv3 DS I/O, default) or v2 (CHUNK ops).
# --shard-size N   Per-data-shard byte size passed to ec_demo's
#                  --shard-size flag.  Default: empty (ec_demo's own
#                  default of 4096 applies).  The shard size is added
#                  to the CSV output as a column so wrappers iterating
#                  over multiple shard sizes produce a single merged CSV.

set -e

# ------------------------------------------------------------------ #
# Parse optional flags before positional args                         #
# ------------------------------------------------------------------ #

DEGRADE=0
FORCE_SCALAR=""
LAYOUT_ARG=""
LAYOUT_TAG="v1"
SHARD_SIZE_ARG=""
SHARD_SIZE_TAG="default"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --degrade) DEGRADE="$2"; shift 2 ;;
        --force-scalar) FORCE_SCALAR="--force-scalar"; shift ;;
        --layout) LAYOUT_ARG="--layout $2"; LAYOUT_TAG="$2"; shift 2 ;;
        --shard-size) SHARD_SIZE_ARG="--shard-size $2"; SHARD_SIZE_TAG="$2"; shift 2 ;;
        *) break ;;
    esac
done

EC_DEMO="${1:-/build/tools/ec_demo}"
MDS="${2:-localhost}"
RUNS=5
WARMUP=2
# File sizes (bytes) are env-overridable so wrapper scripts can
# extend the matrix.  Default matches the historical benchmark
# matrix (4 KiB - 1 MiB).
SIZES="${SIZES:-4096 16384 65536 262144 1048576}"

# Geometries to test: "k:m" pairs
GEOMETRIES="4:2 8:2"

# Number of DSes available (for stripe width)
NUM_DS=10

# ------------------------------------------------------------------ #
# Helpers                                                              #
# ------------------------------------------------------------------ #

now_ms() {
    date +%s%N | cut -b1-13
}

wait_for_mds() {
    echo "Waiting for MDS at $MDS:2049..." >&2
    for i in $(seq 1 60); do
        if "$EC_DEMO" put --mds "$MDS" --file _probe --input /dev/null \
                2>/dev/null; then
            echo "MDS ready." >&2
            return 0
        fi
        sleep 1
    done
    echo "ERROR: MDS did not become ready within 60 seconds." >&2
    exit 1
}

generate_test_files() {
    for sz in $SIZES; do
        dd if=/dev/urandom of="/tmp/bench_${sz}" bs="$sz" count=1 \
            2>/dev/null
    done
}

# Build comma-separated skip list: indices 0..N-1 (data shards).
build_skip_list() {
    local n="$1"
    local list=""
    for i in $(seq 0 $((n - 1))); do
        [ -n "$list" ] && list="${list},"
        list="${list}${i}"
    done
    echo "$list"
}

# ------------------------------------------------------------------ #
# Benchmark one codec + size + geometry combination                    #
# ------------------------------------------------------------------ #

# bench_one <codec> <k> <m> <sz> <run> <mode> [skip_ds_list]
bench_one() {
    local codec="$1"
    local k="$2"
    local m="$3"
    local sz="$4"
    local run="$5"
    local mode="$6"
    local skip_ds="$7"
    local geom="${k}+${m}"
    local stag="s"
    [ -n "$FORCE_SCALAR" ] && stag="n"
    local fname="bench_${LAYOUT_TAG}_${stag}_${codec}_${geom}_${sz}_${run}"
    local input="/tmp/bench_${sz}"
    local skip_arg=""
    [ -n "$skip_ds" ] && skip_arg="--skip-ds $skip_ds"

    local write_ms=0

    # Write only on healthy pass
    if [ "$mode" = "healthy" ]; then
        local t0
        t0=$(now_ms)
        # shellcheck disable=SC2086
        "$EC_DEMO" write --mds "$MDS" --file "$fname" --input "$input" \
            --k "$k" --m "$m" --codec "$codec" \
            $FORCE_SCALAR $LAYOUT_ARG $SHARD_SIZE_ARG 2>/dev/null
        local t1
        t1=$(now_ms)
        write_ms=$(( t1 - t0 ))
    fi

    # Read (with optional --skip-ds for degraded mode)
    local t0
    t0=$(now_ms)
    # shellcheck disable=SC2086
    "$EC_DEMO" read --mds "$MDS" --file "$fname" --output "/tmp/out_${sz}" \
        --k "$k" --m "$m" --codec "$codec" --size "$sz" \
        $skip_arg $FORCE_SCALAR $LAYOUT_ARG $SHARD_SIZE_ARG \
        2>>/tmp/ec_bench_err.log
    local t1
    t1=$(now_ms)
    local read_ms=$(( t1 - t0 ))

    # Verify
    local verify="OK"
    if ! cmp -s "$input" "/tmp/out_${sz}"; then
        verify="FAIL"
    fi

    echo "${codec},${geom},${sz},${run},${write_ms},${read_ms},${verify},${mode},${LAYOUT_TAG},${CPU_INFO},${SHARD_SIZE_TAG}"
}

# ------------------------------------------------------------------ #
# Plain mirroring baseline (no EC, no degraded mode)                   #
# ------------------------------------------------------------------ #

bench_plain() {
    local sz="$1"
    local run="$2"
    local stag="s"
    [ -n "$FORCE_SCALAR" ] && stag="n"
    local fname="bench_${LAYOUT_TAG}_${stag}_plain_${sz}_${run}"
    local input="/tmp/bench_${sz}"

    local t0
    t0=$(now_ms)
    "$EC_DEMO" put --mds "$MDS" --file "$fname" --input "$input" \
        2>/dev/null
    local t1
    t1=$(now_ms)
    local write_ms=$(( t1 - t0 ))

    t0=$(now_ms)
    "$EC_DEMO" get --mds "$MDS" --file "$fname" --output "/tmp/out_${sz}" \
        --size "$sz" 2>/dev/null
    t1=$(now_ms)
    local read_ms=$(( t1 - t0 ))

    local verify="OK"
    if ! cmp -s "$input" "/tmp/out_${sz}"; then
        verify="FAIL"
    fi

    # Shard size has no meaning for the plain (no-codec) path -- but
    # we still write it to keep the CSV column count uniform across
    # codec and plain rows.
    echo "plain,1+0,${sz},${run},${write_ms},${read_ms},${verify},healthy,${LAYOUT_TAG},${CPU_INFO},${SHARD_SIZE_TAG}"
}

# ------------------------------------------------------------------ #
# Pure striping (no redundancy, parallel I/O across all DSes)          #
# ------------------------------------------------------------------ #

bench_stripe() {
    local sz="$1"
    local run="$2"
    local stag="s"
    [ -n "$FORCE_SCALAR" ] && stag="n"
    local fname="bench_${LAYOUT_TAG}_${stag}_stripe_${sz}_${run}"
    local input="/tmp/bench_${sz}"

    local t0
    t0=$(now_ms)
    "$EC_DEMO" write --mds "$MDS" --file "$fname" --input "$input" \
        --k $NUM_DS --m 0 --codec stripe 2>/dev/null
    local t1
    t1=$(now_ms)
    local write_ms=$(( t1 - t0 ))

    t0=$(now_ms)
    "$EC_DEMO" read --mds "$MDS" --file "$fname" --output "/tmp/out_${sz}" \
        --k $NUM_DS --m 0 --codec stripe --size "$sz" 2>/dev/null
    t1=$(now_ms)
    local read_ms=$(( t1 - t0 ))

    local verify="OK"
    if ! cmp -s "$input" "/tmp/out_${sz}"; then
        verify="FAIL"
    fi

    echo "stripe,${NUM_DS}+0,${sz},${run},${write_ms},${read_ms},${verify},healthy,${LAYOUT_TAG},${CPU_INFO}"
}

# ------------------------------------------------------------------ #
# Main                                                                 #
# ------------------------------------------------------------------ #

# ------------------------------------------------------------------ #
# Gather system info                                                   #
# ------------------------------------------------------------------ #

gather_cpu_info() {
    local arch
    arch=$(uname -m)
    local model="unknown"
    if [ -f /proc/cpuinfo ]; then
        model=$(grep -m1 "model name" /proc/cpuinfo | sed 's/.*: //' | \
                sed 's/  */ /g')
    elif command -v sysctl >/dev/null 2>&1; then
        model=$(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo "unknown")
    fi
    local kernel
    kernel=$(uname -r)
    local simd="scalar"
    case "$arch" in
        aarch64|arm64) simd="neon" ;;
        x86_64)
            if grep -q avx2 /proc/cpuinfo 2>/dev/null; then
                simd="avx2"
            else
                simd="sse2"
            fi
            ;;
    esac
    [ -n "$FORCE_SCALAR" ] && simd="scalar(forced)"
    echo "${arch},${model},${kernel},${simd}"
}

CPU_INFO=$(gather_cpu_info)

echo "=== EC Benchmark ===" >&2
echo "MDS: $MDS  GEOMETRIES=$GEOMETRIES  RUNS=$RUNS  DEGRADE=$DEGRADE  LAYOUT=$LAYOUT_TAG" >&2
echo "CPU: $CPU_INFO" >&2
echo "Sizes: $SIZES" >&2
echo "" >&2

wait_for_mds
generate_test_files

# CSV header
echo "codec,geometry,size_bytes,run,write_ms,read_ms,verify,mode,layout,arch,cpu,kernel,simd"

for sz in $SIZES; do
    echo "--- Size: $sz bytes ---" >&2

    # Warmup (not recorded)
    for w in $(seq 1 $WARMUP); do
        bench_plain "$sz" "w${w}" > /dev/null 2>&1 || true
        bench_stripe "$sz" "w${w}" > /dev/null 2>&1 || true
        for geom in $GEOMETRIES; do
            k=${geom%%:*}
            m=${geom##*:}
            for codec in rs mojette-sys mojette-nonsys; do
                bench_one "$codec" "$k" "$m" "$sz" "w${w}" "healthy" "" \
                    > /dev/null 2>&1 || true
            done
        done
    done

    # Measured runs
    for run in $(seq 1 $RUNS); do
        # Plain + stripe baselines
        bench_plain "$sz" "$run"
        bench_stripe "$sz" "$run"

        # Each geometry
        for geom in $GEOMETRIES; do
            k=${geom%%:*}
            m=${geom##*:}

            # Healthy pass -- || true prevents set -e from killing
            # the script when a codec/layout combination fails
            # (e.g., Mojette + v2 CHUNK variable chunk size mismatch).
            bench_one "rs" "$k" "$m" "$sz" "$run" "healthy" "" || true
            bench_one "mojette-sys" "$k" "$m" "$sz" "$run" "healthy" "" || true
            bench_one "mojette-nonsys" "$k" "$m" "$sz" "$run" "healthy" "" || true

            # Degraded pass (skip plain and stripe -- no redundancy)
            if [ "$DEGRADE" -gt 0 ] && [ "$DEGRADE" -le "$m" ]; then
                skip_list=$(build_skip_list "$DEGRADE")
                bench_one "rs" "$k" "$m" "$sz" "$run" \
                    "degraded-${DEGRADE}" "$skip_list" || true
                bench_one "mojette-sys" "$k" "$m" "$sz" "$run" \
                    "degraded-${DEGRADE}" "$skip_list" || true
                bench_one "mojette-nonsys" "$k" "$m" "$sz" "$run" \
                    "degraded-${DEGRADE}" "$skip_list" || true
            fi
        done
    done
done

echo "" >&2
echo "=== Benchmark complete ===" >&2
