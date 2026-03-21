/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <rpc/auth.h>
#include <rpc/xdr.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "nfsv42_xdr.h"
#include "reffs/log.h"
#include "reffs/io.h"
#include "reffs/rpc.h"
#include "nfs4/cb.h"
#include "nfs4/session.h"

/* CB_COMPOUND procedure number (NFS4_CALLBACK/NFS_CB from nfsv42_xdr.h) */
#define CB_COMPOUND_PROC 1u

/*
 * nfs4_cb_recall -- build and send a CB_COMPOUND [CB_SEQUENCE, CB_RECALL]
 * on the back channel, fire-and-forget.
 *
 * We do not pause the compound or wait for a reply.  The CB_COMPOUND
 * reply from the client is only an RPC acknowledgment; the actual
 * delegation return arrives later as a separate DELEGRETURN on the
 * fore channel.  Callers must return NFS4ERR_DELAY to the client after
 * this call and let it retry.
 *
 * Returns 0 on success, errno on failure (write not sent).
 */
int nfs4_cb_recall(struct nfs4_session *session, const stateid4 *stateid,
		   const nfs_fh4 *fh, bool truncate)
{
	CB_COMPOUND4args args = { 0 };
	nfs_cb_argop4 ops[2] = { 0 };
	CB_SEQUENCE4args *seq;
	CB_RECALL4args *rec;
	u_long xdr_size;
	size_t buf_len;
	char *buf;
	uint32_t *p;
	uint32_t body_len;
	static _Atomic uint32_t cb_xid = 0x10000000u;
	uint32_t xid;
	struct rpc_trans *cb_rt;
	XDR xdrs = { 0 };
	int ret;

	if (!session || !fh)
		return EINVAL;
	if (session->ns_cb_fd < 0)
		return ENOTCONN;

	/* Build CB_COMPOUND4args. */
	args.tag.utf8string_val = (char *)"CB_RECALL";
	args.tag.utf8string_len = sizeof("CB_RECALL") - 1;
	args.minorversion = 1;
	args.callback_ident = session->ns_cb_program;
	args.argarray.argarray_len = 2;
	args.argarray.argarray_val = ops;

	/* Op 0: CB_SEQUENCE */
	ops[0].argop = OP_CB_SEQUENCE;
	seq = &ops[0].nfs_cb_argop4_u.opcbsequence;
	memcpy(seq->csa_sessionid, session->ns_sessionid, sizeof(sessionid4));
	pthread_mutex_lock(&session->ns_cb_mutex);
	seq->csa_sequenceid = ++session->ns_cb_seqid;
	pthread_mutex_unlock(&session->ns_cb_mutex);
	seq->csa_slotid = 0;
	seq->csa_highest_slotid = 0;
	seq->csa_cachethis = false;
	seq->csa_referring_call_lists.csa_referring_call_lists_len = 0;
	seq->csa_referring_call_lists.csa_referring_call_lists_val = NULL;

	/* Op 1: CB_RECALL */
	ops[1].argop = OP_CB_RECALL;
	rec = &ops[1].nfs_cb_argop4_u.opcbrecall;
	memcpy(&rec->stateid, stateid, sizeof(*stateid));
	rec->truncate = truncate;
	rec->fh.nfs_fh4_len = fh->nfs_fh4_len;
	rec->fh.nfs_fh4_val =
		fh->nfs_fh4_val; /* borrowed; freed after encode */

	/*
	 * Calculate wire size and allocate the CALL buffer.
	 * Layout: fragment_marker(4) + RPC_CALL_header(10×4) + XDR_args
	 * The 11-word header is: frag, XID, call=0, rpc_vers=2, prog, vers,
	 * proc, cred_flavor, cred_len, verf_flavor, verf_len.
	 */
	xdr_size = xdr_sizeof((xdrproc_t)xdr_CB_COMPOUND4args, &args);
	buf_len = 11 * sizeof(uint32_t) + xdr_size;

	buf = calloc(buf_len, 1);
	if (!buf)
		return ENOMEM;

	xid = atomic_fetch_add_explicit(&cb_xid, 1u, memory_order_relaxed) + 1u;

	p = (uint32_t *)buf;
	body_len = (uint32_t)(buf_len - sizeof(uint32_t));
	*p++ = htonl(body_len | 0x80000000u); /* fragment marker, last frag */
	*p++ = htonl(xid);
	*p++ = htonl(0); /* CALL */
	*p++ = htonl(2); /* RPC version 2 */
	*p++ = htonl(NFS4_CALLBACK);
	*p++ = htonl(NFS_CB);
	*p++ = htonl(CB_COMPOUND_PROC);
	*p++ = htonl(AUTH_NONE); /* credential flavor */
	*p++ = htonl(0); /* credential length */
	*p++ = htonl(AUTH_NONE); /* verifier flavor */
	*p++ = htonl(0); /* verifier length */

	xdrmem_create(&xdrs, (char *)p, buf_len - 11 * sizeof(uint32_t),
		      XDR_ENCODE);
	if (!xdr_CB_COMPOUND4args(&xdrs, &args)) {
		xdr_destroy(&xdrs);
		free(buf);
		return EINVAL;
	}
	xdr_destroy(&xdrs);

	/* Create a minimal rpc_trans for the outgoing CB call. */
	cb_rt = calloc(1, sizeof(*cb_rt));
	if (!cb_rt) {
		free(buf);
		return ENOMEM;
	}

	cb_rt->rt_fd = session->ns_cb_fd;
	cb_rt->rt_reply = buf;
	cb_rt->rt_reply_len = buf_len;
	cb_rt->rt_info.ri_xid = xid;

	/*
	 * Fire-and-forget: submit the write, then discard cb_rt.
	 * io_rpc_trans_cb takes ownership of buf (sets rt_reply = NULL).
	 * We don't register for a reply; the CB_COMPOUND reply from the
	 * client is just an RPC ack that we intentionally ignore.
	 */
	ret = io_rpc_trans_cb(cb_rt);
	free(cb_rt);

	return ret;
}
