#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# test_krb5_ffv1_stress_local.sh -- local integration test for
# scripts/krb5_ffv1_stress.sh.
#
# Stands up an embedded mini-KDC and a combined-mode reffsd
# configured for FFv1 + krb5 (one loopback dstore), provisions N
# krb5 principals + a principals file, runs the stress script
# against the reffsd, and asserts exit 0.  Tears everything down on
# the way out.
#
# This is the "developer / pre-push" check for krb5_ffv1_stress.sh;
# QA still runs the script against their real AD-joined server.
#
# Currently EXPECTED RED until #60 (heap-use-after-free in
# ec_disconnect_all when ctx_conns has shallow-copied duplicates) is
# fixed.  The test gets all the way through kinit, krb5 session
# setup, OPEN, and LAYOUTGET on every worker, then ASAN-aborts in
# ec_demo's DS-side cleanup.  That UAF is in the reffs client
# library, not in the script under test -- krb5_ffv1_stress.sh
# itself is exercised correctly and is what QA receives.
#
# Usage: test_krb5_ffv1_stress_local.sh [REFFSD_BIN]
#   REFFSD_BIN  Path to reffsd (default: /build/src/reffsd).
#   EC_DEMO     ec_demo path (default: <build>/tools/ec_demo).

set -euo pipefail

REFFSD=${1:-/build/src/reffsd}
BUILD_DIR=$(dirname "$(dirname "$REFFSD")")
EC_DEMO=${EC_DEMO:-$BUILD_DIR/tools/ec_demo}
STRESS=$(dirname "$0")/krb5_ffv1_stress.sh

REALM=TEST.REFFS
N=4
# Non-default port: ds_io.c's ec_demo client bypasses portmap only when
# the layout encodes a port other than 0/2049.  We have no rpcbind in
# this test environment, so a 2049 setup would ECONNREFUSED on DS I/O.
NFS_PORT=12049

die() {
	echo "FATAL: $*" >&2
	exit 1
}

[ -x "$REFFSD" ] || die "reffsd not executable: $REFFSD"
[ -x "$EC_DEMO" ] || die "ec_demo not executable: $EC_DEMO"
[ -x "$STRESS" ] || die "stress script not executable: $STRESS"

# Make libtool's in-tree .libs/ visible to the inner ELF; see
# krb5_ffv1_stress.sh for the full rationale.
_build_root=$(cd "$(dirname "$EC_DEMO")/.." 2>/dev/null && pwd)
if [ -n "$_build_root" ] && [ -d "$_build_root/lib" ]; then
	_extra=$(find "$_build_root" -type d -name '.libs' 2>/dev/null | paste -sd:)
	[ -n "$_extra" ] && export LD_LIBRARY_PATH="$_extra${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi
command -v krb5kdc >/dev/null 2>&1 || {
	echo "SKIP: krb5kdc not available"; exit 0; }

run_dir=$(mktemp -d /tmp/test_krb5_ffv1.XXXXXX)
keep_dir=0
reffsd_pid=0
kdc_pid=0

cleanup() {
	[ "$reffsd_pid" -gt 0 ] && kill "$reffsd_pid" 2>/dev/null
	[ "$kdc_pid" -gt 0 ] && kill "$kdc_pid" 2>/dev/null
	wait 2>/dev/null || true
	if [ "$keep_dir" -eq 0 ]; then
		rm -rf "$run_dir"
	else
		echo "test_krb5_ffv1: run dir kept: $run_dir" >&2
	fi
	return 0
}
trap cleanup EXIT

# --- 1. KDC ----------------------------------------------------------
kdc_port=$(python3 -c '
import socket
s = socket.socket()
s.bind(("", 0))
print(s.getsockname()[1])
s.close()')

cat >"$run_dir/krb5.conf" <<-EOF
	[libdefaults]
	    default_realm = $REALM
	    dns_lookup_realm = false
	    dns_lookup_kdc = false
	    rdns = false
	[realms]
	    $REALM = {
	        kdc = localhost:$kdc_port
	        admin_server = localhost
	    }
EOF
cat >"$run_dir/kdc.conf" <<-EOF
	[kdcdefaults]
	    kdc_ports = $kdc_port
	    kdc_tcp_ports = $kdc_port
	[realms]
	    $REALM = {
	        database_name = $run_dir/principal
	        key_stash_file = $run_dir/.k5.$REALM
	        kdc_ports = $kdc_port
	        kdc_tcp_ports = $kdc_port
	    }
EOF
export KRB5_CONFIG=$run_dir/krb5.conf
export KRB5_KDC_PROFILE=$run_dir/kdc.conf

kdb5_util create -s -r "$REALM" -P masterpass >/dev/null 2>&1 ||
	die "kdb5_util create failed"
# ec_demo derives the SPN from the --server string verbatim
# (nfs/<host>@<realm>), so register both forms users might pass.
for host in localhost 127.0.0.1; do
	kadmin.local -r "$REALM" addprinc -randkey "nfs/$host@$REALM" \
		>/dev/null 2>&1 || die "addprinc nfs/$host"
	kadmin.local -r "$REALM" ktadd -k "$run_dir/keytab" \
		"nfs/$host@$REALM" >/dev/null 2>&1 || die "ktadd $host"
done

princs=$run_dir/principals.txt
: >"$princs"
for ((i = 0; i < N; i++)); do
	kadmin.local -r "$REALM" addprinc -pw "TestPass$i" \
		"ffv1user$i@$REALM" >/dev/null 2>&1 ||
		die "addprinc ffv1user$i"
	echo "ffv1user$i@$REALM TestPass$i" >>"$princs"
done

krb5kdc -n >/dev/null 2>&1 &
kdc_pid=$!
sleep 1

# --- 2. reffsd: combined + FFv1 + krb5 -------------------------------
mkdir -p "$run_dir/data" "$run_dir/state" "$run_dir/ds"
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
	layout_types = ["ffv1"]
	dstores      = [1]

	    [[export.clients]]
	    match       = "*"
	    access      = "rw"
	    root_squash = false
	    flavors     = ["krb5"]

	[[data_server]]
	id      = 1
	address = "127.0.0.1"
	port    = $NFS_PORT
	path    = "/"
EOF

KRB5_KTNAME=$run_dir/keytab "$REFFSD" \
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
sleep 0.5  # brief grace post-listen, mirrors nfs_krb5_multiclient

# --- 3. run the stress script ----------------------------------------
echo "=== running krb5_ffv1_stress.sh (N=$N, combined reffsd FFv1) ==="
if ! "$STRESS" \
	--server "localhost:$NFS_PORT" \
	--path / \
	--clients "$N" \
	--principals "$princs" \
	--ec-demo "$EC_DEMO" \
	--k 1 --m 0 \
	--codec stripe; then
	keep_dir=1
	echo "----- reffsd.log -----" >&2
	tail -60 "$run_dir/reffsd.log" >&2
	exit 1
fi

echo "PASS: krb5_ffv1_stress.sh against combined reffsd, N=$N"
