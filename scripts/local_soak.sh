#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# local_soak.sh -- Run soak test directly on the dev box, no Docker.
#
# Usage:
#   scripts/local_soak.sh [OPTIONS]
#
# Options:
#   --posix           POSIX backend (default)
#   --rocksdb         RocksDB metadata backend
#   --duration MIN    Soak duration in minutes (default: 30)
#   --restart MIN     Restart interval in minutes (default: 5)
#   --clients N       Number of workload processes (default: 2)
#
# Quick triage (2 min, 1 restart):
#   scripts/local_soak.sh --duration 2 --restart 1
#
# Builds reffsd in build/, runs it against /reffs as backing store.
# Default: POSIX backend.  --rocksdb uses RocksDB metadata backend.
#
# Prerequisites:
#   - /reffs exists and is writable
#   - rpcbind running (or will be started)
#   - sudo access for mount/umount/rpcbind/mkdir
#   - Build dependencies installed
#
# Run as your normal user, NOT as root.  The script uses targeted
# sudo only for operations that require it (mount, umount, rpcbind,
# mkdir on /mnt).  Running the whole script as root breaks PATH
# (pip-installed tools like xdr-parser aren't in root's PATH).

set -euo pipefail

# -- Defaults (overridable via command line) --
BACKEND_TYPE="posix"
DURATION_MIN=30
RESTART_MIN=5
CLIENTS=2

while [[ $# -gt 0 ]]; do
    case "$1" in
    --rocksdb)    BACKEND_TYPE="rocksdb"; shift ;;
    --posix)      BACKEND_TYPE="posix"; shift ;;
    --duration)   DURATION_MIN="$2"; shift 2 ;;
    --restart)    RESTART_MIN="$2"; shift 2 ;;
    --clients)    CLIENTS="$2"; shift 2 ;;
    *)            echo "Unknown: $1"; exit 1 ;;
    esac
done

DURATION_SEC=$((DURATION_MIN * 60))
RESTART_SEC=$((RESTART_MIN * 60))

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"
REFFSD="$BUILD_DIR/src/reffsd"

DATA_DIR="/reffs_data/soak_data"
STATE_DIR="/reffs_data/soak_state"
MOUNT="/mnt/reffs_local_soak"
CONFIG="/reffs_data/soak.toml"
LOG="/reffs_data/soak.log"
TRACE="/reffs_data/soak-trace.log"

REFFSD_PID=
WORKLOAD_PIDS=()
DSTATE_MON_PID=
DSTATE_LOG="/reffs_data/soak-dstate.log"
FAILED=false

die() { echo "SOAK FAIL: $*" >&2; FAILED=true; }
info() { echo "[$(date +%H:%M:%S)] $*"; }

# ERR trap: log the failing command for post-mortem diagnosis.
# Disabled inside cleanup() which sets +e.
trap 'echo "[$(date +%H:%M:%S)] ERR at line $LINENO: $BASH_COMMAND (exit $?)" >&2' ERR

# -----------------------------------------------------------------------
# Build
# -----------------------------------------------------------------------

info "Building reffsd..."
cd "$PROJECT_ROOT"
if [ ! -f configure ]; then
    mkdir -p m4 && autoreconf -fi
fi
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
if [ ! -f Makefile ]; then
    "$PROJECT_ROOT/configure" --enable-asan --enable-ubsan
fi
make -j$(nproc)
cd "$PROJECT_ROOT"

if [ ! -x "$REFFSD" ]; then
    # libtool wrapper
    REFFSD="$BUILD_DIR/src/.libs/reffsd"
fi

if [ ! -x "$REFFSD" ]; then
    die "Cannot find reffsd binary"
    exit 1
fi

info "Using: $REFFSD"
info "Backend: $BACKEND_TYPE"

# -----------------------------------------------------------------------
# Cleanup from previous runs
# -----------------------------------------------------------------------

# Kill any process still using the mount from a previous run.
# fuser -k sends SIGKILL to killable (non-D-state) processes.
# The subsequent umount -f -l detaches the mount so any surviving
# D-state processes wake with EIO and exit on their own.
if mountpoint -q "$MOUNT" 2>/dev/null; then
    sudo fuser -k -m "$MOUNT" 2>/dev/null || true
    sleep 1
fi
sudo umount -f -l "$MOUNT" 2>/dev/null || true
rm -rf "$DATA_DIR" "$STATE_DIR"
rm -f "$LOG" "$TRACE" /reffs_data/soak-trace-*.log /reffs_data/soak-trace-*.log.zst
mkdir -p "$DATA_DIR" "$STATE_DIR"

