/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * EC demo client -- minimal NFSv4.2 client for erasure-coding demonstration.
 *
 * Talks NFSv4.2 to the MDS (EXCHANGE_ID, CREATE_SESSION, SEQUENCE,
 * OPEN, LAYOUTGET, GETDEVICEINFO, LAYOUTRETURN, CLOSE) and NFSv3 to
 * the data servers (READ, WRITE).
 */

#ifndef _REFFS_EC_CLIENT_H
#define _REFFS_EC_CLIENT_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <rpc/rpc.h>
#include <rpc/auth_unix.h>

#include "nfsv42_xdr.h"

/* ------------------------------------------------------------------ */
/* MDS session                                                         */
/* ------------------------------------------------------------------ */

struct mds_session {
	CLIENT *ms_clnt;
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

int mds_layout_get(struct mds_session *ms, struct mds_file *mf,
		   layoutiomode4 iomode, layouttype4 layout_type,
		   struct ec_layout *layout);
int mds_layout_return(struct mds_session *ms, struct mds_file *mf,
		      struct ec_layout *layout);
void ec_layout_free(struct ec_layout *layout);

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
 * Returns 0 on success, -ESTALE if DS returns NFS4ERR_BAD_STATEID,
 * or other negative errno on failure.
 */
int ds_chunk_write(struct mds_session *ds, const uint8_t *fh, uint32_t fh_len,
		   uint64_t block_offset, uint32_t chunk_size,
		   const uint8_t *data, uint32_t data_len, uint32_t owner_id,
		   const stateid4 *stateid);

/*
 * ds_chunk_read -- CHUNK_READ from a data server.
 * block_offset: starting block number.
 * count: number of blocks to read.
 * out_data/out_len: output buffer (caller-allocated, chunk_size * count).
 * stateid: layout stateid for tight coupling (NULL = anonymous stateid).
 * Returns 0 on success, -ESTALE if DS returns NFS4ERR_BAD_STATEID,
 * or other negative errno on failure.
 */
int ds_chunk_read(struct mds_session *ds, const uint8_t *fh, uint32_t fh_len,
		  uint64_t block_offset, uint32_t count, uint8_t *out_data,
		  uint32_t chunk_size, uint32_t *nread,
		  const stateid4 *stateid);

/*
 * ds_chunk_finalize -- CHUNK_FINALIZE on a data server.
 */
int ds_chunk_finalize(struct mds_session *ds, const uint8_t *fh,
		      uint32_t fh_len, uint64_t block_offset, uint32_t count,
		      uint32_t owner_id);

/*
 * ds_chunk_commit -- CHUNK_COMMIT on a data server.
 */
int ds_chunk_commit(struct mds_session *ds, const uint8_t *fh, uint32_t fh_len,
		    uint64_t block_offset, uint32_t count, uint32_t owner_id);

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

enum ec_codec_type {
	EC_CODEC_RS = 0, /* Reed-Solomon (default) */
	EC_CODEC_MOJETTE_SYS = 1, /* Mojette systematic */
	EC_CODEC_MOJETTE_NONSYS = 2, /* Mojette non-systematic */
	EC_CODEC_STRIPE = 3, /* pure striping, no redundancy */
};

int ec_write(struct mds_session *ms, const char *path, const uint8_t *data,
	     size_t data_len, int k, int m);
int ec_read(struct mds_session *ms, const char *path, uint8_t *buf,
	    size_t buf_len, size_t *out_len, int k, int m);

int ec_write_codec(struct mds_session *ms, const char *path,
		   const uint8_t *data, size_t data_len, int k, int m,
		   enum ec_codec_type codec_type, layouttype4 layout_type);
int ec_read_codec(struct mds_session *ms, const char *path, uint8_t *buf,
		  size_t buf_len, size_t *out_len, int k, int m,
		  enum ec_codec_type codec_type, layouttype4 layout_type,
		  uint64_t skip_ds_mask);

#endif /* _REFFS_EC_CLIENT_H */
