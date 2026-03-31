/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>

#include <xxhash.h>

#include "nfsv42_xdr.h"
#include "nfsv42_names.h"
#include "reffs/log.h"
#include "reffs/rcu.h"
#include "reffs/rpc.h"
#include "reffs/server.h"
#include "reffs/time.h"
#include "nfs4/compound.h"
#include "nfs4/session.h"
#include "nfs4/ops.h"
#include "nfs4/errors.h"
#include "nfs4/client.h"
#include "nfs4/client_persist.h"

/* ------------------------------------------------------------------ */
/* Session hash table helpers                                          */

static uint64_t g_session_counter = 0;

static void session_make_id(sessionid4 sid, const struct nfs4_client *nc)
{
	uint64_t clid = nfs4_client_to_client((struct nfs4_client *)nc)->c_id;
	uint64_t ctr =
		__atomic_add_fetch(&g_session_counter, 1, __ATOMIC_RELAXED);
	memcpy(sid, &clid, sizeof(clid));
	memcpy(sid + sizeof(clid), &ctr, sizeof(ctr));
}

static int session_match(struct cds_lfht_node *ht_node, const void *vkey)
{
	const struct nfs4_session *ns =
		caa_container_of(ht_node, struct nfs4_session, ns_node);
	const char *key = vkey;

	return memcmp(key, ns->ns_sessionid, sizeof(sessionid4)) == 0;
}

/* ------------------------------------------------------------------ */
/* Refcount / release                                                  */

static void nfs4_session_free_rcu(struct rcu_head *rcu)
{
	struct nfs4_session *ns =
		caa_container_of(rcu, struct nfs4_session, ns_rcu);
	uint32_t i;

	for (i = 0; i < ns->ns_slot_count; i++) {
		pthread_mutex_destroy(&ns->ns_slots[i].sl_mutex);
		free(ns->ns_slots[i].sl_reply);
	}
	free(ns->ns_slots);
	pthread_mutex_destroy(&ns->ns_cb_mutex);
	__atomic_fetch_sub(&ns->ns_client->nc_session_count, 1,
			   __ATOMIC_RELAXED);
	nfs4_client_put(ns->ns_client);
	free(ns);
}

static void nfs4_session_release_cb(struct urcu_ref *ref)
{
	struct nfs4_session *ns =
		caa_container_of(ref, struct nfs4_session, ns_ref);

	call_rcu(&ns->ns_rcu, ns->ns_free_rcu);
}

static void session_release(struct urcu_ref *ref)
{
	struct nfs4_session *ns =
		caa_container_of(ref, struct nfs4_session, ns_ref);
	/*
	 * Sessions are normally unhashed explicitly via DESTROY_SESSION or
	 * client eviction before their last put.  This server_state_find()
	 * is a safety net for abnormal paths (e.g., shutdown ordering).  If
	 * the server is already torn down, ss will be NULL and the lfht
	 * delete is skipped — the hash table itself is gone at that point
	 * so there is no dangling node to worry about.
	 */
	struct server_state *ss = server_state_find();

	if (ss) {
		nfs4_session_unhash(ss, ns);
		server_state_put(ss);
	}
	ns->ns_release(ref);
}

struct nfs4_session *nfs4_session_get(struct nfs4_session *ns)
{
	if (!ns)
		return NULL;

	if (!urcu_ref_get_unless_zero(&ns->ns_ref))
		return NULL;

	return ns;
}

void nfs4_session_put(struct nfs4_session *ns)
{
	if (!ns)
		return;

	urcu_ref_put(&ns->ns_ref, session_release);
}

/* ------------------------------------------------------------------ */
/* Hash / unhash                                                       */

bool nfs4_session_unhash(struct server_state *ss, struct nfs4_session *ns)
{
	uint64_t state;
	int __attribute__((unused)) ret;

	assert(ss);

	state = __atomic_fetch_and(&ns->ns_state, ~NFS4_SESSION_IS_HASHED,
				   __ATOMIC_ACQUIRE);
	if (!(state & NFS4_SESSION_IS_HASHED))
		return false;

	ret = cds_lfht_del(ss->ss_session_ht, &ns->ns_node);
	assert(!ret);

	/*
	 * Drop the hash table's own ref.  The caller still holds theirs;
	 * the session is freed when the last ref is released.
	 */
	nfs4_session_put(ns);
	return true;
}

