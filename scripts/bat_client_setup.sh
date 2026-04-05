#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# bat_client_setup.sh -- Set up a BAT CI client to test against a
# remote reffsd server.
#
# Creates directories, mounts NFSv4.2 + NFSv3, copies Kerberos
# config and TLS CA cert from the server, gets a test TGT.
#
# Usage: sudo bat_client_setup.sh SERVER [HOSTNAME]
#   SERVER    IP or hostname of the reffsd server (required)
#   HOSTNAME  This client's FQDN (default: $(hostname -f))
#
# Prerequisites:
#   - SSH access to SERVER as loghyr (for copying krb5.conf and CA cert)
#   - NFS client packages (nfs-utils)
#   - sudo / root access

set -euo pipefail

SERVER=${1:?Usage: bat_client_setup.sh SERVER [HOSTNAME]}
HOSTNAME=${2:-$(hostname -f)}
V4_MOUNT="/mnt/reffs_v4"
V3_MOUNT="/mnt/reffs_v3"

die() { echo "FATAL: $*" >&2; exit 1; }
info() { echo "[client] $*"; }

if [ "$(id -u)" -ne 0 ]; then
	die "Must run as root"
fi

info "=== BAT Client Setup ==="
info "Server: $SERVER"
info "Client: $HOSTNAME"

# -----------------------------------------------------------------------
# Step 1: Create directories
# -----------------------------------------------------------------------

info ""
info "=== Step 1/5: Directories ==="

mkdir -p /reffs_data/ci_remote "$V4_MOUNT" "$V3_MOUNT"
info "Created /reffs_data, $V4_MOUNT, $V3_MOUNT"

# -----------------------------------------------------------------------
# Step 2: Kerberos client config
# -----------------------------------------------------------------------

info ""
info "=== Step 2/5: Kerberos ==="

# Install krb5-workstation if needed
if ! command -v kinit >/dev/null 2>&1; then
	if command -v dnf >/dev/null 2>&1; then
		info "Installing krb5-workstation..."
		dnf install -y krb5-workstation
	elif command -v apt-get >/dev/null 2>&1; then
		info "Installing krb5-user..."
		apt-get install -y krb5-user
	else
		info "WARN: kinit not found, install Kerberos client manually"
	fi
fi

# Copy krb5.conf from server
info "Copying /etc/krb5.conf from $SERVER..."
if scp "loghyr@$SERVER:/etc/krb5.conf" /etc/krb5.conf 2>/dev/null; then
	info "krb5.conf installed"

	# Add this client's principals to the server's KDC
	info "Creating client principals on server KDC..."
	ssh "loghyr@$SERVER" "sudo kadmin.local -r REFFS.BAT -q 'addprinc -randkey host/$HOSTNAME@REFFS.BAT' 2>/dev/null; \
		sudo kadmin.local -r REFFS.BAT -q 'addprinc -randkey nfs/$HOSTNAME@REFFS.BAT' 2>/dev/null; \
		true" || info "WARN: could not create client principals on server"

	# Extract keytab for this client
	info "Extracting client keytab..."
	ssh "loghyr@$SERVER" "sudo kadmin.local -r REFFS.BAT -q 'ktadd -k /tmp/client_$HOSTNAME.keytab host/$HOSTNAME@REFFS.BAT' 2>/dev/null; \
		sudo kadmin.local -r REFFS.BAT -q 'ktadd -k /tmp/client_$HOSTNAME.keytab nfs/$HOSTNAME@REFFS.BAT' 2>/dev/null; \
		sudo chmod 644 /tmp/client_$HOSTNAME.keytab; \
		true"
	scp "loghyr@$SERVER:/tmp/client_$HOSTNAME.keytab" /etc/krb5.keytab 2>/dev/null && \
		info "Keytab installed at /etc/krb5.keytab" || \
		info "WARN: could not copy keytab"
	ssh "loghyr@$SERVER" "sudo rm -f /tmp/client_$HOSTNAME.keytab" 2>/dev/null || true

	# Get test TGT
	info "Getting test TGT..."
	echo "testpass" | kinit nfstest@REFFS.BAT 2>/dev/null && \
		info "TGT obtained for nfstest@REFFS.BAT" || \
		info "WARN: kinit failed (KDC may not be reachable)"
else
	info "SKIP: could not copy krb5.conf from $SERVER"
fi

# -----------------------------------------------------------------------
# Step 3: TLS CA cert
# -----------------------------------------------------------------------

info ""
info "=== Step 3/5: TLS ==="

mkdir -p /etc/reffs/tls
if scp "loghyr@$SERVER:/etc/reffs/tls/ca.pem" /etc/reffs/tls/ca.pem 2>/dev/null; then
	info "CA cert installed at /etc/reffs/tls/ca.pem"
else
	info "SKIP: could not copy CA cert from $SERVER"
fi

# -----------------------------------------------------------------------
# Step 4: Mount NFS
# -----------------------------------------------------------------------

info ""
info "=== Step 4/5: NFS mounts ==="

if mountpoint -q "$V4_MOUNT" 2>/dev/null; then
	info "$V4_MOUNT already mounted"
else
	info "Mounting NFSv4.2 at $V4_MOUNT..."
	mount -o vers=4.2,sec=sys "$SERVER":/ "$V4_MOUNT" || \
		die "NFSv4.2 mount failed"
fi

if mountpoint -q "$V3_MOUNT" 2>/dev/null; then
	info "$V3_MOUNT already mounted"
else
	info "Mounting NFSv3 at $V3_MOUNT..."
	mount -o vers=3,sec=sys,nolock,tcp,mountproto=tcp "$SERVER":/ "$V3_MOUNT" || \
		die "NFSv3 mount failed"
fi

# Smoke test
ls "$V4_MOUNT" >/dev/null || die "$V4_MOUNT not accessible"
ls "$V3_MOUNT" >/dev/null || die "$V3_MOUNT not accessible"
info "Mounts OK"

# -----------------------------------------------------------------------
# Step 5: fstab
# -----------------------------------------------------------------------

info ""
info "=== Step 5/5: fstab ==="

for entry in \
	"$SERVER:/ $V4_MOUNT nfs4 vers=4.2,sec=sys,hard,_netdev 0 0" \
	"$SERVER:/ $V3_MOUNT nfs vers=3,sec=sys,nolock,tcp,mountproto=tcp,hard,_netdev 0 0"
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
info "Client:  $HOSTNAME"
info "Mounts:"
info "  NFSv4.2: $V4_MOUNT"
info "  NFSv3:   $V3_MOUNT"
info ""
info "Run CI:"
info "  scripts/ci_remote.sh --server $SERVER"
info ""
info "Kerberos:"
klist 2>/dev/null | head -5 || info "  (no TGT)"
info ""
info "TLS CA:"
ls -la /etc/reffs/tls/ca.pem 2>/dev/null || info "  (no CA cert)"
