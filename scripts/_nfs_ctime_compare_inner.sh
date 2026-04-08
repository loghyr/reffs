#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# _nfs_ctime_compare_inner.sh -- Runs INSIDE the reffs-ci container.
# Do not call directly; use nfs_ctime_compare.sh.
#
# Installs nfs-kernel-server (ephemeral), builds reffsd, starts both
# servers, mounts each, then runs ctime/nlink comparison tests.

set -euo pipefail

REFFS_PORT=3049
KNFSD_PORT=2049
REFFS_MOUNT=/mnt/reffs
KNFSD_MOUNT=/mnt/knfsd
KNFSD_EXPORT=/srv/knfsd

# ------------------------------------------------------------------ helpers --

ok=0
fail=0

result() {
	local tag="$1" desc="$2"
	if [ "$tag" = "PASS" ]; then
		printf "  %-6s %s\n" "PASS" "$desc"
		ok=$((ok + 1))
	else
		printf "  %-6s %s\n" "FAIL" "$desc"
		fail=$((fail + 1))
	fi
}

# ctime_of: return ctime as seconds.nanoseconds (or just seconds if %N absent)
ctime_of() {
	stat -c '%Z' "$1" 2>/dev/null
}

# nlink_of: return hard link count
nlink_of() {
	stat -c '%h' "$1" 2>/dev/null
}

# -------------------------------------------------------- install knfsd ------

echo "=== Installing nfs-kernel-server (ephemeral) ==="
apt-get update -qq
apt-get install -y -q nfs-kernel-server

# -------------------------------------------------------- build reffsd -------

echo ""
echo "=== Building reffsd ==="
mkdir -p /src /build
# tar avoids rsync receiver getcwd() failures on macOS Docker Desktop overlay mounts
tar -C /reffs --exclude='.git' --exclude='./build' --exclude='./logs' \
	-cf - . | tar -C /src -xf -
cd /src
find . -name parsetab.py -delete
autoreconf -fi >/dev/null 2>&1
cd /build
../src/configure >/dev/null 2>&1
make -j"$(nproc)" 2>&1 | tail -3

# -------------------------------------------------------- rpcbind -------------

echo ""
echo "=== Starting rpcbind ==="
if ! rpcinfo -p 127.0.0.1 >/dev/null 2>&1; then
	rpcbind || { echo "rpcbind failed to start"; exit 1; }
	sleep 1
fi
echo "  rpcbind ready"

# -------------------------------------------------------- start reffsd -------

echo ""
echo "=== Starting reffsd on port $REFFS_PORT ==="
mkdir -p /tmp/reffs_data /tmp/reffs_state

cat > /tmp/reffsd.toml <<EOF
[server]
port           = $REFFS_PORT
bind           = "*"
role           = "standalone"
minor_versions = [1, 2]
grace_period   = 5
workers        = 4
nfs4_domain    = "reffs.test"

[backend]
type       = "posix"
path       = "/tmp/reffs_data"
state_file = "/tmp/reffs_state"

[[export]]
path        = "/"
clients     = "*"
access      = "rw"
root_squash = false
flavors     = ["sys"]
EOF

/build/src/reffsd --config=/tmp/reffsd.toml >/tmp/reffsd.log 2>&1 &
REFFSD_PID=$!

# Give it a moment to fail fast or start up
sleep 2
if ! kill -0 "$REFFSD_PID" 2>/dev/null; then
	wait "$REFFSD_PID"; RC=$?
	echo "  reffsd exited immediately (exit code $RC)"
	echo "reffsd log:"
	cat /tmp/reffsd.log
	echo "trace log (if any):"
	cat /build/reffsd.log 2>/dev/null || true
	cat /tmp/reffsd_trace.log 2>/dev/null || true
	exit 1
fi

echo -n "  waiting for reffsd"
for i in $(seq 1 28); do
	sleep 1
	echo -n "."
	if (echo > /dev/tcp/127.0.0.1/"$REFFS_PORT") 2>/dev/null; then
		echo " ready"
		break
	fi
	if ! kill -0 "$REFFSD_PID" 2>/dev/null; then
		echo " died"
		wait "$REFFSD_PID"; RC=$?
		echo "reffsd log:"
		cat /tmp/reffsd.log
		echo "trace log (if any):"
		cat /build/reffsd.log 2>/dev/null || true
		exit 1
	fi
	if [ "$i" -eq 28 ]; then
		echo " TIMEOUT"
		echo "reffsd log:"
		cat /tmp/reffsd.log
		exit 1
	fi
done

mkdir -p "$REFFS_MOUNT"
mount -t nfs4 \
	-o vers=4.2,sec=sys,soft,timeo=10,retrans=2,port="$REFFS_PORT" \
	127.0.0.1:/ "$REFFS_MOUNT"
echo "  mounted at $REFFS_MOUNT"

# -------------------------------------------------------- start knfsd --------

echo ""
echo "=== Starting knfsd on port $KNFSD_PORT ==="

# Export a tmpfs so the test directory is always clean and writable
mkdir -p "$KNFSD_EXPORT"
mount -t tmpfs tmpfs "$KNFSD_EXPORT"

# Load the kernel NFS server module (goes into the host kernel -- privileged)
modprobe nfsd 2>/dev/null || true

# rpcbind is already in the image; start it if not running
rpcbind 2>/dev/null || true
sleep 1

# NFSv4 pseudo-root requires fsid=0
cat > /etc/exports << EOF
$KNFSD_EXPORT 127.0.0.1(rw,no_root_squash,fsid=0,no_subtree_check,insecure,sync)
EOF