# -----------------------------------------------------------------------
# Config
# -----------------------------------------------------------------------

NFS_PORT=12049

cat >"$CONFIG" <<EOF
[server]
port           = $NFS_PORT
bind           = "*"
role           = "standalone"
minor_versions = [1, 2]
grace_period   = 5
workers        = 4
trace_file     = "$TRACE"

[backend]
type       = "$BACKEND_TYPE"
path       = "$DATA_DIR"
state_file = "$STATE_DIR"

[cache]
inode_cache_max  = 16384
dirent_cache_max = 65536

[[export]]
path        = "/"
clients     = "*"
access      = "rw"
root_squash = false
flavors     = ["sys"]
EOF

# -----------------------------------------------------------------------
# Helpers
# -----------------------------------------------------------------------

start_server() {
    info "Starting reffsd..."
    local asan_opts="quarantine_size_mb=256:detect_leaks=0:halt_on_error=1:handle_abort=1"
    if [ "$BACKEND_TYPE" = "rocksdb" ]; then
        asan_opts="${asan_opts}:check_malloc_usable_size=0"
    fi
    ASAN_OPTIONS="$asan_opts" \
    UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
    "$REFFSD" --config="$CONFIG" -c 8 >>"$LOG" 2>&1 &
    REFFSD_PID=$!

    for i in $(seq 1 30); do
        (echo >/dev/tcp/127.0.0.1/$NFS_PORT) 2>/dev/null && break
        kill -0 "$REFFSD_PID" 2>/dev/null || {
            info "reffsd died during startup"
            tail -20 "$LOG"
            die "reffsd startup failure"
            return 1
        }
        sleep 1
    done
    info "reffsd up (PID $REFFSD_PID)"

    # Show FDs at startup
    info "  FDs at start: $(get_fd_count $REFFSD_PID)"
    ls -la /proc/$REFFSD_PID/fd/ 2>/dev/null | head -20 || true
}

stop_server() {
    info "Stopping reffsd (PID $REFFSD_PID)..."

    # Show FDs before stop
    info "  FDs before stop: $(get_fd_count $REFFSD_PID)"

    kill -TERM "$REFFSD_PID" 2>/dev/null || true
    for i in $(seq 1 60); do
        kill -0 "$REFFSD_PID" 2>/dev/null || break
        sleep 1
    done
    if kill -0 "$REFFSD_PID" 2>/dev/null; then
        info "reffsd still running after 60s, sending SIGKILL"
        kill -KILL "$REFFSD_PID" 2>/dev/null || true
    fi
    wait "$REFFSD_PID" 2>/dev/null || true
    REFFSD_PID=
}

mount_nfs() {
    info "  mount: vers=4.2,sec=sys,port=$NFS_PORT -> $MOUNT"
    sudo timeout 30 mount -v -o vers=4.2,sec=sys,port=$NFS_PORT 127.0.0.1:/ "$MOUNT" 2>&1
    local rc=$?
    if [ "$rc" -eq 0 ]; then
        info "  mount: success"
    else
        info "  mount: failed (exit $rc)"
    fi
    return $rc
}

unmount_nfs() {
    info "  umount -f -l $MOUNT"
    sudo umount -f -l "$MOUNT" 2>/dev/null
    local rc=$?
    info "  umount: exit $rc"
    return 0  # best-effort
}

get_rss_kb() {
    awk '/^VmRSS:/ {print $2}' "/proc/$1/status" 2>/dev/null || echo 0
}

get_fd_count() {
    ls "/proc/$1/fd" 2>/dev/null | wc -l || echo 0
}

check_asan() {
    if grep -qE "ERROR: (AddressSanitizer|LeakSanitizer)" "$LOG" 2>/dev/null; then
        info "ASAN/LSAN error detected!"
        grep -A5 "ERROR:" "$LOG"
        die "ASAN error in reffsd log"
    fi
    if grep -q "runtime error:" "$LOG" 2>/dev/null; then
        info "UBSAN error detected!"
        grep "runtime error:" "$LOG"
        die "UBSAN error in reffsd log"
    fi
}

