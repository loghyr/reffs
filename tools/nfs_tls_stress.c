/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * nfs_tls_stress — TLS NFS connection stress tester.
 *
 * Standalone diagnostic tool for stress-testing TLS connections to
 * NFSv4.2 servers.  Exercises STARTTLS (RFC 9289), hot reconnect,
 * mid-operation disconnect, and rapid cycling.
 *
 * Hand-rolled RPC/XDR — no TIRPC, no liburcu, no reffs libraries.
 * Statically linkable with OpenSSL for customer deployment.
 *
 * NFSv4.2 session setup: EXCHANGE_ID → CREATE_SESSION, then
 * SEQUENCE + PUTROOTFH + GETATTR in a loop.  GETATTR of root
 * is pure metadata — no I/O on the server.
 *
 * Usage:
 *   nfs_tls_stress --host <addr> [--port <port>] [--mode <mode>]
 *                  [--iterations <n>] [--cert <path>] [--key <path>]
 *                  [--ca <path>] [--direct-tls] [--verbose]
 */

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include "reffs/tls_client.h"

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define NFS_PORT 2049
#define AUTH_SYS 1
#define AUTH_NONE 0

/* NFSv4.2 program and version */
#define NFS4_PROGRAM 100003
#define NFS4_VERSION 4
#define NFS4_MINOR_VERSION 2
#define NFS4_COMPOUND_PROC 1

/* NFSv4 op numbers we need */
#define OP_GETATTR 9
#define OP_PUTROOTFH 24
#define OP_SEQUENCE 53
#define OP_EXCHANGE_ID 42
#define OP_CREATE_SESSION 43
#define OP_DESTROY_SESSION 44
#define OP_DESTROY_CLIENTID 57

/* NFSv4 EXCHANGE_ID flags */
#define EXCHGID4_FLAG_USE_NON_PNFS 0x00010000
#define EXCHGID4_FLAG_SUPP_MOVED_REFER 0x00000001

/* CREATE_SESSION flags */
#define CREATE_SESSION4_FLAG_PERSIST 0x00000001

/* XDR buffer size */
#define XDR_BUFSZ 4096
#define RECV_BUFSZ 8192

/* ------------------------------------------------------------------ */
/* XDR encoder — minimal, write-only, network byte order              */
/* ------------------------------------------------------------------ */

struct xdr_buf {
	uint8_t *base;
	size_t pos;
	size_t len;
};

static void xdr_init(struct xdr_buf *x, uint8_t *buf, size_t len)
{
	x->base = buf;
	x->pos = 0;
	x->len = len;
}

static int xdr_u32(struct xdr_buf *x, uint32_t v)
{
	if (x->pos + 4 > x->len)
		return -1;
	uint32_t nv = htonl(v);
	memcpy(x->base + x->pos, &nv, 4);
	x->pos += 4;
	return 0;
}

static int xdr_u64(struct xdr_buf *x, uint64_t v)
{
	if (xdr_u32(x, (uint32_t)(v >> 32)))
		return -1;
	return xdr_u32(x, (uint32_t)(v & 0xFFFFFFFF));
}

static int xdr_opaque(struct xdr_buf *x, const void *data, uint32_t len)
{
	if (xdr_u32(x, len))
		return -1;
	uint32_t padded = (len + 3) & ~3u;
	if (x->pos + padded > x->len)
		return -1;
	memcpy(x->base + x->pos, data, len);
	if (padded > len)
		memset(x->base + x->pos + len, 0, padded - len);
	x->pos += padded;
	return 0;
}

static int xdr_string(struct xdr_buf *x, const char *s)
{
	return xdr_opaque(x, s, (uint32_t)strlen(s));
}

/* ------------------------------------------------------------------ */
/* XDR decoder — minimal, read-only                                   */
/* ------------------------------------------------------------------ */

struct xdr_reader {
	const uint8_t *base;
	size_t pos;
	size_t len;
};

static void xdr_reader_init(struct xdr_reader *r, const uint8_t *buf,
			    size_t len)
{
	r->base = buf;
	r->pos = 0;
	r->len = len;
}

static int xdr_read_u32(struct xdr_reader *r, uint32_t *out)
{
	if (r->pos + 4 > r->len)
		return -1;
	uint32_t nv;
	memcpy(&nv, r->base + r->pos, 4);
	*out = ntohl(nv);
	r->pos += 4;
	return 0;
}

static int xdr_read_u64(struct xdr_reader *r, uint64_t *out)
{
	uint32_t hi, lo;
	if (xdr_read_u32(r, &hi) || xdr_read_u32(r, &lo))
		return -1;
	*out = ((uint64_t)hi << 32) | lo;
	return 0;
}

