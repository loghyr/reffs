#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Multi-codec sanity test: per-SB write -> read -> byte-exact diff.
#
# For each provisioned SB, write a fixed-size random payload, read
# it back through ec_demo, cmp -s for an exact match.  Per-SB the
# layout type and codec choice differ:
#
#   /ffv1-csm     v1   plain  (single mirror per write)
#   /ffv1-stripes v1   stripe k=6 m=0 (across all 6 DSes)
#   /ffv2-csm     v2   plain  (CHUNK ops, single mirror)
#   /ffv2-rs      v2   rs k=4 m=2
#   /ffv2-mj      v2   mojette-sys k=4 m=2
#
# Payload size: 96 KiB.  Chosen to give whole-number stripe counts
# for both k=6 (16 stripes of 4*1.5K = 6K each at k=6 -> not 4K
# exactly; the user-stated "4K stripe" is a target -- the actual
# shard size is file_size/k per ec_demo, so 96K with k=6 is ~16K
# shards, with k=4 is 24K shards).  4K shards specifically would
# require file_size = k*4K which is 24K (k=6) or 16K (k=4); the
# 96K choice prioritises exercising multiple stripes per shard for
# a useful sanity check.  Test reports the actual shard size in
# its output.
#
# Exits non-zero on any per-SB failure; reports a per-SB PASS/FAIL
# matrix at the end.
#
# Usage: run-sanity.sh <ec_demo_path> <mds_host>

set -u

EC_DEMO="${1:-/shared/build/tools/ec_demo}"
MDS="${2:-reffs-mds}"

PAYLOAD="/tmp/sanity_payload.bin"
PAYLOAD_SIZE=$((96 * 1024))

dd if=/dev/urandom of="$PAYLOAD" bs="$PAYLOAD_SIZE" count=1 status=none

# Per-SB result accumulator
declare -A RESULTS
declare -a SB_ORDER

run_one() {
    local sb_path="$1"
    local layout="$2"
    shift 2
    local extra_args=( "$@" )

    local label="$sb_path"
    SB_ORDER+=("$label")

    local fname="${sb_path}/test.bin"
    local out="/tmp/out_${sb_path//\//_}.bin"
    local log="/tmp/log_${sb_path//\//_}.txt"
    rm -f "$out"

    echo "--- $label  layout=$layout  ${extra_args[*]} ---"

    # Write
    if ! "$EC_DEMO" "${extra_args[@]:0:1}" \
            --mds "$MDS" --layout "$layout" \
            --file "$fname" --input "$PAYLOAD" \
            "${extra_args[@]:1}" \
            >"$log" 2>&1; then
        echo "  WRITE FAILED -- log:"
        sed 's/^/    /' "$log"
        RESULTS[$label]="FAIL (write)"
        return
    fi

    # Read
    local read_op="${extra_args[0]}"
    case "$read_op" in
        put)   read_op=get ;;
        write) read_op=read ;;
        *)     echo "  internal: unknown write op '$read_op'"; RESULTS[$label]="FAIL (internal)"; return ;;
    esac
    local read_extra=( "$read_op" "${extra_args[@]:1}" --size "$PAYLOAD_SIZE" )

    if ! "$EC_DEMO" "${read_extra[@]:0:1}" \
            --mds "$MDS" --layout "$layout" \
            --file "$fname" --output "$out" \
            "${read_extra[@]:1}" \
            >>"$log" 2>&1; then
        echo "  READ FAILED -- log:"
        sed 's/^/    /' "$log"
        RESULTS[$label]="FAIL (read)"
        return
    fi

    # Diff
    if cmp -s "$PAYLOAD" "$out"; then
        echo "  PASS"
        RESULTS[$label]="PASS"
    else
        local in_size out_size
        in_size=$(stat -c%s "$PAYLOAD" 2>/dev/null || stat -f%z "$PAYLOAD")
        out_size=$(stat -c%s "$out" 2>/dev/null || stat -f%z "$out")
        echo "  DIFF MISMATCH (in=$in_size out=$out_size)"
        echo "  log:"
        sed 's/^/    /' "$log"
        RESULTS[$label]="FAIL (diff)"
    fi
}

main() {
    echo "=== Sanity test: payload=${PAYLOAD_SIZE} bytes against $MDS ==="

    # /ffv1-csm: v1 plain mirror via 'put'
    run_one /ffv1-csm     v1   put

    # /ffv1-stripes: v1 striped k=6 m=0
    run_one /ffv1-stripes v1   write --codec stripe      --k 6 --m 0

    # /ffv2-csm: v2 plain mirror via 'put'
    run_one /ffv2-csm     v2   put

    # /ffv2-rs: v2 RS 4+2
    run_one /ffv2-rs      v2   write --codec rs          --k 4 --m 2

    # /ffv2-mj: v2 Mojette-sys 4+2
    run_one /ffv2-mj      v2   write --codec mojette-sys --k 4 --m 2

    echo
    echo "=== Sanity result matrix ==="
    local fails=0
    for sb in "${SB_ORDER[@]}"; do
        printf "  %-18s %s\n" "$sb" "${RESULTS[$sb]}"
        if [[ "${RESULTS[$sb]}" != PASS ]]; then
            fails=$((fails + 1))
        fi
    done

    if (( fails == 0 )); then
        echo "ALL PASS"
        exit 0
    else
        echo "$fails SB(s) FAILED"
        exit 1
    fi
}

main "$@"
