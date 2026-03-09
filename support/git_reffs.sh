#!/bin/bash
# SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: GPL-2.0+

# git_reffs.sh - Clone reffs.git onto a reffs mount and run tests
#
# Usage: ./git_reffs.sh [--server <server:export>] [--mount <mountpoint>]
#                       [--repo <git-url>] [--logdir <dir>] [--iters <count>]
#                       [--no-build] [--no-unit-test]
#
# Defaults:
#   --server  127.0.0.1:/
#   --mount   /mnt/reffs
#   --repo    /home/loghyr/reffs
#   --logdir  /home/loghyr/reffs/git_logs
#   --iters   1
#
# Example:
#   ./git_reffs.sh
#   ./git_reffs.sh --server 127.0.0.1:/ --mount /mnt/reffs --repo /home/loghyr/reffs --iters 10

set -euo pipefail

# --- Defaults ---
NFS_TARGET="127.0.0.1:/"
MOUNT_POINT="/mnt/reffs"
REPO_URL="/home/loghyr/reffs"
LOG_DIR="/home/loghyr/reffs/git_logs"
ITERS=1
RUN_BUILD=true
RUN_UNIT_TEST=true

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
        --repo)
            REPO_URL="$2"
            shift 2
            ;;
        --logdir)
            LOG_DIR="$2"
            shift 2
            ;;
        --iters)
            ITERS="$2"
            shift 2
            ;;
        --no-build)
            RUN_BUILD=false
            RUN_UNIT_TEST=false
            shift
            ;;
        --no-unit-test)
            RUN_UNIT_TEST=false
            shift
            ;;
        *)
            echo "Unknown option: $1" >&2
            echo "Usage: $0 [--server <server:export>] [--mount <mountpoint>] [--repo <git-url>] [--logdir <dir>] [--iters <count>] [--no-build] [--no-unit-test]" >&2
            exit 1
            ;;
    esac
done

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
        sudo umount "${MOUNT_POINT}" || { log "ERROR: Failed to unmount ${MOUNT_POINT}"; return 1; }
    fi
    log "Mounting ${NFS_TARGET} -> ${MOUNT_POINT}"
    sudo mkdir -p "${MOUNT_POINT}"
    sudo chmod 777 "${MOUNT_POINT}"
    if ! sudo mount -o tcp,mountproto=tcp,vers=3,nolock "${NFS_TARGET}" "${MOUNT_POINT}"; then
        log "ERROR: Failed to mount ${NFS_TARGET} on ${MOUNT_POINT}"
        return 1
    fi
    # Ensure wide open permissions on the mounted root
    sudo chmod 777 "${MOUNT_POINT}"
}

unmount_nfs() {
    if mountpoint -q "${MOUNT_POINT}" 2>/dev/null; then
        log "Unmounting ${MOUNT_POINT}"
        sudo umount "${MOUNT_POINT}" || log "WARNING: umount failed (ignored)"
    fi
}

# --- Main ---
mount_nfs || exit 1
# trap unmount_nfs EXIT

log "Starting git clone test..."
log "  Server : ${NFS_TARGET}"
log "  Mount  : ${MOUNT_POINT}"
log "  Repo   : ${REPO_URL}"
log "  Iters  : ${ITERS}"
log "  Build  : ${RUN_BUILD}"
log "  Tests  : ${RUN_UNIT_TEST}"

for ((i=1; i<=ITERS; i++)); do
    log "Iteration $i / $ITERS"

    cd "${MOUNT_POINT}"

    log "Cleaning up old clone..."
    sudo rm -rf "reffs"

    log "Cloning repository..."
    if git clone "${REPO_URL}"; then
        log "Clone: PASSED"
    else
        log "Clone: FAILED"
        exit 1
    fi

    REPO_DIR="${MOUNT_POINT}/reffs"
    if [[ ! -d "${REPO_DIR}" ]]; then
        # Fallback: find the directory created if it's not 'reffs'
        REPO_DIR=$(ls -td "${MOUNT_POINT}"/*/ | head -1)
    fi

    cd "${REPO_DIR}"

    if [ "$RUN_BUILD" = true ]; then
        log "Building inside mount..."
        if make -f Makefile.reffs build; then
            log "Build: PASSED"
        else
            log "Build: FAILED"
            exit 1
        fi
    fi

    if [ "$RUN_UNIT_TEST" = true ]; then
        log "Running unit tests inside mount..."
        if make -f Makefile.reffs check; then
            log "Tests: PASSED"
        else
            log "Tests: FAILED"
            exit 1
        fi
    fi
done

log "Git Reffs test completed successfully after ${ITERS} iterations."
exit 0