exportfs -rav 2>/dev/null
rpc.nfsd 8
sleep 1
rpc.mountd --no-udp 2>/dev/null || rpc.mountd 2>/dev/null || true

echo -n "  waiting for knfsd"
for i in $(seq 1 15); do
	sleep 1
	echo -n "."
	if rpcinfo -p 127.0.0.1 2>/dev/null | grep -q "100003"; then
		echo " ready"
		break
	fi
	if [ "$i" -eq 15 ]; then
		echo " TIMEOUT"
		rpcinfo -p 127.0.0.1 2>/dev/null || true
		exit 1
	fi
done

mkdir -p "$KNFSD_MOUNT"
mount -t nfs4 \
	-o vers=4.2,sec=sys,soft,timeo=10,retrans=2 \
	127.0.0.1:/ "$KNFSD_MOUNT"
echo "  mounted at $KNFSD_MOUNT"

# -------------------------------------------------------- run tests ----------

run_tests() {
	local label="$1" mnt="$2"
	echo ""
	echo "=== $label ==="

	local dir="$mnt/ctime_nlink_test"
	rm -rf "$dir"
	mkdir -p "$dir"

	# -- ctime after link (with 1s pause to guarantee clock tick) --
	touch "$dir/a"
	local c1
	c1=$(ctime_of "$dir/a")
	sleep 1
	ln "$dir/a" "$dir/b"
	local c2
	c2=$(ctime_of "$dir/a")
	[ "$c1" != "$c2" ] \
		&& result PASS "ctime updated after link (1s pause)" \
		|| result FAIL "ctime updated after link (1s pause) [before=$c1 after=$c2]"

	# -- ctime after link (no pause -- tests sub-second visibility) --
	touch "$dir/x"
	local cx1
	cx1=$(ctime_of "$dir/x")
	ln "$dir/x" "$dir/x2"
	local cx2
	cx2=$(ctime_of "$dir/x")
	[ "$cx1" != "$cx2" ] \
		&& result PASS "ctime updated after link (no pause)" \
		|| result FAIL "ctime updated after link (no pause) [before=$cx1 after=$cx2]"
	rm "$dir/x" "$dir/x2"

	# -- nlink after link --
	local n
	n=$(nlink_of "$dir/a")
	[ "$n" = "2" ] \
		&& result PASS "nlink=2 after link" \
		|| result FAIL "nlink=2 after link (got $n)"

	# -- ctime after unlink (surviving link) --
	local c3
	c3=$(ctime_of "$dir/a")
	sleep 1
	rm "$dir/b"
	local c4
	c4=$(ctime_of "$dir/a")
	[ "$c3" != "$c4" ] \
		&& result PASS "ctime updated after unlink of hard link" \
		|| result FAIL "ctime updated after unlink of hard link [before=$c3 after=$c4]"

	# -- nlink after unlink --
	n=$(nlink_of "$dir/a")
	[ "$n" = "1" ] \
		&& result PASS "nlink=1 after unlink" \
		|| result FAIL "nlink=1 after unlink (got $n)"

	# -- ctime after rename --
	local c5
	c5=$(ctime_of "$dir/a")
	sleep 1
	mv "$dir/a" "$dir/c"
	local c6
	c6=$(ctime_of "$dir/c")
	[ "$c5" != "$c6" ] \
		&& result PASS "ctime updated after rename" \
		|| result FAIL "ctime updated after rename [before=$c5 after=$c6]"
	rm "$dir/c"

	# -- nlink chain: 3 -> 2 -> 1 across multiple unlinks --
	touch "$dir/f"
	ln "$dir/f" "$dir/g"
	ln "$dir/f" "$dir/h"
	local n1 n2 n3
	n1=$(nlink_of "$dir/f")
	rm "$dir/h"
	n2=$(nlink_of "$dir/f")
	rm "$dir/g"
	n3=$(nlink_of "$dir/f")
	rm "$dir/f"
	[[ "$n1" = "3" && "$n2" = "2" && "$n3" = "1" ]] \
		&& result PASS "nlink chain 3->2->1 after sequential unlinks" \
		|| result FAIL "nlink chain 3->2->1 (got $n1->$n2->$n3)"

	rm -rf "$dir"
}

REFFS_OK=0 REFFS_FAIL=0 KNFSD_OK=0 KNFSD_FAIL=0

ok=0; fail=0
run_tests "reffs (port $REFFS_PORT)" "$REFFS_MOUNT"
REFFS_OK=$ok; REFFS_FAIL=$fail

ok=0; fail=0
run_tests "knfsd (port $KNFSD_PORT)" "$KNFSD_MOUNT"
KNFSD_OK=$ok; KNFSD_FAIL=$fail

# -------------------------------------------------------- summary ------------

echo ""
echo "=== Summary ==="
printf "  reffs:  %d passed, %d failed\n" "$REFFS_OK"  "$REFFS_FAIL"
printf "  knfsd:  %d passed, %d failed\n" "$KNFSD_OK"  "$KNFSD_FAIL"

# -------------------------------------------------------- cleanup ------------

umount "$REFFS_MOUNT" 2>/dev/null || true
kill "$REFFSD_PID" 2>/dev/null || true
umount "$KNFSD_MOUNT" 2>/dev/null || true
exportfs -uav 2>/dev/null || true
umount "$KNFSD_EXPORT" 2>/dev/null || true

# Exit non-zero only if reffs failed something knfsd passed --
# that isolates server bugs from client/kernel issues.
if [ "$REFFS_FAIL" -gt 0 ] && [ "$KNFSD_FAIL" -eq 0 ]; then
	echo "  reffs-specific failures detected"
	exit 1
fi
exit 0
