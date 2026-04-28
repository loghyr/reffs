/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <errno.h>
#include <stdbool.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/prov_ssl.h>
#include <openssl/ssl.h>
#include <openssl/types.h>
#include <stdlib.h>
#include <string.h>

#include "reffs/log.h"
#include "reffs/tls.h"

/*
 * Per-listener SSL_CTX registry.  Index 0 is the native NFS listener;
 * 1..REFFS_TLS_MAX_LISTENERS-1 are the [[proxy_mds]] listeners.
 * 16 slots covers REFFS_CONFIG_MAX_PROXY_MDS=8 with headroom; the
 * cost is 16 pointers (~128 bytes).
 *
 * Populated by io_tls_init_listener_context (and the back-compat
 * io_tls_init_server_context for slot 0).  Read by
 * io_tls_get_listener_context, which the accept path calls per-fd
 * via the conn_info ci_listener_id.
 *
 * Single-writer (server startup, before any worker thread runs);
 * multi-reader (every TLS-accept compares).  The registry is set
 * up before any TLS connection can fire -- no synchronization
 * needed beyond the implicit publish-before-accept happens-before.
 */
#define REFFS_TLS_MAX_LISTENERS 16
static SSL_CTX *listener_ctx[REFFS_TLS_MAX_LISTENERS];

/*
 * Back-compat alias.  Old call sites that used the global directly
 * (lib/io/handlers.c) now go through io_tls_get_listener_context;
 * this pointer mirrors slot 0 so external test fixtures that may
 * have read it before the refactor still see the native context.
 */
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

	TRACE("Certificate verification: preverify=%d depth=%d subject=%s",
	      preverify_ok, depth, subject);

	if (!preverify_ok) {
		TRACE("Certificate verification error: %d (%s)", err,
		      X509_verify_cert_error_string(err));

		// Always return 1 to accept all certificates
		return 1;
	}

	return 1; // Always accept
}
#endif /* NOT_NOW */

static int io_tls_alpn_select_cb(SSL __attribute__((unused)) * ssl,
				 const unsigned char **out,
				 unsigned char *outlen, const unsigned char *in,
				 unsigned int inlen,
				 void __attribute__((unused)) * arg)
{
	TRACE("ALPN callback invoked with offered protocols:");

	// No ALPN offered - accept connection anyway
	if (inlen == 0) {
		TRACE("No ALPN extension present - accepting connection regardless");
		*out = (const unsigned char *)"sunrpc";
		*outlen = 6;
		return SSL_TLSEXT_ERR_OK;
	}

	// Normal ALPN processing
	for (unsigned int i = 0; i < inlen;) {
		unsigned int len = in[i];
		TRACE("  Offered: %.*s", len, &in[i + 1]);
		i += len + 1;
	}

	static const unsigned char alpn_proto[] = "\x06sunrpc";
	if (SSL_select_next_proto((unsigned char **)out, outlen, alpn_proto,
				  sizeof(alpn_proto) - 1, in,
				  inlen) != OPENSSL_NPN_NEGOTIATED) {
		// Changed behavior - accept even if sunrpc not offered
		TRACE("ALPN sunrpc not offered, accepting anyway");
		*out = (const unsigned char *)"sunrpc";
		*outlen = 6;
		return SSL_TLSEXT_ERR_OK;
	}

	TRACE("ALPN sunrpc negotiated");
	return SSL_TLSEXT_ERR_OK;
}

static void ssl_info_callback(const SSL __attribute__((unused)) * ssl, int type,
			      int val)
{
	if (type & SSL_CB_ALERT) {
		TRACE("SSL ALERT: %s:%s", SSL_alert_type_string_long(val),
		      SSL_alert_desc_string_long(val));
	} else if (type & SSL_CB_HANDSHAKE_START) {
		TRACE("SSL HANDSHAKE START");
	} else if (type & SSL_CB_HANDSHAKE_DONE) {
		TRACE("SSL HANDSHAKE DONE");
	}
}

