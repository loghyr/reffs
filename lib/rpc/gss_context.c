/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * RPCSEC_GSS context cache — hash table of established GSS contexts.
 *
 * Prior art: Peterson & Weldon 1972 for hash table design principles.
 * GSS-API usage follows RFC 2743 and RFC 2203 (RPCSEC_GSS).
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <errno.h>
#include <grp.h>
#include <pthread.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>

#include <urcu.h>
#include <urcu/rculfhash.h>
#include <urcu/ref.h>

#include <openssl/rand.h>
#include <xxhash.h>

#include "reffs/gss_context.h"
#include "reffs/log.h"
#include "reffs/rpc.h"

#ifdef HAVE_LIBNFSIDMAP
#include <nfsidmap.h>
#endif

static struct cds_lfht *gss_ctx_ht;

#ifdef HAVE_GSSAPI_KRB5
static gss_cred_id_t gss_server_cred = GSS_C_NO_CREDENTIAL;
#endif

/* ------------------------------------------------------------------ */
/* Hash table match function                                           */
/* ------------------------------------------------------------------ */

static int gss_ctx_match(struct cds_lfht_node *node, const void *key)
{
	struct gss_ctx_entry *entry =
		caa_container_of(node, struct gss_ctx_entry, gc_node);
	const uint8_t *handle = key;

	return memcmp(entry->gc_handle, handle, GSS_HANDLE_LEN) == 0;
}

static unsigned long gss_ctx_hash(const uint8_t *handle)
{
	return XXH3_64bits(handle, GSS_HANDLE_LEN);
}

/* ------------------------------------------------------------------ */
/* RCU-deferred free                                                   */
/* ------------------------------------------------------------------ */

static void gss_ctx_free_rcu(struct rcu_head *head)
{
	struct gss_ctx_entry *entry =
		caa_container_of(head, struct gss_ctx_entry, gc_rcu);

#ifdef HAVE_GSSAPI_KRB5
	OM_uint32 minor;

	if (entry->gc_gss_ctx != GSS_C_NO_CONTEXT)
		gss_delete_sec_context(&minor, &entry->gc_gss_ctx,
				       GSS_C_NO_BUFFER);
	if (entry->gc_client_name != GSS_C_NO_NAME)
		gss_release_name(&minor, &entry->gc_client_name);
#endif
	free(entry);
}

