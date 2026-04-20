/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <assert.h>
#include <errno.h>
#ifdef HAVE_HDR_HISTOGRAM
#include <hdr/hdr_histogram.h>
#endif
#include <rpc/auth.h>
#include <rpc/auth_unix.h>
#include <rpc/clnt.h>
#include <rpc/rpc_msg.h>
#include <rpc/xdr.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

#include "reffs/darwin_rpc_compat.h" /* xdr_sizeof shim on __APPLE__ */
#include "reffs/log.h"
#include "reffs/context.h"
#include "reffs/io.h"
#include "reffs/network.h"
#include "reffs/rcu.h"
#include "reffs/rpc.h"
#include "reffs/gss_context.h"
#include "reffs/task.h"
#include "reffs/time.h"
#include "reffs/tls.h"
#include "reffs/trace/rpc.h"
#include "reffs/trace/security.h"

struct rcu_head;

CDS_LIST_HEAD(rpc_program_handler_list);

/*
 * xdrproc_t is bool_t (*)(XDR *, ...) -- variadic -- for historical reasons.
 * Actual XDR functions are non-variadic (e.g. bool_t xdr_FOO(XDR *, FOO *)).
 * Calling them through xdrproc_t is ABI-safe but a strict type mismatch that
 * UBSan's -fsanitize=function flags.  Isolate all direct calls here so the
 * suppression is narrow.
 */
__attribute__((no_sanitize("function"))) static bool_t
rpc_call_xdr(xdrproc_t fn, XDR *xdrs, void *obj)
{
#ifdef __APPLE__
	/*
	 * Darwin's xdrproc_t is int (*)(XDR *, void *, unsigned int) --
	 * the third arg is recursion depth.  Linux (libtirpc) and
	 * FreeBSD (base libc) use the historical 2-arg form.  reffs's
	 * generated XDR funcs ignore the depth arg on Darwin, so
	 * passing 0 is safe.
	 */
	return fn(xdrs, obj, 0);
#else
	return fn(xdrs, obj);
#endif
}

/*
 * Darwin's xdr_sizeof shim lives in reffs/darwin_rpc_compat.h and
 * is force-included on Darwin via configure.ac's CPPFLAGS, so no
 * explicit include is needed here.  Linux/FreeBSD get the native
 * libtirpc/base-libc xdr_sizeof.
 */

static bool __rpc_log_packets = false;

void rpc_enable_packet_logging(void)
{
	__rpc_log_packets = true;
}

void rpc_disable_packet_logging(void)
{
	__rpc_log_packets = false;
}

static void rpc_program_handler_free_rcu(struct rcu_head *rcu)
{
	struct rpc_program_handler *rph =
		caa_container_of(rcu, struct rpc_program_handler, rph_rcu);

#ifdef HAVE_HDR_HISTOGRAM
	for (size_t i = 0; i < rph->rph_ops_len; i++) {
		hdr_close(rph->rph_ops[i].roh_stats.rs_histogram);
		rph->rph_ops[i].roh_stats.rs_histogram = NULL;
	}
#endif

	free(rph);
}

static void rpc_program_handler_release(struct urcu_ref *ref)
{
	struct rpc_program_handler *rph =
		caa_container_of(ref, struct rpc_program_handler, rph_ref);

	uint32_t flags = __atomic_fetch_and(&rph->rph_flags, ~RPH_IN_LIST,
					    __ATOMIC_ACQUIRE);
	if (flags & RPH_IN_LIST)
		cds_list_del_init(&rph->rph_list);

	call_rcu(&rph->rph_rcu, rpc_program_handler_free_rcu);
}

struct rpc_program_handler *
rpc_program_handler_alloc(uint32_t program, uint32_t version,
			  struct rpc_operations_handler *roh, size_t roh_len)
{
	struct rpc_program_handler *rph;

	rph = rpc_program_handler_find(program, version);
	if (rph) {
		rpc_program_handler_put(rph);
		return NULL;
	}

	rph = calloc(1, sizeof(*rph));
	if (!rph) {
		LOG("Could not alloc a rph");
		return NULL;
	}

	rph->rph_program = program;
	rph->rph_version = version;
	rph->rph_ops = roh;
	rph->rph_ops_len = roh_len;

	urcu_ref_init(&rph->rph_ref);
	__atomic_fetch_or(&rph->rph_flags, RPH_IN_LIST, __ATOMIC_RELEASE);
	cds_list_add_rcu(&rph->rph_list, &rpc_program_handler_list);

#ifdef HAVE_HDR_HISTOGRAM
	for (size_t i = 0; i < roh_len; i++) {
		hdr_init(1, INT64_C(3600000000000), 3,
			 &roh[i].roh_stats.rs_histogram);
	}
#endif

	return rph;
}

struct rpc_program_handler *rpc_program_handler_find(uint32_t program,
						     uint32_t version)
{
	struct rpc_program_handler *rph = NULL;
	struct rpc_program_handler *tmp;

	rcu_read_lock();
	cds_list_for_each_entry_rcu(tmp, &rpc_program_handler_list, rph_list)
		if (program == tmp->rph_program &&
		    version == tmp->rph_version) {
			rph = rpc_program_handler_get(tmp);
			break;
		}
	rcu_read_unlock();

	return rph;
}

struct rpc_program_handler *
rpc_program_handler_get(struct rpc_program_handler *rph)
{
	if (!rph)
		return NULL;

	if (!urcu_ref_get_unless_zero(&rph->rph_ref))
		return NULL;

	return rph;
}

void rpc_program_handler_put(struct rpc_program_handler *rph)
{
	if (!rph)
		return;

	urcu_ref_put(&rph->rph_ref, rpc_program_handler_release);
}

static void update_max_duration_rcu(uint64_t duration_ns,
				    struct rpc_operations_handler *roh)
{
	uint64_t old_max;

	__atomic_load(&roh->roh_stats.rs_duration_max, &old_max,
		      __ATOMIC_RELAXED);

	if (duration_ns > old_max) {
		// Keep trying until either we succeed or someone else updates to larger value
		while (1) {
			uint64_t expected = old_max;
			uint64_t desired = duration_ns;
			bool success = __atomic_compare_exchange(
				&roh->roh_stats.rs_duration_max, &expected,
				&desired, 0, __ATOMIC_RELAXED,
				__ATOMIC_RELAXED);

			if (success)
				break;

			old_max = expected;

			if (duration_ns <= old_max)
				break;
		}
	}
}

static void rpc_record_operation_stats(struct rpc_operations_handler *roh,
				       int64_t duration_ns, int ret)
{
	update_max_duration_rcu(duration_ns, roh);

	__atomic_add_fetch(&roh->roh_stats.rs_calls, 1, __ATOMIC_RELAXED);

	__atomic_add_fetch(&roh->roh_stats.rs_duration_total, duration_ns,
			   __ATOMIC_RELAXED);
	if (ret)
		__atomic_fetch_add(&roh->roh_stats.rs_fails, 1,
				   __ATOMIC_RELAXED);

#ifdef HAVE_HDR_HISTOGRAM
	hdr_record_value(roh->roh_stats.rs_histogram, duration_ns);
#else
	(void)duration_ns;
#endif
}

int rpc_parse_call_data(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;

	XDR xdrs = { 0 };

	size_t len;

	uint32_t *p = (uint32_t *)(rt->rt_body + rt->rt_offset);

	uint32_t start_pos, end_pos;

	if (!ph->ph_op_handler || !ph->ph_op_handler->roh_args_f)
		return 0;

	xdrmem_create(&xdrs, (char *)p, rt->rt_body_len - rt->rt_offset,
		      XDR_DECODE);

	start_pos = xdr_getpos(&xdrs);

	if (!rpc_call_xdr(ph->ph_op_handler->roh_args_f, &xdrs, ph->ph_args)) {
		xdr_destroy(&xdrs);
		return EINVAL;
	}

	end_pos = xdr_getpos(&xdrs);

	len = end_pos - start_pos;

	xdr_destroy(&xdrs);

	rt->rt_offset += len;

	return 0;
}

