/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * TLS client library for NFS tools.
 *
 * Shared code for STARTTLS (RFC 9289), direct TLS, RPC framing,
 * and connection management.  Used by nfs_tls_stress, nfs_tls_test,
 * and future TLS verification tools.
 *
 * No TIRPC dependency -- hand-rolled RPC record marking.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifdef HAVE_CONFIG_H
#include "config.h" /* IWYU pragma: keep */
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include "reffs/tls_client.h"

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define AUTH_TLS 7
#define AUTH_NONE 0
#define NFS4_PROGRAM 100003
#define NFS4_VERSION 4
#define STARTTLS_VERIFIER "STARTTLS"
#define MAX_RPC_SEND 4096

/* ------------------------------------------------------------------ */
/* SSL_CTX creation                                                    */
/* ------------------------------------------------------------------ */

static const uint8_t alpn_sunrpc[] = { 6, 's', 'u', 'n', 'r', 'p', 'c' };

SSL_CTX *tls_client_ctx_create(const struct tls_client_config *cfg)
{
	SSL_library_init();
	OpenSSL_add_all_algorithms();
	SSL_load_error_strings();

	SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
	if (!ctx) {
		fprintf(stderr, "tls: SSL_CTX_new failed\n");
		return NULL;
	}

	SSL_CTX_set_alpn_protos(ctx, alpn_sunrpc, sizeof(alpn_sunrpc));

	if (cfg->ca_path &&
	    !SSL_CTX_load_verify_locations(ctx, cfg->ca_path, NULL)) {
		fprintf(stderr, "tls: failed to load CA: %s\n", cfg->ca_path);
		SSL_CTX_free(ctx);
		return NULL;
	}

	if (cfg->cert_path &&
	    SSL_CTX_use_certificate_file(ctx, cfg->cert_path,
					 SSL_FILETYPE_PEM) <= 0) {
		fprintf(stderr, "tls: failed to load cert: %s\n",
			cfg->cert_path);
		SSL_CTX_free(ctx);
		return NULL;
	}

	if (cfg->key_path &&
	    SSL_CTX_use_PrivateKey_file(ctx, cfg->key_path, SSL_FILETYPE_PEM) <=
		    0) {
		fprintf(stderr, "tls: failed to load key: %s\n", cfg->key_path);
		SSL_CTX_free(ctx);
		return NULL;
	}

	/*
	 * Enable peer verification when a CA is present.  OpenSSL's
	 * default SSL_CTX verify mode is SSL_VERIFY_NONE -- loading a
	 * CA via SSL_CTX_load_verify_locations does not turn checking
	 * on; that requires an explicit SSL_CTX_set_verify call.  When
	 * the operator supplies a CA the contract is "verify the
	 * server", and that includes hostname binding via SAN/CN if
	 * cfg->hostname is set.  cfg->no_verify takes precedence as
	 * the explicit downgrade for tests and ad-hoc deployments.
	 */
	if (cfg->no_verify) {
		SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
	} else if (cfg->ca_path) {
		SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
		if (cfg->hostname) {
			X509_VERIFY_PARAM *p = SSL_CTX_get0_param(ctx);

			if (X509_VERIFY_PARAM_set1_host(p, cfg->hostname, 0) !=
			    1) {
				fprintf(stderr,
					"tls: failed to set verify hostname: %s\n",
					cfg->hostname);
				SSL_CTX_free(ctx);
				return NULL;
			}
		}
	} else {
		/*
		 * No CA configured and no_verify not set: keep the
		 * historical no-verify behaviour explicit so we never
		 * silently inherit an OpenSSL default.
		 */
		SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
	}

	return ctx;
}

/* ------------------------------------------------------------------ */
/* AUTH_TLS probe (RFC 9289 STARTTLS)                                  */
/* ------------------------------------------------------------------ */

