#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# ci_wardtest.sh -- Run wardtest against reffsd over NFSv4.2.
#
# Builds reffsd + wardtest, starts the server, mounts, runs 4 writer
# clients with 2 threads each, then runs a verify-only phase to
# confirm all data is intact.
#
# Usage:
#   scripts/ci_wardtest.sh [OPTIONS]
#
# Options:
#   --duration N      Total run time in seconds (default: 60)
#   --iterations N    Iterations per client (default: 0, use duration)
#   --clients N       Threads per client process (default: 2)
#   --codec TYPE      xor or rs (default: xor)
#   --k N             Data shards (default: 4)
#   --m N             Parity shards (default: 1)
#   --crash           Enable crash testing (SIGKILL every 30s)
#   --crash-interval N  Seconds between kills (default: 30)
#   --wardtest-dir P  Path to wardtest source (default: ~/wardtest)
#
# Run as your normal user, NOT root.  Uses targeted sudo for
# mount/umount only.

set -euo pipefail

# -- Defaults --
DURATION=60
ITERATIONS=0
NCLIENTS=2
CODEC="xor"
K=4
M=1
CRASH=0
CRASH_INTERVAL=30
WARDTEST_DIR="${HOME}/wardtest"
PORT=12049
NUM_WRITERS=4

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"
REFFSD="$BUILD_DIR/src/reffsd"

# -- Parse args --
while [[ $# -gt 0 ]]; do
    case "$1" in
        --duration)       DURATION="$2"; shift 2 ;;
        --iterations)     ITERATIONS="$2"; shift 2 ;;
        --clients)        NCLIENTS="$2"; shift 2 ;;
        --codec)          CODEC="$2"; shift 2 ;;
        --k)              K="$2"; shift 2 ;;
        --m)              M="$2"; shift 2 ;;
        --crash)          CRASH=1; shift ;;
        --crash-interval) CRASH_INTERVAL="$2"; shift 2 ;;
        --wardtest-dir)   WARDTEST_DIR="$2"; shift 2 ;;
        *) echo "Unknown: $1"; exit 1 ;;
    esac
done

# -- Validate --
if [[ ! -x "${REFFSD}" ]]; then
    echo "Error: reffsd not found at ${REFFSD}"
    echo "Build first: cd ${BUILD_DIR} && make -j\$(nproc)"
    exit 1
fi

if [[ ! -f "${WARDTEST_DIR}/src/wardtest.c" ]]; then
    echo "Error: wardtest source not found at ${WARDTEST_DIR}"
    echo "Use --wardtest-dir to specify the path"
    exit 1
fi

# -- Build wardtest --
WARDTEST_BIN="${WARDTEST_DIR}/build/src/wardtest"
if [[ ! -x "${WARDTEST_BIN}" ]]; then
    echo "--- Building wardtest ---"
    (
        cd "${WARDTEST_DIR}"
        mkdir -p m4 && autoreconf -fi >/dev/null 2>&1
        mkdir -p build && cd build
        ../configure --enable-optimize --silent
        make -j"$(nproc)" --silent
    )
fi

info() { echo "[$(date +%H:%M:%S)] $*"; }

# -- Setup --
WORKDIR=$(mktemp -d /tmp/wt-ci-XXXXXX)
MOUNT_DIR=$(mktemp -d /tmp/wt-mnt-XXXXXX)
REFFSD_PID=""
CLIENT_PIDS=()

cat > "${WORKDIR}/config.toml" << EOF
[server]
port           = ${PORT}
bind           = "*"
role           = "standalone"
minor_versions = [1, 2]
grace_period   = 5
workers        = 4

[backend]
type       = "ram"
path       = "${WORKDIR}/data"
state_file = "${WORKDIR}/state"

[[export]]
path        = "/"
clients     = "*"
access      = "rw"
root_squash = false
flavors     = ["sys"]
EOF

mkdir -p "${WORKDIR}"/{data,state}

cleanup() {
    info "Cleanup"
    for pid in "${CLIENT_PIDS[@]}"; do
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    done
    if [[ -n "${REFFSD_PID}" ]]; then
        kill "${REFFSD_PID}" 2>/dev/null || true
        wait "${REFFSD_PID}" 2>/dev/null || true
    fi
    sudo umount "${MOUNT_DIR}" 2>/dev/null || true
    rmdir "${MOUNT_DIR}" 2>/dev/null || true
    rm -rf "${WORKDIR}"
}
trap cleanup EXIT

start_server() {
    "${REFFSD}" --config "${WORKDIR}/config.toml" &
    REFFSD_PID=$!
    for i in $(seq 1 30); do
        if ss -tln | grep -q ":${PORT} " 2>/dev/null; then
            return 0
        fi
        sleep 0.5
    done
    info "Error: reffsd didn't start"
    return 1
}

