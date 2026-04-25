#!/bin/sh
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# witchie_ps_creds_test.sh -- FreeBSD validation of slice 2e-iv-c-iii
# (PS forwards end-client AUTH_SYS credentials to upstream MDS).
#
# Sets up:
#   - MDS reffsd on :12049, posix backend at /reffs_data/ps_test/mds_data
#   - PS  reffsd on :12050 native + :14098 proxy, [[proxy_mds]] -> :12049
#
# Seeds the MDS namespace with three files via root mount:
#   /world.txt        mode 0644  (anyone can read)
#   /loghyr-only.txt  owner=1066 mode 0600  (only loghyr)
#   /nobody-only.txt  owner=65534 mode 0600  (only nobody)
#
# Then mounts the PS at :14098 and verifies:
#   1. root can read world.txt (control)
#   2. loghyr (uid 1066) can read loghyr-only.txt        <-- c-iii pass case
#   3. loghyr (uid 1066) is REFUSED on nobody-only.txt   <-- c-iii fail case
#
# (3) is the discriminator for c-iii: pre-c-iii the PS would have
# forwarded its own service identity (root) so loghyr would have
# read nobody-only.txt successfully -- which is wrong.

set -eu

PROJECT_ROOT="$HOME/reffs-main"
BUILD_DIR="$PROJECT_ROOT/build"
REFFSD="$BUILD_DIR/src/reffsd"

MDS_PORT=12049
MDS_PROBE_PORT=20490
PS_NATIVE_PORT=12050
PS_PROXY_PORT=14098
PS_PROBE_PORT=20491

RUN_DIR="/reffs_data/ps_test"
MDS_CONFIG="$RUN_DIR/mds.toml"
PS_CONFIG="$RUN_DIR/ps.toml"
MDS_LOG="$RUN_DIR/mds.log"
PS_LOG="$RUN_DIR/ps.log"

MDS_MOUNT="/mnt/reffs_ps_test_mds"
PS_MOUNT="/mnt/reffs_ps_test_ps"

LOGHYR_UID=1066

MDS_PID=""
PS_PID=""
PASS=0
FAIL=0

info()  { echo "[$(date +%H:%M:%S)] $*"; }
mark_pass() { info "PASS: $1"; PASS=$((PASS + 1)); }
mark_fail() { info "FAIL: $1"; FAIL=$((FAIL + 1)); }

cleanup() {
	set +e
	mount | grep -q "$PS_MOUNT" && sudo umount -f "$PS_MOUNT" 2>/dev/null
	mount | grep -q "$MDS_MOUNT" && sudo umount -f "$MDS_MOUNT" 2>/dev/null
	[ -n "$PS_PID" ] && kill -TERM "$PS_PID" 2>/dev/null && sleep 1
	[ -n "$MDS_PID" ] && kill -TERM "$MDS_PID" 2>/dev/null && sleep 1
	[ -n "$PS_PID" ] && kill -KILL "$PS_PID" 2>/dev/null
	[ -n "$MDS_PID" ] && kill -KILL "$MDS_PID" 2>/dev/null
	echo
	echo "===== RESULT ====="
	echo "PASS: $PASS"
	echo "FAIL: $FAIL"
	if [ "$FAIL" -gt 0 ]; then
		echo "--- MDS tail ---"; tail -n 30 "$MDS_LOG" 2>/dev/null
		echo "--- PS tail ---";  tail -n 30 "$PS_LOG"  2>/dev/null
		exit 1
	fi
	exit 0
}
trap cleanup EXIT INT TERM

# ----- prep ----------------------------------------------------------
info "Preparing $RUN_DIR"
sudo rm -rf "$RUN_DIR"
mkdir -p "$RUN_DIR/mds_data" "$RUN_DIR/mds_state" \
         "$RUN_DIR/ps_data"  "$RUN_DIR/ps_state"
sudo mkdir -p "$MDS_MOUNT" "$PS_MOUNT"

# ----- configs -------------------------------------------------------
cat >"$MDS_CONFIG" <<EOF
[server]
port           = $MDS_PORT
probe_port     = $MDS_PROBE_PORT
bind           = "*"
role           = "standalone"
minor_versions = [1, 2]
grace_period   = 5
workers        = 4

[backend]
type       = "posix"
path       = "$RUN_DIR/mds_data"
state_file = "$RUN_DIR/mds_state"

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
probe_port     = $PS_PROBE_PORT
bind           = "*"
role           = "standalone"
minor_versions = [1, 2]
grace_period   = 5
workers        = 4

[backend]
type       = "posix"
path       = "$RUN_DIR/ps_data"
state_file = "$RUN_DIR/ps_state"

[[export]]
path        = "/"
clients     = "*"
access      = "rw"
root_squash = false
flavors     = ["sys"]

[[proxy_mds]]
id        = 1
port      = $PS_PROXY_PORT
bind      = "*"
address   = "127.0.0.1"
mds_port  = $MDS_PORT
mds_probe = $MDS_PROBE_PORT
EOF

