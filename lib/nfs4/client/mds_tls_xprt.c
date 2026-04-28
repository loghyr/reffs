/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * Custom libtirpc CLIENT* over an SSL-protected TCP fd.  See
 * mds_tls_xprt.h for scope.  Implementation strategy:
 *
 *   cl_call:
 *     1. Encode RPC call header (xid, CALL, rpcvers, prog, vers,
 *        proc) by hand via xdr_u_int.  Marshal the auth flavour
 *        + body via AUTH_MARSHALL on the current cl_auth (or
 *        AUTH_NONE inline if cl_auth is NULL).  Encode args via
 *        xargs(xdrs, argsp).
 *     2. tls_rpc_send the buffer (writes record marker + body).
 *     3. tls_rpc_recv into a reply buffer (single fragment).
 *     4. Decode the reply header (xid, REPLY, accept_stat,
 *        verifier) by hand and skip AUTH_VALIDATE (the TLS
 *        channel already authenticates the server end-to-end;
 *        AUTH_SYS verifier is the all-zeros AUTH_NONE on the
 *        wire so validating it would be a no-op on success
 *        and add platform-specific 2-vs-3-arg AUTH_VALIDATE
 *        portability concerns).
 *     5. Decode results via xresults(xdrs, resultsp).
 *
 *   cl_destroy: SSL_shutdown -> SSL_free -> close(fd) -> free.
 *
 * Hand-rolled headers (instead of xdr_callmsg / xdr_replymsg)
 * avoid the libtirpc-vs-Darwin-libsystem struct rpc_msg shape
 * differences (Darwin's `acpted_rply.ar_results.proc` is a u_int,
 * libtirpc's is an xdrproc_t); the slice is meant to compile on
 * dreamer ASAN as canonical and not block on local Darwin builds.
 *
 * Buffers are sized to MDS_TLS_BUFSZ; the PS-MDS protocol surface
 * (EXCHANGE_ID + CREATE_SESSION + SEQUENCE + PROXY_*) fits well
 * under 64 KiB per request and reply.  Callers needing larger I/O
 * (CHUNK_WRITE / CHUNK_READ) do not go through this path -- those
 * stay on the dedicated DS sessions.
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <openssl/ssl.h>
#include <rpc/rpc.h>

#include "reffs/tls_client.h"

#include "mds_tls_xprt.h"

#define MDS_TLS_BUFSZ (64 * 1024)

/*
 * RPC wire constants -- not platform-specific.  Named with an
 * RPCWIRE_ prefix to avoid clashing with libtirpc / Apple RPC
 * macro definitions of the same bare names.
 */
#define RPCWIRE_MSG_VERSION 2
#define RPCWIRE_CALL 0
#define RPCWIRE_REPLY 1
#define RPCWIRE_MSG_ACCEPTED 0
#define RPCWIRE_MSG_DENIED 1
#define RPCWIRE_AUTH_NONE 0
#define RPCWIRE_PROC_SUCCESS 0

struct mds_tls_xprt_priv {
	int fd;
	SSL *ssl;
	uint32_t prog;
	uint32_t vers;
	uint32_t xid_next;
	struct rpc_err err;
	uint8_t *call_buf;
	uint8_t *reply_buf;
};

static void mds_tls_set_err(struct mds_tls_xprt_priv *priv, enum clnt_stat code,
			    int errnum)
{
	priv->err.re_status = code;
	priv->err.re_errno = errnum;
}

/*
 * Encode a 4-byte big-endian uint into priv->call_buf at *pos.
 * Bumps *pos.  Returns 0 on success or -1 if out of room.
 */
static int put_u32(uint8_t *buf, size_t bufsz, size_t *pos, uint32_t val)
{
	if (*pos + 4 > bufsz)
		return -1;
	buf[*pos + 0] = (uint8_t)(val >> 24);
	buf[*pos + 1] = (uint8_t)(val >> 16);
	buf[*pos + 2] = (uint8_t)(val >> 8);
	buf[*pos + 3] = (uint8_t)val;
	*pos += 4;
	return 0;
}

static int get_u32(const uint8_t *buf, size_t bufsz, size_t *pos, uint32_t *out)
{
	if (*pos + 4 > bufsz)
		return -1;
	*out = ((uint32_t)buf[*pos + 0] << 24) |
	       ((uint32_t)buf[*pos + 1] << 16) |
	       ((uint32_t)buf[*pos + 2] << 8) | (uint32_t)buf[*pos + 3];
	*pos += 4;
	return 0;
}

/*
 * Write an empty AUTH_NONE opaque_auth (flavor=0, length=0) at
 * *pos, bumping it.
 */
static int put_auth_none(uint8_t *buf, size_t bufsz, size_t *pos)
{
	if (put_u32(buf, bufsz, pos, RPCWIRE_AUTH_NONE) < 0)
		return -1;
	if (put_u32(buf, bufsz, pos, 0) < 0)
		return -1;
	return 0;
}

/*
 * Marshal cl_auth's cred + verf into the call buffer at *pos.
 * Falls back to AUTH_NONE when cl_auth is NULL (which is the
 * libtirpc default on a freshly-created CLIENT*).
 *
 * Implementation: AUTH_MARSHALL writes both cred and verf into an
 * xdrmem stream, so we wrap a sub-region of priv->call_buf.
 */
static int mds_tls_marshal_auth(AUTH *au, uint8_t *buf, size_t bufsz,
				size_t *pos)
{
	if (!au) {
		if (put_auth_none(buf, bufsz, pos) < 0)
			return -1;
		if (put_auth_none(buf, bufsz, pos) < 0)
			return -1;
		return 0;
	}

	XDR xdrs;

	if (*pos > bufsz)
		return -1;
	xdrmem_create(&xdrs, (char *)(buf + *pos), (u_int)(bufsz - *pos),
		      XDR_ENCODE);
	if (!AUTH_MARSHALL(au, &xdrs))
		return -1;
	*pos += xdr_getpos(&xdrs);
	return 0;
}

static int mds_tls_encode_call(CLIENT *clnt, uint32_t proc, xdrproc_t xargs,
			       void *argsp, uint32_t xid, size_t *len_out)
{
	struct mds_tls_xprt_priv *priv =
		(struct mds_tls_xprt_priv *)clnt->cl_private;
	size_t pos = 0;
	uint8_t *buf = priv->call_buf;

	if (put_u32(buf, MDS_TLS_BUFSZ, &pos, xid) < 0 ||
	    put_u32(buf, MDS_TLS_BUFSZ, &pos, RPCWIRE_CALL) < 0 ||
	    put_u32(buf, MDS_TLS_BUFSZ, &pos, RPCWIRE_MSG_VERSION) < 0 ||
	    put_u32(buf, MDS_TLS_BUFSZ, &pos, priv->prog) < 0 ||
	    put_u32(buf, MDS_TLS_BUFSZ, &pos, priv->vers) < 0 ||
	    put_u32(buf, MDS_TLS_BUFSZ, &pos, proc) < 0) {
		mds_tls_set_err(priv, RPC_CANTENCODEARGS, EIO);
		return -1;
	}

	if (mds_tls_marshal_auth(clnt->cl_auth, buf, MDS_TLS_BUFSZ, &pos) < 0) {
		mds_tls_set_err(priv, RPC_CANTENCODEARGS, EIO);
		return -1;
	}

	XDR xdrs;

	xdrmem_create(&xdrs, (char *)(buf + pos), (u_int)(MDS_TLS_BUFSZ - pos),
		      XDR_ENCODE);
	if (!xargs(&xdrs, argsp)) {
		mds_tls_set_err(priv, RPC_CANTENCODEARGS, EIO);
		return -1;
	}
	pos += xdr_getpos(&xdrs);

	*len_out = pos;
	return 0;
}

/*
 * Decode the reply header and leave the XDR stream positioned at
 * the start of the results.  Returns RPC_SUCCESS on success.
 */
static enum clnt_stat
mds_tls_decode_reply_header(struct mds_tls_xprt_priv *priv, size_t reply_len,
			    uint32_t expected_xid, size_t *body_pos_out)
{
	size_t pos = 0;
	uint8_t *buf = priv->reply_buf;
	uint32_t v;

	/* xid */
	if (get_u32(buf, reply_len, &pos, &v) < 0)
		goto bad;
	if (v != expected_xid)
		goto bad;

	/* direction = REPLY */
	if (get_u32(buf, reply_len, &pos, &v) < 0)
		goto bad;
	if (v != RPCWIRE_REPLY)
		goto bad;

	/* reply_stat: ACCEPTED or DENIED */
	if (get_u32(buf, reply_len, &pos, &v) < 0)
		goto bad;
	if (v == RPCWIRE_MSG_DENIED) {
		mds_tls_set_err(priv, RPC_AUTHERROR, EACCES);
		return RPC_AUTHERROR;
	}
	if (v != RPCWIRE_MSG_ACCEPTED)
		goto bad;

	/* opaque_auth verifier: flavor + length + body (skip body) */
	if (get_u32(buf, reply_len, &pos, &v) < 0)
		goto bad; /* flavor */
	if (get_u32(buf, reply_len, &pos, &v) < 0)
		goto bad; /* length */
	uint32_t verf_len = v;
	uint32_t verf_padded = (verf_len + 3) & ~3u;

	if (pos + verf_padded > reply_len)
		goto bad;
	pos += verf_padded;

	/* accept_stat */
	if (get_u32(buf, reply_len, &pos, &v) < 0)
		goto bad;
	if (v != RPCWIRE_PROC_SUCCESS) {
		/*
		 * Map every non-success accept_stat onto a generic
		 * RPC_PROGUNAVAIL since the slice plan-1-tls.a scope
		 * does not need finer error discrimination -- callers
		 * see the failure via clnt_perror and proceed to error
		 * handling.  Future polish (slice 1-tls.b) can split
		 * PROG_UNAVAIL / PROG_MISMATCH / PROC_UNAVAIL /
		 * GARBAGE_ARGS / SYSTEM_ERR when mds_session needs
		 * them.
		 */
		mds_tls_set_err(priv, RPC_PROGUNAVAIL, EIO);
		return RPC_PROGUNAVAIL;
	}

	*body_pos_out = pos;
	return RPC_SUCCESS;

bad:
	mds_tls_set_err(priv, RPC_CANTDECODERES, EIO);
	return RPC_CANTDECODERES;
}

static enum clnt_stat mds_tls_call(CLIENT *clnt, rpcproc_t proc,
				   xdrproc_t xargs, void *argsp,
				   xdrproc_t xresults, void *resultsp,
				   struct timeval timeout
				   __attribute__((unused)))
{
	struct mds_tls_xprt_priv *priv =
		(struct mds_tls_xprt_priv *)clnt->cl_private;
	uint32_t xid = priv->xid_next++;
	size_t call_len = 0;

	if (mds_tls_encode_call(clnt, (uint32_t)proc, xargs, argsp, xid,
				&call_len) < 0)
		return priv->err.re_status;

	if (tls_rpc_send(priv->ssl, priv->fd, priv->call_buf, call_len) < 0) {
		mds_tls_set_err(priv, RPC_CANTSEND, EIO);
		return RPC_CANTSEND;
	}

	ssize_t n = tls_rpc_recv(priv->ssl, priv->fd, priv->reply_buf,
				 MDS_TLS_BUFSZ);

	if (n < 0) {
		mds_tls_set_err(priv, RPC_CANTRECV, EIO);
		return RPC_CANTRECV;
	}

	size_t body_pos = 0;
	enum clnt_stat hdr_stat =
		mds_tls_decode_reply_header(priv, (size_t)n, xid, &body_pos);

	if (hdr_stat != RPC_SUCCESS)
		return hdr_stat;

	if (xresults) {
		XDR xdrs;

		xdrmem_create(&xdrs, (char *)(priv->reply_buf + body_pos),
			      (u_int)((size_t)n - body_pos), XDR_DECODE);
		if (!xresults(&xdrs, resultsp)) {
			mds_tls_set_err(priv, RPC_CANTDECODERES, EIO);
			return RPC_CANTDECODERES;
		}
	}

	mds_tls_set_err(priv, RPC_SUCCESS, 0);
	return RPC_SUCCESS;
}

static void mds_tls_geterr(CLIENT *clnt, struct rpc_err *errp)
{
	struct mds_tls_xprt_priv *priv =
		(struct mds_tls_xprt_priv *)clnt->cl_private;

	*errp = priv->err;
}

static bool_t mds_tls_freeres(CLIENT *clnt __attribute__((unused)),
			      xdrproc_t xresults, void *resultsp)
{
	XDR xdrs;

	memset(&xdrs, 0, sizeof(xdrs));
	xdrs.x_op = XDR_FREE;
	return xresults(&xdrs, resultsp);
}

static void mds_tls_abort(CLIENT *clnt __attribute__((unused)))
{
	/* No long-running async ops -- nothing to abort. */
}

static bool_t mds_tls_control(CLIENT *clnt __attribute__((unused)),
			      u_int request __attribute__((unused)),
			      void *info __attribute__((unused)))
{
	/*
	 * mds_session does not call clnt_control on this client; libtirpc
	 * routes specific requests (CLSET_TIMEOUT, etc.) here.  Return FALSE
	 * for "not implemented" so callers can fall back gracefully.
	 */
	return FALSE;
}

static void mds_tls_destroy(CLIENT *clnt)
{
	struct mds_tls_xprt_priv *priv;

	if (!clnt)
		return;
	priv = (struct mds_tls_xprt_priv *)clnt->cl_private;
	if (priv) {
		if (priv->ssl) {
			SSL_shutdown(priv->ssl);
			SSL_free(priv->ssl);
		}
		if (priv->fd >= 0)
			close(priv->fd);
		free(priv->call_buf);
		free(priv->reply_buf);
		free(priv);
	}
	free(clnt->cl_ops);
	free(clnt);
}

CLIENT *mds_tls_xprt_create(int fd, SSL *ssl, uint32_t prog, uint32_t vers)
{
	if (fd < 0 || !ssl)
		return NULL;

	CLIENT *clnt = calloc(1, sizeof(*clnt));

	if (!clnt)
		return NULL;

	struct clnt_ops *ops = calloc(1, sizeof(*ops));

	if (!ops) {
		free(clnt);
		return NULL;
	}

	struct mds_tls_xprt_priv *priv = calloc(1, sizeof(*priv));

	if (!priv) {
		free(ops);
		free(clnt);
		return NULL;
	}

	priv->call_buf = malloc(MDS_TLS_BUFSZ);
	priv->reply_buf = malloc(MDS_TLS_BUFSZ);
	if (!priv->call_buf || !priv->reply_buf) {
		free(priv->call_buf);
		free(priv->reply_buf);
		free(priv);
		free(ops);
		free(clnt);
		return NULL;
	}

	priv->fd = fd;
	priv->ssl = ssl;
	priv->prog = (uint32_t)prog;
	priv->vers = (uint32_t)vers;
	priv->xid_next = 1;
	priv->err.re_status = RPC_SUCCESS;

	ops->cl_call = mds_tls_call;
	ops->cl_abort = mds_tls_abort;
	ops->cl_geterr = mds_tls_geterr;
	ops->cl_freeres = mds_tls_freeres;
	ops->cl_destroy = mds_tls_destroy;
	ops->cl_control = mds_tls_control;

	clnt->cl_ops = ops;
	clnt->cl_private = priv;
	clnt->cl_auth = authnone_create();

	return clnt;
}
