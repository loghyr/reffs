#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# ec-repair bench harness -- drives the IETF 126 cells for the
# "cost of collisions and repair" slide.  Companion to
# reffs-docs/ec-repair-bench-tier2.md.
#
# For each cell (size x codec x loss-pattern x iter): write a file
# of `size` bytes through the codec, then run `ec_demo repair` with
# the loss mask, parse the per-phase timing line ec_demo prints,
# emit a CSV row.
#
# Cells default to the IETF spec:
#   sizes:  4k, 64k, 1m, 16m
#   codecs: rs, mojette-sys
#   losses: 1 shard (mask 0x1), 2 shards (mask 0x3)
#   iters:  5
#
# Total: 4 x 2 x 2 x 5 = 80 cells.
#
# Usage:
#   ec_repair_bench.sh [OPTIONS] <ec_demo_path> <mds_host>
#
# Options:
#   --sizes "4k 64k 1m"      Override the size axis (space-separated).
#   --codecs "rs mojette-sys"  Override the codec axis.
#   --losses "0x1 0x3"       Override the loss mask axis.
#   --iters N                Override iteration count (default 5).
#   --k K                    Data shards (default 4).
#   --m M                    Parity shards (default 2).
#   --shard-size N           Per-shard byte size (default 4096).
#   --layout v1|v2           Layout type (default v2 for CHUNK ops).
#   --tmp-prefix PFX         NFS-side path prefix for test files.
#
# Output CSV columns (header on first line):
#   size_bytes,codec,k,m,loss_mask,iter,layoutget_ms,read_ms,
#   decode_ms,write_repair_ms,finalize_ms,commit_ms,
#   chunk_repaired_ms,layoutreturn_ms,total_ms,bytes_repaired,
#   shards_repaired,stripes_processed,verify_ok

set -e

# ------------------------------------------------------------------
# Defaults + flag parsing
# ------------------------------------------------------------------

SIZES="4k 64k 1m 16m"
CODECS="rs mojette-sys"
LOSSES="0x1 0x3"
ITERS=5
K=4
M=2
SHARD_SIZE=4096
LAYOUT="v2"
TMP_PREFIX="/repair_bench"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --sizes)       SIZES="$2";       shift 2 ;;
        --codecs)      CODECS="$2";      shift 2 ;;
        --losses)      LOSSES="$2";      shift 2 ;;
        --iters)       ITERS="$2";       shift 2 ;;
        --k)           K="$2";           shift 2 ;;
        --m)           M="$2";           shift 2 ;;
        --shard-size)  SHARD_SIZE="$2";  shift 2 ;;
        --layout)      LAYOUT="$2";      shift 2 ;;
        --tmp-prefix)  TMP_PREFIX="$2";  shift 2 ;;
        *)             break ;;
    esac
done

EC_DEMO="${1:-/build/tools/ec_demo}"
MDS="${2:-localhost}"

if [ ! -x "$EC_DEMO" ]; then
    echo "ec_repair_bench: ec_demo not executable: $EC_DEMO" >&2
    exit 1
fi

# Make libtool's in-tree .libs/ visible to the inner ELF (mirrors
# ec_benchmark.sh's pattern; harmless inside the Docker container).
_build_root=$(cd "$(dirname "$EC_DEMO")/.." 2>/dev/null && pwd)
if [ -n "$_build_root" ] && [ -d "$_build_root/lib" ]; then
    _extra=$(find "$_build_root" -type d -name '.libs' 2>/dev/null | paste -sd:)
    [ -n "$_extra" ] && export LD_LIBRARY_PATH="$_extra${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi

# ------------------------------------------------------------------
# Size suffix -> bytes
# ------------------------------------------------------------------

size_to_bytes() {
    local s="$1"
    local n="${s%[kKmMgG]}"
    local suf="${s: -1}"
    case "$suf" in
        k|K) echo "$((n * 1024))" ;;
        m|M) echo "$((n * 1024 * 1024))" ;;
        g|G) echo "$((n * 1024 * 1024 * 1024))" ;;
        *)   echo "$s" ;;
    esac
}

# ------------------------------------------------------------------
# Header + per-cell loop
# ------------------------------------------------------------------

