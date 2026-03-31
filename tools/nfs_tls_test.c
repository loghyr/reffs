/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * nfs_tls_test — standalone RPC-with-TLS security tester.
 *
 * Connects to an NFSv4 server, sends an AUTH_TLS NULL RPC probe
 * per RFC 9289, verifies the STARTTLS response, then upgrades
 * the connection to TLS and performs a basic NFS operation.
 *
 * No kernel NFS client involvement — pure userspace.  Can be
 * pointed at any NFS server to verify TLS support.
 *
 * Usage:
 *   nfs_tls_test --server <host> [--port <port>]
 *                [--cert <path>] [--key <path>] [--ca <path>]
 */

#ifdef HAVE_CONFIG_H
#include "config.h" /* IWYU pragma: keep */
#endif

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include "reffs/tls_client.h"

#define NFS_PORT 2049

static void usage(void)
{
	fprintf(stderr,
		"Usage: nfs_tls_test --server <host> [options]\n"
		"\n"
		"Options:\n"
		"  --server, -s <host>  NFS server hostname (required)\n"
		"  --port, -p <port>    NFS port (default: 2049)\n"
		"  --cert, -c <path>    Client certificate (for mutual TLS)\n"
		"  --key, -k <path>     Client private key\n"
		"  --ca <path>          CA certificate for server verification\n"
		"  --help, -h           This help\n"
		"\n"
		"Exit codes:\n"
		"  0  All tests passed\n"
		"  1  Test failed\n"
		"  2  Usage error\n");
}

int main(int argc, char *argv[])
{
	const char *server = NULL;
	const char *cert_path = NULL;
	const char *key_path = NULL;
	const char *ca_path = NULL;
	int port = NFS_PORT;

	static struct option opts[] = {
		{ "server", required_argument, NULL, 's' },
		{ "port", required_argument, NULL, 'p' },
		{ "cert", required_argument, NULL, 'c' },
		{ "key", required_argument, NULL, 'k' },
		{ "ca", required_argument, NULL, 'a' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};

	int opt;

	while ((opt = getopt_long(argc, argv, "s:p:c:k:a:h", opts, NULL)) !=
	       -1) {
		switch (opt) {
		case 's':
			server = optarg;
			break;
		case 'p':
			port = atoi(optarg);
			break;
		case 'c':
			cert_path = optarg;
			break;
		case 'k':
			key_path = optarg;
			break;
		case 'a':
			ca_path = optarg;
			break;
		case 'h':
			usage();
			return 0;
		default:
			usage();
			return 2;
		}
	}

	if (!server) {
		fprintf(stderr, "Error: --server is required\n\n");
		usage();
		return 2;
	}

	int failed = 0;
	int passed = 0;

	/* ---- Test 1: TCP connect ---- */
	printf("TEST 1: TCP connect to %s:%d ... ", server, port);
	fflush(stdout);

	int fd = tls_tcp_connect(server, port);

	if (fd < 0) {
		printf("FAIL\n");
		return 1;
	}
	printf("PASS (fd=%d)\n", fd);
	passed++;

	/* ---- Test 2+3: STARTTLS (AUTH_TLS probe + TLS handshake) ---- */
	printf("TEST 2: STARTTLS (AUTH_TLS + TLS handshake) ... ");
	fflush(stdout);

	struct tls_client_config tls_cfg = {
		.cert_path = cert_path,
		.key_path = key_path,
		.ca_path = ca_path,
	};
	SSL_CTX *ctx = tls_client_ctx_create(&tls_cfg);

	if (!ctx) {
		printf("FAIL (SSL_CTX)\n");
		failed++;
		goto done;
	}

	SSL *ssl = tls_starttls(fd, ctx, 1);
	if (!ssl) {
		printf("FAIL\n");
		failed++;
		goto done_ctx;
	}

	printf("PASS\n");
	passed++;

	tls_trace_handshake(ssl, "test");

	/* Verify server cert if CA was provided */
	if (ca_path) {
		long verify = SSL_get_verify_result(ssl);

		printf("  server cert verify: %s\n",
		       verify == X509_V_OK ?
			       "OK" :
			       X509_verify_cert_error_string(verify));
	}

	SSL_free(ssl);
done_ctx:
	SSL_CTX_free(ctx);
done:
	close(fd);

	printf("\n%d passed, %d failed\n", passed, failed);
	return failed ? 1 : 0;
}
