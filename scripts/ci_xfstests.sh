#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# ci_xfstests.sh -- Run xfstests NFS group against reffsd or an external server.
#
# xfstests is GPL-2.0 -- run as external process only.
# Cached in external/xfstests-dev (.gitignore'd).
#
# Requires two mount points: test and scratch, both from the same NFS export.
# xfstests does not mkfs NFS mounts -- it just wipes content under SCRATCH_MNT.
#
# Test failures are treated as baseline (no exclude list yet); CI only blocks
# on ASAN/UBSAN errors in a locally-started reffsd.
#
# Usage:
#   ci_xfstests.sh [REFFSD_BIN]
#   ci_xfstests.sh --server HOST [--test-mnt DIR] [--scratch-mnt DIR]

set -euo pipefail

REFFSD_BIN=""
XFST_SERVER=""
XFST_TEST_MNT_ARG=""
XFST_SCRATCH_MNT_ARG=""
EXTERNAL_MODE=false

while [[ $# -gt 0 ]]; do
	case "$1" in
		--server)     XFST_SERVER="$2"; EXTERNAL_MODE=true; shift 2 ;;
		--test-mnt)   XFST_TEST_MNT_ARG="$2"; shift 2 ;;
		--scratch-mnt) XFST_SCRATCH_MNT_ARG="$2"; shift 2 ;;
		*)            REFFSD_BIN="$1"; shift ;;
	esac
done

if [ "$EXTERNAL_MODE" = false ]; then
	REFFSD_BIN=${REFFSD_BIN:-/build/src/reffsd}
	XFST_SERVER="127.0.0.1"
fi

EXTERNAL_DIR=${EXTERNAL_DIR:-$(cd "$(dirname "$0")/.." && pwd)/external}
XFSTESTS_DIR="$EXTERNAL_DIR/xfstests-dev"
XFSTESTS_URL="git://git.kernel.org/pub/scm/fs/xfs/xfstests-dev.git"

die() { echo "FAIL: $*" >&2; FAILED=1; }
info() { echo "[$(date +%H:%M:%S)] $*"; }

if [ -n "${REFFS_WORK_DIR:-}" ] && [ -d "$REFFS_WORK_DIR" ]; then
	WORK_DIR="$REFFS_WORK_DIR"
elif [ -d /reffs_data ]; then
	WORK_DIR=/reffs_data
else
	WORK_DIR=/tmp
fi

TEST_MNT="${XFST_TEST_MNT_ARG:-/mnt/reffs_xfs_test}"
SCRATCH_MNT="${XFST_SCRATCH_MNT_ARG:-/mnt/reffs_xfs_scratch}"
DATA_DIR=$WORK_DIR/reffs_xfstests_data
STATE_DIR=$WORK_DIR/reffs_xfstests_state
CONFIG=$WORK_DIR/reffs_xfstests.toml
LOG=$WORK_DIR/reffsd_xfstests.log
RESULTS=$WORK_DIR/xfstests_results.txt

REFFSD_PID=
FAILED=0
_mount_test=false
_mount_scratch=false

cleanup() {
	set +e
	[ "$_mount_test" = true ] && umount -f "$TEST_MNT" 2>/dev/null || true
	[ "$_mount_scratch" = true ] && umount -f "$SCRATCH_MNT" 2>/dev/null || true
	if [ -n "$REFFSD_PID" ]; then
		kill -TERM "$REFFSD_PID" 2>/dev/null
		sleep 2
		kill -KILL "$REFFSD_PID" 2>/dev/null
		wait "$REFFSD_PID" 2>/dev/null
	fi
}
trap cleanup EXIT

# -----------------------------------------------------------------------
# Clone and build xfstests
# -----------------------------------------------------------------------

fetch_xfstests() {
	if [ -d "$XFSTESTS_DIR" ]; then
		info "xfstests: using cached $XFSTESTS_DIR"
	else
		info "xfstests: cloning from $XFSTESTS_URL"
		mkdir -p "$EXTERNAL_DIR"
		git clone --depth 1 "$XFSTESTS_URL" "$XFSTESTS_DIR"
	fi
	if [ ! -f "$XFSTESTS_DIR/check" ]; then
		info "xfstests: building"
		make -C "$XFSTESTS_DIR" -j"$(nproc)" 2>&1 | tail -5
	fi
}

# -----------------------------------------------------------------------
# Server management
# -----------------------------------------------------------------------