/*
 * Compute the GSS reply verifier for RPCSEC_GSS DATA requests.
 * RFC 2203 S5.3.3.3: the verifier is a MIC over the sequence
 * number (4 bytes, network order).
 *
 * On success, sets rt->rt_info.ri_verifier_body/len and returns
 * the padded size to add to the reply buffer.  On failure or
 * non-GSS requests, returns 0 (use AUTH_NONE verifier).
 */
static uint32_t rpc_compute_gss_reply_verifier(struct rpc_trans *rt
#ifndef HAVE_GSSAPI_KRB5
					       __attribute__((unused))
#endif
)
{
#ifdef HAVE_GSSAPI_KRB5
	if (rt->rt_info.ri_cred.rc_flavor != RPCSEC_GSS ||
	    rt->rt_info.ri_cred.rc_gss.gc_proc != RPCSEC_GSS_DATA)
		return 0;

	struct gss_ctx_entry *gctx =
		gss_ctx_find(rt->rt_info.ri_cred.rc_gss.gc_handle,
			     rt->rt_info.ri_cred.rc_gss.gc_handle_len);
	if (!gctx)
		return 0;

	/* MIC is over the sequence number in network byte order. */
	uint32_t seq_net = htonl(rt->rt_info.ri_cred.rc_gss.gc_seq);
	void *mic = NULL;
	uint32_t mic_len = 0;
	uint32_t major;

	major = gss_ctx_get_mic(gctx, &seq_net, sizeof(seq_net), &mic,
				&mic_len);
	gss_ctx_put(gctx);

	if (major != GSS_S_COMPLETE || !mic)
		return 0;

	/* Free inbound verifier before overwriting with reply MIC. */
	free(rt->rt_info.ri_verifier_body);
	rt->rt_info.ri_verifier_body = malloc(mic_len);
	if (!rt->rt_info.ri_verifier_body) {
		OM_uint32 minor;
		gss_buffer_desc buf = { .length = mic_len, .value = mic };

		gss_release_buffer(&minor, &buf);
		return 0;
	}
	memcpy(rt->rt_info.ri_verifier_body, mic, mic_len);
	rt->rt_info.ri_verifier_len = mic_len;

	{
		OM_uint32 minor;
		gss_buffer_desc buf = { .length = mic_len, .value = mic };

		gss_release_buffer(&minor, &buf);
	}

	return (mic_len + 3) & ~3u;
#else
	return 0;
#endif
}

/*
 * Encode the reply verifier (AUTH_NONE or RPCSEC_GSS MIC).
 * Returns the next write pointer, or NULL on failure.
 * Caller must have allocated space for the verifier in the reply buffer.
 */
static uint32_t *rpc_encode_reply_verifier(struct rpc_trans *rt, uint32_t *p)
{
	if (rt->rt_info.ri_verifier_body && rt->rt_info.ri_verifier_len > 0) {
		/* RPCSEC_GSS verifier. */
		p = rpc_encode_uint32_t(rt, p, RPCSEC_GSS);
		if (!p)
			return NULL;
		p = rpc_encode_uint32_t(rt, p, rt->rt_info.ri_verifier_len);
		if (!p)
			return NULL;
		memcpy(p, rt->rt_info.ri_verifier_body,
		       rt->rt_info.ri_verifier_len);
		uint32_t padded = (rt->rt_info.ri_verifier_len + 3) & ~3u;

		p = (uint32_t *)((char *)p + padded);
		rt->rt_offset += padded;
		return p;
	}

	/* AUTH_NONE verifier (default). */
	p = rpc_encode_uint32_t(rt, p, 0);
	if (!p)
		return NULL;
	p = rpc_encode_uint32_t(rt, p, 0);
	return p;
}

/* ------------------------------------------------------------------ */
/* Reply construction helpers                                          */
/* ------------------------------------------------------------------ */

/*
 * MSG_ACCEPTED header: 7 words base + verifier extra.
 *   record_mark | xid | MSG_REPLY(1) | MSG_ACCEPTED(0) |
 *   verf_flavor | verf_len | [verf_body] | accept_stat
 */
#define RPC_ACCEPTED_HDR_WORDS 7

/*
 * MSG_DENIED header: 4 words base (no verifier).
 *   record_mark | xid | MSG_REPLY(1) | MSG_DENIED(1)
 */
#define RPC_DENIED_HDR_WORDS 4

/*
 * After the reply body has been XDR-encoded, wrap it for GSS
 * integrity or privacy.  Replaces rt->rt_reply in-place with a
 * new buffer containing header + wrapped body.  @hdr_len is the
 * number of bytes in the reply before the body starts.
 */
#ifdef HAVE_GSSAPI_KRB5
static int rpc_gss_wrap_reply(struct rpc_trans *rt, size_t hdr_len)
{
	if (rt->rt_info.ri_cred.rc_flavor != RPCSEC_GSS ||
	    rt->rt_info.ri_cred.rc_gss.gc_proc != RPCSEC_GSS_DATA ||
	    rt->rt_info.ri_cred.rc_gss.gc_svc == RPC_GSS_SVC_NONE)
		return 0;

	struct gss_ctx_entry *gctx =
		gss_ctx_find(rt->rt_info.ri_cred.rc_gss.gc_handle,
			     rt->rt_info.ri_cred.rc_gss.gc_handle_len);
	if (!gctx)
		return -EIO;

	char *plain_body = rt->rt_reply + hdr_len;
	size_t plain_len = rt->rt_reply_len - hdr_len;
	char *wrapped = NULL;
	size_t wrapped_len = 0;

	int ret = gss_ctx_wrap_reply(gctx, rt->rt_info.ri_cred.rc_gss.gc_svc,
				     rt->rt_info.ri_cred.rc_gss.gc_seq,
				     plain_body, plain_len, &wrapped,
				     &wrapped_len);
	gss_ctx_put(gctx);

	if (ret)
		return ret;

	/* Rebuild reply: original header + wrapped body. */
	size_t new_total = hdr_len + wrapped_len;
	char *new_reply = malloc(new_total);

	if (!new_reply) {
		free(wrapped);
		return -ENOMEM;
	}

	memcpy(new_reply, rt->rt_reply, hdr_len);
	memcpy(new_reply + hdr_len, wrapped, wrapped_len);
	free(wrapped);

	/* Update record mark with new total size. */
	uint32_t msg_len = (uint32_t)(new_total - sizeof(uint32_t));

	*(uint32_t *)new_reply = htonl(msg_len | 0x80000000);

	free(rt->rt_reply);
	rt->rt_reply = new_reply;
	rt->rt_reply_len = new_total;
	return 0;
}
#endif

int rpc_alloc_accepted_reply(struct rpc_trans *rt, size_t body_size)
{
	uint32_t verf_extra = rpc_compute_gss_reply_verifier(rt);

	rt->rt_reply_len = RPC_ACCEPTED_HDR_WORDS * sizeof(uint32_t) +
			   verf_extra + body_size;
	rt->rt_reply = calloc(rt->rt_reply_len, 1);
	if (!rt->rt_reply)
		return -1;
	rt->rt_offset = 0;
	return 0;
}

int rpc_alloc_denied_reply(struct rpc_trans *rt, size_t body_size)
{
	rt->rt_reply_len = RPC_DENIED_HDR_WORDS * sizeof(uint32_t) + body_size;
	rt->rt_reply = calloc(rt->rt_reply_len, 1);
	if (!rt->rt_reply)
		return -1;
	rt->rt_offset = 0;
	return 0;
}

