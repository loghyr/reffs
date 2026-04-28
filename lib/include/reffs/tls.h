/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_TLS_H
#define _REFFS_TLS_H

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <stdint.h>

#include "reffs/log.h" /* LOG() used in the inline helper below */

#define AUTH_TLS 7
#define STARTTLS_VERIFIER "STARTTLS"
#define REFFS_TLS_HANDSHAKE 22 // Content type for TLS handshake
#define REFFS_TLS_MAJOR_VERSION \
	3 // TLS version 1.0 or higher has major version 3

/*
 * Backwards-compat alias for the listener_id=0 (native) SSL_CTX.
 * New code should call io_tls_get_listener_context(listener_id)
 * so a combined role=mds + [[proxy_mds]] reffsd can run separate
 * server-side TLS postures per listener.  Slice plan-1-tls.c
 * documented the global-CTX limitation; this is the lift.
 */
extern SSL_CTX *reffs_server_ssl_ctx;

/*
 * Initialize TLS server context for listener_id=0 (native).  Path
 * arguments may be NULL; the function falls back to env vars
 * (REFFS_CERT_PATH, REFFS_KEY_PATH), then to /etc/tlshd/ defaults
 * (shared with kernel tlshd).
 */
int io_tls_init_server_context(const char *cert, const char *key,
			       const char *ca);

/*
 * Initialize TLS server context for an explicit listener_id.
 * listener_id 0 is the native NFS listener; 1..N are the
 * [[proxy_mds]] listeners.  Each listener gets its own SSL_CTX
 * with its own cert / key / CA so the server-side TLS posture is
 * per-listener rather than process-global.  Idempotent: a second
 * call with the same listener_id is a no-op.  Returns 0 on
 * success or -errno.
 */
int io_tls_init_listener_context(uint32_t listener_id, const char *cert,
				 const char *key, const char *ca);

/*
 * Look up the SSL_CTX for a listener.  Returns NULL when no
 * context has been initialised for that id (which is the normal
 * state for non-TLS listeners).
 */
SSL_CTX *io_tls_get_listener_context(uint32_t listener_id);

static inline void io_ssl_err_print(int fd, const char *msg, const char *func,
				    const int line)
{
	char err_buf[256];
	ERR_error_string_n(ERR_get_error(), err_buf, sizeof(err_buf));
	LOG("%s:%d: SSL error %s for fd=%d: %s", func, line, msg, fd, err_buf);
}

#endif /* _REFFS_TLS_H */
