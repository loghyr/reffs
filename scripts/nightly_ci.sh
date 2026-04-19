#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# nightly_ci.sh -- Full CI run on bare metal, no Docker.
#
# Architecture:
#   1. Build + unit tests + style + license (no server needed)
#   2. Start reffsd once, mount NFSv3 + NFSv4.2 (persistent)
#   3. Run all external test suites against those mounts
#   4. Unmount, stop server
#   5. Soak tests (own server lifecycle -- crash recovery)
#   6. Email summary
#
# Crontab entry (e.g., 2am nightly):
#   0 2 * * * /home/loghyr/reffs/scripts/nightly_ci.sh 2>&1
#
# Quick soak-only triage (skip build/tests, 5 min soaks):
#   scripts/nightly_ci.sh --soak-only --soak-duration 5 2>&1 | tee /tmp/soak.log
#
# Options:
#   --soak-only          Skip build, unit tests, and external test suites
#   --soak-duration MIN  Soak duration in minutes (default: 30)
#
# Prerequisites:
#   - /reffs_data writable
#   - sudo NOPASSWD for mount/umount/rpcbind/mkdir/chmod/mountpoint
#   - msmtp or mailx configured for email
#   - Build dependencies installed

set -uo pipefail

# Cron runs with a minimal PATH that omits ~/.local/bin where pip
# installs user-level tools (xdr-parser from reply-xdr).
export PATH="$HOME/.local/bin:$PATH"

# On Rocky/RHEL, HdrHistogram_c is built from source into /usr/local.
# Extend PKG_CONFIG_PATH so configure finds it.
export PKG_CONFIG_PATH="/usr/local/lib64/pkgconfig:/usr/local/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

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

# -- CLI flags --
SOAK_ONLY=false
SOAK_DURATION=30        # minutes per soak (default: 30)

while [[ $# -gt 0 ]]; do
    case "$1" in
    --soak-only)       SOAK_ONLY=true; shift ;;
    --soak-duration)   SOAK_DURATION="$2"; shift 2 ;;
    *)                 echo "Unknown: $1"; exit 1 ;;
    esac
done

V4_MOUNT=/mnt/reffs_v4
V3_MOUNT=/mnt/reffs_v3

# -----------------------------------------------------------------------
# Lock / stuck detection
# -----------------------------------------------------------------------

cleanup_lock() {
    rm -f "$LOCKFILE"
}

if [ -f "$LOCKFILE" ]; then
    PID=$(cat "$LOCKFILE" 2>/dev/null)
    if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
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
        rm -f "$LOCKFILE"
    fi
fi

echo $$ > "$LOCKFILE"
trap cleanup_lock EXIT

# Evict any reffsd orphaned by a previous soak or aborted nightly run.
# Verify the holder is reffsd before killing -- a blind port-based kill
# has killed unrelated services (e.g. NetworkManager) in the past.
if sudo fuser "$NFS_PORT/tcp" >/dev/null 2>&1; then
    _killed=false
    for _pid in $(sudo fuser "$NFS_PORT/tcp" 2>/dev/null); do
        [[ "$_pid" =~ ^[0-9]+$ ]] || continue
        _comm=$(cat "/proc/$_pid/comm" 2>/dev/null || echo "unknown")
        if [[ "$_comm" == reffsd* ]]; then
            echo "Evicting stale reffsd on port $NFS_PORT (PID $_pid)"
            sudo kill -KILL "$_pid" 2>/dev/null || true
            _killed=true
        else
            echo "WARNING: port $NFS_PORT held by '$_comm' (PID $_pid) -- not reffsd, skipping kill"
        fi
    done
    $_killed && sleep 1 || true
fi

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
CI_START=$(date +%s)

section_start() {
    eval "_section_${1}_start=$(date +%s)"
    echo ""
    echo "=== $2 [$(date +%H:%M:%S)] ==="
}

