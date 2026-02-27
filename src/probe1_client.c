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
#include "reffs/trace/common.h"

// Global flag for clean shutdown
volatile sig_atomic_t running = 1;

// Signal handler
static void signal_handler(int sig)
{
	TRACE("Received signal %d, initiating shutdown...", sig);

	__atomic_store(&running, &(int){ 0 }, __ATOMIC_SEQ_CST);

	// Wake up any waiting worker threads
	wake_worker_threads();
}

static void usage(const char *prog)
{
	printf("Usage: %s [options]\n", prog);
	printf("Options:\n");
	printf("  -h  --help                   Print this usage and exit\n");
	printf("  -f  --file=fname             Save tracing data to this file \"fname\"\n");
	printf("  -o  --op=op                  Perform the \"op\"\n");
	printf("  -g  --program=pgm            Probe this program \"pgm\"\n");
	printf("  -v  --version=v              Probe this program version \"vers\"\n");
	printf("  -p  --port=port              Connect to server at the \"port\"\n");
	printf("  -s  --server=server          Connect to server at the \"server\"\n");
	printf("  -c  --category=cat           Enable tracing for a category\n");
	printf("                                     0 - General\n");
	printf("                                     1 - IO\n");
	printf("                                     2 - RPC\n");
	printf("                                     3 - NFS\n");
	printf("                                     4 - FS\n");
}

static struct option long_opts[] = {
	{ "category", required_argument, 0, 'c' },
	{ "file", required_argument, 0, 'f' },
	{ "program", required_argument, 0, 'g' },
	{ "op", required_argument, 0, 'o' },
	{ "port", required_argument, 0, 'p' },
	{ "server", required_argument, 0, 's' },
	{ "version", required_argument, 0, 'v' },
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
	int program = 100003;
	int version = 3;

	struct rpc_trans *rt = NULL;

	struct ring_context rc;

	char *trace_file = "./probe1_clnt.log";

	char *op = "gather";

	// Initialize userspace RCU
	rcu_init();

	char *opts = "hc:f:p:o:s:g:v:";

	while ((opt = getopt_long(argc, argv, opts, long_opts, NULL)) != -1) {
		switch (opt) {
		case 'c': {
			int tracing = atoi(optarg);
			if (tracing > 0 && tracing < REFFS_TRACE_CAT_ALL) {
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
		case 'g':
			program = atoi(optarg);
			break;
		case 'v':
			version = atoi(optarg);
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

	// Ignore SIGPIPE
	signal(SIGPIPE, SIG_IGN);

	// Setup signal handlers
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = signal_handler;
	sa.sa_flags = SA_RESTART; // Restart interrupted system calls
	sigemptyset(&sa.sa_mask);
	sigaddset(&sa.sa_mask, SIGINT);
	sigaddset(&sa.sa_mask, SIGTERM);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);
	// Block signals in main thread temporarily
	pthread_sigmask(SIG_BLOCK, &mask, NULL);

	// Initialize IO handler
	if (io_handler_init(&rc) < 0) {
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
		rt = probe1_client_op_stats_gather(program, version);
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

	rt->rt_rc = &rc;

	ret = io_send_request(rt);
	if (ret) {
		rpc_protocol_free(rt);
		goto done;
	}

	pthread_sigmask(SIG_UNBLOCK, &mask, NULL);

	__atomic_store(&running, &(int){ 1 }, __ATOMIC_SEQ_CST);

	// Run the main IO processing loop
	io_handler_main_loop(&running, &rc);

	TRACE("Main loop exited, cleaning up...");

	io_handler_fini(&rc);
	io_uring_queue_exit(&rc.rc_ring);

done:
	running = 0;
	wake_worker_threads();

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
