/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_JEMALLOC
#include <jemalloc/jemalloc.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <getopt.h>
#include <fcntl.h>
#include <pthread.h>
#include <errno.h>
#include <urcu.h>

#include "probe1_xdr.h"

#include "reffs/log.h"
#include "reffs/rpc.h"
#include "reffs/probe1.h"
#include "reffs/server.h"
#include "reffs/io.h"
#include "reffs/trace/trace.h"

// Global flag for clean shutdown
volatile sig_atomic_t running = 1;

static void usage(const char *prog)
{
	printf("Usage: %s [options]\n", prog);
	printf("Options:\n");
	printf("  -h  --help                   Print this usage and exit\n");
	printf("  -f  --file=fname             Save tracing data to this file \"fname\"\n");
	printf("  -o  --op=op                  Perform the \"op\"\n");
	printf("  -p  --port=port              Connect to server at the \"port\"\n");
	printf("  -s  --server=server          Connect to server at the \"server\"\n");
	printf("  -c  --category=cat           Enable tracing for a category");
	printf("                                     0 - General");
	printf("                                     1 - IO");
	printf("                                     2 - RPC");
	printf("                                     3 - NFS");
	printf("                                     4 - FS");
}

static struct option long_opts[] = {
	{ "category", required_argument, 0, 'c' },
	{ "file", required_argument, 0, 'f' },
	{ "port", required_argument, 0, 'p' },
	{ "server", required_argument, 0, 's' },
	{ "help", no_argument, 0, 'h' },
	{ NULL, 0, NULL, 0 },
};

int main(int argc, char *argv[])
{
	int exit_code = 0;
	int port = PROBE_PORT;
	char *server = "127.0.0.1";
	int opt;
	int ret;

	struct rpc_trans *rt = NULL;

	struct io_uring ring;

	char *trace_file = "/tmp/reffs_probe1_clnt.log";

	char *op = "gather";

	// Initialize userspace RCU
	rcu_init();

	char *opts = "hc:f:p:o:s:";

	while ((opt = getopt_long(argc, argv, opts, long_opts, NULL)) != -1) {
		switch (opt) {
		case 'c': {
			int tracing = atoi(optarg);
			if (tracing > 0 && tracing < REFFS_TRACE_CAT_MAX) {
				reffs_trace_enable_category(tracing);
			}
			break;
		}
		case 'f':
			trace_file = optarg;
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		case 'o':
			op = optarg;
			break;
		case 'p':
			port = atoi(optarg);
			break;
		case 's':
			server = optarg;
			break;
		}
	}

	setvbuf(stdout, NULL, _IOLBF, 0);
	reffs_trace_init(trace_file);

	// Initialize IO handler
	if (io_handler_init(&ring) < 0) {
		return 1;
	}

	if (probe1_protocol_register()) {
		exit_code = 1;
		goto out;
	}

	if (create_worker_threads(&running) < 0) {
                exit_code = 1;
                goto out;
        }

	if (!strcmp(op, "gather")) {
		rt = probe1_client_op_stats_gather(PROBE_PROGRAM, PROBE_V1);
	} else if (!strcmp(op, "context")) {
		rt = probe1_client_op_context();
	} else if (!strcmp(op, "null")) {
		rt = probe1_client_op_null();
	} else {
		LOG("op = \"%s\" is not supported", op);
	}

	if (!rt)
		goto done;

	rt->rt_port = port;
	rt->rt_addr_str = server;

	ret = rpc_prepare_send_call(rt);
	if (ret)
		goto done;

	while (running) {
		usleep(100);
	}

	close(rt->rt_fd);
	rpc_protocol_free(rt);

done:
	running = 0;
	wake_worker_threads();

	// Clean up IO handler
	io_handler_cleanup(&ring);

	wait_for_worker_threads();
out:
	// Wait for RCU grace period
	TRACE("Calling rcu_barrier()...");
	rcu_barrier();

	// Let inodes clear out of memory
	TRACE("Calling rcu_barrier()...");
	rcu_barrier();

	probe1_protocol_deregister();

	LOG("Shutdown complete");
	return exit_code;
}
