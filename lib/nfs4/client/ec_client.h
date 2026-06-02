/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * NFSv4.2 client library -- the public surface every reffs MDS-
 * facing consumer (ec_demo, the PS forwarders, the dstore-vtable-v2
 * MDS-to-DS path, nfs_krb5_test) calls.
 *
 * Talks NFSv4.2 to the MDS (EXCHANGE_ID, CREATE_SESSION, SEQUENCE,
 * OPEN, LAYOUTGET, GETDEVICEINFO, LAYOUTRETURN, CLOSE) and NFSv3 to
 * the data servers (READ, WRITE).  CHUNK ops over NFSv4.2 are also
 * here for the v2 / RFC 9754 erasure path.
 *
 * History: this header is named ec_client.h because the original
 * caller was the standalone tool tools/ec_demo.  It has since grown
 * a second life as the MDS-client library for everything PS-side,
 * which is why ec_demo-specific types (ec_layout, ec_mirror,
 * ec_device) keep the prefix even though no consumer of this header
 * is required to be an EC tool.
 *
 * Re-entrancy / threading contract:
 *   - struct mds_session is single-owner.  Two threads must not
 *     drive the same session concurrently; mds_compound_send takes
 *     ms_call_mutex internally so per-compound auth swaps inside
 *     the PS forwarders are safe, but higher-level state (slot
 *     seqid, layout state, last-error) is not protected against
 *     reordering across threads.  The PS uses one session per
 *     listener; ec_demo uses one session per CLI invocation.
 *   - struct ds_conn is single-owner per (DS, client-uid/gid).
 *     The PS dedups DS connections per (host:port, uid, gid)
 *     tuple; concurrent reads / writes on the same ds_conn are
 *     not safe (libtirpc CLIENT* serialisation is at clnt_call
 *     granularity, and ds_read / ds_write hold no extra mutex).
 *   - struct mds_compound and struct ec_layout are stack-local;
 *     no thread shares them.
 *   - mds_session_destroy is only safe from the same thread that
 *     last drove the session; callers needing a destroy from a
 *     different thread should serialise externally.
 */

#ifndef _REFFS_EC_CLIENT_H
#define _REFFS_EC_CLIENT_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <rpc/rpc.h>
#include <rpc/auth_unix.h>

#include "nfsv42_xdr.h"

/*
 * Forward decl for the PS listener state.  Defined in
 * lib/nfs4/ps/ps_state.h; opaque to non-PS callers.  Threaded as
 * the trailing `pls` parameter on the public ec_pipeline entry
 * points (Phase 5 short-circuit).  ec_demo and other standalone
 * callers pass NULL -- the dispatch then leaves em_local false on
 * every mirror and the existing RPC path runs unchanged.
 */
struct ps_listener_state;

/* ------------------------------------------------------------------ */
/* MDS session                                                         */
/* ------------------------------------------------------------------ */

struct mds_session {
	CLIENT *ms_clnt; /* primary transport == ms_clnts[0] when ms_nconnect > 0 */
	/*
	 * Optional secondary transports for kernel-style nconnect.
	 *
	 * Default (single-transport): ms_clnts == NULL, ms_nconnect == 1,
	 * ms_clnt is the only transport.  Every existing caller of
	 * mds_session_create / _sec / _sec_spn / _tls lands here byte-
	 * identical to before; mds_compound_send_with_auth's hot path
	 * resolves to ms_clnt without an array deref.
	 *
	 * Multi-transport (kernel-style nconnect, opt-in via the
	 * mds_session_create_sec_spn_nc variant): ms_clnts is an array
	 * of ms_nconnect CLIENT* with ms_clnts[0] == ms_clnt.  Transport
	 * 0 carries EXCHANGE_ID + CREATE_SESSION; transports 1..N-1 are
	 * bound to the same sessionid via BIND_CONN_TO_SESSION (RFC 8881
	 * sec 18.34).  All N carry their own RPCSEC_GSS context, which
	 * is the axis the krb5 multi-mount stress harness uses to drive
	 * concurrent gss_accept_sec_context calls on the server.
	 *
	 * COMPOUND submission round-robins across the array via
	 * ms_xprt_rr.  Single-slot serialisation via ms_call_mutex still
	 * applies (sa_slotid is hardcoded to 0 in mds_compound_add_sequence);
	 * the nconnect knob currently delivers wire-shape accuracy and
	 * GSS-context fan-out, NOT multi-slot throughput.  Multi-slot is
	 * a separate slice the consumers (ps, dstore, ec_pipeline) can
	 * opt into after it lands.
	 */
	CLIENT **ms_clnts;
	unsigned int ms_nconnect;
	_Atomic unsigned int ms_xprt_rr;
	clientid4 ms_clientid;
	sequenceid4 ms_create_seq;
	sessionid4 ms_sessionid;
	uint32_t ms_slot_seqid; /* next seqid for slot 0 */
	uint32_t ms_maxrequestsize; /* negotiated fore-channel ca_maxrequestsize */
	char ms_owner[256]; /* client owner string for EXCHANGE_ID */
	/*
	 * Serialises auth-swap + clnt_call inside mds_compound_send.
	 * CLIENT's cl_auth is a single-owner field; concurrent compounds
	 * that swap it in parallel would race on each other.  Today only
	 * the proxy-server forwarders swap auth (per-compound end-client
	 * creds); all other users leave the default auth from
	 * mds_session_create and still pay the lock overhead -- a tiny
	 * cost for code simplicity at reffsd's current concurrency.
	 */
	pthread_mutex_t ms_call_mutex;
	/*
	 * Default auth installed at session_create (authunix with PS's
	 * own service identity).  Kept so mds_compound_send can restore
	 * it after a per-compound override.  NULL means "never
	 * installed" (e.g. GSS session that doesn't use ms_auth_default).
	 */
	AUTH *ms_auth_default;
	/*
	 * Owned SSL_CTX for TLS-protected sessions
	 * (mds_session_create_tls).  NULL on plain TCP sessions.
	 * mds_session_destroy frees this AFTER clnt_destroy so the
	 * SSL_free inside the custom XPRT runs while its parent CTX
	 * is still alive.  Type is void * so this header stays free
	 * of the OpenSSL include.
	 */
	void *ms_tls_ctx;
	/*
	 * EXCHGID4 flag the session sends in EXCHANGE_ID.  Default 0
	 * means "use the historical EXCHGID4_FLAG_USE_NON_PNFS" --
	 * what the PS-MDS path needs and what every existing caller
	 * of mds_session_create relies on.  The MDS-to-DS dstore
	 * vtable (lib/nfs4/dstore/dstore_ops_nfsv4.c when it lands)
	 * sets this to EXCHGID4_FLAG_USE_PNFS_MDS so trust_stateid
	 * gating on the DS side accepts TRUST_STATEID compounds from
	 * the MDS.  Set the field before mds_session_create*; zero
	 * preserves existing behaviour.  See task #140 in the topic
	 * board ("Plan A reviewer follow-up #1").
	 */
	uint32_t ms_exchgid_flags;
	/*
	 * Optional per-session tag used by the proxy-server forwarders
	 * (lib/nfs4/ps/ps_proxy_ops.c).  When non-zero, the worker-side
	 * compound-send wrapper classifies the result via
	 * ps_session_is_dead and, on a session-killer, calls
	 * ps_listener_kick_reconnect(ms_kick_listener_id) so the PS
	 * renewal thread starts the rebuild without waiting for the
	 * next renewal interval.  Default 0 means "no kick wanted":
	 * sessions that aren't owned by a PS listener (ec_demo client,
	 * dstore MDS-to-DS sessions) leave this at zero and the wrapper
	 * is a thin pass-through.
	 *
	 * _Atomic with relaxed ordering: writes happen at session
	 * publish (single writer, before any worker can see the
	 * session pointer), reads happen on the worker hot path.  The
	 * publish edge in ps_state_set_session / ps_listener_session_replace
	 * (rwlock-protected swap of pls_session) provides the
	 * synchronizes-with for the field's value to be visible.
	 * Relaxed is sufficient because the ordering already comes
	 * from the rwlock; the atomic qualifier is there to (a)
	 * eliminate data-race UB on the read side from many worker
	 * threads, and (b) keep TSAN clean if a future code path
	 * writes the field outside the rwlock-protected publish (e.g.
	 * an admin-driven retag) racing with worker reads.
	 */
	_Atomic uint32_t ms_kick_listener_id;
};

