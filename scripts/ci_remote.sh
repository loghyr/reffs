#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# ci_remote.sh -- Run CI test suites against a pre-existing NFS server.
#
# The server is already running (systemd service or manual).  The mounts
# are already present.  This script does NOT mount/unmount or start/stop
# the server.  It just runs test suites and reports results.
#
# Usage:
#   scripts/ci_remote.sh [OPTIONS]
#
# Options:
#   --server HOST      NFS server hostname (default: localhost)
#   --port PORT        NFS server port (default: 2049)
#   --v4-mount PATH    NFSv4.2 mount point (default: /mnt/reffs_v4)
#   --v3-mount PATH    NFSv3 mount point (default: /mnt/reffs_v3)
#   --results DIR      Results directory (default: /reffs_data/ci_remote)
#   --email ADDR       Email results (default: none)
#
# Quick run:
#   scripts/ci_remote.sh
#
# Cron (every 4 hours against System A):
#   0 */4 * * * /home/loghyr/reffs/scripts/ci_remote.sh 2>&1

set -uo pipefail

# -- Defaults --
SERVER="localhost"
PORT=2049
V4_MOUNT="/mnt/reffs_v4"
V3_MOUNT="/mnt/reffs_v3"
RESULTS_BASE="/reffs_data/ci_remote"
EMAIL=""

while [[ $# -gt 0 ]]; do
    case "$1" in
    --server)    SERVER="$2"; shift 2 ;;
    --port)      PORT="$2"; shift 2 ;;
    --v4-mount)  V4_MOUNT="$2"; shift 2 ;;
    --v3-mount)  V3_MOUNT="$2"; shift 2 ;;
    --results)   RESULTS_BASE="$2"; shift 2 ;;
    --email)     EMAIL="$2"; shift 2 ;;
    *)           echo "Unknown: $1"; exit 1 ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/.." && pwd)"
DATE=$(date +%Y%m%d-%H%M%S)
LOGDIR="$RESULTS_BASE/$DATE"
mkdir -p "$LOGDIR" 2>/dev/null || LOGDIR="/tmp/ci_remote_$DATE"
mkdir -p "$LOGDIR"

# Also write results to the server's /results if available
SERVER_RESULTS="$V4_MOUNT/results"
if [ -d "$SERVER_RESULTS" ]; then
    SERVER_LOGDIR="$SERVER_RESULTS/$(hostname -s)_$DATE"
    mkdir -p "$SERVER_LOGDIR" 2>/dev/null || SERVER_LOGDIR=""
else
    SERVER_LOGDIR=""
fi

LOG="$LOGDIR/ci_remote.log"
exec > >(tee "$LOG") 2>&1

HOSTNAME=$(hostname -s)
PASSED=0
FAILED=0
CI_START=$(date +%s)

info() { echo "[$(date +%H:%M:%S)] $*"; }

record() {
    local name=$1 rc=$2
    local end=$(date +%s)
    local start_var="_section_${name}_start"
    local start=${!start_var:-$CI_START}
    local dur=$((end - start))
    if [ "$rc" -eq 0 ]; then
        echo "  PASS: $name (${dur}s)"
        PASSED=$((PASSED + 1))
    else
        echo "  FAIL: $name (exit $rc) (${dur}s)"
        FAILED=$((FAILED + 1))
    fi
}

section_start() {
    eval "_section_${1}_start=$(date +%s)"
    echo ""
    echo "=== $2 [$(date +%H:%M:%S)] ==="
}

# -----------------------------------------------------------------------
# Preflight
# -----------------------------------------------------------------------

echo "=== Remote CI: $HOSTNAME → $SERVER $DATE ==="
echo "  NFSv4.2 mount: $V4_MOUNT"
echo "  NFSv3 mount:   $V3_MOUNT"
echo ""

# Verify mounts exist
if ! mountpoint -q "$V4_MOUNT" 2>/dev/null; then
    echo "FATAL: $V4_MOUNT is not mounted"
    exit 1
