/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <assert.h>
#include <errno.h>
#include <hdr/hdr_histogram.h>
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

#include "reffs/log.h"
#include "reffs/context.h"
#include "reffs/io.h"
#include "reffs/network.h"
#include "reffs/rcu.h"
#include "reffs/rpc.h"
#include "reffs/task.h"
#include "reffs/tls.h"
#include "reffs/trace/rpc.h"

struct rcu_head;

CDS_LIST_HEAD(rpc_program_handler_list);

/*
 * xdrproc_t is bool_t (*)(XDR *, ...) — variadic — for historical reasons.
 * Actual XDR functions are non-variadic (e.g. bool_t xdr_FOO(XDR *, FOO *)).
 * Calling them through xdrproc_t is ABI-safe but a strict type mismatch that
 * UBSan's -fsanitize=function flags.  Isolate all direct calls here so the
 * suppression is narrow.
 */
__attribute__((no_sanitize("function"))) static bool_t
rpc_call_xdr(xdrproc_t fn, XDR *xdrs, void *obj)
{
	return fn(xdrs, obj);
}

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

	for (size_t i = 0; i < rph->rph_ops_len; i++) {
		hdr_close(rph->rph_ops[i].roh_stats.rs_histogram);
		rph->rph_ops[i].roh_stats.rs_histogram = NULL;
	}

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

	for (size_t i = 0; i < roh_len; i++) {
		hdr_init(1, INT64_C(3600000000000), 3,
			 &roh[i].roh_stats.rs_histogram);
	}

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

	hdr_record_value(roh->roh_stats.rs_histogram, duration_ns);
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
		/* NFSv3 ops signal async with -EINPROGRESS; normalize for caller */
		if (ret == -EINPROGRESS)
			ret = EINPROGRESS;

		duration_ns = (end.tv_sec - start.tv_sec) * 1000000000ULL +
			      (end.tv_nsec - start.tv_nsec);

		rpc_record_operation_stats(op_handler, duration_ns, ret);
	} else {
		rt->rt_info.ri_accept_stat = PROG_UNAVAIL;
	}

	return ret;
}