/*
 * mds_session_set_owner -- set the client owner string before create.
 *
 * Builds "hostname:id" where id defaults to the PID if not provided.
 * Must be called before mds_session_create().
 */
void mds_session_set_owner(struct mds_session *ms, const char *id);

enum ec_sec_flavor {
	EC_SEC_SYS = 0,
	EC_SEC_KRB5 = 1,
	EC_SEC_KRB5I = 2,
	EC_SEC_KRB5P = 3,
};

int mds_session_create(struct mds_session *ms, const char *host);
int mds_session_create_sec(struct mds_session *ms, const char *host,
			   enum ec_sec_flavor sec);

/*
 * Like mds_session_create_sec, with an explicit Kerberos target
 * service principal name (SPN) override.  When spn is non-NULL,
 * it is passed verbatim to authgss_create_default as the service
 * name.  Accepted forms:
 *
 *    nfs/host.example.com           principal-name; library fills
 *                                   in the default realm
 *    nfs/host.example.com@REALM     fully-qualified
 *    nfs@host.example.com           host-based service form; library
 *                                   canonicalizes to nfs/<host>@<REALM>
 *
 * When spn is NULL, the behavior is identical to mds_session_create_sec
 * (build "nfs@<host>" from the host argument).
 *
 * Intended for the krb5 stress reproducer (see
 * .claude/design/krb5-stress-multi-xprt.md): drives the server's
 * SPN-resolution path with caller-chosen principals rather than
 * letting the library default.
 */
int mds_session_create_sec_spn(struct mds_session *ms, const char *host,
			       enum ec_sec_flavor sec, const char *spn);

/*
 * Kernel-style nconnect variant of mds_session_create_sec_spn.
 *
 * Opens nconnect TCP transports to host.  Transport 0 carries
 * EXCHANGE_ID + CREATE_SESSION; transports 1..nconnect-1 are bound
 * to the same sessionid4 via BIND_CONN_TO_SESSION (RFC 8881 sec
 * 18.34, cdfc4_dir = CDFC4_FORE).  Each transport carries its own
 * RPCSEC_GSS context -- nconnect parallel gss_init_sec_context /
 * gss_accept_sec_context exchanges, which is the load shape the
 * krb5 multi-mount stress harness drives at scale.
 *
 * COMPOUND submission round-robins across the transports inside
 * mds_compound_send_with_auth (ms_xprt_rr counter).  Single-slot
 * serialisation via ms_call_mutex still applies; the current client
 * lib hardcodes sa_slotid = 0 so the nconnect knob delivers
 * wire-shape accuracy and per-transport GSS context fan-out, NOT
 * throughput improvement.  See ec_client.h struct comment.
 *
 * nconnect == 1 is byte-identical to mds_session_create_sec_spn.
 * nconnect == 0 is rejected (-EINVAL).
 */
int mds_session_create_sec_spn_nc(struct mds_session *ms, const char *host,
				  enum ec_sec_flavor sec, const char *spn,
				  unsigned int nconnect);

/*
 * TLS variant for the PS-MDS session (slice plan-1-tls.b,
 * .claude/design/proxy-server-tls.md).  When tls_cert and tls_key
 * are non-empty, opens a TCP connection to host:port, brings up
 * mutually-authenticated TLS via tls_starttls (or direct TLS if
 * tls_mode == REFFS_PROXY_TLS_DIRECT, value 2), then wraps the
 * SSL in a libtirpc CLIENT* via mds_tls_xprt_create.  When the
 * cert paths are empty, falls through to the plain mds_session_create
 * path.
 *
 * tls_mode values mirror enum reffs_proxy_tls_mode in
 * lib/include/reffs/settings.h: 0 = OFF, 1 = STARTTLS, 2 = DIRECT.
 * Header avoids the settings.h dep so ec_client stays light.
 */