fi
if ! mountpoint -q "$V3_MOUNT" 2>/dev/null; then
    echo "FATAL: $V3_MOUNT is not mounted"
    exit 1
fi

# Quick smoke test
if ! ls "$V4_MOUNT" >/dev/null 2>&1; then
    echo "FATAL: $V4_MOUNT not accessible"
    exit 1
fi

# -----------------------------------------------------------------------
# Test suites
# -----------------------------------------------------------------------

# pynfs
if [ -x "$REPO/scripts/ci_pynfs.sh" ]; then
    section_start pynfs "pynfs"
    "$REPO/scripts/ci_pynfs.sh" --server "$SERVER" --port "$PORT" \
        2>&1 | tee "$LOGDIR/pynfs.log" | tail -20
    record "pynfs" ${PIPESTATUS[0]}
fi

# CTHON04
if [ -x "$REPO/scripts/ci_cthon04_test.sh" ]; then
    section_start cthon04 "CTHON04"
    "$REPO/scripts/ci_cthon04_test.sh" \
        --v3-mount "$V3_MOUNT" --v4-mount "$V4_MOUNT" \
        2>&1 | tee "$LOGDIR/cthon04.log" | tail -20
    record "cthon04" ${PIPESTATUS[0]}
fi

# pjdfstest
if [ -x "$REPO/scripts/ci_pjdfstest.sh" ]; then
    section_start pjdfstest "pjdfstest"
    "$REPO/scripts/ci_pjdfstest.sh" \
        --v3-mount "$V3_MOUNT" --v4-mount "$V4_MOUNT" \
        2>&1 | tee "$LOGDIR/pjdfstest.log" | tail -20
    record "pjdfstest" ${PIPESTATUS[0]}
fi

# wardtest (5 min with verify phase)
if [ -x "$REPO/scripts/ci_wardtest.sh" ]; then
    section_start wardtest "wardtest"
    "$REPO/scripts/ci_wardtest.sh" --mount "$V4_MOUNT" --duration 300 \
        2>&1 | tee "$LOGDIR/wardtest.log" | tail -20
    record "wardtest" ${PIPESTATUS[0]}
fi

# -----------------------------------------------------------------------
# Build-on-NFS: exercises hardlinks, large writes, readdir, renames
# -----------------------------------------------------------------------

section_start build_on_nfs "Build-on-NFS (NFSv4.2)"
SRC_DIR="$REPO"
BUILD_NFS_DIR="$V4_MOUNT/ci_build_$$"
(
    set -e
    git clone --quiet "$SRC_DIR" "$BUILD_NFS_DIR"
    cd "$BUILD_NFS_DIR"

    # Verify clone integrity
    sum_nfs=$(md5sum configure.ac | awk '{print $1}')
    sum_src=$(md5sum "$SRC_DIR/configure.ac" | awk '{print $1}')
    if [ "$sum_nfs" != "$sum_src" ]; then
        echo "FAIL: md5sum mismatch: $sum_nfs != $sum_src"
        exit 1
    fi
    echo "Clone integrity OK: $sum_nfs"

    # Build on the NFS mount
    mkdir -p m4
    autoreconf -fi >/dev/null 2>&1
    mkdir -p build_nfs
    cd build_nfs
    ../configure --disable-asan --disable-ubsan >/dev/null 2>&1
    make -j$(nproc) 2>&1 | tail -3
    echo "Build-on-NFS: make succeeded"
) 2>&1 | tee "$LOGDIR/build_on_nfs.log" | tail -10
BUILD_NFS_RC=${PIPESTATUS[0]}
rm -rf "$BUILD_NFS_DIR" 2>/dev/null || true
record "build_on_nfs" $BUILD_NFS_RC

# -----------------------------------------------------------------------
# SEEK test: write a file with holes, verify SEEK_HOLE/SEEK_DATA
# -----------------------------------------------------------------------

