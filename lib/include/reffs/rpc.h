/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifndef _REFFS_RPC_H
#define _REFFS_RPC_H

#include <unistd.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <rpc/xdr.h>
#include <rpc/auth.h>
#include <rpc/auth_unix.h>
#include <rpc/clnt.h>
#include <rpc/rpc_msg.h>

#include <urcu.h>
#include <urcu/rculist.h>
#include <urcu/ref.h>

#include <time.h>

#include <liburing.h>

#include "reffs/network.h"
#include "reffs/log.h"
#include "reffs/task.h"

struct rpc_cred {
	uint32_t rc_flavor;
	union {
		struct authunix_parms rc_unix;
	};
};

struct rpc_info {
	uint32_t ri_xid;
	uint32_t ri_type;
	uint32_t ri_rpc_version;
	uint32_t ri_program;
	uint32_t ri_version;
	uint32_t ri_procedure;
	struct connection_info ri_ci;
	struct rpc_cred ri_cred;
	uint32_t ri_verifier_flavor;

	enum reply_stat ri_reply_stat;
	enum reject_stat ri_reject_stat;
	enum accept_stat ri_accept_stat;
	enum auth_stat ri_auth_stat;
};

struct rpc_program_handler;

struct rpc_trans {
	struct rpc_info rt_info; // The RPC Header
	int rt_fd; // For sending responses
	char *rt_body; // The raw RPC payload
	size_t rt_body_len; // The length of the payload
	size_t rt_offset; // Current offset to be parsed
	char *rt_reply; // The raw RPC payload
	size_t rt_reply_len; // The length of the payload
	void *rt_context; // Protocol specific context
	struct io_uring *rt_ring;
	int (*rt_cb)(struct rpc_trans *rt); // Callback function pointer
	struct rpc_program_handler *rt_rph;
};

/*
 * The per protocol operation handler.
 */
struct rpc_operations_handler {
	uint32_t roh_operation; // The operation ID
	const char *roh_name;
	xdrproc_t roh_args_f; // The function to process the args
	size_t roh_args_size; // The size of the base args structure
	xdrproc_t roh_res_f; // The function to process the res
	size_t roh_res_size; // The size of the base res structure
	int (*roh_action)(
		struct rpc_trans *rt); // The protocol handler for calls
	uint64_t roh_duration_max;
	uint64_t roh_duration_total;
	uint64_t roh_calls;
};

/*
 * The protocol handler.
 */
struct rpc_program_handler {
	uint32_t rph_program; // Which program?
	uint32_t rph_version; // Which version?
	struct rpc_operations_handler *rph_ops; // Array of operations
	size_t rph_ops_len; // Length of operations array
	struct rcu_head rph_rcu;
	struct urcu_ref rph_ref;
	struct cds_list_head rph_list;
#define RPH_IN_LIST (1 << 0)
	uint32_t rph_flags;
};

/*
 * Typically stuffed into the rt_context of a struct rpc_trans.
 */
struct protocol_handler {
	struct rpc_cred *ph_cred; // A pointer to the credential
	void *ph_args; // The base args
	void *ph_res; // The base res
	int ph_stat; // Protocol error code
	struct rpc_operations_handler *ph_op_handler;
};

#define RPC_OPERATION_INIT(PROTOCOL, NAME, ARGS_F, ARGS, RES_F, RES, CALL) \
	{ .roh_operation = PROTOCOL##_##NAME,                              \
	  .roh_name = #NAME,                                               \
	  .roh_args_f = (xdrproc_t)ARGS_F,                                 \
	  .roh_args_size = sizeof(ARGS),                                   \
	  .roh_res_f = (xdrproc_t)RES_F,                                   \
	  .roh_res_size = sizeof(RES),                                     \
	  .roh_action = CALL }

static inline uint32_t *rpc_decode_uint32_t(struct rpc_trans *rt, uint32_t *p,
					    uint32_t *dst)
{
	if (rt->rt_offset + sizeof(uint32_t) <= rt->rt_body_len) {
		*dst = ntohl(*p);
		rt->rt_offset += sizeof(uint32_t);
	} else {
		return NULL;
	}

	return ++p;
}

static inline uint32_t *rpc_encode_uint32_t(struct rpc_trans *rt, uint32_t *p,
					    uint32_t src)
{
	if (rt->rt_offset + sizeof(uint32_t) <= rt->rt_reply_len) {
		*p = htonl(src);
		rt->rt_offset += sizeof(uint32_t);
	} else {
		return NULL;
	}

	return ++p;
}

int rpc_protocol_allocate_call(struct rpc_trans *rt);
void rpc_protocol_free(struct rpc_trans *rt);
int rpc_protocol_op_call(struct rpc_trans *rt);

struct rpc_program_handler *
rpc_program_handler_alloc(uint32_t program, uint32_t version,
			  struct rpc_operations_handler *ops, size_t ops_len);

struct rpc_program_handler *rpc_program_handler_find(uint32_t program,
						     uint32_t version);

struct rpc_program_handler *
rpc_program_handler_get(struct rpc_program_handler *rph);
void rpc_program_handler_put(struct rpc_program_handler *rph);

int rpc_process_task(struct task *t);

#endif