uint32_t *rpc_build_accepted_header(struct rpc_trans *rt, uint32_t accept_stat)
{
	uint32_t msg_len = (uint32_t)(rt->rt_reply_len - sizeof(uint32_t));
	uint32_t *p = (uint32_t *)rt->rt_reply;

	p = rpc_encode_uint32_t(rt, p, msg_len | 0x80000000);
	if (!p)
		return NULL;
	p = rpc_encode_uint32_t(rt, p, rt->rt_info.ri_xid);
	if (!p)
		return NULL;
	p = rpc_encode_uint32_t(rt, p, 1); /* MSG_REPLY */
	if (!p)
		return NULL;
	p = rpc_encode_uint32_t(rt, p, 0); /* MSG_ACCEPTED */
	if (!p)
		return NULL;
	p = rpc_encode_reply_verifier(rt, p);
	if (!p)
		return NULL;
	p = rpc_encode_uint32_t(rt, p, accept_stat);
	return p;
}

uint32_t *rpc_build_denied_header(struct rpc_trans *rt)
{
	uint32_t msg_len = (uint32_t)(rt->rt_reply_len - sizeof(uint32_t));
	uint32_t *p = (uint32_t *)rt->rt_reply;

	p = rpc_encode_uint32_t(rt, p, msg_len | 0x80000000);
	if (!p)
		return NULL;
	p = rpc_encode_uint32_t(rt, p, rt->rt_info.ri_xid);
	if (!p)
		return NULL;
	p = rpc_encode_uint32_t(rt, p, 1); /* MSG_REPLY */
	if (!p)
		return NULL;
	p = rpc_encode_uint32_t(rt, p, 1); /* MSG_DENIED */
	return p;
}

static int send_auth_tls_response(struct rpc_trans *rt)
{
	uint32_t msg_len;
	uint32_t *p;

	// Calculate reply size (include space for STARTTLS verifier)
	size_t verifier_len = 8;

	rt->rt_reply_len = 7 * sizeof(uint32_t) + verifier_len;
	msg_len = rt->rt_reply_len - sizeof(uint32_t);

	// Allocate memory for reply
	rt->rt_reply = calloc(rt->rt_reply_len, sizeof(char));
	if (!rt->rt_reply) {
		return ENOMEM;
	}

	p = (uint32_t *)rt->rt_reply;

	// Record marker
	p = rpc_encode_uint32_t(rt, p, msg_len | 0x80000000);
	if (!p)
		goto error;

	// XID
	p = rpc_encode_uint32_t(rt, p, rt->rt_info.ri_xid);
	if (!p)
		goto error;

	// Reply type (1 for reply)
	p = rpc_encode_uint32_t(rt, p, 1);
	if (!p)
		goto error;

	// Reply stat (0 for MSG_ACCEPTED)
	p = rpc_encode_uint32_t(rt, p, 0);
	if (!p)
		goto error;

	// AUTH_NONE verifier flavor
	p = rpc_encode_uint32_t(rt, p, AUTH_NONE);
	if (!p)
		goto error;

	// Verifier length
	p = rpc_encode_uint32_t(rt, p, verifier_len);
	if (!p)
		goto error;

	// Copy STARTTLS verifier
	memcpy(p, STARTTLS_VERIFIER, verifier_len);

	// Update position past verifier (with alignment)
	p = (uint32_t *)((char *)p + verifier_len);

	// SUCCESS accept_stat
	p = rpc_encode_uint32_t(rt, p, 0);
	if (!p)
		goto error;

	// Update the offset
	rt->rt_offset = rt->rt_reply_len;

	TRACE("Sending STARTTLS response for fd=%d xid=0x%08x", rt->rt_fd,
	      rt->rt_info.ri_xid);

	// Send the response via callback
	if (rt->rt_rc && rt->rt_cb) {
		rt->rt_cb(rt);
	}

	return 0;

error:
	free(rt->rt_reply);
	rt->rt_reply = NULL;
	return EINVAL;
}

int rpc_protocol_allocate_call(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;

	rt->rt_rph = rpc_program_handler_find(rt->rt_info.ri_program,
					      rt->rt_info.ri_version);
	if (!rt->rt_rph) {
		return ENOENT;
	}

	for (size_t i = 0; i < rt->rt_rph->rph_ops_len; i++) {
		if (rt->rt_rph->rph_ops[i].roh_operation ==
		    rt->rt_info.ri_procedure) {
			if (!rt->rt_rph->rph_ops[i].roh_action)
				return 0;

			ph->ph_op_handler = &rt->rt_rph->rph_ops[i];

			if (rt->rt_rph->rph_ops[i].roh_args_f &&
			    rt->rt_rph->rph_ops[i].roh_args_size) {
				ph->ph_args = calloc(
					1,
					rt->rt_rph->rph_ops[i].roh_args_size);
				if (!ph->ph_args)
					return ENOMEM;
			}

			if (rt->rt_rph->rph_ops[i].roh_res_f &&
			    rt->rt_rph->rph_ops[i].roh_res_size) {
				ph->ph_res = calloc(
					1, rt->rt_rph->rph_ops[i].roh_res_size);
				if (!ph->ph_res) {
					free(ph->ph_args);
					ph->ph_args = NULL;
					return ENOMEM;
				}
			}

			return 0;
		}
	}

	return ENOENT;
}

void rpc_log_packet(const char *prefix, const void *data, size_t len)
{
	const unsigned char *bytes = (const unsigned char *)data;
	char line[256];
	char *ptr;
	int i, j;

	for (i = 0; i < (int)len; i += 16) {
		ptr = line;
		ptr += sprintf(ptr, "%04x  ", i);

		for (j = 0; j < 16; j++) {
			if (i + j < (int)len)
				ptr += sprintf(ptr, "%02x ", bytes[i + j]);
			else
				ptr += sprintf(ptr, "   ");
		}

		TRACE("%s%s", prefix, line);
	}
}

int rpc_protocol_op_call(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	int ret = 0;

	if (ph->ph_op_handler && ph->ph_op_handler->roh_action) {
		struct timespec start, end;
		uint64_t duration_ns;

		/*
		 * Save the op_handler pointer before the call.  If the
		 * handler goes async (-EINPROGRESS), another thread may
		 * resume the task, complete it, and free rt/ph before we
		 * reach the stats recording below.  The op_handler itself
		 * lives in a static table and is safe to dereference.
		 */
		struct rpc_operations_handler *op_handler = ph->ph_op_handler;

		clock_gettime(CLOCK_MONOTONIC, &start);
		ret = op_handler->roh_action(rt);
		clock_gettime(CLOCK_MONOTONIC, &end);

		/*
		 * NFSv3 ops return negative errno on failure.
		 * Trace non-zero results for debugging.
		 */
		/*
		 * NFSv3 ops signal async with -EINPROGRESS; NFSv4 uses
		 * positive EINPROGRESS.  After either, rt is owned by
		 * the resume path and must not be touched.
		 */
		if (ret == -EINPROGRESS)
			ret = EINPROGRESS;

		if (ret && ret != EINPROGRESS)
			TRACE("op %u/%u ret=%d xid=0x%08x",
			      rt->rt_info.ri_program, rt->rt_info.ri_procedure,
			      ret, rt->rt_info.ri_xid);

		duration_ns = (end.tv_sec - start.tv_sec) * 1000000000ULL +
			      (end.tv_nsec - start.tv_nsec);

		rpc_record_operation_stats(op_handler, duration_ns, ret);
	} else {
		rt->rt_info.ri_accept_stat = PROG_UNAVAIL;
	}

	return ret;
}

static void rpc_trans_release(struct urcu_ref *ref)
{
	struct rpc_trans *rt = caa_container_of(ref, struct rpc_trans, rt_ref);
	struct protocol_handler *ph;

	free(rt->rt_unwrapped_body);
	rt->rt_unwrapped_body = NULL;

	switch (rt->rt_info.ri_cred.rc_flavor) {
	case AUTH_SYS:
		xdr_free((xdrproc_t)xdr_authunix_parms,
			 (char *)&rt->rt_info.ri_cred.rc_unix);
		break;
	default:
		break;
	}

	free(rt->rt_info.ri_verifier_body);
	rt->rt_info.ri_verifier_body = NULL;

	ph = (struct protocol_handler *)rt->rt_context;
	if (ph) {
		if (ph->ph_op_handler) {
			if (ph->ph_op_handler->roh_args_f) {
				xdr_free(ph->ph_op_handler->roh_args_f,
					 ph->ph_args);
				free(ph->ph_args);
			}

			if (ph->ph_op_handler->roh_res_f) {
				xdr_free(ph->ph_op_handler->roh_res_f,
					 ph->ph_res);
				free(ph->ph_res);
			}
		}
		free(ph->ph_path);
		free(ph);
	}

	rpc_program_handler_put(rt->rt_rph);

	free(rt->rt_reply);
	free(rt);
}