static int send_auth_tls_probe(int fd, uint32_t xid)
{
	uint32_t msg[13];
	uint32_t body_len = 12 * sizeof(uint32_t);

	msg[0] = htonl(0x80000000 | body_len);
	msg[1] = htonl(xid);
	msg[2] = htonl(0); /* CALL */
	msg[3] = htonl(2); /* RPC version */
	msg[4] = htonl(NFS4_PROGRAM); /* NFS program */
	msg[5] = htonl(NFS4_VERSION); /* NFS version */
	msg[6] = htonl(0); /* NULL procedure */
	msg[7] = htonl(AUTH_TLS); /* cred flavor */
	msg[8] = htonl(0); /* cred len */
	msg[9] = htonl(AUTH_NONE); /* verf flavor */
	msg[10] = htonl(8); /* verf len */
	memcpy(&msg[11], STARTTLS_VERIFIER, 8);

	ssize_t n = write(fd, msg, sizeof(msg));
	return (n == sizeof(msg)) ? 0 : -1;
}

static int recv_auth_tls_reply(int fd, uint32_t expected_xid)
{
	uint8_t buf[256];
	size_t got = 0;
	ssize_t n;

	while (got < 28) {
		n = read(fd, buf + got, sizeof(buf) - got);
		if (n <= 0)
			return -1;
		got += (size_t)n;
	}
	n = (ssize_t)got;

	uint32_t *p = (uint32_t *)buf;
	uint32_t recmark = ntohl(p[0]);
	uint32_t xid = ntohl(p[1]);
	uint32_t msg_type = ntohl(p[2]);
	uint32_t reply_stat = ntohl(p[3]);

	if (!(recmark & 0x80000000) || xid != expected_xid || msg_type != 1 ||
	    reply_stat != 0)
		return -1;

	uint32_t verf_len = ntohl(p[5]);
	uint32_t verf_words = (verf_len + 3) / 4;
	uint32_t accept_idx = 6 + verf_words;

	if ((accept_idx + 1) * 4 > (uint32_t)n)
		return -1;

	return (ntohl(p[accept_idx]) == 0) ? 0 : -1;
}

/* Monotonic XID counter shared by all library callers */
static uint32_t tls_next_xid = 0x544c5301;

static uint32_t tls_alloc_xid(void)
{
	return tls_next_xid++;
}

/* ------------------------------------------------------------------ */
/* Connection establishment                                            */
/* ------------------------------------------------------------------ */

SSL *tls_starttls(int fd, SSL_CTX *ctx, int verbose)
{
	uint32_t xid = tls_alloc_xid();

	if (send_auth_tls_probe(fd, xid)) {
		if (verbose)
			fprintf(stderr, "  STARTTLS: send failed\n");
		return NULL;
	}
	if (recv_auth_tls_reply(fd, xid)) {
		if (verbose)
			fprintf(stderr, "  STARTTLS: reply failed\n");
		return NULL;
	}

	SSL *ssl = SSL_new(ctx);
	if (!ssl)
		return NULL;

	SSL_set_fd(ssl, fd);

	if (SSL_connect(ssl) <= 0) {
		if (verbose) {
			unsigned long err = ERR_get_error();
			char errbuf[256];
			ERR_error_string_n(err, errbuf, sizeof(errbuf));
			fprintf(stderr, "  STARTTLS: SSL_connect: %s\n",
				errbuf);
		}
		SSL_free(ssl);
		return NULL;
	}

	return ssl;
}

SSL *tls_direct_connect(int fd, SSL_CTX *ctx, int verbose)
{
	SSL *ssl = SSL_new(ctx);
	if (!ssl)
		return NULL;

	SSL_set_fd(ssl, fd);

	if (SSL_connect(ssl) <= 0) {
		if (verbose) {
			unsigned long err = ERR_get_error();
			char errbuf[256];
			ERR_error_string_n(err, errbuf, sizeof(errbuf));
			fprintf(stderr, "  direct TLS: SSL_connect: %s\n",
				errbuf);
		}
		SSL_free(ssl);
		return NULL;
	}

	return ssl;
}