static int xdr_read_opaque(struct xdr_reader *r, void *buf, uint32_t max,
			   uint32_t *actual)
{
	uint32_t len;
	if (xdr_read_u32(r, &len))
		return -1;
	if (len > max)
		return -1;
	uint32_t padded = (len + 3) & ~3u;
	if (r->pos + padded > r->len)
		return -1;
	memcpy(buf, r->base + r->pos, len);
	r->pos += padded;
	if (actual)
		*actual = len;
	return 0;
}

static int xdr_skip(struct xdr_reader *r, size_t bytes)
{
	if (r->pos + bytes > r->len)
		return -1;
	r->pos += bytes;
	return 0;
}

/* Skip a variable-length opaque (length-prefixed) */
static int xdr_skip_opaque(struct xdr_reader *r)
{
	uint32_t len;
	if (xdr_read_u32(r, &len))
		return -1;
	uint32_t padded = (len + 3) & ~3u;
	return xdr_skip(r, padded);
}

/* ------------------------------------------------------------------ */
/* XID allocator (local to this tool)                                  */
/* ------------------------------------------------------------------ */

static uint32_t next_xid = 0x544c5301;

static uint32_t alloc_xid(void)
{
	return next_xid++;
}

/* ------------------------------------------------------------------ */
/* RPC call builder helpers                                           */
/* ------------------------------------------------------------------ */

/* Encode RPC call header with AUTH_SYS credentials */
static int rpc_call_header(struct xdr_buf *x, uint32_t xid, uint32_t prog,
			   uint32_t vers, uint32_t proc)
{
	if (xdr_u32(x, xid))
		return -1; /* xid */
	if (xdr_u32(x, 0))
		return -1; /* CALL */
	if (xdr_u32(x, 2))
		return -1; /* RPC version */
	if (xdr_u32(x, prog))
		return -1;
	if (xdr_u32(x, vers))
		return -1;
	if (xdr_u32(x, proc))
		return -1;

	/* AUTH_SYS credential */
	if (xdr_u32(x, AUTH_SYS))
		return -1;
	/* credential body: stamp + machine + uid + gid + gids[] */
	size_t cred_start = x->pos;
	if (xdr_u32(x, 0))
		return -1; /* placeholder for cred length */
	if (xdr_u32(x, 0))
		return -1; /* stamp */
	if (xdr_string(x, "tlsstress"))
		return -1; /* machine name */
	if (xdr_u32(x, 0))
		return -1; /* uid */
	if (xdr_u32(x, 0))
		return -1; /* gid */
	if (xdr_u32(x, 0))
		return -1; /* 0 aux gids */

	/* Fix up credential length */
	uint32_t cred_len = (uint32_t)(x->pos - cred_start - 4);
	uint32_t ncl = htonl(cred_len);
	memcpy(x->base + cred_start, &ncl, 4);

	/* AUTH_NONE verifier */
	if (xdr_u32(x, AUTH_NONE))
		return -1;
	if (xdr_u32(x, 0))
		return -1;

	return 0;
}

/* Parse RPC reply header, return accept_stat. -1 on error. */
static int rpc_reply_check(struct xdr_reader *r, uint32_t expected_xid)
{
	uint32_t xid, msg_type, reply_stat;

	if (xdr_read_u32(r, &xid) || xdr_read_u32(r, &msg_type))
		return -1;
	if (xid != expected_xid || msg_type != 1)
		return -1;
	if (xdr_read_u32(r, &reply_stat))
		return -1;
	if (reply_stat != 0)
		return -1; /* MSG_DENIED */

	/* Skip verifier */
	uint32_t verf_flavor, verf_len;
	if (xdr_read_u32(r, &verf_flavor) || xdr_read_u32(r, &verf_len))
		return -1;
	if (xdr_skip(r, (verf_len + 3) & ~3u))
		return -1;

	/* Accept stat */
	uint32_t accept_stat;
	if (xdr_read_u32(r, &accept_stat))
		return -1;

	return (int)accept_stat;
}

/* ------------------------------------------------------------------ */
/* NFSv4.2 COMPOUND builder                                           */
/* ------------------------------------------------------------------ */

/* Start a COMPOUND4args: tag + minorversion + argarray header */
static int compound_start(struct xdr_buf *x, uint32_t num_ops)
{
	if (xdr_string(x, ""))
		return -1; /* tag */
	if (xdr_u32(x, NFS4_MINOR_VERSION))
		return -1;
	if (xdr_u32(x, num_ops))
		return -1;
	return 0;
}

/* ------------------------------------------------------------------ */
/* NFSv4.2 ops                                                        */
/* ------------------------------------------------------------------ */