void rpc_protocol_free(struct rpc_trans *rt)
{
	struct protocol_handler *ph;

	if (!rt)
		return;

	switch (rt->rt_info.ri_cred.rc_flavor) {
	case AUTH_SYS:
		xdr_free((xdrproc_t)xdr_authunix_parms,
			 (char *)&rt->rt_info.ri_cred.rc_unix);
		break;
	default:
		break;
	}

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
	u_long msg_len;
	uint32_t *p;
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	u_long xdr_size = 0;

	rt->rt_offset = 0;

	if (ph && ph->ph_op_handler && ph->ph_op_handler->roh_res_f)
		xdr_size = xdr_sizeof(ph->ph_op_handler->roh_res_f, ph->ph_res);

	rt->rt_reply_len = 7 * sizeof(uint32_t) + xdr_size;
	msg_len = rt->rt_reply_len - sizeof(uint32_t);
	rt->rt_reply = calloc(rt->rt_reply_len, sizeof(char));
	if (!rt->rt_reply)
		goto drop;

	p = (uint32_t *)rt->rt_reply;

	p = rpc_encode_uint32_t(rt, p, (uint32_t)(msg_len | 0x80000000));
	if (!p)
		goto enc_err;
	p = rpc_encode_uint32_t(rt, p, rt->rt_info.ri_xid);
	if (!p)
		goto enc_err;
	p = rpc_encode_uint32_t(rt, p, 1); /* MSG_REPLY */
	if (!p)
		goto enc_err;
	p = rpc_encode_uint32_t(rt, p, 0); /* MSG_ACCEPTED */
	if (!p)
		goto enc_err;
	p = rpc_encode_uint32_t(rt, p, 0); /* AUTH_NULL flavor */
	if (!p)
		goto enc_err;
	p = rpc_encode_uint32_t(rt, p, 0); /* verifier len = 0 */
	if (!p)
		goto enc_err;
	p = rpc_encode_uint32_t(rt, p, 0); /* accept_stat = SUCCESS */
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
	u_long msg_len = 0;
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
		if (!p) {
			rt->rt_info.ri_accept_stat = GARBAGE_ARGS;
			__atomic_fetch_add(&rph->rph_accepted_errors, 1,
					   __ATOMIC_RELAXED);
			goto handle_rpc_error;
		}

		xdrmem_create(&xdrs, (char *)p, rt->rt_body_len - rt->rt_offset,
			      XDR_DECODE);

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
	case AUTH_SHORT:
	case AUTH_DH:
	case RPCSEC_GSS:
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
	case AUTH_SYS:
	case AUTH_SHORT:
	case AUTH_DH:
	case RPCSEC_GSS:
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

	if (rt->rt_info.ri_cred.rc_flavor == AUTH_TLS &&
	    rt->rt_info.ri_procedure == 0) {
		LOG("AUTH_TLS probe detected on fd=%d len=%u xid=0x%08x",
		    rt->rt_fd, verifier_len, rt->rt_info.ri_xid);

#ifdef LINUX_IS_NOT_RFC_9289_COMPLIANT
		if (verifier_len != 8 ||
		    memcmp(p, STARTTLS_VERIFIER, strlen(STARTTLS_VERIFIER))) {
			rt->rt_info.ri_accept_stat = GARBAGE_ARGS;
			__atomic_fetch_add(&rph->rph_accepted_errors, 1,
					   __ATOMIC_RELAXED);
			goto handle_rpc_error;
		}
#endif

		rt->rt_info.ri_reply_stat = MSG_ACCEPTED;
		rt->rt_info.ri_accept_stat = SUCCESS;

		rt->rt_rc = t->t_rc;
		rt->rt_offset = 0;

		// Send STARTTLS response
		ret = send_auth_tls_response(rt);

		// Mark connection as waiting for TLS handshake
		struct conn_info *ci = io_conn_get(rt->rt_fd);
		if (ci) {
			ci->ci_tls_handshaking = true;
		}

		rpc_program_handler_put(rph);

		// Don't continue with regular RPC processing
		rpc_protocol_free(rt);
		return ret;
	}

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
			rt->rt_reply_len = 7 * sizeof(uint32_t);
			msg_len = rt->rt_reply_len - sizeof(uint32_t);
			rt->rt_reply = calloc(rt->rt_reply_len, sizeof(char));
			if (!rt->rt_reply) {
				goto drop_on_floor;
			}

			p = (uint32_t *)rt->rt_reply;

			p = rpc_encode_uint32_t(rt, p, msg_len | 0x80000000);
			if (!p) {
				free(rt->rt_reply);
				rt->rt_reply = NULL;
				goto drop_on_floor;
			}

			p = rpc_encode_uint32_t(rt, p, rt->rt_info.ri_xid);
			if (!p) {
				free(rt->rt_reply);
				rt->rt_reply = NULL;
				goto drop_on_floor;
			}

			p = rpc_encode_uint32_t(rt, p, 1);
			if (!p) {
				free(rt->rt_reply);
				rt->rt_reply = NULL;
				goto drop_on_floor;
			}

			p = rpc_encode_uint32_t(rt, p, 0);
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

			p = rpc_encode_uint32_t(rt, p, 2);
			if (!p) {
				free(rt->rt_reply);
				rt->rt_reply = NULL;
				goto drop_on_floor;
			}

			p = rpc_encode_uint32_t(rt, p, 2);
			if (!p) {
				free(rt->rt_reply);
				rt->rt_reply = NULL;
				goto drop_on_floor;
			}
		} else {
			rt->rt_reply_len = 6 * sizeof(uint32_t);
			msg_len = rt->rt_reply_len - sizeof(uint32_t);
			rt->rt_reply = calloc(rt->rt_reply_len, sizeof(char));
			if (!rt->rt_reply) {
				rt->rt_reply = NULL;
				goto drop_on_floor;
			}
			p = (uint32_t *)rt->rt_reply;

			p = rpc_encode_uint32_t(rt, p, msg_len | 0x80000000);
			if (!p) {
				free(rt->rt_reply);
				rt->rt_reply = NULL;
				goto drop_on_floor;
			}

			p = rpc_encode_uint32_t(rt, p, rt->rt_info.ri_xid);
			if (!p) {
				free(rt->rt_reply);
				rt->rt_reply = NULL;
				goto drop_on_floor;
			}

			p = rpc_encode_uint32_t(rt, p, 1);
			if (!p) {
				free(rt->rt_reply);
				rt->rt_reply = NULL;
				goto drop_on_floor;
			}

			p = rpc_encode_uint32_t(rt, p, 0);
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
		rt->rt_reply_len = 8 * sizeof(uint32_t);
		msg_len = rt->rt_reply_len - sizeof(uint32_t);
		rt->rt_reply = calloc(rt->rt_reply_len, sizeof(char));
		if (!rt->rt_reply) {
			goto drop_on_floor;
		}

		p = (uint32_t *)rt->rt_reply;

		p = rpc_encode_uint32_t(rt, p, msg_len | 0x80000000);
		if (!p) {
			free(rt->rt_reply);
			rt->rt_reply = NULL;
			goto drop_on_floor;
		}

		p = rpc_encode_uint32_t(rt, p, rt->rt_info.ri_xid);
		if (!p) {
			free(rt->rt_reply);
			rt->rt_reply = NULL;
			goto drop_on_floor;
		}

		p = rpc_encode_uint32_t(rt, p, 1);
		if (!p) {
			free(rt->rt_reply);
			rt->rt_reply = NULL;
			goto drop_on_floor;
		}

		p = rpc_encode_uint32_t(rt, p, 1);
		if (!p) {
			free(rt->rt_reply);
			rt->rt_reply = NULL;
			goto drop_on_floor;
		}

		p = rpc_encode_uint32_t(rt, p, 0);
		if (!p) {
			free(rt->rt_reply);
			rt->rt_reply = NULL;
			goto drop_on_floor;
		}

		p = rpc_encode_uint32_t(rt, p, 0);
		if (!p) {
			free(rt->rt_reply);
			rt->rt_reply = NULL;
			goto drop_on_floor;
		}

		p = rpc_encode_uint32_t(rt, p, 0);
		if (!p) {
			free(rt->rt_reply);
			rt->rt_reply = NULL;
			goto drop_on_floor;
		}

		p = rpc_encode_uint32_t(rt, p, rt->rt_info.ri_accept_stat);
		if (!p) {
			free(rt->rt_reply);
			rt->rt_reply = NULL;
			goto drop_on_floor;
		}
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

		rt->rt_reply_len = 7 * sizeof(uint32_t) + xdr_size;
		msg_len = rt->rt_reply_len - sizeof(uint32_t);
		rt->rt_reply = calloc(rt->rt_reply_len, sizeof(char));
		if (!rt->rt_reply) {
			goto drop_on_floor;
		}

		p = (uint32_t *)rt->rt_reply;

		p = rpc_encode_uint32_t(rt, p, msg_len | 0x80000000);
		if (!p) {
			rt->rt_info.ri_accept_stat = SYSTEM_ERR;
			__atomic_fetch_add(&rph->rph_accepted_errors, 1,
					   __ATOMIC_RELAXED);
			free(rt->rt_reply);
			rt->rt_reply = NULL;
			goto handle_rpc_error;
		}

		p = rpc_encode_uint32_t(rt, p, rt->rt_info.ri_xid);
		if (!p) {
			rt->rt_info.ri_accept_stat = SYSTEM_ERR;
			__atomic_fetch_add(&rph->rph_accepted_errors, 1,
					   __ATOMIC_RELAXED);
			free(rt->rt_reply);
			rt->rt_reply = NULL;
			goto handle_rpc_error;
		}

		p = rpc_encode_uint32_t(rt, p, 1);
		if (!p) {
			rt->rt_info.ri_accept_stat = SYSTEM_ERR;
			__atomic_fetch_add(&rph->rph_accepted_errors, 1,
					   __ATOMIC_RELAXED);
			free(rt->rt_reply);
			rt->rt_reply = NULL;
			goto handle_rpc_error;
		}

		p = rpc_encode_uint32_t(rt, p, 0);
		if (!p) {
			rt->rt_info.ri_accept_stat = SYSTEM_ERR;
			__atomic_fetch_add(&rph->rph_accepted_errors, 1,
					   __ATOMIC_RELAXED);
			free(rt->rt_reply);
			rt->rt_reply = NULL;
			goto handle_rpc_error;
		}

		p = rpc_encode_uint32_t(rt, p, 0);
		if (!p) {
			rt->rt_info.ri_accept_stat = SYSTEM_ERR;
			__atomic_fetch_add(&rph->rph_accepted_errors, 1,
					   __ATOMIC_RELAXED);
			free(rt->rt_reply);
			rt->rt_reply = NULL;
			goto handle_rpc_error;
		}

		p = rpc_encode_uint32_t(rt, p, 0);
		if (!p) {
			rt->rt_info.ri_accept_stat = SYSTEM_ERR;
			__atomic_fetch_add(&rph->rph_accepted_errors, 1,
					   __ATOMIC_RELAXED);
			free(rt->rt_reply);
			rt->rt_reply = NULL;
			goto handle_rpc_error;
		}

		p = rpc_encode_uint32_t(rt, p, 0);
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

		assert(rt->rt_offset == rt->rt_reply_len);
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