int mds_session_create_tls(struct mds_session *ms, const char *host,
			   uint16_t port, const char *tls_cert,
			   const char *tls_key, const char *tls_ca,
			   int tls_mode, bool tls_insecure_no_verify);

void mds_session_destroy(struct mds_session *ms);

/*
 * Map a PROXY_REGISTRATION COMPOUND or per-op nfsstat4 onto the
 * negative-errno value the PS startup loop expects.  Surfaced here
 * (not file-local in mds_session.c) so unit tests can pin the
 * mapping without driving a full RPC round-trip; callers other
 * than PROXY_REGISTRATION should not use it -- the mapping reflects
 * that op's documented error contract specifically.
 */
int proxy_reg_nfsstat_to_errno(nfsstat4 status);

/*
 * Slice plan-A.iii: PS-side PROXY_REGISTRATION send.  Builds the
 * compound `SEQUENCE PROXY_REGISTRATION(reg_id)` and sends it to
 * the upstream MDS over `ms`.  The PS uses this once at startup
 * to declare itself a registered Proxy Server; the MDS validates
 * the identity context (`compound->c_gss_principal` from slice
 * plan-A.i, or `compound->c_tls_fingerprint` from plan-A.ii)
 * against the `[[allowed_ps]]` allowlist (slice 6b-i).
 *
 * `registration_id` is a stable per-PS-instance opaque cookie the
 * MDS uses to distinguish a renewal (same id, refreshes lease)
 * from a squat attempt (different id while prior lease is valid;
 * MDS returns NFS4ERR_DELAY).  Caller is responsible for
 * persisting the id across PS-process restarts (see
 * proxy-server-plan-a.md "registration_id persistence").
 *
 * Returns 0 on NFS4_OK, -EPERM if the MDS rejected (allowlist
 * miss / AUTH_SYS / wrong session flag), -EAGAIN on
 * NFS4ERR_DELAY (squat blocked; caller MAY retry after one
 * lease period), other -errno on protocol failures.
 */
int mds_session_send_proxy_registration(struct mds_session *ms,
					const uint8_t *registration_id,
					uint32_t registration_id_len);

/*
 * Send a SEQUENCE-only compound to renew the upstream lease.
 *
 * Used by the PS renewal thread to keep its [[proxy_mds]] sessions
 * alive between bursts of forwarded client traffic.  Without periodic
 * renewals the upstream MDS expires the session after one lease
 * period (~90s by default) and subsequent forwards return
 * NFS4ERR_BADSESSION; see .claude/design/proxy-server.md "Phase 6
 * follow-on: PS upstream session keepalive".
 *
 * Returns:
 *   0        success (server accepted SEQUENCE; session still alive)
 *   -errno   wire failure or non-OK SEQUENCE status; caller logs and
 *            optionally schedules a reconnect via the _ex variant
 *            below + ps_session_is_dead().
 */
int mds_session_renew_lease(struct mds_session *ms);

/*
 * Same as mds_session_renew_lease but also exposes the SEQUENCE op's
 * wire-level sr_status.  The PS reconnect path needs this to
 * distinguish session-killer codes (NFS4ERR_BADSESSION,
 * NFS4ERR_DEADSESSION, NFS4ERR_STALE_CLIENTID) from per-op transients
 * like NFS4ERR_DELAY -- mds_compound_send flattens both into
 * -EREMOTEIO.
 *
 * On a successful round-trip with a non-OK SEQUENCE status, returns
 * -EREMOTEIO and sets *sr_status_out to the wire status.  On RPC-
 * level failure (no decoded SEQUENCE result), returns the underlying
 * errno and sets *sr_status_out to NFS4_OK.  sr_status_out may be
 * NULL.
 */
int mds_session_renew_lease_ex(struct mds_session *ms, nfsstat4 *sr_status_out);

/*
 * Session-death classifier shared by both the PS-to-MDS and the
 * MDS-to-DS keep-alive paths (lib/nfs4/ps/ps_renewal.c +
 * lib/nfs4/dstore/ds_renewal.c).  Promoted from ps_state.c so DS
 * renewal does not have to copy-paste.  Returns true when the
 * combination of (errno from the renewal call, sr_status from the
 * SEQUENCE op) indicates the session is dead and the caller should
 * destroy + reconnect.  Returns false for per-op transients
 * (e.g. NFS4ERR_DELAY) where the session is still alive.
 *
 * Session-killer NFSv4.2 statuses: NFS4ERR_BADSESSION,
 * NFS4ERR_DEADSESSION, NFS4ERR_STALE_CLIENTID,
 * NFS4ERR_BAD_SESSION_DIGEST.
 *
 * Session-killer wire errnos: -EIO, -EPIPE, -ECONNRESET, -ETIMEDOUT,
 * -ENOTCONN, -ENETUNREACH (TCP teardown signals).
 *
 * See .claude/design/mds-ds-session-keepalive.md.
 */
bool mds_session_is_dead(int err, nfsstat4 sr_status);

/*
 * Exponential backoff scheduler for reconnect attempts after a
 * session-killer.  Schedule: 0, 1, 2, 4, 8, 16, 32, 60, 60, ...
 * First call (*backoff_sec == 0) returns 0 (immediate retry
 * permitted) and bumps *backoff_sec to 1.  Subsequent calls return
 * the prior wait and double it, capped at 60.
 *
 * Promoted from ps_state.c alongside mds_session_is_dead.
 */
uint32_t mds_reconnect_backoff_next(uint32_t *backoff_sec);

/* Reset the backoff counter to 0 after a successful reconnect. */
void mds_reconnect_backoff_reset(uint32_t *backoff_sec);

/*
 * Slice 6c-z: PS-side PROXY_PROGRESS / PROXY_DONE / PROXY_CANCEL
 * senders + the PS migration step driver.  See
 * lib/nfs4/server/proxy_registration.c for the MDS side and
 * draft-haynes-nfsv4-flexfiles-v2-data-mover sec-PROXY_PROGRESS /
 * sec-PROXY_DONE / sec-PROXY_CANCEL for the wire shapes.
 *
 * The migration step driver is the thin wrapper that reffsd-as-PS
 * runs on a polling cadence: it sends one PROXY_PROGRESS, invokes
 * a caller-supplied callback for each work assignment in the
 * reply, and lets the caller decide whether to drive each one
 * synchronously or queue it for a worker thread.  The actual
 * byte-shoveling that fulfils a MOVE / REPAIR assignment lives
 * outside this slice (slot reserved for a future slice that wires
 * the ec_pipeline against the migration's source/target dstores).
 */

