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

#include <urcu/ref.h>

#include "nfsv42_xdr.h"
#include "reffs/darwin_rpc_compat.h" /* xdr_sizeof shim on __APPLE__ */
#include "reffs/log.h"
#include "reffs/io.h"
#include "reffs/rpc.h"
#include "reffs/task.h"
#include "nfs4/cb.h"
#include "nfs4/session.h"

/* CB_COMPOUND procedure number (NFS4_CALLBACK/NFS_CB from nfsv42_xdr.h) */
#define CB_COMPOUND_PROC 1u

/* RPC CALL header: frag + 10 words (XID, type, rpc_ver, prog, ver, proc,
 * cred_flavor, cred_len, verf_flavor, verf_len) */
#define RPC_CALL_HEADER_WORDS 11

static _Atomic uint32_t cb_xid = 0x10000000u;

/* ------------------------------------------------------------------ */
/* Common CB_COMPOUND builder                                          */
/* ------------------------------------------------------------------ */

/*
 * Fill the CB_SEQUENCE op (always op[0] in a CB_COMPOUND).
 */
static void cb_fill_sequence(nfs_cb_argop4 *op, struct nfs4_session *session)
{
	CB_SEQUENCE4args *seq;

	op->argop = OP_CB_SEQUENCE;
	seq = &op->nfs_cb_argop4_u.opcbsequence;
	memcpy(seq->csa_sessionid, session->ns_sessionid, sizeof(sessionid4));
	pthread_mutex_lock(&session->ns_cb_mutex);
	seq->csa_sequenceid = ++session->ns_cb_seqid;
	pthread_mutex_unlock(&session->ns_cb_mutex);
	seq->csa_slotid = 0;
	seq->csa_highest_slotid = 0;
	seq->csa_cachethis = false;
	seq->csa_referring_call_lists.csa_referring_call_lists_len = 0;
	seq->csa_referring_call_lists.csa_referring_call_lists_val = NULL;
}

/*
 * XDR-encode CB_COMPOUND4args into an RPC CALL buffer and allocate
 * an rpc_trans for submission.
 *
 * On success, *out_rt holds the rpc_trans (caller must either submit
 * via io_rpc_trans_cb or free it).  *out_xid holds the XID.
 *
 * Returns 0 on success, errno on failure.
 */
static int cb_build_and_alloc(struct nfs4_session *session,
			      CB_COMPOUND4args *args, struct rpc_trans **out_rt,
			      uint32_t *out_xid)
{
	u_long xdr_size;
	size_t buf_len;
	char *buf;
	uint32_t *p;
	uint32_t body_len;
	uint32_t xid;
	struct rpc_trans *cb_rt;
	XDR xdrs = { 0 };

	xdr_size = xdr_sizeof((xdrproc_t)xdr_CB_COMPOUND4args, args);
	buf_len = RPC_CALL_HEADER_WORDS * sizeof(uint32_t) + xdr_size;

	buf = calloc(buf_len, 1);
	if (!buf)
		return ENOMEM;

	xid = atomic_fetch_add_explicit(&cb_xid, 1u, memory_order_relaxed) + 1u;

	p = (uint32_t *)buf;
	body_len = (uint32_t)(buf_len - sizeof(uint32_t));
	*p++ = htonl(body_len | 0x80000000u); /* last fragment */
	*p++ = htonl(xid);
	*p++ = htonl(0); /* CALL */
	*p++ = htonl(2); /* RPC version 2 */
	*p++ = htonl(NFS4_CALLBACK);
	*p++ = htonl(NFS_CB);
	*p++ = htonl(CB_COMPOUND_PROC);
	*p++ = htonl(AUTH_NONE);
	*p++ = htonl(0); /* cred length */
	*p++ = htonl(AUTH_NONE);
	*p++ = htonl(0); /* verf length */

	xdrmem_create(&xdrs, (char *)p,
		      buf_len - RPC_CALL_HEADER_WORDS * sizeof(uint32_t),
		      XDR_ENCODE);
	if (!xdr_CB_COMPOUND4args(&xdrs, args)) {
		xdr_destroy(&xdrs);
		free(buf);
		return EINVAL;
	}
	xdr_destroy(&xdrs);

	cb_rt = calloc(1, sizeof(*cb_rt));
	if (!cb_rt) {
		free(buf);
		return ENOMEM;
	}

	/*
	 * Initialize the urcu ref so io_find_request_by_xid -> rpc_trans_get
	 * on this cb_rt (via the reply-matching path in rpc.c) operates on a
	 * valid refcount.  Raw calloc leaves rt_ref zeroed; any subsequent
	 * urcu_ref_get on that is UB.  See #31.
	 */
	urcu_ref_init(&cb_rt->rt_ref);

	cb_rt->rt_fd = session->ns_cb_fd;
	cb_rt->rt_reply = buf;
	cb_rt->rt_reply_len = buf_len;
	cb_rt->rt_info.ri_xid = xid;
	cb_rt->rt_rc = io_network_get_global();

	*out_rt = cb_rt;
	*out_xid = xid;
	return 0;
}