void rpc_protocol_free(struct rpc_trans *rt)
{
	if (!rt)
		return;

	urcu_ref_put(&rt->rt_ref, rpc_trans_release);
}

struct rpc_trans *rpc_trans_get(struct rpc_trans *rt)
{
	if (!rt)
		return NULL;

	urcu_ref_get(&rt->rt_ref);
	return rt;
}

/*
 * rpc_complete_resumed_task -- encode and send the reply for an async compound
 * that has just finished its resume callback.
 *
 * On the fresh path rpc_process_task() handles both the op dispatch and the
 * reply encoding+send.  On the resume path the worker calls
 * rpc_protocol_op_call() directly, so the reply encoding never happens.
 * This function fills that gap: it XDR-encodes a success RPC reply from the
 * result already sitting in ph->ph_res, queues the write via rt->rt_cb(), and
 * then releases all resources with rpc_protocol_free().
 *
 * On any encoding failure the reply is silently dropped (the client will
 * time-out and retry) and resources are still freed.
 */
void rpc_complete_resumed_task(struct rpc_trans *rt, struct task *t)
{
	uint32_t *p;
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	u_long xdr_size = 0;

	rt->rt_offset = 0;

	if (ph && ph->ph_op_handler && ph->ph_op_handler->roh_res_f)
		xdr_size = xdr_sizeof(ph->ph_op_handler->roh_res_f, ph->ph_res);

	if (rpc_alloc_accepted_reply(rt, xdr_size) < 0)
		goto drop;

	p = rpc_build_accepted_header(rt, 0); /* SUCCESS */
	if (!p)
		goto enc_err;

	if (ph && ph->ph_op_handler && ph->ph_op_handler->roh_res_f) {
		XDR xdrs = { 0 };
		uint32_t start_pos, end_pos;

		xdrmem_create(&xdrs, (char *)p,
			      rt->rt_reply_len - rt->rt_offset, XDR_ENCODE);
		start_pos = xdr_getpos(&xdrs);

		if (!rpc_call_xdr(ph->ph_op_handler->roh_res_f, &xdrs,
				  ph->ph_res)) {
			xdr_destroy(&xdrs);
			goto enc_err;
		}

		end_pos = xdr_getpos(&xdrs);
		xdr_destroy(&xdrs);
		rt->rt_offset += end_pos - start_pos;
	}

#ifdef HAVE_GSSAPI_KRB5
	{
		size_t hdr = rt->rt_reply_len - xdr_size;

		if (rpc_gss_wrap_reply(rt, hdr) < 0)
			goto enc_err;
	}
#endif

	rt->rt_rc = t->t_rc;
	if (__rpc_log_packets)
		rpc_log_packet("TX(resume): ", rt->rt_reply, rt->rt_reply_len);
	rt->rt_cb(rt);
	rpc_protocol_free(rt);
	return;

enc_err:
	free(rt->rt_reply);
	rt->rt_reply = NULL;
drop:
	rpc_protocol_free(rt);
}

struct rpc_trans *rpc_trans_create(void)
{
	struct rpc_trans *rt = calloc(1, sizeof(*rt));
	if (!rt)
		return NULL;

	urcu_ref_init(&rt->rt_ref);

	rt->rt_info.ri_reply_stat = MSG_ACCEPTED;
	rt->rt_info.ri_reject_stat = RPC_MISMATCH;
	rt->rt_info.ri_accept_stat = SUCCESS;
	rt->rt_info.ri_auth_stat = AUTH_OK;

	struct protocol_handler *ph = calloc(1, sizeof(*ph));
	if (!ph) {
		free(rt);
		return NULL;
	}

	rt->rt_context = (void *)ph;

	return rt;
}

static struct rpc_trans *rpc_trans_create_from_task(struct task *t)
{
	struct rpc_trans *rt = rpc_trans_create();
	if (!rt)
		return NULL;

	rt->rt_cb = t->t_cb;

	rt->rt_fd = t->t_fd;
	rt->rt_body = t->t_buffer;
	rt->rt_body_len = t->t_bytes_read;
	rt->rt_offset = 0;
	copy_connection_info(&rt->rt_info.ri_ci, &t->t_ci);

	rt->rt_task = t;
	t->t_rt = rt;
	atomic_store_explicit(&t->t_state, TASK_RUNNING, memory_order_release);

	return rt;
}

// Generate a unique transaction ID for RPC
static uint32_t generate_xid(void)
{
	static _Atomic uint32_t next_id = 1;
	return atomic_fetch_add_explicit(&next_id, 1, memory_order_seq_cst) + 1;
}

int rpc_prepare_send_call(struct rpc_trans *rt)
{
	u_long msg_len = 0;

	uint32_t *p;

	__atomic_fetch_add(&rt->rt_rph->rph_calls, 1, __ATOMIC_RELAXED);

	p = (uint32_t *)rt->rt_body;

	rt->rt_offset = 0;

	XDR xdrs = { 0 };

	uint32_t start_pos, end_pos;
	size_t len;

	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;

	u_long xdr_size = 0;

	if (ph->ph_op_handler->roh_args_f) {
		xdr_size =
			xdr_sizeof(ph->ph_op_handler->roh_args_f, ph->ph_args);
	}

	rt->rt_reply_len = 11 * sizeof(uint32_t) + xdr_size;
	msg_len = rt->rt_reply_len - sizeof(uint32_t);
	rt->rt_reply = calloc(rt->rt_reply_len, sizeof(char));
	if (!rt->rt_reply) {
		return ENOMEM;
	}

	p = (uint32_t *)rt->rt_reply;
	p = rpc_encode_uint32_t(rt, p, msg_len | 0x80000000);
	if (!p) {
		goto drop_on_floor;
	}

	rt->rt_info.ri_xid = generate_xid();

	p = rpc_encode_uint32_t(rt, p, rt->rt_info.ri_xid);
	if (!p) {
		goto drop_on_floor;
	}

	p = rpc_encode_uint32_t(rt, p, 0);
	if (!p) {
		goto drop_on_floor;
	}

	p = rpc_encode_uint32_t(rt, p, 2);
	if (!p) {
		goto drop_on_floor;
	}

	p = rpc_encode_uint32_t(rt, p, rt->rt_info.ri_program);
	if (!p) {
		goto drop_on_floor;
	}

	p = rpc_encode_uint32_t(rt, p, rt->rt_info.ri_version);
	if (!p) {
		goto drop_on_floor;
	}

	p = rpc_encode_uint32_t(rt, p, rt->rt_info.ri_procedure);
	if (!p) {
		goto drop_on_floor;
	}

	p = rpc_encode_uint32_t(rt, p, AUTH_NONE);
	if (!p) {
		goto drop_on_floor;
	}

	p = rpc_encode_uint32_t(rt, p, 0);
	if (!p) {
		goto drop_on_floor;
	}

	p = rpc_encode_uint32_t(rt, p, AUTH_NONE);
	if (!p) {
		goto drop_on_floor;
	}

	p = rpc_encode_uint32_t(rt, p, 0);
	if (!p) {
		goto drop_on_floor;
	}

	if (rt->rt_offset + xdr_size > rt->rt_reply_len) {
		goto drop_on_floor;
	}

	if (ph->ph_op_handler->roh_args_f) {
		xdrmem_create(&xdrs, (char *)p,
			      rt->rt_reply_len - rt->rt_offset, XDR_ENCODE);

		start_pos = xdr_getpos(&xdrs);

		if (!rpc_call_xdr(ph->ph_op_handler->roh_args_f, &xdrs,
				  ph->ph_args)) {
			xdr_destroy(&xdrs);
			goto drop_on_floor;
		}

		end_pos = xdr_getpos(&xdrs);

		len = end_pos - start_pos;

		xdr_destroy(&xdrs);

		rt->rt_offset += len;
	}

	assert(rt->rt_offset == rt->rt_reply_len);

	TRACE("fd=%d xid=0x%08x", rt->rt_fd, rt->rt_info.ri_xid);

	// rpc_log_packet("  ", rt->rt_reply, rt->rt_reply_len);

	return 0;

drop_on_floor:
	free(rt->rt_reply);
	rt->rt_reply = NULL;
	return EINVAL;
}

