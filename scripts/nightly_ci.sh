#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# nightly_ci.sh — Full CI run on bare metal, no Docker.
#
# Crontab entry (e.g., 2am nightly):
#   0 2 * * * /home/loghyr/reffs/scripts/nightly_ci.sh 2>&1
#
# Prerequisites:
#   - /reffs_data writable
#   - sudo NOPASSWD for mount/umount/rpcbind/mkdir/chmod
#   - msmtp or mailx configured for email
#   - Build dependencies installed
#
# Features:
#   - Lock file prevents concurrent runs
#   - Stuck detection (kill after MAX_RUNTIME)
#   - Git pull + clean build
#   - Unit tests, pjdfstest (if available), soak (30 min)
#   - Email summary on completion

set -uo pipefail

# -----------------------------------------------------------------------
# Configuration
# -----------------------------------------------------------------------

REPO=/home/loghyr/reffs
BUILD=$REPO/build
RESULTS=/reffs_data/nightly
LOCKFILE=/reffs_data/reffs_ci.lock
MAX_RUNTIME=$((3 * 3600))  # 3 hours max
EMAIL="loghyr@gmail.com"
HOSTNAME=$(hostname -s)
NFS_PORT=12049

# -----------------------------------------------------------------------
# Lock / stuck detection
# -----------------------------------------------------------------------

cleanup_lock() {
    rm -f "$LOCKFILE"
}

if [ -f "$LOCKFILE" ]; then
    PID=$(cat "$LOCKFILE" 2>/dev/null)
    if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
        # Check if stuck (running longer than MAX_RUNTIME)
        START_TIME=$(stat -c %Y "$LOCKFILE" 2>/dev/null || echo 0)
        NOW=$(date +%s)
        ELAPSED=$((NOW - START_TIME))
        if [ "$ELAPSED" -gt "$MAX_RUNTIME" ]; then
            echo "Stuck CI run (PID $PID, ${ELAPSED}s). Killing."
            kill -TERM "$PID" 2>/dev/null
            sleep 5
            kill -KILL "$PID" 2>/dev/null
            rm -f "$LOCKFILE"
        else
            echo "CI already running (PID $PID, ${ELAPSED}s). Skipping."
            exit 0
        fi
    else
        # Stale lock
        rm -f "$LOCKFILE"
    fi
fi

echo $$ > "$LOCKFILE"
trap cleanup_lock EXIT

# -----------------------------------------------------------------------
# Setup
# -----------------------------------------------------------------------

DATE=$(date +%Y%m%d-%H%M%S)
LOGDIR="$RESULTS/$DATE"
mkdir -p "$LOGDIR"

LOG="$LOGDIR/ci.log"
exec > >(tee "$LOG") 2>&1

echo "=== Nightly CI: $HOSTNAME $DATE ==="
echo "Repo: $REPO"
echo "Results: $LOGDIR"

PASSED=0
FAILED=0
SKIPPED=0

record() {
    local name=$1 result=$2
    if [ "$result" -eq 0 ]; then
        echo "  PASS: $name"
        PASSED=$((PASSED + 1))
    else
        echo "  FAIL: $name (exit $result)"
        FAILED=$((FAILED + 1))
    fi
}

skip() {
    echo "  SKIP: $1"
    SKIPPED=$((SKIPPED + 1))
}

# -----------------------------------------------------------------------
# Git pull
# -----------------------------------------------------------------------

echo ""
echo "=== Git pull ==="
cd "$REPO"
git fetch origin 2>&1
git log --oneline HEAD..origin/main | head -5
git merge --ff-only origin/main 2>&1 || {
    echo "WARNING: ff-only merge failed, continuing with current HEAD"
}
echo "HEAD: $(git log --oneline -1)"

# -----------------------------------------------------------------------
# Build
# -----------------------------------------------------------------------

echo ""
echo "=== Build ==="
cd "$REPO"
if [ ! -f configure ]; then
    mkdir -p m4 && autoreconf -fi 2>&1 | tail -3
fi

mkdir -p "$BUILD"
cd "$BUILD"
if [ ! -f Makefile ]; then
    "$REPO/configure" --enable-asan --enable-ubsan 2>&1 | tail -5
fi

make -j$(nproc) 2>&1 | tail -10
BUILD_RC=$?
record "build" $BUILD_RC

if [ $BUILD_RC -ne 0 ]; then
    echo "Build failed — aborting"
    FAILED=$((FAILED + 1))
    # Skip to email
    exec 3>&1 4>&2
    goto_email=true