/* ------------------------------------------------------------------ */
/* Alloc / find                                                        */

struct nfs4_session *nfs4_session_alloc(struct server_state *ss,
					struct nfs4_client *nc,
					const channel_attrs4 *fore_req,
					uint32_t __attribute__((unused)) flags)
{
	struct nfs4_session *ns;
	struct cds_lfht_node *node;
	unsigned long hash;
	uint32_t i;

	if (!nc || !fore_req)
		return NULL;

	ns = calloc(1, sizeof(*ns));
	if (!ns)
		return NULL;

	/* Negotiate fore-channel attributes against server caps. */
	ns->ns_headerpadsize = 0;

	ns->ns_maxrequestsize = fore_req->ca_maxrequestsize;
	if (ns->ns_maxrequestsize == 0 ||
	    ns->ns_maxrequestsize > NFS4_SESSION_MAX_REQUEST_SIZE)
		ns->ns_maxrequestsize = NFS4_SESSION_MAX_REQUEST_SIZE;

	ns->ns_maxresponsesize = fore_req->ca_maxresponsesize;
	if (ns->ns_maxresponsesize == 0 ||
	    ns->ns_maxresponsesize > NFS4_SESSION_MAX_RESPONSE_SIZE)
		ns->ns_maxresponsesize = NFS4_SESSION_MAX_RESPONSE_SIZE;

	ns->ns_maxresponsesize_cached = fore_req->ca_maxresponsesize_cached;
	if (ns->ns_maxresponsesize_cached > NFS4_SESSION_MAX_RESPONSE_CACHED)
		ns->ns_maxresponsesize_cached =
			NFS4_SESSION_MAX_RESPONSE_CACHED;

	ns->ns_maxoperations = fore_req->ca_maxoperations;
	if (ns->ns_maxoperations < 2 ||
	    ns->ns_maxoperations > NFS4_SESSION_MAX_OPS)
		ns->ns_maxoperations = NFS4_SESSION_MAX_OPS;

	ns->ns_slot_count = fore_req->ca_maxrequests;
	if (ns->ns_slot_count == 0 ||
	    ns->ns_slot_count > NFS4_SESSION_MAX_SLOTS)
		ns->ns_slot_count = NFS4_SESSION_MAX_SLOTS;

	ns->ns_flags = 0; /* no persistent sessions or RDMA */

	/* Allocate slot table. */
	ns->ns_slots = calloc(ns->ns_slot_count, sizeof(*ns->ns_slots));
	if (!ns->ns_slots)
		goto err_free_ns;

	for (i = 0; i < ns->ns_slot_count; i++) {
		pthread_mutex_init(&ns->ns_slots[i].sl_mutex, NULL);
		ns->ns_slots[i].sl_seqid = 0;
		ns->ns_slots[i].sl_state = NFS4_SLOT_IDLE;
	}

	pthread_mutex_init(&ns->ns_cb_mutex, NULL);
	ns->ns_cb_fd = -1;

	/* Generate session ID: clientid4 || monotonic counter. */
	session_make_id(ns->ns_sessionid, nc);

	/* Hold ref on client for session lifetime. */
	ns->ns_client = nfs4_client_get(nc);
	if (!ns->ns_client)
		goto err_free_slots;
	__atomic_fetch_add(&nc->nc_session_count, 1, __ATOMIC_RELAXED);

	/* Set up ref counting and callbacks. */
	cds_lfht_node_init(&ns->ns_node);
	urcu_ref_init(&ns->ns_ref);
	ns->ns_free_rcu = nfs4_session_free_rcu;
	ns->ns_release = nfs4_session_release_cb;

	/* Insert into hash table. */
	hash = XXH3_64bits(ns->ns_sessionid, sizeof(ns->ns_sessionid));

	rcu_read_lock();
	ns->ns_state |= NFS4_SESSION_IS_HASHED;
	node = cds_lfht_add_unique(ss->ss_session_ht, hash, session_match,
				   ns->ns_sessionid, &ns->ns_node);
	rcu_read_unlock();

	if (caa_unlikely(node != &ns->ns_node)) {
		/* Session ID collision — should be impossible. */
		ns->ns_state &= ~NFS4_SESSION_IS_HASHED;
		goto err_put_client;
	}

	/* Bump ref for the caller; hash table holds its own ref. */
	nfs4_session_get(ns);
	return ns;

err_put_client:
	nfs4_client_put(ns->ns_client);
err_free_slots:
	for (i = 0; i < ns->ns_slot_count; i++)
		pthread_mutex_destroy(&ns->ns_slots[i].sl_mutex);
	free(ns->ns_slots);
err_free_ns:
	free(ns);
	return NULL;
}

