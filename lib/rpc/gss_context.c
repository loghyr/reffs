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
#include <inttypes.h>
#include <pthread.h>
#include <pwd.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <urcu.h>
#include <urcu/rculfhash.h>
#include <urcu/ref.h>

#include <openssl/rand.h>
#include <xxhash.h>

#include "reffs/gss_context.h"
#include "reffs/idmap.h"
#include "reffs/identity_map.h"
#include "reffs/log.h"
#include "reffs/rpc.h"
#include "reffs/time.h"
#include "reffs/trace/security.h"

#ifdef HAVE_LIBNFSIDMAP
#include <nfsidmap.h>
#endif

static struct cds_lfht *gss_ctx_ht;
static _Atomic bool gss_cred_available;

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
	pthread_mutex_destroy(&entry->gc_seq_lock);
	free(entry);
}

static void gss_ctx_release(struct urcu_ref *ref)
{
	struct gss_ctx_entry *entry =
		caa_container_of(ref, struct gss_ctx_entry, gc_ref);

	/*
	 * Remove from hash table before scheduling the RCU free.
	 * This ensures no iterator can find the entry after the last
	 * ref is dropped.  cds_lfht_del is idempotent if already removed.
	 */
	if (gss_ctx_ht) {
		rcu_read_lock();
		cds_lfht_del(gss_ctx_ht, &entry->gc_node);
		rcu_read_unlock();
	}

	call_rcu(&entry->gc_rcu, gss_ctx_free_rcu);
}

/* ------------------------------------------------------------------ */
/* Context reaper thread                                               */
/* ------------------------------------------------------------------ */

/*
 * Default TTL: 8 hours.  Kerberos ticket lifetimes are typically 8-24h.
 * Contexts are renewed on each DATA request (gc_last_activity_ns).
 */
#define GSS_CTX_TTL_NS (8ULL * 3600 * 1000000000ULL)
#define GSS_CTX_SCAN_SEC 60

static pthread_t gss_reaper_thread;
static _Atomic uint32_t gss_reaper_running;
static pthread_mutex_t gss_reaper_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gss_reaper_cv = PTHREAD_COND_INITIALIZER;

static void *gss_reaper_thread_fn(void *arg __attribute__((unused)))
{
	rcu_register_thread();

	while (atomic_load_explicit(&gss_reaper_running,
				    memory_order_relaxed)) {
		struct timespec ts;

		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_sec += GSS_CTX_SCAN_SEC;

		pthread_mutex_lock(&gss_reaper_mtx);
		pthread_cond_timedwait(&gss_reaper_cv, &gss_reaper_mtx, &ts);
		pthread_mutex_unlock(&gss_reaper_mtx);

		if (!atomic_load_explicit(&gss_reaper_running,
					  memory_order_relaxed))
			break;

		if (!gss_ctx_ht)
			continue;

		uint64_t now = reffs_now_ns();
		struct cds_lfht_iter iter;
		struct cds_lfht_node *node;

		/*
		 * Scan under rcu_read_lock.  For each expired entry,
		 * take a ref (skip if already dying), advance the
		 * iterator past it, then drop the creation ref.
		 * gss_ctx_release handles cds_lfht_del when the
		 * refcount reaches zero.
		 */
		rcu_read_lock();
		cds_lfht_first(gss_ctx_ht, &iter);
		while ((node = cds_lfht_iter_get_node(&iter)) != NULL) {
			struct gss_ctx_entry *entry = caa_container_of(
				node, struct gss_ctx_entry, gc_node);

			cds_lfht_next(gss_ctx_ht, &iter);

			if (!urcu_ref_get_unless_zero(&entry->gc_ref))
				continue;

			uint64_t last = entry->gc_last_activity_ns;

			if (last == 0 || now <= last ||
			    now - last < GSS_CTX_TTL_NS) {
				gss_ctx_put(entry);
				continue;
			}

			TRACE("gss_reaper: expiring context "
			      "idle=%" PRIu64 "s",
			      (uint64_t)((now - last) / 1000000000ULL));

			/* Drop our ref + the creation ref. */
			gss_ctx_put(entry);
			gss_ctx_put(entry);
		}
		rcu_read_unlock();
	}

	rcu_unregister_thread();
	return NULL;
}

