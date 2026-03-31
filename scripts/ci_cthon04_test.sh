#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# ci_cthon04_test.sh — Run Connectathon (CTHON04) test suite against reffsd.
#
# Clones and caches cthon04 in external/ (out-of-tree, .gitignore'd).
# Runs basic, general, special, and lock tests against three configurations:
#   1. NFSv4.2 standalone
#   2. NFSv3 standalone
#   3. Combined pNFS (MDS+DS)
#
# Usage:
#   scripts/ci_cthon04_test.sh [REFFSD_BIN]
#
# Prerequisites: rpcbind, NFS client tools, build tools (make, cc).

set -euo pipefail

REFFSD_BIN=${1:-/build/src/reffsd}
BUILD_DIR=$(dirname "$(dirname "$REFFSD_BIN")")
EXTERNAL_DIR=${EXTERNAL_DIR:-$(cd "$(dirname "$0")/.." && pwd)/external}
CTHON_DIR="$EXTERNAL_DIR/cthon04"
CTHON_URL="git://git.linux-nfs.org/projects/steved/cthon04.git"

MOUNT=/mnt/reffs_cthon
DATA_DIR=/tmp/reffs_cthon_data
STATE_DIR=/tmp/reffs_cthon_state
DS_DIR=/tmp/reffs_cthon_ds
CONFIG=/tmp/reffs_cthon.toml
LOG=/tmp/reffsd_cthon.log

REFFSD_PID=
FAILED=0

die() { echo "FAIL: $*" >&2; FAILED=1; }
info() { echo "[$(date +%H:%M:%S)] $*"; }

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
# Clone and build cthon04 (cached in external/)
# -----------------------------------------------------------------------

fetch_cthon04() {
	if [ -d "$CTHON_DIR" ]; then
		info "cthon04: using cached $CTHON_DIR"
	else
		info "cthon04: cloning from $CTHON_URL"
		mkdir -p "$EXTERNAL_DIR"
		git clone --depth 1 "$CTHON_URL" "$CTHON_DIR"
	fi

	if [ ! -f "$CTHON_DIR/basic/runtests" ]; then
		info "cthon04: building"
		make -C "$CTHON_DIR" -j"$(nproc)" 2>&1 | tail -5
	fi
}

# -----------------------------------------------------------------------
# Server management
# -----------------------------------------------------------------------

write_config() {
	local role=$1
	local extra=${2:-}

	rm -rf "$DATA_DIR" "$STATE_DIR" "$DS_DIR"
	mkdir -p "$DATA_DIR" "$STATE_DIR" "$DS_DIR" "$MOUNT"

	cat >"$CONFIG" <<EOF
[server]
port           = 2049
bind           = "*"
role           = "$role"
minor_versions = [1, 2]
grace_period   = 5
workers        = 4

[backend]
type       = "posix"
path       = "$DATA_DIR"
state_file = "$STATE_DIR"
ds_path    = "$DS_DIR"

[[export]]
path        = "/"
clients     = "*"
access      = "rw"
root_squash = false
flavors     = ["sys"]
$extra
EOF
}

start_server() {
	info "Starting reffsd (role=$1)..."
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
# Run cthon04 tests
# -----------------------------------------------------------------------

run_cthon04() {
	local label=$1
	local mount_opts=$2
	local test_flags=${3:--a} # default: all tests (basic, general, special, lock)

	info "--- $label ---"

	umount -f "$MOUNT" 2>/dev/null || true
	mount -o "$mount_opts" 127.0.0.1:/ "$MOUNT" || {
		die "$label: mount failed"
		return 1
	}

	info "Running cthon04 ($test_flags) on $MOUNT"

	local pass=0
	local fail=0

	# Run each test set separately for better reporting
	for test_set in basic general special lock; do
		if [ -d "$CTHON_DIR/$test_set" ]; then
			info "  $test_set..."
			if (cd "$CTHON_DIR" && ./runtests -t "$MOUNT" -"${test_set:0:1}" 2>&1); then
				info "  $test_set: PASS"
				pass=$((pass + 1))
			else
				info "  $test_set: FAIL"
				fail=$((fail + 1))
			fi
		fi
	done

	umount -f "$MOUNT" 2>/dev/null || true

	check_asan

	info "$label: $pass passed, $fail failed"
	if [ "$fail" -gt 0 ]; then
		die "$label: $fail test sets failed"
	fi
}

# -----------------------------------------------------------------------
# Main
# -----------------------------------------------------------------------

info "=== CTHON04 Test Suite ==="

# Ensure rpcbind is running
if ! rpcinfo -p 127.0.0.1 >/dev/null 2>&1; then
	rpcbind || die "rpcbind failed"
	sleep 1
fi

fetch_cthon04

# ---------- Test 1: NFSv4.2 standalone ----------

info ""
info "========== NFSv4.2 Standalone =========="
write_config "standalone"
start_server "standalone" || exit 1
run_cthon04 "NFSv4.2" "vers=4.2,sec=sys"
stop_server

# ---------- Test 2: NFSv3 standalone ----------

info ""
info "========== NFSv3 Standalone =========="
write_config "standalone"
start_server "standalone" || exit 1
run_cthon04 "NFSv3" "vers=3,sec=sys"
stop_server

# ---------- Test 3: Combined pNFS (MDS+DS) ----------

info ""
info "========== Combined pNFS (MDS+DS) =========="
DS_EXTRA='
[[data_server]]
id      = 1
address = "127.0.0.1"
path    = "/"
'
write_config "combined" "$DS_EXTRA"
start_server "combined" || exit 1
run_cthon04 "pNFS" "vers=4.2,sec=sys"
stop_server

# ---------- Summary ----------

info ""
if [ "$FAILED" -eq 0 ]; then
	info "=== CTHON04 ALL PASSED ==="
else
	info "=== CTHON04 FAILED ==="
	exit 1
fi