/* EXCHANGE_ID: establish client identity */
static int encode_exchange_id(struct xdr_buf *x, const char *owner)
{
	if (xdr_u32(x, OP_EXCHANGE_ID))
		return -1;

	/* eia_clientowner: verifier(8) + ownerid(opaque) */
	uint64_t verf = (uint64_t)time(NULL);
	if (xdr_u64(x, verf))
		return -1;
	if (xdr_opaque(x, owner, (uint32_t)strlen(owner)))
		return -1;

	/* eia_flags */
	if (xdr_u32(x, EXCHGID4_FLAG_USE_NON_PNFS))
		return -1;

	/* eia_state_protect: SP4_NONE */
	if (xdr_u32(x, 0))
		return -1;

	/* eia_client_impl_id: empty array */
	if (xdr_u32(x, 0))
		return -1;

	return 0;
}

/* Session state — filled from EXCHANGE_ID + CREATE_SESSION replies */
struct nfs4_session_state {
	uint64_t clientid;
	uint32_t sequenceid;
	uint8_t sessionid[16];
	uint32_t slot_seqid; /* per-slot sequence, starts at 1 */
};

/*
 * Parse EXCHANGE_ID reply, extract clientid + sequenceid.
 * Only decodes the fields we need — eir_flags, eir_state_protect,
 * eir_server_owner, eir_server_scope, and eir_server_impl_id are
 * skipped.  This works because EXCHANGE_ID is the sole op in its
 * compound, so no subsequent op decode depends on reader position.
 */
static int decode_exchange_id(struct xdr_reader *r,
			      struct nfs4_session_state *ss)
{
	/* COMPOUND4res: status + tag + resarray_len */
	uint32_t status;
	if (xdr_read_u32(r, &status))
		return -1;
	if (status != 0)
		return -(int)status;
	if (xdr_skip_opaque(r))
		return -1; /* tag */

	uint32_t num_ops;
	if (xdr_read_u32(r, &num_ops) || num_ops < 1)
		return -1;

	/* resop: opnum + status */
	uint32_t resop, op_status;
	if (xdr_read_u32(r, &resop) || resop != OP_EXCHANGE_ID)
		return -1;
	if (xdr_read_u32(r, &op_status) || op_status != 0)
		return -1;

	/* eir_clientid, eir_sequenceid */
	if (xdr_read_u64(r, &ss->clientid))
		return -1;
	if (xdr_read_u32(r, &ss->sequenceid))
		return -1;

	return 0;
}

/* CREATE_SESSION */
static int encode_create_session(struct xdr_buf *x,
				 struct nfs4_session_state *ss)
{
	if (xdr_u32(x, OP_CREATE_SESSION))
		return -1;

	/* csa_clientid */
	if (xdr_u64(x, ss->clientid))
		return -1;
	/* csa_sequence */
	if (xdr_u32(x, ss->sequenceid))
		return -1;
	/* csa_flags */
	if (xdr_u32(x, 0))
		return -1;

	/* csa_fore_chan_attrs: headerpadsize, maxrequestsize,
	 * maxresponsesize, maxresponsesize_cached, ca_maxoperations,
	 * ca_maxrequests, ca_rdma_ird[] */
	if (xdr_u32(x, 0))
		return -1; /* headerpadsize */
	if (xdr_u32(x, 1048576))
		return -1; /* maxrequestsize */
	if (xdr_u32(x, 1048576))
		return -1; /* maxresponsesize */
	if (xdr_u32(x, 4096))
		return -1; /* maxresponsesize_cached */
	if (xdr_u32(x, 16))
		return -1; /* ca_maxoperations */
	if (xdr_u32(x, 1))
		return -1; /* ca_maxrequests (1 slot) */
	if (xdr_u32(x, 0))
		return -1; /* rdma_ird empty array */

	/* csa_back_chan_attrs: same structure */
	if (xdr_u32(x, 0))
		return -1;
	if (xdr_u32(x, 4096))
		return -1;
	if (xdr_u32(x, 4096))
		return -1;
	if (xdr_u32(x, 4096))
		return -1;
	if (xdr_u32(x, 2))
		return -1;
	if (xdr_u32(x, 1))
		return -1;
	if (xdr_u32(x, 0))
		return -1;

	/* csa_cb_program */
	if (xdr_u32(x, 0x40000000))
		return -1;

	/* csa_sec_parms: 1 entry, AUTH_NONE (flavor=0, empty body) */
	if (xdr_u32(x, 1))
		return -1;
	if (xdr_u32(x, AUTH_NONE))
		return -1;

	return 0;
}

/* Parse CREATE_SESSION reply, extract sessionid */
static int decode_create_session(struct xdr_reader *r,
				 struct nfs4_session_state *ss)
{
	uint32_t status;
	if (xdr_read_u32(r, &status))
		return -1;
	if (status != 0)
		return -(int)status;
	if (xdr_skip_opaque(r))
		return -1; /* tag */