struct nfs4_session *nfs4_session_find(struct server_state *ss,
				       const sessionid4 sid)
{
	struct nfs4_session *ns = NULL;
	struct nfs4_session *tmp;
	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;
	unsigned long hash = XXH3_64bits(sid, sizeof(sessionid4));

	rcu_read_lock();
	cds_lfht_lookup(ss->ss_session_ht, hash, session_match, sid, &iter);
	node = cds_lfht_iter_get_node(&iter);
	if (node) {
		tmp = caa_container_of(node, struct nfs4_session, ns_node);
		ns = nfs4_session_get(tmp);
	}
	rcu_read_unlock();

	return ns;
}

struct nfs4_session *nfs4_session_find_for_client(struct server_state *ss,
						  struct nfs4_client *nc)
{
	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;
	struct nfs4_session *found = NULL;

	rcu_read_lock();
	cds_lfht_for_each(ss->ss_session_ht, &iter, node)
	{
		struct nfs4_session *tmp =
			caa_container_of(node, struct nfs4_session, ns_node);
		if (tmp->ns_client == nc && tmp->ns_cb_fd >= 0) {
			found = nfs4_session_get(tmp);
			if (found)
				break;
		}
	}
	rcu_read_unlock();

	return found;
}

/* ------------------------------------------------------------------ */
/* Op handlers                                                         */

