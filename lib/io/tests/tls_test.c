/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * TLS unit tests.
 *
 * Tests the TLS server context initialization, certificate loading,
 * ALPN negotiation, and the tls_available() flag behavior.  Uses a
 * mini-CA fixture that generates ephemeral self-signed certs in setUp.
 *
 * RFC reference: RFC 9289 (RPC-over-TLS), ALPN protocol "sunrpc".
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <check.h>

#include "reffs/log.h"
#include "reffs/tls.h"

/* ------------------------------------------------------------------ */
/* Mini-CA fixture: ephemeral self-signed certs                       */
/* ------------------------------------------------------------------ */

static char tls_dir[] = "/tmp/reffs-tls-test-XXXXXX";
static char cert_path[PATH_MAX];
static char key_path[PATH_MAX];

/*
 * Generate a self-signed cert + key pair using the OpenSSL API.
 * No shell commands, no temporary processes.
 */
static int generate_self_signed(const char *cert_file, const char *key_file)
{
	EVP_PKEY *pkey = NULL;
	X509 *x509 = NULL;
	EVP_PKEY_CTX *pctx = NULL;
	FILE *fp = NULL;
	int ret = -1;

	/* Generate RSA 2048-bit key */
	pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
	if (!pctx)
		goto out;
	if (EVP_PKEY_keygen_init(pctx) <= 0)
		goto out;
	if (EVP_PKEY_CTX_set_rsa_keygen_bits(pctx, 2048) <= 0)
		goto out;
	if (EVP_PKEY_keygen(pctx, &pkey) <= 0)
		goto out;

	/* Create self-signed X509 certificate */
	x509 = X509_new();
	if (!x509)
		goto out;

	ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
	X509_gmtime_adj(X509_get_notBefore(x509), 0);
	X509_gmtime_adj(X509_get_notAfter(x509), 86400); /* 1 day */

	X509_set_pubkey(x509, pkey);

	X509_NAME *name = X509_get_subject_name(x509);
	X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
				   (const unsigned char *)"reffs-test", -1, -1,
				   0);
	X509_set_issuer_name(x509, name); /* self-signed */

	if (X509_sign(x509, pkey, EVP_sha256()) <= 0)
		goto out;

	/* Write key to file */
	fp = fopen(key_file, "w");
	if (!fp)
		goto out;
	if (!PEM_write_PrivateKey(fp, pkey, NULL, NULL, 0, NULL, NULL))
		goto out_close;
	fclose(fp);
	fp = NULL;

	/* Write cert to file */
	fp = fopen(cert_file, "w");
	if (!fp)
		goto out;
	if (!PEM_write_X509(fp, x509))
		goto out_close;
	fclose(fp);
	fp = NULL;

	ret = 0;
	goto out;

out_close:
	if (fp)
		fclose(fp);
out:
	X509_free(x509);
	EVP_PKEY_free(pkey);
	EVP_PKEY_CTX_free(pctx);
	return ret;
}

static void tls_setup(void)
{
	ck_assert_ptr_nonnull(mkdtemp(tls_dir));

	snprintf(cert_path, sizeof(cert_path), "%s/server.pem", tls_dir);
	snprintf(key_path, sizeof(key_path), "%s/server.key", tls_dir);

	ck_assert_int_eq(generate_self_signed(cert_path, key_path), 0);

	/*
	 * Reset the global SSL context so each test starts clean.
	 * io_tls_init_server_context is idempotent -- it checks if
	 * reffs_server_ssl_ctx is already set.  We need to force
	 * re-initialization by clearing it.
	 */
	if (reffs_server_ssl_ctx) {
		SSL_CTX_free(reffs_server_ssl_ctx);
		reffs_server_ssl_ctx = NULL;
	}
}

static void tls_teardown(void)
{
	if (reffs_server_ssl_ctx) {
		SSL_CTX_free(reffs_server_ssl_ctx);
		reffs_server_ssl_ctx = NULL;
	}

	char cmd[512];
	snprintf(cmd, sizeof(cmd), "rm -rf %s", tls_dir);
	system(cmd);
	strcpy(tls_dir, "/tmp/reffs-tls-test-XXXXXX");
}

/* ------------------------------------------------------------------ */
/* Context initialization tests                                        */
/* ------------------------------------------------------------------ */

START_TEST(test_tls_ctx_init_valid_certs)
{
	int ret = io_tls_init_server_context(cert_path, key_path, NULL);
	ck_assert_int_eq(ret, 0);
	ck_assert_ptr_nonnull(reffs_server_ssl_ctx);
}
END_TEST

START_TEST(test_tls_ctx_init_missing_cert)
{
	int ret = io_tls_init_server_context("/nonexistent/cert.pem", key_path,
					     NULL);
	ck_assert_int_ne(ret, 0);
}
END_TEST

