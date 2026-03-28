<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# TLS Setup and Debugging Guide

## Overview

reffsd supports RPC-with-TLS per RFC 9289.  The kernel NFS client
sends an AUTH_TLS NULL RPC probe with a STARTTLS verifier.  If the
server accepts, the connection upgrades to TLS and all subsequent
RPC traffic is encrypted.  This protects AUTH_SYS credentials and
NFS data on the wire without requiring Kerberos.

## Prerequisites

- Server: TLS certificate + private key (PEM format)
- Client: `tlshd` running (kernel NFS TLS handoff daemon)
- Trust: client must trust the server's CA

## Server Setup

### 1. Install packages

**Fedora 43:**
```bash
dnf install openssl
```

**Ubuntu 24.04:**
```bash
apt install openssl
```

reffsd links against `libssl-dev` / `openssl-devel` at build time.
No additional runtime packages are needed beyond OpenSSL.

### 2. Generate certificates

For testing / development (self-signed):
```bash
# Generate CA
openssl req -x509 -newkey rsa:4096 -keyout ca.key -out ca.pem \
    -days 365 -nodes -subj "/CN=reffs-ca"

# Generate server cert signed by CA
openssl req -newkey rsa:4096 -keyout server.key -out server.csr \
    -nodes -subj "/CN=server.example.com"
openssl x509 -req -in server.csr -CA ca.pem -CAkey ca.key \
    -CAcreateserial -out server.pem -days 365

# Install
mkdir -p /etc/tlshd
cp server.pem /etc/tlshd/server.pem
cp server.key /etc/tlshd/server.key
chmod 600 /etc/tlshd/server.key
```

For production: use certificates from your organization's PKI or
a tool like `certbot`.

### 3. Certificate path resolution

reffsd looks for certificates in this order:

| Priority | Source | Cert | Key |
|----------|--------|------|-----|
| 1 | Config file | `tls_cert` | `tls_key` |
| 2 | Environment | `REFFS_CERT_PATH` | `REFFS_KEY_PATH` |
| 3 | Default | `/etc/tlshd/server.pem` | `/etc/tlshd/server.key` |

The `/etc/tlshd/` default is chosen to share certificates with the
kernel's `tlshd` daemon, which uses the same directory.

### 4. Configure reffsd

```toml
[server]
port     = 2049
tls_cert = "/etc/tlshd/server.pem"
tls_key  = "/etc/tlshd/server.key"

[[export]]
path        = "/"
clients     = "*"
access      = "rw"
flavors     = ["tls"]
# Or mixed: flavors = ["sys", "tls"]
# TLS means AUTH_SYS over TLS transport
```

The `tls` flavor means "AUTH_SYS credentials are required, but
only accepted over a TLS-encrypted connection."  Unencrypted
AUTH_SYS connections receive `NFS4ERR_WRONGSEC`.

### 5. Start reffsd

```bash
reffsd --config=/etc/reffs.toml
```

On startup, look for the TLS context initialization trace.
If certificates are not found:
```
TLS context init deferred (certs not found at startup)
```

### 6. Environment variables

| Variable | Default | Purpose |
|----------|---------|---------|
| `REFFS_CERT_PATH` | `/etc/tlshd/server.pem` | Server certificate |
| `REFFS_KEY_PATH` | `/etc/tlshd/server.key` | Private key |
| `REFFS_MIN_TLS_VERSION` | `1.3` | Minimum TLS version (1.0-1.3) |
| `REFFS_REQUIRE_ALPN` | `1` | Require ALPN "sunrpc" (RFC 9289) |

## Client Setup

### 1. Install tlshd

**Fedora 43:**
```bash
dnf install ktls-utils
systemctl enable --now tlshd
```

**Ubuntu 24.04:**
```bash
# ktls-utils may need to be built from source or installed
# from a PPA.  Check: https://github.com/oracle/ktls-utils
apt install ktls-utils   # if available
systemctl enable --now tlshd
```

`tlshd` is the kernel NFS TLS handoff daemon.  When the kernel NFS
client detects a server that supports TLS (via the AUTH_TLS probe),
it hands the connection to `tlshd` for the TLS handshake, then
takes back the encrypted socket.

### 2. Trust the server's CA

Copy the CA certificate to the client's trust store:

**Fedora 43:**
```bash
cp ca.pem /etc/pki/tls/certs/reffs-ca.pem
update-ca-trust
```

