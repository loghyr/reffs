/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <unistd.h>
#include <rpc/xdr.h>
#include <rpc/auth.h>
#include <rpc/auth_unix.h>
#include <errno.h>
#include "reffs/test.h"
#include "reffs/rpc.h"

static int rpc_parse_call_data(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;

	XDR xdrs = { 0 };

	size_t len;

	uint32_t *p = (uint32_t *)(rt->rt_body + rt->rt_offset);

	uint32_t start_pos, end_pos;

	if (!ph->ph_op_handler->roh_args_f)
		return 0;

	xdrmem_create(&xdrs, (char *)p, rt->rt_len - rt->rt_offset, XDR_DECODE);

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

int rpc_protocol_allocate_call(struct rpc_trans *rt,
			       struct rpc_program_handler *rph)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;

	for (size_t i = 0; i < rph->rph_ops_len; i++) {
		if (rph->rph_ops[i].roh_operation == rt->rt_info.ri_procedure) {
			if (!rph->rph_ops[i].roh_action)
				return 0;

			ph->ph_op_handler = &rph->rph_ops[i];

			if (rph->rph_ops[i].roh_args_f &&
			    rph->rph_ops[i].roh_args_size) {
				ph->ph_args = calloc(
					1, rph->rph_ops[i].roh_args_size);
				if (!ph->ph_args)
					return ENOMEM;
			}

			if (rph->rph_ops[i].roh_res_f &&
			    rph->rph_ops[i].roh_res_size) {
				ph->ph_res =
					calloc(1, rph->rph_ops[i].roh_res_size);
				if (!ph->ph_res) {
					free(ph->ph_args);
					ph->ph_args = NULL;
					return ENOMEM;
				}
			}

			return rpc_parse_call_data(rt);
		}
	}

	return ENOENT;
}

int rpc_protocol_op_call(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	int ret = -1;

	if (ph->ph_op_handler->roh_action)
		ret = ph->ph_op_handler->roh_action(rt);

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
	default:
		break;
	}

	ph = (struct protocol_handler *)rt->rt_context;
	if (ph) {
		if (ph->ph_op_handler->roh_args_f) {
			xdr_free(ph->ph_op_handler->roh_args_f,
				 (char *)ph->ph_args);
		}

		if (ph->ph_op_handler->roh_res_f) {
			xdr_free(ph->ph_op_handler->roh_args_f,
				 (char *)ph->ph_res);
		}
		free(ph);
	}

	free(rt);
}