/* ------------------------------------------------------------------ */
/* cb_pending lifecycle                                                 */
/* ------------------------------------------------------------------ */

struct cb_pending *cb_pending_alloc(struct task *task,
				    struct compound *compound, nfs_cb_opnum4 op)
{
	struct cb_pending *cp = calloc(1, sizeof(*cp));

	if (!cp)
		return NULL;

	cp->cp_task = task;
	cp->cp_compound = compound;
	cp->cp_op = op;
	atomic_store_explicit(&cp->cp_status, CB_PENDING_INFLIGHT,
			      memory_order_relaxed);
	cp->cp_xid = 0;
	return cp;
}

void cb_pending_free(struct cb_pending *cp)
{
	if (!cp)
		return;

	xdr_free((xdrproc_t)xdr_CB_COMPOUND4res, (caddr_t)&cp->cp_res);
	free(cp);
}

/* ------------------------------------------------------------------ */
/* CB reply handler                                                    */
/*                                                                     */
/* Called by the RPC REPLY dispatcher (rpc.c) when a CB_COMPOUND reply */
/* arrives on the backchannel.  The rt_context has been transferred    */
/* from the registered rpc_trans to this reply rt.                     */
/* ------------------------------------------------------------------ */

int cb_reply_handler(struct rpc_trans *rt)
{
	struct cb_pending *cp = rt->rt_context;

	rt->rt_context = NULL; /* take ownership */

	if (!cp) {
		TRACE("CB reply with no cb_pending, xid=0x%08x",
		      rt->rt_info.ri_xid);
		return 0;
	}

	/*
	 * Decode CB_COMPOUND4res from the raw reply body.
	 *
	 * The RPC REPLY header (XID, type=1, reply_stat, verf, accept_stat)
	 * has already been parsed by rpc_process_task.  rt->rt_body points
	 * at the start of the record, and rt->rt_offset is past the header
	 * fields already consumed.  The remaining bytes from rt_offset are
	 * the XDR-encoded CB_COMPOUND4res.
	 */
	int status = -EIO;

	if (rt->rt_offset < rt->rt_body_len) {
		XDR xdrs;
		size_t remaining = rt->rt_body_len - rt->rt_offset;

		xdrmem_create(&xdrs, rt->rt_body + rt->rt_offset, remaining,
			      XDR_DECODE);
		if (xdr_CB_COMPOUND4res(&xdrs, &cp->cp_res))
			status = 0;
		xdr_destroy(&xdrs);
	}

	/* Remove from timeout tracking. */
	cb_timeout_unregister(cp);

	/*
	 * CAS ensures only one of reply handler / timeout calls
	 * task_resume.  If the timeout thread already won, we
	 * silently drop the late reply.
	 */
	if (cb_pending_try_complete(cp, status))
		task_resume(cp->cp_task);
	return 0;
}

/* ------------------------------------------------------------------ */
/* CB_RECALL -- fire-and-forget (unchanged behavior)                    */
/* ------------------------------------------------------------------ */

int nfs4_cb_recall(struct nfs4_session *session, const stateid4 *stateid,
		   const nfs_fh4 *fh, int truncate)
{
	CB_COMPOUND4args args = { 0 };
	nfs_cb_argop4 ops[2] = { 0 };
	CB_RECALL4args *rec;
	struct rpc_trans *cb_rt;
	uint32_t xid;
	int ret;

	if (!session || !fh)
		return EINVAL;
	if (session->ns_cb_fd < 0)
		return ENOTCONN;

	args.tag.utf8string_val = (char *)"CB_RECALL";
	args.tag.utf8string_len = sizeof("CB_RECALL") - 1;
	args.minorversion = 1;
	args.callback_ident = session->ns_cb_program;
	args.argarray.argarray_len = 2;
	args.argarray.argarray_val = ops;

	cb_fill_sequence(&ops[0], session);

	ops[1].argop = OP_CB_RECALL;
	rec = &ops[1].nfs_cb_argop4_u.opcbrecall;
	memcpy(&rec->stateid, stateid, sizeof(*stateid));
	rec->truncate = truncate;
	rec->fh.nfs_fh4_len = fh->nfs_fh4_len;
	rec->fh.nfs_fh4_val = fh->nfs_fh4_val;

	ret = cb_build_and_alloc(session, &args, &cb_rt, &xid);
	if (ret)
		return ret;

	/* Fire-and-forget: don't register, don't wait. */
	ret = io_rpc_trans_cb(cb_rt);
	/*
	 * Drop creator-ref.  rt_context is NULL on this path (no cb_pending
	 * is set for fire-and-forget), so rpc_trans_release's protocol-
	 * handler cleanup skips cleanly.  rt_reply was transferred to the
	 * io_context by io_rpc_trans_cb on success; on failure it is still
	 * ours and release frees it.
	 */
	rpc_protocol_free(cb_rt);

	return ret;
}

