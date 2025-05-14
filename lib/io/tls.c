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

#ifdef NOT_NOW
static int verify_callback(int preverify_ok, X509_STORE_CTX *ctx)
{
	int err = X509_STORE_CTX_get_error(ctx);
	int depth = X509_STORE_CTX_get_error_depth(ctx);
	X509 *cert = X509_STORE_CTX_get_current_cert(ctx);

	char subject[256];
	X509_NAME_oneline(X509_get_subject_name(cert), subject,
			  sizeof(subject));

	LOG("Certificate verification: preverify=%d depth=%d subject=%s",
	    preverify_ok, depth, subject);

	if (!preverify_ok) {
		LOG("Certificate verification error: %d (%s)", err,
		    X509_verify_cert_error_string(err));

		// Always return 1 to accept all certificates
		return 1;
	}

	return 1; // Always accept
}

// In tls.c - io_tls_alpn_select_cb
static int io_tls_alpn_select_cb(SSL __attribute__((unused)) * ssl,
				 const unsigned char **out,
				 unsigned char *outlen, const unsigned char *in,
				 unsigned int inlen,
				 void __attribute__((unused)) * arg)
{
	LOG("ALPN callback invoked with offered protocols:");

	// No ALPN offered - accept connection anyway
	if (inlen == 0) {
		LOG("No ALPN extension present - accepting connection regardless");
		*out = (const unsigned char *)"sunrpc";
		*outlen = 6;
		return SSL_TLSEXT_ERR_OK;
	}

	// Normal ALPN processing
	for (unsigned int i = 0; i < inlen;) {
		unsigned int len = in[i];
		LOG("  Offered: %.*s", len, &in[i + 1]);
		i += len + 1;
	}

	const unsigned char alpn_proto[] = "\x06sunrpc";
	if (SSL_select_next_proto((unsigned char **)out, outlen, alpn_proto,
				  sizeof(alpn_proto) - 1, in,
				  inlen) != OPENSSL_NPN_NEGOTIATED) {
		// Changed behavior - accept even if sunrpc not offered
		LOG("ALPN sunrpc not offered, accepting anyway");
		*out = (const unsigned char *)"sunrpc";
		*outlen = 6;
		return SSL_TLSEXT_ERR_OK;
	}

	LOG("ALPN sunrpc negotiated");
	return SSL_TLSEXT_ERR_OK;
}
#endif

static void ssl_info_callback(const SSL __attribute__((unused)) * ssl, int type,
			      int val)
{
	if (type & SSL_CB_ALERT) {
		LOG("SSL ALERT: %s:%s", SSL_alert_type_string_long(val),
		    SSL_alert_desc_string_long(val));
	} else if (type & SSL_CB_HANDSHAKE_START) {
		LOG("SSL HANDSHAKE START");
	} else if (type & SSL_CB_HANDSHAKE_DONE) {
		LOG("SSL HANDSHAKE DONE");
	}
}

