#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# test_rename_v4.sh — Reproduce CTHON04 test7 rename bug.
#
# Mounts NFSv4.2 with actimeo=0, creates a file, renames it,
# then stats the old name.  If stat succeeds, the client cache
# is stale.
#
# Run inside the CI container with reffsd already running.

set -euo pipefail

MOUNT=${1:-/mnt/reffs_rename_test}
mkdir -p "$MOUNT"

echo "=== Rename test: NFSv4.2 vs NFSv3 ==="

# Cleanup
umount -f "$MOUNT" 2>/dev/null || true

# --- NFSv4.2 ---
echo ""
echo "--- NFSv4.2 (actimeo=0) ---"
mount -o vers=4.2,sec=sys,actimeo=0 127.0.0.1:/ "$MOUNT"

TESTDIR="$MOUNT/rename_test_$$"
mkdir -p "$TESTDIR"

# Create file
touch "$TESTDIR/file.0"

# Rename
mv "$TESTDIR/file.0" "$TESTDIR/newfile.0"

# Check old name — should NOT exist
if stat "$TESTDIR/file.0" >/dev/null 2>&1; then
    echo "FAIL: file.0 exists after rename (NFSv4.2)"
    V4_RESULT="FAIL"
else
    echo "PASS: file.0 does not exist after rename (NFSv4.2)"
    V4_RESULT="PASS"
fi

# Verify new name
if stat "$TESTDIR/newfile.0" >/dev/null 2>&1; then
    echo "PASS: newfile.0 exists (NFSv4.2)"
else
    echo "FAIL: newfile.0 does not exist (NFSv4.2)"
    V4_RESULT="FAIL"
fi

rm -rf "$TESTDIR"
umount -f "$MOUNT"

# --- NFSv3 ---
echo ""
echo "--- NFSv3 (nolock) ---"
mount -o vers=3,sec=sys,nolock,tcp,mountproto=tcp 127.0.0.1:/ "$MOUNT"

TESTDIR="$MOUNT/rename_test_$$"
mkdir -p "$TESTDIR"

touch "$TESTDIR/file.0"
mv "$TESTDIR/file.0" "$TESTDIR/newfile.0"

if stat "$TESTDIR/file.0" >/dev/null 2>&1; then
    echo "FAIL: file.0 exists after rename (NFSv3)"
    V3_RESULT="FAIL"
else
    echo "PASS: file.0 does not exist after rename (NFSv3)"
    V3_RESULT="PASS"
fi

if stat "$TESTDIR/newfile.0" >/dev/null 2>&1; then
    echo "PASS: newfile.0 exists (NFSv3)"
else
    echo "FAIL: newfile.0 does not exist (NFSv3)"
    V3_RESULT="FAIL"
fi

rm -rf "$TESTDIR"
umount -f "$MOUNT"

echo ""
echo "=== Results ==="
echo "  NFSv4.2: $V4_RESULT"
echo "  NFSv3:   $V3_RESULT"

if [ "$V4_RESULT" = "FAIL" ] || [ "$V3_RESULT" = "FAIL" ]; then
    exit 1
fi