/*
 * Sent-and-decoded representation of one assignment from the
 * PROXY_PROGRESS reply.  Mirrors proxy_assignment4 with the
 * file_FH unpacked into (sb_id, ino) since that's what the PS's
 * downstream OPEN+LAYOUTGET needs anyway.
 */
struct ps_progress_assignment {
	uint32_t pa_kind; /* proxy_op_kind4: MOVE | REPAIR | CANCEL_PRIOR */
	stateid4 pa_stateid; /* proxy_stateid; PS uses in DONE/CANCEL */
	uint64_t pa_sb_id;
	uint64_t pa_ino;
	uint64_t pa_source_dstore_id;
	uint64_t pa_target_dstore_id;
};

/*
 * mds_session_send_proxy_progress -- send PROXY_PROGRESS, decode
 * the reply.
 *
 * On NFS4_OK: populates `*lease_remaining_sec` with the MDS's
 * lease-renewal hint and copies up to `max_assignments` work items
 * into `out_assignments` (no allocation; caller owns storage).
 * Returns the number of assignments actually delivered.
 *
 * Negative return: -EPERM if the MDS rejected the call (caller is
 * not registered-PS; should not happen for a PS that has already
 * registered), -EINVAL if the MDS rejected ppa_flags as reserved,
 * -EREMOTEIO on COMPOUND-level failure, other -errno on protocol
 * failure.
 */
int mds_session_send_proxy_progress(
	struct mds_session *ms, struct ps_progress_assignment *out_assignments,
	uint32_t max_assignments, uint32_t *lease_remaining_sec);

/*
 * mds_session_send_proxy_done -- terminal commit / rollback for an
 * in-flight migration the PS was assigned via PROXY_PROGRESS.
 *
 * The PS issues this in a compound `SEQUENCE PUTFH(file_FH)
 * LAYOUTRETURN(L3_stid) PROXY_DONE(pd_stateid, status)` per the
 * draft.  The wrapper here builds just the SEQUENCE+PROXY_DONE
 * pair -- the surrounding LAYOUTRETURN + the L3_stateid mgmt are
 * the PS's responsibility (a future slice that wires the
 * ec_pipeline byte-shoveling will compound them all together).
 *
 * `status == NFS4_OK` directs the MDS to commit the migration.
 * Any other value rolls back.
 *
 * Returns 0 on NFS4_OK, -EPERM on identity/auth mismatch,
 * -ESTALE on STALE_STATEID, -EBADF on BAD_STATEID, -EAGAIN on
 * OLD_STATEID, -EREMOTEIO on COMPOUND failure, other -errno.
 */
int mds_session_send_proxy_done(struct mds_session *ms,
				const stateid4 *pd_stateid, nfsstat4 pd_status);

/*
 * mds_session_send_proxy_cancel -- abandon an assigned migration
 * the PS cannot complete.  Same auth + error mapping as
 * mds_session_send_proxy_done.
 */
int mds_session_send_proxy_cancel(struct mds_session *ms,
				  const stateid4 *pc_stateid);

/*
 * Callback invoked by ps_migration_step for each assignment in
 * the PROXY_PROGRESS reply.  The PS can drive the work
 * synchronously, queue it on a worker thread, or hand it off to
 * the ec_pipeline -- this slice is silent on the choice.  The
 * callback's return value is reflected back to the caller of
 * ps_migration_step in case it needs to short-circuit.
 *
 * Note: `ms` is the same session passed to ps_migration_step,
 * convenient for the callback to issue PROXY_DONE / PROXY_CANCEL
 * once the work completes.
 */
typedef int (*ps_assignment_handler)(struct mds_session *ms,
				     const struct ps_progress_assignment *a,
				     void *ctx);

/*
 * ps_migration_step -- one PROXY_PROGRESS poll iteration.
 *
 * Sends PROXY_PROGRESS to `ms`'s upstream MDS, invokes `handler`
 * once per delivered assignment (in FIFO order), and returns the
 * number of assignments processed.  `lease_remaining_sec` is
 * populated with the MDS's lease-renewal hint regardless of how
 * many assignments were delivered (zero or more); the caller uses
 * it to size the next poll deadline (lease/2 in steady state).
 *
 * Negative return values pass through from the underlying
 * mds_session_send_proxy_progress.
 *
 * The PS's main loop is responsible for the polling cadence;
 * this function is a single step.  No internal threading; the
 * caller drives the cadence.
 */
int ps_migration_step(struct mds_session *ms, ps_assignment_handler handler,
		      void *ctx, uint32_t *lease_remaining_sec);

/* ------------------------------------------------------------------ */
/* COMPOUND builder                                                    */
/* ------------------------------------------------------------------ */

/*
 * Simple COMPOUND builder for the client side.  Allocates an argarray
 * of max_ops entries, tracks the current count, and sends via clnt_call.
 */
struct mds_compound {
	COMPOUND4args mc_args;
	COMPOUND4res mc_res;
	uint32_t mc_max_ops;
	uint32_t mc_count;
};

int mds_compound_init(struct mds_compound *mc, uint32_t max_ops,
		      const char *tag);
void mds_compound_fini(struct mds_compound *mc);

/* Returns pointer to the next nfs_argop4 slot (caller fills the union). */
nfs_argop4 *mds_compound_add_op(struct mds_compound *mc, nfs_opnum4 op);

/* Add SEQUENCE as the first op, using session state. */
int mds_compound_add_sequence(struct mds_compound *mc, struct mds_session *ms);

/* Send the COMPOUND and receive the response. */
int mds_compound_send(struct mds_compound *mc, struct mds_session *ms);