/*
 * Build a fresh SSL_CTX with the cert / key / CA the caller named.
 * Returns NULL on any failure (cert load, key load, mismatch, etc.).
 * Caller owns the returned context and must SSL_CTX_free it.
 *
 * Cert / key path priority: argument -> env (REFFS_CERT_PATH /
 * REFFS_KEY_PATH) -> /etc/tlshd/ defaults (shared with kernel
 * tlshd).  CA path is honoured verbatim when set; empty leaves
 * verify mode at SSL_VERIFY_NONE for backwards-compat.
 */
static SSL_CTX *build_ssl_ctx(const char *cfg_cert, const char *cfg_key,
			      const char *cfg_ca)
{
	ERR_clear_error();

	SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());

	if (!ctx) {
		TRACE("Error creating SSL context");
		return NULL;
	}

	SSL_CTX_set_info_callback(ctx, ssl_info_callback);
	SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_SERVER);
	SSL_CTX_set_session_id_context(ctx, (const unsigned char *)"reffs", 5);

	/*
	 * Slice plan-1-tls.c (#139): when the operator supplies a CA
	 * bundle, request and verify the peer's client cert so the
	 * per-connection peer-cert fingerprint becomes available to
	 * io_conn_get_peer_cert_fingerprint (used by the MDS
	 * PROXY_REGISTRATION allowlist check).  Without a CA bundle we
	 * keep the historical SSL_VERIFY_NONE so existing TLS-server-
	 * only deployments do not start rejecting clients.
	 */
	if (cfg_ca && cfg_ca[0] != '\0') {
		if (SSL_CTX_load_verify_locations(ctx, cfg_ca, NULL) != 1) {
			TRACE("io_tls: SSL_CTX_load_verify_locations(%s) failed",
			      cfg_ca);
			SSL_CTX_free(ctx);
			return NULL;
		}
		SSL_CTX_set_verify(
			ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
			NULL);
	} else {
		SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
	}

	SSL_CTX_clear_options(ctx, SSL_OP_ALL);
	SSL_CTX_clear_options(ctx, SSL_OP_NO_RENEGOTIATION);
	SSL_CTX_set_options(ctx, SSL_OP_LEGACY_SERVER_CONNECT);
	SSL_CTX_set_options(
		ctx, SSL_OP_NO_TICKET |
			     SSL_OP_NO_SESSION_RESUMPTION_ON_RENEGOTIATION |
			     SSL_OP_NO_RENEGOTIATION | SSL_OP_NO_COMPRESSION);

	SSL_CTX_set_max_send_fragment(ctx, 16384);
	SSL_CTX_set_verify_depth(ctx, 4);

	const char *min_tls = getenv("REFFS_MIN_TLS_VERSION");

	if (min_tls) {
		if (strcmp(min_tls, "1.0") == 0)
			SSL_CTX_set_min_proto_version(ctx, TLS1_VERSION);
		else if (strcmp(min_tls, "1.1") == 0)
			SSL_CTX_set_min_proto_version(ctx, TLS1_1_VERSION);
		else if (strcmp(min_tls, "1.2") == 0)
			SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
		else
			SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
	} else {
		SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
		SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);
	}

	const char *cert_path = cfg_cert && cfg_cert[0] ? cfg_cert : NULL;

	if (!cert_path)
		cert_path = getenv("REFFS_CERT_PATH");
	if (!cert_path)
		cert_path = "/etc/tlshd/server.pem";

	const char *key_path = cfg_key && cfg_key[0] ? cfg_key : NULL;

	if (!key_path)
		key_path = getenv("REFFS_KEY_PATH");
	if (!key_path)
		key_path = "/etc/tlshd/server.key";

	if (SSL_CTX_use_certificate_file(ctx, cert_path, SSL_FILETYPE_PEM) <=
	    0) {
		TRACE("Error loading certificate file %s", cert_path);
		SSL_CTX_free(ctx);
		return NULL;
	}
	if (SSL_CTX_use_PrivateKey_file(ctx, key_path, SSL_FILETYPE_PEM) <= 0) {
		TRACE("Error loading private key file %s", key_path);
		SSL_CTX_free(ctx);
		return NULL;
	}
	if (!SSL_CTX_check_private_key(ctx)) {
		TRACE("Private key and certificate do not match");
		SSL_CTX_free(ctx);
		return NULL;
	}

	/* RFC 9289 S3: server MUST select "sunrpc" via ALPN. */
	const unsigned char alpn_protos[] = { 6, 's', 'u', 'n', 'r', 'p', 'c' };

	SSL_CTX_set_alpn_protos(ctx, alpn_protos, sizeof(alpn_protos));
	SSL_CTX_set_alpn_select_cb(ctx, io_tls_alpn_select_cb, NULL);

	return ctx;
}

