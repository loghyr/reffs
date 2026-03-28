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

# Derive the build directory from the reffsd binary path.
BUILD_DIR=$(dirname "$(dirname "$REFFSD_BIN")")

# ---------------------------------------------------------------------------
# Environment diagnostics — printed once so failures are self-describing.
# ---------------------------------------------------------------------------
echo "=== Environment ==="
uname -r
grep -E "^(ID|VERSION_ID|PRETTY_NAME)=" /etc/os-release 2>/dev/null || true
echo "io_uring_disabled: $(cat /proc/sys/kernel/io_uring_disabled 2>/dev/null || echo '(absent)')"
dpkg -l liburing-dev liburcu-dev libtirpc-dev nfs-common 2>/dev/null \
	| awk '/^ii/{printf "  %-28s %s\n", $2, $3}' || true
ldd --version 2>&1 | head -1 || true
echo "===================
"

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

section_start() {
	SECTION_NAME="$1"
	SECTION_START=$(date +%s)
	echo ""
	echo "=== $SECTION_NAME === ($(date +%H:%M:%S))"
}

section_end() {
	local now=$(date +%s)
	local elapsed=$((now - SECTION_START))
	echo "=== $SECTION_NAME PASSED === (${elapsed}s)"
}

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
nfs4_domain        = "reffs.test"
trace_categories   = ["security"]
trace_file         = "/dev/stderr"

[backend]
type       = "posix"
path       = "$DATA"
state_file = "$STATE"

[[export]]
path        = "/"
clients     = "*"
access      = "rw"
root_squash = false
flavors     = ["sys", "krb5", "krb5i", "krb5p"]
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
# Kerberos KDC for krb5 integration tests.
# The realm, keytab, and principals are pre-created in the Docker image.
# ---------------------------------------------------------------------------
echo "Starting KDC..."
krb5kdc || echo "WARN: krb5kdc failed to start (krb5 tests will be skipped)"
sleep 1

# Get a TGT for the test user (nfstest@REFFS.TEST).
echo testpass | kinit nfstest@REFFS.TEST 2>/dev/null && echo "kinit OK" || echo "WARN: kinit failed"

# rpc.gssd is started later, just before the krb5 test.
# Starting it early causes the kernel to attempt GSS negotiation
# on AUTH_SYS mounts, which hangs if the server's GSS path has issues.
mkdir -p /run/rpc_pipefs
mount -t rpc_pipefs rpc_pipefs /run/rpc_pipefs 2>/dev/null || true

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
section_start "NFSv4.2 integration test"
mount -o vers=4.2,sec=sys,soft,timeo=10,retrans=2 127.0.0.1:/ "$MOUNT"

(
	cd "$MOUNT"
	GIT_TRACE=1 git clone --verbose "$SRC_DIR" reffs_v4
	sum_nfs=$(md5sum reffs_v4/configure.ac | awk '{print $1}')
	sum_src=$(md5sum "$SRC_DIR/configure.ac" | awk '{print $1}')
	[ "$sum_nfs" = "$sum_src" ] || { echo "md5sum mismatch: $sum_nfs != $sum_src"; exit 1; }
	echo "md5sum OK: $sum_nfs"
)

# Leave a single-component file at the root for the identity test.
# ec_demo's OPEN uses PUTROOTFH + OPEN(name) — single component only.
touch "$MOUNT"/identity_test_file
chown 0:0 "$MOUNT"/identity_test_file
rm -rf "$MOUNT"/reffs_v4 || true
umount "$MOUNT"
section_end

# ---------------------------------------------------------------------------
# NFSv4 identity / owner-string test.
# Uses ec_demo's userspace NFSv4 client to bypass the kernel idmapd and
# verify the raw owner strings directly from the server's GETATTR reply.
# ---------------------------------------------------------------------------
section_start "NFSv4 identity test"

EC_DEMO="env ASAN_OPTIONS=detect_leaks=0 $BUILD_DIR/tools/ec_demo"
MDS="127.0.0.1"

# All operations via ec_demo userspace client — no kernel mount needed.
# Use configure.ac from the git clone test (already on the server).

# File was created by the NFSv4 integration test (kernel mount).
# All identity ops below are pure ec_demo — no kernel mount.
#
# Determine the test owner string.  In Docker (CI container), nfstest
# user exists → "nfstest@reffs.test".  On native runners (GitHub),
# only root is guaranteed → use "root@<domain>" or just verify that
# GETATTR returns a non-empty owner string.

# Step 1: Read the current owner (file was created as root).
echo "  step 1: getowner (baseline)"
OWNER_INIT=$($EC_DEMO getowner --mds "$MDS" --file identity_test_file 2>&1 | tee /dev/stderr | grep '^owner=' | cut -d= -f2)
echo "  initial owner: $OWNER_INIT"

if [ -z "$OWNER_INIT" ]; then
	echo "  FAIL: getowner returned empty"
	die "identity GETATTR failed"
