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

#ifdef NOT_NOW

static int nfs3_getattr(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;

	XDR xdrs = { 0 };

	//GETATTR3res *res = ph->ph_res;
	GETATTR3args *args = ph->ph_args;

	void *data;
	xdrproc_t f;

	size_t len;

	uint32_t *p = (uint32_t *)(rt->rt_body + rt->rt_offset);

	uint32_t start_pos, end_pos;

	LOG("GETATTR");

	if (rt->rt_info.ri_type == 0) {
		data = ph->ph_args;
		f = ph->ph_op_handler->roh_args_f;
	} else {
		data = ph->ph_res;
		f = ph->ph_op_handler->roh_res_f;
	}

	xdrmem_create(&xdrs, (char *)p, rt->rt_len - rt->rt_offset, XDR_DECODE);

	start_pos = xdr_getpos(&xdrs);

	if (!f(&xdrs, data)) {
		xdr_destroy(&xdrs);
		return -1;
	}

	end_pos = xdr_getpos(&xdrs);

	len = end_pos - start_pos;

	xdr_destroy(&xdrs);

	rt->rt_offset += len;
	p = (uint32_t *)(p + len / sizeof(uint32_t));

	if (rt->rt_info.ri_type == 0) {
		print_nfs_fh3_hex(&args->object);
	} else {
	}

	printf("There are %lu bytes remaining\n", rt->rt_len - rt->rt_offset);
	return 0;
}

#endif

int rpc_protocol_allocate(struct rpc_trans *rt, struct rpc_program_handler *rph)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;

	for (size_t i = 0; i < rph->rph_ops_len; i++) {
		if (rph->rph_ops[i].roh_operation == rt->rt_info.ri_procedure) {
			if (!rph->rph_ops[i].roh_action)
				return 0;

			ph->ph_op_handler = &rph->rph_ops[i];

			ph->ph_args = calloc(1, rph->rph_ops[i].roh_args_size);
			if (!ph->ph_args)
				return ENOMEM;
			ph->ph_res = calloc(1, rph->rph_ops[i].roh_res_size);
			if (!ph->ph_res) {
				free(ph->ph_args);
				ph->ph_args = NULL;
				return ENOMEM;
			}

			return 0;
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