START_TEST(test_tls_ctx_init_missing_key)
{
	int ret = io_tls_init_server_context(cert_path, "/nonexistent/key.pem",
					     NULL);
	ck_assert_int_ne(ret, 0);
}
END_TEST

START_TEST(test_tls_ctx_init_idempotent)
{
	/* First init succeeds */
	int ret = io_tls_init_server_context(cert_path, key_path, NULL);
	ck_assert_int_eq(ret, 0);
	SSL_CTX *first = reffs_server_ssl_ctx;
	ck_assert_ptr_nonnull(first);

	/* Second init returns success without changing the context */
	ret = io_tls_init_server_context(cert_path, key_path, NULL);
	ck_assert_int_eq(ret, 0);
	ck_assert_ptr_eq(reffs_server_ssl_ctx, first);
}
END_TEST

/* ------------------------------------------------------------------ */
/* ALPN tests                                                          */
/* ------------------------------------------------------------------ */

START_TEST(test_tls_alpn_sunrpc)
{
	/*
	 * After context init, verify that the ALPN protocol is set
	 * to "sunrpc" per RFC 9289.  We check by creating an SSL
	 * object and inspecting its ALPN settings.
	 */
	int ret = io_tls_init_server_context(cert_path, key_path, NULL);
	ck_assert_int_eq(ret, 0);

	SSL *ssl = SSL_new(reffs_server_ssl_ctx);
	ck_assert_ptr_nonnull(ssl);

	/*
	 * The server sets ALPN via SSL_CTX_set_alpn_protos which
	 * stores the protocol list.  We can't directly inspect it,
	 * but we can verify the context was created successfully
	 * and the SSL object is usable.
	 */
	ck_assert_ptr_nonnull(SSL_get_SSL_CTX(ssl));

	SSL_free(ssl);
}
END_TEST

/*
 * Helper: drive a BIO-based TLS handshake to completion.
 * Returns true if the handshake completed within 20 rounds.
 */
static bool do_loopback_handshake(SSL *server_ssl, SSL *client_ssl)
{
	char buf[16384];
	BIO *s_wbio = SSL_get_wbio(server_ssl);
	BIO *s_rbio = SSL_get_rbio(server_ssl);
	BIO *c_wbio = SSL_get_wbio(client_ssl);
	BIO *c_rbio = SSL_get_rbio(client_ssl);

	for (int rounds = 0; rounds < 20; rounds++) {
		SSL_do_handshake(client_ssl);
		int n = BIO_read(c_wbio, buf, sizeof(buf));
		if (n > 0)
			BIO_write(s_rbio, buf, n);

		SSL_do_handshake(server_ssl);
		n = BIO_read(s_wbio, buf, sizeof(buf));
		if (n > 0)
			BIO_write(c_rbio, buf, n);

		if (SSL_is_init_finished(server_ssl) &&
		    SSL_is_init_finished(client_ssl))
			return true;
	}
	return false;
}

START_TEST(test_tls_alpn_negotiated)
{
	/*
	 * Client offers "sunrpc"; server callback must select it.
	 * RFC 9289 S3: both ends should negotiate "sunrpc".
	 */
	int ret = io_tls_init_server_context(cert_path, key_path, NULL);
	ck_assert_int_eq(ret, 0);

	SSL *server_ssl = SSL_new(reffs_server_ssl_ctx);
	ck_assert_ptr_nonnull(server_ssl);
	SSL_set_accept_state(server_ssl);
	BIO *s_rbio = BIO_new(BIO_s_mem());
	BIO *s_wbio = BIO_new(BIO_s_mem());
	SSL_set_bio(server_ssl, s_rbio, s_wbio);

	SSL_CTX *client_ctx = SSL_CTX_new(TLS_client_method());
	ck_assert_ptr_nonnull(client_ctx);
	SSL_CTX_set_verify(client_ctx, SSL_VERIFY_NONE, NULL);
	/* Offer "sunrpc" as the only ALPN protocol. */
	const unsigned char sunrpc_alpn[] = { 6, 's', 'u', 'n', 'r', 'p', 'c' };
	SSL_CTX_set_alpn_protos(client_ctx, sunrpc_alpn, sizeof(sunrpc_alpn));

	SSL *client_ssl = SSL_new(client_ctx);
	ck_assert_ptr_nonnull(client_ssl);
	SSL_set_connect_state(client_ssl);
	BIO *c_rbio = BIO_new(BIO_s_mem());
	BIO *c_wbio = BIO_new(BIO_s_mem());
	SSL_set_bio(client_ssl, c_rbio, c_wbio);

	ck_assert_msg(do_loopback_handshake(server_ssl, client_ssl),
		      "handshake did not complete");

	const unsigned char *alpn_data;
	unsigned int alpn_len;
	SSL_get0_alpn_selected(server_ssl, &alpn_data, &alpn_len);
	ck_assert_uint_eq(alpn_len, 6);
	ck_assert_mem_eq(alpn_data, "sunrpc", 6);

	SSL_get0_alpn_selected(client_ssl, &alpn_data, &alpn_len);
	ck_assert_uint_eq(alpn_len, 6);
	ck_assert_mem_eq(alpn_data, "sunrpc", 6);

	SSL_free(server_ssl);
	SSL_free(client_ssl);
	SSL_CTX_free(client_ctx);
}
END_TEST