section_start seek_test "SEEK (hole/data)"
SEEK_DIR="$V4_MOUNT/ci_seek_$$"
(
    set -e
    mkdir -p "$SEEK_DIR"

    # Create a file with a hole: write at offset 0, skip 1MB, write again
    TESTF="$SEEK_DIR/holey"
    dd if=/dev/urandom of="$TESTF" bs=4096 count=1 2>/dev/null
    dd if=/dev/urandom of="$TESTF" bs=4096 count=1 seek=256 2>/dev/null

    # Verify the file has the right size (256*4096 + 4096 = 1052672)
    SIZE=$(stat -c %s "$TESTF")
    if [ "$SIZE" -ne 1052672 ]; then
        echo "FAIL: expected size 1052672, got $SIZE"
        exit 1
    fi

    # Use SEEK_HOLE/SEEK_DATA via python (portable, no special tool)
    python3 -c "
import os
fd = os.open('$TESTF', os.O_RDONLY)
try:
    # SEEK_DATA from 0 should return 0 (data at start)
    pos = os.lseek(fd, 0, os.SEEK_DATA)
    assert pos == 0, f'SEEK_DATA from 0: expected 0, got {pos}'

    # SEEK_HOLE from 0 should find the hole after the first block
    pos = os.lseek(fd, 0, os.SEEK_HOLE)
    assert pos > 0, f'SEEK_HOLE from 0: expected >0, got {pos}'
    print(f'SEEK_HOLE from 0 = {pos} (hole starts after first data block)')

    # SEEK_DATA from the hole region should find the second data block
    pos = os.lseek(fd, 8192, os.SEEK_DATA)
    assert pos > 0, f'SEEK_DATA from 8192: expected >0, got {pos}'
    print(f'SEEK_DATA from 8192 = {pos} (second data block)')

    print('SEEK test: PASS')
finally:
    os.close(fd)
"
) 2>&1 | tee "$LOGDIR/seek_test.log" | tail -10
SEEK_RC=${PIPESTATUS[0]}
rm -rf "$SEEK_DIR" 2>/dev/null || true
record "seek_test" $SEEK_RC

# -----------------------------------------------------------------------
# Space accounting: du, df, stat consistency
# -----------------------------------------------------------------------

if [ -x "$REPO/scripts/ci_space_test.sh" ]; then
    section_start space_test "Space accounting (du/df/stat)"
    "$REPO/scripts/ci_space_test.sh" "$V4_MOUNT" "$V3_MOUNT" \
        2>&1 | tee "$LOGDIR/space_test.log" | tail -20
    record "space_test" ${PIPESTATUS[0]}
fi

# -----------------------------------------------------------------------
# Summary
# -----------------------------------------------------------------------

CI_END=$(date +%s)
CI_ELAPSED=$(( CI_END - CI_START ))

echo ""
echo "========================================"
echo "=== Remote CI Summary: $HOSTNAME → $SERVER"
echo "=== Started: $DATE  Duration: $((CI_ELAPSED / 60))m $((CI_ELAPSED % 60))s"
echo "========================================"
echo "  PASSED:  $PASSED"
echo "  FAILED:  $FAILED"
echo "  Log dir: $LOGDIR"
echo "========================================"

# -----------------------------------------------------------------------
# Copy results to server
# -----------------------------------------------------------------------

if [ -n "$SERVER_LOGDIR" ]; then
    cp "$LOGDIR"/*.log "$SERVER_LOGDIR/" 2>/dev/null || true
    info "Results copied to $SERVER_LOGDIR"
fi

# -----------------------------------------------------------------------
# Email (optional)
# -----------------------------------------------------------------------

if [ -n "$EMAIL" ] && command -v msmtp >/dev/null 2>&1; then
    SUBJECT="CI Remote: $HOSTNAME → $SERVER — $PASSED pass, $FAILED fail"
    {
        echo "Subject: $SUBJECT"
        echo "From: reffs-ci@$HOSTNAME"
        echo "To: $EMAIL"
        echo ""
        tail -20 "$LOG"
    } | msmtp "$EMAIL" 2>/dev/null || \
        info "Email send failed (msmtp)"
fi

if [ "$FAILED" -gt 0 ]; then
    exit 1
fi