uint32_t nfs4_op_exchange_id(struct compound *compound)
{
	EXCHANGE_ID4args *args = NFS4_OP_ARG_SETUP(compound, opexchange_id);
	EXCHANGE_ID4res *res = NFS4_OP_RES_SETUP(compound, opexchange_id);
	nfsstat4 *status = &res->eir_status;
	EXCHANGE_ID4resok *resok =
		NFS4_OP_RESOK_SETUP(res, EXCHANGE_ID4res_u, eir_resok4);

	u_int num_ops = compound->c_args->argarray.argarray_len;
	struct server_state *ss = compound->c_server_state;
	struct nfs4_client *nc = NULL;
	struct nfs_impl_id4 *impl_id = NULL;
	struct sockaddr_in sin;

	if (compound->c_curr_op == 0 && num_ops > 1) {
		*status = NFS4ERR_NOT_ONLY_OP;
		goto out;
	}

	/*
	 * RFC 8881 §18.35.3: validate eia_flags.
	 * Only the USE_* bits and UPD_CONFIRMED_REC_A are valid.
	 * Undefined bits → NFS4ERR_INVAL.
	 */
#define EID_VALID_FLAGS                                                   \
	(EXCHGID4_FLAG_SUPP_MOVED_REFER | EXCHGID4_FLAG_SUPP_MOVED_MIGR | \
	 EXCHGID4_FLAG_BIND_PRINC_STATEID | EXCHGID4_FLAG_USE_NON_PNFS |  \
	 EXCHGID4_FLAG_USE_PNFS_MDS | EXCHGID4_FLAG_USE_PNFS_DS |         \
	 EXCHGID4_FLAG_UPD_CONFIRMED_REC_A)
	if (args->eia_flags & ~EID_VALID_FLAGS) {
		*status = NFS4ERR_INVAL;
		goto out;
	}
#undef EID_VALID_FLAGS

	bool update_requested =
		!!(args->eia_flags & EXCHGID4_FLAG_UPD_CONFIRMED_REC_A);

	if (args->eia_client_impl_id.eia_client_impl_id_len > 0)
		impl_id = args->eia_client_impl_id.eia_client_impl_id_val;

	rpc_trans_get_sockaddr_in(compound->c_rt, &sin);

	nc = nfs4_client_alloc_or_find(ss, &args->eia_clientowner, impl_id,
				       &args->eia_clientowner.co_verifier,
				       &sin);
	if (!nc) {
		*status = NFS4ERR_SERVERFAULT;
		goto out;
	}

	/*
	 * If the client requested an update but the record is not
	 * confirmed, there is nothing to update.
	 */
	if (update_requested && !nc->nc_confirmed) {
		nfs4_client_put(nc);
		nc = NULL;
		*status = NFS4ERR_NOENT;
		goto out;
	}

	resok->eir_clientid = (clientid4)nfs4_client_to_client(nc)->c_id;
	resok->eir_sequenceid = 1;
	nc->nc_create_seq = resok->eir_sequenceid;

	resok->eir_flags = ss->ss_exchgid_flags;
	if (nc->nc_confirmed)
		resok->eir_flags |= EXCHGID4_FLAG_CONFIRMED_R;

	if (args->eia_state_protect.spa_how != SP4_NONE) {
		*status = NFS4ERR_NOTSUPP;
		goto out;
	}
	resok->eir_state_protect.spr_how = SP4_NONE;

	char *major_id = calloc(1, ss->ss_owner_id_len + 1);
	char *scope = calloc(1, ss->ss_owner_id_len + 1);

	if (!major_id || !scope) {
		free(major_id);
		free(scope);
		*status = NFS4ERR_SERVERFAULT;
		goto out;
	}
	memcpy(major_id, ss->ss_owner_id, ss->ss_owner_id_len);
	memcpy(scope, ss->ss_owner_id, ss->ss_owner_id_len);

	resok->eir_server_owner.so_minor_id = 0;
	resok->eir_server_owner.so_major_id.so_major_id_val = major_id;
	resok->eir_server_owner.so_major_id.so_major_id_len =
		ss->ss_owner_id_len;

	resok->eir_server_scope.eir_server_scope_val = scope;
	resok->eir_server_scope.eir_server_scope_len = ss->ss_owner_id_len;

	resok->eir_server_impl_id.eir_server_impl_id_val = NULL;
	resok->eir_server_impl_id.eir_server_impl_id_len = 0;

	compound->c_nfs4_client = nc;
	nc = NULL;

out:
	nfs4_client_put(nc);

	return 0;
}

