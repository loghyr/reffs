#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Mini-CA fixture for the slice plan-1-tls.c PS-MDS smoke (#139).
#
# Generates a self-signed CA + a PS client cert signed by it, then
# computes the SHA-256 fingerprint of the PS cert in the colon-
# separated hex form that [[allowed_ps]].tls_cert_fingerprint
# expects (matches lib/nfs4/server/proxy_registration.c's
# tls_cert_fingerprint allowlist semantics).
#
# Output (under <out_dir>, default $1 or /tmp/reffs_ps_tls):
#   ca.crt         -- self-signed CA cert (PEM)
#   ca.key         -- CA private key (PEM)
#   ps.crt         -- PS client cert, signed by ca.crt
#   ps.key         -- PS client private key
#   ps.fpr         -- SHA-256 fingerprint of ps.crt (colon-hex)
#
# The smoke pipes ps.fpr into the MDS [[allowed_ps]] block before
# starting reffsd, then mounts the [[proxy_mds]] cfg with tls_cert
# / tls_key / tls_ca / tls_insecure_no_verify=true so the PS
# brings up mTLS to the upstream MDS.  PROXY_REGISTRATION reaches
# NFS4_OK because the MDS sees the PS's allowlisted fingerprint.
#
# Why not openssl req -newkey:
#   The combined "generate key + CSR + self-sign + extensions" form
#   pulls in a config file or a long -addext chain.  Using two
#   `openssl genpkey` + one `openssl x509 -req` call keeps the
#   moving parts visible and the cert minimal (no SAN -- this is
#   client auth where the server validates by fingerprint, not
#   subject).

set -euo pipefail

OUT="${1:-/tmp/reffs_ps_tls}"
mkdir -p "$OUT"

# ---- CA ----
openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 \
    -out "$OUT/ca.key" 2>/dev/null

# Inline config instead of relying on the host's openssl.cnf so the
# script behaves identically on Fedora (system cnf has CA defaults)
# and Ubuntu (different defaults).  The basicConstraints CA:TRUE
# is essential -- without it the MDS's SSL_CTX_load_verify_locations
# refuses to treat ca.crt as a trust anchor and PROXY_REGISTRATION
# stalls in handshake with "certificate verify failed".
CA_CFG=$(mktemp)
trap 'rm -f "$CA_CFG"' EXIT
cat > "$CA_CFG" <<'EOF'
[req]
distinguished_name = dn
prompt = no
x509_extensions = v3_ca
[dn]
CN = reffs-test-CA
[v3_ca]
basicConstraints = critical,CA:TRUE
keyUsage = critical,keyCertSign,cRLSign
EOF

openssl req -x509 -new -key "$OUT/ca.key" -days 7 \
    -config "$CA_CFG" -out "$OUT/ca.crt" 2>/dev/null

# ---- PS client cert ----
openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 \
    -out "$OUT/ps.key" 2>/dev/null

PS_CFG=$(mktemp)
cat > "$PS_CFG" <<'EOF'
[req]
distinguished_name = dn
prompt = no
[dn]
CN = reffs-test-PS
EOF

openssl req -new -key "$OUT/ps.key" -config "$PS_CFG" \
    -out "$OUT/ps.csr" 2>/dev/null
rm -f "$PS_CFG"

openssl x509 -req -in "$OUT/ps.csr" \
    -CA "$OUT/ca.crt" -CAkey "$OUT/ca.key" \
    -CAcreateserial -days 7 -out "$OUT/ps.crt" 2>/dev/null

rm -f "$OUT/ps.csr" "$OUT/ca.srl"

# ---- Fingerprint (SHA-256, colon-separated hex, matches the
#      MDS allowlist parser's expected form) ----
openssl x509 -in "$OUT/ps.crt" -noout -fingerprint -sha256 \
    | sed 's/^.*Fingerprint=//' \
    > "$OUT/ps.fpr"

chmod 0600 "$OUT/ca.key" "$OUT/ps.key"

echo "mini-CA materials in $OUT:"
ls -l "$OUT"
echo
echo "PS cert fingerprint:"
cat "$OUT/ps.fpr"