int io_tls_init_listener_context(uint32_t listener_id, const char *cfg_cert,
				 const char *cfg_key, const char *cfg_ca)
{
	if (listener_id >= REFFS_TLS_MAX_LISTENERS) {
		LOG("io_tls: listener_id %u exceeds REFFS_TLS_MAX_LISTENERS=%d",
		    listener_id, REFFS_TLS_MAX_LISTENERS);
		return -EINVAL;
	}

	if (listener_ctx[listener_id]) {
		/*
		 * Idempotent: a second call with the same listener_id
		 * (e.g. retry, double-init) is a no-op.  If the operator
		 * meant to swap configurations they need to tear down the
		 * listener and re-create.
		 */
		return 0;
	}

	/*
	 * One-time OpenSSL global init.  These are safe to call many
	 * times -- libssl ref-counts internally -- but we keep the
	 * trace output for the FIRST listener only so logs are not
	 * flooded on combined deployments.
	 */
	static bool ossl_inited;

	if (!ossl_inited) {
		TRACE("OpenSSL version: %s", OpenSSL_version(OPENSSL_VERSION));
		SSL_load_error_strings();
		ERR_load_crypto_strings();
		SSL_library_init();
		OpenSSL_add_all_algorithms();
		ossl_inited = true;
	}

	SSL_CTX *ctx = build_ssl_ctx(cfg_cert, cfg_key, cfg_ca);

	if (!ctx)
		return -EINVAL;

	listener_ctx[listener_id] = ctx;

	/*
	 * Mirror slot 0 onto the back-compat global so old call sites
	 * that read reffs_server_ssl_ctx directly (some tests, the
	 * pre-refactor accept-path) still see the native context.
	 */
	if (listener_id == 0)
		reffs_server_ssl_ctx = ctx;

	TRACE("Server TLS context for listener_id=%u initialised", listener_id);
	return 0;
}

int io_tls_init_server_context(const char *cfg_cert, const char *cfg_key,
			       const char *cfg_ca)
{
	int ret = io_tls_init_listener_context(0, cfg_cert, cfg_key, cfg_ca);

	/*
	 * Match the historical return shape: 0 on success, EINVAL on
	 * any failure.  Callers existed before the per-listener API
	 * and expect the bare positive errno.
	 */
	return (ret == 0) ? 0 : EINVAL;
}

SSL_CTX *io_tls_get_listener_context(uint32_t listener_id)
{
	if (listener_id >= REFFS_TLS_MAX_LISTENERS)
		return NULL;
	if (listener_ctx[listener_id])
		return listener_ctx[listener_id];
	/*
	 * Fall back to the native (slot 0) context.  Historical
	 * single-listener deployments brought TLS up via slot 0; a
	 * proxy listener that has not registered its own server-side
	 * TLS cert (today: every [[proxy_mds]] listener) keeps the
	 * pre-refactor behaviour of using the same context as the
	 * native listener.  The combined deployment that wants
	 * different posture per listener will register both slots
	 * explicitly and never reach this fallback.
	 */
	return listener_ctx[0];
}