	uint32_t num_ops;
	if (xdr_read_u32(r, &num_ops) || num_ops < 1)
		return -1;

	uint32_t resop, op_status;
	if (xdr_read_u32(r, &resop) || resop != OP_CREATE_SESSION)
		return -1;
	if (xdr_read_u32(r, &op_status) || op_status != 0)
		return -1;

	/* csr_sessionid: 16 bytes (fixed, not opaque) */
	if (r->pos + 16 > r->len)
		return -1;
	memcpy(ss->sessionid, r->base + r->pos, 16);
	r->pos += 16;

	/* csr_sequence */
	if (xdr_read_u32(r, &ss->sequenceid))
		return -1;

	ss->slot_seqid = 1; /* First SEQUENCE will use seqid=1 */

	return 0;
}

/* SEQUENCE + PUTROOTFH + GETATTR */
static int encode_probe_compound(struct xdr_buf *x,
				 struct nfs4_session_state *ss)
{
	if (compound_start(x, 3))
		return -1;

	/* SEQUENCE */
	if (xdr_u32(x, OP_SEQUENCE))
		return -1;
	/* sa_sessionid: 16 bytes inline */
	if (x->pos + 16 > x->len)
		return -1;
	memcpy(x->base + x->pos, ss->sessionid, 16);
	x->pos += 16;
	/* sa_sequenceid */
	if (xdr_u32(x, ss->slot_seqid))
		return -1;
	/* sa_slotid */
	if (xdr_u32(x, 0))
		return -1;
	/* sa_highest_slotid */
	if (xdr_u32(x, 0))
		return -1;
	/* sa_cachethis */
	if (xdr_u32(x, 0))
		return -1; /* FALSE */

	/* PUTROOTFH */
	if (xdr_u32(x, OP_PUTROOTFH))
		return -1;

	/* GETATTR — request supported_attrs (bit 0, word 0) */
	if (xdr_u32(x, OP_GETATTR))
		return -1;
	/* attr_request bitmap: 1 word, bit 0 (supported_attrs) */
	if (xdr_u32(x, 1))
		return -1; /* bitmap len = 1 */
	if (xdr_u32(x, 1))
		return -1; /* word 0: bit 0 */

	return 0;
}

/* Parse probe reply — just check status, bump slot_seqid */
static int decode_probe_reply(struct xdr_reader *r,
			      struct nfs4_session_state *ss)
{
	uint32_t status;
	if (xdr_read_u32(r, &status))
		return -1;
	if (status != 0)
		return -(int)status;

	ss->slot_seqid++;
	return 0;
}

/* ------------------------------------------------------------------ */
/* High-level NFS4.2 session operations                               */
/* ------------------------------------------------------------------ */

/*
 * Send a single COMPOUND (RPC call + compound body) and receive reply.
 * Returns 0 on success, -1 on transport error, >0 on NFS error.
 */
static int nfs4_call(SSL *ssl, int fd, uint8_t *call_buf, size_t call_len,
		     uint8_t *reply_buf, size_t reply_bufsz,
		     struct xdr_reader *reply_out, uint32_t xid)
{
	if (tls_rpc_send(ssl, fd, call_buf, call_len))
		return -1;

	ssize_t n = tls_rpc_recv(ssl, fd, reply_buf, reply_bufsz);
	if (n < 0)
		return -1;

	xdr_reader_init(reply_out, reply_buf, (size_t)n);

	int accept = rpc_reply_check(reply_out, xid);
	if (accept != 0)
		return -1;

	return 0;
}