/*
 * Same as mds_compound_send, but installs a per-compound AUTH_SYS
 * credential before the RPC and restores the session's default
 * auth after.  `creds` may be NULL (equivalent to mds_compound_send).
 *
 * The auth swap runs under ms->ms_call_mutex so concurrent compounds
 * on the same session don't race on cl_auth.  If creds is non-NULL
 * but authunix_create fails, the call is rejected with -ENOMEM
 * before hitting the wire.
 *
 * Added for the proxy-server forwarders (slice 2e-iv-c) so a
 * forwarded op can carry the end client's AUTH_SYS creds rather
 * than the PS's service creds.
 */
int mds_compound_send_with_auth(struct mds_compound *mc, struct mds_session *ms,
				const struct authunix_parms *creds);

/* Access result for op at index i. */
static inline nfs_resop4 *mds_compound_result(struct mds_compound *mc,
					      uint32_t i)
{
	if (i >= mc->mc_res.resarray.resarray_len)
		return NULL;
	return &mc->mc_res.resarray.resarray_val[i];
}

/* ------------------------------------------------------------------ */
/* File operations                                                     */
/* ------------------------------------------------------------------ */

struct mds_file {
	stateid4 mf_stateid; /* open stateid */
	nfs_fh4 mf_fh; /* current filehandle */
};

int mds_file_open(struct mds_session *ms, const char *path,
		  struct mds_file *mf);
int mds_file_close(struct mds_session *ms, struct mds_file *mf);
int mds_file_getattr(struct mds_session *ms, struct mds_file *mf, char *owner,
		     size_t owner_size, char *owner_group,
		     size_t owner_group_size);
int mds_file_setattr_owner(struct mds_session *ms, struct mds_file *mf,
			   const char *owner, const char *owner_group);
int mds_file_write(struct mds_session *ms, struct mds_file *mf,
		   const uint8_t *data, uint32_t len, uint64_t offset);
int mds_file_read(struct mds_session *ms, struct mds_file *mf, uint8_t *buf,
		  uint32_t len, uint64_t offset, uint32_t *nread);
int mds_file_remove(struct mds_session *ms, const char *name);
int mds_file_clone(struct mds_session *ms, struct mds_file *src,
		   struct mds_file *dst, uint64_t src_offset,
		   uint64_t dst_offset, uint64_t count);
int mds_file_exchange_range(struct mds_session *ms, struct mds_file *src,
			    struct mds_file *dst, uint64_t src_offset,
			    uint64_t dst_offset, uint64_t count);

/* ------------------------------------------------------------------ */
/* Layout operations                                                   */
/* ------------------------------------------------------------------ */

/* Parsed mirror from a Flex Files layout. */
struct ec_mirror {
	deviceid4 em_deviceid;
	uint32_t em_efficiency;
	uint8_t em_fh[128]; /* NFSv3/NFSv4 filehandle */
	uint32_t em_fh_len;
	uint32_t em_uid;
	uint32_t em_gid;
	uint32_t em_flags; /* FFV2_DS_FLAGS_* (v2 only, 0 for v1) */
	bool em_tight_coupled; /* DS supports TRUST_STATEID */
	/*
	 * Per-mirror checksum algorithm from the layout
	 * (ffm_checksum_algorithm).  Used to dispatch CRC computation
	 * on CHUNK_WRITE and verification on CHUNK_READ; the supported-
	 * set check happens inside mds_layout_get so callers never see
	 * a layout this client can't compute against.
	 */
	uint32_t em_checksum_algorithm;
	/*
	 * em_local: set by ec_resolve_mirrors when the mirror's
	 * deviceinfo resolves to one of the PS's own bound addresses
	 * (Phase 5 short-circuit).  When true, ec_chunk_read /
	 * ec_chunk_write bypass the DS RPC and call the local DS sb's
	 * data backend directly via ps_shortcircuit_read / _write.
	 * Always false when the caller did not pass a pls (ec_demo,
	 * standalone unit tests) -- the dispatch falls through to the
	 * existing RPC path with zero behaviour change.
	 */
	bool em_local;
};

/* Parsed layout from LAYOUTGET. */
struct ec_layout {
	stateid4 el_stateid;
	layouttype4 el_layout_type; /* v1 or v2 */
	uint32_t el_stripe_unit; /* v1: ffl_stripe_unit */
	uint32_t el_chunk_size; /* v2: ffm_striping_unit_size */
	uint32_t el_coding_type; /* v2: ffv2_coding_type4 */
	uint32_t el_nmirrors;
	struct ec_mirror *el_mirrors;
};

/*
 * mds_layout_get / mds_layout_return -- LAYOUTGET / LAYOUTRETURN
 * over `ms`.  `creds` is an optional per-call AUTH_SYS override that
 * the proxy-server forwarders use to surface the end client's
 * identity to the upstream MDS (see ps_proxy_pipeline_read in
 * lib/nfs4/ps/ps_proxy_ops.c).  Pass NULL to use the session's
 * default auth -- the historical behaviour, which every non-PS
 * caller (ec_demo, ec_pipeline back-compat wrappers) still wants.
 */
int mds_layout_get(struct mds_session *ms, struct mds_file *mf,
		   layoutiomode4 iomode, layouttype4 layout_type,
		   const struct authunix_parms *creds,
		   struct ec_layout *layout);
int mds_layout_return(struct mds_session *ms, struct mds_file *mf,
		      const struct authunix_parms *creds,
		      struct ec_layout *layout);
void ec_layout_free(struct ec_layout *layout);

/*
 * Pending Change 6 step 7: validate every mirror's
 * em_checksum_algorithm against the client's supported set.
 * Returns 0 if every mirror declares an algorithm this client can
 * compute, -ENOTSUP otherwise (with *bad_mirror_out set to the
 * first offending mirror index when non-NULL).  Does NOT mutate
 * the layout.  mds_layout_get calls this immediately after
 * decoding the wire layout and, on failure, issues LAYOUTERROR +
 * LAYOUTRETURN before returning -ENOTSUP to the caller; callers
 * can also call it directly against a hand-built layout for
 * unit-testing the policy.
 */
int ec_layout_validate_checksums(const struct ec_layout *layout,
				 uint32_t *bad_mirror_out);

