#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# bat_setup.sh -- One-time setup for BAT (Build Acceptance Test) server.
#
# Configures Kerberos, TLS, starts reffsd as a systemd service, creates
# per-flavor exports, and sets up persistent NFS mounts.
#
# Usage: sudo bat_setup.sh [HOSTNAME]
#   HOSTNAME  FQDN for certs and principals (default: $(hostname -f))
#
# Prerequisites:
#   - reffs RPM installed (reffsd, reffs-probe.py, bat_*_setup.sh)
#   - /etc/reffs/reffsd.toml present (from RPM)
#   - sudo / root access

set -euo pipefail

HOSTNAME=${1:-$(hostname -f)}
CONFIG="/etc/reffs/reffsd.toml"
V4_MOUNT="/mnt/reffs_v4"
V3_MOUNT="/mnt/reffs_v3"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

die() { echo "FATAL: $*" >&2; exit 1; }
info() { echo "[bat] $*"; }

# -----------------------------------------------------------------------
# Preflight checks
# -----------------------------------------------------------------------

info "=== BAT Setup for $HOSTNAME ==="

if [ "$(id -u)" -ne 0 ]; then
	die "Must run as root"
fi

if ! command -v reffsd >/dev/null 2>&1; then
	# Try the build directory if RPM isn't installed
	if [ -x "$SCRIPT_DIR/../build/src/reffsd" ]; then
		info "Using reffsd from build directory"
	else
		die "reffsd not found -- install the RPM or build first"
	fi
fi

if [ ! -f "$CONFIG" ]; then
	info "Config not found at $CONFIG, installing from examples..."
	mkdir -p /etc/reffs
	if [ -f "$SCRIPT_DIR/../examples/reffsd-bat.toml" ]; then
		cp "$SCRIPT_DIR/../examples/reffsd-bat.toml" "$CONFIG"
	else
		die "No config file found"
	fi
fi

# -----------------------------------------------------------------------
# Step 1: Kerberos setup
# -----------------------------------------------------------------------

info ""
info "=== Step 1/6: Kerberos ==="

if [ -x "$SCRIPT_DIR/bat_krb5_setup.sh" ]; then
	"$SCRIPT_DIR/bat_krb5_setup.sh" "$HOSTNAME"
elif command -v bat_krb5_setup.sh >/dev/null 2>&1; then
	bat_krb5_setup.sh "$HOSTNAME"
else
	info "SKIP: bat_krb5_setup.sh not found (Kerberos not configured)"
fi

# -----------------------------------------------------------------------
# Step 2: TLS setup
# -----------------------------------------------------------------------

info ""
info "=== Step 2/6: TLS ==="

if [ -x "$SCRIPT_DIR/bat_tls_setup.sh" ]; then
	"$SCRIPT_DIR/bat_tls_setup.sh" "$HOSTNAME"
elif command -v bat_tls_setup.sh >/dev/null 2>&1; then
	bat_tls_setup.sh "$HOSTNAME"
else
	info "SKIP: bat_tls_setup.sh not found (TLS not configured)"
fi

# -----------------------------------------------------------------------
# Step 3: Create directories and start rpcbind
# -----------------------------------------------------------------------

info ""
info "=== Step 3/6: Prerequisites ==="

mkdir -p /var/lib/reffs/data /var/lib/reffs/state /var/log/reffs

if ! rpcinfo -p 127.0.0.1 >/dev/null 2>&1; then
	info "Starting rpcbind..."
	systemctl enable --now rpcbind 2>/dev/null || rpcbind
	sleep 1
fi
info "rpcbind OK"

# -----------------------------------------------------------------------
# Step 4: Start reffsd
# -----------------------------------------------------------------------

info ""
info "=== Step 4/6: Start reffsd ==="

if systemctl is-active reffsd >/dev/null 2>&1; then
	info "reffsd already running"
	systemctl status reffsd --no-pager || true
else
	info "Enabling and starting reffsd..."
	systemctl enable reffsd 2>/dev/null || true
	systemctl start reffsd
fi

# Wait for NFS port
info "Waiting for port 2049..."
for i in $(seq 1 30); do
	(echo >/dev/tcp/127.0.0.1/2049) 2>/dev/null && break
	sleep 1
done
(echo >/dev/tcp/127.0.0.1/2049) 2>/dev/null || \
	die "reffsd not listening on port 2049 after 30s"
info "reffsd listening on port 2049"

# -----------------------------------------------------------------------
# Step 5: Create exports
# -----------------------------------------------------------------------

info ""
info "=== Step 5/6: Create exports ==="

if [ -x "$SCRIPT_DIR/bat_export_setup.sh" ]; then
	"$SCRIPT_DIR/bat_export_setup.sh"
elif command -v bat_export_setup.sh >/dev/null 2>&1; then
	bat_export_setup.sh
else
	info "SKIP: bat_export_setup.sh not found"
fi

# -----------------------------------------------------------------------
# Step 6: Mount persistent test points
# -----------------------------------------------------------------------

info ""
info "=== Step 6/6: Persistent mounts ==="

mkdir -p "$V4_MOUNT" "$V3_MOUNT"

if mountpoint -q "$V4_MOUNT" 2>/dev/null; then
	info "$V4_MOUNT already mounted"
else
	info "Mounting NFSv4.2 at $V4_MOUNT..."
	mount -o vers=4.2,sec=sys 127.0.0.1:/ "$V4_MOUNT"
fi

if mountpoint -q "$V3_MOUNT" 2>/dev/null; then
	info "$V3_MOUNT already mounted"
else
	info "Mounting NFSv3 at $V3_MOUNT..."
	mount -o vers=3,sec=sys,nolock,tcp,mountproto=tcp 127.0.0.1:/ "$V3_MOUNT"
fi

# Add to fstab if not already present
for entry in \
	"127.0.0.1:/ $V4_MOUNT nfs4 vers=4.2,sec=sys,hard,_netdev 0 0" \
	"127.0.0.1:/ $V3_MOUNT nfs vers=3,sec=sys,nolock,tcp,mountproto=tcp,hard,_netdev 0 0"
do
	mount_point=$(echo "$entry" | awk '{print $2}')
	if ! grep -q "$mount_point" /etc/fstab 2>/dev/null; then
		echo "$entry" >> /etc/fstab
		info "Added $mount_point to /etc/fstab"
	fi
done

# -----------------------------------------------------------------------
# Summary
# -----------------------------------------------------------------------

info ""
info "========================================"
info "=== BAT Setup Complete ==="
info "========================================"
info ""
info "Server:  reffsd (systemd service)"
info "Config:  $CONFIG"
info "Host:    $HOSTNAME"
info "Mounts:"
info "  NFSv4.2: $V4_MOUNT"
info "  NFSv3:   $V3_MOUNT"
info ""
info "Exports:"
reffs-probe.py sb-list 2>/dev/null || \
	info "  (reffs-probe.py not available, use reffs_probe1_clnt)"
info ""
info "Kerberos:"
klist -kt /etc/krb5.keytab 2>/dev/null | head -10 || \
	info "  (no keytab found)"
info ""
info "TLS:"
ls -la /etc/reffs/tls/*.pem 2>/dev/null || \
	info "  (no certs found)"
info ""
info "Run CI against this server:"
info "  scripts/ci_remote.sh --v4-mount $V4_MOUNT --v3-mount $V3_MOUNT"
