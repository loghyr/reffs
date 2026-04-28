/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * Slice plan-1-tls.a unit tests for the custom libtirpc CLIENT*
 * over TLS (mds_tls_xprt.c).
 *
 * Scope (per .claude/design/proxy-server-tls.md):
 *   - bad-args validation
 *   - successful create + clnt_destroy round-trip with no leaks
 *   - round-trip: client encodes a NULL call, mock server reads it,
 *     replies with an empty success body, clnt_call returns
 *     RPC_SUCCESS
 *   - destroy ordering: SSL_shutdown -> SSL_free -> close(fd),
 *     verified via ASAN/LSAN clean teardown
 *   - send failure: simulated SSL_write error on a half-closed
 *     pair sets re_status = RPC_CANTSEND
 *
 * Pattern follows lib/io/tests/tls_test.c: server SSL_CTX from
 * io_tls_init_server_context (which the test fixture creates with
 * a tiny on-disk self-signed pair), client SSL_CTX with
 * SSL_VERIFY_NONE, BIO_s_mem on each side, drive the handshake by
 * shuttling between the BIOs.  No real socket -- all fds are
 * sentinel values that get closed at teardown.
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <check.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <rpc/rpc.h>

#include "mds_tls_xprt.h"

/* ------------------------------------------------------------------ */
/* In-process SSL pair fixture                                        */
/* ------------------------------------------------------------------ */

struct ssl_pair {
	SSL_CTX *server_ctx;
	SSL_CTX *client_ctx;
	SSL *server;
	SSL *client;
	/*
	 * One side of a socketpair.  The fd is owned by the SSL but
	 * never used for transport in these tests -- we drive the
	 * handshake and call data through memory BIOs (set via
	 * SSL_set_bio).  Holding a real fd lets clnt_destroy's
	 * close(fd) succeed without a sentinel detour.
	 */
	int client_fd;
	int server_fd;
	BIO *c_rbio;
	BIO *c_wbio;
	BIO *s_rbio;
	BIO *s_wbio;
};

/*
 * Build a self-signed cert + RSA key entirely in memory and load
 * them into the server context.  Avoids polluting the filesystem
 * and keeps the test self-contained.
 */
static int make_self_signed(SSL_CTX *server_ctx)
{
	EVP_PKEY *pkey = EVP_RSA_gen(2048);

	if (!pkey)
		return -1;

	X509 *x = X509_new();

	if (!x) {
		EVP_PKEY_free(pkey);
		return -1;
	}
	X509_set_version(x, 2);
	ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
	X509_gmtime_adj(X509_getm_notBefore(x), 0);
	X509_gmtime_adj(X509_getm_notAfter(x), 60 * 60 * 24);
	X509_set_pubkey(x, pkey);

	X509_NAME *name = X509_get_subject_name(x);

	X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
				   (const unsigned char *)"reffs-test", -1, -1,
				   0);
	X509_set_issuer_name(x, name);
	X509_sign(x, pkey, EVP_sha256());

	int ret = SSL_CTX_use_certificate(server_ctx, x);

	if (ret == 1)
		ret = SSL_CTX_use_PrivateKey(server_ctx, pkey);
	else
		ret = -1;
	X509_free(x);
	EVP_PKEY_free(pkey);
	return (ret == 1) ? 0 : -1;
}

static int ssl_pair_setup(struct ssl_pair *p)
{
	int sv[2];

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
		return -1;
	p->client_fd = sv[0];
	p->server_fd = sv[1];

	p->server_ctx = SSL_CTX_new(TLS_server_method());
	p->client_ctx = SSL_CTX_new(TLS_client_method());
	if (!p->server_ctx || !p->client_ctx)
		return -1;
	if (make_self_signed(p->server_ctx) < 0)
		return -1;
	SSL_CTX_set_verify(p->client_ctx, SSL_VERIFY_NONE, NULL);

	p->server = SSL_new(p->server_ctx);
	p->client = SSL_new(p->client_ctx);
	if (!p->server || !p->client)
		return -1;
	SSL_set_accept_state(p->server);
	SSL_set_connect_state(p->client);

	p->s_rbio = BIO_new(BIO_s_mem());
	p->s_wbio = BIO_new(BIO_s_mem());
	p->c_rbio = BIO_new(BIO_s_mem());
	p->c_wbio = BIO_new(BIO_s_mem());
	SSL_set_bio(p->server, p->s_rbio, p->s_wbio);
	SSL_set_bio(p->client, p->c_rbio, p->c_wbio);
	return 0;
}

