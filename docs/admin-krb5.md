<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Kerberos 5 Setup and Debugging Guide

## Overview

reffsd supports RPCSEC_GSS with Kerberos 5 for both NFSv3 and NFSv4
authentication.  Three service levels are available:

| Mount option | Service | Protection |
|-------------|---------|------------|
| `sec=krb5`  | Authentication only | Client identity verified |
| `sec=krb5i` | Integrity | + MIC on every RPC body |
| `sec=krb5p` | Privacy | + encryption of every RPC body |

## Prerequisites

- A Kerberos KDC (MIT or Heimdal) reachable from both server and client
- A service principal `nfs/<hostname>@REALM` for the server
- A keytab containing the service principal's key
- Client: `rpc.gssd` running, valid TGT for the user

## Server Setup

### 1. Install packages

**Fedora 43:**
```bash
dnf install krb5-workstation krb5-devel
```

**Ubuntu 24.04:**
```bash
apt install krb5-user libkrb5-dev
```

### 2. Configure /etc/krb5.conf

```ini
[libdefaults]
    default_realm = EXAMPLE.COM
    dns_lookup_realm = false
    dns_lookup_kdc = false
    rdns = false

[realms]
    EXAMPLE.COM = {
        kdc = kdc.example.com
        admin_server = kdc.example.com
    }

[domain_realm]
    .example.com = EXAMPLE.COM
    example.com = EXAMPLE.COM
```

`rdns = false` avoids reverse-DNS issues in environments where PTR
records don't match forward lookups (containers, VPNs, etc.).

### 3. Obtain and install the keytab

On the KDC (or via kadmin):
```bash
# Create the service principal
kadmin -p admin/admin addprinc -randkey nfs/server.example.com@EXAMPLE.COM

# Extract the keytab
kadmin -p admin/admin ktadd -k /tmp/nfs.keytab nfs/server.example.com@EXAMPLE.COM
```

Copy the keytab to the server:
```bash
scp /tmp/nfs.keytab server:/etc/krb5.keytab
chmod 600 /etc/krb5.keytab
```

Verify the keytab:
```bash
klist -k /etc/krb5.keytab
# Should show: nfs/server.example.com@EXAMPLE.COM
```

### 4. Configure reffsd

```toml
[server]
port           = 2049
nfs4_domain    = "example.com"

[[export]]
path        = "/"
clients     = "*"
access      = "rw"
flavors     = ["sys", "krb5"]
# For integrity/privacy: flavors = ["krb5i"] or ["krb5p"]
# Multiple flavors: flavors = ["sys", "krb5", "krb5i", "krb5p"]
```

reffsd reads the keytab from the default location (`/etc/krb5.keytab`)
via `gss_acquire_cred()`.  No keytab path configuration is needed.

### 5. Start reffsd

```bash
reffsd --config=/etc/reffs.toml
```

On startup, look for:
```
RPCSEC_GSS: server credential acquired from keytab
```

If you see:
```
GSS server credential not available (no keytab)
```

...the keytab is missing or unreadable.  krb5 clients will receive
`NFS4ERR_DELAY` until the keytab is fixed.

## Client Setup

### 1. Install packages

**Fedora 43:**
```bash
dnf install nfs-utils krb5-workstation
```

**Ubuntu 24.04:**
```bash
apt install nfs-common krb5-user
```

### 2. Configure /etc/krb5.conf

Same as the server — must point at the same KDC and realm.

### 3. Start rpc.gssd

```bash
# Fedora (systemd):
systemctl start rpc-gssd

# Ubuntu (systemd):
systemctl start rpc-gssd

# Manual / container:
mkdir -p /run/rpc_pipefs
mount -t rpc_pipefs rpc_pipefs /run/rpc_pipefs
rpc.gssd
```

### 4. Get a TGT

```bash
kinit user@EXAMPLE.COM
klist   # verify: should show TGT for EXAMPLE.COM
```

### 5. Mount

```bash
# NFSv4.2:
mount -o vers=4.2,sec=krb5 server.example.com:/ /mnt

# NFSv3:
mount -o vers=3,sec=krb5 server.example.com:/ /mnt
```

