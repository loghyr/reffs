<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# NFS Security Test Tools

Standalone userspace tools for verifying NFS security configurations.
No kernel NFS client involvement — can be pointed at any NFSv4
server from any platform.

## nfs_krb5_test

Tests RPCSEC_GSS (Kerberos 5) authentication, data integrity, and
identity mapping by establishing a GSS session, writing data,
reading it back with CRC verification, and cleaning up.

### Prerequisites

1. A valid Kerberos TGT:
   ```bash
   kinit user@REALM
   klist   # verify non-expired TGT
   ```

2. The target server must have:
   - A keytab with `nfs/<hostname>@REALM`
   - An export with krb5 in its flavor list

### Usage

```bash
# Basic krb5 test (session + write + read/CRC + cleanup):
nfs_krb5_test --server nfs.example.com

# Test with krb5i (integrity) or krb5p (privacy):
nfs_krb5_test --server nfs.example.com --sec krb5i
nfs_krb5_test --server nfs.example.com --sec krb5p

# Also test owner string GETATTR/SETATTR:
nfs_krb5_test --server nfs.example.com --file somefile \
              --setowner alice@EXAMPLE.COM

# Custom client owner (for multi-instance testing):
nfs_krb5_test --server nfs.example.com --id mytest01
```

### What it tests

| Test | Description |
|------|-------------|
| 1. GSS session | EXCHANGE_ID + CREATE_SESSION with RPCSEC_GSS |
| 2. WRITE | Create file, write 8 KB (two 4 KB blocks) |
| 3. READ + CRC | Reopen, read back, FNV-1a hash verify |
| 4. GETATTR | Owner string resolution (if --file) |
| 5. SETATTR | Owner string round-trip (if --setowner) |
| Cleanup | REMOVE test file |

### Exit codes

- `0` — all tests passed
- `1` — one or more tests failed
- `2` — usage error

### Troubleshooting

| Error | Cause | Fix |
|-------|-------|-----|
| `FAIL (ret=-111)` | Connection refused | Server not running or wrong port |
| `FAIL (ret=-13)` | Permission denied / WRONGSEC | Check export flavors include krb5 |
| `authgss_create_default failed` | GSS negotiation failed | Check kinit, keytab, KDC |
| `FAIL (open: -121)` | OPEN failed | File path issue or server error |
| `FAIL (crc mismatch)` | Data corruption | Server write/read bug |

### How TGTs work

A Kerberos Ticket-Granting Ticket (TGT) is obtained by `kinit`
and cached in a credential cache file (typically `/tmp/krb5cc_UID`
or pointed to by `KRB5CCNAME`).  The TGT is used by
`gss_init_sec_context()` to obtain a service ticket for
`nfs/<hostname>@REALM`, which is then used for RPCSEC_GSS
authentication.

The TGT has a limited lifetime (typically 8-24 hours).  If the
test fails with a GSS error after it previously worked, check
`klist` for an expired ticket and re-run `kinit`.

---

## nfs_tls_test

Tests RPC-with-TLS (RFC 9289) support by sending an AUTH_TLS
probe, verifying the STARTTLS response, and upgrading the
connection to TLS.

### Prerequisites

1. The target server must support AUTH_TLS (RFC 9289)
2. For server certificate verification: the CA certificate
3. For mutual TLS: a client certificate + private key

### Usage

```bash
# Basic TLS probe + handshake:
nfs_tls_test --server nfs.example.com

# With server certificate verification:
nfs_tls_test --server nfs.example.com --ca /path/to/ca.pem

# With mutual TLS (client cert):
nfs_tls_test --server nfs.example.com \
             --ca /path/to/ca.pem \
             --cert /path/to/client.pem \
             --key /path/to/client.key

# Non-standard port:
nfs_tls_test --server nfs.example.com --port 20490
```

### What it tests

| Test | Description |
|------|-------------|
| 1. TCP connect | Basic connectivity to the NFS port |
| 2. AUTH_TLS probe | Send NULL RPC with AUTH_TLS + STARTTLS verifier |
| 3. TLS handshake | SSL_connect with ALPN "sunrpc" |

### Exit codes

Same as nfs_krb5_test.

### Troubleshooting

| Error | Cause | Fix |
|-------|-------|-----|
| `FAIL (connect)` | Server not running | Check server + port |
| `FAIL (reply)` | Server doesn't support AUTH_TLS | Check TLS config |
| `FAIL (SSL error)` | TLS handshake failed | Check certs, CA trust |
| `PASS (no ALPN)` | Server didn't negotiate ALPN | Informational, still works |

### How certificates work

RPC-with-TLS uses standard X.509 certificates.  The server presents
a certificate signed by a CA that the client trusts.  For mutual
TLS, the client also presents a certificate.

**For testing / development:**
```bash
# Self-signed CA + server cert:
openssl req -x509 -newkey rsa:4096 -keyout ca.key -out ca.pem \
    -days 365 -nodes -subj "/CN=test-ca"
openssl req -newkey rsa:4096 -keyout server.key -out server.csr \
    -nodes -subj "/CN=localhost"
openssl x509 -req -in server.csr -CA ca.pem -CAkey ca.key \
    -CAcreateserial -out server.pem -days 365
```

**For the BAT (NFS Bake-a-thon):**
See `bat-setup/BAT-PLAN.md` for the 2-tier CA workflow with
separate serverAuth and clientAuth certificates.

**Certificate path on the server:**
reffsd looks for certs in this order:
1. Config: `tls_cert` / `tls_key`
2. Environment: `REFFS_CERT_PATH` / `REFFS_KEY_PATH`
3. Default: `/etc/tlshd/server.pem` / `/etc/tlshd/server.key`

---

## Running in CI

Both tools are built automatically by `make` and used in the CI
integration tests:

- `nfs_krb5_test` runs in `ci-check` when a KDC is available
  (Docker CI image has one pre-configured)
- `nfs_tls_test` is not yet in CI (requires TLS cert setup)

For local testing:
```bash
# Start reffsd, then:
./build/tools/nfs_krb5_test --server 127.0.0.1 --sec krb5
./build/tools/nfs_tls_test --server 127.0.0.1
```