fi

# -----------------------------------------------------------------------
# Unit tests
# -----------------------------------------------------------------------

if [ -z "${goto_email:-}" ]; then
echo ""
echo "=== Unit tests ==="
make check 2>&1 | tee "$LOGDIR/unit_tests.log" | tail -20
UNIT_RC=${PIPESTATUS[0]}
record "unit_tests" $UNIT_RC
fi

# -----------------------------------------------------------------------
# Style + License
# -----------------------------------------------------------------------

if [ -z "${goto_email:-}" ]; then
echo ""
echo "=== Style ==="
cd "$REPO"
make -f Makefile.reffs style 2>&1 | tail -5
record "style" ${PIPESTATUS[0]}

echo ""
echo "=== License ==="
SKIP_STYLE=1 make -f Makefile.reffs license 2>&1 | tail -5
record "license" ${PIPESTATUS[0]}
fi

# -----------------------------------------------------------------------
# Integration test (NFS mount + git clone)
# -----------------------------------------------------------------------

if [ -z "${goto_email:-}" ]; then
echo ""
echo "=== Integration test ==="
REFFSD="$BUILD/src/reffsd"
INT_DATA=/reffs_data/nightly_int_data
INT_STATE=/reffs_data/nightly_int_state
INT_MOUNT=/mnt/reffs_nightly_int
INT_LOG="$LOGDIR/integration.log"

rm -rf "$INT_DATA" "$INT_STATE"
mkdir -p "$INT_DATA" "$INT_STATE"
sudo mkdir -p "$INT_MOUNT"
sudo chmod 777 "$INT_MOUNT"

cat > /tmp/reffs_nightly.toml <<EOF
[server]
port           = $NFS_PORT
bind           = "*"
role           = "standalone"
minor_versions = [1, 2]
grace_period   = 5
workers        = 4

[backend]
type       = "posix"
path       = "$INT_DATA"
state_file = "$INT_STATE"

[[export]]
path        = "/"
clients     = "*"
access      = "rw"
root_squash = false
flavors     = ["sys"]
EOF

if ! rpcinfo -p 127.0.0.1 >/dev/null 2>&1; then
    sudo rpcbind 2>&1 || true
    sleep 1
fi

ASAN_OPTIONS="detect_leaks=0:halt_on_error=1" \
UBSAN_OPTIONS="halt_on_error=1" \
"$REFFSD" --config=/tmp/reffs_nightly.toml > "$INT_LOG" 2>&1 &
INT_PID=$!
sleep 3

if kill -0 $INT_PID 2>/dev/null; then
    sudo timeout 30 mount -o vers=4.2,sec=sys,port=$NFS_PORT \
        127.0.0.1:/ "$INT_MOUNT" 2>&1

    if mountpoint -q "$INT_MOUNT" 2>/dev/null; then
        # Quick smoke test: create, write, read, delete
        echo "test data" > "$INT_MOUNT/nightly_test" 2>&1
        cat "$INT_MOUNT/nightly_test" > /dev/null 2>&1
        rm -f "$INT_MOUNT/nightly_test" 2>&1
        INT_RC=0
    else
        INT_RC=1
        echo "Mount failed"
    fi

    sudo umount -f "$INT_MOUNT" 2>/dev/null
    kill -TERM $INT_PID 2>/dev/null
    wait $INT_PID 2>/dev/null
else
    INT_RC=1
    echo "reffsd failed to start"
fi
record "integration" $INT_RC
fi

# -----------------------------------------------------------------------
# pynfs (NFSv4.1 protocol conformance)
# -----------------------------------------------------------------------

if [ -z "${goto_email:-}" ]; then
echo ""
echo "=== pynfs ==="
"$REPO/scripts/ci_pynfs.sh" 2>&1 | tee "$LOGDIR/pynfs.log" | \
    grep -E '(=== |PASS|FAIL|running|tests passed)' | tail -20
PYNFS_RC=${PIPESTATUS[0]}
record "pynfs" $PYNFS_RC
fi

# -----------------------------------------------------------------------
# CTHON04 (Connectathon NFS tests)
# -----------------------------------------------------------------------

if [ -z "${goto_email:-}" ]; then
echo ""
echo "=== CTHON04 ==="
"$REPO/scripts/ci_cthon04_test.sh" 2>&1 | tee "$LOGDIR/cthon04.log" | \
    grep -E '(=== |PASS|FAIL|All tests|Congratulations)' | tail -20
CTHON04_RC=${PIPESTATUS[0]}
record "cthon04" $CTHON04_RC
fi