static void gss_ctx_release(struct urcu_ref *ref)
{
	struct gss_ctx_entry *entry =
		caa_container_of(ref, struct gss_ctx_entry, gc_ref);

	call_rcu(&entry->gc_rcu, gss_ctx_free_rcu);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int gss_ctx_cache_init(void)
{
	gss_ctx_ht = cds_lfht_new(16, 16, 0, CDS_LFHT_AUTO_RESIZE, NULL);
	if (!gss_ctx_ht) {
		LOG("gss_ctx_cache_init: cds_lfht_new failed");
		return -1;
	}
	return 0;
}

void gss_ctx_cache_fini(void)
{
	if (!gss_ctx_ht)
		return;

	/* Drain all entries. */
	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;

	rcu_read_lock();
	cds_lfht_for_each(gss_ctx_ht, &iter, node)
	{
		struct gss_ctx_entry *entry =
			caa_container_of(node, struct gss_ctx_entry, gc_node);
		cds_lfht_del(gss_ctx_ht, node);
		gss_ctx_release(&entry->gc_ref);
	}
	rcu_read_unlock();

	/* Wait for all RCU callbacks to complete. */
	synchronize_rcu();
	cds_lfht_destroy(gss_ctx_ht, NULL);
	gss_ctx_ht = NULL;
}

int gss_server_cred_init(void)
{
#ifdef HAVE_GSSAPI_KRB5
	OM_uint32 major, minor;

	major = gss_acquire_cred(&minor, GSS_C_NO_NAME, GSS_C_INDEFINITE,
				 GSS_C_NO_OID_SET, GSS_C_ACCEPT,
				 &gss_server_cred, NULL, NULL);
	if (major != GSS_S_COMPLETE) {
		LOG("gss_acquire_cred failed: major=%u minor=%u", major, minor);
		return -1;
	}

	TRACE("RPCSEC_GSS: server credential acquired from keytab");
	return 0;
#else
	return 0;
#endif
}

void gss_server_cred_fini(void)
{
#ifdef HAVE_GSSAPI_KRB5
	OM_uint32 minor;

	if (gss_server_cred != GSS_C_NO_CREDENTIAL) {
		gss_release_cred(&minor, &gss_server_cred);
		gss_server_cred = GSS_C_NO_CREDENTIAL;
	}
#endif
}

struct gss_ctx_entry *gss_ctx_create(const uint8_t *handle, uint32_t handle_len)
{
	struct gss_ctx_entry *entry = calloc(1, sizeof(*entry));

	if (!entry)
		return NULL;

	memcpy(entry->gc_handle, handle,
	       handle_len < GSS_HANDLE_LEN ? handle_len : GSS_HANDLE_LEN);
	entry->gc_handle_len = handle_len;
#ifdef HAVE_GSSAPI_KRB5
	entry->gc_gss_ctx = GSS_C_NO_CONTEXT;
	entry->gc_client_name = GSS_C_NO_NAME;
#endif
	entry->gc_seq_window = 128;
	urcu_ref_init(&entry->gc_ref);

	unsigned long hash = gss_ctx_hash(entry->gc_handle);

	rcu_read_lock();
	cds_lfht_add(gss_ctx_ht, hash, &entry->gc_node);
	rcu_read_unlock();

	return entry;
}

struct gss_ctx_entry *gss_ctx_find(const uint8_t *handle,
				   uint32_t handle_len __attribute__((unused)))
{
	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;
	struct gss_ctx_entry *entry = NULL;
	unsigned long hash = gss_ctx_hash(handle);

	if (!gss_ctx_ht)
		return NULL;

	rcu_read_lock();
	cds_lfht_lookup(gss_ctx_ht, hash, gss_ctx_match, handle, &iter);
	node = cds_lfht_iter_get_node(&iter);
	if (node) {
		struct gss_ctx_entry *tmp =
			caa_container_of(node, struct gss_ctx_entry, gc_node);
		if (urcu_ref_get_unless_zero(&tmp->gc_ref))
			entry = tmp;
	}
	rcu_read_unlock();

	return entry;
}

void gss_ctx_put(struct gss_ctx_entry *entry)
{
	if (entry)
		urcu_ref_put(&entry->gc_ref, gss_ctx_release);
}

void gss_ctx_destroy(const uint8_t *handle, uint32_t handle_len)
{
	struct gss_ctx_entry *entry = gss_ctx_find(handle, handle_len);

	if (!entry)
		return;

	rcu_read_lock();
	cds_lfht_del(gss_ctx_ht, &entry->gc_node);
	rcu_read_unlock();

	/* Drop the find ref and the creation ref. */
	gss_ctx_put(entry);
	gss_ctx_put(entry);
}

#ifdef HAVE_GSSAPI_KRB5

uint32_t gss_ctx_accept(struct gss_ctx_entry *entry, const void *input_token,
			uint32_t input_len, void **output_token,
			uint32_t *output_len, uint32_t *minor_status)
{
	gss_buffer_desc in_buf = { .length = input_len,
				   .value = (void *)input_token };
	gss_buffer_desc out_buf = GSS_C_EMPTY_BUFFER;
	OM_uint32 major, minor, ret_flags;

	major = gss_accept_sec_context(&minor, &entry->gc_gss_ctx,
				       gss_server_cred, &in_buf,
				       GSS_C_NO_CHANNEL_BINDINGS,
				       &entry->gc_client_name, NULL, &out_buf,
				       &ret_flags, NULL, NULL);

	*minor_status = minor;
	*output_token = out_buf.value;
	*output_len = (uint32_t)out_buf.length;

	if (major == GSS_S_COMPLETE) {
		char *name = gss_ctx_principal(entry);

		TRACE("GSS context established for %s",
		      name ? name : "(unknown)");
		free(name);
	}

	return major;
}

uint32_t gss_ctx_get_mic(struct gss_ctx_entry *entry, const void *data,
			 uint32_t data_len, void **mic, uint32_t *mic_len)
{
	gss_buffer_desc msg = { .length = data_len, .value = (void *)data };
	gss_buffer_desc token = GSS_C_EMPTY_BUFFER;
	OM_uint32 major, minor;

	major = gss_get_mic(&minor, entry->gc_gss_ctx, GSS_C_QOP_DEFAULT, &msg,
			    &token);

	*mic = token.value;
	*mic_len = (uint32_t)token.length;
	return major;
}

uint32_t gss_ctx_verify_mic(struct gss_ctx_entry *entry, const void *data,
			    uint32_t data_len, const void *mic,
			    uint32_t mic_len)
{
	gss_buffer_desc msg = { .length = data_len, .value = (void *)data };
	gss_buffer_desc token = { .length = mic_len, .value = (void *)mic };
	OM_uint32 major, minor;

	major = gss_verify_mic(&minor, entry->gc_gss_ctx, &msg, &token, NULL);
	return major;
}

char *gss_ctx_principal(struct gss_ctx_entry *entry)
{
	if (!entry || entry->gc_client_name == GSS_C_NO_NAME)
		return NULL;

	gss_buffer_desc name_buf = GSS_C_EMPTY_BUFFER;
	OM_uint32 major, minor;

	major = gss_display_name(&minor, entry->gc_client_name, &name_buf,
				 NULL);
	if (major != GSS_S_COMPLETE)
		return NULL;

	char *name = strndup(name_buf.value, name_buf.length);

	gss_release_buffer(&minor, &name_buf);
	return name;
}

/*
 * Build and send an rpc_gss_init_res reply.
 *
 * Wire format (after standard RPC reply header):
 *   handle<>        — opaque context handle
 *   gss_major       — GSS major status
 *   gss_minor       — GSS minor status
 *   seq_window      — sequence window size
 *   gss_token<>     — output token for client
 */
static int send_gss_init_reply(struct rpc_trans *rt, struct gss_ctx_entry *ctx,
			       uint32_t gss_major, uint32_t gss_minor,
			       const void *out_token, uint32_t out_token_len)
{
	uint32_t handle_padded = (ctx->gc_handle_len + 3) & ~3u;
	uint32_t token_padded = (out_token_len + 3) & ~3u;

	/*
	 * Reply layout:
	 *   record_mark + xid + msg_type + reply_stat +
	 *   verf_flavor + verf_len + accept_stat +
	 *   handle_len + handle + major + minor + window +
	 *   token_len + token
	 */
	size_t reply_size = 7 * sizeof(uint32_t) + /* standard header */
			    sizeof(uint32_t) + handle_padded + /* handle */
			    3 * sizeof(uint32_t) + /* major, minor, window */
			    sizeof(uint32_t) + token_padded; /* token */

	rt->rt_reply_len = reply_size;
	rt->rt_reply = calloc(reply_size, 1);
	if (!rt->rt_reply)
		return ENOMEM;

	uint32_t msg_len = (uint32_t)(reply_size - sizeof(uint32_t));
	uint32_t *p = (uint32_t *)rt->rt_reply;

	rt->rt_offset = 0;

	/* Standard RPC reply header. */
	p = rpc_encode_uint32_t(rt, p, msg_len | 0x80000000);
	p = rpc_encode_uint32_t(rt, p, rt->rt_info.ri_xid);
	p = rpc_encode_uint32_t(rt, p, 1); /* MSG_REPLY */
	p = rpc_encode_uint32_t(rt, p, 0); /* MSG_ACCEPTED */
	p = rpc_encode_uint32_t(rt, p, 0); /* AUTH_NONE verifier */
	p = rpc_encode_uint32_t(rt, p, 0); /* verifier len = 0 */
	p = rpc_encode_uint32_t(rt, p, 0); /* accept_stat = SUCCESS */

	if (!p)
		goto err;

	/* rpc_gss_init_res body. */
	p = rpc_encode_uint32_t(rt, p, ctx->gc_handle_len);
	if (!p)
		goto err;
	if (ctx->gc_handle_len > 0) {
		memcpy(p, ctx->gc_handle, ctx->gc_handle_len);
		p = (uint32_t *)((char *)p + handle_padded);
		rt->rt_offset += handle_padded;
	}

	p = rpc_encode_uint32_t(rt, p, gss_major);
	p = rpc_encode_uint32_t(rt, p, gss_minor);
	p = rpc_encode_uint32_t(rt, p, ctx->gc_seq_window);
	if (!p)
		goto err;

	p = rpc_encode_uint32_t(rt, p, out_token_len);
	if (!p)
		goto err;
	if (out_token_len > 0) {
		memcpy(p, out_token, out_token_len);
		rt->rt_offset += token_padded;
	}

	rt->rt_offset = rt->rt_reply_len;

	TRACE("GSS INIT reply: xid=0x%08x major=%u handle_len=%u token_len=%u",
	      rt->rt_info.ri_xid, gss_major, ctx->gc_handle_len, out_token_len);

	if (rt->rt_rc && rt->rt_cb)
		rt->rt_cb(rt);

	return 0;

err:
	free(rt->rt_reply);
	rt->rt_reply = NULL;
	return EINVAL;
}

/*
 * Map a GSS principal to uid/gid using libnfsidmap (preferred)
 * or getpwnam fallback.  Called from the RPC layer for DATA
 * requests to populate rc_unix before compound dispatch.
 */
#ifdef HAVE_LIBNFSIDMAP
static pthread_once_t idmap_once = PTHREAD_ONCE_INIT;

static void idmap_init_once(void)
{
	nfs4_init_name_mapping(NULL); /* uses /etc/idmapd.conf */
}
#endif

int gss_ctx_map_to_unix(struct gss_ctx_entry *entry, uid_t *uid, gid_t *gid)
{
	char *principal = gss_ctx_principal(entry);

	if (!principal) {
		*uid = 65534;
		*gid = 65534;
		return -EPERM;
	}

#ifdef HAVE_LIBNFSIDMAP
	pthread_once(&idmap_once, idmap_init_once);

	if (nfs4_owner_to_uid(principal, uid) < 0) {
		TRACE("nfs4_owner_to_uid(%s) failed", principal);
		*uid = 65534;
		*gid = 65534;
		free(principal);
		return -EPERM;
	}

	if (nfs4_group_owner_to_gid(principal, gid) < 0) {
		struct passwd pwbuf, *pw = NULL;
		char buf[1024];

		if (getpwuid_r(*uid, &pwbuf, buf, sizeof(buf), &pw) == 0 && pw)
			*gid = pw->pw_gid;
		else
			*gid = 65534;
	}
#else
	/* Fallback: strip @REALM, use getpwnam_r(). */
	char local[256];
	const char *at = strchr(principal, '@');
	size_t len = at ? (size_t)(at - principal) : strlen(principal);

	if (len >= sizeof(local))
		len = sizeof(local) - 1;
	memcpy(local, principal, len);
	local[len] = '\0';

	struct passwd pwbuf, *pw = NULL;
	char buf[1024];

	if (getpwnam_r(local, &pwbuf, buf, sizeof(buf), &pw) != 0 || !pw) {
		TRACE("getpwnam_r(%s) failed for principal %s", local,
		      principal);
		*uid = 65534;
		*gid = 65534;
		free(principal);
		return -EPERM;
	}

	*uid = pw->pw_uid;
	*gid = pw->pw_gid;
#endif

	TRACE("GSS principal %s mapped to uid=%u gid=%u", principal, *uid,
	      *gid);
	free(principal);
	return 0;
}

#endif /* HAVE_GSSAPI_KRB5 */

/*
 * Handle RPCSEC_GSS_INIT / CONTINUE_INIT.
 *
 * The RPC call body (after the header) is the gss_token from the
 * client.  We pass it to gss_accept_sec_context and return the
 * rpc_gss_init_res with the output token.
 */
int rpc_gss_handle_init(struct rpc_trans *rt
#ifndef HAVE_GSSAPI_KRB5
			__attribute__((unused))
#endif
)
{
#ifdef HAVE_GSSAPI_KRB5
	struct rpc_gss_cred *gc = &rt->rt_info.ri_cred.rc_gss;
	struct gss_ctx_entry *ctx;

	if (gc->gc_proc == RPCSEC_GSS_INIT) {
		/* New context: generate a random handle. */
		uint8_t handle[GSS_HANDLE_LEN];

		RAND_bytes(handle, GSS_HANDLE_LEN);
		ctx = gss_ctx_create(handle, GSS_HANDLE_LEN);
		if (!ctx) {
			LOG("GSS INIT: failed to create context entry");
			return ENOMEM;
		}
		ctx->gc_service = gc->gc_svc;
	} else {
		/* CONTINUE_INIT: look up existing partial context. */
		ctx = gss_ctx_find(gc->gc_handle, gc->gc_handle_len);
		if (!ctx) {
			LOG("GSS CONTINUE_INIT: context not found");
			return EINVAL;
		}
	}

	/*
	 * The remaining call body after the RPC header is the
	 * rpc_gss_init_arg { opaque gss_token<> }.
	 * Read the token length and pointer from the raw buffer.
	 */
	uint32_t *body = (uint32_t *)(rt->rt_body + rt->rt_offset);
	uint32_t token_len = ntohl(*body);

	body++;
	void *input_token = body;

	void *output_token = NULL;
	uint32_t output_len = 0;
	uint32_t minor_status = 0;

	uint32_t major = gss_ctx_accept(ctx, input_token, token_len,
					&output_token, &output_len,
					&minor_status);

	int ret;

	if (major == GSS_S_COMPLETE || major == GSS_S_CONTINUE_NEEDED)
		ret = send_gss_init_reply(rt, ctx, major, minor_status,
					  output_token, output_len);
	else
		ret = EINVAL;

	/* Free the output token from gss_accept_sec_context. */
	if (output_token) {
		OM_uint32 minor;
		gss_buffer_desc buf = { .length = output_len,
					.value = output_token };

		gss_release_buffer(&minor, &buf);
	}

	gss_ctx_put(ctx);
	return ret;
#else
	return ENOTSUP;
#endif
}

/*
 * Handle RPCSEC_GSS_DESTROY — verify credentials, compute reply
 * verifier, tear down the context, and send success reply.
 *
 * The reply verifier must be computed BEFORE destroying the context
 * since gss_get_mic needs the live GSS context.
 */
int rpc_gss_handle_destroy(struct rpc_trans *rt
#ifndef HAVE_GSSAPI_KRB5
			   __attribute__((unused))
#endif
)
{
#ifdef HAVE_GSSAPI_KRB5
	struct rpc_gss_cred *gc = &rt->rt_info.ri_cred.rc_gss;

	/* Look up context — reject if not found. */
	struct gss_ctx_entry *gctx =
		gss_ctx_find(gc->gc_handle, gc->gc_handle_len);
	if (!gctx) {
		TRACE("GSS DESTROY: context not found");
		return EINVAL;
	}

	/*
	 * Verify the client's MIC over the sequence number
	 * (same check as DATA requests per RFC 2203).
	 */
	if (rt->rt_info.ri_verifier_body && rt->rt_info.ri_verifier_len > 0) {
		uint32_t seq_net = htonl(gc->gc_seq);
		uint32_t vmaj;

		vmaj = gss_ctx_verify_mic(gctx, &seq_net, sizeof(seq_net),
					  rt->rt_info.ri_verifier_body,
					  rt->rt_info.ri_verifier_len);
		if (vmaj != GSS_S_COMPLETE) {
			TRACE("GSS DESTROY: verifier MIC failed major=%u",
			      vmaj);
			gss_ctx_put(gctx);
			return EINVAL;
		}
	}

	/*
	 * Compute reply verifier (MIC over seq_window) BEFORE
	 * destroying the context.
	 */
	uint32_t seq_net = htonl(gc->gc_seq);
	void *mic = NULL;
	uint32_t mic_len = 0;

	gss_ctx_get_mic(gctx, &seq_net, sizeof(seq_net), &mic, &mic_len);

	/* Now destroy the context. */
	gss_ctx_put(gctx);
	gss_ctx_destroy(gc->gc_handle, gc->gc_handle_len);

	TRACE("GSS DESTROY: context removed xid=0x%08x", rt->rt_info.ri_xid);

	/* Build reply with GSS verifier (or AUTH_NONE if MIC failed). */
	uint32_t mic_padded = mic ? ((mic_len + 3) & ~3u) : 0;
	uint32_t verf_extra = mic ? mic_padded : 0;

	rt->rt_reply_len = 7 * sizeof(uint32_t) + verf_extra;
	rt->rt_reply = calloc(rt->rt_reply_len, 1);
	if (!rt->rt_reply) {
		if (mic) {
			OM_uint32 minor;
			gss_buffer_desc buf = { .length = mic_len,
						.value = mic };

			gss_release_buffer(&minor, &buf);
		}
		return ENOMEM;
	}

	uint32_t msg_len = (uint32_t)(rt->rt_reply_len - sizeof(uint32_t));
	uint32_t *p = (uint32_t *)rt->rt_reply;

	rt->rt_offset = 0;
	p = rpc_encode_uint32_t(rt, p, msg_len | 0x80000000);
	p = rpc_encode_uint32_t(rt, p, rt->rt_info.ri_xid);
	p = rpc_encode_uint32_t(rt, p, 1); /* MSG_REPLY */
	p = rpc_encode_uint32_t(rt, p, 0); /* MSG_ACCEPTED */

	if (mic && p) {
		p = rpc_encode_uint32_t(rt, p, RPCSEC_GSS);
		p = rpc_encode_uint32_t(rt, p, mic_len);
		if (p) {
			memcpy(p, mic, mic_len);
			p = (uint32_t *)((char *)p + mic_padded);
			rt->rt_offset += mic_padded;
		}
	} else if (p) {
		p = rpc_encode_uint32_t(rt, p, 0); /* AUTH_NONE */
		p = rpc_encode_uint32_t(rt, p, 0);
	}

	if (p)
		p = rpc_encode_uint32_t(rt, p, 0); /* accept_stat = SUCCESS */

	if (mic) {
		OM_uint32 minor;
		gss_buffer_desc buf = { .length = mic_len, .value = mic };

		gss_release_buffer(&minor, &buf);
	}

	if (!p) {
		free(rt->rt_reply);
		rt->rt_reply = NULL;
		return EINVAL;
	}

	rt->rt_offset = rt->rt_reply_len;

	if (rt->rt_rc && rt->rt_cb)
		rt->rt_cb(rt);

	return 0;
#else
	return ENOTSUP;
#endif
}
