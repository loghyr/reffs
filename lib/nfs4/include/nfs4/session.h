/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_NFS4_SESSION_H
#define _REFFS_NFS4_SESSION_H

#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

#include <urcu/ref.h>
#include <urcu/rculfhash.h>

#include "nfsv42_xdr.h"
#include "reffs/rcu.h"
#include "nfs4/client.h"

struct server_state;

#define NFS4_SESSION_IS_HASHED (1ULL << 0)
#define NFS4_SESSION_IS_ZOMBIE (1ULL << 1)

#define NFS4_SESSION_MAX_SLOTS 64U
/*
 * Must exceed the max WRITE/READ data size plus RPC/XDR overhead
 * (~4KB for SEQUENCE+PUTFH+WRITE headers).  If too small, the
 * server returns NFS4ERR_REQ_TOO_BIG and the client gets EIO.
 */
#define NFS4_SESSION_MAX_REQUEST_SIZE (1024U * 1024U + 64U * 1024U)
#define NFS4_SESSION_MAX_RESPONSE_SIZE (1024U * 1024U + 64U * 1024U)
#define NFS4_SESSION_MAX_RESPONSE_CACHED 4096U
#define NFS4_SESSION_MAX_OPS 16U

/* ------------------------------------------------------------------ */
/* Slot                                                                 */

enum nfs4_slot_state {
	NFS4_SLOT_IDLE = 0,
	NFS4_SLOT_IN_USE,
	NFS4_SLOT_CACHED,
};

/*
 * nfs4_slot - one entry in the fore-channel slot table.
 *
 * sl_seqid: sequence number of the last completed (or in-flight) request;
 *           0 means the slot has never been used.
 * sl_reply: XDR-encoded COMPOUND4res for replay; NULL until a request
 *           completes with sa_cachethis set.  Freed when the next new
 *           request arrives on this slot.
 */
struct nfs4_slot {
	sequenceid4 sl_seqid;
	enum nfs4_slot_state sl_state;
	pthread_mutex_t sl_mutex;
	void *sl_reply; /* calloc'd XDR buffer, or NULL */
	uint32_t sl_reply_len;
};

/* ------------------------------------------------------------------ */
/* Session                                                              */

/*
 * nfs4_session - per-session state for NFSv4.1 compounds.
 *
 * Created by CREATE_SESSION after EXCHANGE_ID.  The session holds one
 * ref on its owning nfs4_client for its lifetime.  A compound holds
 * one ref on the session for its duration (set by SEQUENCE).
 *
 * Fore-channel attributes are stored as plain scalars.  We do not
 * support RDMA; ca_rdma_ird is always empty in our responses.
 */
struct nfs4_session {
	sessionid4 ns_sessionid;
	struct nfs4_client *ns_client; /* ref held */

	/* Negotiated fore-channel attributes */
	uint32_t ns_headerpadsize;
	uint32_t ns_maxrequestsize;
	uint32_t ns_maxresponsesize;
	uint32_t ns_maxresponsesize_cached;
	uint32_t ns_maxoperations;

	uint32_t ns_flags;

	uint32_t ns_slot_count;
	struct nfs4_slot *ns_slots; /* calloc'd [0..ns_slot_count-1] */

	/* Back channel */
	uint32_t ns_cb_program; /* csa_cb_program from CREATE_SESSION */
	int ns_cb_fd; /* fd of the connection (fore-channel reused) */
	sequenceid4 ns_cb_seqid; /* next CB_SEQUENCE sequenceid to send */
	pthread_mutex_t ns_cb_mutex; /* serializes CB sends on this session */

	struct rcu_head ns_rcu;
	struct urcu_ref ns_ref;
	uint64_t ns_state; /* atomic flag word */
	struct cds_lfht_node ns_node; /* in ss->ss_session_ht */

	void (*ns_free_rcu)(struct rcu_head *rcu);
	void (*ns_release)(struct urcu_ref *ref);
};

/* ------------------------------------------------------------------ */
/* Lifetime                                                             */

/*
 * nfs4_session_alloc - allocate a session for nc, negotiating fore-channel
 * attributes against the server caps.  Inserts into ss->ss_session_ht.
 *
 * Returns a ref-bumped session or NULL.  Caller must nfs4_session_put().
 */
struct nfs4_session *nfs4_session_alloc(struct server_state *ss,
					struct nfs4_client *nc,
					const channel_attrs4 *fore_req,
					uint32_t flags);

/*
 * nfs4_session_find - look up by sessionid4.
 * Returns a ref-bumped session or NULL.  Caller must nfs4_session_put().
 */
struct nfs4_session *nfs4_session_find(struct server_state *ss,
				       const sessionid4 sid);

/*
 * nfs4_session_find_for_client - find any session for nc that has a
 * valid back-channel fd (ns_cb_fd >= 0).
 * Returns a ref-bumped session or NULL.  Caller must nfs4_session_put().
 */
struct nfs4_session *nfs4_session_find_for_client(struct server_state *ss,
						  struct nfs4_client *nc);

struct nfs4_session *nfs4_session_get(struct nfs4_session *ns);
void nfs4_session_put(struct nfs4_session *ns);

/* Remove from ss_session_ht (idempotent).  Returns true if it was hashed. */
bool nfs4_session_unhash(struct server_state *ss, struct nfs4_session *ns);

/* Destroy all sessions belonging to nc.  Used by nfs4_client_expire. */
void nfs4_session_destroy_for_client(struct server_state *ss,
				     struct nfs4_client *nc);

/*
 * Re-parent old_nc's sessions to new_nc as zombies.  After this,
 * old_nc has no sessions and can be expired without session loss.
 * Used by replace_client for RFC 8881 Table 11 case 7.
 */
void nfs4_session_reparent_for_replace(struct server_state *ss,
				       struct nfs4_client *old_nc,
				       struct nfs4_client *new_nc);

/* Destroy all zombie sessions on nc.  Called on CREATE_SESSION confirm. */
void nfs4_session_destroy_zombies(struct server_state *ss,
				  struct nfs4_client *nc);

#endif /* _REFFS_NFS4_SESSION_H */
