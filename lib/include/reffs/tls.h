/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifndef _REFFS_TLS_H
#define _REFFS_TLS_H

#include <openssl/ssl.h>
#include <openssl/err.h>

#define AUTH_TLS 7
#define STARTTLS_VERIFIER "STARTTLS"
#define REFFS_TLS_HANDSHAKE 22 // Content type for TLS handshake
#define REFFS_TLS_MAJOR_VERSION \
	3 // TLS version 1.0 or higher has major version 3

extern SSL_CTX *reffs_server_ssl_ctx;

int io_tls_init_server_context(void);

#endif /* _REFFS_TLS_H */
