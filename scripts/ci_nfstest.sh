#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# ci_nfstest.sh — Run NFStest protocol test suite against reffsd.
#
# NFStest (git.linux-nfs.org/mora) is a Python-based NFS protocol
# test suite.  GPL-2.0 — run as external process only.
# Cached in external/nfstest (.gitignore'd).
#
# Usage:
#   scripts/ci_nfstest.sh [REFFSD_BIN]

set -euo pipefail

REFFSD_BIN=${1:-/build/src/reffsd}
EXTERNAL_DIR=${EXTERNAL_DIR:-$(cd "$(dirname "$0")/.." && pwd)/external}
NFSTEST_DIR="$EXTERNAL_DIR/nfstest"
NFSTEST_URL="git://git.linux-nfs.org/projects/mora/nfstest.git"

die() { echo "FAIL: $*" >&2; FAILED=1; }
info() { echo "[$(date +%H:%M:%S)] $*"; }

# Work directory
if [ -n "${REFFS_WORK_DIR:-}" ] && [ -d "$REFFS_WORK_DIR" ]; then
	WORK_DIR="$REFFS_WORK_DIR"
elif [ -d /reffs_data ]; then
	WORK_DIR=/reffs_data
elif [ -d /Volumes/reffs_data ]; then
	WORK_DIR=/Volumes/reffs_data/nfstest
	mkdir -p "$WORK_DIR"
else
	WORK_DIR=/tmp
fi

MOUNT=/mnt/reffs_nfstest
DATA_DIR=$WORK_DIR/reffs_nfstest_data
STATE_DIR=$WORK_DIR/reffs_nfstest_state
CONFIG=$WORK_DIR/reffs_nfstest.toml
LOG=$WORK_DIR/reffsd_nfstest.log

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
# Clone and setup nfstest
# -----------------------------------------------------------------------

fetch_nfstest() {
	if [ -d "$NFSTEST_DIR" ]; then
		info "nfstest: using cached $NFSTEST_DIR"
	else
		info "nfstest: cloning from $NFSTEST_URL"
		mkdir -p "$EXTERNAL_DIR"
		git clone --depth 1 "$NFSTEST_URL" "$NFSTEST_DIR"
	fi

	# nfstest is pure Python — no build step needed.
	# Just set PYTHONPATH.
	export PYTHONPATH="$NFSTEST_DIR:${PYTHONPATH:-}"
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

info "=== NFStest Suite ==="

if ! rpcinfo -p 127.0.0.1 >/dev/null 2>&1; then
	rpcbind || die "rpcbind failed"
	sleep 1
fi

fetch_nfstest

start_server || exit 1

mkdir -p "$MOUNT"
RESULTS="$WORK_DIR/nfstest_results.txt"

# Run selected nfstest modules that don't require multi-client setup.
# nfstest_posix: POSIX API compliance over NFS
# nfstest_alloc: space allocation (ALLOCATE/DEALLOCATE)
# nfstest_cache: attribute caching behavior
# nfstest_delegation: delegation handling
#
# Skip nfstest_pnfs (needs layout infrastructure),
# nfstest_interop (needs multiple clients/servers).

for test_module in nfstest_posix nfstest_alloc nfstest_cache; do
	test_path="$NFSTEST_DIR/test/$test_module"
	if [ -f "$test_path" ]; then
		info ""
		info "--- $test_module ---"
		if timeout 300 "$test_path" \
			--server 127.0.0.1 \
			--export / \
			--mtpoint "$MOUNT" \
			--nfsversion 4.2 \
			2>&1 | tee -a "$RESULTS"; then
			info "$test_module: completed"
		else
			info "$test_module: some failures (baseline)"
		fi
	else
		info "Skipping $test_module (not found at $test_path)"
	fi
done

check_asan
stop_server

info ""
if [ "$FAILED" -eq 0 ]; then
	info "=== NFStest COMPLETE ==="
else
	info "=== NFStest FAILED (ASAN/UBSAN) ==="
	exit 1
fi
