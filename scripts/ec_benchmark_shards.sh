#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Drives experiment 03-larger-shards (reffs-docs/experiments/).
#
# Wraps ec_benchmark.sh to run the SIMD-vs-scalar matrix at several
# shard sizes and emit a single merged CSV.
#
# Matrix:
#   codecs        : rs, mojette-sys (set via ec_benchmark.sh's loop)
#   geometries    : 4+2, 8+2 (set via ec_benchmark.sh's GEOMETRIES)
#   file sizes    : 64 KiB, 256 KiB, 1 MiB, 4 MiB, 16 MiB
#   shard sizes   : 4 KiB, 16 KiB, 64 KiB, 256 KiB
#   SIMD modes    : "simd"   = normal build, no --force-scalar
#                   "scalar" = --enable-noscalar-vec build + --force-scalar
#   runs per cell : 5
#
# Total cells per host: 2 codecs * 2 geom * 5 file * 4 shard * 2 simd = 160
# At 5 runs each, 800 measurement runs per host.  Plain path adds
# baseline runs without the matrix expansion.
#
# Usage:
#   ec_benchmark_shards.sh \
#       --simd-binary   /path/to/build-simd/tools/ec_demo \
#       --scalar-binary /path/to/build-scalar/tools/ec_demo \
#       --mds localhost \
#       [--layout v1|v2] \
#       [--shard-sizes "4096 16384 65536 262144"] \
#       [--file-sizes  "65536 262144 1048576 4194304 16777216"] \
#       > results.csv
#
# Prereqs:
#   - SIMD binary: built normally (auto-vectorization enabled)
#   - Scalar binary: built with --enable-noscalar-vec (so the codec
#     scalar path cannot be auto-vectorized).  Run with
#     --force-scalar to take the scalar code path at runtime.
#   - MDS reachable at the specified address with HS-35556 topology.
#   - io_uring large-message fix landed (commit c01a293b8c8f); cite
#     the SHA in the experiment results.md.

set -e

# --- defaults -------------------------------------------------------

SIMD_BINARY=""
SCALAR_BINARY=""
MDS="localhost"
LAYOUT="v1"
SHARD_SIZES_DEFAULT="4096 16384 65536 262144"
FILE_SIZES_DEFAULT="65536 262144 1048576 4194304 16777216"
SHARD_SIZES="$SHARD_SIZES_DEFAULT"
FILE_SIZES="$FILE_SIZES_DEFAULT"

# --- argparse -------------------------------------------------------

while [[ $# -gt 0 ]]; do
    case "$1" in
        --simd-binary)   SIMD_BINARY="$2"; shift 2 ;;
        --scalar-binary) SCALAR_BINARY="$2"; shift 2 ;;
        --mds)           MDS="$2"; shift 2 ;;
        --layout)        LAYOUT="$2"; shift 2 ;;
        --shard-sizes)   SHARD_SIZES="$2"; shift 2 ;;
        --file-sizes)    FILE_SIZES="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,40p' "$0" >&2
            exit 0
            ;;
        *) echo "Unknown arg: $1" >&2; exit 1 ;;
    esac
done

if [[ -z "$SIMD_BINARY" || -z "$SCALAR_BINARY" ]]; then
    echo "ERROR: both --simd-binary and --scalar-binary are required" >&2
    exit 1
fi
[[ -x "$SIMD_BINARY"   ]] || { echo "ERROR: not executable: $SIMD_BINARY"   >&2; exit 1; }
[[ -x "$SCALAR_BINARY" ]] || { echo "ERROR: not executable: $SCALAR_BINARY" >&2; exit 1; }

INNER="$(dirname "$0")/ec_benchmark.sh"
[[ -x "$INNER" ]] || { echo "ERROR: not executable: $INNER" >&2; exit 1; }

# --- env header (logged to stderr; CSV body to stdout) --------------

{
    echo "# ec_benchmark_shards: $(date -Iseconds)"
    echo "# host:           $(uname -a)"
    echo "# simd binary:    $SIMD_BINARY"
    echo "# scalar binary:  $SCALAR_BINARY"
    echo "# mds:            $MDS"
    echo "# layout:         $LAYOUT"
    echo "# shard_sizes:    $SHARD_SIZES"
    echo "# file_sizes:     $FILE_SIZES"
    echo "# simd binary build-id:   $(file -L "$SIMD_BINARY" | grep -oE 'BuildID\[sha1\]=[0-9a-f]+' || echo n/a)"
    echo "# scalar binary build-id: $(file -L "$SCALAR_BINARY" | grep -oE 'BuildID\[sha1\]=[0-9a-f]+' || echo n/a)"
} >&2

# --- CSV header to stdout ------------------------------------------

echo "codec,geom,file_size,run,write_ms,read_ms,verify,mode,layout,cpu,shard_size,simd_mode"

# --- run matrix -----------------------------------------------------

run_one_arm() {
    local simd_mode="$1"  # "simd" or "scalar"
    local binary="$2"
    local extra_args="$3"
    local shard_size="$4"

    # ec_benchmark.sh emits its own CSV without simd_mode column;
    # we append it here with awk.  Header lines are suppressed by
    # ec_benchmark.sh; if it ever starts emitting one we'd need to
    # filter that too.
    SIZES="$FILE_SIZES" \
        "$INNER" \
            $extra_args \
            --layout "$LAYOUT" \
            --shard-size "$shard_size" \
            "$binary" "$MDS" 2>>/tmp/ec_benchmark_shards.err |
        awk -v simd="$simd_mode" 'NF { print $0 "," simd }'
}

for shard in $SHARD_SIZES; do
    echo "# === shard_size=$shard ===" >&2

    echo "# --- simd arm (no --force-scalar) ---" >&2
    run_one_arm "simd"   "$SIMD_BINARY"   "" "$shard"

    echo "# --- scalar arm (--force-scalar, noscalar-vec build) ---" >&2
    run_one_arm "scalar" "$SCALAR_BINARY" "--force-scalar" "$shard"
done

echo "# done $(date -Iseconds)" >&2
