#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# local_ps_test.sh -- End-to-end PS integration test on a single host.
#
# Boots two reffsd processes -- an MDS and a PS -- and verifies the
# proxy-server forward path (LOOKUP + GETATTR) against a live NFSv4.2
# mount.  Intended for developer use on garbo / dreamer / reffs.ci
# or any Linux box with rpcbind + sudo + kernel NFS client.
#
# Topology:
#
#   reffsd(MDS)  :12049 -- serves /<tree built by this script>
#   reffsd(PS)   :12050 native, :14098 proxy
#                     [[proxy_mds]] -> 127.0.0.1:12049
#   client mount :14098 ->  verifies LOOKUP + GETATTR pipeline
#
# Prerequisites:
#   - /reffs_data exists and is writable
#   - rpcbind running (script starts it if missing)
#   - sudo for mount/umount/rpcbind
#   - Linux kernel with NFSv4.2 client
#
# Do NOT run the whole script as root -- rpcbind + mount use targeted
# sudo; `make` and the reffsd process run as the invoking user.
#
# Usage:
#   scripts/local_ps_test.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"
REFFSD="$BUILD_DIR/src/reffsd"

# -- Ports (non-default so the test coexists with a running reffs) --
MDS_PORT=12049
PS_NATIVE_PORT=12050
PS_PROXY_PORT=14098

# -- Paths --
RUN_DIR="/reffs_data/ps_test"
MDS_DATA="$RUN_DIR/mds_data"
MDS_STATE="$RUN_DIR/mds_state"
PS_DATA="$RUN_DIR/ps_data"
PS_STATE="$RUN_DIR/ps_state"
MDS_CONFIG="$RUN_DIR/mds.toml"
PS_CONFIG="$RUN_DIR/ps.toml"
MDS_LOG="$RUN_DIR/mds.log"
PS_LOG="$RUN_DIR/ps.log"
MDS_TRACE="$RUN_DIR/mds-trace.log"
PS_TRACE="$RUN_DIR/ps-trace.log"

MDS_MOUNT="/mnt/reffs_ps_test_mds"
PS_MOUNT="/mnt/reffs_ps_test_ps"

MDS_PID=""
PS_PID=""
FAILED=false

info() { echo "[$(date +%H:%M:%S)] $*"; }
die() {
	echo "PS_TEST FAIL: $*" >&2
	FAILED=true
}

cleanup() {
	set +e
	if mountpoint -q "$PS_MOUNT" 2>/dev/null; then
		sudo umount -f -l "$PS_MOUNT" 2>/dev/null || true
	fi
	if mountpoint -q "$MDS_MOUNT" 2>/dev/null; then
		sudo umount -f -l "$MDS_MOUNT" 2>/dev/null || true
	fi
	if [ -n "$PS_PID" ] && kill -0 "$PS_PID" 2>/dev/null; then
		kill -TERM "$PS_PID" 2>/dev/null || true
		sleep 2
		kill -KILL "$PS_PID" 2>/dev/null || true
	fi
	if [ -n "$MDS_PID" ] && kill -0 "$MDS_PID" 2>/dev/null; then
		kill -TERM "$MDS_PID" 2>/dev/null || true
		sleep 2
		kill -KILL "$MDS_PID" 2>/dev/null || true
	fi
	if [ "$FAILED" = "true" ]; then
		info "--- MDS tail (last 50 lines) ---"
		tail -n 50 "$MDS_LOG" 2>/dev/null || true
		info "--- PS tail (last 50 lines) ---"
		tail -n 50 "$PS_LOG" 2>/dev/null || true
		exit 1
	fi
}
trap cleanup EXIT

# ---------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------
info "Building reffsd..."
cd "$PROJECT_ROOT"
[ -f configure ] || { mkdir -p m4 && autoreconf -fi; }
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
[ -f Makefile ] || "$PROJECT_ROOT/configure" --enable-asan --enable-ubsan
make -j"$(nproc)"
cd "$PROJECT_ROOT"

[ -x "$REFFSD" ] || REFFSD="$BUILD_DIR/src/.libs/reffsd"
[ -x "$REFFSD" ] || { die "Cannot find reffsd binary"; exit 1; }

# ---------------------------------------------------------------------
# rpcbind
# ---------------------------------------------------------------------
if ! pgrep -x rpcbind >/dev/null; then
	info "Starting rpcbind..."
	sudo rpcbind 2>/dev/null || die "rpcbind failed to start"
fi

# ---------------------------------------------------------------------
# Fresh working dirs + configs
# ---------------------------------------------------------------------
rm -rf "$RUN_DIR"
mkdir -p "$MDS_DATA" "$MDS_STATE" "$PS_DATA" "$PS_STATE"
sudo mkdir -p "$MDS_MOUNT" "$PS_MOUNT"