/* Establish NFSv4.2 session: EXCHANGE_ID + CREATE_SESSION */
static int nfs4_session_setup(SSL *ssl, int fd, struct nfs4_session_state *ss,
			      const char *client_owner, int verbose)
{
	uint8_t buf[XDR_BUFSZ], rbuf[RECV_BUFSZ];
	struct xdr_buf x;
	struct xdr_reader r;
	uint32_t xid;

	/* --- EXCHANGE_ID --- */
	xid = alloc_xid();
	xdr_init(&x, buf, sizeof(buf));
	if (rpc_call_header(&x, xid, NFS4_PROGRAM, NFS4_VERSION,
			    NFS4_COMPOUND_PROC))
		return -1;
	if (compound_start(&x, 1))
		return -1;
	if (encode_exchange_id(&x, client_owner))
		return -1;

	if (nfs4_call(ssl, fd, buf, x.pos, rbuf, sizeof(rbuf), &r, xid)) {
		if (verbose)
			fprintf(stderr, "  EXCHANGE_ID: transport error\n");
		return -1;
	}
	if (decode_exchange_id(&r, ss)) {
		if (verbose)
			fprintf(stderr, "  EXCHANGE_ID: decode error\n");
		return -1;
	}
	if (verbose)
		printf("  EXCHANGE_ID: clientid=0x%016lx seq=%u\n",
		       (unsigned long)ss->clientid, ss->sequenceid);

	/* --- CREATE_SESSION --- */
	xid = alloc_xid();
	xdr_init(&x, buf, sizeof(buf));
	if (rpc_call_header(&x, xid, NFS4_PROGRAM, NFS4_VERSION,
			    NFS4_COMPOUND_PROC))
		return -1;
	if (compound_start(&x, 1))
		return -1;
	if (encode_create_session(&x, ss))
		return -1;

	if (nfs4_call(ssl, fd, buf, x.pos, rbuf, sizeof(rbuf), &r, xid)) {
		if (verbose)
			fprintf(stderr, "  CREATE_SESSION: transport error\n");
		return -1;
	}
	if (decode_create_session(&r, ss)) {
		if (verbose)
			fprintf(stderr, "  CREATE_SESSION: decode error\n");
		return -1;
	}
	if (verbose)
		printf("  CREATE_SESSION: session established, slot_seqid=%u\n",
		       ss->slot_seqid);

	return 0;
}

/* Send SEQUENCE + PUTROOTFH + GETATTR probe */
static int nfs4_probe(SSL *ssl, int fd, struct nfs4_session_state *ss)
{
	uint8_t buf[XDR_BUFSZ], rbuf[RECV_BUFSZ];
	struct xdr_buf x;
	struct xdr_reader r;
	uint32_t xid;

	xid = alloc_xid();
	xdr_init(&x, buf, sizeof(buf));
	if (rpc_call_header(&x, xid, NFS4_PROGRAM, NFS4_VERSION,
			    NFS4_COMPOUND_PROC))
		return -1;
	if (encode_probe_compound(&x, ss))
		return -1;

	if (nfs4_call(ssl, fd, buf, x.pos, rbuf, sizeof(rbuf), &r, xid))
		return -1;

	return decode_probe_reply(&r, ss);
}

/* ------------------------------------------------------------------ */
/* Timing helpers                                                     */
/* ------------------------------------------------------------------ */

static uint64_t now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

/* ------------------------------------------------------------------ */
/* Test modes                                                         */
/* ------------------------------------------------------------------ */

struct stress_config {
	const char *host;
	int port;
	int iterations;
	int verbose;
	int trace;
	int direct_tls;
	SSL_CTX *ctx;
	const char *client_owner;
};

struct stress_stats {
	int successes;
	int failures;
	int reconnects;
	uint64_t total_us;
	uint64_t max_us;
};

/* Mode 1: STARTTLS loop — connect, STARTTLS, session setup, probe, close */
static void mode_starttls_loop(struct stress_config *cfg,
			       struct stress_stats *stats)
{
	for (int i = 0; i < cfg->iterations; i++) {
		uint64_t t0 = now_us();

		int fd = tls_tcp_connect(cfg->host, cfg->port);
		if (fd < 0) {
			stats->failures++;
			if (cfg->verbose)
				printf("  [%d] connect FAIL\n", i + 1);
			continue;
		}

		SSL *ssl =
			cfg->direct_tls ?
				tls_direct_connect(fd, cfg->ctx, cfg->verbose) :
				tls_starttls(fd, cfg->ctx, cfg->verbose);
		if (!ssl) {
			stats->failures++;
			close(fd);
			if (cfg->verbose)
				printf("  [%d] TLS FAIL\n", i + 1);
			continue;
		}
		if (cfg->trace)
			tls_trace_handshake(ssl, "starttls");

		struct nfs4_session_state ss = { 0 };
		if (nfs4_session_setup(ssl, fd, &ss, cfg->client_owner,
				       cfg->verbose)) {
			stats->failures++;
			SSL_free(ssl);
			close(fd);
			if (cfg->verbose)
				printf("  [%d] session FAIL\n", i + 1);
			continue;
		}

		int rc = nfs4_probe(ssl, fd, &ss);

		uint64_t elapsed = now_us() - t0;
		stats->total_us += elapsed;
		if (elapsed > stats->max_us)
			stats->max_us = elapsed;

		if (rc == 0) {
			stats->successes++;
			if (cfg->verbose)
				printf("  [%d/%d] OK %lu.%03lums\n", i + 1,
				       cfg->iterations,
				       (unsigned long)(elapsed / 1000),
				       (unsigned long)(elapsed % 1000));
		} else {
			stats->failures++;
			if (cfg->verbose)
				printf("  [%d/%d] FAIL (probe rc=%d)\n", i + 1,
				       cfg->iterations, rc);
		}

		SSL_shutdown(ssl);
		SSL_free(ssl);
		close(fd);
	}
}