/*
 * mds_layout_error -- report a DS I/O error to the MDS.
 * Called when a DS operation fails so the MDS can take
 * corrective action (fence, repair, etc.).
 */
int mds_layout_error(struct mds_session *ms, struct mds_file *mf,
		     struct ec_layout *layout, uint32_t mirror_idx,
		     nfsstat4 nfs4_status, nfs_opnum4 opnum);

/* Resolved data server address from GETDEVICEINFO. */
struct ec_device {
	char ed_host[256];
	uint16_t ed_port;
	bool ed_tight_coupled; /* DS advertises ffdv_tightly_coupled */
};

int mds_getdeviceinfo(struct mds_session *ms, const deviceid4 devid,
		      layouttype4 layout_type, struct ec_device *dev);

/* ------------------------------------------------------------------ */
/* DS I/O (NFSv3)                                                      */
/* ------------------------------------------------------------------ */

struct ds_conn {
	CLIENT *dc_clnt;
};

int ds_connect(struct ds_conn *dc, const struct ec_device *dev, uint32_t uid,
	       uint32_t gid);
void ds_disconnect(struct ds_conn *dc);

int ds_write(struct ds_conn *dc, const uint8_t *fh, uint32_t fh_len,
	     uint64_t offset, const uint8_t *data, uint32_t len);
int ds_read(struct ds_conn *dc, const uint8_t *fh, uint32_t fh_len,
	    uint64_t offset, uint8_t *data, uint32_t len, uint32_t *nread);

/* ------------------------------------------------------------------ */
/* DS I/O (NFSv4.2 CHUNK ops)                                          */
/* ------------------------------------------------------------------ */

/*
 * ds_chunk_write -- CHUNK_WRITE to a data server.
 * block_offset: block number (not byte offset).
 * chunk_size: size of each chunk in bytes.
 * data/data_len: chunk data (one or more chunks of chunk_size bytes).
 * owner_id: chunk owner identifier.
 * stateid: layout stateid for tight coupling (NULL = anonymous stateid).
 * guard: when non-NULL, sets cwa_guard.cwg_check=TRUE and CAS-checks
 *   the existing block's {cg_gen_id, cg_client_id} against this guard
 *   on the server side.  Returns -EAGAIN on mismatch (NFS4ERR_DELAY on
 *   the wire), distinct from -ESTALE / -EREMOTEIO so the RMW retry
 *   path can recognise it as "the read-time version moved underneath
 *   us; redo the RMW".
 * Returns 0 on success, -ESTALE if DS returns NFS4ERR_BAD_STATEID,
 * -EAGAIN if DS returns NFS4ERR_DELAY (typically a guard mismatch),
 * or other negative errno on failure.
 */
int ds_chunk_write(struct mds_session *ds, const uint8_t *fh, uint32_t fh_len,
		   uint64_t block_offset, uint32_t chunk_size,
		   const uint8_t *data, uint32_t data_len, uint32_t owner_id,
		   const stateid4 *stateid, const chunk_guard4 *guard);

/*
 * ds_chunk_read -- CHUNK_READ from a data server.
 * block_offset: starting block number.
 * count: number of blocks to read.
 * out_data/out_len: output buffer (caller-allocated, chunk_size * count).
 * stateid: layout stateid for tight coupling (NULL = anonymous stateid).
 * out_owners: when non-NULL, caller-allocated array of `count`
 *   chunk_owner4 entries.  Populated with the server's per-block
 *   chunk_owner4 (one per returned block).  Lets the RMW path
 *   capture the version it just read so the matching CHUNK_WRITE can
 *   present a CAS guard.  Blocks not returned (server returned fewer
 *   than `count`) leave their out_owners[i] zeroed.
 * Returns 0 on success, -ESTALE if DS returns NFS4ERR_BAD_STATEID,
 * or other negative errno on failure.
 */
int ds_chunk_read(struct mds_session *ds, const uint8_t *fh, uint32_t fh_len,
		  uint64_t block_offset, uint32_t count, uint8_t *out_data,
		  uint32_t chunk_size, uint32_t *nread, const stateid4 *stateid,
		  chunk_owner4 *out_owners);

/*
 * ds_chunk_finalize -- CHUNK_FINALIZE on a data server.
 */
int ds_chunk_finalize(struct mds_session *ds, const uint8_t *fh,
		      uint32_t fh_len, uint64_t block_offset, uint32_t count,
		      uint32_t owner_id);

/*
 * ds_chunk_commit -- CHUNK_COMMIT on a data server.
 *
 * `writeverf_out` (PS Phase 4b slice 4b.4) -- optional 8-byte buffer
 * that receives the response's `ccr_writeverf` on success.  Pass
 * NULL to ignore.  The verifier captures the DS's boot-epoch token
 * and is folded into the PS composed write verifier (see
 * ps_compose_write_verf) so a DS reboot between WRITE and COMMIT
 * surfaces to the client as a verifier mismatch.  On error or NULL
 * pass-through the buffer is left untouched.
 */
int ds_chunk_commit(struct mds_session *ds, const uint8_t *fh, uint32_t fh_len,
		    uint64_t block_offset, uint32_t count, uint32_t owner_id,
		    uint8_t writeverf_out[8]);

/* ------------------------------------------------------------------ */
/* Plain I/O -- write/read through layout, no erasure coding            */
/* ------------------------------------------------------------------ */

int plain_write(struct mds_session *ms, const char *path, const uint8_t *data,
		size_t data_len, layouttype4 layout_type);
int plain_read(struct mds_session *ms, const char *path, uint8_t *buf,
	       size_t buf_len, size_t *out_len, layouttype4 layout_type);

/* ------------------------------------------------------------------ */
/* EC I/O -- high-level erasure-coded write/read                        */
/* ------------------------------------------------------------------ */

enum ec_encoding_type {
	EC_ENCODING_RS = 0, /* Reed-Solomon (default) */
	EC_ENCODING_MOJETTE_SYS = 1, /* Mojette systematic */
	EC_ENCODING_MOJETTE_NONSYS = 2, /* Mojette non-systematic */
	EC_ENCODING_STRIPE = 3, /* pure striping, no redundancy */
	EC_ENCODING_MIRROR = 4, /* N replicas via FFV2_ENCODING_MIRRORED */
};

