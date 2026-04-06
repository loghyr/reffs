/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * TLS client library for NFS tools.
 *
 * Provides STARTTLS (RFC 9289), direct TLS, RPC framing over TLS,
 * and TLS handshake tracing.  No TIRPC dependency -- hand-rolled
 * RPC record marking for standalone tools.
 */

#ifndef _REFFS_TLS_CLIENT_H
#define _REFFS_TLS_CLIENT_H

#include <openssl/ssl.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* SSL_CTX creation                                                    */
/* ------------------------------------------------------------------ */

struct tls_client_config {
	const char *cert_path; /* client certificate (mutual TLS), or NULL */
	const char *key_path; /* client private key, or NULL */
	const char *ca_path; /* CA certificate for server verification, or NULL */
	int no_verify; /* skip server certificate verification */
};

/*
 * Create a client SSL_CTX configured for NFS-over-TLS:
 *   - ALPN "sunrpc" per RFC 9289
 *   - Optional client certificate + key (mutual TLS)
 *   - Optional CA for server verification
 *
 * Returns SSL_CTX* on success, NULL on failure (logs to stderr).
 * Caller must SSL_CTX_free() when done.
 */
SSL_CTX *tls_client_ctx_create(const struct tls_client_config *cfg);

/* ------------------------------------------------------------------ */
/* Connection establishment                                            */
/* ------------------------------------------------------------------ */

/*
 * Perform RFC 9289 STARTTLS upgrade on a cleartext TCP socket:
 *   1. Send AUTH_TLS NULL RPC probe (cleartext)
 *   2. Receive and validate STARTTLS reply
 *   3. SSL_connect to upgrade to TLS
 *
 * Returns SSL* on success, NULL on failure.
 * Caller must SSL_free() when done (before closing fd).
 */
SSL *tls_starttls(int fd, SSL_CTX *ctx, int verbose);

/*
 * Direct TLS connection -- SSL_connect immediately on the TCP socket.
 * No STARTTLS negotiation.  Used when the server expects TLS from
 * the start (like HTTPS), or for "hot reconnect" testing.
 *
 * Returns SSL* on success, NULL on failure.
 */
SSL *tls_direct_connect(int fd, SSL_CTX *ctx, int verbose);

/* ------------------------------------------------------------------ */
/* RPC framing over TLS or cleartext                                   */
/* ------------------------------------------------------------------ */

/*
 * Send an RPC message with record marking.
 * body points to the XDR-encoded RPC body (starting from xid).
 * If ssl is non-NULL, sends over TLS; otherwise cleartext.
 *
 * Returns 0 on success, -1 on error.
 */
int tls_rpc_send(SSL *ssl, int fd, const uint8_t *body, size_t body_len);

/*
 * Receive a complete RPC reply (record-marked, single fragment).
 * Multi-fragment replies are not supported.
 * If ssl is non-NULL, receives over TLS; otherwise cleartext.
 *
 * Returns bytes read (excluding record mark), or -1 on error.
 */
ssize_t tls_rpc_recv(SSL *ssl, int fd, uint8_t *buf, size_t bufsz);

/* ------------------------------------------------------------------ */
/* TLS handshake tracing                                               */
/* ------------------------------------------------------------------ */

/*
 * Print TLS handshake details for a connection:
 *   TLS version, cipher suite, ALPN result, session reuse.
 *
 * label identifies the connection context (e.g., "starttls", "direct").
 */
void tls_trace_handshake(SSL *ssl, const char *label);

/* ------------------------------------------------------------------ */
/* TCP helpers                                                         */
/* ------------------------------------------------------------------ */

/*
 * Connect a TCP socket to host:port with a 5-second timeout.
 * Returns fd on success, -1 on error.
 */
int tls_tcp_connect(const char *host, int port);

/*
 * Close a TCP socket with RST (no FIN/ACK) via SO_LINGER=0.
 * Simulates a client crash or network failure.
 */
void tls_tcp_reset(int fd);

#endif /* _REFFS_TLS_CLIENT_H */
