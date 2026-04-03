#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# ci_pynfs.sh — Run pynfs NFSv4.1 test suite against reffsd.
#
# pynfs is GPL-2.0 — run as an external process only, never vendor
# or link.  Cached in external/ (.gitignore'd).
#
# Runs the NFSv4.1 test suite (nfs4.1/testserver.py) which exercises
# EXCHANGE_ID, CREATE_SESSION, SEQUENCE, OPEN, CLOSE, GETATTR, SETATTR,
# LOOKUP, READDIR, RENAME, REMOVE, LOCK, DELEGATION, etc.
#
# Usage:
#   scripts/ci_pynfs.sh [REFFSD_BIN]
#
# Prerequisites: rpcbind, python3, python3-ply, python3-gssapi, xdrlib3.

set -euo pipefail

REFFSD_BIN=${1:-/build/src/reffsd}
EXTERNAL_DIR=${EXTERNAL_DIR:-$(cd "$(dirname "$0")/.." && pwd)/external}
PYNFS_DIR="$EXTERNAL_DIR/pynfs"
PYNFS_URL="git://git.linux-nfs.org/projects/cdmackay/pynfs.git"

die() { echo "FAIL: $*" >&2; FAILED=1; }
info() { echo "[$(date +%H:%M:%S)] $*"; }

# Work directory
if [ -n "${REFFS_WORK_DIR:-}" ] && [ -d "$REFFS_WORK_DIR" ]; then
	WORK_DIR="$REFFS_WORK_DIR"
elif [ -d /reffs_data ]; then
	WORK_DIR=/reffs_data
elif [ -d /Volumes/reffs_data ]; then
	WORK_DIR=/Volumes/reffs_data/pynfs
	mkdir -p "$WORK_DIR"
else
	WORK_DIR=/tmp
fi

DATA_DIR=$WORK_DIR/reffs_pynfs_data
STATE_DIR=$WORK_DIR/reffs_pynfs_state
CONFIG=$WORK_DIR/reffs_pynfs.toml
LOG=$WORK_DIR/reffsd_pynfs.log

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
# Clone and build pynfs (cached in external/)
# -----------------------------------------------------------------------

fetch_pynfs() {
	if [ -d "$PYNFS_DIR" ]; then
		info "pynfs: using cached $PYNFS_DIR"
	else
		info "pynfs: cloning from $PYNFS_URL"
		mkdir -p "$EXTERNAL_DIR"
		git clone --depth 1 "$PYNFS_URL" "$PYNFS_DIR"
	fi

	# Set up a virtual environment for pynfs dependencies.
	# ply is GPL — installed as a runtime tool dependency in the
	# venv, not linked into reffs.
	PYNFS_VENV="$PYNFS_DIR/.venv"
	if [ ! -d "$PYNFS_VENV" ]; then
		info "pynfs: creating virtual environment"
		python3 -m venv "$PYNFS_VENV"
	fi
	. "$PYNFS_VENV/bin/activate"

	pip install -q setuptools ply xdrlib3 2>&1 | tail -3 || true

	# Build pynfs v4.1 only (generates XDR code from .x files).
	# Skip 4.0 — we only test NFSv4.1/4.2.
	if [ ! -d "$PYNFS_DIR/nfs4.1/nfs4" ]; then
		info "pynfs: building (setup.py build)"
		(cd "$PYNFS_DIR" && python3 setup.py build) 2>&1 | tail -5
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

info "=== pynfs NFSv4.1 Test Suite ==="

if ! rpcinfo -p 127.0.0.1 >/dev/null 2>&1; then
	rpcbind || die "rpcbind failed"
	sleep 1
fi

fetch_pynfs

start_server || exit 1

# pynfs connects directly to the server — no kernel mount needed.
# testserver.py takes server:path as the first argument.
info "Running pynfs v4.1 tests against 127.0.0.1:/"

RESULTS_FILE="$WORK_DIR/pynfs_results.txt"

cd "$PYNFS_DIR/nfs4.1"
# Skip tests that hang, timeout, or test unimplemented features:
#   courteous: sleeps for lease_time+10 per test (55s+ each)
#   reboot: grace period sleeps
#   flex: pNFS-specific (enable when testing pNFS)
#   deleg: CB_RECALL stateid incompatible with pynfs callback model
#   xattr: extended attributes not implemented (NFS4ERR_NOTSUPP)
if timeout 600 python3 testserver.py 127.0.0.1:/ \
	--maketree --rundeps -v \
	all nocourteous noreboot noflex nodeleg noxattr nodestroy_clientid 2>&1 | tee "$RESULTS_FILE"; then
	info "pynfs: ALL PASSED"
else
	info "pynfs: some tests failed (baseline run)"
fi

cd /
check_asan
stop_server

# Summary: count pass/fail from output
PASS_COUNT=$(grep -c '^\*\*\*\? PASS' "$RESULTS_FILE" 2>/dev/null || echo 0)
FAIL_COUNT=$(grep -c '^\*\*\*\? FAILURE' "$RESULTS_FILE" 2>/dev/null || echo 0)
WARN_COUNT=$(grep -c '^\*\*\*\? WARNING' "$RESULTS_FILE" 2>/dev/null || echo 0)
SKIP_COUNT=$(grep -c 'DEPENDENCY' "$RESULTS_FILE" 2>/dev/null || echo 0)

info ""
info "=== pynfs Results ==="
info "  PASS: $PASS_COUNT"
info "  FAIL: $FAIL_COUNT"
info "  WARN: $WARN_COUNT"
info "  SKIP: $SKIP_COUNT"

if [ "$FAILED" -eq 0 ]; then
	info "=== pynfs COMPLETE (no ASAN/UBSAN errors) ==="
else
	info "=== pynfs FAILED (ASAN/UBSAN errors in server) ==="
	exit 1
fi