int ec_write(struct mds_session *ms, const char *path, const uint8_t *data,
	     size_t data_len, int k, int m);
int ec_read(struct mds_session *ms, const char *path, uint8_t *buf,
	    size_t buf_len, size_t *out_len, int k, int m);

/*
 * shard_size: bytes per data shard (= grid row width for Mojette,
 * bytes per RS shard).  Must be a non-zero multiple of 8 (Mojette
 * indexes columns as uint64_t).  Pass EC_SHARD_SIZE_DEFAULT for
 * the historical 4 KiB benchmark geometry; passing 24576 enables
 * the Mojette 24 KiB shard demo (96 KiB / k=4) without changing
 * the encoding or the FINALIZE/COMMIT total_blocks math.
 */
int ec_write_encoding(struct mds_session *ms, const char *path,
		   const uint8_t *data, size_t data_len, int k, int m,
		   enum ec_encoding_type encoding_type, layouttype4 layout_type,
		   size_t shard_size);
int ec_read_encoding(struct mds_session *ms, const char *path, uint8_t *buf,
		  size_t buf_len, size_t *out_len, int k, int m,
		  enum ec_encoding_type encoding_type, layouttype4 layout_type,
		  uint64_t skip_ds_mask, size_t shard_size);

/*
 * Partial-range variants -- chunk-collision Track 1b
 * (.claude/design/chunk-collision-t1b.md).  Write or read
 * `length` bytes of the file starting at `offset`, walking only
 * the stripes the range touches.  The prefix and suffix stripes
 * are RMW-merged via ec_read_stripe_with_file +
 * ec_write_stripe_with_file; fully-dirty interior stripes go
 * straight through ec_write_stripe_with_file.
 *
 * `length == 0` is a no-op success.  `offset + length` MUST NOT
 * overflow uint64_t.  For the write side, `offset + length` MUST
 * NOT exceed the existing file's logical size on the prefix /
 * suffix stripes that need RMW -- sparse RMW is the
 * per-stripe-primitive's NOT_NOW_BROWN_COW, so the harness
 * pre-fills the file with a full-file ec_write_encoding before any
 * range writers start.  Each stripe is one LAYOUTGET /
 * FINALIZE / COMMIT / LAYOUTRETURN cycle; the demo client cost
 * model is correctness over throughput.
 */
int ec_write_encoding_range(struct mds_session *ms, const char *path,
			 const uint8_t *data, size_t length, uint64_t offset,
			 int k, int m, enum ec_encoding_type encoding_type,
			 layouttype4 layout_type, size_t shard_size);
int ec_read_encoding_range(struct mds_session *ms, const char *path, uint8_t *buf,
			size_t length, uint64_t offset, int k, int m,
			enum ec_encoding_type encoding_type, layouttype4 layout_type,
			size_t shard_size);

/*
 * FH-anchored variant of ec_read_encoding.  Skips the OPEN-by-path
 * dance (PUTROOTFH + LOOKUP* + OPEN + GETFH) and uses the caller-
 * provided mds_file directly.  The caller owns the mds_file's
 * lifecycle (the FH bytes and the open stateid).  This function
 * does NOT call mds_file_open or mds_file_close.
 *
 * Used by the proxy-server subsystem to drive EC reads through the
 * pipeline without re-opening files it already discovered (PS
 * Phase 3 -- see .claude/design/proxy-server-phase3.md).
 * ec_read_encoding() remains the caller-friendly form for ec_demo and
 * tests.
 *
 * Layout grant is acquired internally and returned via LAYOUTRETURN
 * before this function returns.  No layout state leaks across the
 * call.
 */
/*
 * `creds` -- optional per-call AUTH_SYS override for the MDS-side
 * compounds (LAYOUTGET / LAYOUTRETURN).  The proxy-server forwarders
 * pass the end client's credentials so the upstream MDS authorises
 * the layout grant against the originating identity rather than the
 * PS service identity.  Pass NULL to use the session's default auth
 * (ec_demo, dstore-MDS-to-DS, internal back-compat wrappers).
 *
 * NOT_NOW_BROWN_COW: DS-side cred forwarding (CHUNK_READ /
 * NFSv3 READ to the DS) is a separate slice -- the DS sessions
 * are pooled across requests today and don't yet have a per-call
 * auth swap.  This call's `creds` only reaches the MDS hops.
 */
int ec_read_encoding_with_file(struct mds_session *ms, struct mds_file *mf,
			    uint8_t *buf, size_t buf_len, size_t *out_len,
			    int k, int m, enum ec_encoding_type encoding_type,
			    layouttype4 layout_type, uint64_t skip_ds_mask,
			    size_t shard_size,
			    const struct authunix_parms *creds,
			    struct ps_listener_state *pls);

/*
 * FH-anchored variant of ec_write_encoding.  Mirror of
 * ec_read_encoding_with_file: caller provides a pre-opened mds_file
 * and an optional AUTH_SYS override.  This function does NOT call
 * mds_file_open or mds_file_close.
 *
 * Used by the proxy-server subsystem (PS Phase 4a) to flush
 * COMMIT-buffered WRITE bytes through the EC pipeline against an
 * already-discovered upstream file, carrying the end client's
 * credentials to the upstream MDS for LAYOUTGET / LAYOUTRETURN.
 * See .claude/design/proxy-server-phase4a.md.
 *
 * `creds` -- optional per-call AUTH_SYS override for the MDS-side
 * compounds (LAYOUTGET / LAYOUTRETURN).  Pass NULL to use the
 * session's default auth (ec_demo, internal back-compat).  The
 * same NOT_NOW_BROWN_COW for DS-side cred forwarding documented
 * on ec_read_encoding_with_file applies here -- CHUNK_WRITE /
 * FINALIZE / COMMIT to DSes still use the DS session's pooled
 * auth, not `creds`.
 */
int ec_write_encoding_with_file(struct mds_session *ms, struct mds_file *mf,
			     const uint8_t *data, size_t data_len, int k, int m,
			     enum ec_encoding_type encoding_type,
			     layouttype4 layout_type, size_t shard_size,
			     const struct authunix_parms *creds,
			     struct ps_listener_state *pls);

