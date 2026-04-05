#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# bat_tls_setup.sh -- Generate TLS certificates for BAT testing.
#
# Creates a self-signed CA, server cert, and client cert under
# /etc/reffs/tls/.  The server cert includes SAN entries for
# 127.0.0.1 and the local hostname.
#
# Usage: sudo bat_tls_setup.sh [HOSTNAME]
#   HOSTNAME  The FQDN for the server cert SAN (default: $(hostname -f))
#
# Prerequisites: openssl

set -euo pipefail

TLS_DIR="/etc/reffs/tls"
HOSTNAME=${1:-$(hostname -f)}
DAYS=365

die() { echo "FATAL: $*" >&2; exit 1; }
info() { echo "[tls] $*"; }

# -----------------------------------------------------------------------
# Verify openssl is available
# -----------------------------------------------------------------------
command -v openssl >/dev/null 2>&1 || die "openssl not found"

# -----------------------------------------------------------------------
# Create TLS directory
# -----------------------------------------------------------------------
mkdir -p "$TLS_DIR"
chmod 700 "$TLS_DIR"

# -----------------------------------------------------------------------
# Generate CA key + self-signed cert
# -----------------------------------------------------------------------
if [ -f "$TLS_DIR/ca.pem" ]; then
	info "CA cert already exists at $TLS_DIR/ca.pem, skipping"
else
	info "Generating CA..."
	openssl req -x509 -newkey rsa:2048 -nodes \
		-keyout "$TLS_DIR/ca-key.pem" \
		-out "$TLS_DIR/ca.pem" \
		-days "$DAYS" \
		-subj "/CN=reffs-bat-ca"
	chmod 600 "$TLS_DIR/ca-key.pem"
fi

# -----------------------------------------------------------------------
# Generate server key + CSR + sign with CA
# -----------------------------------------------------------------------
if [ -f "$TLS_DIR/server.pem" ]; then
	info "Server cert already exists at $TLS_DIR/server.pem, skipping"
else
	info "Generating server cert for $HOSTNAME..."

	# SAN config for server cert
	cat >"$TLS_DIR/server-san.cnf" <<EOF
[req]
distinguished_name = req_dn
req_extensions = v3_req
prompt = no

[req_dn]
CN = $HOSTNAME

[v3_req]
subjectAltName = IP:127.0.0.1,DNS:$HOSTNAME,DNS:localhost
EOF

	openssl req -newkey rsa:2048 -nodes \
		-keyout "$TLS_DIR/server-key.pem" \
		-out "$TLS_DIR/server.csr" \
		-config "$TLS_DIR/server-san.cnf"

	openssl x509 -req \
		-in "$TLS_DIR/server.csr" \
		-CA "$TLS_DIR/ca.pem" \
		-CAkey "$TLS_DIR/ca-key.pem" \
		-CAcreateserial \
		-out "$TLS_DIR/server.pem" \
		-days "$DAYS" \
		-extensions v3_req \
		-extfile "$TLS_DIR/server-san.cnf"

	chmod 600 "$TLS_DIR/server-key.pem"
	rm -f "$TLS_DIR/server.csr" "$TLS_DIR/server-san.cnf"
fi

# -----------------------------------------------------------------------
# Generate client key + CSR + sign with CA
# -----------------------------------------------------------------------
if [ -f "$TLS_DIR/client.pem" ]; then
	info "Client cert already exists at $TLS_DIR/client.pem, skipping"
else
	info "Generating client cert..."

	openssl req -newkey rsa:2048 -nodes \
		-keyout "$TLS_DIR/client-key.pem" \
		-out "$TLS_DIR/client.csr" \
		-subj "/CN=reffs-bat-client"

	openssl x509 -req \
		-in "$TLS_DIR/client.csr" \
		-CA "$TLS_DIR/ca.pem" \
		-CAkey "$TLS_DIR/ca-key.pem" \
		-CAcreateserial \
		-out "$TLS_DIR/client.pem" \
		-days "$DAYS"

	chmod 600 "$TLS_DIR/client-key.pem"
	rm -f "$TLS_DIR/client.csr"
fi

# -----------------------------------------------------------------------
# Cleanup temp files
# -----------------------------------------------------------------------
rm -f "$TLS_DIR/ca.srl"

# -----------------------------------------------------------------------
# Verify
# -----------------------------------------------------------------------
info "Verifying certificates..."
openssl verify -CAfile "$TLS_DIR/ca.pem" "$TLS_DIR/server.pem" || \
	die "Server cert verification failed"
openssl verify -CAfile "$TLS_DIR/ca.pem" "$TLS_DIR/client.pem" || \
	die "Client cert verification failed"

info ""
info "=== TLS setup complete ==="
info "CA:     $TLS_DIR/ca.pem"
info "Server: $TLS_DIR/server.pem (key: server-key.pem)"
info "Client: $TLS_DIR/client.pem (key: client-key.pem)"
info "SAN:    IP:127.0.0.1, DNS:$HOSTNAME, DNS:localhost"
info "Valid:  $DAYS days"
