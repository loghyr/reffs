#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# ci_ltp.sh -- Run LTP NFS tests against reffsd or an external server.
#
# LTP is GPL-2.0 -- run as external process only.
# Cached in external/ltp (.gitignore'd).
#
# Only the NFS subset is built (testcases/network/nfs/) to avoid
# a full 20+ minute LTP build.
#
# Test failures are treated as baseline (no exclude list yet); CI only
# blocks on ASAN/UBSAN errors in a locally-started reffsd.
#
# Usage:
#   ci_ltp.sh [REFFSD_BIN]
#   ci_ltp.sh --server HOST [--mount DIR] [--port PORT]

set -euo pipefail

REFFSD_BIN=""
LTP_SERVER=""
LTP_PORT="2049"
LTP_MOUNT_ARG=""
EXTERNAL_MODE=false

while [[ $# -gt 0 ]]; do
	case "$1" in
		--server) LTP_SERVER="$2"; EXTERNAL_MODE=true; shift 2 ;;
		--port)   LTP_PORT="$2"; shift 2 ;;
		--mount)  LTP_MOUNT_ARG="$2"; shift 2 ;;
		*)        REFFSD_BIN="$1"; shift ;;
	esac
done

if [ "$EXTERNAL_MODE" = false ]; then
	REFFSD_BIN=${REFFSD_BIN:-/build/src/reffsd}
	LTP_SERVER="127.0.0.1"
	LTP_PORT="2049"
fi

EXTERNAL_DIR=${EXTERNAL_DIR:-$(cd "$(dirname "$0")/.." && pwd)/external}
LTP_DIR="$EXTERNAL_DIR/ltp"
LTP_URL="https://github.com/linux-test-project/ltp.git"

die() { echo "FAIL: $*" >&2; FAILED=1; }
info() { echo "[$(date +%H:%M:%S)] $*"; }

if [ -n "${REFFS_WORK_DIR:-}" ] && [ -d "$REFFS_WORK_DIR" ]; then
	WORK_DIR="$REFFS_WORK_DIR"
elif [ -d /reffs_data ]; then
	WORK_DIR=/reffs_data
else
	WORK_DIR=/tmp
fi

MOUNT="${LTP_MOUNT_ARG:-/mnt/reffs_ltp}"
DATA_DIR=$WORK_DIR/reffs_ltp_data
STATE_DIR=$WORK_DIR/reffs_ltp_state
CONFIG=$WORK_DIR/reffs_ltp.toml
LOG=$WORK_DIR/reffsd_ltp.log
RESULTS=$WORK_DIR/ltp_results.txt

REFFSD_PID=
FAILED=0
_our_mount=false

cleanup() {
	set +e
	[ "$_our_mount" = true ] && umount -f -l "$MOUNT" 2>/dev/null || true
	if [ -n "$REFFSD_PID" ]; then
		kill -TERM "$REFFSD_PID" 2>/dev/null
		sleep 2
		kill -KILL "$REFFSD_PID" 2>/dev/null
		wait "$REFFSD_PID" 2>/dev/null
	fi
}
trap cleanup EXIT

# -----------------------------------------------------------------------
# Clone and build LTP NFS subset
# -----------------------------------------------------------------------

fetch_ltp() {
	if [ -d "$LTP_DIR" ]; then
		info "ltp: using cached $LTP_DIR"
	else
		info "ltp: cloning from $LTP_URL"
		mkdir -p "$EXTERNAL_DIR"
		git clone --depth 1 "$LTP_URL" "$LTP_DIR"
	fi

	if [ ! -f "$LTP_DIR/testcases/network/nfs/nfs01" ]; then
		info "ltp: building NFS subset"
		(
			cd "$LTP_DIR"
			if [ ! -f configure ]; then
				make autotools 2>&1 | tail -5
			fi
			if [ ! -f Makefile ]; then
				./configure --quiet 2>&1 | tail -5
			fi
			make -C testcases/network/nfs/ -j"$(nproc)" 2>&1 | tail -10
		)
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
workers        = 4

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

info "=== LTP NFS Test Suite ==="

if [ "$EXTERNAL_MODE" = false ]; then
	for stale in $(mount -t nfs,nfs4 2>/dev/null | awk '{print $3}'); do
		info "Cleaning stale NFS mount: $stale"
		umount -f -l "$stale" 2>/dev/null || true
	done
	if ! rpcinfo -p 127.0.0.1 >/dev/null 2>&1; then
		rpcbind || die "rpcbind failed"
		sleep 1
	fi
fi

fetch_ltp

if [ "$EXTERNAL_MODE" = false ]; then
	start_server || exit 1
fi

# Mount unless caller supplied a pre-mounted directory.
if [ -n "$LTP_MOUNT_ARG" ]; then
	_our_mount=false
else
	_our_mount=true
	umount -f -l "$MOUNT" 2>/dev/null || true
	mkdir -p "$MOUNT"
	mount -o vers=4.2,sec=sys "${LTP_SERVER}:/" "$MOUNT" || {
		info "NFS mount failed"
		[ "$EXTERNAL_MODE" = false ] && stop_server
		exit 1
	}
fi

info "Running LTP NFS tests against $MOUNT (20-minute cap)..."
info "Failures are baseline -- no exclude list yet."

# Create a scratch directory on the NFS mount for LTP.
LTP_TEST_DIR="$MOUNT/ltp_$$"
mkdir -p "$LTP_TEST_DIR" 2>/dev/null || true

(
	timeout --kill-after=60 1200 \
		"$LTP_DIR/runltp" \
			-p \
			-l "$RESULTS" \
			-d "$LTP_TEST_DIR" \
			-f nfs \
			-t 20m \
			2>&1 | tee "${RESULTS}.raw" || true
)

rm -rf "$LTP_TEST_DIR" 2>/dev/null || true

info ""
info "=== LTP NFS Summary ==="
if [ -f "$RESULTS" ]; then
	grep -E '(PASS|FAIL|BROK|CONF|Total)' "$RESULTS" 2>/dev/null | tail -10 || true
elif [ -f "${RESULTS}.raw" ]; then
	grep -E '(PASS|FAIL|BROK|CONF)' "${RESULTS}.raw" 2>/dev/null | tail -10 || true
fi

if [ "$_our_mount" = true ]; then
	umount -f -l "$MOUNT" 2>/dev/null || true
fi

if [ "$EXTERNAL_MODE" = false ]; then
	check_asan
	stop_server
fi

info ""
if [ "$FAILED" -eq 0 ]; then
	info "=== LTP NFS COMPLETE ==="
else
	info "=== LTP NFS FAILED (ASAN/UBSAN) ==="
	exit 1
fi
