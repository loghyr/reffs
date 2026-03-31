<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# nfs_tls_stress — TLS NFS Connection Stress Tester

A standalone diagnostic tool for stress-testing TLS connections to
NFSv4.2 servers.  Deploys as a single binary with no dependencies
beyond OpenSSL.

## Problem

TLS reconnection on NFS can cause a "loop-of-death" when:

- The server expects cleartext STARTTLS (RFC 9289) on a fresh
  connection but the client sends an SSL ClientHello (hot reconnect)
- TLS handshake errors are reported as I/O errors (ESHUTDOWN),
  causing full xprt teardown instead of connection retry with backoff
- The server doesn't process encrypted messages on a reconnected socket

This tool systematically exercises these failure modes.

## Building

```bash
# Dynamic link (development)
cc -o nfs_tls_stress tools/nfs_tls_stress.c -lssl -lcrypto

# Static link (customer deployment)
cc -static -o nfs_tls_stress tools/nfs_tls_stress.c -lssl -lcrypto -lpthread -ldl
```

No autotools, no configure, no TIRPC.  The tool has zero reffs
library dependencies — it hand-rolls all RPC/XDR encoding.

## Quick Start

```bash
# Basic STARTTLS cycling — verify TLS works
./nfs_tls_stress --host 10.0.0.1 --verbose

# Hot reconnect — reproduce the loop-of-death
./nfs_tls_stress --host 10.0.0.1 --mode hot-reconnect -n 50 --verbose

# All modes, with TLS handshake tracing
./nfs_tls_stress --host 10.0.0.1 --mode all --trace --verbose

# With client certificates (mutual TLS)
./nfs_tls_stress --host 10.0.0.1 \
    --cert /etc/ssl/client.pem \
    --key /etc/ssl/client.key \
    --ca /etc/ssl/ca.pem

# Direct TLS (server expects TLS immediately, no STARTTLS)
./nfs_tls_stress --host 10.0.0.1 --direct-tls --mode starttls-loop
```

## Test Modes

### starttls-loop (default)

Normal STARTTLS connection cycling.  Each iteration:

1. TCP connect
2. Send AUTH_TLS NULL RPC (cleartext, RFC 9289)
3. Receive STARTTLS reply
4. SSL_connect (upgrade to TLS)
5. NFSv4.2 session setup (EXCHANGE_ID + CREATE_SESSION)
6. Probe: SEQUENCE + PUTROOTFH + GETATTR (metadata only, no I/O)
7. SSL_shutdown + TCP close

Validates the basic RFC 9289 path works reliably under repetition.
The same client identity is used across iterations — the server
must correctly handle the "same principal, new verifier" path
(EXCHANGE_ID case 3: new incarnation, invalidate old state).

### mid-op-disconnect

Drop the connection while an NFS operation is in flight.  Each
iteration has two phases:

**Phase 1 (break):**
1. Establish TLS + session
2. Send GETATTR probe (encrypted)
3. Hard close immediately — no SSL_shutdown, no recv

**Phase 2 (verify recovery):**
1. Reconnect (TCP + TLS + new session)
2. Send GETATTR probe
3. Verify reply — server recovered from the broken connection

This triggers ESHUTDOWN → xprt teardown on the server.  Phase 2
verifies the server can accept new connections after the failure.

### hot-reconnect

Simulate the "loop-of-death" where a client reconnects and sends
SSL ClientHello instead of cleartext STARTTLS:

**Phase 1:** Normal STARTTLS → probe → hard close
**Phase 2:** TCP connect → SSL_connect immediately (NO STARTTLS)

If the server accepts direct TLS: probe and verify.
If the server rejects: retry with proper STARTTLS to verify the
server hasn't entered an unrecoverable state.

This is the exact scenario that causes permanent reconnect failures
when the server only handles cleartext STARTTLS on fresh connections.

### rapid-cycle

Maximum connection churn with no delay:

1. TCP connect → TLS → session → probe
2. TCP RST (SO_LINGER=0) — immediate close, no FIN/ACK
3. Immediately reconnect (no delay)

Tests connection table cleanup under load.  Verifies the server
doesn't leak file descriptors, SSL contexts, or NFS session state.

### all

Runs all four modes in sequence.

## NFSv4.2 Protocol Details

The tool implements a minimal NFSv4.2 client — just enough for
connection verification:

- **EXCHANGE_ID**: Establishes client identity with the server.
  Uses `EXCHGID4_FLAG_USE_NON_PNFS` (no pNFS).
- **CREATE_SESSION**: Creates a single-slot session.  Backchannel
  uses AUTH_NONE (simplest).
- **SEQUENCE + PUTROOTFH + GETATTR**: The probe compound.  Gets
  `supported_attrs` from the root filehandle.  Pure metadata — no
  file I/O, no directory traversal.

The tool uses the same client owner string for all iterations.
Each reconnection sends EXCHANGE_ID with a new verifier, which
tells the server "same client, restarted."  The server must
invalidate old state (sessions, stateids) and issue a new
clientid.  This exercises the full reconnection path.

## Options

| Option | Short | Description |
|--------|-------|-------------|
| `--host` | `-H` | Server address (required) |
| `--port` | `-p` | NFS port (default: 2049) |
| `--mode` | `-m` | Test mode (default: starttls-loop) |
| `--iterations` | `-n` | Iterations per mode (default: 100) |
| `--cert` | `-c` | Client certificate for mutual TLS |
| `--key` | `-k` | Client private key |
| `--ca` | | CA certificate for server verification |
| `--direct-tls` | | Use direct TLS (skip STARTTLS) |
| `--no-verify` | | Skip server certificate verification |
| `--trace` | `-t` | Log TLS handshake details per connection |
| `--verbose` | `-v` | Per-iteration output |
| `--help` | `-h` | Help |

## Output

### Normal (default)

```
nfs_tls_stress: host=10.0.0.1 port=2049 mode=starttls-loop iterations=100

Running: starttls-loop

=== starttls-loop ===
  Iterations: 100
  Successes:  100 (100.0%)
  Failures:   0
  Reconnects: 0
  Avg latency: 5.231ms
  Max latency: 12.407ms
```

### Verbose (`--verbose`)

```
  [1/100] OK 5.421ms
  [2/100] OK 4.892ms
  ...
  [42/100] FAIL (probe rc=-1)
  [43/100] OK 5.102ms
```

### Trace (`--trace`)

```
  [TLS starttls] TLSv1.3 cipher=TLS_AES_256_GCM_SHA384 alpn=sunrpc session=new
  [1/100] OK 5.421ms
  [TLS starttls] TLSv1.3 cipher=TLS_AES_256_GCM_SHA384 alpn=sunrpc session=new
  [2/100] OK 4.892ms
```

## Exit Codes

- **0**: All iterations succeeded
- **1**: One or more iterations failed
- **2**: Usage error

## Troubleshooting

### All iterations fail at STARTTLS

The server may not support RFC 9289 AUTH_TLS.  Try `--direct-tls`
if the server expects TLS immediately on connect.

### Hot-reconnect always rejected

Expected — most servers require STARTTLS on fresh connections.
The test verifies the server recovers gracefully after rejecting
the direct TLS attempt.  Check the STARTTLS retry in the output.

### Timeouts during session setup

The tool uses a 5-second socket timeout.  If the server is slow
(heavy load, distant network), increase the timeout by editing
`tcp_connect()` in the source.

### SSL certificate errors

Use `--no-verify` to skip server certificate verification, or
provide the server's CA with `--ca`.  For mutual TLS, provide
both `--cert` and `--key`.
