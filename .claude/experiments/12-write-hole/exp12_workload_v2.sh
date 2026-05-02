#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
# v2 (CHUNK ops) variant of exp12_workload.sh
set -u
KILL_MS=${1:-500}
LEASE_S=95
SIZE=$((1024 * 1024))
EC=/shared/build/tools/ec_demo
MDS=reffs-mds
FILE=whtest_v2_${KILL_MS}

dd if=/dev/urandom of=/tmp/X bs=$SIZE count=1 2>/dev/null
dd if=/dev/urandom of=/tmp/Y bs=$SIZE count=1 2>/dev/null

"$EC" write --mds "$MDS" --file "$FILE" --input /tmp/X \
    --k 4 --m 2 --codec rs --layout v2 2>/dev/null
"$EC" read --mds "$MDS" --file "$FILE" --output /tmp/cX \
    --k 4 --m 2 --codec rs --size $SIZE --layout v2 2>/dev/null
cmp -s /tmp/X /tmp/cX || { echo "BASELINE_FAIL"; exit 1; }

"$EC" write --mds "$MDS" --file "$FILE" --input /tmp/Y \
    --k 4 --m 2 --codec rs --layout v2 >/tmp/wr.out 2>/tmp/wr.err &
WPID=$!
sleep $(awk -v ms=$KILL_MS 'BEGIN{printf "%.3f\n", ms/1000.0}')
kill -9 $WPID 2>/dev/null; wait $WPID 2>/dev/null

echo "[v2 kill_ms=$KILL_MS] waiting ${LEASE_S}s for lease/CHUNK cleanup..."
sleep $LEASE_S

if "$EC" read --mds "$MDS" --file "$FILE" --output /tmp/post \
    --k 4 --m 2 --codec rs --size $SIZE --layout v2 2>/tmp/rd.err; then
    READ_OK=1
else
    READ_OK=0
fi

if [ $READ_OK -eq 0 ]; then OUTCOME=READ_FAILED
elif cmp -s /tmp/X /tmp/post; then OUTCOME=PRE_WRITE_X
elif cmp -s /tmp/Y /tmp/post; then OUTCOME=POST_WRITE_Y
else
    XL=$(cmp -l /tmp/X /tmp/post 2>/dev/null | wc -l)
    OUTCOME="MIXED(diff_X=$XL)"
fi
echo "RESULT_V2 kill_ms=$KILL_MS outcome=$OUTCOME read_ok=$READ_OK"