uint32_t nfs4_op_create_session(struct compound *compound)
{
	CREATE_SESSION4args *args =
		NFS4_OP_ARG_SETUP(compound, opcreate_session);
	CREATE_SESSION4res *res = NFS4_OP_RES_SETUP(compound, opcreate_session);
	nfsstat4 *status = &res->csr_status;
	CREATE_SESSION4resok *resok =
		NFS4_OP_RESOK_SETUP(res, CREATE_SESSION4res_u, csr_resok4);

	u_int num_ops = compound->c_args->argarray.argarray_len;
	struct nfs4_client *nc = NULL;
	struct nfs4_session *ns = NULL;

	if (compound->c_curr_op == 0 && num_ops > 1) {
		*status = NFS4ERR_NOT_ONLY_OP;
		goto out;
	}

	/*
	 * RFC 8881 §18.36.3: validate flags.  Only defined flags are
	 * CREATE_SESSION4_FLAG_PERSIST and
	 * CREATE_SESSION4_FLAG_CONN_BACK_CHAN and
	 * CREATE_SESSION4_FLAG_CONN_RDMA.
	 * Undefined flags → NFS4ERR_INVAL.
	 */
#define CSESS_VALID_FLAGS                                                     \
	(CREATE_SESSION4_FLAG_PERSIST | CREATE_SESSION4_FLAG_CONN_BACK_CHAN | \
	 CREATE_SESSION4_FLAG_CONN_RDMA)
	if (args->csa_flags & ~CSESS_VALID_FLAGS) {
		*status = NFS4ERR_INVAL;
		goto out;
	}
#undef CSESS_VALID_FLAGS

	nc = nfs4_client_find(args->csa_clientid);
	if (!nc) {
		*status = NFS4ERR_STALE_CLIENTID;
		goto out;
	}

	/*
	 * RFC 8881 §18.36.4: sequence ID validation.
	 *   csa_sequence == nc_create_seq → replay, return cached result
	 *   csa_sequence == nc_create_seq + 1 → new request (but we
	 *     don't require +1, just != cached seqid, for flexibility)
	 *   anything else → NFS4ERR_SEQ_MISORDERED
	 */
	if (nc->nc_create_reply && args->csa_sequence == nc->nc_create_seq) {
		/* Replay: return the cached CREATE_SESSION result. */
		XDR xdrs;
		xdrmem_create(&xdrs, nc->nc_create_reply,
			      nc->nc_create_reply_len, XDR_DECODE);
		if (xdr_CREATE_SESSION4resok(&xdrs, resok)) {
			xdr_destroy(&xdrs);
			nfs4_client_put(nc);
			nc = NULL;
			goto out; /* status is NFS4_OK (calloc'd) */
		}
		xdr_destroy(&xdrs);
		/* Decode failed — fall through to create new session. */
	}

	/*
	 * Seqid validation: the first CREATE_SESSION uses
	 * csa_sequence == nc_create_seq (set by EXCHANGE_ID).
	 * After a successful CREATE_SESSION, nc_create_seq is
	 * bumped, so subsequent requests must use the new value.
	 * Only enforce when a prior reply was cached (not first time).
	 */
	if (nc->nc_create_reply &&
	    args->csa_sequence != nc->nc_create_seq + 1) {
		nfs4_client_put(nc);
		nc = NULL;
		*status = NFS4ERR_SEQ_MISORDERED;
		goto out;
	}

	/*
	 * RFC 8881 §18.36.3: channel attrs — accept the client's
	 * requested sizes.  nfs4_session_alloc clamps to the server's
	 * limits.  Only reject if truly unusable (zero).
	 */
	if (args->csa_fore_chan_attrs.ca_maxrequestsize == 0 ||
	    args->csa_fore_chan_attrs.ca_maxresponsesize == 0) {
		nfs4_client_put(nc);
		nc = NULL;
		*status = NFS4ERR_TOOSMALL;
		goto out;
	}

	/*
	 * RFC 8881 §18.36.3: RDMA not supported.
	 */
	if (args->csa_fore_chan_attrs.ca_rdma_ird.ca_rdma_ird_len > 0) {
		nfs4_client_put(nc);
		nc = NULL;
		*status = NFS4ERR_INVAL;
		goto out;
	}

	ns = nfs4_session_alloc(compound->c_server_state, nc,
				&args->csa_fore_chan_attrs, args->csa_flags);
	if (!ns) {
		*status = NFS4ERR_DELAY;
		goto out;
	}

	nc->nc_confirmed = true;

	/* Save back-channel parameters. */
	ns->ns_cb_program = args->csa_cb_program;
	ns->ns_cb_fd = compound->c_rt->rt_fd;
	ns->ns_cb_seqid = 0;

	memcpy(resok->csr_sessionid, ns->ns_sessionid, sizeof(sessionid4));
	resok->csr_sequence = args->csa_sequence;
	resok->csr_flags = 0;

	resok->csr_fore_chan_attrs.ca_headerpadsize = ns->ns_headerpadsize;
	resok->csr_fore_chan_attrs.ca_maxrequestsize = ns->ns_maxrequestsize;
	resok->csr_fore_chan_attrs.ca_maxresponsesize = ns->ns_maxresponsesize;
	resok->csr_fore_chan_attrs.ca_maxresponsesize_cached =
		ns->ns_maxresponsesize_cached;
	resok->csr_fore_chan_attrs.ca_maxoperations = ns->ns_maxoperations;
	resok->csr_fore_chan_attrs.ca_maxrequests = ns->ns_slot_count;
	resok->csr_fore_chan_attrs.ca_rdma_ird.ca_rdma_ird_len = 0;
	resok->csr_fore_chan_attrs.ca_rdma_ird.ca_rdma_ird_val = NULL;

	/*
	 * Back channel: share the fore-channel connection; accept the
	 * client's requested attrs (clamped to sensible limits) and echo
	 * them back so the client knows we'll honour them.
	 */
	resok->csr_back_chan_attrs.ca_headerpadsize = 0;
	resok->csr_back_chan_attrs.ca_maxrequestsize =
		args->csa_back_chan_attrs.ca_maxrequestsize ?
			args->csa_back_chan_attrs.ca_maxrequestsize :
			NFS4_SESSION_MAX_REQUEST_SIZE;
	resok->csr_back_chan_attrs.ca_maxresponsesize =
		args->csa_back_chan_attrs.ca_maxresponsesize ?
			args->csa_back_chan_attrs.ca_maxresponsesize :
			NFS4_SESSION_MAX_RESPONSE_SIZE;
	resok->csr_back_chan_attrs.ca_maxresponsesize_cached = 0;
	resok->csr_back_chan_attrs.ca_maxoperations =
		args->csa_back_chan_attrs.ca_maxoperations ?
			args->csa_back_chan_attrs.ca_maxoperations :
			NFS4_SESSION_MAX_OPS;
	resok->csr_back_chan_attrs.ca_maxrequests = 1; /* single CB slot */
	resok->csr_back_chan_attrs.ca_rdma_ird.ca_rdma_ird_len = 0;
	resok->csr_back_chan_attrs.ca_rdma_ird.ca_rdma_ird_val = NULL;

	/*
	 * Cache the result for replay detection.  Record the seqid
	 * so we can detect replays vs new requests.
	 */
	nc->nc_create_seq = args->csa_sequence;
	free(nc->nc_create_reply);
	nc->nc_create_reply = NULL;
	nc->nc_create_reply_len = 0;
	{
		uint32_t sz =
			xdr_sizeof((xdrproc_t)xdr_CREATE_SESSION4resok, resok);
		nc->nc_create_reply = calloc(1, sz);
		if (nc->nc_create_reply) {
			XDR xdrs;
			xdrmem_create(&xdrs, nc->nc_create_reply, sz,
				      XDR_ENCODE);
			if (xdr_CREATE_SESSION4resok(&xdrs, resok)) {
				nc->nc_create_reply_len = sz;
			} else {
				free(nc->nc_create_reply);
				nc->nc_create_reply = NULL;
			}
			xdr_destroy(&xdrs);
		}
	}

out:
	nfs4_session_put(ns);
	nfs4_client_put(nc);

	return 0;
}

