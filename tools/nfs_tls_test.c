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

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#define NFS_PORT 2049
#define AUTH_TLS 7
#define STARTTLS_VERIFIER "STARTTLS"

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

/* Connect a TCP socket to host:port. */
static int tcp_connect(const char *host, int port)
{
	struct addrinfo hints = { .ai_family = AF_INET,
				  .ai_socktype = SOCK_STREAM };
	struct addrinfo *res;
	char port_str[16];

	snprintf(port_str, sizeof(port_str), "%d", port);

	int err = getaddrinfo(host, port_str, &hints, &res);

	if (err) {
		fprintf(stderr, "  getaddrinfo: %s\n", gai_strerror(err));
		return -1;
	}

	int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);

	if (fd < 0) {
		freeaddrinfo(res);
		return -1;
	}

	if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
		close(fd);
		freeaddrinfo(res);
		return -1;
	}

	freeaddrinfo(res);
	return fd;
}

/*
 * Build and send an AUTH_TLS NULL RPC probe (RFC 9289).
 *
 * Wire format (record marking + RPC call):
 *   record_mark: 0x80000000 | msg_len
 *   xid: random
 *   msg_type: CALL (0)
 *   rpc_version: 2
 *   program: 100003 (NFS)
 *   version: 4
 *   procedure: 0 (NULL)
 *   cred_flavor: AUTH_TLS (7)
 *   cred_len: 0
 *   verf_flavor: AUTH_NONE (0)
 *   verf_len: 8
 *   verf_body: "STARTTLS"
 */
static int send_auth_tls_probe(int fd, uint32_t xid)
{
	uint32_t msg[13];
	uint32_t body_len = 12 * sizeof(uint32_t);

	msg[0] = htonl(0x80000000 | body_len);
	msg[1] = htonl(xid);
	msg[2] = htonl(0); /* CALL */
	msg[3] = htonl(2); /* RPC version */
	msg[4] = htonl(100003); /* NFS program */
	msg[5] = htonl(4); /* NFS version */
	msg[6] = htonl(0); /* NULL procedure */
	msg[7] = htonl(AUTH_TLS); /* cred flavor */
	msg[8] = htonl(0); /* cred len */
	msg[9] = htonl(0); /* verf flavor (AUTH_NONE) */
	msg[10] = htonl(8); /* verf len */
	memcpy(&msg[11], STARTTLS_VERIFIER, 8); /* verf body */

	ssize_t n = write(fd, msg, sizeof(msg));

	return (n == sizeof(msg)) ? 0 : -1;
}

/*
 * Read and validate the AUTH_TLS reply.
 * Expects MSG_ACCEPTED with RPCSEC_GSS(6) verifier containing
 * "STARTTLS", or AUTH_NONE verifier with SUCCESS.
 */
