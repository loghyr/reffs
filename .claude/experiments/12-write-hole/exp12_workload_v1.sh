#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Write-hole experiment: pre-write file with X, start writing Y,
# kill the writer at varying delays, wait > lease_period, read back,
# classify outcome (X / Y / MIXED / EMPTY / FAIL).
#
# Usage: bash exp12_workload.sh <kill_delay_ms>

set -u
KILL_MS=${1:-500}
LEASE_S=95   # bench mds.toml grace_period=5; lease default 90s -> wait 95s
SIZE=$((1024 * 1024))
EC_DEMO=/shared/build/tools/ec_demo
MDS=reffs-mds
FILE=whtest_${KILL_MS}

# Generate two distinct 1 MB blobs
dd if=/dev/urandom of=/tmp/X bs=$SIZE count=1 2>/dev/null
dd if=/dev/urandom of=/tmp/Y bs=$SIZE count=1 2>/dev/null

# Phase 1: write X to file, verify
"$EC_DEMO" write --mds "$MDS" --file "$FILE" --input /tmp/X \
    --k 4 --m 2 --codec rs --layout v1 2>/dev/null
"$EC_DEMO" read --mds "$MDS" --file "$FILE" --output /tmp/check_X \
    --k 4 --m 2 --codec rs --size $SIZE --layout v1 2>/dev/null
if ! cmp -s /tmp/X /tmp/check_X; then
    echo "BASELINE_FAIL: pre-write X did not round-trip"
    exit 1
fi

# Phase 2: start writing Y in background; kill after KILL_MS ms
"$EC_DEMO" write --mds "$MDS" --file "$FILE" --input /tmp/Y \
    --k 4 --m 2 --codec rs --layout v1 >/tmp/wr.out 2>/tmp/wr.err &
WPID=$!
sleep $(awk -v ms=$KILL_MS 'BEGIN{printf "%.3f\n", ms/1000.0}')
kill -9 $WPID 2>/dev/null
wait $WPID 2>/dev/null

# Phase 3: wait for lease to expire so MDS cleans up the dead session/layout
echo "[kill_ms=$KILL_MS] waiting ${LEASE_S}s for lease expiry..."
sleep $LEASE_S

# Phase 4: read back, classify
if "$EC_DEMO" read --mds "$MDS" --file "$FILE" --output /tmp/post \
    --k 4 --m 2 --codec rs --size $SIZE --layout v1 2>/tmp/rd.err; then
    READ_OK=1
else
    READ_OK=0
fi

if [ $READ_OK -eq 0 ]; then
    OUTCOME=READ_FAILED
elif cmp -s /tmp/X /tmp/post; then
    OUTCOME=PRE_WRITE_X
elif cmp -s /tmp/Y /tmp/post; then
    OUTCOME=POST_WRITE_Y
else
    # Compute overlap to classify mixed vs unreadable
    XL=$(cmp -l /tmp/X /tmp/post 2>/dev/null | wc -l)
    YL=$(cmp -l /tmp/Y /tmp/post 2>/dev/null | wc -l)
    OUTCOME="MIXED(diff_X=$XL diff_Y=$YL)"
fi

echo "RESULT kill_ms=$KILL_MS outcome=$OUTCOME read_ok=$READ_OK"
echo "--- read stderr ---"
head -3 /tmp/rd.err 2>/dev/null