cleanup() {
    set +e
    info "=== Cleanup ==="
    # Stop D-state monitor first
    if [ -n "$DSTATE_MON_PID" ] && kill -0 "$DSTATE_MON_PID" 2>/dev/null; then
        kill -TERM "$DSTATE_MON_PID" 2>/dev/null
        wait "$DSTATE_MON_PID" 2>/dev/null
    fi
    # Order matters: stop server first, then force-unmount to
    # release D-state processes, then kill workloads.
    if [ -n "$REFFSD_PID" ]; then
        info "  [1/3] Stopping reffsd (PID $REFFSD_PID): SIGKILL"
        kill -9 "$REFFSD_PID" 2>/dev/null
        wait "$REFFSD_PID" 2>/dev/null
        info "    reffsd exited"
        REFFSD_PID=
    fi
    info "  [2/3] Force-unmount $MOUNT"
    sudo fuser -k -m "$MOUNT" 2>/dev/null || true
    sleep 1
    sudo umount -f -l "$MOUNT" 2>/dev/null || true
    sleep 1
    info "  [3/3] Killing ${#WORKLOAD_PIDS[@]} workload processes"
    for pid in "${WORKLOAD_PIDS[@]}"; do
        if kill -0 "$pid" 2>/dev/null; then
            info "    kill -9 $pid"
            kill -9 "$pid" 2>/dev/null
        else
            info "    $pid already exited"
        fi
        wait "$pid" 2>/dev/null
    done
    info "  Cleanup complete"
}
trap cleanup EXIT

# -----------------------------------------------------------------------
# Workloads
# -----------------------------------------------------------------------

workload_build() {
    local id=$1
    local mount_dir=$2
    while true; do
        local work_dir="$mount_dir/soak_build_$id"
        rm -rf "$work_dir" 2>/dev/null
        git clone --quiet "$PROJECT_ROOT" "$work_dir" 2>/dev/null || continue
        (
            cd "$work_dir" || exit
            mkdir -p m4
            autoreconf -fi >/dev/null 2>&1 || exit
            mkdir -p build_soak
            cd build_soak || exit
            ../configure --disable-asan --disable-ubsan >/dev/null 2>&1 || exit
            make -j2 >/dev/null 2>&1 || exit
        ) 2>/dev/null
        rm -rf "$work_dir" 2>/dev/null
        sleep 1
    done
}

workload_fileops() {
    local id=$1
    local mount_dir=$2
    local dir="$mount_dir/soak_ops_$id"
    mkdir -p "$dir" 2>/dev/null || true
    local seq=0
    while true; do
        seq=$((seq + 1))
        local f="$dir/file_$((seq % 100))"
        local d="$dir/dir_$((seq % 20))"
        dd if=/dev/urandom of="$f" bs=4096 count=$((RANDOM % 64 + 1)) 2>/dev/null || true
        mkdir -p "$d" 2>/dev/null || true
        local f2="$dir/renamed_$((seq % 50))"
        mv "$f" "$f2" 2>/dev/null || true
        ls "$dir" >/dev/null 2>/dev/null || true
        rm -f "$dir/renamed_$((RANDOM % 50))" 2>/dev/null || true
        rmdir "$d" 2>/dev/null || true
        sleep 0
    done
}

# -----------------------------------------------------------------------
# Main
# -----------------------------------------------------------------------

info "=== Local soak: ${DURATION_MIN}m, restart every ${RESTART_MIN}m, ${CLIENTS} clients, backend=${BACKEND_TYPE} ==="

if ! rpcinfo -p 127.0.0.1 >/dev/null 2>&1; then
    sudo rpcbind || die "rpcbind failed"
    sleep 1
fi

sudo mkdir -p "$MOUNT"
sudo chmod 777 "$MOUNT"

start_server || exit 1
mount_nfs || { tail -20 "$LOG"; die "initial mount failed"; exit 1; }

# Start D-state monitor.  Send an immediate USR1 so that any D-state
# processes left over from a previous soak run (which reference the
# same mount path) are treated as transient and suppressed for 60s.
# If they clear within the grace window, the soak is not penalised.
# If they persist beyond 60s, the monitor will flag them as FAILURE.
"$SCRIPT_DIR/dstate_monitor.sh" "$MOUNT" "$DSTATE_LOG" &
DSTATE_MON_PID=$!
info "D-state monitor running (PID $DSTATE_MON_PID)"
sleep 0.1  # let the monitor install its signal handler before USR1
kill -USR1 "$DSTATE_MON_PID" 2>/dev/null || true
info "D-state startup grace period started (60s)"

# Start workloads.  Redirect stdout/stderr to /dev/null so that
# D-state grandchildren (e.g. rm -rf waiting on a dead NFS mount)
# do not hold the write end of the parent pipeline open and block
# the nightly tee from seeing EOF after the soak exits.
for i in $(seq 1 "$CLIENTS"); do
    if [ $((i % 2)) -eq 0 ]; then
        workload_fileops "$i" "$MOUNT" >/dev/null 2>&1 &
    else
        workload_build "$i" "$MOUNT" >/dev/null 2>&1 &
    fi
    WORKLOAD_PIDS+=($!)
