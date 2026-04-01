#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# ci_libnfs.sh — Run libnfs test suite and examples against reffsd.
#
# libnfs is a userspace NFS client library (BSD/LGPL/GPL multi-licensed).
# Bypasses the kernel NFS client entirely — no dcache, no attribute
# cache, no delegations.  Pure protocol testing.
#
# Cached in external/libnfs (.gitignore'd).
#
# Usage:
#   scripts/ci_libnfs.sh [REFFSD_BIN]

set -euo pipefail

REFFSD_BIN=${1:-/build/src/reffsd}
EXTERNAL_DIR=${EXTERNAL_DIR:-$(cd "$(dirname "$0")/.." && pwd)/external}
LIBNFS_DIR="$EXTERNAL_DIR/libnfs"
LIBNFS_URL="https://github.com/sahlberg/libnfs.git"

die() { echo "FAIL: $*" >&2; FAILED=1; }
info() { echo "[$(date +%H:%M:%S)] $*"; }

# Work directory
if [ -n "${REFFS_WORK_DIR:-}" ] && [ -d "$REFFS_WORK_DIR" ]; then
	WORK_DIR="$REFFS_WORK_DIR"
elif [ -d /reffs_data ]; then
	WORK_DIR=/reffs_data
elif [ -d /Volumes/reffs_data ]; then
	WORK_DIR=/Volumes/reffs_data/libnfs
	mkdir -p "$WORK_DIR"
else
	WORK_DIR=/tmp
fi

DATA_DIR=$WORK_DIR/reffs_libnfs_data
STATE_DIR=$WORK_DIR/reffs_libnfs_state
CONFIG=$WORK_DIR/reffs_libnfs.toml
LOG=$WORK_DIR/reffsd_libnfs.log

REFFSD_PID=
FAILED=0

cleanup() {
	set +e
	if [ -n "$REFFSD_PID" ]; then
		kill -TERM "$REFFSD_PID" 2>/dev/null
		sleep 2
		kill -KILL "$REFFSD_PID" 2>/dev/null
		wait "$REFFSD_PID" 2>/dev/null
	fi
}
trap cleanup EXIT

# -----------------------------------------------------------------------
# Clone and build libnfs
# -----------------------------------------------------------------------

fetch_libnfs() {
	if [ -d "$LIBNFS_DIR" ]; then
		info "libnfs: using cached $LIBNFS_DIR"
	else
		info "libnfs: cloning from $LIBNFS_URL"
		mkdir -p "$EXTERNAL_DIR"
		git clone --depth 1 "$LIBNFS_URL" "$LIBNFS_DIR"
	fi

	if [ ! -f "$LIBNFS_DIR/lib/.libs/libnfs.so" ] && \
	   [ ! -f "$LIBNFS_DIR/build/lib/libnfs.so" ]; then
		info "libnfs: building"
		if [ -f "$LIBNFS_DIR/CMakeLists.txt" ]; then
			# CMake build — enable tests and examples
			mkdir -p "$LIBNFS_DIR/build"
			(cd "$LIBNFS_DIR/build" && \
			 cmake .. -DENABLE_TESTS=ON -DENABLE_EXAMPLES=ON && \
			 make -j"$(nproc)") 2>&1 | tail -5
		else
			# Autotools build
			(cd "$LIBNFS_DIR" && ./bootstrap && ./configure && make -j"$(nproc)") 2>&1 | tail -5
		fi
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

info "=== libnfs Test Suite ==="

if ! rpcinfo -p 127.0.0.1 >/dev/null 2>&1; then
	rpcbind || die "rpcbind failed"
	sleep 1
fi

fetch_libnfs

start_server || exit 1

RESULTS="$WORK_DIR/libnfs_results.txt"
NFS_URL="nfs://127.0.0.1/"

# Run libnfs tests
info "Running libnfs tests against $NFS_URL"

BUILD="$LIBNFS_DIR/build"

# libnfs tests are shell scripts that use the built utils.
# They need LD_LIBRARY_PATH and the NFS URL as env vars.
export LD_LIBRARY_PATH="$BUILD/lib:${LD_LIBRARY_PATH:-}"
export PATH="$BUILD/utils:$BUILD/examples:$PATH"
export LIBNFS_URL="$NFS_URL"

for test_script in "$BUILD"/tests/test_*.sh; do
	[ -f "$test_script" ] || continue
	name=$(basename "$test_script" .sh)
	info "  $name..."
	if timeout 30 bash "$test_script" "$NFS_URL" 2>&1; then
		echo "  $name: PASS" | tee -a "$RESULTS"
	else
		echo "  $name: FAIL (rc=$?)" | tee -a "$RESULTS"
	fi
done

# Smoke test with nfs-ls
if [ -x "$BUILD/utils/nfs-ls" ]; then
	info "  nfs-ls smoke test..."
	if timeout 10 "$BUILD/utils/nfs-ls" "$NFS_URL" 2>&1 | head -5; then
		echo "  nfs-ls: PASS" | tee -a "$RESULTS"
	else
		echo "  nfs-ls: FAIL" | tee -a "$RESULTS"
	fi
fi

check_asan
stop_server

# Summary
PASS_COUNT=$(grep -c 'PASS' "$RESULTS" 2>/dev/null || echo 0)
FAIL_COUNT=$(grep -c 'FAIL' "$RESULTS" 2>/dev/null || echo 0)

info ""
info "=== libnfs Results ==="
info "  PASS: $PASS_COUNT"
info "  FAIL: $FAIL_COUNT"

if [ "$FAILED" -eq 0 ]; then
	info "=== libnfs COMPLETE ==="
else
	info "=== libnfs FAILED (ASAN/UBSAN) ==="
	exit 1
fi