record() {
    local name=$1 result=$2
    local elapsed=""
    local start_var="_section_${name}_start"
    if [ -n "${!start_var:-}" ]; then
        elapsed=" ($(( $(date +%s) - ${!start_var} ))s)"
    fi
    if [ "$result" -eq 0 ]; then
        echo "  PASS: $name${elapsed}"
        PASSED=$((PASSED + 1))
    else
        echo "  FAIL: $name (exit $result)${elapsed}"
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

section_start git_pull "Git pull"
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

if [ "$SOAK_ONLY" = true ]; then
    echo ""
    echo "=== --soak-only: skipping build, tests, and external suites ==="
    echo ""
    # Jump past all pre-soak sections.  goto_email is unset so soak
    # sections will run.  The soak script does its own build.
else

section_start build "Build"
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
    echo "Build failed -- aborting"
    FAILED=$((FAILED + 1))
    goto_email=true
fi

REFFSD="$BUILD/src/reffsd"

# -----------------------------------------------------------------------
# Unit tests
# -----------------------------------------------------------------------

if [ -z "${goto_email:-}" ]; then
section_start unit_tests "Unit tests"
make check 2>&1 | tee "$LOGDIR/unit_tests.log" | tail -20
UNIT_RC=${PIPESTATUS[0]}
record "unit_tests" $UNIT_RC
fi

# -----------------------------------------------------------------------
# Style + License
# -----------------------------------------------------------------------

if [ -z "${goto_email:-}" ]; then
section_start style "Style"
cd "$REPO"
make -f Makefile.reffs style 2>&1 | tail -5
record "style" ${PIPESTATUS[0]}

section_start license "License"
SKIP_STYLE=1 make -f Makefile.reffs license 2>&1 | tail -5
record "license" ${PIPESTATUS[0]}
fi

# =======================================================================
# Start server + persistent mounts (NFSv3 + NFSv4.2)
# =======================================================================

REFFSD_PID=
INT_DATA=/reffs_data/nightly_data
INT_STATE=/reffs_data/nightly_state
INT_CONFIG=/tmp/reffs_nightly.toml
INT_LOG="$LOGDIR/reffsd.log"

stop_nfs_server() {
    echo "  Stopping reffsd (PID ${REFFSD_PID:-none})"
    if [ -n "${REFFSD_PID:-}" ]; then
        kill -TERM "$REFFSD_PID" 2>/dev/null
        for i in $(seq 1 10); do
            kill -0 "$REFFSD_PID" 2>/dev/null || break
            sleep 1
        done
        if kill -0 "$REFFSD_PID" 2>/dev/null; then
            echo "  reffsd still alive after 10s, SIGKILL"
            kill -KILL "$REFFSD_PID" 2>/dev/null
        fi
        wait "$REFFSD_PID" 2>/dev/null || true
        echo "  reffsd exited"
        REFFSD_PID=
    fi
}

unmount_all() {
    echo "  Unmounting $V4_MOUNT"
    sudo umount -f "$V4_MOUNT" 2>/dev/null || true
    echo "  Unmounting $V3_MOUNT"
    sudo umount -f "$V3_MOUNT" 2>/dev/null || true
}

nfs_cleanup() {
    echo "=== NFS cleanup ==="
    unmount_all
    stop_nfs_server
    # Check for ASAN/UBSAN in server log
    if [ -f "$INT_LOG" ]; then
        if grep -qE "ERROR: (AddressSanitizer|LeakSanitizer)" "$INT_LOG" 2>/dev/null; then
            echo "  ASAN/LSAN error in server log!"
            grep -A5 "ERROR:" "$INT_LOG" | head -20
        fi
        if grep -q "runtime error:" "$INT_LOG" 2>/dev/null; then
            echo "  UBSAN error in server log!"
            grep "runtime error:" "$INT_LOG" | head -10
        fi
    fi
}

if [ -z "${goto_email:-}" ]; then
section_start nfs_setup "Start NFS server + mounts"

rm -rf "$INT_DATA" "$INT_STATE"
mkdir -p "$INT_DATA" "$INT_STATE"
sudo mkdir -p "$V4_MOUNT" "$V3_MOUNT"

cat > "$INT_CONFIG" <<EOF
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

: > "$INT_LOG"
ASAN_OPTIONS="detect_leaks=0:halt_on_error=1" \
UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
"$REFFSD" --config="$INT_CONFIG" >"$INT_LOG" 2>&1 &
REFFSD_PID=$!

# Wait for server
SERVER_OK=false
for i in $(seq 1 30); do
    (echo >/dev/tcp/127.0.0.1/$NFS_PORT) 2>/dev/null && { SERVER_OK=true; break; }
    kill -0 "$REFFSD_PID" 2>/dev/null || { echo "reffsd died during startup"; break; }
    sleep 1
done

if [ "$SERVER_OK" = true ]; then
    echo "  reffsd up (PID $REFFSD_PID, port $NFS_PORT)"

