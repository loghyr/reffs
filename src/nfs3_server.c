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
#include <signal.h>
#include <getopt.h>
#include <fcntl.h>
#include <pthread.h>
#include <errno.h>
#include <urcu.h>
#include <rpc/pmap_clnt.h>

#include "nfsv3_xdr.h"
#include "mntv3_xdr.h"

#include "reffs/log.h"
#include "reffs/rpc.h"
#include "reffs/nfs3.h"
#include "reffs/mount3.h"
#include "reffs/server.h"
#include "reffs/ns.h"
#include "reffs/io.h"

#define NFS_PORT 2049

// Global flag for clean shutdown
volatile sig_atomic_t running = 1;

// Signal handler
static void signal_handler(int sig)
{
	TRACE(REFFS_TRACE_LEVEL_ERR,
	      "Received signal %d, initiating shutdown...", sig);
	running = 0;

	// Wake up any waiting worker threads
	wake_worker_threads();
}

static void usage(const char *prog)
{
	printf("Usage: %s [options]\n", prog);
	printf("Options:\n");
	printf("  -h  --help                   Print this usage and exit\n");
	printf("  -p  --port=id                Serve NFS traffic from this \"port\"\n");
	printf("  -t  --tracing=lvl            Enable tracing at a level");
	printf("                                     0 - Debug");
	printf("                                     1 - Info");
	printf("                                     2 - Notice");
	printf("                                     3 - Warning");
	printf("                                     4 - Error");
	printf("                                     5 - Disabled");
	printf("  -a  --assembly=lvl           Enable packet assembly tracing at a level");
	printf("                                     0 - Debug");
	printf("                                     1 - Info");
	printf("                                     2 - Notice");
	printf("                                     3 - Warning");
	printf("                                     4 - Error");
	printf("                                     5 - Disabled");
	printf("  -f  --fragment=lvl           Enable write fragment tracing at a level");
	printf("                                     0 - Debug");
	printf("                                     1 - Info");
	printf("                                     2 - Notice");
	printf("                                     3 - Warning");
	printf("                                     4 - Error");
	printf("                                     5 - Disabled");
}

static struct option long_opts[] = {
	{ "help", no_argument, 0, 'h' },
	{ "port", required_argument, 0, 'p' },
	{ "tracing", required_argument, 0, 't' },
	{ "assembly", required_argument, 0, 'a' },
	{ "fragment", required_argument, 0, 'f' },
	{ NULL, 0, NULL, 0 },
};

int main(int argc, char *argv[])
{
	int listener_fd;
	int exit_code = 0;
	int port = NFS_PORT;
	int opt;

	struct io_uring ring;

	// Initialize userspace RCU
	rcu_init();

	server_boot_uuid_generate();

	while ((opt = getopt_long(argc, argv, "p:ht:a:f:", long_opts, NULL)) !=
	       -1) {
		switch (opt) {
		case 'p':
			port = atoi(optarg);
			break;
		case 'a': {
			int tracing = atoi(optarg);
			enum reffs_trace_level packet_assembly_trace = tracing;
			if (tracing < 0)
				packet_assembly_trace = REFFS_TRACE_LEVEL_DEBUG;
			else if (tracing > REFFS_TRACE_LEVEL_DISABLED)
				packet_assembly_trace =
					REFFS_TRACE_LEVEL_DISABLED;
			packet_assembly_trace_set(packet_assembly_trace);
			break;
		}
		case 'f': {
			int tracing = atoi(optarg);
			enum reffs_trace_level write_fragment_trace = tracing;
			if (tracing < 0)
				write_fragment_trace = REFFS_TRACE_LEVEL_DEBUG;
			else if (tracing > REFFS_TRACE_LEVEL_DISABLED)
				write_fragment_trace =
					REFFS_TRACE_LEVEL_DISABLED;
			write_fragment_trace_set(write_fragment_trace);
			break;
		}
		case 't': {
			int tracing = atoi(optarg);
			enum reffs_trace_level level = tracing;
			if (tracing < 0)
				level = REFFS_TRACE_LEVEL_DEBUG;
			else if (tracing > REFFS_TRACE_LEVEL_DISABLED)
				level = REFFS_TRACE_LEVEL_DISABLED;
			reffs_tracing_set(level);
			break;
		}
		case 'h':
			usage(argv[0]);
			return 0;
		}
	}

	setvbuf(stdout, NULL, _IOLBF, 0);

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
	if (io_handler_init(&ring) < 0) {
		return 1;
	}

	// Set up protocol handlers
	if (nfs3_protocol_register()) {
		exit_code = 1;
		goto out;
	}

	if (mount3_protocol_register()) {
		exit_code = 1;
		goto out;
	}

	exit_code = reffs_ns_init();
	if (exit_code)
		goto out;

	// Create worker threads
	if (create_worker_threads(&running) < 0) {
		exit_code = 1;
		goto out;
	}

	pthread_sigmask(SIG_UNBLOCK, &mask, NULL);

	// Setup NFS listener
	listener_fd = setup_listener(port);
	if (listener_fd < 0) {
		LOG("Failed to setup listener on port %d", port);
		exit_code = 1;
		goto out;
	}

	request_accept_op(listener_fd, NULL, &ring);

	if (!pmap_set(NFS3_PROGRAM, NFS_V3, IPPROTO_TCP, port)) {
		LOG("Failed to register with portmapper for NFSv3");
		exit_code = 1;
		goto out;
	}

	if (!pmap_set(MOUNT_PROGRAM, MOUNT_V3, IPPROTO_TCP, port)) {
		LOG("Failed to register with portmapper for MOUNTv3");
		pmap_unset(NFS3_PROGRAM, NFS_V3);
		exit_code = 1;
		goto out;
	}

	// Run the main IO processing loop
	io_handler_main_loop(&running, &ring);

	TRACE(REFFS_TRACE_LEVEL_WARNING, "Main loop exited, cleaning up...");

	// Cleanup listener socket
	close(listener_fd);

	// Clean up IO handler
	io_handler_cleanup(&ring);

	// Wait for worker threads to finish
	wait_for_worker_threads();

	TRACE(REFFS_TRACE_LEVEL_WARNING, "Unregistering Port Mapper");
	pmap_unset(MOUNT_PROGRAM, MOUNT_V3);
	pmap_unset(NFS3_PROGRAM, NFS_V3);

out:
	TRACE(REFFS_TRACE_LEVEL_WARNING,
	      "Final io_context statistics: created=%d, freed=%d, difference=%d",
	      get_context_created(), get_context_freed(),
	      get_context_created() - get_context_freed());

	// Wait for RCU grace period
	TRACE(REFFS_TRACE_LEVEL_WARNING, "Calling rcu_barrier()...");
	rcu_barrier();

	reffs_ns_fini();

	// Let inodes clear out of memory
	TRACE(REFFS_TRACE_LEVEL_WARNING, "Calling rcu_barrier()...");
	rcu_barrier();

	mount3_protocol_deregister();
	nfs3_protocol_deregister();

	LOG("Shutdown complete");
	return exit_code;
}
