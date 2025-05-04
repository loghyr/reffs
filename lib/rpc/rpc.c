/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <unistd.h>
#include <time.h>
#include <rpc/xdr.h>
#include <rpc/auth.h>
#include <rpc/auth_unix.h>
#include <rpc/clnt.h>
#include <rpc/rpc_msg.h>
#include <errno.h>

#include "reffs/test.h"

#include <rpc/clnt.h>
#include <rpc/rpc_msg.h>
#include <rpc/pmap_clnt.h>

#include "reffs/log.h"
#include "reffs/rpc.h"
#include "reffs/io.h"
#include "reffs/network.h"
#include "reffs/task.h"
#include "reffs/trace/rpc.h"

CDS_LIST_HEAD(rpc_program_handler_list);

static void rpc_program_handler_free_rcu(struct rcu_head *rcu)
{
	struct rpc_program_handler *rph =
		caa_container_of(rcu, struct rpc_program_handler, rph_rcu);

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
			  struct rpc_operations_handler *ops, size_t ops_len)
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
	rph->rph_ops = ops;
	rph->rph_ops_len = ops_len;

	urcu_ref_init(&rph->rph_ref);
	__atomic_fetch_or(&rph->rph_flags, RPH_IN_LIST, __ATOMIC_RELEASE);
	cds_list_add_rcu(&rph->rph_list, &rpc_program_handler_list);

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

static int rpc_parse_call_data(struct rpc_trans *rt)
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

	if (!ph->ph_op_handler->roh_args_f(&xdrs, ph->ph_args)) {
		xdr_destroy(&xdrs);
		return EINVAL;
	}

	end_pos = xdr_getpos(&xdrs);

	len = end_pos - start_pos;

	xdr_destroy(&xdrs);

	rt->rt_offset += len;

	return 0;
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

static void update_max_duration_rcu(uint64_t duration_ns,
				    struct protocol_handler *ph)
{
	uint64_t old_max;

	__atomic_load(&ph->ph_op_handler->roh_duration_max, &old_max,
		      __ATOMIC_RELAXED);

	if (duration_ns > old_max) {
		// Keep trying until either we succeed or someone else updates to larger value
		while (1) {
			uint64_t expected = old_max;
			uint64_t desired = duration_ns;
			bool success = __atomic_compare_exchange(
				&ph->ph_op_handler->roh_duration_max, &expected,
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

int rpc_protocol_op_call(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	int ret = 0;

	if (ph->ph_op_handler && ph->ph_op_handler->roh_action) {
		struct timespec start, end;
		uint64_t duration_ns;

		clock_gettime(CLOCK_MONOTONIC, &start);
		ret = ph->ph_op_handler->roh_action(rt);
		clock_gettime(CLOCK_MONOTONIC, &end);

		duration_ns = (end.tv_sec - start.tv_sec) * 1000000000ULL +
			      (end.tv_nsec - start.tv_nsec);

		uint64_t calls = __atomic_add_fetch(
			&ph->ph_op_handler->roh_calls, 1, __ATOMIC_RELAXED);
		uint64_t duration_total = __atomic_add_fetch(
			&ph->ph_op_handler->roh_duration_total, duration_ns,
			__ATOMIC_RELAXED);
		if (ret)
			__atomic_fetch_add(&ph->ph_op_handler->roh_fails, 1,
					   __ATOMIC_RELAXED);

		update_max_duration_rcu(duration_ns, ph);

		uint64_t avg_duration = duration_total / calls;

		trace_rpc_duration(rt, duration_ns, avg_duration);
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
		free(ph);
	}

	rpc_program_handler_put(rt->rt_rph);

	free(rt->rt_reply);
	free(rt);
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

	rt->rt_reply_len = 7 * sizeof(uint32_t) + xdr_size;
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

	p = rpc_encode_uint32_t(rt, p, 1);
	if (!p) {
		goto drop_on_floor;
	}

	p = rpc_encode_uint32_t(rt, p, 0);
	if (!p) {
		goto drop_on_floor;
	}

	p = rpc_encode_uint32_t(rt, p, 0);
	if (!p) {
		goto drop_on_floor;
	}

	p = rpc_encode_uint32_t(rt, p, 0);
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

		if (!ph->ph_op_handler->roh_args_f(&xdrs, ph->ph_args)) {
			xdr_destroy(&xdrs);
			goto drop_on_floor;
		}

		end_pos = xdr_getpos(&xdrs);

		len = end_pos - start_pos;

		xdr_destroy(&xdrs);

		rt->rt_offset += len;
	}

	assert(rt->rt_offset == rt->rt_reply_len);

	return 0;

drop_on_floor:
	free(rt->rt_reply);
	rt->rt_reply = NULL;
	return EINVAL;
}

int rpc_process_task(struct task *t)
{
	u_long msg_len = 0;
	int ret = 0;

	uint32_t *p;

	if (!t)
		return EINVAL;

	if (t->t_bytes_read < (int)(2 * sizeof(uint32_t)))
		return 0;

	struct rpc_trans *rt = rpc_trans_create_from_task(t);
	if (!rt)
		return ENOMEM;

	struct rpc_program_handler *rph = NULL;

	p = (uint32_t *)rt->rt_body;

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
		struct rpc_trans *rt_old =
			io_find_request_by_xid(rt->rt_info.ri_xid);
		if (!rt_old)
			goto drop_on_floor;

		io_unregister_request(rt->rt_info.ri_xid);
		rt->rt_ring = rt_old->rt_ring;
		rt->rt_cb = rt_old->rt_cb;

		// Caller is responsible for releasing rt_old
		// Also, should share the args and res between the two!
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

	switch (rt->rt_info.ri_cred.rc_flavor) {
	case AUTH_NONE: {
		uint32_t len;
		p = rpc_decode_uint32_t(rt, p, &len);
		if (!p) {
			rt->rt_info.ri_accept_stat = GARBAGE_ARGS;
			__atomic_fetch_add(&rph->rph_accepted_errors, 1,
					   __ATOMIC_RELAXED);
			goto handle_rpc_error;
		}
		break;
	}
	case AUTH_SYS: {
		XDR xdrs = { 0 };

		uint32_t len;
		p = rpc_decode_uint32_t(rt, p, &len);
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
		rt->rt_offset += len;
		p = (uint32_t *)(p + len / sizeof(uint32_t));
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

	switch (rt->rt_info.ri_verifier_flavor) {
	case AUTH_NONE: {
		uint32_t len;
		p = rpc_decode_uint32_t(rt, p, &len);
		if (!p) {
			rt->rt_info.ri_accept_stat = GARBAGE_ARGS;
			__atomic_fetch_add(&rph->rph_accepted_errors, 1,
					   __ATOMIC_RELAXED);
			goto handle_rpc_error;
		}
		break;
	}
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

			if (!ph->ph_op_handler->roh_res_f(&xdrs, ph->ph_res)) {
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
		rt->rt_ring = t->t_ring;
		rt->rt_cb(rt);
	}

drop_on_floor:
	rpc_program_handler_put(rph);
	rpc_protocol_free(rt);

	return 0;
}