void rpc_trans_get_sockaddr_in(struct rpc_trans *rt, struct sockaddr_in *sin)
{
	struct connection_info *ci = &rt->rt_info.ri_ci;

	memset(sin, 0, sizeof(*sin));
	if (ci->ci_peer.ss_family == AF_INET) {
		memcpy(sin, &ci->ci_peer, sizeof(struct sockaddr_in));
	}
}

int rpc_process_task(struct task *t)
{
	int ret = 0;
	uint32_t *p;

	if (!t)
		return EINVAL;

	if (t->t_bytes_read < (int)(2 * sizeof(uint32_t))) {
		TRACE("%p", (void *)t);
		return 0;
	}

	struct rpc_trans *rt = rpc_trans_create_from_task(t);
	if (!rt)
		return ENOMEM;

	struct rpc_program_handler *rph = NULL;

	p = (uint32_t *)rt->rt_body;

	if (__rpc_log_packets)
		rpc_log_packet("RX: ", rt->rt_body, rt->rt_body_len);

	p = rpc_decode_uint32_t(rt, p, &rt->rt_info.ri_xid);
	if (!p) {
		rt->rt_info.ri_accept_stat = GARBAGE_ARGS;
		goto handle_rpc_error;
	}

	p = rpc_decode_uint32_t(rt, p, &rt->rt_info.ri_type);
	if (!p) {
		rt->rt_info.ri_accept_stat = GARBAGE_ARGS;
		goto handle_rpc_error;
	}

	if (rt->rt_info.ri_type) {
		TRACE("fd=%d xid=0x%08x", rt->rt_fd, rt->rt_info.ri_xid);

		struct rpc_trans *rt_old =
			io_find_request_by_xid(rt->rt_info.ri_xid);
		if (!rt_old)
			goto drop_on_floor;

		io_unregister_request(rt->rt_info.ri_xid);
		rt->rt_rc = rt_old->rt_rc;
		rt->rt_cb = rt_old->rt_cb;
		rt->rt_compound = rt_old->rt_compound;

		/*
		 * Transfer the protocol-handler context (ph_args, ph_res,
		 * etc.) from the pending outgoing request to this inbound
		 * reply so the reply callback can access decoded results.
		 * The fresh context allocated by rpc_trans_create() has
		 * all-NULL fields and can be freed directly.
		 */
		free(rt->rt_context);
		rt->rt_context = rt_old->rt_context;
		rt_old->rt_context = NULL;

		/* rt_old is now a shell; release it (drops rt_rph ref). */
		rpc_protocol_free(rt_old);

		/*
		 * Invoke the reply callback immediately.  The callback is
		 * responsible for decoding rt->rt_body if needed.
		 */
		if (rt->rt_cb)
			rt->rt_cb(rt);
		rpc_protocol_free(rt);
		return 0;
	}

	p = rpc_decode_uint32_t(rt, p, &rt->rt_info.ri_rpc_version);
	if (!p) {
		rt->rt_info.ri_accept_stat = GARBAGE_ARGS;
		goto handle_rpc_error;
	}

	if (rt->rt_info.ri_rpc_version != 2) {
		rt->rt_info.ri_reply_stat = MSG_DENIED;
		rt->rt_info.ri_reject_stat = RPC_MISMATCH;
	}

	p = rpc_decode_uint32_t(rt, p, &rt->rt_info.ri_program);
	if (!p) {
		rt->rt_info.ri_accept_stat = GARBAGE_ARGS;
		goto handle_rpc_error;
	}

	p = rpc_decode_uint32_t(rt, p, &rt->rt_info.ri_version);
	if (!p) {
		rt->rt_info.ri_accept_stat = GARBAGE_ARGS;
		goto handle_rpc_error;
	}

	rph = rpc_program_handler_find(rt->rt_info.ri_program,
				       rt->rt_info.ri_version);
	if (!rph) {
		rt->rt_info.ri_accept_stat = PROG_UNAVAIL;
		goto handle_rpc_error;
	}

	__atomic_fetch_add(&rph->rph_calls, 1, __ATOMIC_RELAXED);

	/* One-shot: log the first call for each protocol. */
	if (!(__atomic_load_n(&rph->rph_flags, __ATOMIC_RELAXED) &
	      RPH_FIRST_LOGGED)) {
		__atomic_fetch_or(&rph->rph_flags, RPH_FIRST_LOGGED,
				  __ATOMIC_RELAXED);
		TRACE("First call: program=%u version=%u", rph->rph_program,
		      rph->rph_version);
	}

	p = rpc_decode_uint32_t(rt, p, &rt->rt_info.ri_procedure);
	if (!p) {
		rt->rt_info.ri_accept_stat = GARBAGE_ARGS;
		__atomic_fetch_add(&rph->rph_accepted_errors, 1,
				   __ATOMIC_RELAXED);
		goto handle_rpc_error;
	}

	p = rpc_decode_uint32_t(rt, p, &rt->rt_info.ri_cred.rc_flavor);
	if (!p) {
		rt->rt_info.ri_accept_stat = GARBAGE_ARGS;
		__atomic_fetch_add(&rph->rph_accepted_errors, 1,
				   __ATOMIC_RELAXED);
		goto handle_rpc_error;
	}

	uint32_t flavor_len;
	switch (rt->rt_info.ri_cred.rc_flavor) {
	case AUTH_NONE:
	case AUTH_TLS:
		p = rpc_decode_uint32_t(rt, p, &flavor_len);
		if (!p) {
			rt->rt_info.ri_accept_stat = GARBAGE_ARGS;
			__atomic_fetch_add(&rph->rph_accepted_errors, 1,
					   __ATOMIC_RELAXED);
			goto handle_rpc_error;
		}
		break;
	case AUTH_SYS: {
		XDR xdrs = { 0 };

		p = rpc_decode_uint32_t(rt, p, &flavor_len);
		if (!p || flavor_len > rt->rt_body_len - rt->rt_offset) {
			rt->rt_info.ri_accept_stat = GARBAGE_ARGS;
			__atomic_fetch_add(&rph->rph_accepted_errors, 1,
					   __ATOMIC_RELAXED);
			goto handle_rpc_error;
		}

		xdrmem_create(&xdrs, (char *)p, flavor_len, XDR_DECODE);

		if (!xdr_authunix_parms(&xdrs, &rt->rt_info.ri_cred.rc_unix)) {
			xdr_free((xdrproc_t)xdr_authunix_parms,
				 (char *)&rt->rt_info.ri_cred.rc_unix);
			rt->rt_info.ri_auth_stat = AUTH_BADCRED;
			rt->rt_info.ri_reply_stat = MSG_DENIED;
			rt->rt_info.ri_reject_stat = AUTH_ERROR;
			__atomic_fetch_add(&rph->rph_authed_errors, 1,
					   __ATOMIC_RELAXED);
			__atomic_fetch_add(&rph->rph_replied_errors, 1,
					   __ATOMIC_RELAXED);
			__atomic_fetch_add(&rph->rph_rejected_errors, 1,
					   __ATOMIC_RELAXED);
			xdr_destroy(&xdrs);
			goto handle_rpc_error;
		}

		xdr_destroy(&xdrs);
		rt->rt_offset += flavor_len;
		p = (uint32_t *)(p + flavor_len / sizeof(uint32_t));
		break;
	}
	case RPCSEC_GSS: {
		/*
		 * RFC 2203 S5.2.2: RPCSEC_GSS credential body is
		 * rpc_gss_cred_vers_1_t {version, gss_proc, seq_num,
		 * service, handle<>}.
		 */
		uint32_t gss_vers;

		p = rpc_decode_uint32_t(rt, p, &flavor_len);
		if (!p) {
			rt->rt_info.ri_accept_stat = GARBAGE_ARGS;
			goto handle_rpc_error;
		}

		uint32_t *gss_start = p;

		p = rpc_decode_uint32_t(rt, p, &gss_vers);
		if (!p || gss_vers != 1) {
			rt->rt_info.ri_auth_stat = AUTH_BADCRED;
			rt->rt_info.ri_reply_stat = MSG_DENIED;
			rt->rt_info.ri_reject_stat = AUTH_ERROR;
			goto handle_rpc_error;
		}

		p = rpc_decode_uint32_t(rt, p,
					&rt->rt_info.ri_cred.rc_gss.gc_proc);
		p = rpc_decode_uint32_t(rt, p,
					&rt->rt_info.ri_cred.rc_gss.gc_seq);
		p = rpc_decode_uint32_t(rt, p,
					&rt->rt_info.ri_cred.rc_gss.gc_svc);
		if (!p) {
			rt->rt_info.ri_accept_stat = GARBAGE_ARGS;
			goto handle_rpc_error;
		}

		/* Decode opaque handle<> */
		uint32_t hlen;

		p = rpc_decode_uint32_t(rt, p, &hlen);
		if (!p || hlen > 16) {
			rt->rt_info.ri_auth_stat = AUTH_BADCRED;
			rt->rt_info.ri_reply_stat = MSG_DENIED;
			rt->rt_info.ri_reject_stat = AUTH_ERROR;
			goto handle_rpc_error;
		}
		if (hlen > 0)
			memcpy(rt->rt_info.ri_cred.rc_gss.gc_handle, p, hlen);
		rt->rt_info.ri_cred.rc_gss.gc_handle_len = hlen;

		/* Advance past handle (XDR 4-byte aligned). */
		uint32_t padded = (hlen + 3) & ~3u;

		rt->rt_offset += padded;
		p = (uint32_t *)((char *)p + padded);

		/* Advance past any remaining credential body. */
		uint32_t consumed = (uint32_t)((char *)p - (char *)gss_start);
		if (consumed < flavor_len) {
			uint32_t skip = flavor_len - consumed;

			rt->rt_offset += skip;
			p = (uint32_t *)((char *)p + skip);
		}

		/* Save the credential end offset for GSS MIC verification.
		 * The MIC covers rt->rt_body[0..ri_cred_end]. */
		rt->rt_info.ri_cred_end = rt->rt_offset;
		break;
	}
	case AUTH_SHORT:
	case AUTH_DH:
	default:
		rt->rt_info.ri_auth_stat = AUTH_BADCRED;
		rt->rt_info.ri_reply_stat = MSG_DENIED;
		rt->rt_info.ri_reject_stat = AUTH_ERROR;
		__atomic_fetch_add(&rph->rph_authed_errors, 1,
				   __ATOMIC_RELAXED);
		__atomic_fetch_add(&rph->rph_replied_errors, 1,
				   __ATOMIC_RELAXED);
		__atomic_fetch_add(&rph->rph_rejected_errors, 1,
				   __ATOMIC_RELAXED);
		break;
	}

	p = rpc_decode_uint32_t(rt, p, &rt->rt_info.ri_verifier_flavor);
	if (!p) {
		rt->rt_info.ri_accept_stat = GARBAGE_ARGS;
		__atomic_fetch_add(&rph->rph_accepted_errors, 1,
				   __ATOMIC_RELAXED);
		goto handle_rpc_error;
	}

	uint32_t verifier_len;
	switch (rt->rt_info.ri_verifier_flavor) {
	case AUTH_TLS:
	case AUTH_NONE:
		p = rpc_decode_uint32_t(rt, p, &verifier_len);
		if (!p) {
			rt->rt_info.ri_accept_stat = GARBAGE_ARGS;
			__atomic_fetch_add(&rph->rph_accepted_errors, 1,
					   __ATOMIC_RELAXED);
			goto handle_rpc_error;
		}
		break;
	case RPCSEC_GSS: {
		/*
		 * RFC 2203 S5.3.3.2: the RPCSEC_GSS verifier is a
		 * MIC over the sequence number.  Save it for later
		 * validation after context lookup.
		 */
		uint32_t gss_verf_len;

		p = rpc_decode_uint32_t(rt, p, &gss_verf_len);
		if (!p || gss_verf_len > 1024) {
			rt->rt_info.ri_auth_stat = AUTH_BADVERF;
			goto handle_rpc_error;
		}

		if (gss_verf_len > 0) {
			rt->rt_info.ri_verifier_body = malloc(gss_verf_len);
			if (!rt->rt_info.ri_verifier_body) {
				rt->rt_info.ri_auth_stat = AUTH_BADVERF;
				goto handle_rpc_error;
			}
			memcpy(rt->rt_info.ri_verifier_body, p, gss_verf_len);
			rt->rt_info.ri_verifier_len = gss_verf_len;

			uint32_t padded = (gss_verf_len + 3) & ~3u;

			rt->rt_offset += padded;
			p = (uint32_t *)((char *)p + padded);
		}
		break;
	}
	case AUTH_SYS:
	case AUTH_SHORT:
	case AUTH_DH:
	default:
		rt->rt_info.ri_auth_stat = AUTH_BADVERF;
		__atomic_fetch_add(&rph->rph_authed_errors, 1,
				   __ATOMIC_RELAXED);
		break;
	}

	trace_rpc_task(rt, __func__, __LINE__);

	if (rt->rt_info.ri_cred.rc_flavor == AUTH_SYS) {
		struct reffs_context ctx = {
			.uid = rt->rt_info.ri_cred.rc_unix.aup_uid,
			.gid = rt->rt_info.ri_cred.rc_unix.aup_gid
		};
		reffs_set_context(&ctx);
	} else {
		reffs_set_context(NULL);
	}

#ifdef HAVE_GSSAPI_KRB5
	/*
	 * For GSS DATA requests, map the principal to uid/gid and
	 * store in rc_unix so downstream code (compound, access
	 * checks) works unchanged.
	 */
	if (rt->rt_info.ri_cred.rc_flavor == RPCSEC_GSS &&
	    rt->rt_info.ri_cred.rc_gss.gc_proc == RPCSEC_GSS_DATA) {
		trace_security_gss_data(
			rt->rt_info.ri_xid, rt->rt_info.ri_cred.rc_gss.gc_seq,
			rt->rt_info.ri_cred.rc_gss.gc_svc,
			rt->rt_info.ri_cred.rc_gss.gc_handle_len, __func__,
			__LINE__);
		struct gss_ctx_entry *gctx =
			gss_ctx_find(rt->rt_info.ri_cred.rc_gss.gc_handle,
				     rt->rt_info.ri_cred.rc_gss.gc_handle_len);
		if (gctx) {
			/* Renew activity timestamp for reaper. */
			gctx->gc_last_activity_ns = reffs_now_ns();

			/*
			 * RFC 2203 S5.3.3.2: verify the client's
			 * MIC over the sequence number.  Reject if
			 * the verifier is missing or invalid.
			 */
			if (!rt->rt_info.ri_verifier_body ||
			    rt->rt_info.ri_verifier_len == 0) {
				trace_security_gss_error(
					"DATA missing verifier", 0, __func__,
					__LINE__);
				gss_ctx_put(gctx);
				rt->rt_info.ri_auth_stat =
					RPCSEC_GSS_CREDPROBLEM;
				rt->rt_info.ri_reply_stat = MSG_DENIED;
				rt->rt_info.ri_reject_stat = AUTH_ERROR;
				goto handle_rpc_error;
			}

			/*
			 * RFC 2203 S5.3.1: the client's MIC covers the
			 * RPC header from XID through the end of the
			 * credential (not just the sequence number).
			 */
			uint32_t vmaj;

			vmaj = gss_ctx_verify_mic(gctx, rt->rt_body,
						  rt->rt_info.ri_cred_end,
						  rt->rt_info.ri_verifier_body,
						  rt->rt_info.ri_verifier_len);
			if (vmaj != GSS_S_COMPLETE) {
				trace_security_gss_error(
					"DATA verifier MIC failed", vmaj,
					__func__, __LINE__);
				gss_ctx_put(gctx);
				rt->rt_info.ri_auth_stat =
					RPCSEC_GSS_CREDPROBLEM;
				rt->rt_info.ri_reply_stat = MSG_DENIED;
				rt->rt_info.ri_reject_stat = AUTH_ERROR;
				goto handle_rpc_error;
			}

			/* Replay detection (RFC 2203 S5.2.1). */
			if (gss_ctx_seq_check(
				    gctx, rt->rt_info.ri_cred.rc_gss.gc_seq)) {
				trace_security_gss_error(
					"DATA seq replay/out-of-window",
					rt->rt_info.ri_cred.rc_gss.gc_seq,
					__func__, __LINE__);
				gss_ctx_put(gctx);
				rt->rt_info.ri_auth_stat =
					RPCSEC_GSS_CREDPROBLEM;
				rt->rt_info.ri_reply_stat = MSG_DENIED;
				rt->rt_info.ri_reject_stat = AUTH_ERROR;
				goto handle_rpc_error;
			}

			uid_t uid;
			gid_t gid;
			int map_ret;

			map_ret = gss_ctx_map_to_unix(gctx, &uid, &gid);
			if (map_ret < 0)
				trace_security_gss_error(
					"principal mapping failed, using nobody",
					0, __func__, __LINE__);

			rt->rt_info.ri_mapped_uid = uid;
			rt->rt_info.ri_mapped_gid = gid;
			rt->rt_info.ri_gss_mapped = true;

			struct reffs_context ctx = { .uid = uid, .gid = gid };

			reffs_set_context(&ctx);
			gss_ctx_put(gctx);
		} else {
			trace_security_gss_error(
				"DATA context not found for handle", 0,
				__func__, __LINE__);
			rt->rt_info.ri_auth_stat = RPCSEC_GSS_CTXPROBLEM;
			rt->rt_info.ri_reply_stat = MSG_DENIED;
			rt->rt_info.ri_reject_stat = AUTH_ERROR;
			goto handle_rpc_error;
		}
	}
#endif

	if (rt->rt_info.ri_cred.rc_flavor == AUTH_TLS &&
	    rt->rt_info.ri_procedure == 0) {
		trace_security_tls(rt->rt_fd, "probe", __func__, __LINE__);

		/*
		 * RFC 9289 S4.1: the "STARTTLS" string belongs in the
		 * server's reply verifier, not the client's call verifier.
		 * The client uses AUTH_NONE with an empty body.  Accept
		 * the probe regardless of verifier content.
		 */
		rt->rt_info.ri_reply_stat = MSG_ACCEPTED;
		rt->rt_info.ri_accept_stat = SUCCESS;

		rt->rt_rc = t->t_rc;
		rt->rt_offset = 0;

		ret = send_auth_tls_response(rt);

		struct conn_info *ci = io_conn_get(rt->rt_fd);
		if (ci)
			ci->ci_tls_handshaking = true;

		rpc_program_handler_put(rph);
		rpc_protocol_free(rt);
		return ret;
	}

	/*
	 * RPCSEC_GSS context establishment: INIT and CONTINUE_INIT
	 * are handled here, not dispatched to protocol ops.  The
	 * call body is a GSS token, not an NFS procedure.
	 */
#ifdef HAVE_GSSAPI_KRB5
	if (rt->rt_info.ri_cred.rc_flavor == RPCSEC_GSS &&
	    (rt->rt_info.ri_cred.rc_gss.gc_proc == RPCSEC_GSS_INIT ||
	     rt->rt_info.ri_cred.rc_gss.gc_proc == RPCSEC_GSS_CONTINUE_INIT)) {
		rt->rt_rc = t->t_rc;
		ret = rpc_gss_handle_init(rt);
		rpc_program_handler_put(rph);
		rpc_protocol_free(rt);
		return ret;
	}

	if (rt->rt_info.ri_cred.rc_flavor == RPCSEC_GSS &&
	    rt->rt_info.ri_cred.rc_gss.gc_proc == RPCSEC_GSS_DESTROY) {
		ret = rpc_gss_handle_destroy(rt);
		rpc_program_handler_put(rph);
		rpc_protocol_free(rt);
		return ret;
	}
#endif

	/*
	 * GSS integrity / privacy: unwrap the call body before
	 * allocating and parsing the XDR arguments.
	 */
#ifdef HAVE_GSSAPI_KRB5
	if (rt->rt_info.ri_cred.rc_flavor == RPCSEC_GSS &&
	    rt->rt_info.ri_cred.rc_gss.gc_proc == RPCSEC_GSS_DATA &&
	    rt->rt_info.ri_cred.rc_gss.gc_svc != RPC_GSS_SVC_NONE) {
		char *unwrapped = NULL;
		size_t unwrapped_len = 0;

		struct gss_ctx_entry *gctx2 =
			gss_ctx_find(rt->rt_info.ri_cred.rc_gss.gc_handle,
				     rt->rt_info.ri_cred.rc_gss.gc_handle_len);
		if (!gctx2) {
			ret = EACCES;
			goto handle_rpc_error;
		}

		ret = gss_ctx_unwrap_request(gctx2,
					     rt->rt_info.ri_cred.rc_gss.gc_svc,
					     rt->rt_info.ri_cred.rc_gss.gc_seq,
					     rt->rt_body + rt->rt_offset,
					     rt->rt_body_len - rt->rt_offset,
					     &unwrapped, &unwrapped_len);
		gss_ctx_put(gctx2);

		if (ret) {
			trace_security_gss_error(
				"unwrap failed",
				rt->rt_info.ri_cred.rc_gss.gc_svc, __func__,
				__LINE__);
			rt->rt_info.ri_auth_stat = RPCSEC_GSS_CREDPROBLEM;
			rt->rt_info.ri_reply_stat = MSG_DENIED;
			rt->rt_info.ri_reject_stat = AUTH_ERROR;
			goto handle_rpc_error;
		}

		/* Replace body with unwrapped data for XDR decode. */
		rt->rt_unwrapped_body = unwrapped;
		rt->rt_body = unwrapped;
		rt->rt_body_len = unwrapped_len;
		rt->rt_offset = 0;
	}
#endif

	ret = rpc_protocol_allocate_call(rt);
	if (!ret)
		ret = rpc_parse_call_data(rt);

	if (ret == ENOENT) {
		rt->rt_info.ri_reply_stat = MSG_ACCEPTED;
		rt->rt_info.ri_accept_stat = PROG_UNAVAIL;
		__atomic_fetch_add(&rph->rph_replied_errors, 1,
				   __ATOMIC_RELAXED);
		__atomic_fetch_add(&rph->rph_accepted_errors, 1,
				   __ATOMIC_RELAXED);
	} else if (ret == EINVAL) {
		/*
		 * XDR decode failure -- RFC 5531 S9: GARBAGE_ARGS,
		 * not SYSTEM_ERR (which is for internal server errors).
		 */
		rt->rt_info.ri_reply_stat = MSG_ACCEPTED;
		rt->rt_info.ri_accept_stat = GARBAGE_ARGS;
		__atomic_fetch_add(&rph->rph_replied_errors, 1,
				   __ATOMIC_RELAXED);
		__atomic_fetch_add(&rph->rph_accepted_errors, 1,
				   __ATOMIC_RELAXED);
	} else if (ret) {
		rt->rt_info.ri_reply_stat = MSG_ACCEPTED;
		rt->rt_info.ri_accept_stat = SYSTEM_ERR;
		__atomic_fetch_add(&rph->rph_replied_errors, 1,
				   __ATOMIC_RELAXED);
		__atomic_fetch_add(&rph->rph_accepted_errors, 1,
				   __ATOMIC_RELAXED);
	} else {
		ret = rpc_protocol_op_call(rt);
		if (ret == EINPROGRESS) {
			/*
			 * The compound went async: an op called task_pause()
			 * and the task has been (or will be) re-enqueued via
			 * task_resume().  The task, rpc_trans, and compound
			 * are now owned by the async completer.
			 *
			 * Release the rph ref we hold and return EINPROGRESS
			 * to the worker loop so it does NOT free the task
			 * buffer.  Everything else stays alive.
			 */
			rpc_program_handler_put(rph);
			return EINPROGRESS;
		}
		if (!ret) {
			p = (uint32_t *)(rt->rt_body + rt->rt_offset);
		}
	}

handle_rpc_error:
	rt->rt_offset = 0;

	if (rt->rt_info.ri_reply_stat == MSG_DENIED) {
		if (rt->rt_info.ri_reject_stat == RPC_MISMATCH) {
			/*
			 * RFC 5531: MSG_DENIED, RPC_MISMATCH.
			 * Body: reject_stat + low_version + high_version.
			 */
			if (rpc_alloc_denied_reply(rt, 3 * sizeof(uint32_t)) <
			    0)
				goto drop_on_floor;

			p = rpc_build_denied_header(rt);
			if (!p) {
				free(rt->rt_reply);
				rt->rt_reply = NULL;
				goto drop_on_floor;
			}

			p = rpc_encode_uint32_t(rt, p, RPC_MISMATCH);
			if (!p) {
				free(rt->rt_reply);
				rt->rt_reply = NULL;
				goto drop_on_floor;
			}

			p = rpc_encode_uint32_t(rt, p, 2); /* low version */
			if (!p) {
				free(rt->rt_reply);
				rt->rt_reply = NULL;
				goto drop_on_floor;
			}

			p = rpc_encode_uint32_t(rt, p, 2); /* high version */
			if (!p) {
				free(rt->rt_reply);
				rt->rt_reply = NULL;
				goto drop_on_floor;
			}
		} else {
			/*
			 * RFC 5531: MSG_DENIED, AUTH_ERROR.
			 * Body: reject_stat + auth_stat.
			 */
			if (rpc_alloc_denied_reply(rt, 2 * sizeof(uint32_t)) <
			    0)
				goto drop_on_floor;

			p = rpc_build_denied_header(rt);
			if (!p) {
				free(rt->rt_reply);
				rt->rt_reply = NULL;
				goto drop_on_floor;
			}

			p = rpc_encode_uint32_t(rt, p, AUTH_ERROR);
			if (!p) {
				free(rt->rt_reply);
				rt->rt_reply = NULL;
				goto drop_on_floor;
			}

			p = rpc_encode_uint32_t(rt, p,
						rt->rt_info.ri_auth_stat);
			if (!p) {
				free(rt->rt_reply);
				rt->rt_reply = NULL;
				goto drop_on_floor;
			}
		}
	} else if (rt->rt_info.ri_accept_stat) {
		if (rpc_alloc_accepted_reply(rt, 0) < 0)
			goto drop_on_floor;
		p = rpc_build_accepted_header(rt, rt->rt_info.ri_accept_stat);
		if (!p) {
			free(rt->rt_reply);
			rt->rt_reply = NULL;
			goto drop_on_floor;
		}
#ifdef HAVE_GSSAPI_KRB5
		if (rpc_gss_wrap_reply(rt, rt->rt_reply_len) < 0) {
			free(rt->rt_reply);
			rt->rt_reply = NULL;
			goto drop_on_floor;
		}
#endif
	} else {
		XDR xdrs = { 0 };

		uint32_t start_pos, end_pos;
		size_t len;

		struct protocol_handler *ph =
			(struct protocol_handler *)rt->rt_context;

		u_long xdr_size = 0;

		if (ph->ph_op_handler->roh_res_f) {
			xdr_size = xdr_sizeof(ph->ph_op_handler->roh_res_f,
					      ph->ph_res);
		}

		if (rpc_alloc_accepted_reply(rt, xdr_size) < 0)
			goto drop_on_floor;

		p = rpc_build_accepted_header(rt, 0); /* SUCCESS */
		if (!p) {
			rt->rt_info.ri_accept_stat = SYSTEM_ERR;
			__atomic_fetch_add(&rph->rph_accepted_errors, 1,
					   __ATOMIC_RELAXED);
			free(rt->rt_reply);
			rt->rt_reply = NULL;
			goto handle_rpc_error;
		}

		if (rt->rt_offset + xdr_size > rt->rt_reply_len) {
			rt->rt_info.ri_accept_stat = SYSTEM_ERR;
			__atomic_fetch_add(&rph->rph_accepted_errors, 1,
					   __ATOMIC_RELAXED);
			free(rt->rt_reply);
			rt->rt_reply = NULL;
			goto handle_rpc_error;
		}

		if (ph->ph_op_handler->roh_res_f) {
			xdrmem_create(&xdrs, (char *)p,
				      rt->rt_reply_len - rt->rt_offset,
				      rt->rt_info.ri_type == 0 ? XDR_ENCODE :
								 XDR_DECODE);

			start_pos = xdr_getpos(&xdrs);

			if (!rpc_call_xdr(ph->ph_op_handler->roh_res_f, &xdrs,
					  ph->ph_res)) {
				xdr_destroy(&xdrs);
				rt->rt_info.ri_accept_stat = SYSTEM_ERR;
				__atomic_fetch_add(&rph->rph_accepted_errors, 1,
						   __ATOMIC_RELAXED);
				free(rt->rt_reply);
				rt->rt_reply = NULL;
				goto handle_rpc_error;
			}

			end_pos = xdr_getpos(&xdrs);

			len = end_pos - start_pos;

			xdr_destroy(&xdrs);

			rt->rt_offset += len;
		}

		if (rt->rt_offset != rt->rt_reply_len) {
			LOG("XDR_SIZE_MISMATCH: xdr_size=%lu rt_offset=%zu "
			    "rt_reply_len=%zu delta=%zd proc=%u prog=%u",
			    xdr_size, rt->rt_offset, rt->rt_reply_len,
			    (ssize_t)rt->rt_offset - (ssize_t)rt->rt_reply_len,
			    rt->rt_info.ri_procedure, rt->rt_info.ri_program);
		}
		assert(rt->rt_offset == rt->rt_reply_len);

#ifdef HAVE_GSSAPI_KRB5
		{
			size_t hdr = rt->rt_reply_len - xdr_size;

			if (rpc_gss_wrap_reply(rt, hdr) < 0) {
				free(rt->rt_reply);
				rt->rt_reply = NULL;
				goto drop_on_floor;
			}
		}
#endif
	}

	if (rt->rt_reply && rt->rt_reply_len > 0) {
		rt->rt_rc = t->t_rc;
		if (__rpc_log_packets)
			rpc_log_packet("TX: ", rt->rt_reply, rt->rt_reply_len);
		rt->rt_cb(rt);

		// Successfully processed and queued for writing
		rpc_program_handler_put(rph);
		rpc_protocol_free(rt);
		return 0;
	}

drop_on_floor:
	TRACE("DROPPED TASK: fd=%d xid=0x%08x", rt->rt_fd, rt->rt_info.ri_xid);
	rpc_program_handler_put(rph);
	rpc_protocol_free(rt);

	return 0;
}