uint32_t nfs4_op_destroy_session(struct compound *compound)
{
	DESTROY_SESSION4args *args =
		NFS4_OP_ARG_SETUP(compound, opdestroy_session);
	DESTROY_SESSION4res *res =
		NFS4_OP_RES_SETUP(compound, opdestroy_session);
	nfsstat4 *status = &res->dsr_status;

	u_int num_ops = compound->c_args->argarray.argarray_len;
	struct nfs4_session *ns = NULL;

	if (compound->c_curr_op == 0 && num_ops > 1) {
		*status = NFS4ERR_NOT_ONLY_OP;
		goto out;
	}

	ns = nfs4_session_find(compound->c_server_state, args->dsa_sessionid);
	if (!ns) {
		*status = NFS4ERR_BADSESSION;
		goto out;
	}

	nfs4_session_unhash(compound->c_server_state, ns);

out:
	nfs4_session_put(ns);

	return 0;
}

uint32_t nfs4_op_sequence(struct compound *compound)
{
	SEQUENCE4args *args = NFS4_OP_ARG_SETUP(compound, opsequence);
	SEQUENCE4res *res = NFS4_OP_RES_SETUP(compound, opsequence);
	nfsstat4 *status = &res->sr_status;
	SEQUENCE4resok *resok =
		NFS4_OP_RESOK_SETUP(res, SEQUENCE4res_u, sr_resok4);

	struct nfs4_session *ns = NULL;
	struct nfs4_slot *slot;

	if (compound->c_curr_op != 0) {
		*status = NFS4ERR_SEQUENCE_POS;
		goto out;
	}

	ns = nfs4_session_find(compound->c_server_state, args->sa_sessionid);
	if (!ns) {
		*status = NFS4ERR_BADSESSION;
		goto out;
	}

	if (args->sa_slotid >= ns->ns_slot_count) {
		*status = NFS4ERR_BADSLOT;
		goto out;
	}

	slot = &ns->ns_slots[args->sa_slotid];

	pthread_mutex_lock(&slot->sl_mutex);

	if (args->sa_sequenceid == slot->sl_seqid) {
		/* Replay of the last request on this slot. */
		if (slot->sl_state == NFS4_SLOT_IN_USE) {
			pthread_mutex_unlock(&slot->sl_mutex);
			*status = NFS4ERR_DELAY;
			goto out;
		}

		if (slot->sl_reply) {
			XDR xdrs;
			COMPOUND4res *full_res = compound->c_res;

			/*
			 * Free the pre-allocated resarray before the
			 * decode overwrites the pointer — prevents leak.
			 */
			free(full_res->resarray.resarray_val);
			full_res->resarray.resarray_val = NULL;
			full_res->resarray.resarray_len = 0;

			/* Also free any tag that was already copied. */
			free(full_res->tag.utf8string_val);
			full_res->tag.utf8string_val = NULL;
			full_res->tag.utf8string_len = 0;

			xdrmem_create(&xdrs, slot->sl_reply, slot->sl_reply_len,
				      XDR_DECODE);
			if (xdr_COMPOUND4res(&xdrs, full_res)) {
				pthread_mutex_unlock(&slot->sl_mutex);
				xdr_destroy(&xdrs);
				/*
				 * The full compound result is now in
				 * c_res.  Return NFS4_OP_FLAG_REPLAY
				 * so dispatch_compound stops the loop
				 * without accessing resarray past the
				 * end.
				 */
				return NFS4_OP_FLAG_REPLAY;
			}
			xdr_destroy(&xdrs);
		}

		/*
		 * Replay but no cached reply (sa_cachethis was false).
		 * RFC 8881 §2.10.6.1.3.
		 */
		pthread_mutex_unlock(&slot->sl_mutex);
		*status = NFS4ERR_RETRY_UNCACHED_REP;
		goto out;
	}

	if (args->sa_sequenceid == (slot->sl_seqid + 1)) {
		/* Correct next request. */
		slot->sl_seqid = args->sa_sequenceid;
		slot->sl_state = NFS4_SLOT_IN_USE;
		free(slot->sl_reply);
		slot->sl_reply = NULL;
		slot->sl_reply_len = 0;
	} else {
		pthread_mutex_unlock(&slot->sl_mutex);
		*status = NFS4ERR_SEQ_MISORDERED;
		goto out;
	}

	pthread_mutex_unlock(&slot->sl_mutex);

	memcpy(resok->sr_sessionid, ns->ns_sessionid, sizeof(sessionid4));
	resok->sr_sequenceid = slot->sl_seqid;
	resok->sr_slotid = args->sa_slotid;
	resok->sr_highest_slotid = ns->ns_slot_count - 1;
	resok->sr_target_highest_slotid = ns->ns_slot_count - 1;
	resok->sr_status_flags = 0;

	if (server_in_grace(compound->c_server_state))
		resok->sr_status_flags |= SEQ4_STATUS_RESTART_RECLAIM_NEEDED;

	compound->c_session = nfs4_session_get(ns);
	compound->c_slot = slot;

	compound->c_nfs4_client = nfs4_client_get(ns->ns_client);

	/* Implicit lease renewal — RFC 5661 §8.3. */
	__atomic_store_n(&ns->ns_client->nc_last_renew_ns, reffs_now_ns(),
			 __ATOMIC_RELEASE);
	ns = NULL; /* ref transferred to c_session */

out:
	nfs4_session_put(ns);

	return 0;
}