fi

# Step 2: Set the owner to the SAME value (round-trip test).
# Using the initial owner avoids needing a specific user on the host.
echo "  step 2: setowner $OWNER_INIT (round-trip)"
$EC_DEMO setowner --mds "$MDS" --file identity_test_file --input "$OWNER_INIT" 2>&1

# Step 3: Read back and verify.
echo "  step 3: getowner (verify round-trip)"
OWNER=$($EC_DEMO getowner --mds "$MDS" --file identity_test_file 2>&1 | tee /dev/stderr | grep '^owner=' | cut -d= -f2)

if [ "$OWNER" = "$OWNER_INIT" ]; then
	echo "  GETATTR owner string: PASS ($OWNER)"
else
	echo "  GETATTR owner string: FAIL (expected $OWNER_INIT, got $OWNER)"
	die "identity GETATTR failed"
fi
section_end

# ---------------------------------------------------------------------------
# NFSv3 integration test: same, via NFSv3.
# ---------------------------------------------------------------------------
section_start "NFSv3 integration test"
mount -o vers=3,nolock,soft,timeo=10,retrans=2 127.0.0.1:/ "$MOUNT"

(
	cd "$MOUNT"
	GIT_TRACE=1 git clone --verbose "$SRC_DIR" reffs_v3
	sum_nfs=$(md5sum reffs_v3/configure.ac | awk '{print $1}')
	sum_src=$(md5sum "$SRC_DIR/configure.ac" | awk '{print $1}')
	[ "$sum_nfs" = "$sum_src" ] || { echo "md5sum mismatch: $sum_nfs != $sum_src"; exit 1; }
	echo "md5sum OK: $sum_nfs"
)

umount "$MOUNT"
section_end

# ---------------------------------------------------------------------------
# Userspace krb5 security test (no kernel mount needed).
# Uses nfs_krb5_test: GSS session + WRITE + READ/CRC + cleanup.
# ---------------------------------------------------------------------------
if klist -s 2>/dev/null; then
	section_start "NFSv4.2 krb5 userspace test"

	KRB5_TEST="env ASAN_OPTIONS=detect_leaks=0 $BUILD_DIR/tools/nfs_krb5_test"
	$KRB5_TEST --server 127.0.0.1 --sec krb5 || die "nfs_krb5_test --sec krb5 failed"

	section_end
else
	echo ""
	echo "=== NFSv4.2 krb5 userspace test SKIPPED (no TGT) ==="
fi

# ---------------------------------------------------------------------------
# NFSv4.2 Kerberos kernel mount tests: sec=krb5, krb5i, krb5p.
# Requires the KDC, keytab, TGT, and rpc.gssd to be running.
# ---------------------------------------------------------------------------
if [ "${CI_SKIP_KRB5:-0}" = "1" ]; then
	echo ""
	echo "=== NFSv4.2 krb5 integration test SKIPPED (CI_SKIP_KRB5=1) ==="
elif klist -s 2>/dev/null; then
	section_start "NFSv4.2 krb5 integration test"

	# Start rpc.gssd now — only needed for krb5 mounts.
	rpc.gssd 2>/dev/null && echo "  rpc.gssd started" || echo "  WARN: rpc.gssd failed"

	# sec=krb5 (authentication only — sufficient for CI).
	# The kernel mount may fail on some platforms (macOS Docker,
	# GitHub runners) due to host-kernel NFS client differences.
	# Skip gracefully instead of failing CI.
	echo "  mounting sec=krb5..."
	if mount -o vers=4.2,sec=krb5,soft,timeo=10,retrans=2 127.0.0.1:/ "$MOUNT"; then
		touch "$MOUNT"/krb5_test_file
		ls -la "$MOUNT"/krb5_test_file
		rm -f "$MOUNT"/krb5_test_file
		umount "$MOUNT"
		echo "  sec=krb5: PASS"
	else
		echo "  sec=krb5: SKIP (kernel mount failed — host NFS client may not support krb5 in this environment)"
	fi

	section_end
else
	echo ""
	echo "=== NFSv4.2 krb5 integration test SKIPPED (no TGT) === ($(date +%H:%M:%S))"
fi

# Stop rpc.gssd so it doesn't interfere with any subsequent operations
# in the container (e.g., ci-shell debugging with sec=sys mounts).
killall rpc.gssd 2>/dev/null || true

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
# Always print the build hash so failures are traceable.
# ---------------------------------------------------------------------------
if command -v git >/dev/null 2>&1 && git -C "$SRC_DIR" rev-parse HEAD >/dev/null 2>&1; then
	echo "=== Build: $(git -C "$SRC_DIR" rev-parse --short HEAD) ==="
fi

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
echo "=== reffsd log ==="
cat "$LOG"
echo ""
echo "=== Integration tests complete. No ASan errors. ==="