/* Mode 2: mid-op disconnect — send probe, close before recv */
static void mode_mid_op_disconnect(struct stress_config *cfg,
				   struct stress_stats *stats)
{
	for (int i = 0; i < cfg->iterations; i++) {
		uint64_t t0 = now_us();

		/* Phase 1: connect, establish, send probe, HARD CLOSE */
		int fd = tls_tcp_connect(cfg->host, cfg->port);
		if (fd < 0) {
			stats->failures++;
			continue;
		}

		SSL *ssl =
			cfg->direct_tls ?
				tls_direct_connect(fd, cfg->ctx, cfg->verbose) :
				tls_starttls(fd, cfg->ctx, cfg->verbose);
		if (!ssl) {
			stats->failures++;
			close(fd);
			continue;
		}

		struct nfs4_session_state ss = { 0 };
		if (nfs4_session_setup(ssl, fd, &ss, cfg->client_owner, 0)) {
			stats->failures++;
			SSL_free(ssl);
			close(fd);
			continue;
		}

		/* Send the probe but DON'T wait for reply */
		uint8_t buf[XDR_BUFSZ];
		struct xdr_buf x;
		uint32_t xid = alloc_xid();
		xdr_init(&x, buf, sizeof(buf));
		rpc_call_header(&x, xid, NFS4_PROGRAM, NFS4_VERSION,
				NFS4_COMPOUND_PROC);
		encode_probe_compound(&x, &ss);
		tls_rpc_send(ssl, fd, buf, x.pos);

		/* Hard close — no SSL_shutdown */
		SSL_free(ssl);
		tls_tcp_reset(fd);

		/* Brief pause */
		usleep((unsigned)(rand() % 100000));

		/* Phase 2: reconnect and verify server recovered */
		fd = tls_tcp_connect(cfg->host, cfg->port);
		if (fd < 0) {
			stats->failures++;
			stats->reconnects++;
			if (cfg->verbose)
				printf("  [%d] reconnect FAIL\n", i + 1);
			continue;
		}

		ssl = cfg->direct_tls ?
			      tls_direct_connect(fd, cfg->ctx, cfg->verbose) :
			      tls_starttls(fd, cfg->ctx, cfg->verbose);
		if (!ssl) {
			stats->failures++;
			stats->reconnects++;
			close(fd);
			continue;
		}

		memset(&ss, 0, sizeof(ss));
		int ok = (nfs4_session_setup(ssl, fd, &ss, cfg->client_owner,
					     0) == 0 &&
			  nfs4_probe(ssl, fd, &ss) == 0);
		if (ok) {
			stats->successes++;
		} else {
			stats->failures++;
		}
		stats->reconnects++;

		uint64_t elapsed = now_us() - t0;
		stats->total_us += elapsed;
		if (elapsed > stats->max_us)
			stats->max_us = elapsed;

		if (cfg->verbose)
			printf("  [%d/%d] mid-op %s %lu.%03lums\n", i + 1,
			       cfg->iterations, ok ? "OK" : "FAIL",
			       (unsigned long)(elapsed / 1000),
			       (unsigned long)(elapsed % 1000));

		SSL_shutdown(ssl);
		SSL_free(ssl);
		close(fd);
	}
}