done
info "Started $CLIENTS workload processes"

# Warmup
info "Warming up (60s)..."
sleep 60
BASELINE_RSS=$(get_rss_kb "$REFFSD_PID")
BASELINE_FD=$(get_fd_count "$REFFSD_PID")
info "Baseline (under load): RSS=${BASELINE_RSS}KB FD=${BASELINE_FD}"
info "Baseline FD list:"
ls -la /proc/$REFFSD_PID/fd/ 2>/dev/null | head -40 || true

SOAK_START=$(date +%s)
RESTART_COUNT=0
NEXT_RESTART=$((SOAK_START + RESTART_SEC))

info "Main loop starting (duration=${DURATION_MIN}m, restart=${RESTART_MIN}m)"

while true; do
    NOW=$(date +%s)
    ELAPSED=$((NOW - SOAK_START))

    if [ "$ELAPSED" -ge "$DURATION_SEC" ]; then
        info "Duration reached (${DURATION_MIN}m). Finishing."
        break
    fi

    # Don't start a restart if we're within 2 minutes of the end --
    # the restart sequence (kill + unmount + remount) takes time and
    # would run past the duration boundary.
    REMAINING=$((DURATION_SEC - ELAPSED))
    if [ "$NOW" -ge "$NEXT_RESTART" ] && [ "$REMAINING" -gt 120 ]; then
        RESTART_COUNT=$((RESTART_COUNT + 1))
        info "=== Restart #$RESTART_COUNT ==="

        # Signal D-state monitor: begin grace period
        if [ -n "$DSTATE_MON_PID" ] && kill -0 "$DSTATE_MON_PID" 2>/dev/null; then
            kill -USR1 "$DSTATE_MON_PID" 2>/dev/null || true
        fi

        # Step 1: SIGKILL server (crash-restart, not graceful)
        info "  [1/6] SIGKILL reffsd (PID $REFFSD_PID)"
        info "    FDs before kill: $(get_fd_count $REFFSD_PID)"
        kill -9 "$REFFSD_PID" 2>/dev/null || true
        wait "$REFFSD_PID" 2>/dev/null || true
        info "    reffsd exited"
        REFFSD_PID=

        # Step 2: force-unmount to unblock D-state workloads
        info "  [2/6] Force-unmount $MOUNT"
        sudo umount -f -l "$MOUNT" 2>/dev/null || true
        sleep 1

        # Step 3: SIGKILL workloads (SIGTERM can't interrupt D-state)
        info "  [3/6] SIGKILL ${#WORKLOAD_PIDS[@]} workload processes"
        for pid in "${WORKLOAD_PIDS[@]}"; do
            if kill -0 "$pid" 2>/dev/null; then
                info "    kill -9 $pid"
                kill -9 "$pid" 2>/dev/null || true
            else
                info "    $pid already exited"
            fi
        done
        # Wait briefly for workloads to exit.  D-state processes on a
        # dead NFS mount may never exit; don't block the restart.
        for pid in "${WORKLOAD_PIDS[@]}"; do
            for w in $(seq 1 5); do
                kill -0 "$pid" 2>/dev/null || break
                sleep 1
            done
        done
        info "    workloads reaped (some may linger in D-state)"
        WORKLOAD_PIDS=()

        # Step 4: check for ASAN/UBSAN errors in server log
        info "  [4/6] Checking ASAN/UBSAN"
        check_asan
        if [ "$FAILED" = "true" ]; then
            info "Aborting soak due to ASAN/UBSAN errors"
            exit 1
        fi
        info "    clean"

        # Step 5: restart server + remount
        info "  [5/6] Restarting server"
        echo "=== Restart #$RESTART_COUNT at $(date) ===" >> "$LOG"
        start_server || exit 1

        sudo umount -f "$MOUNT" 2>/dev/null || true
        sleep 1
        mount_ok=false
        for try in $(seq 1 6); do
            # mount_nfs returns non-zero on timeout/failure;
            # must suppress set -e to allow retry loop to continue
            if mount_nfs; then
                mount_ok=true
                break
            fi
            info "    mount attempt $try/6 failed, retrying in 5s"
            sleep 5
        done
        if [ "$mount_ok" != "true" ]; then
            die "mount failed after restart #$RESTART_COUNT (6 attempts)"
            exit 1
        fi

        # Step 6: relaunch workloads
        info "  [6/6] Relaunching $CLIENTS workloads"
        for i in $(seq 1 "$CLIENTS"); do
            if [ $((i % 2)) -eq 0 ]; then
                workload_fileops "$i" "$MOUNT" >/dev/null 2>&1 &
            else
                workload_build "$i" "$MOUNT" >/dev/null 2>&1 &
            fi
            WORKLOAD_PIDS+=($!)
            info "    workload $i: PID ${WORKLOAD_PIDS[-1]}"
        done

        # Reset the D-state grace timer now that the server is back and
        # workloads have restarted.  The first USR1 (sent before the kill)
        # is consumed mostly by the restart sequence itself; D-state
        # processes from the old mount need a full grace window AFTER
        # server recovery to drain and exit.
        if [ -n "$DSTATE_MON_PID" ] && kill -0 "$DSTATE_MON_PID" 2>/dev/null; then
            kill -USR1 "$DSTATE_MON_PID" 2>/dev/null || true
            info "  D-state grace timer reset (server recovered)"
        fi

        NEXT_RESTART=$((NOW + RESTART_SEC))
        info "  Restart #$RESTART_COUNT complete, next in ${RESTART_SEC}s"
    fi

    sleep 10

    if kill -0 "$REFFSD_PID" 2>/dev/null; then
        # get_rss_kb and get_fd_count tolerate proc disappearance
        # (process could exit between kill -0 and the /proc read)
        RSS=$(get_rss_kb "$REFFSD_PID")
        FD=$(get_fd_count "$REFFSD_PID")
        if [ "$RSS" -eq 0 ] && ! kill -0 "$REFFSD_PID" 2>/dev/null; then
            die "reffsd crashed during soak (detected at health check)"
            tail -20 "$LOG"
            exit 1
        fi
        info "Health: RSS=${RSS}KB FD=${FD} elapsed=${ELAPSED}s restarts=${RESTART_COUNT}"
    else
        die "reffsd crashed during soak"
        tail -20 "$LOG"
        exit 1
    fi
