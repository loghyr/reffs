#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# ci_space_test.sh -- Verify space accounting (du, df, stat) on NFS.
#
# Creates files, verifies sizes, block counts, and filesystem usage
# go up.  Removes files, verifies they go down.  Assumes single
# writer (no concurrent modifications).
#
# Usage:
#   scripts/ci_space_test.sh MOUNT_PATH
#
# Example:
#   scripts/ci_space_test.sh /mnt/reffs_v4

set -euo pipefail

MOUNT=${1:?Usage: ci_space_test.sh MOUNT_PATH}
TESTDIR="$MOUNT/ci_space_$$"
PASS=0
FAIL=0

die() { echo "FAIL: $*" >&2; FAIL=$((FAIL + 1)); }
ok()  { echo "  OK: $*"; PASS=$((PASS + 1)); }
info() { echo "[space] $*"; }

cleanup() {
    rm -rf "$TESTDIR" 2>/dev/null || true
}
trap cleanup EXIT

# -----------------------------------------------------------------------
# Helpers
# -----------------------------------------------------------------------

# Get space_used (bytes) for a file via stat
file_blocks_bytes() {
    local f=$1
    # st_blocks is in 512-byte units
    local blocks
    blocks=$(stat -c %b "$f")
    echo $((blocks * 512))
}

file_size() {
    stat -c %s "$1"
}

# Get filesystem used bytes from df
fs_used_bytes() {
    # df -B1 reports in bytes; Used is column 3
    df -B1 "$MOUNT" | tail -1 | awk '{print $3}'
}

# Get filesystem available bytes from df
fs_avail_bytes() {
    df -B1 "$MOUNT" | tail -1 | awk '{print $4}'
}

# Get du -sb (apparent size in bytes) for a path
du_bytes() {
    du -sb "$1" 2>/dev/null | awk '{print $1}'
}

# -----------------------------------------------------------------------
# Setup
# -----------------------------------------------------------------------

info "=== Space Accounting Test ==="
info "Mount: $MOUNT"
info "Test dir: $TESTDIR"

mkdir -p "$TESTDIR"

# Record baseline
BASELINE_FS_USED=$(fs_used_bytes)
BASELINE_FS_AVAIL=$(fs_avail_bytes)
info "Baseline: fs_used=${BASELINE_FS_USED} fs_avail=${BASELINE_FS_AVAIL}"

# -----------------------------------------------------------------------
# Test 1: Create a known-size file, verify stat
# -----------------------------------------------------------------------

info ""
info "--- Test 1: Create 1MB file, verify stat ---"

dd if=/dev/urandom of="$TESTDIR/file1" bs=1024 count=1024 2>/dev/null
# Close ensures LAYOUTRETURN fires; sleep gives the server time to
# update i_used from the DS reflected GETATTR.  The kernel NFS
# client caches attrs, so we stat twice with a gap.
sync
sleep 2

SIZE=$(file_size "$TESTDIR/file1")
BLOCKS=$(file_blocks_bytes "$TESTDIR/file1")

if [ "$SIZE" -eq 1048576 ]; then
    ok "file1 size = $SIZE (expected 1048576)"
else
    die "file1 size = $SIZE (expected 1048576)"
fi

if [ "$BLOCKS" -gt 0 ]; then
    ok "file1 blocks = $BLOCKS bytes (> 0)"
else
    die "file1 blocks = $BLOCKS bytes (expected > 0)"
fi

# blocks should be >= size (allocated storage >= logical size)
if [ "$BLOCKS" -ge "$SIZE" ]; then
    ok "file1 blocks ($BLOCKS) >= size ($SIZE)"
else
    die "file1 blocks ($BLOCKS) < size ($SIZE)"
fi

# -----------------------------------------------------------------------
# Test 2: df used should increase after file creation
# -----------------------------------------------------------------------

info ""
info "--- Test 2: df used increases ---"

AFTER_CREATE_USED=$(fs_used_bytes)
DELTA=$((AFTER_CREATE_USED - BASELINE_FS_USED))