/*
 * Fire-and-forget variant of nfs4_cb_layoutrecall_send.  Same wire
 * shape (CB_COMPOUND { SEQUENCE, CB_LAYOUTRECALL { LAYOUTRECALL4_FILE,
 * fh, range, lo_stateid } }), but no cb_pending and no caller wait
 * for the ack -- mirrors nfs4_cb_recall above.
 *
 * Intended for the migration-commit recall path (slice 6c-x.5):
 * PROXY_DONE(NFS4_OK) issues recalls to every external client whose
 * cached layout includes a now-removed DRAINING DS.  The PS does
 * not block on the ack; the next LAYOUTGET each client issues sees
 * the post-image (omit-and-replace policy) and the lease reaper
 * handles unresponsive clients.
 */
int nfs4_cb_layoutrecall_fnf(struct nfs4_session *session,
			     layouttype4 layout_type, layoutiomode4 iomode,
			     int changed, const nfs_fh4 *fh, uint64_t offset,
			     uint64_t length, const stateid4 *lo_stateid)
{
	CB_COMPOUND4args args = { 0 };
	nfs_cb_argop4 ops[2] = { 0 };
	CB_LAYOUTRECALL4args *lr;
	struct rpc_trans *cb_rt;
	uint32_t xid;
	int ret;

	if (!session || !fh || !lo_stateid)
		return EINVAL;
	if (session->ns_cb_fd < 0)
		return ENOTCONN;

	args.tag.utf8string_val = (char *)"CB_LAYOUTRECALL";
	args.tag.utf8string_len = sizeof("CB_LAYOUTRECALL") - 1;
	args.minorversion = 1;
	args.callback_ident = session->ns_cb_program;
	args.argarray.argarray_len = 2;
	args.argarray.argarray_val = ops;

	cb_fill_sequence(&ops[0], session);

	ops[1].argop = OP_CB_LAYOUTRECALL;
	lr = &ops[1].nfs_cb_argop4_u.opcblayoutrecall;
	lr->clora_type = layout_type;
	lr->clora_iomode = iomode;
	lr->clora_changed = changed;

	lr->clora_recall.lor_recalltype = LAYOUTRECALL4_FILE;
	lr->clora_recall.layoutrecall4_u.lor_layout.lor_fh.nfs_fh4_len =
		fh->nfs_fh4_len;
	lr->clora_recall.layoutrecall4_u.lor_layout.lor_fh.nfs_fh4_val =
		fh->nfs_fh4_val;
	lr->clora_recall.layoutrecall4_u.lor_layout.lor_offset = offset;
	lr->clora_recall.layoutrecall4_u.lor_layout.lor_length = length;
	memcpy(&lr->clora_recall.layoutrecall4_u.lor_layout.lor_stateid,
	       lo_stateid, sizeof(stateid4));

	ret = cb_build_and_alloc(session, &args, &cb_rt, &xid);
	if (ret)
		return ret;

	/* Fire-and-forget: don't register, don't wait for ack. */
	ret = io_rpc_trans_cb(cb_rt);
	rpc_protocol_free(cb_rt);
	return ret;
}

/* ------------------------------------------------------------------ */
/* CB_GETATTR -- send and wait for reply                                */
/* ------------------------------------------------------------------ */

