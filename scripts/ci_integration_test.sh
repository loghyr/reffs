#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# ci_integration_test.sh -- Start reffsd and run integration tests by cloning
# the reffs source onto an NFS mount via NFSv4.2 and NFSv3, then verifying
# a file checksum.  The clone exercises creates, writes, renames, and
# directory operations at scale; the md5sum confirms read correctness.
#
# Usage: ci_integration_test.sh [REFFSD_BIN [SRC_DIR]]
#   REFFSD_BIN  Path to the reffsd binary  (default: /build/src/reffsd)
#   SRC_DIR     Path to the reffs source with .git (default: /reffs)
#
# ASan/LSan errors from reffsd are captured in $LOG and checked at shutdown.

set -euo pipefail

REFFSD_BIN=${1:-/build/src/reffsd}
SRC_DIR=${2:-/reffs}

MOUNT=/mnt/reffs
DATA=/tmp/reffs_ci_data
STATE=/tmp/reffs_ci_state   # directory; server_persist appends "server_state"
CONFIG=/tmp/reffsd_ci.toml
LOG=/tmp/reffsd_ci.log

REFFSD_PID=
REFFSD_DONE=false

die() { echo "FATAL: $*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Cleanup: unmount if needed, kill reffsd if not already shut down cleanly.
# ---------------------------------------------------------------------------
cleanup() {
	set +e
	umount -f "$MOUNT" 2>/dev/null || true
	if [ -n "$REFFSD_PID" ] && [ "$REFFSD_DONE" != "true" ]; then
		echo "Emergency cleanup: killing reffsd (PID $REFFSD_PID)" >&2
		kill -KILL "$REFFSD_PID" 2>/dev/null || true
		wait "$REFFSD_PID" 2>/dev/null || true
		echo "=== reffsd log ===" >&2
		cat "$LOG" >&2
	fi
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# Setup
# ---------------------------------------------------------------------------
[ -x "$REFFSD_BIN" ] || die "reffsd binary not found or not executable: $REFFSD_BIN"
[ -d "$SRC_DIR/.git" ] || die "SRC_DIR does not look like a git repo: $SRC_DIR"

# Git 2.35.2+ refuses to operate in directories owned by a different user.
# In CI the bind-mounted source is owned by the host user but this script
# runs as root, so mark all directories safe for this container session.
git config --global --add safe.directory '*'

mkdir -p "$DATA" "$MOUNT" "$STATE"

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
path       = "$DATA"
state_file = "$STATE"

[[export]]
path        = "/"
clients     = "*"
access      = "rw"
root_squash = false
flavors     = ["sys"]
EOF

# ---------------------------------------------------------------------------
# rpcbind (needed for NFSv3 mount protocol)
# ---------------------------------------------------------------------------
if ! rpcinfo -p 127.0.0.1 >/dev/null 2>&1; then
	echo "Starting rpcbind..."
	rpcbind || die "rpcbind failed to start"
	sleep 1
fi

# ---------------------------------------------------------------------------
# Start reffsd.  ASan/UBSan options (detect_leaks=0, halt_on_error=0) are
# compiled in via __asan_default_options/__ubsan_default_options when the
# binary is built with --enable-asan/--enable-ubsan.
# ---------------------------------------------------------------------------
echo "Starting reffsd ($REFFSD_BIN)..."
"$REFFSD_BIN" --config="$CONFIG" >"$LOG" 2>&1 &
REFFSD_PID=$!

echo "Waiting for reffsd (PID $REFFSD_PID) to accept connections..."
for i in $(seq 1 30); do
	(echo >/dev/tcp/127.0.0.1/2049) 2>/dev/null && break
	kill -0 "$REFFSD_PID" 2>/dev/null || { echo "reffsd died early"; cat "$LOG"; die "reffsd exited before accepting connections"; }
	sleep 1
	[ $i -lt 30 ] || { echo "startup timeout"; cat "$LOG"; die "reffsd did not start in time"; }
done
echo "reffsd is up."

# ---------------------------------------------------------------------------
# NFSv4.2 integration test: clone the repo onto the NFS mount.
# The clone exercises creates, writes, and directory operations at scale.
# ---------------------------------------------------------------------------
echo ""
echo "=== NFSv4.2 integration test ==="
mount -o vers=4.2,soft,timeo=30,retrans=2 127.0.0.1:/ "$MOUNT"

(
	cd "$MOUNT"
	git clone "$SRC_DIR" reffs_v4
	sum_nfs=$(md5sum reffs_v4/configure.ac | awk '{print $1}')
	sum_src=$(md5sum "$SRC_DIR/configure.ac" | awk '{print $1}')
	[ "$sum_nfs" = "$sum_src" ] || { echo "md5sum mismatch: $sum_nfs != $sum_src"; exit 1; }
	echo "md5sum OK: $sum_nfs"
)

# Best-effort cleanup so reffsd can free inode/fd cache before the v3 test.
rm -rf "$MOUNT"/reffs_v4 || true
umount "$MOUNT"
echo "=== NFSv4.2 integration test PASSED ==="

# ---------------------------------------------------------------------------
# NFSv3 integration test: same, via NFSv3.
# ---------------------------------------------------------------------------
echo ""
echo "=== NFSv3 integration test ==="
mount -o vers=3,nolock,soft,timeo=30,retrans=2 127.0.0.1:/ "$MOUNT"

(
	cd "$MOUNT"
	git clone "$SRC_DIR" reffs_v3
	sum_nfs=$(md5sum reffs_v3/configure.ac | awk '{print $1}')
	sum_src=$(md5sum "$SRC_DIR/configure.ac" | awk '{print $1}')
	[ "$sum_nfs" = "$sum_src" ] || { echo "md5sum mismatch: $sum_nfs != $sum_src"; exit 1; }
	echo "md5sum OK: $sum_nfs"
)

umount "$MOUNT"
echo "=== NFSv3 integration test PASSED ==="

# ---------------------------------------------------------------------------
# Graceful shutdown: SIGTERM, wait up to 30 s for ASan to flush its report.
# ---------------------------------------------------------------------------
echo ""
echo "Shutting down reffsd..."
kill -TERM "$REFFSD_PID"
for i in $(seq 1 30); do
	kill -0 "$REFFSD_PID" 2>/dev/null || break
	sleep 1
done
kill -KILL "$REFFSD_PID" 2>/dev/null || true
wait "$REFFSD_PID" 2>/dev/null || true
REFFSD_DONE=true
REFFSD_PID=

# ---------------------------------------------------------------------------
# Check reffsd log for ASan / LSan errors.
# ---------------------------------------------------------------------------
if grep -qE "ERROR: (AddressSanitizer|LeakSanitizer)" "$LOG"; then
	echo ""
	echo "=== ASan/LSan errors detected in reffsd ==="
	cat "$LOG"
	die "memory errors detected in reffsd"
fi

echo ""
echo "=== Integration tests complete. No ASan errors. ==="
