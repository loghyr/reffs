#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# EC Benchmark — compare encoding overhead across codecs.
#
# Usage: ec_benchmark.sh [--degrade N] <ec_demo_path> <mds_host>
#
# Runs write/verify cycles for each codec at several file sizes,
# measuring wall-clock time.  Output is CSV to stdout.
#
# --degrade N  After each healthy read, re-read with N data shards
#              skipped to measure reconstruction overhead.

set -e

# ------------------------------------------------------------------ #
# Parse optional flags before positional args                         #
# ------------------------------------------------------------------ #

DEGRADE=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --degrade) DEGRADE="$2"; shift 2 ;;
        *) break ;;
    esac
done

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
# Benchmark one codec + size combination                               #
# ------------------------------------------------------------------ #

# bench_one <codec> <sz> <run> <mode> [skip_ds_list]
#
# mode=healthy: write + read
# mode=degraded-N: read only (reuses file from healthy pass)
bench_one() {
    local codec="$1"
    local sz="$2"
    local run="$3"
    local mode="$4"
    local skip_ds="$5"
    local fname="bench_${codec}_${sz}_${run}"
    local input="/tmp/bench_${sz}"
    local skip_arg=""
    [ -n "$skip_ds" ] && skip_arg="--skip-ds $skip_ds"

    local write_ms=0

    # Write only on healthy pass
    if [ "$mode" = "healthy" ]; then
        local t0
        t0=$(now_ms)
        "$EC_DEMO" write --mds "$MDS" --file "$fname" --input "$input" \
            --k $K --m $M --codec "$codec" 2>/dev/null
        local t1
        t1=$(now_ms)
        write_ms=$(( t1 - t0 ))
    fi

    # Read (with optional --skip-ds for degraded mode)
    local t0
    t0=$(now_ms)
    # shellcheck disable=SC2086
    "$EC_DEMO" read --mds "$MDS" --file "$fname" --output "/tmp/out_${sz}" \
        --k $K --m $M --codec "$codec" --size "$sz" $skip_arg 2>/dev/null
    local t1
    t1=$(now_ms)
    local read_ms=$(( t1 - t0 ))

    # Verify
    local verify="OK"
    if ! cmp -s "$input" "/tmp/out_${sz}"; then
        verify="FAIL"
    fi

    echo "${codec},${sz},${run},${write_ms},${read_ms},${verify},${mode}"
}

# ------------------------------------------------------------------ #
# Plain mirroring baseline (no EC, no degraded mode)                   #
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

    echo "plain,${sz},${run},${write_ms},${read_ms},${verify},healthy"
}

# ------------------------------------------------------------------ #
# Main                                                                 #
# ------------------------------------------------------------------ #

if [ "$DEGRADE" -ge "$M" ]; then
    echo "ERROR: --degrade $DEGRADE must be less than M=$M" >&2
    exit 1
fi

SKIP_LIST=""
[ "$DEGRADE" -gt 0 ] && SKIP_LIST=$(build_skip_list "$DEGRADE")

echo "=== EC Benchmark ===" >&2
echo "MDS: $MDS  K=$K M=$M  RUNS=$RUNS  WARMUP=$WARMUP  DEGRADE=$DEGRADE" >&2
echo "Sizes: $SIZES" >&2
[ -n "$SKIP_LIST" ] && echo "Skip DSes: $SKIP_LIST" >&2
echo "" >&2

wait_for_mds
generate_test_files

# CSV header
echo "codec,size_bytes,run,write_ms,read_ms,verify,mode"

for sz in $SIZES; do
    echo "--- Size: $sz bytes ---" >&2

    # Warmup (not recorded)
    for w in $(seq 1 $WARMUP); do
        for codec in rs mojette-sys mojette-nonsys; do
            bench_one "$codec" "$sz" "w${w}" "healthy" "" \
                > /dev/null 2>&1 || true
        done
        bench_plain "$sz" "w${w}" > /dev/null 2>&1 || true
    done

    # Measured runs
    for run in $(seq 1 $RUNS); do
        # Healthy pass (write + read, all codecs)
        bench_plain "$sz" "$run"
        bench_one "rs" "$sz" "$run" "healthy" ""
        bench_one "mojette-sys" "$sz" "$run" "healthy" ""
        bench_one "mojette-nonsys" "$sz" "$run" "healthy" ""

        # Degraded pass (read only, skip plain)
        if [ "$DEGRADE" -gt 0 ]; then
            bench_one "rs" "$sz" "$run" "degraded-${DEGRADE}" "$SKIP_LIST"
            bench_one "mojette-sys" "$sz" "$run" \
                "degraded-${DEGRADE}" "$SKIP_LIST"
            bench_one "mojette-nonsys" "$sz" "$run" \
                "degraded-${DEGRADE}" "$SKIP_LIST"
        fi
    done
done

echo "" >&2
echo "=== Benchmark complete ===" >&2
