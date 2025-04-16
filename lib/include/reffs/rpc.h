/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifndef _REFFS_RPC_H
#define _REFFS_RPC_H

#include <unistd.h>
#include <rpc/xdr.h>
#include <rpc/auth.h>
#include <rpc/auth_unix.h>

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
	struct rpc_cred ri_cred;
	uint32_t ri_verifier_flavor;
	enum auth_stat ri_stat;
};

struct rpc_trans {
	struct rpc_info rt_info; // The RPC Header
	char *rt_body; // The raw RPC payload
	size_t rt_len; // The length of the payload
	size_t rt_offset; // Current offset to be parsed
	void *rt_context; // Protocol specific context
};

/*
 * The per protocol operation handler.
 */
struct rpc_operations_handler {
	uint32_t roh_operation; // The operation ID
	xdrproc_t roh_args_f; // The function to process the args
	size_t roh_args_size; // The size of the base args structure
	xdrproc_t roh_res_f; // The function to process the res
	size_t roh_res_size; // The size of the base res structure
	int (*roh_action)(
		struct rpc_trans *rt); // The protocol handler for calls
};

/*
 * The protocol handler.
 */
struct rpc_program_handler {
	uint32_t rph_program; // Which program?
	uint32_t rph_version; // Which version?
	const struct rpc_operations_handler *rph_ops; // Array of operations
	size_t rph_ops_len; // Length of operations array
};

/*
 * Typically stuffed into the rt_context of a struct rpc_trans.
 */
struct protocol_handler {
	struct rpc_cred *ph_cred; // A pointer to the credential
	void *ph_args; // The base args
	void *ph_res; // The base res
	int ph_stat; // Protocol error code
	const struct rpc_operations_handler *ph_op_handler;
};

#define RPC_OPERATION_INIT(OP, ARGS_F, ARGS, RES_F, RES, CALL) \
	{ .roh_operation = OP,                                 \
	  .roh_args_f = (xdrproc_t)ARGS_F,                     \
	  .roh_args_size = sizeof(ARGS),                       \
	  .roh_res_f = (xdrproc_t)RES_F,                       \
	  .roh_res_size = sizeof(RES),                         \
	  .roh_action = CALL }

static inline uint32_t *decode_uint32_t(struct rpc_trans *rt, uint32_t *p,
					uint32_t *dst)
{
	if (rt->rt_offset + sizeof(uint32_t) <= rt->rt_len) {
		*dst = ntohl(*p);
		rt->rt_offset += sizeof(uint32_t);
	} else {
		return NULL;
	}

	return ++p;
}

int rpc_protocol_allocate(struct rpc_trans *rt,
			  struct rpc_program_handler *rph);
void rpc_protocol_free(struct rpc_trans *rt);
int rpc_protocol_op_call(struct rpc_trans *rt);

#endif