/* ------------------------------------------------------------------ */
/* RPC framing over TLS or cleartext                                   */
/* ------------------------------------------------------------------ */

int tls_rpc_send(SSL *ssl, int fd, const uint8_t *body, size_t body_len)
{
	uint32_t rm = htonl(0x80000000 | (uint32_t)body_len);

	if (ssl) {
		if (body_len > MAX_RPC_SEND)
			return -1;
		uint8_t tmp[MAX_RPC_SEND + 4];
		memcpy(tmp, &rm, 4);
		memcpy(tmp + 4, body, body_len);
		int n = SSL_write(ssl, tmp, 4 + (int)body_len);
		return (n == (int)(4 + body_len)) ? 0 : -1;
	}

	struct iovec iov[2] = {
		{ .iov_base = &rm, .iov_len = 4 },
		{ .iov_base = (void *)body, .iov_len = body_len },
	};
	ssize_t n = writev(fd, iov, 2);
	return (n == (ssize_t)(4 + body_len)) ? 0 : -1;
}

ssize_t tls_rpc_recv(SSL *ssl, int fd, uint8_t *buf, size_t bufsz)
{
	uint8_t rm_buf[4];
	ssize_t n;

	if (ssl)
		n = SSL_read(ssl, rm_buf, 4);
	else
		n = read(fd, rm_buf, 4);

	if (n != 4)
		return -1;

	uint32_t rm;
	memcpy(&rm, rm_buf, 4);
	rm = ntohl(rm);

	if (!(rm & 0x80000000))
		return -1; /* multi-fragment not supported */

	uint32_t frag_len = rm & 0x7FFFFFFF;
	if (frag_len > bufsz)
		return -1;

	size_t got = 0;
	while (got < frag_len) {
		if (ssl)
			n = SSL_read(ssl, buf + got, (int)(frag_len - got));
		else
			n = read(fd, buf + got, frag_len - got);
		if (n <= 0)
			return -1;
		got += (size_t)n;
	}

	return (ssize_t)frag_len;
}

/* ------------------------------------------------------------------ */
/* TLS handshake tracing                                               */
/* ------------------------------------------------------------------ */

void tls_trace_handshake(SSL *ssl, const char *label)
{
	const char *version = SSL_get_version(ssl);
	const char *cipher = SSL_get_cipher_name(ssl);
	const uint8_t *alpn_out = NULL;
	unsigned int alpn_len = 0;
	int reused = SSL_session_reused(ssl);

	SSL_get0_alpn_selected(ssl, &alpn_out, &alpn_len);

	printf("  [TLS %s] %s cipher=%s alpn=%.*s session=%s\n", label, version,
	       cipher, (int)alpn_len, alpn_out ? (const char *)alpn_out : "-",
	       reused ? "reused" : "new");
}

/* ------------------------------------------------------------------ */
/* TCP helpers                                                         */
/* ------------------------------------------------------------------ */

int tls_tcp_connect(const char *host, int port)
{
	struct addrinfo hints = { .ai_family = AF_UNSPEC,
				  .ai_socktype = SOCK_STREAM };
	struct addrinfo *res;
	char port_str[16];

	snprintf(port_str, sizeof(port_str), "%d", port);

	int err = getaddrinfo(host, port_str, &hints, &res);
	if (err) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(err));
		return -1;
	}

	int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (fd < 0) {
		freeaddrinfo(res);
		return -1;
	}

	struct timeval tv = { .tv_sec = 5 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
		close(fd);
		freeaddrinfo(res);
		return -1;
	}

	freeaddrinfo(res);
	return fd;
}

void tls_tcp_reset(int fd)
{
	struct linger l = { .l_onoff = 1, .l_linger = 0 };
	setsockopt(fd, SOL_SOCKET, SO_LINGER, &l, sizeof(l));
	close(fd);
}
