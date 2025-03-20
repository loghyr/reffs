/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <netconfig.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <getopt.h>
#include <assert.h>
#include <urcu.h>
#include <urcu/rculist.h>
#include <urcu/wfcqueue.h>
#include <urcu/ref.h>
#include "reffs/log.h"
#include "reffs_echo_xdr.h"

#include <netconfig.h>
#include <rpc/rpcb_clnt.h>
#include <rpc/nettype.h>

static int safe_gets(char *buf, size_t size)
{
	if (fgets(buf, size, stdin) != NULL) {
		size_t len = strlen(buf);
		if (len > 0 && buf[len - 1] == '\n') {
			buf[len - 1] = '\0';
		}
		return 0;
	} else {
		return 1;
	}
}

static void usage(const char *me)
{
	fprintf(stdout, "Usage: %s [options]\n", me);
	fprintf(stdout, "Options:\n");
	fprintf(stdout, " -h  --help         Show help\n");
	fprintf(stdout, " -p  --port         Port number\n");
	fprintf(stdout, " -s  --server       Server name\n");
}

static struct option options[] = {
	{ "help", no_argument, 0, 'h' },
	{ "port", required_argument, 0, 'p' },
	{ NULL, 0, NULL, 0 },
};

#ifndef NGROUPS_MAX
#define NGROUPS_MAX (16)
#endif

#define HOSTNAME_MAX (256)
#define MSG_MAX (1024)

int main(int argc, char *argv[])
{
	int ret;

	// int i;

	unsigned short port = RE_PORT;

	char host_name[HOSTNAME_MAX];
	char server_name[HOSTNAME_MAX] = "adept";
	gid_t groups[NGROUPS_MAX];
	uid_t uid;
	gid_t gid;
	int num_groups;

	pid_t pid = getpid();

	int opt;

	enum clnt_stat stat;

	CLIENT *clnt;
	struct timeval tv = { .tv_sec = 30 };

	struct netconfig *nconf;
	struct netbuf svcaddr;

	char buf[MSG_MAX];
	int len;

	re_message1_args *args;
	re_message1_res *res;

	while ((opt = getopt_long(argc, argv, "hp:s:", options, NULL)) != -1) {
		switch (opt) {
		case 'p':
			port = atoi(optarg);
			break;
		case 's':
			strncpy(server_name, optarg, HOSTNAME_MAX - 1);
			break;
		case 'h':
			usage(argv[0]);
			exit(1);
		}
	}

	uid = geteuid();
	LOG("uid = %d", uid);
	gid = getegid();
	LOG("gid = %d", gid);

	num_groups = getgroups(NGROUPS_MAX, groups);
	if (num_groups < 0) {
		ret = errno;
		LOG("getgroups() failed: %d", ret);
		return 1;
	}

	ret = gethostname(host_name, HOSTNAME_MAX);
	if (ret < 0) {
		ret = errno;
		LOG("gethost_name() failed: %d", ret);
		sprintf(host_name, "reffs_cl_%d_%d", pid, port);
	}

	LOG("host_name = %s", host_name);

	nconf = getnetconfigent(NC_INET6);
	if (!nconf) {
		ret = errno;
		LOG("getnetconfigent() failed: %d", ret);
		return 1;
	}

	svcaddr.maxlen = 16;
	svcaddr.buf = calloc(1, svcaddr.maxlen);
	if (!svcaddr.buf) {
		LOG("Could not alloc");
		return 1;
	}

	if (!rpcb_getaddr(RE_ADMIN_PROGRAM, RE_ADMIN_V1, nconf, &svcaddr,
			  server_name)) {
		ret = errno;
		LOG("Could not rpcb_getaddr() with clnt_stat=%d: %d",
		    rpc_createerr.cf_stat, ret);
		return 1;
	}

	clnt = clnt_tli_create(RPC_ANYFD, nconf, &svcaddr, RE_ADMIN_PROGRAM,
			       RE_ADMIN_V1, 0, 0);
	if (!clnt) {
		ret = errno;
		LOG("Could not clnt_tli_create() with clnt_stat=%d: %d",
		    rpc_createerr.cf_stat, ret);
		return 1;
	}

	clnt->cl_auth = authsys_create(host_name, uid, gid, num_groups, groups);
	if (!clnt->cl_auth) {
		ret = errno;
		LOG("authsys_create() failed: %d", ret);
		return 1;
	}

	while (1) {
		// Get the user input
		memset(&buf, '\0', MSG_MAX);
		ret = safe_gets(buf, MSG_MAX);
		if (ret) {
			ret = errno;
			LOG("scanf() failed: %d", ret);
			return 1;
		}

		len = strlen(buf);
		if (!strncmp(buf, "done", len))
			break;

		args = calloc(1, sizeof(*args));
		if (!args) {
			LOG("Could not alloc");
			return 1;
		}

		res = calloc(1, sizeof(*res));
		if (!args) {
			LOG("Could not alloc");
			return 1;
		}

		args->rma_message.re_message_string1_len = len;
		args->rma_message.re_message_string1_val = calloc(1, len + 1);
		if (!args->rma_message.re_message_string1_val) {
			LOG("Could not alloc");
			return 1;
		}

		strcpy(args->rma_message.re_message_string1_val, buf);

		stat = clnt_call(clnt, RE_PROC1_MESSAGE,
				 (xdrproc_t)xdr_re_message1_args, (char *)args,
				 (xdrproc_t)xdr_re_message1_res, (char *)res,
				 tv);
		if (stat != RPC_SUCCESS) {
			ret = errno;
			LOG("clnt_call() failed with clnt_stat=%d: %d", stat,
			    ret);
			return 1;
		}

		switch (res->rmr_status) {
		case RE1_OK:
			assert(res->re_message1_res_u.rmr_reply
				       .re_message_string1_len < MSG_MAX);
			memset(&buf, '\0', MSG_MAX);
			strncpy(buf,
				res->re_message1_res_u.rmr_reply
					.re_message_string1_val,
				res->re_message1_res_u.rmr_reply
					.re_message_string1_len);
			LOG("%s", buf);
			break;
		default:
			LOG("Got an error of %d", res->rmr_status);
			break;
		}

		free(args->rma_message.re_message_string1_val);
		free(args);
		free(res->re_message1_res_u.rmr_reply.re_message_string1_val);
		free(res);
	}

	freenetconfigent(nconf);
	free(svcaddr.buf);
	clnt_destroy(clnt);

	return 0;
}
