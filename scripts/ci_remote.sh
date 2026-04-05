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
mkdir -p "$LOGDIR"

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

# wardtest
if [ -x "$REPO/scripts/ci_wardtest.sh" ]; then
    section_start wardtest "wardtest"
    "$REPO/scripts/ci_wardtest.sh" --mount "$V4_MOUNT" --duration 60 \
        2>&1 | tee "$LOGDIR/wardtest.log" | tail -20
    record "wardtest" ${PIPESTATUS[0]}
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