# -----------------------------------------------------------------------
# pjdfstest (POSIX filesystem compliance)
# -----------------------------------------------------------------------

if [ -z "${goto_email:-}" ]; then
echo ""
echo "=== pjdfstest ==="
"$REPO/scripts/ci_pjdfstest.sh" 2>&1 | tee "$LOGDIR/pjdfstest.log" | \
    grep -E '(=== |PASS|FAIL|tests|Failed)' | tail -20
PJDFSTEST_RC=${PIPESTATUS[0]}
record "pjdfstest" $PJDFSTEST_RC
fi

# -----------------------------------------------------------------------
# wardtest (EC data integrity over NFSv4.2)
# -----------------------------------------------------------------------

if [ -z "${goto_email:-}" ]; then
echo ""
echo "=== wardtest ==="
WARDTEST_DIR="$HOME/wardtest"
if [ ! -d "$WARDTEST_DIR" ]; then
    echo "Cloning wardtest..."
    git clone git@github.com:loghyr/wardtest.git "$WARDTEST_DIR" 2>&1 | tail -3
fi
(cd "$WARDTEST_DIR" && git pull --ff-only 2>&1 | tail -3)

"$REPO/scripts/ci_wardtest.sh" --duration 60 --wardtest-dir "$WARDTEST_DIR" \
    2>&1 | tee "$LOGDIR/wardtest.log" | \
    grep -E '(=== |PASS|FAIL|iterations|verify)' | tail -20
WARDTEST_RC=${PIPESTATUS[0]}
record "wardtest" $WARDTEST_RC
fi

# -----------------------------------------------------------------------
# Soak test (30 min)
# -----------------------------------------------------------------------

if [ -z "${goto_email:-}" ]; then
echo ""
echo "=== Soak test (POSIX, 30 min) ==="
"$REPO/scripts/local_soak.sh" --posix 2>&1 | tee "$LOGDIR/soak_posix.log" | \
    grep -E '(=== |Health:.*restarts=[0-9]|PASS|FAIL)' | tail -20
SOAK_POSIX_RC=${PIPESTATUS[0]}
record "soak_posix" $SOAK_POSIX_RC

echo ""
echo "=== Soak test (RocksDB, 30 min) ==="
"$REPO/scripts/local_soak.sh" --rocksdb 2>&1 | tee "$LOGDIR/soak_rocksdb.log" | \
    grep -E '(=== |Health:.*restarts=[0-9]|PASS|FAIL)' | tail -20
SOAK_ROCKSDB_RC=${PIPESTATUS[0]}
record "soak_rocksdb" $SOAK_ROCKSDB_RC
fi

# -----------------------------------------------------------------------
# Summary + email
# -----------------------------------------------------------------------

echo ""
echo "========================================"
echo "=== Nightly CI Summary: $HOSTNAME $DATE"
echo "========================================"
echo "  PASSED:  $PASSED"
echo "  FAILED:  $FAILED"
echo "  SKIPPED: $SKIPPED"
echo "  Log dir: $LOGDIR"
echo "========================================"

SUBJECT="reffs CI $HOSTNAME: $PASSED pass, $FAILED fail"
if [ "$FAILED" -gt 0 ]; then
    SUBJECT="[FAIL] $SUBJECT"
else
    SUBJECT="[PASS] $SUBJECT"
fi

# Send email — try msmtp, then mailx, then just log
{
    echo "Subject: $SUBJECT"
    echo "From: reffs-ci@$HOSTNAME"
    echo "To: $EMAIL"
    echo ""
    echo "Nightly CI: $HOSTNAME $(date)"
    echo "Branch: $(git -C "$REPO" branch --show-current)"
    echo "HEAD: $(git -C "$REPO" log --oneline -1)"
    echo ""
    echo "Results:"
    echo "  PASSED:  $PASSED"
    echo "  FAILED:  $FAILED"
    echo "  SKIPPED: $SKIPPED"
    echo ""
    echo "Log dir: $LOGDIR"
    echo ""
    tail -50 "$LOG"
} | msmtp "$EMAIL" 2>/dev/null || \
  mail -s "$SUBJECT" "$EMAIL" < "$LOG" 2>/dev/null || \
  echo "(email not configured — results in $LOG)"

# -----------------------------------------------------------------------
# Cleanup old results (keep 7 days)
# -----------------------------------------------------------------------

find "$RESULTS" -maxdepth 1 -type d -mtime +7 -exec rm -rf {} \; 2>/dev/null

exit $FAILED