/* Mode 3: hot reconnect — close TLS, reconnect with SSL_connect only */
static void mode_hot_reconnect(struct stress_config *cfg,
			       struct stress_stats *stats)
{
	for (int i = 0; i < cfg->iterations; i++) {
		uint64_t t0 = now_us();

		/* Phase 1: normal STARTTLS + probe */
		int fd = tls_tcp_connect(cfg->host, cfg->port);
		if (fd < 0) {
			stats->failures++;
			continue;
		}

		SSL *ssl = tls_starttls(fd, cfg->ctx, cfg->verbose);
		if (!ssl) {
			stats->failures++;
			close(fd);
			continue;
		}

		struct nfs4_session_state ss = { 0 };
		if (nfs4_session_setup(ssl, fd, &ss, cfg->client_owner, 0)) {
			stats->failures++;
			SSL_free(ssl);
			close(fd);
			continue;
		}
		nfs4_probe(ssl, fd, &ss);

		/* Hard close */
		SSL_free(ssl);
		close(fd);

		/* Phase 2: reconnect with DIRECT TLS (no STARTTLS) */
		fd = tls_tcp_connect(cfg->host, cfg->port);
		if (fd < 0) {
			stats->failures++;
			stats->reconnects++;
			continue;
		}

		ssl = tls_direct_connect(fd, cfg->ctx, cfg->verbose);
		if (!ssl) {
			/* Expected failure if server requires STARTTLS */
			stats->reconnects++;
			if (cfg->verbose)
				printf("  [%d] hot-reconnect: server rejected"
				       " direct TLS (expected)\n",
				       i + 1);
			close(fd);

			/* Retry with STARTTLS to verify recovery */
			fd = tls_tcp_connect(cfg->host, cfg->port);
			if (fd < 0) {
				stats->failures++;
				continue;
			}
			ssl = tls_starttls(fd, cfg->ctx, cfg->verbose);
			if (!ssl) {
				stats->failures++;
				close(fd);
				continue;
			}
		} else {
			if (cfg->verbose)
				printf("  [%d] hot-reconnect: server accepted"
				       " direct TLS\n",
				       i + 1);
			stats->reconnects++;
		}

		/* Verify we can work on the (re)connection */
		memset(&ss, 0, sizeof(ss));
		if (nfs4_session_setup(ssl, fd, &ss, cfg->client_owner, 0) ||
		    nfs4_probe(ssl, fd, &ss)) {
			stats->failures++;
		} else {
			stats->successes++;
		}

		uint64_t elapsed = now_us() - t0;
		stats->total_us += elapsed;
		if (elapsed > stats->max_us)
			stats->max_us = elapsed;

		if (cfg->verbose)
			printf("  [%d/%d] hot %s %lu.%03lums\n", i + 1,
			       cfg->iterations,
			       (stats->failures == 0) ? "OK" : "FAIL",
			       (unsigned long)(elapsed / 1000),
			       (unsigned long)(elapsed % 1000));

		SSL_shutdown(ssl);
		SSL_free(ssl);
		close(fd);
	}
}

/* Mode 4: rapid cycle — RST close, no delay */
static void mode_rapid_cycle(struct stress_config *cfg,
			     struct stress_stats *stats)
{
	for (int i = 0; i < cfg->iterations; i++) {
		uint64_t t0 = now_us();

		int fd = tls_tcp_connect(cfg->host, cfg->port);
		if (fd < 0) {
			stats->failures++;
			continue;
		}

		SSL *ssl =
			cfg->direct_tls ?
				tls_direct_connect(fd, cfg->ctx, cfg->verbose) :
				tls_starttls(fd, cfg->ctx, cfg->verbose);
		if (!ssl) {
			stats->failures++;
			close(fd);
			continue;
		}

		struct nfs4_session_state ss = { 0 };
		if (nfs4_session_setup(ssl, fd, &ss, cfg->client_owner, 0) ||
		    nfs4_probe(ssl, fd, &ss)) {
			stats->failures++;
		} else {
			stats->successes++;
		}

		uint64_t elapsed = now_us() - t0;
		stats->total_us += elapsed;
		if (elapsed > stats->max_us)
			stats->max_us = elapsed;

		/* RST close — no SSL_shutdown, no FIN */
		SSL_free(ssl);
		tls_tcp_reset(fd);
		/* No delay — immediately loop */
	}

	if (cfg->verbose)
		printf("  rapid-cycle: %d OK, %d FAIL\n", stats->successes,
		       stats->failures);
}

/* ------------------------------------------------------------------ */
/* Main                                                               */
/* ------------------------------------------------------------------ */

static void print_summary(const char *mode_name, struct stress_stats *stats,
			  int iterations)
{
	int total = stats->successes + stats->failures;
	uint64_t avg_us = total > 0 ? stats->total_us / (uint64_t)total : 0;

	printf("\n=== %s ===\n", mode_name);
	printf("  Iterations: %d\n", iterations);
	printf("  Successes:  %d (%.1f%%)\n", stats->successes,
	       total > 0 ? 100.0 * stats->successes / total : 0.0);
	printf("  Failures:   %d\n", stats->failures);
	printf("  Reconnects: %d\n", stats->reconnects);
	printf("  Avg latency: %lu.%03lums\n", (unsigned long)(avg_us / 1000),
	       (unsigned long)(avg_us % 1000));
	printf("  Max latency: %lu.%03lums\n",
	       (unsigned long)(stats->max_us / 1000),
	       (unsigned long)(stats->max_us % 1000));
}

static void usage(void)
{
	fprintf(stderr,
		"Usage: nfs_tls_stress --host <addr> [options]\n"
		"\n"
		"Options:\n"
		"  --host, -H <addr>   Server address (required)\n"
		"  --port, -p <port>   NFS port (default: 2049)\n"
		"  --mode, -m <mode>   starttls-loop | mid-op-disconnect |\n"
		"                      hot-reconnect | rapid-cycle | all\n"
		"  --iterations, -n N  Iterations per mode (default: 100)\n"
		"  --cert, -c <path>   Client certificate\n"
		"  --key, -k <path>    Client key\n"
		"  --ca <path>         CA certificate\n"
		"  --direct-tls        Use direct TLS (skip STARTTLS)\n"
		"  --no-verify         Skip server certificate verification\n"
		"  --trace, -t         Log TLS handshake details per connection\n"
		"  --verbose, -v       Per-iteration output\n"
		"  --help, -h          This help\n");
}

