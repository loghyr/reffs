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

#include <stdlib.h>
#include <string.h>

#include <urcu.h>
#include <urcu/rculfhash.h>
#include <urcu/ref.h>

#include <openssl/rand.h>
#include <xxhash.h>

#include "reffs/gss_context.h"
#include "reffs/log.h"

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

#endif /* HAVE_GSSAPI_KRB5 */
