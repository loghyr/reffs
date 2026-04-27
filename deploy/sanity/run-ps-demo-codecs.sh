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
# Codecs tested (skips mojette-sys per task #147):
#   put          v1 plain mirror across all 6 DSes
#   stripe k=6   v1 striped (no parity)
#   put          v2 plain mirror via CHUNK ops
#   rs k=4 m=2   v2 Reed-Solomon
#
# Bare filenames at the MDS root SB (no per-SB paths) -- the root SB
# is configured by setup-sbs.sh / mds-setup with layout-types=both,
# dstores=1..6, stripe-unit=4096.  This sidesteps task #149
# (per-listener multi-SB mount-crossing).
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

dd if=/dev/urandom of="$PAYLOAD" bs="$PAYLOAD_SIZE" count=1 status=none

declare -A RESULTS
declare -a ORDER

run_codec() {
    local label="$1"
    local layout="$2"
    local write_op="$3"
    shift 3
    local args=( "$@" )

    local fname="codec_${label}.bin"
    local out="/tmp/out_${label}.bin"
    local logw="/tmp/logw_${label}.txt"
    local logr="/tmp/logr_${label}.txt"

    ORDER+=("$label")
    rm -f "$out"

    echo "--- $label  layout=$layout  ${args[*]}  write via PS-A, read via PS-B ---"

    if ! "$EC_DEMO" "$write_op" \
            --mds "$PS_A" --layout "$layout" \
            --file "$fname" --input "$PAYLOAD" \
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
            --file "$fname" --output "$out" --size "$PAYLOAD_SIZE" \
            "${args[@]}" \
            >"$logr" 2>&1; then
        echo "  READ FAILED -- log:"
        sed 's/^/    /' "$logr"
        RESULTS[$label]="FAIL (read via PS-B)"
        return
    fi

    if cmp -s "$PAYLOAD" "$out"; then
        echo "  PASS"
        RESULTS[$label]="PASS"
    else
        local in_size out_size
        in_size=$(stat -c%s "$PAYLOAD")
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

    run_codec ffv1-plain    v1   put
    run_codec ffv1-stripe   v1   write --codec stripe      --k 6 --m 0
    run_codec ffv2-plain    v2   put
    run_codec ffv2-rs       v2   write --codec rs          --k 4 --m 2

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