START_TEST(test_tls_alpn_not_offered)
{
	/*
	 * Client offers no ALPN at all; server callback must still
	 * accept the connection (RFC 9289 S4.1.1 permits this for
	 * compatibility with clients that do not support ALPN).
	 */
	int ret = io_tls_init_server_context(cert_path, key_path, NULL);
	ck_assert_int_eq(ret, 0);

	SSL *server_ssl = SSL_new(reffs_server_ssl_ctx);
	ck_assert_ptr_nonnull(server_ssl);
	SSL_set_accept_state(server_ssl);
	BIO *s_rbio = BIO_new(BIO_s_mem());
	BIO *s_wbio = BIO_new(BIO_s_mem());
	SSL_set_bio(server_ssl, s_rbio, s_wbio);

	SSL_CTX *client_ctx = SSL_CTX_new(TLS_client_method());
	ck_assert_ptr_nonnull(client_ctx);
	SSL_CTX_set_verify(client_ctx, SSL_VERIFY_NONE, NULL);
	/* Deliberately offer no ALPN. */

	SSL *client_ssl = SSL_new(client_ctx);
	ck_assert_ptr_nonnull(client_ssl);
	SSL_set_connect_state(client_ssl);
	BIO *c_rbio = BIO_new(BIO_s_mem());
	BIO *c_wbio = BIO_new(BIO_s_mem());
	SSL_set_bio(client_ssl, c_rbio, c_wbio);

	ck_assert_msg(do_loopback_handshake(server_ssl, client_ssl),
		      "handshake should complete even without client ALPN");

	SSL_free(server_ssl);
	SSL_free(client_ssl);
	SSL_CTX_free(client_ctx);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Handshake tests (loopback with memory BIOs)                        */
/* ------------------------------------------------------------------ */

START_TEST(test_tls_handshake_loopback)
{
	/*
	 * Create a server SSL and a client SSL connected via memory
	 * BIOs, perform the handshake, and verify it completes.
	 */
	int ret = io_tls_init_server_context(cert_path, key_path, NULL);
	ck_assert_int_eq(ret, 0);

	/* Server side */
	SSL *server_ssl = SSL_new(reffs_server_ssl_ctx);
	ck_assert_ptr_nonnull(server_ssl);
	SSL_set_accept_state(server_ssl);

	BIO *s_rbio = BIO_new(BIO_s_mem());
	BIO *s_wbio = BIO_new(BIO_s_mem());
	SSL_set_bio(server_ssl, s_rbio, s_wbio);

	/* Client side -- create a separate client context */
	SSL_CTX *client_ctx = SSL_CTX_new(TLS_client_method());
	ck_assert_ptr_nonnull(client_ctx);
	/* Don't verify server cert (self-signed) */
	SSL_CTX_set_verify(client_ctx, SSL_VERIFY_NONE, NULL);

	SSL *client_ssl = SSL_new(client_ctx);
	ck_assert_ptr_nonnull(client_ssl);
	SSL_set_connect_state(client_ssl);

	BIO *c_rbio = BIO_new(BIO_s_mem());
	BIO *c_wbio = BIO_new(BIO_s_mem());
	SSL_set_bio(client_ssl, c_rbio, c_wbio);

	/*
	 * Drive the handshake by shuttling data between the
	 * client and server memory BIOs.
	 */
	int rounds = 0;
	bool done = false;
	while (!done && rounds < 20) {
		rounds++;

		/* Client step */
		int cr = SSL_do_handshake(client_ssl);
		/* Shuttle client output --> server input */
		char buf[16384];
		int pending = BIO_pending(c_wbio);
		if (pending > 0) {
			int n = BIO_read(c_wbio, buf, sizeof(buf));
			if (n > 0)
				BIO_write(s_rbio, buf, n);
		}

		/* Server step */
		int sr = SSL_do_handshake(server_ssl);
		/* Shuttle server output --> client input */
		pending = BIO_pending(s_wbio);
		if (pending > 0) {
			int n = BIO_read(s_wbio, buf, sizeof(buf));
			if (n > 0)
				BIO_write(c_rbio, buf, n);
		}

		if (cr == 1 && sr == 1)
			done = true;
	}

	ck_assert_msg(done, "TLS handshake did not complete in %d rounds",
		      rounds);
	ck_assert(SSL_is_init_finished(server_ssl));
	ck_assert(SSL_is_init_finished(client_ssl));

	SSL_free(server_ssl);
	SSL_free(client_ssl);
	SSL_CTX_free(client_ctx);
}
END_TEST

START_TEST(test_tls_data_roundtrip)
{
	/*
	 * After handshake, send data from client --> server and back.
	 */
	int ret = io_tls_init_server_context(cert_path, key_path, NULL);
	ck_assert_int_eq(ret, 0);

	/* Set up server */
	SSL *server_ssl = SSL_new(reffs_server_ssl_ctx);
	SSL_set_accept_state(server_ssl);
	BIO *s_rbio = BIO_new(BIO_s_mem());
	BIO *s_wbio = BIO_new(BIO_s_mem());
	SSL_set_bio(server_ssl, s_rbio, s_wbio);

	/* Set up client */
	SSL_CTX *client_ctx = SSL_CTX_new(TLS_client_method());
	SSL_CTX_set_verify(client_ctx, SSL_VERIFY_NONE, NULL);
	SSL *client_ssl = SSL_new(client_ctx);
	SSL_set_connect_state(client_ssl);
	BIO *c_rbio = BIO_new(BIO_s_mem());
	BIO *c_wbio = BIO_new(BIO_s_mem());
	SSL_set_bio(client_ssl, c_rbio, c_wbio);

	/* Complete handshake */
	char buf[16384];
	for (int i = 0; i < 20; i++) {
		SSL_do_handshake(client_ssl);
		int n = BIO_read(c_wbio, buf, sizeof(buf));
		if (n > 0)
			BIO_write(s_rbio, buf, n);
		SSL_do_handshake(server_ssl);
		n = BIO_read(s_wbio, buf, sizeof(buf));
		if (n > 0)
			BIO_write(c_rbio, buf, n);
		if (SSL_is_init_finished(client_ssl) &&
		    SSL_is_init_finished(server_ssl))
			break;
	}
	ck_assert(SSL_is_init_finished(client_ssl));

	/* Client writes data */
	const char *msg = "RPC over TLS test payload";
	int msg_len = strlen(msg);
	int written = SSL_write(client_ssl, msg, msg_len);
	ck_assert_int_eq(written, msg_len);

	/* Shuttle encrypted data to server */
	int n = BIO_read(c_wbio, buf, sizeof(buf));
	ck_assert_int_gt(n, 0);
	BIO_write(s_rbio, buf, n);

	/* Server reads data */
	char read_buf[256] = { 0 };
	int nread = SSL_read(server_ssl, read_buf, sizeof(read_buf));
	ck_assert_int_eq(nread, msg_len);
	ck_assert_str_eq(read_buf, msg);

	SSL_free(server_ssl);
	SSL_free(client_ssl);
	SSL_CTX_free(client_ctx);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite                                                               */
/* ------------------------------------------------------------------ */

static Suite *tls_suite(void)
{
	Suite *s = suite_create("tls");
	TCase *tc;

	tc = tcase_create("context_init");
	tcase_add_checked_fixture(tc, tls_setup, tls_teardown);
	tcase_add_test(tc, test_tls_ctx_init_valid_certs);
	tcase_add_test(tc, test_tls_ctx_init_missing_cert);
	tcase_add_test(tc, test_tls_ctx_init_missing_key);
	tcase_add_test(tc, test_tls_ctx_init_idempotent);
	suite_add_tcase(s, tc);

	tc = tcase_create("alpn");
	tcase_add_checked_fixture(tc, tls_setup, tls_teardown);
	tcase_add_test(tc, test_tls_alpn_sunrpc);
	tcase_add_test(tc, test_tls_alpn_negotiated);
	tcase_add_test(tc, test_tls_alpn_not_offered);
	suite_add_tcase(s, tc);

	tc = tcase_create("handshake");
	tcase_add_checked_fixture(tc, tls_setup, tls_teardown);
	tcase_add_test(tc, test_tls_handshake_loopback);
	tcase_add_test(tc, test_tls_data_roundtrip);
	suite_add_tcase(s, tc);

	return s;
}

int main(void)
{
	int nfailed;
	Suite *s = tls_suite();
	SRunner *sr = srunner_create(s);

	srunner_run_all(sr, CK_NORMAL);
	nfailed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return nfailed ? 1 : 0;
}
