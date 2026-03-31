<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# TLS NFS Connection Stress Tool

## Problem

TLS reconnection on NFS causes a loop-of-death when:
- The server expects cleartext STARTTLS on a fresh connection but the
  client sends an SSL ClientHello (hot reconnect)
- TLS handshake errors are reported as I/O errors (ESHUTDOWN), causing
  full xprt teardown instead of connection retry with backoff
- The server doesn't process encrypted messages on a reconnected socket

## Goal

A standalone, statically-linked diagnostic tool that stress-tests TLS
NFS connections.  Deploy to customer environments with zero dependencies
(beyond libc).  No TIRPC, no liburcu, no reffs libraries.

## Design

### Architecture

Single-threaded, event-loop-free.  Connect → op → disconnect → repeat.
All RPC encoding/decoding is hand-rolled (minimal, just enough for
FSINFO/NULL/GETATTR).  OpenSSL linked statically.

```
┌─────────────────────────────────────┐
│          nfs_tls_stress             │
├─────────────────────────────────────┤
│  CLI: --host --port --proto --mode  │
│  --duration --iterations --verbose  │
├─────────────────────────────────────┤
│  Test modes:                        │
│  1. starttls-loop                   │
│  2. mid-op-disconnect               │
│  3. hot-reconnect                   │
│  4. rapid-cycle                     │
│  5. all (runs 1-4 in sequence)      │
├─────────────────────────────────────┤
│  RPC layer (hand-rolled):           │
│  - RPC record marking (fragments)   │
│  - AUTH_TLS NULL (STARTTLS trigger) │
│  - AUTH_SYS credentials             │
│  - NFSv3 FSINFO / NULL              │
│  - NFSv4.2 COMPOUND (SEQUENCE +     │
│    PUTROOTFH + GETATTR)             │
├─────────────────────────────────────┤
│  TLS layer:                         │
│  - SSL_CTX with configurable certs  │
│  - ALPN "sunrpc" (RFC 9289)        │
│  - Direct TLS (no STARTTLS)         │
│  - STARTTLS (AUTH_TLS NULL first)   │
├─────────────────────────────────────┤
│  Socket layer:                      │
│  - TCP connect/close                │
│  - Configurable send/recv timeout   │
│  - SO_LINGER for hard close         │
└─────────────────────────────────────┘
```

### Test Modes

#### 1. starttls-loop

Normal STARTTLS connection cycling:
```
for each iteration:
  TCP connect
  send AUTH_TLS NULL RPC (cleartext)
  recv AUTH_TLS reply
  SSL_connect (upgrade to TLS)
  send FSINFO RPC (encrypted)
  recv FSINFO reply
  SSL_shutdown
  TCP close
```

Validates the basic RFC 9289 path works reliably under repetition.

#### 2. mid-op-disconnect

Drop the connection while an NFS operation is in flight:
```
for each iteration:
  TCP connect
  STARTTLS or direct TLS
  send FSINFO RPC (encrypted)
  TCP close immediately (no SSL_shutdown, no recv)
  sleep random 0-100ms
  TCP connect
  STARTTLS or direct TLS
  send FSINFO RPC
  recv FSINFO reply (verify server recovered)
  clean shutdown
```

This is the scenario that triggers ESHUTDOWN → xprt teardown.

#### 3. hot-reconnect

Simulate the "loop-of-death": client reconnects and sends SSL
ClientHello instead of cleartext STARTTLS:
```
for each iteration:
  TCP connect
  STARTTLS → TLS established
  send FSINFO (encrypted) → success
  TCP close (no SSL_shutdown)
  TCP connect (same port)
  SSL_connect immediately (NO cleartext STARTTLS first)
  → This is the "hot reconnect" that breaks the server
  if server accepts: send FSINFO → verify
  if server rejects: log the error and retry with STARTTLS
  clean shutdown
```

This tests whether the server can handle a client that skips
STARTTLS and goes straight to TLS handshake on a fresh TCP
connection (the "direct TLS" mode).

#### 4. rapid-cycle

Maximum connection churn with no delay:
```
for each iteration:
  TCP connect
  STARTTLS → TLS
  FSINFO
  TCP RST (SO_LINGER=0) — immediate close, no FIN/ACK
  // no delay, immediately reconnect
```

Tests connection table cleanup under load.

### RPC Encoding (Hand-Rolled)

