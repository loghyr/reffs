#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# ci_sigmund.sh -- Run sigmund NFS test framework against reffsd.
#
# Sigmund is a bash-based NFS test wrapper that exercises POSIX
# operations over an NFS mount.  Cached in external/sigmund.
#
# Usage:
#   scripts/ci_sigmund.sh [REFFSD_BIN]

set -euo pipefail

REFFSD_BIN=${1:-/build/src/reffsd}
EXTERNAL_DIR=${EXTERNAL_DIR:-$(cd "$(dirname "$0")/.." && pwd)/external}
SIGMUND_DIR="$EXTERNAL_DIR/sigmund"
SIGMUND_URL="https://github.com/phdeniel/sigmund.git"

die() { echo "FAIL: $*" >&2; FAILED=1; }
info() { echo "[$(date +%H:%M:%S)] $*"; }

# Work directory
if [ -n "${REFFS_WORK_DIR:-}" ] && [ -d "$REFFS_WORK_DIR" ]; then
	WORK_DIR="$REFFS_WORK_DIR"
elif [ -d /reffs_data ]; then
	WORK_DIR=/reffs_data
elif [ -d /Volumes/reffs_data ]; then
	WORK_DIR=/Volumes/reffs_data/sigmund
	mkdir -p "$WORK_DIR"
else
	WORK_DIR=/tmp
fi

MOUNT=/mnt/reffs_sigmund
DATA_DIR=$WORK_DIR/reffs_sigmund_data
STATE_DIR=$WORK_DIR/reffs_sigmund_state
CONFIG=$WORK_DIR/reffs_sigmund.toml
LOG=$WORK_DIR/reffsd_sigmund.log

REFFSD_PID=
FAILED=0

cleanup() {
	set +e
	umount -f "$MOUNT" 2>/dev/null
	if [ -n "$REFFSD_PID" ]; then
		kill -TERM "$REFFSD_PID" 2>/dev/null
		sleep 2
		kill -KILL "$REFFSD_PID" 2>/dev/null
		wait "$REFFSD_PID" 2>/dev/null
	fi
}
trap cleanup EXIT

# -----------------------------------------------------------------------
# Clone and build sigmund
# -----------------------------------------------------------------------

fetch_sigmund() {
	if [ -d "$SIGMUND_DIR" ]; then
		info "sigmund: using cached $SIGMUND_DIR"
	else
		info "sigmund: cloning from $SIGMUND_URL"
		mkdir -p "$EXTERNAL_DIR"
		git clone --depth 1 "$SIGMUND_URL" "$SIGMUND_DIR"
	fi

	# Build test programs if needed
	if [ -f "$SIGMUND_DIR/build_test.sh" ] && [ ! -f "$SIGMUND_DIR/test_progs/test_open" ]; then
		info "sigmund: building test programs"
		(cd "$SIGMUND_DIR" && bash build_test.sh) 2>&1 | tail -5
	fi
}

# -----------------------------------------------------------------------
# Server management
# -----------------------------------------------------------------------

start_server() {
	rm -rf "$DATA_DIR" "$STATE_DIR"
	mkdir -p "$DATA_DIR" "$STATE_DIR" "$MOUNT"

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

info "=== Sigmund NFS Test Suite ==="

if ! rpcinfo -p 127.0.0.1 >/dev/null 2>&1; then
	rpcbind || die "rpcbind failed"
	sleep 1
fi

fetch_sigmund

start_server || exit 1

mkdir -p "$MOUNT"
mount -o vers=4.2,sec=sys 127.0.0.1:/ "$MOUNT" || {
	die "mount failed"
	exit 1
}

RESULTS="$WORK_DIR/sigmund_results.txt"
TESTDIR="$MOUNT/sigmund_test"
mkdir -p "$TESTDIR"

info "Running sigmund on $TESTDIR"

# Create sigmund config in the sigmund directory
cat > "$SIGMUND_DIR/run_test.rc" <<EOF
TEST_USER=root
TEST_DIR=$TESTDIR
BUILD_TEST_DIR=$SIGMUND_DIR
EOF

# Run sigmund in quiet mode -- rcfile is positional, not -f
if (cd "$SIGMUND_DIR" && bash sigmund.sh nfs -q 2>&1) | tee "$RESULTS"; then
	info "sigmund: completed"
else
	info "sigmund: some failures (baseline)"
fi

rm -rf "$TESTDIR"
umount -f "$MOUNT" 2>/dev/null || true

check_asan
stop_server

info ""
if [ "$FAILED" -eq 0 ]; then
	info "=== Sigmund COMPLETE ==="
else
	info "=== Sigmund FAILED (ASAN/UBSAN) ==="
	exit 1
fi