/*
 * Per-stripe write primitive (PS Phase 4b).  Mirror of
 * ec_write_encoding_with_file but encodes and writes exactly one
 * stripe at file-level stripe number `stripe_no`, with its own
 * LAYOUTGET / FINALIZE / COMMIT / LAYOUTRETURN cycle.
 *
 * `stripe_bytes` MUST be exactly k * shard_size bytes -- the
 * caller (ps_proxy_pipeline_commit walking the dirty-stripe
 * bitmap) only invokes this for fully-dirty stripes where every
 * data shard is present in the buffer.  Partial-stripe RMW is
 * the next slice (4b.3) and is implemented separately.
 *
 * DS-side block offsets are computed from `stripe_no` so two
 * concurrent PSes writing disjoint stripes of the same upstream
 * FH no longer clobber each other's bytes (the 4a multi-writer
 * bug).  Returns 0 on success or -errno on failure; on failure
 * the dirty bitmap entry for `stripe_no` MUST stay set so the
 * client can retry COMMIT.
 */
/*
 * `mds_verf_out` / `mds_verf_set_out` (PS Phase 4b slice 4b.4) --
 * optional out-params for the composed-write-verifier mix.  On
 * success the function captures the writeverf from the first
 * mirror's CHUNK_COMMIT response into `mds_verf_out` and sets
 * `*mds_verf_set_out = true`.  Pass NULL for both to skip the
 * capture (ec_demo and the back-compat ec_write_encoding callers do
 * not need this).  On failure or NULL pass-through neither out-
 * param is touched.
 */
/*
 * `ctx_in_out` is an opaque pointer to a `struct ec_context` (internal
 * to ec_pipeline.c).  Pass NULL for the today-default behavior --
 * each call builds its own ctx, issues LAYOUTGET + ec_resolve_mirrors,
 * does its work, then tears down (ec_disconnect_all + LAYOUTRETURN).
 * Pass a populated context to share DS sessions and the layout across
 * a batch of stripe calls -- the bulk RMW path
 * (`ec_write_encoding_range`) uses this to avoid the per-stripe DS
 * session create/destroy storm that pre-empts concurrent multi-writer
 * RMW workloads (Track 1b second-mechanism finding in
 * chunk-collision-validation.md).  Callers outside ec_pipeline.c must
 * pass NULL.
 */
int ec_write_stripe_with_file(struct mds_session *ms, struct mds_file *mf,
			      uint64_t stripe_no, const uint8_t *stripe_bytes,
			      size_t stripe_len, int k, int m,
			      enum ec_encoding_type encoding_type,
			      layouttype4 layout_type, size_t shard_size,
			      const struct authunix_parms *creds,
			      uint8_t mds_verf_out[8], bool *mds_verf_set_out,
			      struct ps_listener_state *pls, void *ctx_in_out);

/*
 * Per-stripe read primitive (PS Phase 4b slice 4b.3).  Mirror of
 * ec_write_stripe_with_file but for the RMW prefix: acquires a
 * READ layout, issues CHUNK_READ on each of the k+m mirrors for
 * exactly this stripe's blocks, decodes via the encoding's
 * `ec_decode` (k-of-(k+m) quorum), and copies the k reconstructed
 * data shards into `stripe_bytes`.  `stripe_bytes` MUST be exactly
 * `k * shard_size` bytes -- the caller (the partial-stripe RMW
 * walk in ps_proxy_pipeline_commit) then overwrites the dirty
 * shard ranges from its in-memory buffer before invoking
 * ec_write_stripe_with_file to flush the merged stripe.
 *
 * The CHUNK_READ pass tolerates up to `m` unreachable shards; if
 * more than `m` shards are missing the decode fails with -EIO and
 * the caller's COMMIT returns NFS4ERR_IO (the dirty bits stay set
 * so a client retry can flush once the affected DSes recover).
 * Sparse-file semantics (every shard reads back zero bytes) are
 * out of scope for this slice -- a partial-stripe RMW into a stripe
 * with no prior bytes on the DS returns -EIO.  The functional
 * scripts/ci_ps_phase4b_test.sh covers the success path against
 * a real MDS+DSes.
 *
 * `creds` -- optional per-call AUTH_SYS override for the MDS-side
 * compounds (LAYOUTGET / LAYOUTRETURN).  Same forwarding contract
 * as ec_write_stripe_with_file; pass NULL to use the session's
 * default auth.  The same NOT_NOW_BROWN_COW for DS-side cred
 * forwarding documented on ec_read_encoding_with_file applies.
 */
/*
 * `ctx_in_out` -- same opaque-shared-ctx contract as
 * ec_write_stripe_with_file: pass NULL for default per-call
 * setup/teardown; the bulk RMW path passes a shared ctx.
 */
int ec_read_stripe_with_file(struct mds_session *ms, struct mds_file *mf,
			     uint64_t stripe_no, uint8_t *stripe_bytes,
			     size_t stripe_len, int k, int m,
			     enum ec_encoding_type encoding_type,
			     layouttype4 layout_type, size_t shard_size,
			     const struct authunix_parms *creds,
			     struct ps_listener_state *pls, void *ctx_in_out);

/*
 * Default shard size for the back-compat ec_write / ec_read
 * wrappers (4 KiB).  Exported so callers that want the legacy
 * geometry have a named constant rather than a magic number.
 */
#define EC_SHARD_SIZE_DEFAULT (4 * 1024)

/*
 * Upper bound on shard_size accepted by ec_write_encoding / ec_read_encoding.
 * 1 MiB is well above any geometry we care about (Mojette 24 KiB at
 * P=3072 is the largest case shipped; the next plausible step is
 * 32 KiB) and bounds the per-stripe allocation to a sane size:
 * stripe_data = k * shard_size <= k * 1 MiB.  Catches caller-passed
 * garbage (CLI flag, malformed config) before we multiply our way
 * into a SIZE_MAX overflow.
 */
#define EC_SHARD_SIZE_MAX (1u * 1024u * 1024u)

#endif /* _REFFS_EC_CLIENT_H */
