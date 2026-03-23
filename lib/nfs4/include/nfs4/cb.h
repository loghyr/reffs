/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_NFS4_CB_H
#define _REFFS_NFS4_CB_H

#include <stdatomic.h>
#include <stdint.h>

#include "nfsv42_xdr.h"

struct nfs4_session;
struct task;
struct compound;
struct rpc_trans;

/* ------------------------------------------------------------------ */
/* cb_pending — tracks a CB that is waiting for a client reply         */
/*                                                                     */
/* Ownership: allocated by the op that initiates the CB (via           */
/* cb_pending_alloc), freed by the op's resume callback (via           */
/* cb_pending_free) after reading cp_status and cp_res.                */
/* Neither cb_reply_handler nor the timeout thread frees it.           */
/* ------------------------------------------------------------------ */

/*
 * cp_status values:
 *   CB_PENDING_INFLIGHT  — CB sent, waiting for reply or timeout
 *   0                    — reply received successfully
 *   -ETIMEDOUT           — timeout fired before reply arrived
 *   -EIO                 — reply arrived but XDR decode failed
 */
#define CB_PENDING_INFLIGHT (-EINPROGRESS)

struct cb_pending {
	struct task *cp_task; /* paused compound's task */
	struct compound *cp_compound; /* paused compound */
	nfs_cb_opnum4 cp_op; /* which CB (CB_GETATTR, etc.) */
	CB_COMPOUND4res cp_res; /* decoded reply from client */
	_Atomic int cp_status; /* see values above */
	uint32_t cp_xid; /* XID for timeout unregistration */

	/* Timeout list linkage (protected by cb_timeout mutex). */
	struct cb_pending *cp_next;
	struct cb_pending *cp_prev;
	uint64_t cp_deadline_ns; /* CLOCK_MONOTONIC deadline */
};

struct cb_pending *cb_pending_alloc(struct task *task,
				    struct compound *compound,
				    nfs_cb_opnum4 op);
void cb_pending_free(struct cb_pending *cp);

/*
 * cb_pending_try_complete — atomically transition cp_status from
 * CB_PENDING_INFLIGHT to @status.  Returns true if this caller won
 * the race and should call task_resume; false if the other path
 * (reply handler or timeout) already completed this cb_pending.
 */
static inline int cb_pending_try_complete(struct cb_pending *cp, int status)
{
	int expected = CB_PENDING_INFLIGHT;

	return atomic_compare_exchange_strong_explicit(&cp->cp_status,
						       &expected, status,
						       memory_order_acq_rel,
						       memory_order_relaxed);
}

/* ------------------------------------------------------------------ */
/* CB reply handler — called by the RPC REPLY dispatcher               */
/* ------------------------------------------------------------------ */

int cb_reply_handler(struct rpc_trans *rt);

/* ------------------------------------------------------------------ */
/* Fire-and-forget callbacks                                           */
/* ------------------------------------------------------------------ */

/*
 * nfs4_cb_recall -- send CB_RECALL, fire-and-forget.
 * Callers must return NFS4ERR_DELAY after this call.
 */
int nfs4_cb_recall(struct nfs4_session *session, const stateid4 *stateid,
		   const nfs_fh4 *fh, int truncate);

/* ------------------------------------------------------------------ */
/* Wait-for-reply callbacks                                            */
/* ------------------------------------------------------------------ */

/*
 * nfs4_cb_getattr_send -- send CB_GETATTR and register for reply.
 *
 * The caller must have already called task_pause() on the compound.
 * When the reply arrives (or timeout fires), task_resume() is called
 * and the compound's resume callback reads cp->cp_res / cp->cp_status.
 *
 * cp must have been allocated by cb_pending_alloc().  The caller's
 * resume callback is responsible for calling cb_pending_free().
 */
int nfs4_cb_getattr_send(struct nfs4_session *session, const nfs_fh4 *fh,
			 const bitmap4 *attr_request, struct cb_pending *cp);

/* ------------------------------------------------------------------ */
/* Timeout infrastructure                                              */
/* ------------------------------------------------------------------ */

int cb_timeout_init(void);
void cb_timeout_fini(void);
void cb_timeout_register(struct cb_pending *cp);
void cb_timeout_unregister(struct cb_pending *cp);

#endif /* _REFFS_NFS4_CB_H */