    echo "  Mounting NFSv4.2 at $V4_MOUNT"
    sudo mount -o vers=4.2,sec=sys,hard,timeo=600,port=$NFS_PORT \
        127.0.0.1:/ "$V4_MOUNT" 2>&1

    echo "  Mounting NFSv3 at $V3_MOUNT"
    sudo mount -o vers=3,sec=sys,hard,nolock,tcp,mountproto=tcp,timeo=600,port=$NFS_PORT \
        127.0.0.1:/ "$V3_MOUNT" 2>&1

    V4_OK=false; V3_OK=false
    sudo mountpoint -q "$V4_MOUNT" 2>/dev/null && V4_OK=true
    sudo mountpoint -q "$V3_MOUNT" 2>/dev/null && V3_OK=true
    echo "  NFSv4.2: $V4_OK  NFSv3: $V3_OK"

    if [ "$V4_OK" = true ] && [ "$V3_OK" = true ]; then
        # Quick smoke test
        echo "smoke" > "$V4_MOUNT/nightly_smoke" 2>/dev/null
        cat "$V4_MOUNT/nightly_smoke" > /dev/null 2>/dev/null
        rm -f "$V4_MOUNT/nightly_smoke" 2>/dev/null
        echo "smoke" > "$V3_MOUNT/nightly_smoke" 2>/dev/null
        cat "$V3_MOUNT/nightly_smoke" > /dev/null 2>/dev/null
        rm -f "$V3_MOUNT/nightly_smoke" 2>/dev/null
        record "nfs_setup" 0
    else
        record "nfs_setup" 1
        nfs_cleanup
        goto_email=true
    fi
else
    echo "  reffsd failed to start"
    tail -20 "$INT_LOG"
    record "nfs_setup" 1
    goto_email=true
fi
fi

# -----------------------------------------------------------------------
# pynfs (userspace RPC client -- uses server address, not mount)
# -----------------------------------------------------------------------

if [ -z "${goto_email:-}" ]; then
section_start pynfs "pynfs"
"$REPO/scripts/ci_pynfs.sh" --server 127.0.0.1 --port "$NFS_PORT" \
    2>&1 | tee "$LOGDIR/pynfs.log" | \
    grep -E '(=== |PASS|FAIL|running|tests passed)' | tail -20
PYNFS_RC=${PIPESTATUS[0]}
record "pynfs" $PYNFS_RC
fi

# -----------------------------------------------------------------------
# cthon04 (uses persistent mounts)
# -----------------------------------------------------------------------

if [ -z "${goto_email:-}" ]; then
section_start cthon04 "cthon04"
"$REPO/scripts/ci_cthon04_test.sh" --v3-mount "$V3_MOUNT" --v4-mount "$V4_MOUNT" \
    2>&1 | tee "$LOGDIR/cthon04.log" | \
    grep -E '(=== |PASS|FAIL|basic|general)' | tail -20
CTHON04_RC=${PIPESTATUS[0]}
record "cthon04" $CTHON04_RC
fi

# -----------------------------------------------------------------------
# nfs-conformance (uses persistent mounts)
# -----------------------------------------------------------------------

if [ -z "${goto_email:-}" ]; then
section_start nfs_conformance "nfs-conformance"
"$REPO/scripts/ci_nfs_conformance_test.sh" --v3-mount "$V3_MOUNT" --v4-mount "$V4_MOUNT" \
    2>&1 | tee "$LOGDIR/nfs_conformance.log" | \
    grep -E '(=== |PASS|FAIL|Files=)' | tail -20
NFS_CONFORMANCE_RC=${PIPESTATUS[0]}
record "nfs_conformance" $NFS_CONFORMANCE_RC
fi

# -----------------------------------------------------------------------
# pjdfstest (uses persistent mounts)
# -----------------------------------------------------------------------

if [ -z "${goto_email:-}" ]; then
section_start pjdfstest "pjdfstest"
"$REPO/scripts/ci_pjdfstest.sh" --v3-mount "$V3_MOUNT" --v4-mount "$V4_MOUNT" \
    2>&1 | tee "$LOGDIR/pjdfstest.log" | \
    grep -E '(=== |PASS|FAIL|tests|Failed)' | tail -20
PJDFSTEST_RC=${PIPESTATUS[0]}
record "pjdfstest" $PJDFSTEST_RC
fi

# -----------------------------------------------------------------------
# wardtest (uses NFSv4.2 persistent mount)
# -----------------------------------------------------------------------

if [ -z "${goto_email:-}" ]; then
section_start wardtest "wardtest"
WARDTEST_DIR="$HOME/wardtest"
if [ ! -d "$WARDTEST_DIR" ]; then
    echo "Cloning wardtest..."
    git clone git@github.com:loghyr/wardtest.git "$WARDTEST_DIR" 2>&1 | tail -3
fi
(cd "$WARDTEST_DIR" && git pull --ff-only 2>&1 | tail -3)

"$REPO/scripts/ci_wardtest.sh" --mount "$V4_MOUNT" --duration 60 \
    --wardtest-dir "$WARDTEST_DIR" \
    2>&1 | tee "$LOGDIR/wardtest.log" | \
    grep -E '(=== |PASS|FAIL|iterations|verify)' | tail -20
WARDTEST_RC=${PIPESTATUS[0]}
record "wardtest" $WARDTEST_RC
fi

# -----------------------------------------------------------------------
# Tear down persistent mounts + server
# -----------------------------------------------------------------------

if [ -z "${goto_email:-}" ]; then
section_start nfs_teardown "NFS teardown"
nfs_cleanup
record "nfs_teardown" 0
fi

fi  # end --soak-only skip block (opened before Build section)

# -----------------------------------------------------------------------
# Soak tests (own server lifecycle -- crash recovery testing)
# -----------------------------------------------------------------------

if [ -z "${goto_email:-}" ]; then
# Timeout = soak duration + 15 min margin for restarts, mount
# retries, and build time.  Prevents hung D-state processes from
# blocking the nightly email indefinitely.
# (+15 instead of +10: garbo hit the 40-min timeout by 1s)
SOAK_TIMEOUT=$(( (SOAK_DURATION + 15) * 60 ))

section_start soak_posix "Soak test (POSIX, ${SOAK_DURATION} min)"
timeout $SOAK_TIMEOUT "$REPO/scripts/local_soak.sh" --posix \
    --duration "$SOAK_DURATION" \
    --dstate-log "$LOGDIR/soak_posix_dstate.log" 2>&1 | \
    tee "$LOGDIR/soak_posix.log" | \
    grep -E '(=== |Health:.*restarts=[0-9]|PASS|FAIL)' | tail -20
SOAK_POSIX_RC=${PIPESTATUS[0]}
record "soak_posix" $SOAK_POSIX_RC

section_start soak_rocksdb "Soak test (RocksDB, ${SOAK_DURATION} min)"
timeout $SOAK_TIMEOUT "$REPO/scripts/local_soak.sh" --rocksdb \
    --duration "$SOAK_DURATION" \
    --dstate-log "$LOGDIR/soak_rocksdb_dstate.log" 2>&1 | \
    tee "$LOGDIR/soak_rocksdb.log" | \
    grep -E '(=== |Health:.*restarts=[0-9]|PASS|FAIL)' | tail -20
SOAK_ROCKSDB_RC=${PIPESTATUS[0]}
record "soak_rocksdb" $SOAK_ROCKSDB_RC
fi

# -----------------------------------------------------------------------
# Summary + email
# -----------------------------------------------------------------------

CI_END=$(date +%s)
CI_ELAPSED=$(( CI_END - CI_START ))
CI_MIN=$(( CI_ELAPSED / 60 ))
CI_SEC=$(( CI_ELAPSED % 60 ))

echo ""
echo "========================================"
echo "=== Nightly CI Summary: $HOSTNAME"
echo "=== Started: $DATE  Finished: $(date +%Y%m%d-%H%M%S)"
echo "=== Duration: ${CI_MIN}m ${CI_SEC}s"
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
} | msmtp "$EMAIL" 2>&1 || {
    MSMTP_RC=$?
    echo "msmtp failed (exit $MSMTP_RC), trying mail..."
    mail -s "$SUBJECT" "$EMAIL" < "$LOG" 2>&1 || \
      echo "(email failed -- results in $LOG)"
}

# -----------------------------------------------------------------------
# Cleanup old results (keep 7 days)
# -----------------------------------------------------------------------

find "$RESULTS" -maxdepth 1 -type d -mtime +7 -exec rm -rf {} \; 2>/dev/null

exit $FAILED