uint32_t nfs4_op_renew(struct compound *compound)
{
	RENEW4res *res = NFS4_OP_RES_SETUP(compound, oprenew);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	return 0;
}

uint32_t nfs4_op_setclientid(struct compound *compound)
{
	SETCLIENTID4res *res = NFS4_OP_RES_SETUP(compound, opsetclientid);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	return 0;
}

uint32_t nfs4_op_setclientid_confirm(struct compound *compound)
{
	SETCLIENTID_CONFIRM4res *res =
		NFS4_OP_RES_SETUP(compound, opsetclientid_confirm);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	return 0;
}

uint32_t nfs4_op_destroy_clientid(struct compound *compound)
{
	DESTROY_CLIENTID4args *args =
		NFS4_OP_ARG_SETUP(compound, opdestroy_clientid);
	DESTROY_CLIENTID4res *res =
		NFS4_OP_RES_SETUP(compound, opdestroy_clientid);
	nfsstat4 *status = &res->dcr_status;

	u_int num_ops = compound->c_args->argarray.argarray_len;
	struct server_state *ss = compound->c_server_state;
	struct nfs4_client *nc = NULL;

	if (compound->c_curr_op == 0 && num_ops > 1) {
		*status = NFS4ERR_NOT_ONLY_OP;
		goto out;
	}

	nc = nfs4_client_find(args->dca_clientid);
	if (!nc) {
		*status = NFS4ERR_STALE_CLIENTID;
		goto out;
	}

	if (__atomic_load_n(&nc->nc_session_count, __ATOMIC_ACQUIRE) > 0) {
		*status = NFS4ERR_CLIENTID_BUSY;
		goto out;
	}

	nfs4_client_expire(ss, nc);
	nc = NULL;

out:
	nfs4_client_put(nc);

	return 0;
}