done

# -----------------------------------------------------------------------
# Final checks
# -----------------------------------------------------------------------

info "Stopping workloads..."
for pid in "${WORKLOAD_PIDS[@]}"; do
    kill "$pid" 2>/dev/null || true
done
for pid in "${WORKLOAD_PIDS[@]}"; do
    wait "$pid" 2>/dev/null || true
done
WORKLOAD_PIDS=()

unmount_nfs

FINAL_RSS=$(get_rss_kb "$REFFSD_PID")
FINAL_FD=$(get_fd_count "$REFFSD_PID")

info "Final FD list:"
ls -la /proc/$REFFSD_PID/fd/ 2>/dev/null | head -40 || true

stop_server
check_asan

info "=== Final metrics ==="
info "  Baseline: RSS=${BASELINE_RSS}KB FD=${BASELINE_FD}"
info "  Final:    RSS=${FINAL_RSS}KB FD=${FINAL_FD}"
info "  Restarts: $RESTART_COUNT"

if [ "$BASELINE_RSS" -gt 0 ] && [ "$FINAL_RSS" -gt $((BASELINE_RSS * 2)) ]; then
    die "Memory growth: ${FINAL_RSS}KB > 2x baseline ${BASELINE_RSS}KB"
fi

if [ "$BASELINE_FD" -gt 0 ]; then
    # RocksDB SST files grow with data volume and across restarts.
    FD_LIMIT=$(( BASELINE_FD + BASELINE_FD / 2 + 5 ))
    if [ "$FINAL_FD" -gt "$FD_LIMIT" ]; then
        die "FD growth: ${FINAL_FD} > ${FD_LIMIT} (baseline ${BASELINE_FD} + 50%)"
    fi
fi

# Check D-state monitor results
if [ -n "$DSTATE_MON_PID" ] && kill -0 "$DSTATE_MON_PID" 2>/dev/null; then
    kill -TERM "$DSTATE_MON_PID" 2>/dev/null || true
    wait "$DSTATE_MON_PID" 2>/dev/null || true
    DSTATE_MON_PID=
fi

if [ -f "$DSTATE_LOG" ] && grep -q "^.*FAILURE:" "$DSTATE_LOG" 2>/dev/null; then
    info "D-state failures detected:"
    grep "FAILURE:" "$DSTATE_LOG"
    die "D-state processes persisted after server recovery"
fi

if [ "$FAILED" = "true" ]; then
    info "=== LOCAL SOAK FAILED ==="
    exit 1
fi

info "=== LOCAL SOAK PASSED (${DURATION_MIN}m, ${RESTART_COUNT} restarts, ${BACKEND_TYPE}) ==="
