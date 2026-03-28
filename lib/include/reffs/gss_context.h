/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * RPCSEC_GSS context cache — per-server table of established GSS
 * security contexts, keyed by opaque handle.
 *
 * Contexts are created during the RPCSEC_GSS_INIT exchange and
 * looked up on every subsequent DATA request.  Destroyed explicitly
 * by RPCSEC_GSS_DESTROY or reaped on client disconnect.
 */

#ifndef _REFFS_GSS_CONTEXT_H
#define _REFFS_GSS_CONTEXT_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef HAVE_GSSAPI_KRB5
#include <gssapi/gssapi.h>
#include <gssapi/gssapi_krb5.h>
#endif

#include <urcu/ref.h>
#include <urcu/rculfhash.h>

#define GSS_HANDLE_LEN 16 /* server-generated opaque handle size */
#define GSS_SEQ_WINDOW 128 /* replay window size (must match bitmap) */

struct gss_ctx_entry {
	struct cds_lfht_node gc_node;
	uint8_t gc_handle[GSS_HANDLE_LEN];
	uint32_t gc_handle_len;
#ifdef HAVE_GSSAPI_KRB5
	gss_ctx_id_t gc_gss_ctx;
	gss_name_t gc_client_name;
#endif
	uint32_t gc_seq_window;
	uint32_t gc_seq_last;
	uint64_t gc_seq_bitmap[2]; /* 128-bit sliding window for replay */
	pthread_mutex_t gc_seq_lock; /* protects seq_last + bitmap */
	uint64_t gc_last_activity_ns; /* CLOCK_MONOTONIC, renewed on DATA */
	uint32_t gc_service; /* rpc_Gss_Svc_t wire value */
	struct urcu_ref gc_ref;
	struct rcu_head gc_rcu;
};

/* Module init/fini — called from rpc layer init. */
int gss_ctx_cache_init(void);
void gss_ctx_cache_fini(void);

/* Server credential (keytab) init/fini. */
int gss_server_cred_init(void);
void gss_server_cred_fini(void);

/*
 * Returns true if the server has a valid GSS credential (keytab).
 * When false, krb5 clients should receive NFS4ERR_DELAY (not
 * WRONGSEC) — the flavor is configured but the backend is broken.
 */
bool gss_server_cred_is_available(void);

/*
 * Create a new context entry with the given handle.
 * Takes ownership of gss_ctx and client_name.
 * Returns 0 on success, -errno on failure.
 */
struct gss_ctx_entry *gss_ctx_create(const uint8_t *handle,
				     uint32_t handle_len);

/* Look up by handle.  Returns NULL or a ref'd entry. */
struct gss_ctx_entry *gss_ctx_find(const uint8_t *handle, uint32_t handle_len);

/* Release a reference obtained from gss_ctx_find(). */
void gss_ctx_put(struct gss_ctx_entry *entry);

/*
 * Check and record a sequence number against the replay window.
 * Returns 0 if the sequence number is valid and not a replay.
 * Returns -EACCES on replay or out-of-window.
 */
int gss_ctx_seq_check(struct gss_ctx_entry *entry, uint32_t seq_num);

/* Remove and destroy a context by handle. */
void gss_ctx_destroy(const uint8_t *handle, uint32_t handle_len);

#ifdef HAVE_GSSAPI_KRB5
/*
 * Accept a GSS token during context establishment (INIT/CONTINUE_INIT).
 * On success, populates output_token (caller must free with
 * gss_release_buffer).  Returns GSS major status.
 */
uint32_t gss_ctx_accept(struct gss_ctx_entry *entry, const void *input_token,
			uint32_t input_len, void **output_token,
			uint32_t *output_len, uint32_t *minor_status);

/*
 * Compute a MIC over data using the context's GSS handle.
 * Caller must free *mic with gss_release_buffer().
 */
uint32_t gss_ctx_get_mic(struct gss_ctx_entry *entry, const void *data,
			 uint32_t data_len, void **mic, uint32_t *mic_len);

/*
 * Verify a MIC over data using the context's GSS handle.
 */
uint32_t gss_ctx_verify_mic(struct gss_ctx_entry *entry, const void *data,
			    uint32_t data_len, const void *mic,
			    uint32_t mic_len);

/* Get the authenticated principal name as a string (caller frees). */
char *gss_ctx_principal(struct gss_ctx_entry *entry);

/*
 * Map a GSS context's principal to uid/gid using libnfsidmap
 * (with getpwnam fallback).
 */
int gss_ctx_map_to_unix(struct gss_ctx_entry *entry, uid_t *uid, gid_t *gid);
#endif /* HAVE_GSSAPI_KRB5 */

/*
 * Unwrap a GSS integrity-protected or privacy-protected request body.
 * Verifies the embedded sequence number matches @seq.
 * On success, returns 0 and sets *out and *out_len to the unwrapped call
 * body (malloc'd; caller must free).
 */
int gss_ctx_unwrap_request(struct gss_ctx_entry *entry, uint32_t svc,
			   uint32_t seq, const char *body, size_t body_len,
			   char **out, size_t *out_len);

/*
 * Wrap a reply body for GSS integrity or privacy protection.
 * Prepends the sequence number and applies the appropriate GSS transform.
 * On success, returns 0 and sets *out and *out_len (malloc'd; caller frees).
 */
int gss_ctx_wrap_reply(struct gss_ctx_entry *entry, uint32_t svc, uint32_t seq,
		       const char *body, size_t body_len, char **out,
		       size_t *out_len);

struct rpc_trans;

/*
 * Handle RPCSEC_GSS_INIT / CONTINUE_INIT in the RPC layer.
 * Performs GSS context establishment and sends the rpc_gss_init_res
 * reply directly (no protocol dispatch).
 */
int rpc_gss_handle_init(struct rpc_trans *rt);
int rpc_gss_handle_destroy(struct rpc_trans *rt);

#endif /* _REFFS_GSS_CONTEXT_H */