Minimal RPC implementation — just enough to send/receive NFS ops:

```c
/* RPC record marking: 4-byte header, bit 31 = last fragment */
struct rpc_record_header {
    uint32_t rm;  /* (1 << 31) | length */
};

/* RPC call header */
struct rpc_call {
    uint32_t xid;
    uint32_t msg_type;      /* 0 = CALL */
    uint32_t rpc_vers;      /* 2 */
    uint32_t prog;          /* 100003 = NFS */
    uint32_t vers;          /* 3 or 4 */
    uint32_t proc;          /* FSINFO=19 (v3), COMPOUND=1 (v4) */
    /* auth + verifier follow */
};
```

AUTH_TLS: `flavor=7, body_len=0, verifier=AUTH_NONE`
AUTH_SYS: `flavor=1, body={stamp, machine, uid, gid, gids[]}`

NFSv4.2 session setup (required — product rejects v4.0):
1. COMPOUND: EXCHANGE_ID (establish client identity)
2. COMPOUND: CREATE_SESSION (establish session, get slot)
3. COMPOUND: SEQUENCE + PUTROOTFH + GETATTR (the actual probe)
   GETATTR of root avoids all I/O — pure metadata.

NFSv3 FSINFO/NULL: stretch goal, not initial priority.

### CLI Interface

```
nfs_tls_stress --host 10.0.0.1 --port 2049 \
    --proto nfsv3 \
    --mode starttls-loop \
    --iterations 1000 \
    --cert /path/to/client.pem \
    --key /path/to/client.key \
    --ca /path/to/ca.pem \
    --direct-tls \
    --verbose

Options:
  --host HOST         Server address
  --port PORT         NFS port (default: 2049)
  --proto PROTO       nfsv3 | nfsv4 (default: nfsv3)
  --mode MODE         starttls-loop | mid-op-disconnect |
                      hot-reconnect | rapid-cycle | all
  --iterations N      Number of iterations (default: 100)
  --duration SECS     Max duration (default: unlimited)
  --cert FILE         Client certificate (optional)
  --key FILE          Client key (optional)
  --ca FILE           CA certificate for server verification
  --direct-tls        Use direct TLS instead of STARTTLS
  --no-verify         Skip server certificate verification
  --delay-ms MS       Delay between iterations (default: 0)
  --verbose           Print per-iteration results
  --quiet             Only print summary
```

### Build

```makefile
# Static build — single binary, no shared deps except libc
cc -static -o nfs_tls_stress \
    nfs_tls_stress.c \
    -lssl -lcrypto -lpthread -ldl

# Or with system OpenSSL (dynamic):
cc -o nfs_tls_stress nfs_tls_stress.c -lssl -lcrypto
```

### Output

```
nfs_tls_stress: starttls-loop mode, 1000 iterations
  [1/1000] connect=1.2ms starttls=3.4ms fsinfo=0.8ms total=5.4ms OK
  [2/1000] connect=1.1ms starttls=3.2ms fsinfo=0.7ms total=5.0ms OK
  ...
  [42/1000] connect=1.3ms starttls=FAIL(SSL_ERROR_SSL) reconnect...OK
  ...

Summary:
  Total:      1000 iterations in 5.2s
  Successes:  998 (99.8%)
  Failures:   2 (0.2%)
  Reconnects: 2
  Avg latency: connect=1.2ms tls=3.3ms op=0.8ms
  Max latency: connect=4.1ms tls=12.3ms op=2.1ms
```

### Implementation Order

1. Socket + RPC record marking (TCP connect, send/recv with framing)
2. RPC NULL call (AUTH_NONE) — simplest possible probe
3. AUTH_TLS NULL + TLS handshake (RFC 9289 STARTTLS)
4. NFSv4.2 EXCHANGE_ID + CREATE_SESSION (hand-rolled XDR)
5. NFSv4.2 COMPOUND: SEQUENCE + PUTROOTFH + GETATTR
6. Test mode: starttls-loop
7. Test mode: mid-op-disconnect
8. Test mode: hot-reconnect
9. Test mode: rapid-cycle
10. Test mode: session-resume (TLS session tickets)
11. CLI parsing + summary stats
12. Stretch: NFSv3 FSINFO/NULL variant

### File

`tools/nfs_tls_stress.c` — single file, ~1500 lines.