# -- Print config --
echo "=== wardtest CI test ==="
echo "  codec:    ${CODEC} k=${K} m=${M}"
echo "  clients:  ${NUM_WRITERS} x ${NCLIENTS} threads"
if [[ ${ITERATIONS} -gt 0 ]]; then
    echo "  mode:     ${ITERATIONS} iterations per client"
else
    echo "  mode:     ${DURATION}s duration"
fi
if [[ ${CRASH} -eq 1 ]]; then
    echo "  crash:    every ${CRASH_INTERVAL}s"
fi
echo ""

# -- Start server --
info "Starting reffsd on port ${PORT}"
start_server
info "reffsd running (PID ${REFFSD_PID})"

# -- Mount --
info "Mounting NFSv4.2 at ${MOUNT_DIR}"
sudo mount -o vers=4.2,sec=sys,hard,timeo=600,port="${PORT}" \
    127.0.0.1:/ "${MOUNT_DIR}"
sudo mkdir -p "${MOUNT_DIR}/wardtest"/{data,meta,history}
sudo chmod 777 "${MOUNT_DIR}/wardtest"/{data,meta,history}

# -- Build wardtest args --
COMMON_ARGS="--data ${MOUNT_DIR}/wardtest/data \
    --meta ${MOUNT_DIR}/wardtest/meta \
    --history ${MOUNT_DIR}/wardtest/history \
    --clients ${NCLIENTS} \
    --codec ${CODEC} --k ${K} --m ${M} \
    --report 10"

if [[ ${ITERATIONS} -gt 0 ]]; then
    COMMON_ARGS="${COMMON_ARGS} --iterations ${ITERATIONS}"
else
    COMMON_ARGS="${COMMON_ARGS} --duration ${DURATION}"
fi

# -- Start writers --
info "Starting ${NUM_WRITERS} writer clients"
for i in $(seq 1 "${NUM_WRITERS}"); do
    SEED=$((0x10000000 * i))
    "${WARDTEST_BIN}" ${COMMON_ARGS} --seed "${SEED}" &
    CLIENT_PIDS+=($!)
done

# -- Crash loop (optional) --
if [[ ${CRASH} -eq 1 ]]; then
    KILLS=0
    START=$(date +%s)
    while true; do
        sleep "${CRASH_INTERVAL}"

        ELAPSED=$(( $(date +%s) - START ))
        if [[ ${ITERATIONS} -eq 0 && ${ELAPSED} -ge $((DURATION - CRASH_INTERVAL)) ]]; then
            info "Final stretch -- letting clients finish"
            break
        fi

        # Check if clients are still alive
        ALIVE=0
        for pid in "${CLIENT_PIDS[@]}"; do
            kill -0 "$pid" 2>/dev/null && ALIVE=$((ALIVE + 1))
        done
        if [[ ${ALIVE} -eq 0 ]]; then
            break
        fi

        KILLS=$((KILLS + 1))
        info "SIGKILL #${KILLS} (${ALIVE} clients alive)"
        kill -9 "${REFFSD_PID}" 2>/dev/null || true
        wait "${REFFSD_PID}" 2>/dev/null || true
        sleep 2
        start_server
        info "Server restarted (PID ${REFFSD_PID})"
    done
fi

# -- Wait for writers --
info "Waiting for writers to finish"
WRITER_FAIL=0
for i in "${!CLIENT_PIDS[@]}"; do
    if ! wait "${CLIENT_PIDS[$i]}"; then
        WRITER_FAIL=1
    fi
done
CLIENT_PIDS=()

# -- Verify phase --
info "Verify phase: reading all surviving stripes"
"${WARDTEST_BIN}" \
    --data "${MOUNT_DIR}/wardtest/data" \
    --meta "${MOUNT_DIR}/wardtest/meta" \
    --history "${MOUNT_DIR}/wardtest/history" \
    --duration 30 --clients 4 --verify-only --report 10
VERIFY_EXIT=$?

# -- Results --
echo ""
if [[ -f "${MOUNT_DIR}/wardtest/meta/.wardtest_stop" ]]; then
    echo "=== CORRUPTION DETECTED ==="
    cat "${MOUNT_DIR}/wardtest/meta/.wardtest_stop"
    exit 2
fi

if [[ ${VERIFY_EXIT} -ne 0 ]]; then
    echo "=== FAIL: verify phase failed ==="
    exit 2
fi

DATA_COUNT=$(ls "${MOUNT_DIR}/wardtest/data/" 2>/dev/null | wc -l)
META_COUNT=$(ls "${MOUNT_DIR}/wardtest/meta/" 2>/dev/null | grep -c '\.meta$' || echo 0)
info "Data files: ${DATA_COUNT}, Meta files: ${META_COUNT}"

if [[ ${CRASH} -eq 1 ]]; then
    info "Server killed ${KILLS} times"
fi

echo ""
echo "=== PASS: zero corruption ==="
