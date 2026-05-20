#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# test_mirror_local.sh -- end-to-end functional check for the
# FFV2_ENCODING_MIRRORED encoding type.
#
# Stands up a combined-mode reffsd with N loopback dstores (so the
# MDS allocates N mirrors per layout), then runs ec_demo `write`
# followed by `verify` against it with --codec mirror --k N --m 0.
# Asserts both invocations exit 0 -- i.e., the data fanned out to N
# replicas via CHUNK_WRITE, was readable back via CHUNK_READ from
# any of those replicas, and byte-compared identical to the input.
#
# This is the "developer / pre-push" check for the MIRROR pipeline
# wiring landed alongside FFV2_ENCODING_MIRRORED in the FFv2 draft.
# Wired as the `mirror` target in Makefile.ci; not run by default
# because the existing `check` and `full` targets do not exercise
# the MIRROR code path.
#
# Usage: test_mirror_local.sh [REFFSD_BIN]
#   REFFSD_BIN  Path to reffsd (default: /build/src/reffsd).
#   EC_DEMO     ec_demo path (default: <build>/tools/ec_demo).
#   MIRRORS     N -- number of mirror replicas (default: 3).
#   SIZE        Input bytes (default: 1048576 = 1 MB).

set -euo pipefail

REFFSD=${1:-/build/src/reffsd}
BUILD_DIR=$(dirname "$(dirname "$REFFSD")")
EC_DEMO=${EC_DEMO:-$BUILD_DIR/tools/ec_demo}

MIRRORS=${MIRRORS:-3}
SIZE=${SIZE:-1048576}
# Non-default port: ds_io.c's ec_demo client bypasses portmap only
# when the layout's uaddr encodes a port other than 0/2049.  We
# have no rpcbind in this test environment, so a 2049 setup would
# ECONNREFUSED on DS I/O.
NFS_PORT=12049

die() {
	echo "FATAL: $*" >&2
	exit 1
}

[ -x "$REFFSD" ] || die "reffsd not executable: $REFFSD"
[ -x "$EC_DEMO" ] || die "ec_demo not executable: $EC_DEMO"
[ "$MIRRORS" -ge 2 ] || die "MIRRORS must be >= 2 (got $MIRRORS)"

run_dir=$(mktemp -d /tmp/test_mirror.XXXXXX)
keep_dir=0
reffsd_pid=0

cleanup() {
	[ "$reffsd_pid" -gt 0 ] && kill "$reffsd_pid" 2>/dev/null
	wait 2>/dev/null || true
	if [ "$keep_dir" -eq 0 ]; then
		rm -rf "$run_dir"
	else
		echo "test_mirror: run dir kept: $run_dir" >&2
	fi
	return 0
}
trap cleanup EXIT

# --- 1. reffsd: combined-mode + N loopback dstores -------------------
mkdir -p "$run_dir/data" "$run_dir/state" "$run_dir/ds"

# Header.
cat >"$run_dir/reffsd.toml" <<-EOF
	[server]
	port            = $NFS_PORT
	bind            = "*"
	role            = "combined"
	minor_versions  = [1, 2]
	workers         = 4
	log_level       = "info"

	[backend]
	type            = "posix"
	path            = "$run_dir/data"
	state_file      = "$run_dir/state"
	ds_path         = "$run_dir/ds"

	[[export]]
	path         = "/"
	layout_types = ["ffv2"]
	dstores      = [$(seq -s, 1 "$MIRRORS")]

	    [[export.clients]]
	    match       = "*"
	    access      = "rw"
	    root_squash = false
	    flavors     = ["sys"]

EOF

# N data_server blocks, all pointing at the local reffsd.  Combined
# mode picks dstore_ops_local on a 127.0.0.1 address, so I/O is via
# the local VFS, not a loopback RPC.
for i in $(seq 1 "$MIRRORS"); do
	cat >>"$run_dir/reffsd.toml" <<-EOF

		[[data_server]]
		id      = $i
		address = "127.0.0.1"
		port    = $NFS_PORT
		path    = "/"
	EOF
done

"$REFFSD" \
	--config="$run_dir/reffsd.toml" \
	--file="$run_dir/reffsd.trc" >"$run_dir/reffsd.log" 2>&1 &
reffsd_pid=$!

# Wait for reffsd to listen on the NFS port.
ready=0
for _ in $(seq 1 50); do
	if (echo >/dev/tcp/127.0.0.1/$NFS_PORT) 2>/dev/null; then
		ready=1
		break
	fi
	sleep 0.1
done
if [ "$ready" -ne 1 ]; then
	keep_dir=1
	echo "reffsd did not listen on $NFS_PORT; reffsd.log:" >&2
	tail -40 "$run_dir/reffsd.log" >&2
	exit 1
fi
sleep 0.5  # brief grace post-listen, matches other local tests

# --- 2. input + ec_demo write/verify ---------------------------------
head -c "$SIZE" /dev/urandom >"$run_dir/input.bin" ||
	die "could not generate $SIZE bytes of input"

echo "=== ec_demo write --codec mirror --k $MIRRORS --m 0 ($SIZE bytes) ==="
if ! "$EC_DEMO" write \
	--mds "127.0.0.1:$NFS_PORT" \
	--file "/test_mirror" \
	--input "$run_dir/input.bin" \
	--codec mirror \
	--k "$MIRRORS" \
	--m 0 \
	--layout v2 \
	--id "test_mirror" \
	>"$run_dir/write.log" 2>&1; then
	keep_dir=1
	echo "ec_demo write FAILED; write.log:" >&2
	tail -40 "$run_dir/write.log" >&2
	echo >&2
	echo "----- reffsd.log -----" >&2
	tail -40 "$run_dir/reffsd.log" >&2
	exit 1
fi

echo "=== ec_demo verify --codec mirror --k $MIRRORS --m 0 ==="
if ! "$EC_DEMO" verify \
	--mds "127.0.0.1:$NFS_PORT" \
	--file "/test_mirror" \
	--input "$run_dir/input.bin" \
	--codec mirror \
	--k "$MIRRORS" \
	--m 0 \
	--layout v2 \
	--id "test_mirror" \
	>"$run_dir/verify.log" 2>&1; then
	keep_dir=1
	echo "ec_demo verify FAILED; verify.log:" >&2
	tail -40 "$run_dir/verify.log" >&2
	echo >&2
	echo "----- reffsd.log -----" >&2
	tail -40 "$run_dir/reffsd.log" >&2
	exit 1
fi

# --- 3. degraded read: drop one replica via --skip-ds --------------
#
# Confirm that the mirror codec recovers when one replica is
# unavailable.  With N mirrors, MIRRORED tolerates up to N-1 losses;
# this test drops one and asserts the read still verifies.
echo "=== ec_demo verify --skip-ds 0 (one mirror dropped) ==="
if ! "$EC_DEMO" verify \
	--mds "127.0.0.1:$NFS_PORT" \
	--file "/test_mirror" \
	--input "$run_dir/input.bin" \
	--codec mirror \
	--k "$MIRRORS" \
	--m 0 \
	--layout v2 \
	--skip-ds 0 \
	--id "test_mirror" \
	>"$run_dir/verify_degraded.log" 2>&1; then
	keep_dir=1
	echo "ec_demo verify (degraded) FAILED; verify_degraded.log:" >&2
	tail -40 "$run_dir/verify_degraded.log" >&2
	echo >&2
	echo "----- reffsd.log -----" >&2
	tail -40 "$run_dir/reffsd.log" >&2
	exit 1
fi

echo "PASS: ec_demo write+verify+degraded-verify against combined reffsd, N=$MIRRORS, codec=mirror"
