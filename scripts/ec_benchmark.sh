#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# EC Benchmark — compare encoding overhead across codecs.
#
# Usage: ec_benchmark.sh <ec_demo_path> <mds_host>
#
# Runs write/verify cycles for each codec at several file sizes,
# measuring wall-clock time.  Output is CSV to stdout.

set -e

EC_DEMO="${1:-/build/tools/ec_demo}"
MDS="${2:-localhost}"
K=4
M=2
RUNS=5
WARMUP=2
SIZES="4096 16384 65536 262144 1048576"

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

# ------------------------------------------------------------------ #
# Benchmark one codec + size combination                               #
# ------------------------------------------------------------------ #

bench_one() {
    local codec="$1"
    local sz="$2"
    local run="$3"
    local fname="bench_${codec}_${sz}_${run}"
    local input="/tmp/bench_${sz}"

    # Write
    local t0
    t0=$(now_ms)
    "$EC_DEMO" write --mds "$MDS" --file "$fname" --input "$input" \
        --k $K --m $M --codec "$codec" 2>/dev/null
    local t1
    t1=$(now_ms)
    local write_ms=$(( t1 - t0 ))

    # Read back
    t0=$(now_ms)
    "$EC_DEMO" read --mds "$MDS" --file "$fname" --output "/tmp/out_${sz}" \
        --k $K --m $M --codec "$codec" --size "$sz" 2>/dev/null
    t1=$(now_ms)
    local read_ms=$(( t1 - t0 ))

    # Verify
    local verify="OK"
    if ! cmp -s "$input" "/tmp/out_${sz}"; then
        verify="FAIL"
    fi

    echo "${codec},${sz},${run},${write_ms},${read_ms},${verify}"
}

# ------------------------------------------------------------------ #
# Also benchmark plain mirroring (no EC) for baseline                  #
# ------------------------------------------------------------------ #

bench_plain() {
    local sz="$1"
    local run="$2"
    local fname="bench_plain_${sz}_${run}"
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

    echo "plain,${sz},${run},${write_ms},${read_ms},${verify}"
}

# ------------------------------------------------------------------ #
# Main                                                                 #
# ------------------------------------------------------------------ #

echo "=== EC Benchmark ===" >&2
echo "MDS: $MDS  K=$K M=$M  RUNS=$RUNS  WARMUP=$WARMUP" >&2
echo "Sizes: $SIZES" >&2
echo "" >&2

wait_for_mds
generate_test_files

# CSV header
echo "codec,size_bytes,run,write_ms,read_ms,verify"

for sz in $SIZES; do
    echo "--- Size: $sz bytes ---" >&2

    # Warmup (not recorded)
    for w in $(seq 1 $WARMUP); do
        for codec in rs mojette-sys mojette-nonsys; do
            bench_one "$codec" "$sz" "w${w}" > /dev/null 2>&1 || true
        done
        bench_plain "$sz" "w${w}" > /dev/null 2>&1 || true
    done

    # Measured runs
    for run in $(seq 1 $RUNS); do
        bench_plain "$sz" "$run"
        bench_one "rs" "$sz" "$run"
        bench_one "mojette-sys" "$sz" "$run"
        bench_one "mojette-nonsys" "$sz" "$run"
    done
done

echo "" >&2
echo "=== Benchmark complete ===" >&2