int nfs4_cb_getattr_send(struct nfs4_session *session, const nfs_fh4 *fh,
			 const bitmap4 *attr_request, struct cb_pending *cp)
{
	CB_COMPOUND4args args = { 0 };
	nfs_cb_argop4 ops[2] = { 0 };
	CB_GETATTR4args *ga;
	struct rpc_trans *cb_rt;
	uint32_t xid;
	int ret;

	if (!session || !fh || !cp)
		return EINVAL;
	if (session->ns_cb_fd < 0)
		return ENOTCONN;

	args.tag.utf8string_val = (char *)"CB_GETATTR";
	args.tag.utf8string_len = sizeof("CB_GETATTR") - 1;
	args.minorversion = 1;
	args.callback_ident = session->ns_cb_program;
	args.argarray.argarray_len = 2;
	args.argarray.argarray_val = ops;

	cb_fill_sequence(&ops[0], session);

	ops[1].argop = OP_CB_GETATTR;
	ga = &ops[1].nfs_cb_argop4_u.opcbgetattr;
	ga->fh.nfs_fh4_len = fh->nfs_fh4_len;
	ga->fh.nfs_fh4_val = fh->nfs_fh4_val;
	memcpy(&ga->attr_request, attr_request, sizeof(bitmap4));

	ret = cb_build_and_alloc(session, &args, &cb_rt, &xid);
	if (ret)
		return ret;

	cp->cp_xid = xid;

	/* Set up for reply matching. */
	cb_rt->rt_context = cp;
	cb_rt->rt_cb = cb_reply_handler;

	ret = io_register_request(cb_rt);
	if (ret) {
		/* cp is owned by the caller's cb_pending_alloc, not cb_rt;
		 * clear rt_context so rpc_trans_release doesn't touch it. */
		cb_rt->rt_context = NULL;
		rpc_protocol_free(cb_rt);
		return ret;
	}

	/* Register for timeout tracking. */
	cb_timeout_register(cp);

	/* Send -- io_rpc_trans_cb takes ownership of rt_reply buffer.
	 * The rpc_trans itself stays in the pending_requests table
	 * until the reply arrives or timeout fires. */
	ret = io_rpc_trans_cb(cb_rt);
	if (ret) {
		io_unregister_request(xid);
		cb_timeout_unregister(cp);
		cb_rt->rt_context = NULL;
		rpc_protocol_free(cb_rt);
		return ret;
	}

	/* cb_rt stays alive in pending_requests -- do NOT free it here. */
	return 0;
}

/* ------------------------------------------------------------------ */
/* CB_LAYOUTRECALL -- send and wait for reply                           */
/* ------------------------------------------------------------------ */

int nfs4_cb_layoutrecall_send(struct nfs4_session *session,
			      layouttype4 layout_type, layoutiomode4 iomode,
			      int changed, const nfs_fh4 *fh, uint64_t offset,
			      uint64_t length, const stateid4 *lo_stateid,
			      struct cb_pending *cp)
{
	CB_COMPOUND4args args = { 0 };
	nfs_cb_argop4 ops[2] = { 0 };
	CB_LAYOUTRECALL4args *lr;
	struct rpc_trans *cb_rt;
	uint32_t xid;
	int ret;

	if (!session || !cp)
		return EINVAL;
	if (session->ns_cb_fd < 0)
		return ENOTCONN;

	args.tag.utf8string_val = (char *)"CB_LAYOUTRECALL";
	args.tag.utf8string_len = sizeof("CB_LAYOUTRECALL") - 1;
	args.minorversion = 1;
	args.callback_ident = session->ns_cb_program;
	args.argarray.argarray_len = 2;
	args.argarray.argarray_val = ops;

	cb_fill_sequence(&ops[0], session);

	ops[1].argop = OP_CB_LAYOUTRECALL;
	lr = &ops[1].nfs_cb_argop4_u.opcblayoutrecall;
	lr->clora_type = layout_type;
	lr->clora_iomode = iomode;
	lr->clora_changed = changed;

	/*
	 * File-level recall.  For FSID and ALL recalls, the caller
	 * would set clora_recall differently; for now we only support
	 * per-file recall.
	 */
	lr->clora_recall.lor_recalltype = LAYOUTRECALL4_FILE;
	lr->clora_recall.layoutrecall4_u.lor_layout.lor_fh.nfs_fh4_len =
		fh->nfs_fh4_len;
	lr->clora_recall.layoutrecall4_u.lor_layout.lor_fh.nfs_fh4_val =
		fh->nfs_fh4_val;
	lr->clora_recall.layoutrecall4_u.lor_layout.lor_offset = offset;
	lr->clora_recall.layoutrecall4_u.lor_layout.lor_length = length;
	memcpy(&lr->clora_recall.layoutrecall4_u.lor_layout.lor_stateid,
	       lo_stateid, sizeof(stateid4));

	ret = cb_build_and_alloc(session, &args, &cb_rt, &xid);
	if (ret)
		return ret;

	cp->cp_xid = xid;

	cb_rt->rt_context = cp;
	cb_rt->rt_cb = cb_reply_handler;

	ret = io_register_request(cb_rt);
	if (ret) {
		cb_rt->rt_context = NULL;
		rpc_protocol_free(cb_rt);
		return ret;
	}

	cb_timeout_register(cp);

	ret = io_rpc_trans_cb(cb_rt);
	if (ret) {
		io_unregister_request(xid);
		cb_timeout_unregister(cp);
		cb_rt->rt_context = NULL;
		rpc_protocol_free(cb_rt);
		return ret;
	}

	return 0;
}