uint32_t nfs4_op_reclaim_complete(struct compound *compound)
{
	RECLAIM_COMPLETE4args *args =
		NFS4_OP_ARG_SETUP(compound, opreclaim_complete);
	RECLAIM_COMPLETE4res *res =
		NFS4_OP_RES_SETUP(compound, opreclaim_complete);
	nfsstat4 *status = &res->rcr_status;

	struct server_state *ss = compound->c_server_state;
	struct nfs4_client *nc = compound->c_nfs4_client;

	if (!nc) {
		*status = NFS4ERR_OP_NOT_IN_SESSION;
		goto out;
	}

	/*
	 * rca_one_fs: per-filesystem reclaim complete — not meaningful
	 * for a DS; accept and ignore without touching the counter.
	 */
	if (!args->rca_one_fs) {
		bool was_reclaiming = __atomic_exchange_n(
			&nc->nc_needs_reclaim, false, __ATOMIC_ACQ_REL);
		if (was_reclaiming)
			server_reclaim_complete(ss);
	}

out:
	return 0;
}

uint32_t nfs4_op_bind_conn_to_session(struct compound *compound)
{
	BIND_CONN_TO_SESSION4args *args =
		NFS4_OP_ARG_SETUP(compound, opbind_conn_to_session);
	BIND_CONN_TO_SESSION4res *res =
		NFS4_OP_RES_SETUP(compound, opbind_conn_to_session);
	nfsstat4 *status = &res->bctsr_status;
	BIND_CONN_TO_SESSION4resok *resok = NFS4_OP_RESOK_SETUP(
		res, BIND_CONN_TO_SESSION4res_u, bctsr_resok4);

	struct nfs4_session *ns;

	/*
	 * We do not support RDMA or the backchannel.  A request that
	 * exclusively targets the back channel is refused; fore-or-both
	 * is satisfied by binding the fore channel only.
	 */
	if (args->bctsa_use_conn_in_rdma_mode) {
		*status = NFS4ERR_NOTSUPP;
		return 0;
	}

	if (args->bctsa_dir == CDFC4_BACK) {
		*status = NFS4ERR_NOTSUPP;
		return 0;
	}

	ns = nfs4_session_find(compound->c_server_state, args->bctsa_sessid);
	if (!ns) {
		*status = NFS4ERR_BADSESSION;
		return 0;
	}

	memcpy(resok->bctsr_sessid, ns->ns_sessionid, sizeof(sessionid4));
	resok->bctsr_dir = CDFS4_FORE;
	resok->bctsr_use_conn_in_rdma_mode = FALSE;

	nfs4_session_put(ns);

	return 0;
}

uint32_t nfs4_op_backchannel_ctl(struct compound *compound)
{
	BACKCHANNEL_CTL4res *res =
		NFS4_OP_RES_SETUP(compound, opbackchannel_ctl);
	nfsstat4 *status = &res->bcr_status;

	*status = NFS4ERR_NOTSUPP;

	return 0;
}

uint32_t nfs4_op_set_ssv(struct compound *compound)
{
	SET_SSV4res *res = NFS4_OP_RES_SETUP(compound, opset_ssv);
	nfsstat4 *status = &res->ssr_status;

	*status = NFS4ERR_NOTSUPP;

	return 0;
}
