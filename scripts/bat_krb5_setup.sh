#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# bat_krb5_setup.sh -- Set up a Kerberos realm for BAT testing.
#
# Creates realm REFFS.BAT with host and nfs service principals for
# the local machine.  Based on Ben Coddington's setup instructions.
#
# Usage: sudo bat_krb5_setup.sh [HOSTNAME]
#   HOSTNAME  The FQDN for service principals (default: $(hostname -f))
#
# Prerequisites: krb5-server, krb5-workstation (Fedora/RHEL)

set -euo pipefail

REALM="REFFS.BAT"
HOSTNAME=${1:-$(hostname -f)}
KDC_MASTER_PW="reffs-bat-master"
TEST_USER_PW="testpass"
KEYTAB="/etc/krb5.keytab"

die() { echo "FATAL: $*" >&2; exit 1; }
info() { echo "[krb5] $*"; }

# -----------------------------------------------------------------------
# Install packages if needed
# -----------------------------------------------------------------------
if command -v dnf >/dev/null 2>&1; then
	rpm -q krb5-server >/dev/null 2>&1 || {
		info "Installing krb5-server krb5-workstation..."
		dnf install -y krb5-server krb5-workstation
	}
elif command -v apt-get >/dev/null 2>&1; then
	dpkg -l krb5-kdc >/dev/null 2>&1 || {
		info "Installing krb5-kdc krb5-admin-server krb5-user..."
		apt-get install -y krb5-kdc krb5-admin-server krb5-user
	}
fi

# -----------------------------------------------------------------------
# Configure krb5.conf
# -----------------------------------------------------------------------
info "Configuring /etc/krb5.conf for realm $REALM"
cat >/etc/krb5.conf <<EOF
[libdefaults]
    default_realm = $REALM
    dns_lookup_realm = false
    dns_lookup_kdc = false
    rdns = false

[realms]
    $REALM = {
        kdc = localhost
        admin_server = localhost
    }

[domain_realm]
    .reffs.bat = $REALM
    reffs.bat = $REALM
EOF

# -----------------------------------------------------------------------
# Configure KDC
# -----------------------------------------------------------------------
KDC_CONF=""
if [ -d /var/kerberos/krb5kdc ]; then
	# Fedora/RHEL layout
	KDC_CONF="/var/kerberos/krb5kdc/kdc.conf"
	KDC_DIR="/var/kerberos/krb5kdc"
elif [ -d /etc/krb5kdc ]; then
	# Debian/Ubuntu layout
	KDC_CONF="/etc/krb5kdc/kdc.conf"
	KDC_DIR="/etc/krb5kdc"
else
	mkdir -p /var/lib/krb5kdc /etc/krb5kdc
	KDC_CONF="/etc/krb5kdc/kdc.conf"
	KDC_DIR="/etc/krb5kdc"
fi

info "Configuring KDC at $KDC_CONF"
cat >"$KDC_CONF" <<EOF
[kdcdefaults]
    kdc_ports = 88

[realms]
    $REALM = {
        database_name = $KDC_DIR/principal
        key_stash_file = $KDC_DIR/.k5.$REALM
    }
EOF

# -----------------------------------------------------------------------
# ACL for admin
# -----------------------------------------------------------------------
KADM5_ACL="$KDC_DIR/kadm5.acl"
info "Configuring $KADM5_ACL"
echo "*/admin@$REALM *" >"$KADM5_ACL"

# -----------------------------------------------------------------------
# Create realm database
# -----------------------------------------------------------------------
if [ ! -f "$KDC_DIR/principal" ]; then
	info "Creating realm database for $REALM..."
	kdb5_util create -s -r "$REALM" -P "$KDC_MASTER_PW"
else
	info "Realm database already exists, skipping create"
fi

# -----------------------------------------------------------------------
# Create principals
# -----------------------------------------------------------------------
info "Creating principals for $HOSTNAME..."

kadmin.local -r "$REALM" -q "addprinc -randkey host/$HOSTNAME@$REALM" 2>/dev/null || true
kadmin.local -r "$REALM" -q "addprinc -randkey nfs/$HOSTNAME@$REALM" 2>/dev/null || true
kadmin.local -r "$REALM" -q "addprinc -randkey host/localhost@$REALM" 2>/dev/null || true
kadmin.local -r "$REALM" -q "addprinc -randkey nfs/localhost@$REALM" 2>/dev/null || true

# Test user
kadmin.local -r "$REALM" -q "addprinc -pw $TEST_USER_PW nfstest@$REALM" 2>/dev/null || true

# Admin principal
kadmin.local -r "$REALM" -q "addprinc -pw $TEST_USER_PW admin/admin@$REALM" 2>/dev/null || true

# -----------------------------------------------------------------------
# Extract keytab
# -----------------------------------------------------------------------
info "Extracting keytab to $KEYTAB..."

kadmin.local -r "$REALM" -q "ktadd -k $KEYTAB host/$HOSTNAME@$REALM"
kadmin.local -r "$REALM" -q "ktadd -k $KEYTAB nfs/$HOSTNAME@$REALM"
kadmin.local -r "$REALM" -q "ktadd -k $KEYTAB host/localhost@$REALM"
kadmin.local -r "$REALM" -q "ktadd -k $KEYTAB nfs/localhost@$REALM"

# -----------------------------------------------------------------------
# Verify
# -----------------------------------------------------------------------
info "Keytab contents:"
klist -kt "$KEYTAB"

# -----------------------------------------------------------------------
# Start KDC
# -----------------------------------------------------------------------
if command -v systemctl >/dev/null 2>&1; then
	info "Starting KDC via systemd..."
	systemctl enable krb5kdc 2>/dev/null || true
	systemctl start krb5kdc
	systemctl status krb5kdc --no-pager || true
else
	info "Starting KDC directly..."
	krb5kdc
fi

# -----------------------------------------------------------------------
# Get test TGT
# -----------------------------------------------------------------------
info "Getting TGT for nfstest@$REALM..."
echo "$TEST_USER_PW" | kinit nfstest@"$REALM"
klist

info ""
info "=== Kerberos setup complete ==="
info "Realm:    $REALM"
info "Hostname: $HOSTNAME"
info "Keytab:   $KEYTAB"
info "Test TGT: nfstest@$REALM"
info ""
info "To mount with krb5:"
info "  mount -o vers=4.2,sec=krb5 $HOSTNAME:/ /mnt/nfs"
