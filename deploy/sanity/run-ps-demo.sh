#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Cross-PS sanity demo: write a file via PS-A, read it back via PS-B,
# byte-exact diff.  Skips /ffv2-mj because the underlying mojette-sys
# CHUNK_READ failure (task #147) is unrelated to the proxy path.
#
# Per .claude/design/proxy-server.md, the PS proxies LOOKUP / OPEN /
# READ / WRITE / GETATTR / CLOSE through to the upstream MDS, which
# in turn issues the layout pointing at the DSes.  The 6-DS layout
# is invariant under which PS the client mounted.  If both PSes can
# resolve the same MDS-allocated layout, the file written by one is
# readable from the other byte-for-byte.
#
# Plan A: PROXY_REGISTRATION will fail with NFS4ERR_PERM in this
# topology (no GSS / TLS in the smoke).  The PSes log the rejection
# and continue without registered-PS privilege.  AUTH_SYS forward
# ops still work because the MDS authorises against the forwarded
# end-client credentials, not the PS's own session credentials.
#
# Usage: run-ps-demo.sh <ec_demo_path> <ps_a_host> <ps_b_host>

set -u

EC_DEMO="${1:-/shared/build/tools/ec_demo}"
PS_A="${2:-reffs-ps-a}"
PS_B="${3:-reffs-ps-b}"

PAYLOAD="/tmp/ps_demo_payload.bin"
PAYLOAD_SIZE=$((96 * 1024))

dd if=/dev/urandom of="$PAYLOAD" bs="$PAYLOAD_SIZE" count=1 status=none

declare -A RESULTS
declare -a SB_ORDER

# write_via_ps_a_read_via_ps_b <sb_path> <layout> <write_op> [extra args for both ops]
run_one() {
    local sb_path="$1"
    local layout="$2"
    local write_op="$3"
    shift 3
    local codec_args=( "$@" )

    local label="$sb_path"
    SB_ORDER+=("$label")

    local fname="${sb_path}/cross.bin"
    local out="/tmp/out_${sb_path//\//_}.bin"
    local logw="/tmp/logw_${sb_path//\//_}.txt"
    local logr="/tmp/logr_${sb_path//\//_}.txt"
    rm -f "$out"

    echo "--- $label  layout=$layout  ${codec_args[*]} ---"
    echo "    write via PS-A ($PS_A)"

    if ! "$EC_DEMO" "$write_op" \
            --mds "$PS_A" --layout "$layout" \
            --file "$fname" --input "$PAYLOAD" \
            "${codec_args[@]}" \
            >"$logw" 2>&1; then
        echo "  WRITE FAILED via PS-A -- log:"
        sed 's/^/    /' "$logw"
        RESULTS[$label]="FAIL (write via PS-A)"
        return
    fi

    # Map write op -> read op
    local read_op
    case "$write_op" in
        put)   read_op=get ;;
        write) read_op=read ;;
        *)     RESULTS[$label]="FAIL (internal: bad write op)"; return ;;
    esac

    echo "    read  via PS-B ($PS_B)"

    if ! "$EC_DEMO" "$read_op" \
            --mds "$PS_B" --layout "$layout" \
            --file "$fname" --output "$out" --size "$PAYLOAD_SIZE" \
            "${codec_args[@]}" \
            >"$logr" 2>&1; then
        echo "  READ FAILED via PS-B -- log:"
        sed 's/^/    /' "$logr"
        RESULTS[$label]="FAIL (read via PS-B)"
        return
    fi

    if cmp -s "$PAYLOAD" "$out"; then
        echo "  PASS"
        RESULTS[$label]="PASS"
    else
        local in_size out_size
        in_size=$(stat -c%s "$PAYLOAD" 2>/dev/null || stat -f%z "$PAYLOAD")
        out_size=$(stat -c%s "$out" 2>/dev/null || stat -f%z "$out")
        echo "  DIFF MISMATCH (in=$in_size out=$out_size)"
        echo "  write log:"
        sed 's/^/    /' "$logw"
        echo "  read log:"
        sed 's/^/    /' "$logr"
        RESULTS[$label]="FAIL (diff)"
    fi
}

main() {
    echo "=== PS demo: write via $PS_A, read via $PS_B (payload=${PAYLOAD_SIZE} bytes) ==="

    # Skip /ffv2-mj per task #147 (unrelated mojette-sys CHUNK_READ bug).
    run_one /ffv1-csm     v1   put
    run_one /ffv1-stripes v1   write --codec stripe      --k 6 --m 0
    run_one /ffv2-csm     v2   put
    run_one /ffv2-rs      v2   write --codec rs          --k 4 --m 2

    echo
    echo "=== PS-demo result matrix ==="
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