start_server() {
	rm -rf "$DATA_DIR" "$STATE_DIR"
	mkdir -p "$DATA_DIR" "$STATE_DIR"

	cat >"$CONFIG" <<EOF
[server]
port           = 2049
bind           = "*"
role           = "standalone"
minor_versions = [1, 2]
grace_period   = 5
workers        = 8

[backend]
type       = "posix"
path       = "$DATA_DIR"
state_file = "$STATE_DIR"

[[export]]
path        = "/"
clients     = "*"
access      = "rw"
root_squash = false
flavors     = ["sys"]
EOF

	info "Starting reffsd..."
	: > "$LOG"
	ASAN_OPTIONS="detect_leaks=0:halt_on_error=0" \
	UBSAN_OPTIONS="halt_on_error=0:print_stacktrace=1" \
	"$REFFSD_BIN" --config="$CONFIG" >"$LOG" 2>&1 &
	REFFSD_PID=$!

	for i in $(seq 1 30); do
		(echo >/dev/tcp/127.0.0.1/2049) 2>/dev/null && break
		kill -0 "$REFFSD_PID" 2>/dev/null || {
			info "reffsd died during startup"
			cat "$LOG"
			return 1
		}
		sleep 1
	done
	info "reffsd up (PID $REFFSD_PID)"
}

stop_server() {
	if [ -n "$REFFSD_PID" ]; then
		kill -TERM "$REFFSD_PID" 2>/dev/null || true
		for i in $(seq 1 10); do
			kill -0 "$REFFSD_PID" 2>/dev/null || break
			sleep 1
		done
		kill -KILL "$REFFSD_PID" 2>/dev/null || true
		wait "$REFFSD_PID" 2>/dev/null || true
		REFFSD_PID=
	fi
}

check_asan() {
	if grep -qE "ERROR: AddressSanitizer" "$LOG" 2>/dev/null; then
		info "ASAN error detected!"
		grep -A5 "ERROR:" "$LOG" | head -20
		die "ASAN error in reffsd log"
	fi
	if grep -q "runtime error:" "$LOG" 2>/dev/null; then
		info "UBSAN error detected!"
		grep "runtime error:" "$LOG" | head -10
		die "UBSAN error in reffsd log"
	fi
}

# -----------------------------------------------------------------------
# Main
# -----------------------------------------------------------------------

info "=== xfstests NFS Suite ==="

fetch_xfstests

if [ "$EXTERNAL_MODE" = false ]; then
	for stale in $(mount -t nfs,nfs4 2>/dev/null | awk '{print $3}'); do
		info "Cleaning stale NFS mount: $stale"
		umount -f -l "$stale" 2>/dev/null || true
	done
	if ! rpcinfo -p 127.0.0.1 >/dev/null 2>&1; then
		rpcbind || die "rpcbind failed"
		sleep 1
	fi
	start_server || exit 1
fi

# Mount test and scratch.  In external mode, caller supplies pre-mounted
# dirs via --test-mnt / --scratch-mnt; otherwise we mount them ourselves.
if [ -z "$XFST_TEST_MNT_ARG" ]; then
	_mount_test=true
	umount -f "$TEST_MNT" 2>/dev/null || true
	mkdir -p "$TEST_MNT"
	mount -o vers=4.2,sec=sys "${XFST_SERVER}:/" "$TEST_MNT" || {
		info "Test mount failed"
		[ "$EXTERNAL_MODE" = false ] && stop_server
		exit 1
	}
fi

if [ -z "$XFST_SCRATCH_MNT_ARG" ]; then
	_mount_scratch=true
	umount -f "$SCRATCH_MNT" 2>/dev/null || true
	mkdir -p "$SCRATCH_MNT"
	mount -o vers=4.2,sec=sys "${XFST_SERVER}:/" "$SCRATCH_MNT" || {
		info "Scratch mount failed"
		[ "$EXTERNAL_MODE" = false ] && stop_server
		exit 1
	}
fi

# Write local.config for xfstests.
LOCAL_CONFIG="$XFSTESTS_DIR/local.config"
cat >"$LOCAL_CONFIG" <<EOF
FSTYP=nfs
TEST_DEV=${XFST_SERVER}:/
TEST_DIR=$TEST_MNT
SCRATCH_DEV=${XFST_SERVER}:/
SCRATCH_MNT=$SCRATCH_MNT
MOUNT_OPTIONS="-o vers=4.2,sec=sys"
NFS_VERSION=4.2
EOF

info "Running xfstests -g nfs (30-minute cap)..."
info "Failures are baseline -- no exclude list yet."
# All test failures are informational: 'check' exits non-zero on any failure
# but we use '|| true' so CI is only blocked by ASAN/UBSAN errors below.
(
	cd "$XFSTESTS_DIR"
	timeout --kill-after=60 1800 ./check -nfs -g nfs 2>&1 | tee "$RESULTS" || true
)

info ""
info "=== xfstests Summary ==="
grep -E '(Passed|Failed|Not run|Ran:)' "$RESULTS" 2>/dev/null | tail -5 || true

if [ "$EXTERNAL_MODE" = false ]; then
	check_asan
	stop_server
fi

info ""
if [ "$FAILED" -eq 0 ]; then
	info "=== xfstests COMPLETE ==="
else
	info "=== xfstests FAILED (ASAN/UBSAN) ==="
	exit 1
fi