static int gss_ctx_reaper_init(void)
{
	atomic_store_explicit(&gss_reaper_running, 1, memory_order_relaxed);
	return pthread_create(&gss_reaper_thread, NULL, gss_reaper_thread_fn,
			      NULL);
}

static void gss_ctx_reaper_fini(void)
{
	atomic_store_explicit(&gss_reaper_running, 0, memory_order_relaxed);
	pthread_cond_signal(&gss_reaper_cv);
	pthread_join(gss_reaper_thread, NULL);
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
	return gss_ctx_reaper_init();
}

void gss_ctx_cache_fini(void)
{
	if (!gss_ctx_ht)
		return;

	gss_ctx_reaper_fini();

	/* Drain: advance iterator before put (Rule 6). */
	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;

	rcu_read_lock();
	cds_lfht_first(gss_ctx_ht, &iter);
	while ((node = cds_lfht_iter_get_node(&iter)) != NULL) {
		struct gss_ctx_entry *entry =
			caa_container_of(node, struct gss_ctx_entry, gc_node);

		cds_lfht_next(gss_ctx_ht, &iter);
		gss_ctx_put(entry);
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

	atomic_store_explicit(&gss_cred_available, true, memory_order_release);
	TRACE("RPCSEC_GSS: server credential acquired from keytab");
	return 0;
#else
	return 0;
#endif
}

bool gss_server_cred_is_available(void)
{
	return atomic_load_explicit(&gss_cred_available, memory_order_acquire);
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
	entry->gc_seq_window = GSS_SEQ_WINDOW;
	entry->gc_last_activity_ns = reffs_now_ns();
	pthread_mutex_init(&entry->gc_seq_lock, NULL);
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

	/* Drop the find ref and the creation ref.
	 * gss_ctx_release (refcount→0) handles cds_lfht_del. */
	gss_ctx_put(entry);
	gss_ctx_put(entry);
}

/*
 * Shift a 128-bit bitmap (two uint64_t) left by @n positions.
 * Bits shifted out of bm[1] are lost; bits shifted from bm[0]
 * into bm[1] are preserved.
 */
static void bitmap128_shift_left(uint64_t bm[2], uint32_t n)
{
	if (n >= 128) {
		bm[0] = 0;
		bm[1] = 0;
	} else if (n >= 64) {
		bm[1] = bm[0] << (n - 64);
		bm[0] = 0;
	} else if (n > 0) {
		bm[1] = (bm[1] << n) | (bm[0] >> (64 - n));
		bm[0] <<= n;
	}
}

static bool bitmap128_test(const uint64_t bm[2], uint32_t bit)
{
	if (bit < 64)
		return (bm[0] >> bit) & 1;
	return (bm[1] >> (bit - 64)) & 1;
}

static void bitmap128_set(uint64_t bm[2], uint32_t bit)
{
	if (bit < 64)
		bm[0] |= (1ULL << bit);
	else
		bm[1] |= (1ULL << (bit - 64));
}

int gss_ctx_seq_check(struct gss_ctx_entry *entry, uint32_t seq_num)
{
	int ret = 0;

	/* RFC 2203: sequence numbers start at 1; 0 is never valid. */
	if (seq_num == 0)
		return -EACCES;

	pthread_mutex_lock(&entry->gc_seq_lock);

	if (seq_num > entry->gc_seq_last) {
		/* Advance window: shift bitmap, mark new seq as seen. */
		uint32_t advance = seq_num - entry->gc_seq_last;

		bitmap128_shift_left(entry->gc_seq_bitmap, advance);
		bitmap128_set(entry->gc_seq_bitmap, 0);
		entry->gc_seq_last = seq_num;
	} else if (entry->gc_seq_last - seq_num >= entry->gc_seq_window) {
		/* Below window — too old. */
		ret = -EACCES;
	} else {
		/* Within window — check for replay. */
		uint32_t offset = entry->gc_seq_last - seq_num;

		if (bitmap128_test(entry->gc_seq_bitmap, offset)) {
			ret = -EACCES; /* replay */
		} else {
			bitmap128_set(entry->gc_seq_bitmap, offset);
		}
	}

	pthread_mutex_unlock(&entry->gc_seq_lock);
	return ret;
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

		trace_security_gss_map(name ? name : "(unknown)", 0, 0,
				       __func__, __LINE__);
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
	 * RFC 2203 §5.2.2.1: the reply verifier for INIT/CONTINUE_INIT
	 * is RPCSEC_GSS with the MIC of the sequence window as the body.
	 * Compute the MIC first so we know its size for the reply buffer.
	 */
	void *verf_mic = NULL;
	uint32_t verf_mic_len = 0;

	if (gss_major == GSS_S_COMPLETE) {
		uint32_t window_net = htonl(ctx->gc_seq_window);
		uint32_t mic_major;

		mic_major = gss_ctx_get_mic(ctx, &window_net,
					    sizeof(window_net), &verf_mic,
					    &verf_mic_len);
		if (mic_major != GSS_S_COMPLETE) {
			LOG("GSS INIT: failed to compute verifier MIC: %u",
			    mic_major);
			return EINVAL;
		}
	}

	uint32_t verf_padded = (verf_mic_len + 3) & ~3u;

	/*
	 * Reply layout:
	 *   record_mark + xid + msg_type + reply_stat +
	 *   verf_flavor + verf_len + verf_body(MIC) +
	 *   accept_stat +
	 *   handle_len + handle + major + minor + window +
	 *   token_len + token
	 */
	size_t reply_size = 4 * sizeof(uint32_t) + /* recmark+xid+type+stat */
			    2 * sizeof(uint32_t) + verf_padded + /* verf */
			    sizeof(uint32_t) + /* accept_stat */
			    sizeof(uint32_t) + handle_padded + /* handle */
			    3 * sizeof(uint32_t) + /* major, minor, window */
			    sizeof(uint32_t) + token_padded; /* token */

	rt->rt_reply_len = reply_size;
	rt->rt_reply = calloc(reply_size, 1);
	if (!rt->rt_reply) {
		OM_uint32 minor;

		if (verf_mic)
			gss_release_buffer(
				&minor,
				&(gss_buffer_desc){ .length = verf_mic_len,
						    .value = verf_mic });
		return ENOMEM;
	}

	uint32_t msg_len = (uint32_t)(reply_size - sizeof(uint32_t));
	uint32_t *p = (uint32_t *)rt->rt_reply;

	rt->rt_offset = 0;

	/* Standard RPC reply header. */
	p = rpc_encode_uint32_t(rt, p, msg_len | 0x80000000);
	p = rpc_encode_uint32_t(rt, p, rt->rt_info.ri_xid);
	p = rpc_encode_uint32_t(rt, p, 1); /* MSG_REPLY */
	p = rpc_encode_uint32_t(rt, p, 0); /* MSG_ACCEPTED */

	/* Verifier: RPCSEC_GSS + MIC of window. */
	p = rpc_encode_uint32_t(rt, p, RPCSEC_GSS);
	p = rpc_encode_uint32_t(rt, p, verf_mic_len);
	if (verf_mic_len > 0) {
		memcpy(p, verf_mic, verf_mic_len);
		p = (uint32_t *)((char *)p + verf_padded);
		rt->rt_offset += verf_padded;
		OM_uint32 minor;

		gss_release_buffer(&minor,
				   &(gss_buffer_desc){ .length = verf_mic_len,
						       .value = verf_mic });
	}

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

	trace_security_gss_init(rt->rt_fd, rt->rt_info.ri_xid, __func__,
				__LINE__);

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

	/*
	 * Identity mapping: check the persistent mapping table first.
	 * Parse "user@REALM", find/create the domain for REALM, hash
	 * the username, and look up the UNIX mapping.
	 */
	const char *at = strchr(principal, '@');

	if (at && at > principal) {
		char user[256];
		size_t ulen = (size_t)(at - principal);

		if (ulen >= sizeof(user))
			ulen = sizeof(user) - 1;
		memcpy(user, principal, ulen);
		user[ulen] = '\0';

		const char *realm = at + 1;
		int domain_idx =
			identity_domain_find_or_create(realm, REFFS_ID_KRB5);

		if (domain_idx > 0) {
			uint32_t local_id = XXH32(user, ulen, 0);
			reffs_id krb5_id = REFFS_ID_MAKE(
				REFFS_ID_KRB5, (uint32_t)domain_idx, local_id);
			reffs_id unix_id = identity_map_unix_for(krb5_id);

			if (!REFFS_ID_IS_NOBODY(unix_id)) {
				*uid = (uid_t)REFFS_ID_LOCAL(unix_id);
				struct passwd pwbuf, *pw = NULL;
				char buf[1024];

				if (getpwuid_r(*uid, &pwbuf, buf, sizeof(buf),
					       &pw) == 0 &&
				    pw)
					*gid = pw->pw_gid;
				else
					*gid = *uid;
				free(principal);
				return 0;
			}
		}
	}

	/*
	 * No persistent mapping found — fall back to libnfsidmap or
	 * getpwnam_r.  If successful, persist the mapping for next time.
	 */
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
	const char *fb_at = strchr(principal, '@');
	size_t len = fb_at ? (size_t)(fb_at - principal) : strlen(principal);

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

	trace_security_gss_map(principal, *uid, *gid, __func__, __LINE__);
	idmap_cache_uid(*uid, principal);

	/*
	 * Persist the mapping so it survives restart.  Parse the
	 * principal again (at may point into the fallback's local buf).
	 */
	{
		const char *p_at = strchr(principal, '@');

		if (p_at && p_at > principal) {
			size_t p_ulen = (size_t)(p_at - principal);
			int dom = identity_domain_find_or_create(p_at + 1,
								 REFFS_ID_KRB5);

			if (dom > 0) {
				uint32_t lid = XXH32(principal, p_ulen, 0);
				reffs_id krb = REFFS_ID_MAKE(
					REFFS_ID_KRB5, (uint32_t)dom, lid);
				reffs_id unix_id = REFFS_ID_MAKE(
					REFFS_ID_UNIX, 0, (uint32_t)*uid);

				identity_map_add(krb, unix_id);
			}
		}
	}

	free(principal);
	return 0;
}

/* ------------------------------------------------------------------ */
/* GSS integrity / privacy unwrap and wrap                             */
/* ------------------------------------------------------------------ */

/*
 * Parse an XDR opaque<> from @p with @remaining bytes available.
 * On success, sets *data and *data_len to the opaque contents and
 * returns a pointer past the opaque (including pad).  Returns NULL
 * on truncation.
 */
static const char *parse_xdr_opaque(const char *p, size_t remaining,
				    const char **data, uint32_t *data_len)
{
	if (remaining < 4)
		return NULL;

	uint32_t len_net;

	memcpy(&len_net, p, 4);

	uint32_t len = ntohl(len_net);

	p += 4;
	remaining -= 4;

	if (len > remaining)
		return NULL;

	*data = p;
	*data_len = len;

	uint32_t padded = (len + 3) & ~3u;

	if (padded > remaining)
		return NULL;
	return p + padded;
}

/*
 * Encode an XDR opaque<> into @buf: [len][data][pad].
 * Returns pointer past the encoded opaque.
 */
static char *encode_xdr_opaque(char *buf, const void *data, uint32_t len)
{
	uint32_t len_net = htonl(len);

	memcpy(buf, &len_net, 4);
	buf += 4;
	memcpy(buf, data, len);
	buf += len;

	/* Pad to 4-byte boundary. */
	uint32_t pad = ((len + 3) & ~3u) - len;

	if (pad > 0) {
		memset(buf, 0, pad);
		buf += pad;
	}
	return buf;
}

int gss_ctx_unwrap_request(struct gss_ctx_entry *entry, uint32_t svc,
			   uint32_t seq, const char *body, size_t body_len,
			   char **out, size_t *out_len)
{
	OM_uint32 major, minor;

	if (body_len > UINT32_MAX - 4)
		return -EINVAL;

	if (svc == RPC_GSS_SVC_INTEGRITY) {
		/*
		 * RFC 2203 §5.3.3.3 — rpc_gss_integ_data:
		 *   opaque databody_integ<>  {seq_num, call_body}
		 *   opaque checksum<>        MIC over databody_integ
		 */
		const char *integ_data;
		uint32_t integ_len;
		const char *after_integ;

		after_integ = parse_xdr_opaque(body, body_len, &integ_data,
					       &integ_len);
		if (!after_integ)
			return -EINVAL;

		const char *checksum;
		uint32_t checksum_len;
		size_t remaining = body_len - (after_integ - body);

		if (!parse_xdr_opaque(after_integ, remaining, &checksum,
				      &checksum_len))
			return -EINVAL;

		/* Verify MIC over the raw integ_data bytes. */
		gss_buffer_desc msg_buf = { .length = integ_len,
					    .value = (void *)integ_data };
		gss_buffer_desc mic_buf = { .length = checksum_len,
					    .value = (void *)checksum };

		pthread_mutex_lock(&entry->gc_seq_lock);
		major = gss_verify_mic(&minor, entry->gc_gss_ctx, &msg_buf,
				       &mic_buf, NULL);
		pthread_mutex_unlock(&entry->gc_seq_lock);

		if (major != GSS_S_COMPLETE) {
			trace_security_gss_error(
				"integ unwrap verify_mic failed", major,
				__func__, __LINE__);
			return -EACCES;
		}

		/* First 4 bytes of integ_data are seq_num (net order). */
		if (integ_len <= 4)
			return -EINVAL;

		uint32_t embedded_seq;

		memcpy(&embedded_seq, integ_data, 4);
		embedded_seq = ntohl(embedded_seq);

		if (embedded_seq != seq)
			return -EACCES;

		uint32_t call_len = integ_len - 4;
		char *result = malloc(call_len);

		if (!result)
			return -ENOMEM;
		memcpy(result, integ_data + 4, call_len);
		*out = result;
		*out_len = call_len;
		return 0;

	} else if (svc == RPC_GSS_SVC_PRIVACY) {
		/*
		 * RFC 2203 §5.3.3.4 — rpc_gss_priv_data:
		 *   opaque databody_priv<>  gss_wrap(conf=1, {seq, body})
		 */
		const char *priv_data;
		uint32_t priv_len;

		if (!parse_xdr_opaque(body, body_len, &priv_data, &priv_len))
			return -EINVAL;

		gss_buffer_desc in_buf = { .length = priv_len,
					   .value = (void *)priv_data };
		gss_buffer_desc out_buf = GSS_C_EMPTY_BUFFER;
		int conf_state = 0;

		pthread_mutex_lock(&entry->gc_seq_lock);
		major = gss_unwrap(&minor, entry->gc_gss_ctx, &in_buf, &out_buf,
				   &conf_state, NULL);
		pthread_mutex_unlock(&entry->gc_seq_lock);

		if (major != GSS_S_COMPLETE) {
			trace_security_gss_error(
				"priv unwrap gss_unwrap failed", major,
				__func__, __LINE__);
			return -EACCES;
		}

		if (!conf_state) {
			trace_security_gss_error(
				"priv unwrap no confidentiality", 0, __func__,
				__LINE__);
			gss_release_buffer(&minor, &out_buf);
			return -EACCES;
		}

		if (out_buf.length <= 4) {
			gss_release_buffer(&minor, &out_buf);
			return -EINVAL;
		}

		uint32_t embedded_seq;

		memcpy(&embedded_seq, out_buf.value, 4);
		embedded_seq = ntohl(embedded_seq);

		if (embedded_seq != seq) {
			gss_release_buffer(&minor, &out_buf);
			return -EACCES;
		}

		uint32_t call_len = (uint32_t)out_buf.length - 4;
		char *result = malloc(call_len);

		if (!result) {
			gss_release_buffer(&minor, &out_buf);
			return -ENOMEM;
		}
		memcpy(result, (char *)out_buf.value + 4, call_len);
		gss_release_buffer(&minor, &out_buf);
		*out = result;
		*out_len = call_len;
		return 0;
	}

	return -EINVAL;
}

int gss_ctx_wrap_reply(struct gss_ctx_entry *entry, uint32_t svc, uint32_t seq,
		       const char *body, size_t body_len, char **out,
		       size_t *out_len)
{
	OM_uint32 major, minor;
	uint32_t seq_net = htonl(seq);

	if (svc == RPC_GSS_SVC_INTEGRITY) {
		/*
		 * Build integ_data = {seq_num_net, reply_body}.
		 * Then compute MIC over integ_data.
		 * Output = opaque integ_data<> + opaque checksum<>.
		 */
		uint32_t integ_len = 4 + (uint32_t)body_len;
		char *integ_data = malloc(integ_len);

		if (!integ_data)
			return -ENOMEM;

		memcpy(integ_data, &seq_net, 4);
		memcpy(integ_data + 4, body, body_len);

		gss_buffer_desc msg_buf = { .length = integ_len,
					    .value = integ_data };
		gss_buffer_desc mic_buf = GSS_C_EMPTY_BUFFER;

		pthread_mutex_lock(&entry->gc_seq_lock);
		major = gss_get_mic(&minor, entry->gc_gss_ctx,
				    GSS_C_QOP_DEFAULT, &msg_buf, &mic_buf);
		pthread_mutex_unlock(&entry->gc_seq_lock);

		if (major != GSS_S_COMPLETE) {
			free(integ_data);
			return -EIO;
		}

		/* Output: opaque integ_data<> + opaque checksum<> */
		uint32_t integ_padded = (integ_len + 3) & ~3u;
		uint32_t mic_padded = ((uint32_t)mic_buf.length + 3) & ~3u;
		uint32_t total = 4 + integ_padded + 4 + mic_padded;
		char *result = malloc(total);

		if (!result) {
			free(integ_data);
			gss_release_buffer(&minor, &mic_buf);
			return -ENOMEM;
		}

		char *p = result;

		p = encode_xdr_opaque(p, integ_data, integ_len);
		p = encode_xdr_opaque(p, mic_buf.value,
				      (uint32_t)mic_buf.length);

		free(integ_data);
		gss_release_buffer(&minor, &mic_buf);

		*out = result;
		*out_len = (size_t)(p - result);
		return 0;

	} else if (svc == RPC_GSS_SVC_PRIVACY) {
		/*
		 * Build plaintext = {seq_num_net, reply_body}.
		 * gss_wrap with conf_req=1.
		 * Output = opaque wrapped<>.
		 */
		uint32_t plain_len = 4 + (uint32_t)body_len;
		char *plaintext = malloc(plain_len);

		if (!plaintext)
			return -ENOMEM;

		memcpy(plaintext, &seq_net, 4);
		memcpy(plaintext + 4, body, body_len);

		gss_buffer_desc in_buf = { .length = plain_len,
					   .value = plaintext };
		gss_buffer_desc out_buf = GSS_C_EMPTY_BUFFER;
		int conf_state = 0;

		pthread_mutex_lock(&entry->gc_seq_lock);
		major = gss_wrap(&minor, entry->gc_gss_ctx, 1,
				 GSS_C_QOP_DEFAULT, &in_buf, &conf_state,
				 &out_buf);
		pthread_mutex_unlock(&entry->gc_seq_lock);

		free(plaintext);

		if (major != GSS_S_COMPLETE)
			return -EIO;

		if (!conf_state) {
			gss_release_buffer(&minor, &out_buf);
			return -EIO;
		}

		uint32_t wrap_padded = ((uint32_t)out_buf.length + 3) & ~3u;
		uint32_t total = 4 + wrap_padded;
		char *result = malloc(total);

		if (!result) {
			gss_release_buffer(&minor, &out_buf);
			return -ENOMEM;
		}

		char *p = encode_xdr_opaque(result, out_buf.value,
					    (uint32_t)out_buf.length);
		gss_release_buffer(&minor, &out_buf);

		*out = result;
		*out_len = (size_t)(p - result);
		return 0;
	}

	return -EINVAL;
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
		trace_security_gss_init(rt->rt_fd, rt->rt_info.ri_xid, __func__,
					__LINE__);
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
	uint32_t remaining = rt->rt_body_len - rt->rt_offset;
	uint32_t *body = (uint32_t *)(rt->rt_body + rt->rt_offset);

	if (remaining < sizeof(uint32_t)) {
		gss_ctx_put(ctx);
		return EINVAL;
	}

	uint32_t token_len = ntohl(*body);

	body++;
	remaining -= sizeof(uint32_t);

	if (token_len > remaining) {
		gss_ctx_put(ctx);
		return EINVAL;
	}

	void *input_token = body;

	void *output_token = NULL;
	uint32_t output_len = 0;
	uint32_t minor_status = 0;

	uint32_t major = gss_ctx_accept(ctx, input_token, token_len,
					&output_token, &output_len,
					&minor_status);

	trace_security_gss_accept(major, minor_status, token_len, output_len,
				  __func__, __LINE__);

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

	/*
	 * On success the creation ref keeps the context alive in the
	 * hash table for future DATA requests.  Only DESTROY or the
	 * reaper drops the creation ref.  On failure, clean up now.
	 */
	if (ret)
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
		trace_security_gss_error("DESTROY context not found", 0,
					 __func__, __LINE__);
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
			trace_security_gss_error("DESTROY verifier MIC failed",
						 vmaj, __func__, __LINE__);
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

	trace_security_gss_data(rt->rt_info.ri_xid, gc->gc_seq, gc->gc_svc,
				gc->gc_handle_len, __func__, __LINE__);

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