echo "size_bytes,codec,k,m,loss_mask,iter,layoutget_ms,read_ms,decode_ms,write_repair_ms,finalize_ms,commit_ms,chunk_repaired_ms,layoutreturn_ms,total_ms,bytes_repaired,shards_repaired,stripes_processed,verify_ok"

TMPDIR=$(mktemp -d -t ec_repair_bench.XXXXXX)
trap 'rm -rf "$TMPDIR"' EXIT

for size in $SIZES; do
    bytes=$(size_to_bytes "$size")
    local_input="$TMPDIR/input.$size.bin"
    dd if=/dev/urandom of="$local_input" bs="$bytes" count=1 \
        status=none 2>/dev/null

    for codec in $CODECS; do
        for loss in $LOSSES; do
            for iter in $(seq 1 "$ITERS"); do
                nfs_file="${TMP_PREFIX}.${codec}.${size}.${loss}.${iter}"

                # Fresh write per cell so repair always starts from
                # a known-good baseline.
                "$EC_DEMO" write \
                    --mds "$MDS" --file "$nfs_file" \
                    --input "$local_input" \
                    --layout "$LAYOUT" \
                    --codec "$codec" \
                    --k "$K" --m "$M" \
                    --shard-size "$SHARD_SIZE" \
                    >/dev/null 2>&1 || {
                        echo "$bytes,$codec,$K,$M,$loss,$iter,,,,,,,,,,,,,write_failed" >&2
                        continue
                    }

                # Drive the repair; parse the per-phase line.
                line=$("$EC_DEMO" repair \
                        --mds "$MDS" --file "$nfs_file" \
                        --size "$bytes" \
                        --skip-ds "$loss" \
                        --layout "$LAYOUT" \
                        --codec "$codec" \
                        --k "$K" --m "$M" \
                        --shard-size "$SHARD_SIZE" \
                        2>/dev/null || echo "REPAIR_FAILED")

                if [[ "$line" == "REPAIR_FAILED" ]]; then
                    echo "$bytes,$codec,$K,$M,$loss,$iter,,,,,,,,,,,,,repair_failed"
                    continue
                fi

                # Extract key=value pairs ec_demo prints.
                lg=$(echo "$line" | sed -nE 's/.*layoutget_ms=([0-9.]+).*/\1/p')
                rd=$(echo "$line" | sed -nE 's/.*read_ms=([0-9.]+).*/\1/p')
                dc=$(echo "$line" | sed -nE 's/.*decode_ms=([0-9.]+).*/\1/p')
                wr=$(echo "$line" | sed -nE 's/.*write_repair_ms=([0-9.]+).*/\1/p')
                fn=$(echo "$line" | sed -nE 's/.*finalize_ms=([0-9.]+).*/\1/p')
                cm=$(echo "$line" | sed -nE 's/.*commit_ms=([0-9.]+).*/\1/p')
                cr=$(echo "$line" | sed -nE 's/.*chunk_repaired_ms=([0-9.]+).*/\1/p')
                lr=$(echo "$line" | sed -nE 's/.*layoutreturn_ms=([0-9.]+).*/\1/p')
                tt=$(echo "$line" | sed -nE 's/.*total_ms=([0-9.]+).*/\1/p')
                br=$(echo "$line" | sed -nE 's/.*bytes_repaired=([0-9]+).*/\1/p')
                sh=$(echo "$line" | sed -nE 's/.*shards_repaired=([0-9]+).*/\1/p')
                sp=$(echo "$line" | sed -nE 's/.*stripes_processed=([0-9]+).*/\1/p')

                # Post-repair verify: read back, compare against the
                # original input.  ec_demo verify exits 0 on match.
                verify_ok="no"
                if "$EC_DEMO" verify \
                        --mds "$MDS" --file "$nfs_file" \
                        --input "$local_input" \
                        --layout "$LAYOUT" \
                        --codec "$codec" \
                        --k "$K" --m "$M" \
                        --shard-size "$SHARD_SIZE" \
                        >/dev/null 2>&1; then
                    verify_ok="yes"
                fi

                echo "$bytes,$codec,$K,$M,$loss,$iter,$lg,$rd,$dc,$wr,$fn,$cm,$cr,$lr,$tt,$br,$sh,$sp,$verify_ok"
            done
        done
    done
done
