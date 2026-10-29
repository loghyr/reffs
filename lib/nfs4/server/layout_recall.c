/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * On-demand CB_LAYOUTRECALL with DELAY resend (RFC 8881 sec 12.5.5,
 * 20.3).
 *
 * Recalls live in a fixed table.  A driver thread does every send;
 * reply and timeout completions arrive through cb_pending's
 * cp_complete hook, which only marks the record and wakes the driver.
 * Lock order is cb_timeout_mutex -> lr_mutex: the hook runs under
 * cb_timeout_mutex on the timeout thread.  So nothing here calls into
 * cb_* (send, unregister) while holding lr_mutex.
 */

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <urcu/rculfhash.h>

#include "nfsv42_xdr.h"
#include "nfsv42_names.h"
#include "reffs/client.h"
#include "reffs/filehandle.h"
#include "reffs/inode.h"
#include "reffs/io.h"
#include "reffs/log.h"
#include "reffs/rcu.h"
#include "reffs/rpc.h"
#include "reffs/server.h"
#include "reffs/stateid.h"
#include "reffs/super_block.h"
#include "reffs/time.h"
#include "reffs/trace/common.h"
#include "nfs4/cb.h"
#include "nfs4/client.h"
#include "nfs4/errors.h"
#include "nfs4/layout_recall.h"
#include "nfs4/ops.h"
#include "nfs4/session.h"
#include "nfs4/stateid.h"

#define LR_DEFAULT_INTERVAL_MS 1000u

struct layout_recall {
	uint32_t lr_id;
	enum layout_recall_state lr_state;
	struct layout_recall_params lr_p;
	stateid4 lr_file_stateid; /* FILE: seqid bumped once, at start */
	uint32_t lr_sends;
	uint64_t lr_next_send_ns;
	uint64_t lr_deadline_ns; /* 0: no time limit */
	bool lr_in_flight;
	bool lr_reply_ready;
	bool lr_draining; /* NOMATCHING seen; drop pending outside lr_mutex */
	struct cb_pending *lr_cp;
	uint32_t lr_nevents;
	uint32_t lr_events_lost;
	struct layout_recall_event lr_events[LAYOUT_RECALL_EVENTS];
};

/* What a send needs, copied out so the send runs without lr_mutex. */
struct lr_send {
	struct layout_recall *ls_rec;
	uint32_t ls_id;
	struct layout_recall_params ls_p;
	stateid4 ls_file_stateid;
	uint64_t ls_deadline_ns;
};

static pthread_mutex_t lr_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t lr_cv = PTHREAD_COND_INITIALIZER;
static struct layout_recall lr_table[LAYOUT_RECALL_MAX];
static uint32_t lr_next_id;
static uint32_t lr_latest_id;
static bool lr_kick;
static bool lr_running;
static bool lr_thread_started;
static pthread_t lr_thread;
/* Lets LAYOUTGET skip lr_mutex when no recall is outstanding. */
static _Atomic uint32_t lr_outstanding;

static bool lr_manual;
static uint64_t (*lr_clock)(void) = reffs_now_ns;
static layout_recall_send_fn lr_send_fn = nfs4_cb_layoutrecall_send_recall;

static uint64_t lr_now(void)
{
	return lr_clock();
}

static const char *lr_type_name(layoutrecall_type4 t)
{
	switch (t) {
	case LAYOUTRECALL4_FILE:
		return "FILE";
	case LAYOUTRECALL4_FSID:
		return "FSID";
	case LAYOUTRECALL4_ALL:
		return "ALL";
	}
	return "?";
}

static const char *lr_event_name(uint32_t kind)
{
	static const char *const names[] = {
		[LAYOUT_RECALL_EV_SEND] = "SEND",
		[LAYOUT_RECALL_EV_REPLY] = "REPLY",
		[LAYOUT_RECALL_EV_TIMEOUT] = "TIMEOUT",
		[LAYOUT_RECALL_EV_RETURN] = "RETURN",
		[LAYOUT_RECALL_EV_DONE] = "DONE",
		[LAYOUT_RECALL_EV_EXPIRED] = "EXPIRED",
		[LAYOUT_RECALL_EV_FAILED] = "FAILED",
		[LAYOUT_RECALL_EV_CANCELLED] = "CANCELLED",
	};

	if (kind < sizeof(names) / sizeof(names[0]) && names[kind])
		return names[kind];
	return "?";
}

static bool lr_is_outstanding(enum layout_recall_state s)
{
	return s == LAYOUT_RECALL_SENDING || s == LAYOUT_RECALL_WAIT_RESEND ||
	       s == LAYOUT_RECALL_ACKED;
}