**Ubuntu 24.04:**
```bash
cp ca.pem /usr/local/share/ca-certificates/reffs-ca.crt
update-ca-certificates
```

`tlshd` uses the system trust store to verify the server's
certificate.

### 3. Mount

```bash
# The kernel auto-negotiates TLS if the server supports it
# and tlshd is running.  No special mount option needed:
mount -o vers=4.2 server.example.com:/ /mnt

# To force TLS (fail if server doesn't support it):
mount -o vers=4.2,xprtsec=tls server.example.com:/ /mnt
```

Note: `xprtsec=tls` requires kernel 6.6+ and a version of
`mount.nfs` that supports the option.

## Debugging

### Server-side

**Check certificates:**
```bash
# Verify cert is readable and not expired:
openssl x509 -in /etc/tlshd/server.pem -noout -dates -subject

# Verify key matches cert:
CERT_MOD=$(openssl x509 -in /etc/tlshd/server.pem -noout -modulus | md5sum)
KEY_MOD=$(openssl pkey -in /etc/tlshd/server.key -pubout 2>/dev/null | openssl md5)
[ "$CERT_MOD" = "$KEY_MOD" ] && echo "Key matches cert" || echo "KEY MISMATCH"

# Check permissions:
ls -la /etc/tlshd/server.key
# Must be readable by the reffsd process (usually root)
```

**Check reffsd traces:**
```bash
grep "TLS\|AUTH_TLS\|STARTTLS\|ssl" /path/to/trace
```

**Key trace messages:**
| Message | Meaning |
|---------|---------|
| `AUTH_TLS probe on fd=N` | Client sent TLS probe (good) |
| `TLS ClientHello detected` | TLS handshake starting |
| `TLS context init deferred` | Certs not found at startup |
| `SSL error` | Handshake failed (see below) |

**Common failures:**
| Symptom | Cause | Fix |
|---------|-------|-----|
| `TLS context init deferred` | Missing cert/key | Check paths, permissions |
| `SSL error` during handshake | Cert expired, key mismatch, or untrusted CA | Verify cert with openssl |
| Client gets WRONGSEC | Export has `flavors = ["tls"]` but client not using TLS | Start tlshd on client |
| Mount hangs | AUTH_TLS probe not answered | Check reffsd is running, cert is loaded |

### Client-side

**Check tlshd is running:**
```bash
systemctl status tlshd
# or
ps aux | grep tlshd
```

**Check CA trust:**
```bash
# Verify the server's cert against the client trust store:
openssl s_client -connect server.example.com:2049 \
    -alpn sunrpc -brief 2>/dev/null | head -5
# Should show: Verification: OK
```

**Network debugging:**
```bash
# Capture the AUTH_TLS probe:
tshark -i eth0 -f "port 2049" -Y "rpc.auth.flavor == 7"
# flavor 7 = AUTH_TLS

# Check if TLS is negotiated:
tshark -i eth0 -f "port 2049" -Y "tls.handshake"
```

**Mount hangs with flavors=["tls"]:**
1. Is tlshd running? (`systemctl status tlshd`)
2. Does the client trust the server CA? (`openssl s_client` test)
3. Is the server cert valid? (`openssl x509 -dates`)
4. Is the key readable? (`ls -la /etc/tlshd/server.key`)
5. Does the export include `tls` in flavors?
6. Does reffsd show `AUTH_TLS probe` in traces?

### Verification script

```bash
#!/bin/bash
# verify-tls.sh — quick check of TLS NFS prerequisites
set -e

SERVER=${1:-localhost}
PORT=${2:-2049}

echo "=== Server certificate ==="
openssl s_client -connect $SERVER:$PORT -alpn sunrpc \
    </dev/null 2>/dev/null | \
    openssl x509 -noout -subject -dates 2>/dev/null || \
    echo "FAILED: cannot connect or no TLS on port $PORT"

echo ""
echo "=== Client checks ==="
echo -n "tlshd: "
pgrep -x tlshd >/dev/null 2>&1 && echo "running" || echo "NOT running"

echo ""
echo "=== Mount test ==="
MOUNT=$(mktemp -d)
mount -o vers=4.2,soft,timeo=10,retrans=2 $SERVER:/ $MOUNT 2>/dev/null && \
    echo "PASS (mount succeeded, TLS auto-negotiated if available)" && \
    umount $MOUNT || echo "FAIL"
rmdir $MOUNT 2>/dev/null
```