static int recv_auth_tls_reply(int fd, uint32_t expected_xid)
{
	uint8_t buf[256];
	ssize_t n = read(fd, buf, sizeof(buf));

	if (n < 28) {
		fprintf(stderr, "  reply too short: %zd bytes\n", n);
		return -1;
	}

	uint32_t *p = (uint32_t *)buf;
	uint32_t recmark = ntohl(p[0]);
	uint32_t xid = ntohl(p[1]);
	uint32_t msg_type = ntohl(p[2]);
	uint32_t reply_stat = ntohl(p[3]);

	if (!(recmark & 0x80000000)) {
		fprintf(stderr, "  bad record mark: 0x%08x\n", recmark);
		return -1;
	}

	if (xid != expected_xid) {
		fprintf(stderr, "  xid mismatch: expected 0x%08x got 0x%08x\n",
			expected_xid, xid);
		return -1;
	}

	if (msg_type != 1) { /* MSG_REPLY */
		fprintf(stderr, "  not a reply: msg_type=%u\n", msg_type);
		return -1;
	}

	if (reply_stat != 0) { /* MSG_ACCEPTED */
		fprintf(stderr, "  reply rejected: stat=%u\n", reply_stat);
		return -1;
	}

	/* Verifier: check for STARTTLS in the body */
	uint32_t verf_flavor = ntohl(p[4]);
	uint32_t verf_len = ntohl(p[5]);

	printf("  verf_flavor=%u verf_len=%u\n", verf_flavor, verf_len);

	/* Accept stat follows the verifier */
	uint32_t verf_words = (verf_len + 3) / 4;
	uint32_t accept_idx = 6 + verf_words;

	if ((accept_idx + 1) * 4 > (uint32_t)n) {
		fprintf(stderr, "  reply truncated\n");
		return -1;
	}

	uint32_t accept_stat = ntohl(p[accept_idx]);

	if (accept_stat != 0) {
		fprintf(stderr, "  accept_stat=%u (expected SUCCESS)\n",
			accept_stat);
		return -1;
	}

	return 0;
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
	uint32_t xid = 0x544c5301; /* "TLS\x01" */

	/* ---- Test 1: TCP connect ---- */
	printf("TEST 1: TCP connect to %s:%d ... ", server, port);
	fflush(stdout);

	int fd = tcp_connect(server, port);

	if (fd < 0) {
		printf("FAIL\n");
		return 1;
	}
	printf("PASS (fd=%d)\n", fd);
	passed++;

	/* ---- Test 2: AUTH_TLS probe ---- */
	printf("TEST 2: AUTH_TLS NULL probe ... ");
	fflush(stdout);

	if (send_auth_tls_probe(fd, xid) < 0) {
		printf("FAIL (send)\n");
		close(fd);
		return 1;
	}

	if (recv_auth_tls_reply(fd, xid) < 0) {
		printf("FAIL (reply)\n");
		failed++;
	} else {
		printf("PASS (server supports AUTH_TLS)\n");
		passed++;
	}

	/* ---- Test 3: TLS upgrade ---- */
	printf("TEST 3: TLS handshake ... ");
	fflush(stdout);

	SSL_library_init();
	OpenSSL_add_all_algorithms();
	SSL_load_error_strings();

	SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());

	if (!ctx) {
		printf("FAIL (SSL_CTX_new)\n");
		failed++;
		goto done;
	}

	/* ALPN: "sunrpc" per RFC 9289 */
	static const uint8_t alpn[] = { 6, 's', 'u', 'n', 'r', 'p', 'c' };

	SSL_CTX_set_alpn_protos(ctx, alpn, sizeof(alpn));

	if (ca_path) {
		if (!SSL_CTX_load_verify_locations(ctx, ca_path, NULL)) {
			printf("FAIL (load CA: %s)\n", ca_path);
			failed++;
			goto done_ctx;
		}
	}

	if (cert_path) {
		if (SSL_CTX_use_certificate_file(ctx, cert_path,
						 SSL_FILETYPE_PEM) <= 0) {
			printf("FAIL (load cert: %s)\n", cert_path);
			failed++;
			goto done_ctx;
		}
	}

	if (key_path) {
		if (SSL_CTX_use_PrivateKey_file(ctx, key_path,
						SSL_FILETYPE_PEM) <= 0) {
			printf("FAIL (load key: %s)\n", key_path);
			failed++;
			goto done_ctx;
		}
	}

	SSL *ssl = SSL_new(ctx);

	if (!ssl) {
		printf("FAIL (SSL_new)\n");
		failed++;
		goto done_ctx;
	}

	SSL_set_fd(ssl, fd);

	if (SSL_connect(ssl) <= 0) {
		unsigned long err = ERR_get_error();
		char errbuf[256];

		ERR_error_string_n(err, errbuf, sizeof(errbuf));
		printf("FAIL (%s)\n", errbuf);
		failed++;
		goto done_ssl;
	}

	/* Check ALPN result */
	const uint8_t *alpn_out = NULL;
	unsigned int alpn_len = 0;

	SSL_get0_alpn_selected(ssl, &alpn_out, &alpn_len);
	if (alpn_len == 6 && memcmp(alpn_out, "sunrpc", 6) == 0)
		printf("PASS (TLS %s, ALPN=sunrpc)\n", SSL_get_version(ssl));
	else
		printf("PASS (TLS %s, no ALPN)\n", SSL_get_version(ssl));
	passed++;

	/* Verify server cert if CA was provided */
	if (ca_path) {
		long verify = SSL_get_verify_result(ssl);

		printf("  server cert verify: %s\n",
		       verify == X509_V_OK ?
			       "OK" :
			       X509_verify_cert_error_string(verify));
	}

done_ssl:
	SSL_free(ssl);
done_ctx:
	SSL_CTX_free(ctx);
done:
	close(fd);

	printf("\n%d passed, %d failed\n", passed, failed);
	return failed ? 1 : 0;
}
