#!/bin/sh
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Start MDS + PS on witchie, seed files, leave them running.
# Cleanup is the operator's job via /tmp/witchie_ps_stop.sh.
set -eu

PROJECT_ROOT="$HOME/reffs-main"
REFFSD="$PROJECT_ROOT/build/src/reffsd"

MDS_PORT=12049
PS_NATIVE_PORT=12050
PS_PROXY_PORT=14098
MDS_PROBE_PORT=20490
PS_PROBE_PORT=20491

RUN_DIR="/reffs_data/ps_test"
MDS_MOUNT="/mnt/reffs_ps_test_mds"

LOGHYR_UID=1066

# tear down any prior run
mount | grep -q "$MDS_MOUNT" && sudo umount -f "$MDS_MOUNT" 2>/dev/null || true
pkill -KILL reffsd 2>/dev/null || true
sleep 1

sudo rm -rf "$RUN_DIR"
mkdir -p "$RUN_DIR/mds_data" "$RUN_DIR/mds_state" \
         "$RUN_DIR/ps_data"  "$RUN_DIR/ps_state"
sudo mkdir -p "$MDS_MOUNT"

cat >"$RUN_DIR/mds.toml" <<EOF
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

cat >"$RUN_DIR/ps.toml" <<EOF
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

# start MDS
ASAN_OPTIONS="detect_leaks=0:halt_on_error=0" \
UBSAN_OPTIONS="halt_on_error=0" \
	"$REFFSD" --config="$RUN_DIR/mds.toml" -c 8 \
	>"$RUN_DIR/mds.log" 2>&1 &
MDS_PID=$!
echo "MDS PID=$MDS_PID"
i=0
while ! grep -q "reffsd ready:" "$RUN_DIR/mds.log" 2>/dev/null; do
	i=$((i+1)); [ $i -gt 60 ] && { echo "MDS startup timeout"; exit 1; }
	sleep 1
done

# seed: mount MDS, create files via root + loghyr
sudo mount -t nfs -o nfsv4,minorversion=2,port=$MDS_PORT \
	127.0.0.1:/ "$MDS_MOUNT"
# Restore mode + unmount even if seeding aborts mid-flight; otherwise an
# error during the loghyr-as-uid-1066 create would leave the export
# world-writable on disk.
trap 'sudo chmod 0755 "$MDS_MOUNT" 2>/dev/null || true; \
      sudo umount -f "$MDS_MOUNT" 2>/dev/null || true' EXIT
sudo sh -c "echo 'world content' > $MDS_MOUNT/world.txt"
sudo chmod 644 "$MDS_MOUNT/world.txt"
sudo sh -c "echo 'root secret' > $MDS_MOUNT/root-only.txt"
sudo chmod 600 "$MDS_MOUNT/root-only.txt"
# loghyr creates his own file -- AUTH_SYS uid 1066 in CREATE owns it
sudo chmod 0777 "$MDS_MOUNT"
su loghyr -c "echo 'loghyr secret' > $MDS_MOUNT/loghyr-only.txt"
su loghyr -c "chmod 600 $MDS_MOUNT/loghyr-only.txt"
sudo chmod 0755 "$MDS_MOUNT"
sudo umount -f "$MDS_MOUNT"
trap - EXIT

# start PS
ASAN_OPTIONS="detect_leaks=0:halt_on_error=0" \
UBSAN_OPTIONS="halt_on_error=0" \
	"$REFFSD" --config="$RUN_DIR/ps.toml" -c 8 \
	>"$RUN_DIR/ps.log" 2>&1 &
PS_PID=$!
echo "PS PID=$PS_PID"
i=0
while ! grep -q "reffsd ready:" "$RUN_DIR/ps.log" 2>/dev/null; do
	i=$((i+1)); [ $i -gt 60 ] && { echo "PS startup timeout"; exit 1; }
	sleep 1
done
sleep 2  # discovery

cat >/tmp/witchie_ps_stop.sh <<EOF
#!/bin/sh
mount | grep -q $MDS_MOUNT && sudo umount -f $MDS_MOUNT 2>/dev/null
sudo pkill -TERM reffsd; sleep 2; sudo pkill -KILL reffsd 2>/dev/null
EOF
chmod +x /tmp/witchie_ps_stop.sh

echo "READY  -- MDS:$MDS_PORT  PS native:$PS_NATIVE_PORT  PS proxy:$PS_PROXY_PORT"
echo "Inode meta on disk:"
for f in "$RUN_DIR/mds_data/sb_1/"ino_*.meta; do
	# uid is at offset 8, 4 bytes LE
	printf '  %s  uid=' "$(basename $f)"
	od -An -tu4 -N4 -j8 "$f" | awk '{print $1}'
done
