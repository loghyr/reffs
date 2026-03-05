#!/bin/bash
# do_cthon.sh - Run cthon04 basic, general, and special tests in a loop
#
# Usage: ./do_cthon.sh [--server <server:export>] [--mount <mountpoint>]
#                      [--iters <n>] [--cthon <cthon04-dir>] [--logdir <dir>]
#
# Defaults:
#   --server  127.0.0.1:/
#   --mount   /mnt/cthon04
#   --iters   1
#   --cthon   /home/loghyr/reffs/cthon04
#   --logdir  /home/loghyr/reffs/cthon_logs
#
# Example:
#   ./do_cthon.sh
#   ./do_cthon.sh --server 192.168.1.10:/ --mount /mnt/test --iters 5

set -euo pipefail

# --- Defaults ---
NFS_TARGET="127.0.0.1:/"
MOUNT_POINT="/mnt/cthon04"
MAX_ITERS=1
CTHON_DIR="/home/loghyr/reffs/cthon04"
LOG_DIR="/home/loghyr/reffs/cthon_logs"

# --- Argument parsing ---
while [[ $# -gt 0 ]]; do
    case "$1" in
        --server)
            NFS_TARGET="$2"
            shift 2
            ;;
        --mount)
            MOUNT_POINT="$2"
            shift 2
            ;;
        --iters)
            MAX_ITERS="$2"
            shift 2
            ;;
        --cthon)
            CTHON_DIR="$2"
            shift 2
            ;;
        --logdir)
            LOG_DIR="$2"
            shift 2
            ;;
        *)
            echo "Unknown option: $1" >&2
            echo "Usage: $0 [--server <server:export>] [--mount <mountpoint>] [--iters <n>] [--cthon <cthon04-dir>] [--logdir <dir>]" >&2
            exit 1
            ;;
    esac
done

if ! [[ "${MAX_ITERS}" =~ ^[1-9][0-9]*$ ]]; then
    echo "ERROR: --iters must be a positive integer, got '${MAX_ITERS}'" >&2
    exit 1
fi

if [[ ! -d "${CTHON_DIR}" ]]; then
    echo "ERROR: cthon04 directory not found: ${CTHON_DIR}" >&2
    exit 1
fi

for suite in basic general special; do
    if [[ ! -x "${CTHON_DIR}/${suite}/runtests" ]]; then
        echo "ERROR: Cannot find executable ${CTHON_DIR}/${suite}/runtests" >&2
        exit 1
    fi
done

# Directory cthon04 will use for its test files (on the NFS mount)
TEST_SUBDIR="${MOUNT_POINT}/cthon04_testdir"

# --- Log setup ---
mkdir -p "${LOG_DIR}"
RUN_LOG="${LOG_DIR}/run_$(date +%Y%m%d_%H%M%S).log"
echo "Logging to: ${RUN_LOG}"

log() {
    local msg="[$(date '+%Y-%m-%d %H:%M:%S')] $*"
    echo "${msg}"
    echo "${msg}" >> "${RUN_LOG}"
}

# --- Mount ---
mount_nfs() {
    if mountpoint -q "${MOUNT_POINT}" 2>/dev/null; then
        log "Unmounting existing mount at ${MOUNT_POINT}"
        umount "${MOUNT_POINT}" || { log "ERROR: Failed to unmount ${MOUNT_POINT}"; return 1; }
    fi
    log "Mounting ${NFS_TARGET} -> ${MOUNT_POINT}"
    mkdir -p "${MOUNT_POINT}"
    if ! mount -o tcp,mountproto=tcp,vers=3,nolock "${NFS_TARGET}" "${MOUNT_POINT}"; then
        log "ERROR: Failed to mount ${NFS_TARGET} on ${MOUNT_POINT}"
        return 1
    fi
}

unmount_nfs() {
    if mountpoint -q "${MOUNT_POINT}" 2>/dev/null; then
        log "Unmounting ${MOUNT_POINT}"
        umount "${MOUNT_POINT}" || log "WARNING: umount failed (ignored)"
    fi
}

# --- Run one sub-suite ---
run_test() {
    local name="$1"   # basic, general, special, or lock
    local iter="$2"
    local iter_log="${LOG_DIR}/iter_${iter}_${name}.log"

    log "  Running ${name} tests..."
    if (cd "${CTHON_DIR}/${name}" && NFSTESTDIR="${TEST_SUBDIR}" ./runtests) >> "${iter_log}" 2>&1; then
        log "  ${name}: PASSED"
        return 0
    else
        local rc=$?
        log "  ${name}: FAILED (exit code ${rc}) -- see ${iter_log}"
        return "${rc}"
    fi
}

# --- Main ---
mount_nfs || exit 1
trap unmount_nfs EXIT

log "Starting cthon04 loop: up to ${MAX_ITERS} iterations"
log "  Server : ${NFS_TARGET}"
log "  Mount  : ${MOUNT_POINT}"
log "  Cthon  : ${CTHON_DIR}"
log "  Testdir: ${TEST_SUBDIR}"
log "  Logdir : ${LOG_DIR}"

FAILED_ITER=0
FAILED_TEST=""

for (( i=1; i<=MAX_ITERS; i++ )); do
    log "=== Iteration ${i}/${MAX_ITERS} ==="

    # for suite in basic general special lock; do
    for suite in basic general special; do
        if ! run_test "${suite}" "${i}"; then
            FAILED_ITER=${i}
            FAILED_TEST="${suite}"
            break 2
        fi
    done

    log "  Iteration ${i} complete: all tests passed"
done

if [[ ${FAILED_ITER} -ne 0 ]]; then
    log "FAILURE detected on iteration ${FAILED_ITER}, test suite: ${FAILED_TEST}"
    log "Check logs in ${LOG_DIR}/ for details"
    exit 1
else
    log "All ${MAX_ITERS} iterations completed successfully."
    exit 0
fi
