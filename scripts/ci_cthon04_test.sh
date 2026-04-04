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

# Work directory — see ci_soak_test.sh for rationale.
if [ -n "${REFFS_WORK_DIR:-}" ] && [ -d "$REFFS_WORK_DIR" ]; then
	WORK_DIR="$REFFS_WORK_DIR"
elif [ -d /reffs_data ]; then
	WORK_DIR=/reffs_data
elif [ -d /Volumes/reffs_data ]; then
	WORK_DIR=/Volumes/reffs_data/cthon
	mkdir -p "$WORK_DIR"
else
	WORK_DIR=/tmp
fi

MOUNT=/mnt/reffs_cthon
DATA_DIR=$WORK_DIR/reffs_cthon_data
STATE_DIR=$WORK_DIR/reffs_cthon_state
DS_DIR=$WORK_DIR/reffs_cthon_ds
CONFIG=$WORK_DIR/reffs_cthon.toml
LOG=$WORK_DIR/reffsd_cthon.log

REFFSD_PID=
FAILED=0

# Per-mode result tracking for final summary.
# Each entry: "mode:basic:general:special:lock"
RESULTS=()

die() { echo "FAIL: $*" >&2; FAILED=1; }
info() { echo "[$(date +%H:%M:%S)] $*"; }

cleanup() {
	set +e
	sudo umount -f "$MOUNT" 2>/dev/null
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

	if [ ! -f "$CTHON_DIR/basic/test1" ]; then
		info "cthon04: building"
		make -C "$CTHON_DIR" -j"$(nproc)" 2>&1 | tail -5
	fi

	# Fix tests.init for dash compatibility:
	# - CFLAGS+= is bash-only; use CFLAGS="$CFLAGS ..."
	if grep -q 'CFLAGS+=' "$CTHON_DIR/tests.init" 2>/dev/null; then
		sed -i 's/CFLAGS+=\(.*\)/CFLAGS="$CFLAGS \1"/' "$CTHON_DIR/tests.init"
	fi

	# cthon04 general/special tests use 'time' command which isn't
	# available in minimal containers.  Install if missing.
	if ! command -v time >/dev/null 2>&1; then
		apt-get update -qq && apt-get install -y -qq time 2>/dev/null || true
	fi
}

# -----------------------------------------------------------------------
# Server management
# -----------------------------------------------------------------------

write_config() {
	local role=$1
	local extra=${2:-}

	rm -rf "$DATA_DIR" "$STATE_DIR" "$DS_DIR"
	mkdir -p "$DATA_DIR" "$STATE_DIR" "$DS_DIR"
	sudo mkdir -p "$MOUNT"

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

	sudo umount -f "$MOUNT" 2>/dev/null || true
	sudo mkdir -p "$MOUNT"
	sudo mount -o "$mount_opts" 127.0.0.1:/ "$MOUNT" || {
		die "$label: mount failed"
		return 1
	}

	# cthon04 runtests tries to rm/mkdir its test directory.
	# Give it a subdirectory of the mount, not the mount point itself.
	local TESTDIR="$MOUNT/cthon_test"
	sudo rm -rf "$TESTDIR" 2>/dev/null || true
	sudo mkdir -p "$TESTDIR"
	sudo chmod 777 "$TESTDIR"

	info "Running cthon04 ($test_flags) on $TESTDIR"

	local pass=0
	local fail=0
	local r_basic="-" r_general="-" r_special="-" r_lock="-"

	# Run each test set separately for better reporting.
	# runtests usage: ./runtests <tests> <testargs> <testpath>
	#   tests: -b=basic, -g=general, -s=special, -l=lock, -a=all
	#   testargs: -f=functional, -t=timing
	for test_set in basic general special lock; do
		local flag="-${test_set:0:1}" # -b, -g, -s, -l
		if [ -d "$CTHON_DIR/$test_set" ]; then
			info "  $test_set..."
			if (cd "$CTHON_DIR" && bash ./runtests "$flag" -f "$TESTDIR" 2>&1); then
				info "  $test_set: PASS"
				pass=$((pass + 1))
				eval "r_${test_set}=PASS"
			else
				info "  $test_set: FAIL"
				fail=$((fail + 1))
				eval "r_${test_set}=FAIL"
			fi
		fi
	done

	RESULTS+=("${label}:${r_basic}:${r_general}:${r_special}:${r_lock}")

	rm -rf "$TESTDIR" 2>/dev/null || true
	sudo umount -f "$MOUNT" 2>/dev/null || true

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
# actimeo=0: disable attribute caching so stat() after rename
# goes to the server immediately.  Without this, the Linux NFS v4
# client may return a cached positive lookup for the old name.
run_cthon04 "NFSv4.2" "vers=4.2,sec=sys,actimeo=0"
stop_server

# ---------- Test 2: NFSv3 standalone ----------

info ""
info "========== NFSv3 Standalone =========="
write_config "standalone"
start_server "standalone" || exit 1
# nolock: NLM sideband protocol clashes with the host's rpcbind
# inside Docker.  Lock tests use NFSv4.2 (above) instead.
run_cthon04 "NFSv3" "vers=3,sec=sys,nolock,tcp,mountproto=tcp"
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
run_cthon04 "pNFS" "vers=4.2,sec=sys,actimeo=0"
stop_server

# ---------- Summary ----------

info ""
info "=== CTHON04 Summary ==="
info ""
printf "  %-12s  %-6s  %-8s  %-8s  %-6s\n" "Mode" "basic" "general" "special" "lock"
printf "  %-12s  %-6s  %-8s  %-8s  %-6s\n" "----" "-----" "-------" "-------" "----"
for entry in "${RESULTS[@]}"; do
	IFS=: read -r mode basic general special lock <<< "$entry"
	printf "  %-12s  %-6s  %-8s  %-8s  %-6s\n" "$mode" "$basic" "$general" "$special" "$lock"
done
info ""

if [ "$FAILED" -eq 0 ]; then
	info "=== CTHON04 ALL PASSED ==="
else
	info "=== CTHON04 FAILED ==="
	exit 1
fi