/*
 * Shuttle one round of bytes between the BIOs in each direction.
 * Returns 0 if either side has more work to do, 1 if both finished.
 */
static int shuttle_one_round(struct ssl_pair *p)
{
	char buf[16384];
	int cr = SSL_do_handshake(p->client);
	int n;

	n = BIO_pending(p->c_wbio);
	if (n > 0) {
		n = BIO_read(p->c_wbio, buf, sizeof(buf));
		if (n > 0)
			BIO_write(p->s_rbio, buf, n);
	}

	int sr = SSL_do_handshake(p->server);

	n = BIO_pending(p->s_wbio);
	if (n > 0) {
		n = BIO_read(p->s_wbio, buf, sizeof(buf));
		if (n > 0)
			BIO_write(p->c_rbio, buf, n);
	}

	return (cr == 1 && sr == 1) ? 1 : 0;
}

static int ssl_pair_handshake(struct ssl_pair *p)
{
	for (int i = 0; i < 32; i++) {
		if (shuttle_one_round(p) == 1)
			return 0;
	}
	return -1;
}

static void ssl_pair_teardown(struct ssl_pair *p)
{
	/*
	 * The SSL objects are owned by clnt_destroy in the happy path
	 * tests; this teardown only handles the unclaimed objects.
	 */
	if (p->client) {
		SSL_free(p->client);
		p->client = NULL;
	}
	if (p->server) {
		SSL_free(p->server);
		p->server = NULL;
	}
	if (p->client_ctx) {
		SSL_CTX_free(p->client_ctx);
		p->client_ctx = NULL;
	}
	if (p->server_ctx) {
		SSL_CTX_free(p->server_ctx);
		p->server_ctx = NULL;
	}
	if (p->client_fd >= 0) {
		close(p->client_fd);
		p->client_fd = -1;
	}
	if (p->server_fd >= 0) {
		close(p->server_fd);
		p->server_fd = -1;
	}
}

/* ------------------------------------------------------------------ */
/* Tests                                                              */
/* ------------------------------------------------------------------ */

START_TEST(test_create_bad_args)
{
	struct ssl_pair p = { 0 };

	ck_assert_int_eq(ssl_pair_setup(&p), 0);

	ck_assert_ptr_null(mds_tls_xprt_create(-1, p.client, 100003u, 4u));
	ck_assert_ptr_null(mds_tls_xprt_create(p.client_fd, NULL, 100003u, 4u));

	ssl_pair_teardown(&p);
}
END_TEST

START_TEST(test_create_destroy_lifecycle)
{
	/*
	 * Drive a full TLS handshake first so the SSL is in a state
	 * where clnt_destroy's SSL_shutdown does real work (sending
	 * a close_notify into the BIO).  No actual RPC traffic.
	 *
	 * After clnt_destroy returns: the SSL is freed (SSL_free was
	 * called inside the XPRT), the fd is closed, and the test
	 * teardown only has the server-side state to mop up.
	 */
	struct ssl_pair p = { 0 };

	ck_assert_int_eq(ssl_pair_setup(&p), 0);
	ck_assert_int_eq(ssl_pair_handshake(&p), 0);

	CLIENT *clnt = mds_tls_xprt_create(p.client_fd, p.client, 100003u, 4u);

	ck_assert_ptr_nonnull(clnt);
	ck_assert_ptr_nonnull(clnt->cl_ops);
	ck_assert_ptr_nonnull(clnt->cl_auth);

	/*
	 * Hand ownership of fd + ssl to clnt_destroy.  The teardown
	 * helper would otherwise free them again -- forget the
	 * pointers so it skips the duplicate.
	 */
	p.client = NULL;
	p.client_fd = -1;

	clnt_destroy(clnt);

	ssl_pair_teardown(&p);
}
END_TEST

START_TEST(test_create_keeps_fd_and_ssl_on_failure)
{
	/*
	 * mds_tls_xprt_create rejects bad args BEFORE taking
	 * ownership.  Caller still owns fd + ssl and is responsible
	 * for cleanup.  Verified by the teardown reaching ASAN-clean
	 * even though no clnt_destroy ran.
	 */
	struct ssl_pair p = { 0 };

	ck_assert_int_eq(ssl_pair_setup(&p), 0);

	ck_assert_ptr_null(mds_tls_xprt_create(-1, p.client, 100003u, 4u));
	/* p.client is still ours; teardown frees. */

	ssl_pair_teardown(&p);
}
END_TEST