int main(int argc, char *argv[])
{
	const char *host = NULL;
	const char *cert_path = NULL;
	const char *key_path = NULL;
	const char *ca_path = NULL;
	const char *mode = "starttls-loop";
	int port = NFS_PORT;
	int iterations = 100;
	int verbose = 0;
	int trace = 0;
	int direct_tls = 0;
	int no_verify = 0;

	static struct option opts[] = {
		{ "host", required_argument, NULL, 'H' },
		{ "port", required_argument, NULL, 'p' },
		{ "mode", required_argument, NULL, 'm' },
		{ "iterations", required_argument, NULL, 'n' },
		{ "cert", required_argument, NULL, 'c' },
		{ "key", required_argument, NULL, 'k' },
		{ "ca", required_argument, NULL, 'a' },
		{ "direct-tls", no_argument, NULL, 'D' },
		{ "no-verify", no_argument, NULL, 'N' },
		{ "trace", no_argument, NULL, 't' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};

	int opt;
	while ((opt = getopt_long(argc, argv, "H:p:m:n:c:k:a:Dtvh", opts,
				  NULL)) != -1) {
		switch (opt) {
		case 'H':
			host = optarg;
			break;
		case 'p':
			port = atoi(optarg);
			break;
		case 'm':
			mode = optarg;
			break;
		case 'n':
			iterations = atoi(optarg);
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
		case 'D':
			direct_tls = 1;
			break;
		case 'N':
			no_verify = 1;
			break;
		case 't':
			trace = 1;
			break;
		case 'v':
			verbose = 1;
			break;
		case 'h':
			usage();
			return 0;
		default:
			usage();
			return 2;
		}
	}

	if (!host) {
		fprintf(stderr, "Error: --host is required\n\n");
		usage();
		return 2;
	}

	struct tls_client_config tls_cfg = {
		.cert_path = cert_path,
		.key_path = key_path,
		.ca_path = ca_path,
		.no_verify = no_verify,
	};
	SSL_CTX *ctx = tls_client_ctx_create(&tls_cfg);
	if (!ctx)
		return 1;

	/* Build a unique client owner string */
	char client_owner[128];
	snprintf(client_owner, sizeof(client_owner), "tls-stress-%d-%ld",
		 getpid(), (long)time(NULL));

	struct stress_config cfg = {
		.host = host,
		.port = port,
		.iterations = iterations,
		.verbose = verbose,
		.trace = trace,
		.direct_tls = direct_tls,
		.ctx = ctx,
		.client_owner = client_owner,
	};

	srand((unsigned)time(NULL));

	printf("nfs_tls_stress: host=%s port=%d mode=%s iterations=%d\n", host,
	       port, mode, iterations);

	int failed = 0;

	if (strcmp(mode, "starttls-loop") == 0 || strcmp(mode, "all") == 0) {
		struct stress_stats stats = { 0 };
		printf("\nRunning: starttls-loop\n");
		mode_starttls_loop(&cfg, &stats);
		print_summary("starttls-loop", &stats, iterations);
		if (stats.failures)
			failed = 1;
	}

	if (strcmp(mode, "mid-op-disconnect") == 0 ||
	    strcmp(mode, "all") == 0) {
		struct stress_stats stats = { 0 };
		printf("\nRunning: mid-op-disconnect\n");
		mode_mid_op_disconnect(&cfg, &stats);
		print_summary("mid-op-disconnect", &stats, iterations);
		if (stats.failures)
			failed = 1;
	}

	if (strcmp(mode, "hot-reconnect") == 0 || strcmp(mode, "all") == 0) {
		struct stress_stats stats = { 0 };
		printf("\nRunning: hot-reconnect\n");
		mode_hot_reconnect(&cfg, &stats);
		print_summary("hot-reconnect", &stats, iterations);
		if (stats.failures)
			failed = 1;
	}

	if (strcmp(mode, "rapid-cycle") == 0 || strcmp(mode, "all") == 0) {
		struct stress_stats stats = { 0 };
		printf("\nRunning: rapid-cycle\n");
		mode_rapid_cycle(&cfg, &stats);
		print_summary("rapid-cycle", &stats, iterations);
		if (stats.failures)
			failed = 1;
	}

	SSL_CTX_free(ctx);

	return failed ? 1 : 0;
}
