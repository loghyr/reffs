#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Capture a RENAME compound on the wire for comparison with knfsd.
# Run inside the CI container with reffsd already running.

set -euo pipefail

REFFSD_BIN=${1:-/build/src/reffsd}
WORK_DIR=${REFFS_WORK_DIR:-/reffs_data}
MOUNT=/mnt/reffs_capture
PCAP="$WORK_DIR/reffs_rename.pcap"
DECODED="$WORK_DIR/reffs_rename_decoded.txt"

mkdir -p "$WORK_DIR" "$MOUNT"

# Start reffsd
DATA="$WORK_DIR/capture_data"
STATE="$WORK_DIR/capture_state"
rm -rf "$DATA" "$STATE"
mkdir -p "$DATA" "$STATE"

cat > "$WORK_DIR/capture.toml" <<EOF
[server]
port           = 2049
bind           = "*"
role           = "standalone"
minor_versions = [1, 2]
grace_period   = 5
workers        = 4

[backend]
type       = "posix"
path       = "$DATA"
state_file = "$STATE"

[[export]]
path        = "/"
clients     = "*"
access      = "rw"
root_squash = false
flavors     = ["sys"]
EOF

rpcbind 2>/dev/null || true
sleep 1

"$REFFSD_BIN" --config="$WORK_DIR/capture.toml" >/dev/null 2>&1 &
REFFSD_PID=$!
sleep 2

# Mount
mount -o vers=4.2,sec=sys,actimeo=0 127.0.0.1:/ "$MOUNT"

# Create test directory and file
TESTDIR="$MOUNT/capture_test"
mkdir -p "$TESTDIR"
touch "$TESTDIR/file.0"
sync

# Start capture
tshark -i lo -f "port 2049" -w "$PCAP" &
TSHARK_PID=$!
sleep 1

# Do the rename
mv "$TESTDIR/file.0" "$TESTDIR/newfile.0"

# Check old name
stat "$TESTDIR/file.0" 2>&1 && echo "BUG: file.0 still exists" || echo "OK: file.0 gone"
stat "$TESTDIR/newfile.0" 2>&1 | head -1

sleep 1
kill "$TSHARK_PID" 2>/dev/null || true
wait "$TSHARK_PID" 2>/dev/null || true

# Decode
tshark -r "$PCAP" -Y "nfs.opcode==29" -V > "$DECODED" 2>/dev/null

echo ""
echo "=== RENAME change_info ==="
grep -A5 'source_cinfo\|target_cinfo\|changeid' "$DECODED" | head -20

# Cleanup
rm -rf "$TESTDIR"
umount -f "$MOUNT" 2>/dev/null || true
kill -TERM "$REFFSD_PID" 2>/dev/null || true
wait "$REFFSD_PID" 2>/dev/null || true

echo ""
echo "Full decode: $DECODED"
echo "PCAP:        $PCAP"
