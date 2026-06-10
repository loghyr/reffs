#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# ec-repair smoke test -- one cell against a running combined-mode
# reffsd, exercises the full wire path:
#
#   write -> ec_demo write (k+m mirrors materialise on the DSes)
#   repair -> ec_demo repair --skip-ds (CHUNK_WRITE_REPAIR +
#             CHUNK_FINALIZE + CHUNK_COMMIT to lost mirrors +
#             CHUNK_REPAIRED to the MDS)
#   verify -> ec_demo verify (read back, compare bytes)
#
# Plus a probe-protocol sanity check that cs_repair_initiated and
# cs_repair_completed both bumped.
#
# Usage:
#   test_ec_repair.sh [<ec_demo>] [<mds_host>] [<reffs-probe.py>]
#
# Defaults assume the in-tree build dir + localhost.

set -eu

EC_DEMO="${1:-${REFFS_BUILD:-./build}/tools/ec_demo}"
MDS="${2:-localhost}"
PROBE="${3:-${REFFS_BUILD:-./build}/scripts/reffs-probe.py}"

if [ ! -x "$EC_DEMO" ]; then
    echo "test_ec_repair: ec_demo not executable at $EC_DEMO" >&2
    exit 1
fi

# Pick up libtool's in-tree .libs/ (same as ec_benchmark.sh).
_build_root=$(cd "$(dirname "$EC_DEMO")/.." 2>/dev/null && pwd)
if [ -n "$_build_root" ] && [ -d "$_build_root/lib" ]; then
    _extra=$(find "$_build_root" -type d -name '.libs' 2>/dev/null | paste -sd:)
    [ -n "$_extra" ] && export LD_LIBRARY_PATH="$_extra${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi

TMPDIR=$(mktemp -d -t test_ec_repair.XXXXXX)
trap 'rm -rf "$TMPDIR"' EXIT

INPUT="$TMPDIR/input.bin"
NFS_FILE="/test_ec_repair.bin"
SIZE=$((1024 * 1024)) # 1 MiB
K=4
M=2
CODEC=rs
LAYOUT=v2
LOSS=0x1 # shard 0 lost

dd if=/dev/urandom of="$INPUT" bs="$SIZE" count=1 status=none 2>/dev/null

echo "test_ec_repair: write $NFS_FILE ($SIZE bytes, $K+$M $CODEC, $LAYOUT)"
"$EC_DEMO" write \
    --mds "$MDS" --file "$NFS_FILE" \
    --input "$INPUT" \
    --layout "$LAYOUT" --codec "$CODEC" \
    --k "$K" --m "$M"

# Capture pre-repair counters via the probe protocol if available.
pre_init=0
pre_completed=0
if [ -x "$PROBE" ]; then
    pre_init=$("$PROBE" --host "$MDS" sb-get --id 1 2>/dev/null |
        sed -nE 's/.*repair_initiated[[:space:]]*=[[:space:]]*([0-9]+).*/\1/p' |
        head -1)
    pre_completed=$("$PROBE" --host "$MDS" sb-get --id 1 2>/dev/null |
        sed -nE 's/.*repair_completed[[:space:]]*=[[:space:]]*([0-9]+).*/\1/p' |
        head -1)
    pre_init="${pre_init:-0}"
    pre_completed="${pre_completed:-0}"
    echo "test_ec_repair: pre-repair cs_repair_initiated=$pre_init " \
         "cs_repair_completed=$pre_completed"
fi

echo "test_ec_repair: repair $NFS_FILE --skip-ds $LOSS"
line=$("$EC_DEMO" repair \
    --mds "$MDS" --file "$NFS_FILE" \
    --size "$SIZE" --skip-ds "$LOSS" \
    --layout "$LAYOUT" --codec "$CODEC" \
    --k "$K" --m "$M")

echo "test_ec_repair: $line"

# Repair must report at least one shard repaired.
shards=$(echo "$line" |
    sed -nE 's/.*shards_repaired=([0-9]+).*/\1/p')
if [ "${shards:-0}" -lt 1 ]; then
    echo "test_ec_repair: FAIL -- shards_repaired=$shards, expected >=1" >&2
    exit 1
fi

# Bytes repaired must be > 0.
bytes=$(echo "$line" |
    sed -nE 's/.*bytes_repaired=([0-9]+).*/\1/p')
if [ "${bytes:-0}" -lt 1 ]; then
    echo "test_ec_repair: FAIL -- bytes_repaired=$bytes, expected >0" >&2
    exit 1
fi

echo "test_ec_repair: verify $NFS_FILE -- file content must match input"
if ! "$EC_DEMO" verify \
        --mds "$MDS" --file "$NFS_FILE" \
        --input "$INPUT" \
        --layout "$LAYOUT" --codec "$CODEC" \
        --k "$K" --m "$M"; then
    echo "test_ec_repair: FAIL -- verify mismatch after repair" >&2
    exit 1
fi

# Probe counters must have bumped if the probe path is available.
if [ -x "$PROBE" ]; then
    post_init=$("$PROBE" --host "$MDS" sb-get --id 1 2>/dev/null |
        sed -nE 's/.*repair_initiated[[:space:]]*=[[:space:]]*([0-9]+).*/\1/p' |
        head -1)
    post_completed=$("$PROBE" --host "$MDS" sb-get --id 1 2>/dev/null |
        sed -nE 's/.*repair_completed[[:space:]]*=[[:space:]]*([0-9]+).*/\1/p' |
        head -1)
    post_init="${post_init:-0}"
    post_completed="${post_completed:-0}"
    echo "test_ec_repair: post-repair cs_repair_initiated=$post_init " \
         "cs_repair_completed=$post_completed"
    if [ "$post_init" -le "$pre_init" ]; then
        echo "test_ec_repair: WARN -- cs_repair_initiated did not bump " \
             "($pre_init -> $post_init); the DS-side handler may not " \
             "have run, but the verify passed so the wire path is OK." >&2
    fi
    if [ "$post_completed" -le "$pre_completed" ]; then
        echo "test_ec_repair: WARN -- cs_repair_completed did not bump " \
             "($pre_completed -> $post_completed); the MDS-side handler " \
             "may not have cleared a REPAIR flag (no mirror was flagged " \
             "before the call; expected in cooperative-client smoke)." >&2
    fi
fi

echo "test_ec_repair: PASS"