info "fs_used: before=$BASELINE_FS_USED after=$AFTER_CREATE_USED delta=$DELTA"

if [ "$DELTA" -gt 0 ]; then
    ok "fs_used increased by $DELTA bytes"
else
    die "fs_used did not increase (delta=$DELTA)"
fi

# Delta should be at least the file size
if [ "$DELTA" -ge 1048576 ]; then
    ok "fs_used delta ($DELTA) >= file size (1048576)"
else
    die "fs_used delta ($DELTA) < file size (1048576)"
fi

# -----------------------------------------------------------------------
# Test 3: df avail should decrease
# -----------------------------------------------------------------------

info ""
info "--- Test 3: df avail decreases ---"

AFTER_CREATE_AVAIL=$(fs_avail_bytes)
AVAIL_DELTA=$((BASELINE_FS_AVAIL - AFTER_CREATE_AVAIL))

info "fs_avail: before=$BASELINE_FS_AVAIL after=$AFTER_CREATE_AVAIL delta=$AVAIL_DELTA"

if [ "$AVAIL_DELTA" -gt 0 ]; then
    ok "fs_avail decreased by $AVAIL_DELTA bytes"
else
    die "fs_avail did not decrease (delta=$AVAIL_DELTA)"
fi

# -----------------------------------------------------------------------
# Test 4: du matches file size
# -----------------------------------------------------------------------

info ""
info "--- Test 4: du matches ---"

DU=$(du_bytes "$TESTDIR/file1")
info "du -sb file1 = $DU"

if [ "$DU" -ge 1048576 ]; then
    ok "du ($DU) >= file size (1048576)"
else
    die "du ($DU) < file size (1048576)"
fi

# -----------------------------------------------------------------------
# Test 5: Create more files, verify cumulative accounting
# -----------------------------------------------------------------------

info ""
info "--- Test 5: Create 5 more files (512KB each) ---"

for i in $(seq 2 6); do
    dd if=/dev/urandom of="$TESTDIR/file$i" bs=1024 count=512 2>/dev/null
done
sync
sleep 2

AFTER_MULTI_USED=$(fs_used_bytes)
MULTI_DELTA=$((AFTER_MULTI_USED - AFTER_CREATE_USED))
EXPECTED=$((512 * 1024 * 5))

info "fs_used: after_multi=$AFTER_MULTI_USED delta=$MULTI_DELTA expected=$EXPECTED"

if [ "$MULTI_DELTA" -ge "$EXPECTED" ]; then
    ok "fs_used increased by $MULTI_DELTA >= $EXPECTED (5 x 512KB)"
else
    die "fs_used delta ($MULTI_DELTA) < expected ($EXPECTED)"
fi

DU_DIR=$(du_bytes "$TESTDIR")
TOTAL_EXPECTED=$((1048576 + EXPECTED))
info "du -sb testdir = $DU_DIR (expected >= $TOTAL_EXPECTED)"

if [ "$DU_DIR" -ge "$TOTAL_EXPECTED" ]; then
    ok "du testdir ($DU_DIR) >= total expected ($TOTAL_EXPECTED)"
else
    die "du testdir ($DU_DIR) < total expected ($TOTAL_EXPECTED)"
fi

# -----------------------------------------------------------------------
# Test 6: Remove files, verify accounting goes down
# -----------------------------------------------------------------------

info ""
info "--- Test 6: Remove all files, verify accounting decreases ---"

BEFORE_REMOVE_USED=$(fs_used_bytes)

rm -f "$TESTDIR"/file*
sync
# Server may batch space reclamation; allow settle time.
sleep 2

AFTER_REMOVE_USED=$(fs_used_bytes)
REMOVE_DELTA=$((BEFORE_REMOVE_USED - AFTER_REMOVE_USED))

info "fs_used: before_remove=$BEFORE_REMOVE_USED after_remove=$AFTER_REMOVE_USED delta=$REMOVE_DELTA"

if [ "$REMOVE_DELTA" -gt 0 ]; then
    ok "fs_used decreased by $REMOVE_DELTA after remove"
else
    die "fs_used did not decrease after remove (delta=$REMOVE_DELTA)"
