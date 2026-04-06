#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# bat_client_setup.sh -- Set up a BAT CI client to test against a
# remote reffsd server.
#
# Mounts NFSv4.2 + NFSv3 from the server, then reads Kerberos
# config and TLS CA cert from the server's /config export --
# no SSH or scp to the server needed.
#
# Usage: sudo bat_client_setup.sh SERVER
#   SERVER    IP or hostname of the reffsd server (required)
#
# Prerequisites:
#   - NFS client packages (nfs-utils)
#   - krb5-workstation (for kinit, installed automatically)
#   - sudo / root access
#   - Server has /config populated by bat_setup.sh

set -euo pipefail

SERVER=${1:?Usage: bat_client_setup.sh SERVER}
V4_MOUNT="/mnt/reffs_v4"
V3_MOUNT="/mnt/reffs_v3"

die() { echo "FATAL: $*" >&2; exit 1; }
info() { echo "[client] $*"; }

if [ "$(id -u)" -ne 0 ]; then
	die "Must run as root"
fi

info "=== BAT Client Setup ==="
info "Server: $SERVER"

# -----------------------------------------------------------------------
# Step 1: Create directories
# -----------------------------------------------------------------------

info ""
info "=== Step 1/5: Directories ==="

mkdir -p /reffs_data/ci_remote "$V4_MOUNT" "$V3_MOUNT"
chown loghyr:wheel /reffs_data /reffs_data/ci_remote 2>/dev/null || true
info "Created /reffs_data, $V4_MOUNT, $V3_MOUNT"

# -----------------------------------------------------------------------
# Step 2: Mount NFS
# -----------------------------------------------------------------------

info ""
info "=== Step 2/5: NFS mounts ==="

if mountpoint -q "$V4_MOUNT" 2>/dev/null; then
	info "$V4_MOUNT already mounted"
else
	info "Mounting NFSv4.2 at $V4_MOUNT..."
	mount -o vers=4.2,sec=sys "$SERVER":/ "$V4_MOUNT" || \
		die "NFSv4.2 mount failed (check server firewall: nfs, rpc-bind)"
fi

if mountpoint -q "$V3_MOUNT" 2>/dev/null; then
	info "$V3_MOUNT already mounted"
else
	info "Mounting NFSv3 at $V3_MOUNT..."
	mount -o vers=3,sec=sys,nolock,tcp,mountproto=tcp,acregmin=0,acregmax=0,acdirmin=0,acdirmax=0 "$SERVER":/ "$V3_MOUNT" || \
		die "NFSv3 mount failed"
fi

ls "$V4_MOUNT/config" >/dev/null 2>&1 || \
	die "$V4_MOUNT/config not found -- run bat_setup.sh on the server first"
info "Mounts OK, /config visible"

# -----------------------------------------------------------------------
# Step 3: Kerberos from /config
# -----------------------------------------------------------------------

info ""
info "=== Step 3/5: Kerberos ==="

if ! command -v kinit >/dev/null 2>&1; then
	if command -v dnf >/dev/null 2>&1; then
		info "Installing krb5-workstation..."
		dnf install -y krb5-workstation
	elif command -v apt-get >/dev/null 2>&1; then
		info "Installing krb5-user..."
		apt-get install -y krb5-user
	fi
fi

if [ -f "$V4_MOUNT/config/krb5.conf" ]; then
	cp "$V4_MOUNT/config/krb5.conf" /etc/krb5.conf
	info "Installed /etc/krb5.conf from server"

	# Get test TGT
	echo "testpass" | kinit nfstest@REFFS.BAT 2>/dev/null && \
		info "TGT obtained for nfstest@REFFS.BAT" || \
		info "WARN: kinit failed (check server firewall: kerberos)"
else
	info "SKIP: no krb5.conf on server /config"
fi

# -----------------------------------------------------------------------
# Step 4: TLS CA cert from /config
# -----------------------------------------------------------------------

info ""
info "=== Step 4/5: TLS ==="

mkdir -p /etc/reffs/tls
if [ -f "$V4_MOUNT/config/ca.pem" ]; then
	cp "$V4_MOUNT/config/ca.pem" /etc/reffs/tls/ca.pem
	info "Installed /etc/reffs/tls/ca.pem from server"
fi
if [ -f "$V4_MOUNT/config/client.pem" ]; then
	cp "$V4_MOUNT/config/client.pem" /etc/reffs/tls/client.pem
	cp "$V4_MOUNT/config/client-key.pem" /etc/reffs/tls/client-key.pem
	chmod 600 /etc/reffs/tls/client-key.pem
	info "Installed client cert + key from server"
else
	info "SKIP: no client certs on server /config"
fi

# -----------------------------------------------------------------------
# Step 5: fstab
# -----------------------------------------------------------------------

info ""
info "=== Step 5/5: fstab ==="

for entry in \
	"$SERVER:/ $V4_MOUNT nfs4 vers=4.2,sec=sys,hard,_netdev 0 0" \
	"$SERVER:/ $V3_MOUNT nfs vers=3,sec=sys,nolock,tcp,mountproto=tcp,hard,acregmin=0,acregmax=0,acdirmin=0,acdirmax=0,_netdev 0 0"
do
	mp=$(echo "$entry" | awk '{print $2}')
	if ! grep -q "$mp" /etc/fstab 2>/dev/null; then
		echo "$entry" >> /etc/fstab
		info "Added $mp to /etc/fstab"
	else
		info "$mp already in fstab"
	fi
done

# -----------------------------------------------------------------------
# Summary
# -----------------------------------------------------------------------

info ""
info "========================================"
info "=== BAT Client Setup Complete ==="
info "========================================"
info ""
info "Server:  $SERVER"
info "Mounts:"
info "  NFSv4.2: $V4_MOUNT"
info "  NFSv3:   $V3_MOUNT"
info ""
info "Run CI:"
info "  scripts/ci_remote.sh --server $SERVER"
