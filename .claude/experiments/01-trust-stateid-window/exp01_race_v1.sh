#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Trust-stateid race test: two ec_demo writers contend for the layout.
# Client A starts a 100 MB v1 RS write; while A is mid-write, Client B
# starts its own write to the same file (forces MDS to recall A's
# layout + REVOKE_STATEID at all DSes).  We observe outcomes.

set -u
EC=/shared/build/tools/ec_demo
MDS=reffs-mds
SIZE=$((100 * 1024 * 1024))   # 100 MB so write takes a few seconds
FILE=race_target

dd if=/dev/urandom of=/tmp/A bs=$SIZE count=1 2>/dev/null
dd if=/dev/urandom of=/tmp/B bs=$SIZE count=1 2>/dev/null

# Start client A in background
echo "[A] start at $(date +%H:%M:%S.%N)"
"$EC" write --mds "$MDS" --file "$FILE" --input /tmp/A \
    --k 4 --m 2 --codec rs --layout v1 --id clientA \
    >/tmp/A.out 2>/tmp/A.err &
APID=$!

# Wait briefly for A's LAYOUTGET + first CHUNK_WRITEs
sleep 0.5

# Start client B
echo "[B] start at $(date +%H:%M:%S.%N)"
"$EC" write --mds "$MDS" --file "$FILE" --input /tmp/B \
    --k 4 --m 2 --codec rs --layout v1 --id clientB \
    >/tmp/B.out 2>/tmp/B.err &
BPID=$!

# Wait for both
wait $APID; ARC=$?
wait $BPID; BRC=$?
echo "[A] exit rc=$ARC at $(date +%H:%M:%S.%N)"
echo "[B] exit rc=$BRC at $(date +%H:%M:%S.%N)"

echo "--- A.err ---"
tail -5 /tmp/A.err
echo "--- B.err ---"
tail -5 /tmp/B.err

# Read file back, see whose content won
sleep 1
"$EC" read --mds "$MDS" --file "$FILE" --output /tmp/post \
    --k 4 --m 2 --codec rs --size $SIZE --layout v1 \
    --id clientReader 2>/tmp/R.err
RR=$?
echo "--- read rc=$RR ---"
if cmp -s /tmp/A /tmp/post; then echo "FILE = A (B lost)"
elif cmp -s /tmp/B /tmp/post; then echo "FILE = B (A lost)"
else
    XA=$(cmp -l /tmp/A /tmp/post 2>/dev/null | wc -l)
    XB=$(cmp -l /tmp/B /tmp/post 2>/dev/null | wc -l)
    echo "FILE = MIXED (diff_A=$XA diff_B=$XB)"
fi
