#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Cross-PS multi-codec demo: ec_demo writes a codec-encoded file via
# PS-A, ec_demo reads it via PS-B, byte-exact diff.  The PS forwards
# LAYOUTGET / GETDEVICEINFO / LAYOUTRETURN to the upstream MDS
# (task #150 layout passthrough); ec_demo dials the upstream DSes
# directly using deviceids in the forwarded layout.
#
# Codecs tested:
#   /ffv1-csm/      v1 plain mirror across all 6 DSes
#   /ffv1-stripes/  v1 striped k=6 m=0 (no parity)
#   /ffv2-csm/      v2 plain mirror via CHUNK ops
#   /ffv2-rs/       v2 Reed-Solomon k=4 m=2
#   /ffv2-mj/       v2 Mojette systematic k=4 m=2 (re-enabled
#                   after task #147's variable-shard ds_stride
#                   ceiling-divide fix)
#
# Per-SB paths -- each codec lands on its own SB so the matrix
# exercises the listener mount-crossing path through PS instead of
# the root-SB sidestep that the early matrix used.  setup-sbs.sh
# configures each SB with the right layout-types / dstores /
# stripe-unit before this script runs.  The mount-crossing path
# itself is unit-pinned by lib/nfs4/ps/tests/ps_sb_alloc_test.c
# test_lookup_mount_cross_without_root_binding (task #149's
# regression test).
#
# Usage: run-ps-demo-codecs.sh <ec_demo_path> <ps_a_host> <ps_b_host>

set -u

EC_DEMO="${1:-/shared/build/tools/ec_demo}"
# Use host:port form so mds_session_clnt_open targets port 2049
# directly (the proxy listener), bypassing rpcbind which would
# resolve NFS_PROGRAM to the PS's native listener (port 12050).
PS_A="${2:-reffs-ps-a}:2049"
PS_B="${3:-reffs-ps-b}:2049"

PAYLOAD_SIZE=$((96 * 1024))
PAYLOAD="/tmp/codec_payload.bin"

# Mojette runs at the same 96 KiB payload as the other codecs by
# routing through ec_demo --shard-size 24576 (k=4 -> 24 KiB per
# data shard, one stripe per file).  See
# .claude/design/mojette-24k-shards.md.  Slice A
# (commit 258c88534693) parameterised shard_size in ec_pipeline
# and pinned the FINALIZE math; this enables the previously-gated
# 24 KiB Mojette case in the cross-PS matrix.
MJ_SHARD_SIZE=$((24 * 1024))

dd if=/dev/urandom of="$PAYLOAD" bs="$PAYLOAD_SIZE" count=1 status=none

declare -A RESULTS
declare -a ORDER

run_codec() {
    local label="$1"
    local sb_path="$2"
    local layout="$3"
    local write_op="$4"
    local payload="$5"
    local payload_size="$6"
    shift 6
    local args=( "$@" )

    local fname="${sb_path}/codec_${label}.bin"
    local out="/tmp/out_${label}.bin"
    local logw="/tmp/logw_${label}.txt"
    local logr="/tmp/logr_${label}.txt"

    ORDER+=("$label")
    rm -f "$out"

    echo "--- $label  layout=$layout  ${args[*]}  size=${payload_size}  write via PS-A, read via PS-B ---"

    if ! "$EC_DEMO" "$write_op" \
            --mds "$PS_A" --layout "$layout" \
            --file "$fname" --input "$payload" \
            "${args[@]}" \
            >"$logw" 2>&1; then
        echo "  WRITE FAILED -- log:"
        sed 's/^/    /' "$logw"
        RESULTS[$label]="FAIL (write via PS-A)"
        return
    fi

    local read_op
    case "$write_op" in
        put)   read_op=get ;;
        write) read_op=read ;;
        *)     RESULTS[$label]="FAIL (internal)"; return ;;
    esac

    if ! "$EC_DEMO" "$read_op" \
            --mds "$PS_B" --layout "$layout" \
            --file "$fname" --output "$out" --size "$payload_size" \
            "${args[@]}" \
            >"$logr" 2>&1; then
        echo "  READ FAILED -- log:"
        sed 's/^/    /' "$logr"
        RESULTS[$label]="FAIL (read via PS-B)"
        return
    fi

    if cmp -s "$payload" "$out"; then
        echo "  PASS"
        RESULTS[$label]="PASS"
    else
        local in_size out_size
        in_size=$(stat -c%s "$payload")
        out_size=$(stat -c%s "$out")
        echo "  DIFF MISMATCH (in=$in_size out=$out_size)"
        echo "  write log:"
        sed 's/^/    /' "$logw"
        echo "  read log:"
        sed 's/^/    /' "$logr"
        RESULTS[$label]="FAIL (diff)"
    fi
}

main() {
    echo "=== PS multi-codec demo: write via $PS_A, read via $PS_B (payload=${PAYLOAD_SIZE}) ==="

    run_codec ffv1-plain    /ffv1-csm     v1   put \
              "$PAYLOAD"    "$PAYLOAD_SIZE"
    run_codec ffv1-stripe   /ffv1-stripes v1   write \
              "$PAYLOAD"    "$PAYLOAD_SIZE" --codec stripe --k 6 --m 0
    run_codec ffv2-plain    /ffv2-csm     v2   put \
              "$PAYLOAD"    "$PAYLOAD_SIZE"
    run_codec ffv2-rs       /ffv2-rs      v2   write \
              "$PAYLOAD"    "$PAYLOAD_SIZE" --codec rs --k 4 --m 2
    run_codec ffv2-mj       /ffv2-mj      v2   write \
              "$PAYLOAD"    "$PAYLOAD_SIZE" --codec mojette-sys \
              --k 4 --m 2 --shard-size "$MJ_SHARD_SIZE"

    echo
    echo "=== PS-demo codecs result matrix ==="
    local fails=0
    for c in "${ORDER[@]}"; do
        printf "  %-18s %s\n" "$c" "${RESULTS[$c]}"
        if [[ "${RESULTS[$c]}" != PASS ]]; then
            fails=$((fails + 1))
        fi
    done

    if (( fails == 0 )); then
        echo "ALL PASS"
        exit 0
    else
        echo "$fails codec(s) FAILED"
        exit 1
    fi
}

main "$@"
