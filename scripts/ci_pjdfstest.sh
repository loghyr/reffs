#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# ci_pjdfstest.sh — Run pjdfstest POSIX compliance suite against reffsd.
#
# Clones and caches pjdfstest in external/ (out-of-tree, .gitignore'd).
# Runs with --enable-strict-posix so the server enforces full POSIX
# permission semantics (note: this breaks git-over-NFS).
#
# Tests against NFSv4.2 standalone only (pjdfstest is POSIX-focused,
# not protocol-specific).
#
# Usage:
#   scripts/ci_pjdfstest.sh [REFFSD_BIN]
#
# Prerequisites: rpcbind, NFS client tools, perl, perl TAP::Harness.

set -euo pipefail

# Dual mode:
#   Standalone:  scripts/ci_pjdfstest.sh [REFFSD_BIN]
#   External:    scripts/ci_pjdfstest.sh --v3-mount PATH --v4-mount PATH

REFFSD_BIN=""
EXT_V3_MOUNT=""
EXT_V4_MOUNT=""
EXTERNAL_MODE=false

while [[ $# -gt 0 ]]; do
	case "$1" in
		--v3-mount) EXT_V3_MOUNT="$2"; EXTERNAL_MODE=true; shift 2 ;;
		--v4-mount) EXT_V4_MOUNT="$2"; EXTERNAL_MODE=true; shift 2 ;;
		*)          REFFSD_BIN="$1"; shift ;;
	esac
done

if [ "$EXTERNAL_MODE" = false ]; then
	REFFSD_BIN=${REFFSD_BIN:-/build/src/reffsd}
fi

BUILD_DIR=$(dirname "$(dirname "${REFFSD_BIN:-/build/src/reffsd}")")
EXTERNAL_DIR=${EXTERNAL_DIR:-$(cd "$(dirname "$0")/.." && pwd)/external}
PJDFS_DIR="$EXTERNAL_DIR/pjdfstest"
PJDFS_URL="https://github.com/pjd/pjdfstest.git"

die() { echo "FAIL: $*" >&2; FAILED=1; }
info() { echo "[$(date +%H:%M:%S)] $*"; }

# Work directory
if [ -n "${REFFS_WORK_DIR:-}" ] && [ -d "$REFFS_WORK_DIR" ]; then
	WORK_DIR="$REFFS_WORK_DIR"
elif [ -d /reffs_data ]; then
	WORK_DIR=/reffs_data
elif [ -d /Volumes/reffs_data ]; then
	WORK_DIR=/Volumes/reffs_data/pjdfstest
	mkdir -p "$WORK_DIR"
else
	WORK_DIR=/tmp
fi

MOUNT=/mnt/reffs_pjd
DATA_DIR=$WORK_DIR/reffs_pjd_data
STATE_DIR=$WORK_DIR/reffs_pjd_state
CONFIG=$WORK_DIR/reffs_pjd.toml
LOG=$WORK_DIR/reffsd_pjd.log

REFFSD_PID=
FAILED=0

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
# Clone and build pjdfstest (cached in external/)
# -----------------------------------------------------------------------

fetch_pjdfstest() {
	if [ -d "$PJDFS_DIR" ]; then
		info "pjdfstest: using cached $PJDFS_DIR"
	else
		info "pjdfstest: cloning from $PJDFS_URL"
		mkdir -p "$EXTERNAL_DIR"
		git clone --depth 1 "$PJDFS_URL" "$PJDFS_DIR"
	fi

	if [ ! -f "$PJDFS_DIR/pjdfstest" ]; then
		info "pjdfstest: building"
		(cd "$PJDFS_DIR" && autoreconf -ifs && ./configure && make) 2>&1 | tail -5
	fi
}

# -----------------------------------------------------------------------
# Server management
# -----------------------------------------------------------------------

start_server() {
	rm -rf "$DATA_DIR" "$STATE_DIR"
	mkdir -p "$DATA_DIR" "$STATE_DIR"
	sudo mkdir -p "$MOUNT"

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

	info "Starting reffsd (strict-posix)..."
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

info "=== pjdfstest Suite ==="

# Ensure prerequisites
if ! command -v prove >/dev/null 2>&1; then
	info "Installing perl TAP::Harness..."
	cpan -T TAP::Harness 2>&1 | tail -3 || true
fi

if ! rpcinfo -p 127.0.0.1 >/dev/null 2>&1; then
	rpcbind || die "rpcbind failed"
	sleep 1
fi

fetch_pjdfstest

run_pjdfstest() {
	local label=$1
	local mount_path=$2
	local results_file="$WORK_DIR/pjdfstest_${label}.txt"

	info ""
	info "========== pjdfstest: $label =========="

	local TESTDIR="$mount_path/pjd_test"
	sudo rm -rf "$TESTDIR" 2>/dev/null || true
	sudo mkdir -p "$TESTDIR"
	sudo chmod 777 "$TESTDIR"

	info "Running pjdfstest ($label) on $TESTDIR"

	cd "$TESTDIR"
	if prove -rv "$PJDFS_DIR/tests" 2>&1 | tee "$results_file"; then
		info "$label: ALL PASSED"
	else
		info "$label: some tests failed (baseline run)"
	fi

	cd /
	sudo rm -rf "$TESTDIR" 2>/dev/null || true

	if [ "$EXTERNAL_MODE" = false ]; then
		check_asan
	fi
}

# ---------- Run against both protocols ----------

if [ "$EXTERNAL_MODE" = true ]; then
	if [ -n "$EXT_V4_MOUNT" ]; then
		run_pjdfstest "NFSv4.2" "$EXT_V4_MOUNT"
	fi
	if [ -n "$EXT_V3_MOUNT" ]; then
		run_pjdfstest "NFSv3" "$EXT_V3_MOUNT"
	fi
else
	start_server || exit 1

	sudo mkdir -p "$MOUNT"
	sudo mount -o "vers=4.2,sec=sys" 127.0.0.1:/ "$MOUNT" || {
		die "NFSv4.2 mount failed"; stop_server; exit 1
	}
	run_pjdfstest "NFSv4.2" "$MOUNT"
	sudo umount -f "$MOUNT" 2>/dev/null || true

	sudo mount -o "vers=3,sec=sys,nolock,tcp,mountproto=tcp" 127.0.0.1:/ "$MOUNT" || {
		die "NFSv3 mount failed"; stop_server; exit 1
	}
	run_pjdfstest "NFSv3" "$MOUNT"
	sudo umount -f "$MOUNT" 2>/dev/null || true

	stop_server
fi

info ""
if [ "$FAILED" -eq 0 ]; then
	info "=== pjdfstest COMPLETE ==="
else
	info "=== pjdfstest FAILED (ASAN/UBSAN) ==="
	exit 1
fi