## Debugging

### Server-side

**Check keytab:**
```bash
klist -k /etc/krb5.keytab
# Must show nfs/<hostname>@REALM
```

**Check reffsd log for GSS events:**
```bash
# Trace file (TRACE output):
grep "GSS INIT\|GSS DATA\|GSS context" /path/to/trace

# Log file (LOG output — errors only):
grep "GSS\|gss_acquire_cred\|WRONGSEC" /path/to/log
```

**Key trace messages:**
| Message | Meaning |
|---------|---------|
| `GSS INIT: new context request` | Client started GSS negotiation |
| `GSS INIT: accept major=0` | Context established successfully |
| `GSS INIT: accept major=N` | GSS accept failed (see below) |
| `GSS DATA: request ... seq=N` | Authenticated RPC being processed |
| `GSS DATA: verifier MIC failed` | Client's MIC doesn't verify |
| `GSS DATA: context not found` | Client using stale/unknown handle |
| `WRONGSEC: client flavor N` | Client flavor doesn't match export |

**Common GSS major status codes:**
| Code | Name | Meaning |
|------|------|---------|
| 0 | GSS_S_COMPLETE | Success |
| 65536 | GSS_S_CONTINUE_NEEDED | Multi-round-trip (normal) |
| 393216 | GSS_S_BAD_SIG | MIC verification failed |
| 851968 | GSS_S_NO_CRED | No keytab or wrong principal |
| 983040 | GSS_S_FAILURE | Generic failure |

**Verify service ticket from client:**
```bash
# On the client, after kinit:
kvno nfs/server.example.com@EXAMPLE.COM
# Should return: kvno = N
# If it fails, the KDC can't issue a ticket for this principal
```

### Client-side

**Check TGT:**
```bash
klist
# Must show a valid, non-expired TGT
```

**Check rpc.gssd is running:**
```bash
ps aux | grep gssd
# Must show rpc.gssd process
```

**Check rpc_pipefs is mounted:**
```bash
mount | grep rpc_pipefs
# Must show rpc_pipefs on /run/rpc_pipefs (or /var/lib/nfs/rpc_pipefs)
```

**Network debugging:**
```bash
# Capture NFS traffic:
tshark -i eth0 -f "port 2049" -Y "rpc.auth.flavor == 6"

# Check connectivity to KDC:
kinit -V user@EXAMPLE.COM   # verbose mode
```

**Mount hangs with sec=krb5:**
1. Is rpc.gssd running? (`ps aux | grep gssd`)
2. Is rpc_pipefs mounted? (`mount | grep rpc_pipefs`)
3. Is the TGT valid? (`klist` — check expiry)
4. Can you get a service ticket? (`kvno nfs/server@REALM`)
5. Is the KDC reachable? (`kinit -V user@REALM`)
6. Is the server's keytab correct? (`klist -k /etc/krb5.keytab`)

### Verification script

```bash
#!/bin/bash
# verify-krb5.sh — quick check of krb5 NFS prerequisites
set -e

echo "=== Client checks ==="
echo -n "TGT: "; klist -s && echo "OK" || echo "MISSING (run kinit)"
echo -n "gssd: "; pgrep -x rpc.gssd >/dev/null && echo "running" || echo "NOT running"
echo -n "rpc_pipefs: "; mount | grep -q rpc_pipefs && echo "mounted" || echo "NOT mounted"

SERVER=${1:-localhost}
REALM=$(klist 2>/dev/null | grep "Default principal" | sed 's/.*@//')
echo -n "service ticket: "
kvno nfs/$SERVER@$REALM 2>/dev/null && echo "OK" || echo "FAILED"

echo ""
echo "=== Mount test ==="
MOUNT=$(mktemp -d)
mount -o vers=4.2,sec=krb5,soft,timeo=10,retrans=2 $SERVER:/ $MOUNT && \
    echo "PASS" && umount $MOUNT || echo "FAIL"
rmdir $MOUNT
```
