/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_TLS_H
#define _REFFS_TLS_H

#include <openssl/ssl.h>
#include <openssl/err.h>

#include "reffs/log.h" /* LOG() used in the inline helper below */

#define AUTH_TLS 7
#define STARTTLS_VERIFIER "STARTTLS"
#define REFFS_TLS_HANDSHAKE 22 // Content type for TLS handshake
#define REFFS_TLS_MAJOR_VERSION \
	3 // TLS version 1.0 or higher has major version 3

extern SSL_CTX *reffs_server_ssl_ctx;

/*
 * Initialize TLS server context.  Path arguments may be NULL; the
 * function falls back to env vars (REFFS_CERT_PATH, REFFS_KEY_PATH),
 * then to /etc/tlshd/ defaults (shared with kernel tlshd).
 */
int io_tls_init_server_context(const char *cert, const char *key,
			       const char *ca);

static inline void io_ssl_err_print(int fd, const char *msg, const char *func,
				    const int line)
{
	char err_buf[256];
	ERR_error_string_n(ERR_get_error(), err_buf, sizeof(err_buf));
	LOG("%s:%d: SSL error %s for fd=%d: %s", func, line, msg, fd, err_buf);
}

#endif /* _REFFS_TLS_H */