# ----- start MDS -----------------------------------------------------
info "Starting MDS on :$MDS_PORT"
ASAN_OPTIONS="detect_leaks=0:halt_on_error=0" \
UBSAN_OPTIONS="halt_on_error=0" \
	"$REFFSD" --config="$MDS_CONFIG" -c 8 >"$MDS_LOG" 2>&1 &
MDS_PID=$!
i=0
while ! grep -q "reffsd ready:" "$MDS_LOG" 2>/dev/null; do
	i=$((i + 1))
	[ $i -gt 60 ] && { mark_fail "MDS startup timeout"; exit 1; }
	sleep 1
done
info "MDS PID=$MDS_PID up"

# ----- mount MDS as root, seed -----------------------------------------
info "Mounting MDS at $MDS_MOUNT"
sudo mount -t nfs -o nfsv4,minorversion=2,port=$MDS_PORT \
	127.0.0.1:/ "$MDS_MOUNT"
sudo sh -c "echo 'world content' > $MDS_MOUNT/world.txt"
sudo chmod 644 "$MDS_MOUNT/world.txt"
# nobody-only: created by root then chowned to nobody.  Mode 0600.
# Since chown's owner string may not roundtrip through idmap, the
# point of this file is the mode-0600 root-owned content -- if a
# c-iii forwarded uid 1066 hits a 0600 root-owned file it must be
# refused.  Rename to root-only.txt to reflect what actually happens.
sudo sh -c "echo 'root secret' > $MDS_MOUNT/root-only.txt"
sudo chmod 600 "$MDS_MOUNT/root-only.txt"
# loghyr-only: have loghyr CREATE it via the same mount.  The
# AUTH_SYS uid in the OPEN+CREATE compound (1066) sets ownership
# directly -- no idmap involved.
sudo chmod 0777 "$MDS_MOUNT"  # loghyr needs write to root dir to create
su loghyr -c "echo 'loghyr secret' > $MDS_MOUNT/loghyr-only.txt"
su loghyr -c "chmod 600 $MDS_MOUNT/loghyr-only.txt"
sudo chmod 0755 "$MDS_MOUNT"  # restore
sudo ls -la "$MDS_MOUNT"
sudo umount -f "$MDS_MOUNT"

# ----- start PS ------------------------------------------------------
info "Starting PS on :$PS_NATIVE_PORT (proxy :$PS_PROXY_PORT)"
ASAN_OPTIONS="detect_leaks=0:halt_on_error=0" \
UBSAN_OPTIONS="halt_on_error=0" \
	"$REFFSD" --config="$PS_CONFIG" -c 8 >"$PS_LOG" 2>&1 &
PS_PID=$!
i=0
while ! grep -q "reffsd ready:" "$PS_LOG" 2>/dev/null; do
	i=$((i + 1))
	[ $i -gt 60 ] && { mark_fail "PS startup timeout"; exit 1; }
	sleep 1
done
info "PS PID=$PS_PID up"
sleep 3  # let discovery finish

# ----- mount PS, run cred test matrix --------------------------------
info "Mounting PS at $PS_MOUNT"
sudo mount -t nfs -o nfsv4,minorversion=2,port=$PS_PROXY_PORT \
	127.0.0.1:/ "$PS_MOUNT"
sudo ls -la "$PS_MOUNT" || true

# (1) root reads world.txt (sanity)
if sudo cat "$PS_MOUNT/world.txt" 2>/dev/null | grep -q "world content"; then
	mark_pass "(1) root reads world.txt (mode 0644)"
else
	mark_fail "(1) root reads world.txt"
fi

# (2) loghyr reads loghyr-only.txt -- c-iii forwards uid 1066, MDS allows
out2=$(su loghyr -c "cat $PS_MOUNT/loghyr-only.txt" 2>&1 || true)
if echo "$out2" | grep -q "loghyr secret"; then
	mark_pass "(2) loghyr reads loghyr-only.txt (uid 1066 forwarded to MDS)"
else
	mark_fail "(2) loghyr reads loghyr-only.txt -- output: $out2"
fi

# (3) loghyr is REFUSED on root-only.txt -- c-iii forwards uid 1066,
#     MDS sees uid 1066 vs file owner 0, mode 0600 -> EACCES.
#     Pre-c-iii would have succeeded (PS forwarded as service uid 0).
out=$(su loghyr -c "cat $PS_MOUNT/root-only.txt" 2>&1 || true)
if echo "$out" | grep -qiE "permission denied|EACCES"; then
	mark_pass "(3) loghyr is denied root-only.txt (mismatched uid -> EACCES)"
elif echo "$out" | grep -q "root secret"; then
	mark_fail "(3) loghyr READ root-only.txt -- c-iii cred forwarding NOT WORKING"
else
	mark_fail "(3) loghyr unexpected output: $out"
fi

exit 0