int io_tls_init_server_context(void)
{
	if (reffs_server_ssl_ctx != NULL)
		return 0; // Already initialized

	ERR_clear_error();
	unsigned long err;
	while ((err = ERR_get_error()) != 0) {
		char err_buf[256];
		ERR_error_string_n(err, err_buf, sizeof(err_buf));
		LOG("OpenSSL error at startup: %s", err_buf);
	}

	LOG("OpenSSL version: %s", OpenSSL_version(OPENSSL_VERSION));
	LOG("OpenSSL TLS method: %p", (void *)TLS_server_method());

	SSL_load_error_strings();
	ERR_load_crypto_strings();

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

	SSL_CTX_set_info_callback(reffs_server_ssl_ctx, ssl_info_callback);

	SSL_CTX_set_session_cache_mode(reffs_server_ssl_ctx,
				       SSL_SESS_CACHE_SERVER);
	SSL_CTX_set_session_id_context(reffs_server_ssl_ctx,
				       (const unsigned char *)"reffs", 5);

	SSL_CTX_set_verify(reffs_server_ssl_ctx, SSL_VERIFY_NONE, NULL);

	SSL_CTX_clear_options(reffs_server_ssl_ctx, SSL_OP_ALL);
	SSL_CTX_clear_options(reffs_server_ssl_ctx, SSL_OP_NO_RENEGOTIATION);

	SSL_CTX_set_options(reffs_server_ssl_ctx, SSL_OP_LEGACY_SERVER_CONNECT);
	SSL_CTX_set_options(
		reffs_server_ssl_ctx,
		SSL_OP_NO_TICKET |
			SSL_OP_NO_SESSION_RESUMPTION_ON_RENEGOTIATION |
			SSL_OP_NO_RENEGOTIATION | SSL_OP_NO_COMPRESSION);

	SSL_CTX_set_max_send_fragment(reffs_server_ssl_ctx, 16384);

	SSL_CTX_set_verify_depth(reffs_server_ssl_ctx, 4);

#ifdef TLS_DEBUGGING
	// Log the verification settings
	LOG("TLS verify mode: %d, depth: %d",
	    SSL_CTX_get_verify_mode(reffs_server_ssl_ctx),
	    SSL_CTX_get_verify_depth(reffs_server_ssl_ctx));
#endif

	const char *min_tls = getenv("REFFS_MIN_TLS_VERSION");
	if (min_tls) {
		if (strcmp(min_tls, "1.0") == 0) {
			LOG("Setting minimum TLS version to 1.0");
			SSL_CTX_set_min_proto_version(reffs_server_ssl_ctx,
						      TLS1_VERSION);
		} else if (strcmp(min_tls, "1.1") == 0) {
			LOG("Setting minimum TLS version to 1.1");
			SSL_CTX_set_min_proto_version(reffs_server_ssl_ctx,
						      TLS1_1_VERSION);
		} else if (strcmp(min_tls, "1.2") == 0) {
			LOG("Setting minimum TLS version to 1.2");
			SSL_CTX_set_min_proto_version(reffs_server_ssl_ctx,
						      TLS1_2_VERSION);
		} else {
			LOG("Setting minimum TLS version to 1.3");
			SSL_CTX_set_min_proto_version(reffs_server_ssl_ctx,
						      TLS1_3_VERSION);
		}
	} else {
		SSL_CTX_set_min_proto_version(reffs_server_ssl_ctx,
					      TLS1_3_VERSION);
		SSL_CTX_set_max_proto_version(reffs_server_ssl_ctx,
					      TLS1_3_VERSION);
	}

#ifdef TLS_DEBUGGING
	const char *ciphers = "HIGH:!aNULL:!MD5:!RC4";
	if (SSL_CTX_set_cipher_list(reffs_server_ssl_ctx, ciphers) == 0) {
		LOG("Error setting cipher list");
		SSL_CTX_free(reffs_server_ssl_ctx);
		reffs_server_ssl_ctx = NULL;
		return EINVAL;
	}

	LOG("Selected ciphers: %s", ciphers);
#endif

	const char *cert_path = getenv("REFFS_CERT_PATH");
	if (!cert_path)
		cert_path = "./.rpc-tls-certs/cert.pem";

	const char *key_path = getenv("REFFS_KEY_PATH");
	if (!key_path)
		key_path = "./.rpc-tls-certs/certkey.pem";

	// Load certificates and private key
	if (SSL_CTX_use_certificate_file(reffs_server_ssl_ctx, cert_path,
					 SSL_FILETYPE_PEM) <= 0) {
		LOG("Error loading certificate file");
		SSL_CTX_free(reffs_server_ssl_ctx);
		reffs_server_ssl_ctx = NULL;
		return EINVAL;
	}

	if (SSL_CTX_use_PrivateKey_file(reffs_server_ssl_ctx, key_path,
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

	// Set ALPN for RPC-with-TLS (RFC 9289)
	const unsigned char alpn_protos[] = {
		6, 's', 'u', 'n', 'r', 'p', 'c' // 6 is the length of "sunrpc"
	};

#ifdef NOT_NOW
	const char *require_alpn = getenv("REFFS_REQUIRE_ALPN");
	bool strict_alpn = true;

	if (require_alpn && strcasecmp(require_alpn, "false") == 0) {
		strict_alpn = false;
		LOG("ALPN requirement disabled - will accept connections without ALPN");
	}

	// Set ALPN only if strict mode is enabled
	if (strict_alpn) {
		LOG("Installing ALPN callback for sunrpc");
		SSL_CTX_set_alpn_protos(reffs_server_ssl_ctx, alpn_protos,
					sizeof(alpn_protos));
		SSL_CTX_set_alpn_select_cb(reffs_server_ssl_ctx,
					   io_tls_alpn_select_cb, NULL);
	}
#else
	LOG("Installing ALPN support for sunrpc (non-strict mode)");
	SSL_CTX_set_alpn_protos(reffs_server_ssl_ctx, alpn_protos,
				sizeof(alpn_protos));

#endif

	LOG("Server TLS context initialized successfully");
	return 0;
}