cat >"$MDS_CONFIG" <<EOF
[server]
port           = $MDS_PORT
bind           = "*"
role           = "standalone"
minor_versions = [1, 2]
grace_period   = 5
workers        = 4
trace_file     = "$MDS_TRACE"

[backend]
type       = "posix"
path       = "$MDS_DATA"
state_file = "$MDS_STATE"

[[export]]
path        = "/"
clients     = "*"
access      = "rw"
root_squash = false
flavors     = ["sys"]
EOF

cat >"$PS_CONFIG" <<EOF
[server]
port           = $PS_NATIVE_PORT
bind           = "*"
role           = "standalone"
minor_versions = [1, 2]
grace_period   = 5
workers        = 4
trace_file     = "$PS_TRACE"

[backend]
type       = "posix"
path       = "$PS_DATA"
state_file = "$PS_STATE"

[[export]]
path        = "/"
clients     = "*"
access      = "rw"
root_squash = false
flavors     = ["sys"]

[[proxy_mds]]
id          = 1
port        = $PS_PROXY_PORT
bind        = "*"
address     = "127.0.0.1"
mds_port    = $MDS_PORT
mds_probe   = 20490
EOF

# ---------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------
start_reffsd() {
	local tag=$1 cfg=$2 log=$3 pidvar=$4
	info "Starting $tag (config=$cfg)..."
	ASAN_OPTIONS="detect_leaks=0:halt_on_error=1:handle_abort=1" \
	UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
		"$REFFSD" --config="$cfg" -c 8 >>"$log" 2>&1 &
	local pid=$!
	printf -v "$pidvar" "%s" "$pid"
	for _ in $(seq 1 60); do
		grep -q "reffsd ready:" "$log" 2>/dev/null && return 0
		kill -0 "$pid" 2>/dev/null || {
			die "$tag died during startup"
			return 1
		}
		sleep 1
	done
	die "$tag did not become ready within 60s"
	return 1
}

mount_nfs() {
	local port=$1 mountpoint=$2 label=$3
	info "  mount $label: vers=4.2 port=$port -> $mountpoint"
	sudo timeout 30 mount -v \
		-o vers=4.2,sec=sys,port=$port,timeo=100 \
		127.0.0.1:/ "$mountpoint" 2>&1
}

# ---------------------------------------------------------------------
# Phase 1: MDS up, seed content
# ---------------------------------------------------------------------
start_reffsd "MDS" "$MDS_CONFIG" "$MDS_LOG" MDS_PID
info "MDS up (PID $MDS_PID)"

mount_nfs "$MDS_PORT" "$MDS_MOUNT" "MDS" || { die "MDS mount failed"; exit 1; }

info "Seeding MDS namespace..."
sudo bash -c "echo 'hello from proxied file' > '$MDS_MOUNT/greeting.txt'"
sudo mkdir -p "$MDS_MOUNT/subdir"
sudo bash -c "echo 'nested content' > '$MDS_MOUNT/subdir/nested.txt'"
sudo ls -la "$MDS_MOUNT"

sudo umount -f -l "$MDS_MOUNT"

# ---------------------------------------------------------------------
# Phase 2: PS up, PS-side mount
# ---------------------------------------------------------------------
start_reffsd "PS" "$PS_CONFIG" "$PS_LOG" PS_PID
info "PS up (PID $PS_PID)"

# PS discovery requires MOUNT3 + path traversal against the MDS.
# The discovery trace logs should show the proxy SB being mounted.
sleep 2  # give discovery a moment
grep -E "(discover|proxy SB|mounted proxy)" "$PS_LOG" "$PS_TRACE" \
	2>/dev/null | head -20 || true

mount_nfs "$PS_PROXY_PORT" "$PS_MOUNT" "PS" || { die "PS mount failed"; exit 1; }

# ---------------------------------------------------------------------
# Phase 3: Verify forwarded LOOKUP + GETATTR
# ---------------------------------------------------------------------
info "Listing PS mount (client sees MDS namespace via proxy)..."
if ! sudo ls -la "$PS_MOUNT" 2>&1 | tee /dev/stderr | grep -q greeting.txt; then
	die "greeting.txt not visible via PS mount"
fi

info "stat /greeting.txt via PS..."
sudo stat "$PS_MOUNT/greeting.txt"

info "cat /greeting.txt via PS..."
local_content=$(sudo cat "$PS_MOUNT/greeting.txt")
info "  got: $local_content"
[ "$local_content" = "hello from proxied file" ] ||
	die "greeting.txt content mismatch"

info "Traversing subdir via PS (exercises LOOKUP type-promotion)..."
sudo ls -la "$PS_MOUNT/subdir/" 2>&1 | tee /dev/stderr | grep -q nested.txt ||
	die "nested.txt not visible via PS (type promotion likely broken)"

info "PASS: PS-side LOOKUP + GETATTR forwarding works end-to-end"

# ---------------------------------------------------------------------
# Clean exit -- cleanup handled by trap
# ---------------------------------------------------------------------
exit 0