fi

# Should return close to baseline (within one block size tolerance)
DRIFT=$((AFTER_REMOVE_USED - BASELINE_FS_USED))
# Allow up to 1MB drift for directory metadata
if [ "$DRIFT" -lt 1048576 ]; then
    ok "fs_used returned near baseline (drift=$DRIFT)"
else
    die "fs_used drift from baseline = $DRIFT (> 1MB tolerance)"
fi

# -----------------------------------------------------------------------
# Test 7: df avail should recover
# -----------------------------------------------------------------------

info ""
info "--- Test 7: df avail recovers after remove ---"

AFTER_REMOVE_AVAIL=$(fs_avail_bytes)
RECOVER_DRIFT=$((BASELINE_FS_AVAIL - AFTER_REMOVE_AVAIL))

info "fs_avail: baseline=$BASELINE_FS_AVAIL after_remove=$AFTER_REMOVE_AVAIL drift=$RECOVER_DRIFT"

if [ "$RECOVER_DRIFT" -lt 1048576 ]; then
    ok "fs_avail recovered (drift=$RECOVER_DRIFT < 1MB)"
else
    die "fs_avail drift = $RECOVER_DRIFT (> 1MB tolerance)"
fi

# -----------------------------------------------------------------------
# Test 8: Empty file has 0 blocks
# -----------------------------------------------------------------------

info ""
info "--- Test 8: Empty file has 0 blocks ---"

touch "$TESTDIR/empty"
sync

EMPTY_SIZE=$(file_size "$TESTDIR/empty")
EMPTY_BLOCKS=$(file_blocks_bytes "$TESTDIR/empty")

if [ "$EMPTY_SIZE" -eq 0 ]; then
    ok "empty file size = 0"
else
    die "empty file size = $EMPTY_SIZE (expected 0)"
fi

if [ "$EMPTY_BLOCKS" -eq 0 ]; then
    ok "empty file blocks = 0"
else
    die "empty file blocks = $EMPTY_BLOCKS (expected 0)"
fi

rm -f "$TESTDIR/empty"

# -----------------------------------------------------------------------
# Test 9: Truncate grows then shrinks
# -----------------------------------------------------------------------

info ""
info "--- Test 9: Truncate accounting ---"

truncate -s 2M "$TESTDIR/trunc"
sync

TRUNC_SIZE=$(file_size "$TESTDIR/trunc")
TRUNC_BLOCKS=$(file_blocks_bytes "$TESTDIR/trunc")

if [ "$TRUNC_SIZE" -eq 2097152 ]; then
    ok "truncated file size = $TRUNC_SIZE"
else
    die "truncated file size = $TRUNC_SIZE (expected 2097152)"
fi

if [ "$TRUNC_BLOCKS" -gt 0 ]; then
    ok "truncated file blocks = $TRUNC_BLOCKS (> 0)"
else
    die "truncated file blocks = $TRUNC_BLOCKS (expected > 0)"
fi

# Shrink
truncate -s 100 "$TESTDIR/trunc"
sync

SHRUNK_SIZE=$(file_size "$TESTDIR/trunc")
SHRUNK_BLOCKS=$(file_blocks_bytes "$TESTDIR/trunc")

if [ "$SHRUNK_SIZE" -eq 100 ]; then
    ok "shrunk file size = $SHRUNK_SIZE"
else
    die "shrunk file size = $SHRUNK_SIZE (expected 100)"
fi

if [ "$SHRUNK_BLOCKS" -lt "$TRUNC_BLOCKS" ]; then
    ok "shrunk blocks ($SHRUNK_BLOCKS) < original ($TRUNC_BLOCKS)"
else
    die "shrunk blocks ($SHRUNK_BLOCKS) >= original ($TRUNC_BLOCKS)"
fi

rm -f "$TESTDIR/trunc"

# -----------------------------------------------------------------------
# Summary
# -----------------------------------------------------------------------

info ""
info "========================================"
info "=== Space Accounting: $PASS pass, $FAIL fail ==="
info "========================================"

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
