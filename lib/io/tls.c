/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <errno.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <stdbool.h>

#include "reffs/log.h"
#include "reffs/network.h"
#include "reffs/test.h"
#include "reffs/io.h"

SSL_CTX *reffs_server_ssl_ctx = NULL;

// ALPN selection callback
static int io_tls_alpn_select_cb(SSL __attribute__((unused)) * ssl,
				 const unsigned char **out,
				 unsigned char *outlen, const unsigned char *in,
				 unsigned int inlen,
				 void __attribute__((unused)) * arg)
{
	const unsigned char *proto = NULL;
	unsigned char proto_len = 0;

	// Look for "sunrpc" in the client's ALPN list
	if (SSL_select_next_proto((unsigned char **)&proto, &proto_len,
				  (const unsigned char *)"\x06sunrpc", 7, in,
				  inlen) != OPENSSL_NPN_NEGOTIATED) {
		return SSL_TLSEXT_ERR_NOACK;
	}

	*out = proto;
	*outlen = proto_len;
	return SSL_TLSEXT_ERR_OK;
}

int io_tls_init_server_context(void)
{
	if (reffs_server_ssl_ctx != NULL)
		return 0; // Already initialized

	// Initialize OpenSSL
	SSL_library_init();
	OpenSSL_add_all_algorithms();
	SSL_load_error_strings();

	// Create a new SSL context
	reffs_server_ssl_ctx = SSL_CTX_new(TLS_server_method());
	if (!reffs_server_ssl_ctx) {
		LOG("Error creating SSL context");
		return EINVAL;
	}

	// Load certificates and private key
	if (SSL_CTX_use_certificate_file(reffs_server_ssl_ctx,
					 "/etc/rpc.tlsservd/cert.pem",
					 SSL_FILETYPE_PEM) <= 0) {
		LOG("Error loading certificate file");
		SSL_CTX_free(reffs_server_ssl_ctx);
		reffs_server_ssl_ctx = NULL;
		return EINVAL;
	}

	if (SSL_CTX_use_PrivateKey_file(reffs_server_ssl_ctx,
					"/etc/rpc.tlsservd/certkey.pem",
					SSL_FILETYPE_PEM) <= 0) {
		LOG("Error loading private key file");
		SSL_CTX_free(reffs_server_ssl_ctx);
		reffs_server_ssl_ctx = NULL;
		return EINVAL;
	}

	// Check key and certificate compatibility
	if (!SSL_CTX_check_private_key(reffs_server_ssl_ctx)) {
		LOG("Private key and certificate do not match");
		SSL_CTX_free(reffs_server_ssl_ctx);
		reffs_server_ssl_ctx = NULL;
		return EINVAL;
	}

#ifdef NOT_NOW_BROWN_COW
	// Set ALPN for RPC-with-TLS (RFC 9289)
	const unsigned char alpn_protos[] = {
		6, 's', 'u', 'n', 'r', 'p', 'c' // 6 is the length of "sunrpc"
	};
#endif

	SSL_CTX_set_alpn_select_cb(reffs_server_ssl_ctx,
				       io_tls_alpn_select_cb, NULL);

	LOG("Server TLS context initialized successfully");
	return 0;
}

ssize_t io_tls_read(int fd, void *buf, size_t count)
{
	struct conn_info *ci = io_conn_get(fd);
	if (!ci || !ci->ci_ssl)
		return read(fd, buf, count); // Non-TLS connection

	return SSL_read(ci->ci_ssl, buf, count);
}

ssize_t io_tls_write(int fd, const void *buf, size_t count)
{
	struct conn_info *ci = io_conn_get(fd);
	if (!ci || !ci->ci_ssl)
		return write(fd, buf, count); // Non-TLS connection

	return SSL_write(ci->ci_ssl, buf, count);
}