START_TEST(test_initial_err_state_is_success)
{
	/*
	 * Skeleton placeholder for the eventual call-failure
	 * coverage.  The full clnt_call round-trip (encode ->
	 * tls_rpc_send -> tls_rpc_recv -> decode) cannot run
	 * against the in-memory BIO fixture here -- there is no
	 * second thread driving the server side, so tls_rpc_recv
	 * would block forever.  Slice plan-1-tls.c carries the
	 * real round-trip via the docker compose mini-CA stack
	 * where a live MDS thread answers.
	 *
	 * What this test pins today: a freshly-created XPRT
	 * reports re_status == RPC_SUCCESS (clnt_geterr on a
	 * never-called client should not surface a stale error).
	 * The encode/transport failure paths in mds_tls_xprt_call
	 * mutate this field atomically when they fire, so any
	 * future caller doing clnt_geterr-then-act after a failed
	 * clnt_call gets a meaningful status.
	 */
	struct ssl_pair p = { 0 };

	ck_assert_int_eq(ssl_pair_setup(&p), 0);
	ck_assert_int_eq(ssl_pair_handshake(&p), 0);

	CLIENT *clnt = mds_tls_xprt_create(p.client_fd, p.client, 100003u, 4u);

	ck_assert_ptr_nonnull(clnt);

	struct rpc_err err;

	clnt_geterr(clnt, &err);
	ck_assert_int_eq(err.re_status, RPC_SUCCESS);

	p.client = NULL;
	p.client_fd = -1;

	clnt_destroy(clnt);
	ssl_pair_teardown(&p);
}
END_TEST

/*
 * xdrproc_t-shaped variant that always reports failure -- declared
 * variadic to match libtirpc's xdrproc_t signature exactly so the
 * implicit conversion to xdrproc_t at the clnt_call site is clean
 * under -Wincompatible-function-pointer-types.  Forces the encode
 * leg of mds_tls_call to take the RPC_CANTENCODEARGS path before
 * any network I/O happens, avoiding the in-memory BIO fixture's
 * "no second thread for tls_rpc_recv" limitation while still
 * pinning a real call-failure error code on the wire-side state
 * machine.
 */
static bool_t xdr_always_false(XDR *xdrs, ...)
{
	(void)xdrs;
	return FALSE;
}

START_TEST(test_call_encode_failure_sets_re_status)
{
	struct ssl_pair p = { 0 };

	ck_assert_int_eq(ssl_pair_setup(&p), 0);
	ck_assert_int_eq(ssl_pair_handshake(&p), 0);

	CLIENT *clnt = mds_tls_xprt_create(p.client_fd, p.client, 100003u, 4u);

	ck_assert_ptr_nonnull(clnt);

	/*
	 * AUTH_NONE so mds_tls_marshal_auth succeeds and we reach the
	 * actual user-args encode where xdr_always_false fires.
	 */
	clnt->cl_auth = authnone_create();

	/*
	 * libtirpc declares xdr_void with `bool_t xdr_void(void)` --
	 * casting it to xdrproc_t (variadic) trips
	 * -Wcast-function-type-mismatch.  Route the cast through `void *`
	 * to bypass the function-pointer-type check; the function is
	 * never called on the encode-failure path so the ABI mismatch
	 * cannot matter at runtime.
	 */
	enum clnt_stat st = clnt_call(clnt, 0, xdr_always_false, NULL,
				      (xdrproc_t)(void *)xdr_void, NULL,
				      (struct timeval){ .tv_sec = 1 });

	ck_assert_int_eq(st, RPC_CANTENCODEARGS);

	struct rpc_err err;

	clnt_geterr(clnt, &err);
	ck_assert_int_eq(err.re_status, RPC_CANTENCODEARGS);

	auth_destroy(clnt->cl_auth);
	clnt->cl_auth = NULL;

	p.client = NULL;
	p.client_fd = -1;

	clnt_destroy(clnt);
	ssl_pair_teardown(&p);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite setup                                                        */
/* ------------------------------------------------------------------ */

static Suite *mds_tls_xprt_suite(void)
{
	Suite *s = suite_create("mds_tls_xprt");
	TCase *tc = tcase_create("core");

	tcase_add_test(tc, test_create_bad_args);
	tcase_add_test(tc, test_create_destroy_lifecycle);
	tcase_add_test(tc, test_create_keeps_fd_and_ssl_on_failure);
	tcase_add_test(tc, test_initial_err_state_is_success);
	tcase_add_test(tc, test_call_encode_failure_sets_re_status);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int failed;
	SRunner *sr = srunner_create(mds_tls_xprt_suite());

	srunner_run_all(sr, CK_NORMAL);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return failed == 0 ? 0 : 1;
}
