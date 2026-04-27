#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Cross-PS demo via the Linux NFSv4.2 kernel client.
#
# Mounts PS-A and PS-B as separate NFSv4.2 mounts, writes a file
# through PS-A, syncs, reads the same file through PS-B, byte-exact
# cmp.  No codecs / no LAYOUTGET in this path -- the kernel client
# speaks plain READ/WRITE which the existing ps_proxy_forward_*
# chain handles.  This is the workaround for tasks #149 + #150
# (multi-SB mount-crossing + layout passthrough) so the user can see
# basic cross-PS proxying work end-to-end.
#
# Usage: run-ps-demo-kmount.sh <ps_a_host> <ps_b_host>

set -u

PS_A="${1:-reffs-ps-a}"
PS_B="${2:-reffs-ps-b}"
MOUNT_A="/mnt/ps_a"
MOUNT_B="/mnt/ps_b"
MOUNT_OPTS="vers=4.2,proto=tcp,nolock,sec=sys,nordirplus,timeo=200,retrans=2"

PAYLOADS=(4096 65536 524288 1048576)

cleanup() {
    umount -f "$MOUNT_A" 2>/dev/null || true
    umount -f "$MOUNT_B" 2>/dev/null || true
}
trap cleanup EXIT

mkdir -p "$MOUNT_A" "$MOUNT_B"

echo "=== Mounting PS-A ($PS_A:/) at $MOUNT_A and PS-B ($PS_B:/) at $MOUNT_B ==="
if ! mount -t nfs4 -o "$MOUNT_OPTS" "$PS_A:/" "$MOUNT_A"; then
    echo "FAIL: mount PS-A"
    exit 1
fi
if ! mount -t nfs4 -o "$MOUNT_OPTS" "$PS_B:/" "$MOUNT_B"; then
    echo "FAIL: mount PS-B"
    exit 1
fi

echo "=== Mount points ==="
mount | grep -E "$MOUNT_A|$MOUNT_B" | sed 's/^/  /'

declare -A RESULTS
declare -a TEST_ORDER

run_one() {
    local size="$1"
    local fname="kmount_${size}.bin"
    local label="size=${size}"
    TEST_ORDER+=("$label")

    local src="/tmp/src_${size}.bin"
    local dst="/tmp/dst_${size}.bin"
    rm -f "$src" "$dst"
    dd if=/dev/urandom of="$src" bs="$size" count=1 status=none

    echo "--- $label : write via PS-A, read via PS-B ---"

    # Write through PS-A's mount
    if ! cp "$src" "$MOUNT_A/$fname" 2>&1; then
        RESULTS[$label]="FAIL (write via PS-A)"
        return
    fi
    sync

    # Read back through PS-B's mount
    if ! cp "$MOUNT_B/$fname" "$dst" 2>&1; then
        RESULTS[$label]="FAIL (read via PS-B)"
        return
    fi

    if cmp -s "$src" "$dst"; then
        echo "  PASS"
        RESULTS[$label]="PASS"
    else
        local sz_in sz_out
        sz_in=$(stat -c%s "$src")
        sz_out=$(stat -c%s "$dst")
        echo "  DIFF MISMATCH (in=$sz_in out=$sz_out)"
        RESULTS[$label]="FAIL (diff)"
    fi
}

main() {
    for sz in "${PAYLOADS[@]}"; do
        run_one "$sz"
    done

    echo
    echo "=== PS-demo kmount result matrix ==="
    local fails=0
    for t in "${TEST_ORDER[@]}"; do
        printf "  %-18s %s\n" "$t" "${RESULTS[$t]}"
        if [[ "${RESULTS[$t]}" != PASS ]]; then
            fails=$((fails + 1))
        fi
    done

    if (( fails == 0 )); then
        echo "ALL PASS"
        exit 0
    else
        echo "$fails test(s) FAILED"
        exit 1
    fi
}

main "$@"