static void lr_set_state_locked(struct layout_recall *r,
				enum layout_recall_state s)
{
	bool was = lr_is_outstanding(r->lr_state);
	bool is = lr_is_outstanding(s);

	r->lr_state = s;
	if (was && !is)
		atomic_fetch_sub_explicit(&lr_outstanding, 1,
					  memory_order_release);
	else if (!was && is)
		atomic_fetch_add_explicit(&lr_outstanding, 1,
					  memory_order_release);
}

/* nfs4_err_name() knows neither NFS4_OK nor -errno, and NULLs on unknowns. */
static const char *lr_status_name(int32_t st)
{
	const char *name;

	if (st < 0)
		return "errno";
	if (st == 0)
		return "NFS4_OK";
	name = nfs4_err_name((uint32_t)st);
	return name ? name : "?";
}

/* Append to the record's log and mirror the line into the trace file. */
static void lr_event_locked(struct layout_recall *r, uint32_t kind,
			    int32_t cb_status, int32_t op_status,
			    uint32_t dropped)
{
	struct layout_recall_event *ev;
	struct timespec ts;

	if (r->lr_nevents == LAYOUT_RECALL_EVENTS) {
		memmove(&r->lr_events[0], &r->lr_events[1],
			sizeof(r->lr_events[0]) * (LAYOUT_RECALL_EVENTS - 1));
		r->lr_nevents--;
		r->lr_events_lost++;
	}
	ev = &r->lr_events[r->lr_nevents++];
	clock_gettime(CLOCK_REALTIME, &ts);
	ev->lre_time_ns =
		(uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
	ev->lre_kind = kind;
	ev->lre_send = r->lr_sends;
	ev->lre_cb_status = cb_status;
	ev->lre_op_status = op_status;
	ev->lre_dropped = dropped;

	reffs_trace_event(REFFS_TRACE_CAT_NFS, __func__, __LINE__,
			  "layoutrecall: id=%u clid=0x%016" PRIx64
			  " type=%s sb=%" PRIu64 " ino=%" PRIu64
			  " %s n=%u cb=%s(%d) op=%s(%d) dropped=%u",
			  r->lr_id, (uint64_t)r->lr_p.lrp_clientid,
			  lr_type_name(r->lr_p.lrp_type), r->lr_p.lrp_sb_id,
			  r->lr_p.lrp_ino, lr_event_name(kind), r->lr_sends,
			  lr_status_name(cb_status), cb_status,
			  lr_status_name(op_status), op_status, dropped);
}

static bool lr_covers(const struct layout_recall *r, const struct inode *inode)
{
	if (r->lr_p.lrp_type == LAYOUTRECALL4_ALL)
		return true;
	if (!inode || !inode->i_sb || inode->i_sb->sb_id != r->lr_p.lrp_sb_id)
		return false;
	return r->lr_p.lrp_type == LAYOUTRECALL4_FSID ||
	       inode->i_ino == r->lr_p.lrp_ino;
}

static struct layout_recall *lr_find_locked(uint32_t id)
{
	if (!id)
		id = lr_latest_id;
	for (unsigned int i = 0; i < LAYOUT_RECALL_MAX; i++)
		if (lr_table[i].lr_id && lr_table[i].lr_id == id)
			return &lr_table[i];
	return NULL;
}

static void lr_wake_locked(void)
{
	lr_kick = true;
	pthread_cond_signal(&lr_cv);
}

/* ------------------------------------------------------------------ */
/* Reply handling                                                      */

static void lr_cb_complete(struct cb_pending *cp)
{
	struct layout_recall *r = cp->cp_complete_arg;

	pthread_mutex_lock(&lr_mutex);
	if (r->lr_cp == cp) {
		r->lr_reply_ready = true;
		lr_wake_locked();
	}
	pthread_mutex_unlock(&lr_mutex);
}

static void lr_finish_locked(struct layout_recall *r,
			     enum layout_recall_state s, uint32_t kind,
			     int32_t cb_status, int32_t op_status,
			     uint32_t dropped)
{
	lr_set_state_locked(r, s);
	lr_event_locked(r, kind, cb_status, op_status, dropped);
}

/*
 * Returns true when a NOMATCHING_LAYOUT reply needs the layouts in
 * scope dropped.  The recall stays outstanding (LAYOUTGET keeps getting
 * RECALLCONFLICT) until lr_drop() has run outside lr_mutex.
 */
static bool lr_process_reply_locked(struct layout_recall *r, uint64_t now)
{
	struct cb_pending *cp = r->lr_cp;
	int cb_status =
		atomic_load_explicit(&cp->cp_status, memory_order_acquire);
	int32_t comp, op;

	r->lr_cp = NULL;
	r->lr_reply_ready = false;
	r->lr_in_flight = false;

	if (!lr_is_outstanding(r->lr_state)) {
		/* Cancelled, or a LAYOUTRETURN completed it meanwhile. */
		cb_pending_free(cp);
		return false;
	}

	if (cb_status != 0) {
		lr_event_locked(r, LAYOUT_RECALL_EV_TIMEOUT, cb_status, 0, 0);
		lr_finish_locked(r, LAYOUT_RECALL_FAILED,
				 LAYOUT_RECALL_EV_FAILED, cb_status, 0, 0);
		cb_pending_free(cp);
		return false;
	}

	/* A CB_SEQUENCE failure leaves no CB_LAYOUTRECALL result. */
	comp = (int32_t)cp->cp_res.status;
	op = comp;
	if (cp->cp_res.resarray.resarray_len >= 2 &&
	    cp->cp_res.resarray.resarray_val[1].resop == OP_CB_LAYOUTRECALL)
		op = (int32_t)cp->cp_res.resarray.resarray_val[1]
			     .nfs_cb_resop4_u.opcblayoutrecall.clorr_status;
	cb_pending_free(cp);
	lr_event_locked(r, LAYOUT_RECALL_EV_REPLY, comp, op, 0);

	switch (op) {
	case NFS4ERR_DELAY: {
		uint64_t next =
			now + (uint64_t)r->lr_p.lrp_interval_ms * 1000000ULL;

		if ((r->lr_p.lrp_limit_count &&
		     r->lr_sends >= r->lr_p.lrp_limit_count) ||
		    (r->lr_deadline_ns && next > r->lr_deadline_ns)) {
			lr_finish_locked(r, LAYOUT_RECALL_EXPIRED,
					 LAYOUT_RECALL_EV_EXPIRED, comp, op, 0);
			break;
		}
		r->lr_next_send_ns = next;
		lr_set_state_locked(r, LAYOUT_RECALL_WAIT_RESEND);
		break;
	}
	case NFS4ERR_NOMATCHING_LAYOUT:
		r->lr_draining = true;
		r->lr_next_send_ns = UINT64_MAX;
		return true;
	case NFS4_OK:
		/* sec 20.3.4: complete once the LAYOUTRETURN arrives. */
		lr_set_state_locked(r, LAYOUT_RECALL_ACKED);
		break;
	default:
		lr_finish_locked(r, LAYOUT_RECALL_FAILED,
				 LAYOUT_RECALL_EV_FAILED, comp, op, 0);
		break;
	}
	return false;
}

/* Client's layout stateid on (sb, ino), ref-bumped, or NULL. */
static struct stateid *lr_find_layout_stid(struct client *client,
					   uint64_t sb_id, uint64_t ino,
					   struct inode **inode_out)
{
	struct super_block *sb = super_block_find(sb_id);
	struct inode *inode = sb ? inode_find(sb, ino) : NULL;
	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;
	struct stateid *found = NULL;

	if (inode && inode->i_stateids) {
		rcu_read_lock();
		cds_lfht_for_each(inode->i_stateids, &iter, node)
		{
			struct stateid *stid = caa_container_of(
				node, struct stateid, s_inode_node);

			if (stid->s_tag == Layout_Stateid &&
			    stid->s_client == client && stateid_get(stid)) {
				found = stid;
				break;
			}
		}
		rcu_read_unlock();
	}
	super_block_put(sb);
	*inode_out = inode;
	return found;
}

/*
 * Free the layouts a NOMATCHING_LAYOUT reply says the client no longer
 * holds.  For FILE, a seqid past the recall's means a LAYOUTGET was
 * granted after the recall went out (sec 12.5.5.2.1.3): that layout is
 * the client's, so leave it.  Bulk recalls carry no stateid and cannot
 * tell; a grant racing the recall is freed with the rest.
 */
static uint32_t lr_drop(const struct layout_recall_params *p,
			const stateid4 *file_stateid)
{
	struct nfs4_client *nc = nfs4_client_find(p->lrp_clientid);
	struct client *client = nc ? nfs4_client_to_client(nc) : NULL;
	uint32_t dropped = 0;

	if (!client)
		return 0;
	if (p->lrp_type == LAYOUTRECALL4_FILE) {
		struct inode *inode = NULL;
		struct stateid *stid = lr_find_layout_stid(client, p->lrp_sb_id,
							   p->lrp_ino, &inode);
		bool newer = stid && __atomic_load_n(&stid->s_seqid,
						     __ATOMIC_RELAXED) !=
					     file_stateid->seqid;

		stateid_put(stid);
		inode_active_put(inode);
		if (stid && !newer)
			dropped = nfs4_layout_drop_client_scope(client,
								p->lrp_sb_id,
								p->lrp_ino,
								p->lrp_iomode);
	} else {
		dropped = nfs4_layout_drop_client_scope(
			client,
			p->lrp_type == LAYOUTRECALL4_ALL ? 0 : p->lrp_sb_id, 0,
			p->lrp_iomode);
	}
	nfs4_client_put(nc);
	return dropped;
}

/* ------------------------------------------------------------------ */
/* Sending                                                             */

static void lr_send_failed(struct layout_recall *r, int err)
{
	pthread_mutex_lock(&lr_mutex);
	r->lr_in_flight = false;
	if (lr_is_outstanding(r->lr_state))
		lr_finish_locked(r, LAYOUT_RECALL_FAILED,
				 LAYOUT_RECALL_EV_FAILED, -err, 0, 0);
	pthread_mutex_unlock(&lr_mutex);
}

static void lr_do_send(const struct lr_send *s)
{
	struct layout_recall *r = s->ls_rec;
	struct server_state *ss = server_state_find();
	struct nfs4_client *nc = nfs4_client_find(s->ls_p.lrp_clientid);
	struct nfs4_session *sess =
		ss && nc ? nfs4_session_find_for_client(ss, nc) : NULL;
	struct network_file_handle nfh = { 0 };
	layoutrecall4 recall = { .lor_recalltype = s->ls_p.lrp_type };
	struct cb_pending *cp = NULL;
	int ret = ENOTCONN;

	if (!sess)
		goto out;

	cp = cb_pending_alloc(NULL, NULL, OP_CB_LAYOUTRECALL);
	if (!cp) {
		ret = ENOMEM;
		goto out;
	}
	cp->cp_complete = lr_cb_complete;
	cp->cp_complete_arg = r;
	if (!lr_manual)
		cp->cp_hard_deadline_ns = s->ls_deadline_ns;

	if (s->ls_p.lrp_type == LAYOUTRECALL4_FILE) {
		layoutrecall_file4 *f = &recall.layoutrecall4_u.lor_layout;

		nfh.nfh_sb = s->ls_p.lrp_sb_id;
		nfh.nfh_ino = s->ls_p.lrp_ino;
		f->lor_fh.nfs_fh4_len = sizeof(nfh);
		f->lor_fh.nfs_fh4_val = (char *)&nfh;
		f->lor_offset = s->ls_p.lrp_offset;
		f->lor_length = s->ls_p.lrp_length;
		f->lor_stateid = s->ls_file_stateid;
	} else if (s->ls_p.lrp_type == LAYOUTRECALL4_FSID) {
		/* reffs reports fsid4 as { sb_id, 0 }; see attr.c. */
		recall.layoutrecall4_u.lor_fsid.major = s->ls_p.lrp_sb_id;
		recall.layoutrecall4_u.lor_fsid.minor = 0;
	}

	/* Publish before sending: the reply can beat the send's return. */
	pthread_mutex_lock(&lr_mutex);
	r->lr_cp = cp;
	pthread_mutex_unlock(&lr_mutex);

	ret = lr_send_fn(sess, s->ls_p.lrp_layout_type, s->ls_p.lrp_iomode,
			 s->ls_p.lrp_changed, &recall, cp);
	if (ret) {
		pthread_mutex_lock(&lr_mutex);
		r->lr_cp = NULL;
		r->lr_reply_ready = false;
		pthread_mutex_unlock(&lr_mutex);
		cb_pending_free(cp);
	}
out:
	if (ret)
		lr_send_failed(r, ret);
	if (sess)
		nfs4_session_put(sess);
	if (nc)
		nfs4_client_put(nc);
	if (ss)
		server_state_put(ss);
}

void layout_recall_run(void)
{
	struct lr_send sends[LAYOUT_RECALL_MAX];
	struct lr_send drops[LAYOUT_RECALL_MAX];
	unsigned int nsend = 0, ndrop = 0;
	uint64_t now = lr_now();

	pthread_mutex_lock(&lr_mutex);
	for (unsigned int i = 0; i < LAYOUT_RECALL_MAX; i++) {
		struct layout_recall *r = &lr_table[i];

		if (!r->lr_id)
			continue;
		if (r->lr_reply_ready && lr_process_reply_locked(r, now)) {
			drops[ndrop].ls_rec = r;
			drops[ndrop].ls_id = r->lr_id;
			drops[ndrop].ls_p = r->lr_p;
			drops[ndrop].ls_file_stateid = r->lr_file_stateid;
			ndrop++;
		}
		if (r->lr_draining)
			continue;
		if (r->lr_state == LAYOUT_RECALL_WAIT_RESEND &&
		    now >= r->lr_next_send_ns)
			lr_set_state_locked(r, LAYOUT_RECALL_SENDING);
		if (r->lr_state == LAYOUT_RECALL_ACKED && r->lr_deadline_ns &&
		    now >= r->lr_deadline_ns)
			lr_finish_locked(r, LAYOUT_RECALL_EXPIRED,
					 LAYOUT_RECALL_EV_EXPIRED, 0, 0, 0);
		if (r->lr_state != LAYOUT_RECALL_SENDING || r->lr_in_flight ||
		    now < r->lr_next_send_ns)
			continue;

		r->lr_in_flight = true;
		r->lr_sends++;
		lr_event_locked(r, LAYOUT_RECALL_EV_SEND, 0, 0, 0);
		sends[nsend].ls_rec = r;
		sends[nsend].ls_p = r->lr_p;
		sends[nsend].ls_file_stateid = r->lr_file_stateid;
		sends[nsend].ls_deadline_ns = r->lr_deadline_ns;
		nsend++;
	}
	pthread_mutex_unlock(&lr_mutex);

	for (unsigned int i = 0; i < ndrop; i++) {
		struct layout_recall *r = drops[i].ls_rec;
		uint32_t dropped =
			lr_drop(&drops[i].ls_p, &drops[i].ls_file_stateid);

		pthread_mutex_lock(&lr_mutex);
		r->lr_draining = false;
		if (r->lr_id == drops[i].ls_id &&
		    lr_is_outstanding(r->lr_state))
			lr_finish_locked(r, LAYOUT_RECALL_DONE,
					 LAYOUT_RECALL_EV_DONE, 0,
					 NFS4ERR_NOMATCHING_LAYOUT, dropped);
		pthread_mutex_unlock(&lr_mutex);
	}

	for (unsigned int i = 0; i < nsend; i++)
		lr_do_send(&sends[i]);
}

/* Earliest monotonic time anything is due; 0 if nothing is scheduled. */
static uint64_t lr_next_due_locked(void)
{
	uint64_t due = 0;

	for (unsigned int i = 0; i < LAYOUT_RECALL_MAX; i++) {
		struct layout_recall *r = &lr_table[i];
		uint64_t t = 0;

		if (r->lr_state == LAYOUT_RECALL_WAIT_RESEND)
			t = r->lr_next_send_ns;
		else if (r->lr_state == LAYOUT_RECALL_ACKED)
			t = r->lr_deadline_ns;
		if (t && (!due || t < due))
			due = t;
	}
	return due;
}

static void *lr_driver(void *arg __attribute__((unused)))
{
	rcu_register_thread();
	pthread_mutex_lock(&lr_mutex);
	while (lr_running) {
		pthread_mutex_unlock(&lr_mutex);
		layout_recall_run();
		pthread_mutex_lock(&lr_mutex);
		if (!lr_running)
			break;
		if (!lr_kick) {
			uint64_t due = lr_next_due_locked();
			uint64_t now = lr_now();
			uint64_t wait_ns = 1000000000ULL;
			struct timespec ts;

			if (due && due > now && due - now < wait_ns)
				wait_ns = due - now;
			else if (due && due <= now)
				wait_ns = 0;
			clock_gettime(CLOCK_REALTIME, &ts);
			ts.tv_sec += (time_t)(wait_ns / 1000000000ULL);
			ts.tv_nsec += (long)(wait_ns % 1000000000ULL);
			if (ts.tv_nsec >= 1000000000L) {
				ts.tv_sec++;
				ts.tv_nsec -= 1000000000L;
			}
			if (wait_ns)
				pthread_cond_timedwait(&lr_cv, &lr_mutex, &ts);
		}
		lr_kick = false;
	}
	pthread_mutex_unlock(&lr_mutex);
	rcu_unregister_thread();
	return NULL;
}

/* ------------------------------------------------------------------ */
/* Start / status / cancel                                             */

/* Resolve the target client; returns a ref the caller must put. */
#define LR_RESOLVE_MAX 64

static int lr_resolve_client(struct layout_recall_params *p,
			     struct nfs4_client **out)
{
	struct nfs4_client *cand[LR_RESOLVE_MAX];
	struct nfs4_client *found = NULL;
	struct server_state *ss;
	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;
	unsigned int ncand = 0, matches = 0;

	if (p->lrp_clientid) {
		*out = nfs4_client_find(p->lrp_clientid);
		return *out ? 0 : -ENOENT;
	}

	ss = server_state_find();
	if (!ss || !ss->ss_client_ht) {
		server_state_put(ss);
		return -ENOENT;
	}
	rcu_read_lock();
	cds_lfht_for_each(ss->ss_client_ht, &iter, node)
	{
		struct client *c =
			caa_container_of(node, struct client, c_node);
		struct nfs4_client *nc = client_to_nfs4(c);

		if (ncand == LR_RESOLVE_MAX)
			break;
		if (p->lrp_addr && nc->nc_sin.sin_addr.s_addr != p->lrp_addr)
			continue;
		if (client_get(c))
			cand[ncand++] = nc;
	}
	rcu_read_unlock();

	/* Only clients that can be called back count; puts stay outside RCU. */
	for (unsigned int i = 0; i < ncand; i++) {
		struct nfs4_session *sess =
			nfs4_session_find_for_client(ss, cand[i]);

		if (sess) {
			nfs4_session_put(sess);
			if (++matches == 1) {
				found = cand[i];
				continue;
			}
		}
		nfs4_client_put(cand[i]);
	}
	server_state_put(ss);

	if (matches > 1) {
		nfs4_client_put(found);
		return -ENOTUNIQ;
	}
	if (!found)
		return -ENOENT;
	p->lrp_clientid = (clientid4)nfs4_client_to_client(found)->c_id;
	*out = found;
	return 0;
}

/* FILE recalls carry the client's layout stateid, seqid bumped (12.5.3). */
static int lr_file_stateid(const struct layout_recall_params *p,
			   struct nfs4_client *nc, stateid4 *out)
{
	struct inode *inode = NULL;
	struct stateid *stid = lr_find_layout_stid(
		nfs4_client_to_client(nc), p->lrp_sb_id, p->lrp_ino, &inode);

	if (stid) {
		__atomic_add_fetch(&stid->s_seqid, 1, __ATOMIC_RELAXED);
		pack_stateid4(out, stid);
		stateid_put(stid);
	}
	inode_active_put(inode);
	return stid ? 0 : -ENOENT;
}

static struct layout_recall *lr_alloc_slot_locked(clientid4 clid, int *err)
{
	struct layout_recall *slot = NULL;

	for (unsigned int i = 0; i < LAYOUT_RECALL_MAX; i++) {
		struct layout_recall *r = &lr_table[i];

		if (r->lr_id && lr_is_outstanding(r->lr_state) &&
		    r->lr_p.lrp_clientid == clid) {
			*err = -EBUSY;
			return NULL;
		}
	}
	/* Prefer an unused slot, else evict the oldest finished one. */
	for (unsigned int i = 0; i < LAYOUT_RECALL_MAX; i++) {
		struct layout_recall *r = &lr_table[i];

		if (!r->lr_id)
			return r;
		if (lr_is_outstanding(r->lr_state) || r->lr_in_flight ||
		    r->lr_draining)
			continue;
		if (!slot || r->lr_id < slot->lr_id)
			slot = r;
	}
	if (!slot)
		*err = -ENOSPC;
	return slot;
}

int layout_recall_start(const struct layout_recall_params *params,
			uint32_t *id_out)
{
	struct layout_recall_params p = *params;
	struct layout_recall *r;
	struct nfs4_client *nc = NULL;
	stateid4 file_stateid = { 0 };
	uint32_t id;
	uint64_t now;
	int ret;

	if (p.lrp_type != LAYOUTRECALL4_FILE &&
	    p.lrp_type != LAYOUTRECALL4_FSID && p.lrp_type != LAYOUTRECALL4_ALL)
		return -EINVAL;
	if (p.lrp_iomode != LAYOUTIOMODE4_READ &&
	    p.lrp_iomode != LAYOUTIOMODE4_RW &&
	    p.lrp_iomode != LAYOUTIOMODE4_ANY)
		return -EINVAL;
	if (!p.lrp_layout_type)
		p.lrp_layout_type = LAYOUT4_FLEX_FILES;
	if (!p.lrp_interval_ms)
		p.lrp_interval_ms = LR_DEFAULT_INTERVAL_MS;
	if (p.lrp_type == LAYOUTRECALL4_FILE && !p.lrp_length)
		p.lrp_length = NFS4_UINT64_MAX;
	if (p.lrp_type == LAYOUTRECALL4_ALL)
		p.lrp_sb_id = 0;
	if (p.lrp_type != LAYOUTRECALL4_FILE)
		p.lrp_ino = p.lrp_offset = p.lrp_length = 0;

	if (p.lrp_type != LAYOUTRECALL4_ALL) {
		struct super_block *sb = super_block_find(p.lrp_sb_id);

		if (!sb)
			return -ENOENT;
		super_block_put(sb);
	}

	ret = lr_resolve_client(&p, &nc);
	if (ret)
		return ret;

	/*
	 * Reserve the slot before touching the FILE stateid: the recall is
	 * outstanding (LAYOUTGET conflicts) from here, and a refused start
	 * never consumes a layout seqid (sec 12.5.3).  Nothing is due to
	 * send until the reservation is filled in.
	 */
	pthread_mutex_lock(&lr_mutex);
	r = lr_alloc_slot_locked(p.lrp_clientid, &ret);
	if (!r) {
		pthread_mutex_unlock(&lr_mutex);
		goto out;
	}
	memset(r, 0, sizeof(*r));
	r->lr_id = ++lr_next_id;
	if (!r->lr_id)
		r->lr_id = ++lr_next_id;
	id = r->lr_id;
	r->lr_p = p;
	r->lr_next_send_ns = UINT64_MAX;
	lr_set_state_locked(r, LAYOUT_RECALL_SENDING);
	pthread_mutex_unlock(&lr_mutex);

	if (p.lrp_type == LAYOUTRECALL4_FILE)
		ret = lr_file_stateid(&p, nc, &file_stateid);

	now = lr_now();
	pthread_mutex_lock(&lr_mutex);
	if (r->lr_id != id) {
		ret = -ENOENT; /* cannot happen: outstanding slots are kept */
	} else if (ret) {
		lr_set_state_locked(r, LAYOUT_RECALL_IDLE);
		memset(r, 0, sizeof(*r));
	} else {
		lr_latest_id = id;
		r->lr_file_stateid = file_stateid;
		r->lr_next_send_ns = now;
		if (p.lrp_limit_ms)
			r->lr_deadline_ns =
				now + (uint64_t)p.lrp_limit_ms * 1000000ULL;
		*id_out = id;
		if (!lr_manual && !lr_thread_started) {
			lr_running = true;
			if (pthread_create(&lr_thread, NULL, lr_driver, NULL) ==
			    0) {
				lr_thread_started = true;
			} else {
				lr_running = false;
				LOG("layoutrecall: cannot start driver thread");
			}
		}
		lr_wake_locked();
	}
	pthread_mutex_unlock(&lr_mutex);
out:
	nfs4_client_put(nc);
	return ret;
}

int layout_recall_status(uint32_t id, struct layout_recall_info *out)
{
	struct layout_recall *r;

	pthread_mutex_lock(&lr_mutex);
	r = lr_find_locked(id);
	if (!r) {
		pthread_mutex_unlock(&lr_mutex);
		return -ENOENT;
	}
	out->lri_id = r->lr_id;
	out->lri_state = r->lr_state;
	out->lri_params = r->lr_p;
	out->lri_sends = r->lr_sends;
	out->lri_nevents = r->lr_nevents;
	out->lri_events_lost = r->lr_events_lost;
	memcpy(out->lri_events, r->lr_events,
	       sizeof(r->lr_events[0]) * r->lr_nevents);
	pthread_mutex_unlock(&lr_mutex);
	return 0;
}

int layout_recall_cancel(uint32_t id)
{
	struct layout_recall *r;
	int ret = 0;

	pthread_mutex_lock(&lr_mutex);
	r = lr_find_locked(id);
	if (!r)
		ret = -ENOENT;
	else if (lr_is_outstanding(r->lr_state))
		lr_finish_locked(r, LAYOUT_RECALL_CANCELLED,
				 LAYOUT_RECALL_EV_CANCELLED, 0, 0, 0);
	pthread_mutex_unlock(&lr_mutex);
	return ret;
}

/* ------------------------------------------------------------------ */
/* LAYOUTGET / LAYOUTRETURN hooks                                      */

bool layout_recall_layoutget_conflict(clientid4 clid, const struct inode *inode)
{
	bool conflict = false;

	if (!atomic_load_explicit(&lr_outstanding, memory_order_acquire))
		return false;
	pthread_mutex_lock(&lr_mutex);
	for (unsigned int i = 0; i < LAYOUT_RECALL_MAX && !conflict; i++) {
		struct layout_recall *r = &lr_table[i];

		conflict = r->lr_id && lr_is_outstanding(r->lr_state) &&
			   r->lr_p.lrp_clientid == clid && lr_covers(r, inode);
	}
	pthread_mutex_unlock(&lr_mutex);
	return conflict;
}

nfsstat4 layout_recall_layoutreturn_check(clientid4 clid,
					  layoutreturn_type4 type,
					  const struct inode *inode)
{
	nfsstat4 status = NFS4_OK;

	if (!atomic_load_explicit(&lr_outstanding, memory_order_acquire))
		return NFS4_OK;
	pthread_mutex_lock(&lr_mutex);
	for (unsigned int i = 0; i < LAYOUT_RECALL_MAX; i++) {
		struct layout_recall *r = &lr_table[i];

		if (!r->lr_id || !lr_is_outstanding(r->lr_state) ||
		    r->lr_p.lrp_clientid != clid ||
		    r->lr_p.lrp_type == LAYOUTRECALL4_FILE)
			continue;
		/* LAYOUTRETURN4_x and LAYOUTRECALL4_x share values. */
		if (type == LAYOUTRETURN4_FILE ?
			    lr_covers(r, inode) :
			    (int)type != (int)r->lr_p.lrp_type) {
			status = NFS4ERR_RECALLCONFLICT;
			break;
		}
	}
	pthread_mutex_unlock(&lr_mutex);
	return status;
}

void layout_recall_layoutreturn_done(clientid4 clid, layoutreturn_type4 type,
				     layoutiomode4 iomode,
				     const struct inode *inode)
{
	if (!atomic_load_explicit(&lr_outstanding, memory_order_acquire))
		return;
	pthread_mutex_lock(&lr_mutex);
	for (unsigned int i = 0; i < LAYOUT_RECALL_MAX; i++) {
		struct layout_recall *r = &lr_table[i];

		if (!r->lr_id || !lr_is_outstanding(r->lr_state) ||
		    r->lr_p.lrp_clientid != clid ||
		    (int)type != (int)r->lr_p.lrp_type)
			continue;
		if (type != LAYOUTRETURN4_ALL && !lr_covers(r, inode))
			continue;
		if (iomode != LAYOUTIOMODE4_ANY && iomode != r->lr_p.lrp_iomode)
			continue;
		lr_event_locked(r, LAYOUT_RECALL_EV_RETURN, 0, 0, 0);
		lr_finish_locked(r, LAYOUT_RECALL_DONE, LAYOUT_RECALL_EV_DONE,
				 0, 0, 0);
	}
	pthread_mutex_unlock(&lr_mutex);
}

/* ------------------------------------------------------------------ */
/* Init / fini / test seams                                            */

void layout_recall_test_manual(uint64_t (*clock_ns)(void),
			       layout_recall_send_fn send)
{
	pthread_mutex_lock(&lr_mutex);
	lr_manual = true;
	lr_clock = clock_ns ? clock_ns : reffs_now_ns;
	lr_send_fn = send ? send : nfs4_cb_layoutrecall_send_recall;
	pthread_mutex_unlock(&lr_mutex);
}

int layout_recall_init(void)
{
	pthread_mutex_lock(&lr_mutex);
	memset(lr_table, 0, sizeof(lr_table));
	lr_next_id = 0;
	lr_latest_id = 0;
	lr_kick = false;
	atomic_store_explicit(&lr_outstanding, 0, memory_order_relaxed);
	pthread_mutex_unlock(&lr_mutex);
	return 0;
}

void layout_recall_fini(void)
{
	pthread_mutex_lock(&lr_mutex);
	lr_running = false;
	pthread_cond_signal(&lr_cv);
	pthread_mutex_unlock(&lr_mutex);
	if (lr_thread_started) {
		pthread_join(lr_thread, NULL);
		lr_thread_started = false;
	}

	/*
	 * Detach in-flight callbacks under lr_mutex, then reclaim them
	 * without it: cb_timeout_unregister() takes cb_timeout_mutex, and
	 * the timeout thread holds that mutex while lr_cb_complete() takes
	 * lr_mutex.
	 */
	struct cb_pending *inflight[LAYOUT_RECALL_MAX];
	bool ready[LAYOUT_RECALL_MAX];
	unsigned int n = 0;

	pthread_mutex_lock(&lr_mutex);
	for (unsigned int i = 0; i < LAYOUT_RECALL_MAX; i++) {
		struct layout_recall *r = &lr_table[i];

		if (!r->lr_cp)
			continue;
		inflight[n] = r->lr_cp;
		ready[n++] = r->lr_reply_ready;
		r->lr_cp = NULL; /* a late lr_cb_complete() is now a no-op */
	}
	pthread_mutex_unlock(&lr_mutex);

	for (unsigned int i = 0; i < n; i++) {
		struct cb_pending *cp = inflight[i];
		struct rpc_trans *out =
			ready[i] ? NULL : io_take_request_by_xid(cp->cp_xid);

		if (out) {
			cb_timeout_unregister(cp);
			out->rt_context = NULL;
			rpc_protocol_free(out);
			cb_pending_free(cp); /* transport ref */
			cb_pending_free(cp); /* ours */
		} else if (ready[i] || lr_manual) {
			/* Completed already, or a test send took no transport ref. */
			cb_pending_free(cp);
		}
		/* else the reply path owns completion; leak rather than race. */
	}

	pthread_mutex_lock(&lr_mutex);
	memset(lr_table, 0, sizeof(lr_table));
	atomic_store_explicit(&lr_outstanding, 0, memory_order_relaxed);
	lr_manual = false;
	lr_clock = reffs_now_ns;
	lr_send_fn = nfs4_cb_layoutrecall_send_recall;
	pthread_mutex_unlock(&lr_mutex);
}
