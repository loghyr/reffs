/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#include "sm_inter.h"
#include "reffs/log.h"
#include "reffs/rpc.h"
#include "reffs/nsm.h"
#include "reffs/nlm_lock.h"

static int sm_state = 1;

struct monitored_host {
	struct cds_list_head mh_list;
	char *mh_name;
	struct mon_id mh_id;
	char mh_priv[16];
};

static CDS_LIST_HEAD(monitored_hosts);
static pthread_mutex_t nsm_mutex = PTHREAD_MUTEX_INITIALIZER;

static void sm_load_state(void)
{
	FILE *fp = fopen("statd.state", "r+");
	if (fp) {
		if (fscanf(fp, "%d", &sm_state) != 1) {
			sm_state = 1;
		}
		sm_state +=
			2; /* Increment state by 2 as per convention (odd numbers = up) */
		rewind(fp);
		fprintf(fp, "%d\n", sm_state);
		fclose(fp);
	} else {
		fp = fopen("statd.state", "w");
		if (fp) {
			sm_state = 1;
			fprintf(fp, "%d\n", sm_state);
			fclose(fp);
		}
	}
	TRACE("NSM: Local state is %d", sm_state);
}

int sm_get_state(void)
{
	return sm_state;
}

static int sm_op_null(struct rpc_trans __attribute__((unused)) *rt)
{
	TRACE("NSM: NULL called");
	return 0;
}

static int sm_op_stat(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	struct sm_name *args = ph->ph_args;
	struct sm_stat_res *res = ph->ph_res;

	TRACE("NSM: STAT called for %s", args->mon_name);

	res->res_stat = stat_succ;
	res->state = sm_state;
	return 0;
}

static int sm_op_mon(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	struct mon *args = ph->ph_args;
	struct sm_stat_res *res = ph->ph_res;
	struct monitored_host *mh;
	bool found = false;

	TRACE("NSM: MON called for %s", args->mon_id.mon_name);

	pthread_mutex_lock(&nsm_mutex);
	cds_list_for_each_entry(mh, &monitored_hosts, mh_list) {
		if (strcmp(mh->mh_name, args->mon_id.mon_name) == 0) {
			found = true;
			break;
		}
	}

	if (!found) {
		mh = calloc(1, sizeof(*mh));
		if (mh) {
			mh->mh_name = strdup(args->mon_id.mon_name);
			/* We should also copy the rest of mon_id and priv if needed for notification */
			cds_list_add(&mh->mh_list, &monitored_hosts);
		}
	}
	pthread_mutex_unlock(&nsm_mutex);

	res->res_stat = stat_succ;
	res->state = sm_state;
	return 0;
}

static int sm_op_unmon(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	struct mon_id *args = ph->ph_args;
	struct sm_stat *res = ph->ph_res;
	struct monitored_host *mh, *tmp;

	TRACE("NSM: UNMON called for %s", args->mon_name);

	pthread_mutex_lock(&nsm_mutex);
	cds_list_for_each_entry_safe(mh, tmp, &monitored_hosts, mh_list) {
		if (strcmp(mh->mh_name, args->mon_name) == 0) {
			cds_list_del(&mh->mh_list);
			free(mh->mh_name);
			free(mh);
		}
	}
	pthread_mutex_unlock(&nsm_mutex);

	res->state = sm_state;
	return 0;
}

static int sm_op_unmon_all(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	struct sm_stat *res = ph->ph_res;
	struct monitored_host *mh, *tmp;

	TRACE("NSM: UNMON_ALL called");

	pthread_mutex_lock(&nsm_mutex);
	cds_list_for_each_entry_safe(mh, tmp, &monitored_hosts, mh_list) {
		cds_list_del(&mh->mh_list);
		free(mh->mh_name);
		free(mh);
	}
	pthread_mutex_unlock(&nsm_mutex);

	res->state = sm_state;
	return 0;
}

static int sm_op_simu_crash(struct rpc_trans __attribute__((unused)) *rt)
{
	TRACE("NSM: SM_SIMU_CRASH received. Re-initializing state.");
	sm_load_state();
	/* A real simulation should notify all monitored hosts */
	return 0;
}

static int sm_op_notify(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	struct status *args = ph->ph_args;
	struct nlm4_notify nlm_args;

	TRACE("NSM: Received NOTIFY from %s (state %d)", args->mon_name,
	      args->state);

	/* Tell NLM to free all locks for this host */
	nlm_args.name = args->mon_name;
	nlm_args.state = args->state;
	reffs_nlm4_free_all(&nlm_args);

	return 0;
}

static struct rpc_operations_handler sm_operations_handler[] = {
	RPC_OPERATION_INIT(SM, PROC_NULL, NULL, NULL, NULL, NULL, sm_op_null),
	RPC_OPERATION_INIT(SM, STAT, xdr_sm_name, struct sm_name,

			   xdr_sm_stat_res, struct sm_stat_res, sm_op_stat),
	RPC_OPERATION_INIT(SM, MON, xdr_mon, struct mon, xdr_sm_stat_res,
			   struct sm_stat_res, sm_op_mon),
	RPC_OPERATION_INIT(SM, UNMON, xdr_mon_id, struct mon_id, xdr_sm_stat,
			   struct sm_stat, sm_op_unmon),
	RPC_OPERATION_INIT(SM, UNMON_ALL, xdr_my_id, struct my_id, xdr_sm_stat,
			   struct sm_stat, sm_op_unmon_all),
	RPC_OPERATION_INIT(SM, SIMU_CRASH, NULL, NULL, NULL, NULL,
			   sm_op_simu_crash),
	RPC_OPERATION_INIT(SM, NOTIFY, xdr_status, struct status, NULL, NULL,
			   sm_op_notify),
};

static struct rpc_program_handler *sm_handler;
static volatile sig_atomic_t sm_registered = 0;

int sm_protocol_register(void)
{
	if (sm_registered)
		return 0;

	sm_load_state();

	sm_handler = rpc_program_handler_alloc(
		SM_PROG, SM_VERS, sm_operations_handler,
		sizeof(sm_operations_handler) / sizeof(*sm_operations_handler));

	if (!sm_handler)
		return ENOMEM;

	sm_registered = 1;
	return 0;
}

int sm_protocol_deregister(void)
{
	if (!sm_registered)
		return 0;

	rpc_program_handler_put(sm_handler);
	sm_handler = NULL;
	sm_registered = 0;
	return 0;
}
