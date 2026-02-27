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
#include "probe1_xdr.h"

#include "reffs/log.h"
#include "reffs/rpc.h"
#include "reffs/nfs3.h"
#include "reffs/mount3.h"
#include "reffs/probe1.h"
#include "reffs/server.h"
#include "reffs/ns.h"
#include "reffs/io.h"
#include "reffs/trace/common.h"

#define NFS_PORT 2049

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
	printf("  -r  --rpc_dump               Dump RPC msg bodies\n");
	printf("  -p  --port=id                Serve NFS traffic from this \"port\"\n");
	printf("  -f  --file=fname             Save tracing data to this file \"fname\"\n");
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
	{ "help", no_argument, 0, 'h' },
	{ "rpc_dump", no_argument, 0, 'r' },
	{ "port", required_argument, 0, 'p' },
	{ NULL, 0, NULL, 0 },
};

int main(int argc, char *argv[])
{
	int lsnr_ipv4_nfs_fd;
	int lsnr_ipv6_nfs_fd;
	int lsnr_ipv4_probe_fd;
	int lsnr_ipv6_probe_fd;

	int exit_code = 0;
	int port = NFS_PORT;
	int opt;

	struct ring_context rc;

	char *trace_file = "./nfs3_srv.log";

	// Initialize userspace RCU
	rcu_init();

	server_boot_uuid_generate();

	char *opts = "p:hrt:c:f:";

	while ((opt = getopt_long(argc, argv, opts, long_opts, NULL)) != -1) {
		switch (opt) {
		case 'p':
			port = atoi(optarg);
			break;
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
		case 'r':
			rpc_enable_packet_logging();
			break;
		case 'h':
			usage(argv[0]);
			return 0;
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

	// Set up protocol handlers
	if (nfs3_protocol_register()) {
		exit_code = 1;
		goto out;
	}

	if (mount3_protocol_register()) {
		exit_code = 1;
		goto out;
	}

	if (probe1_protocol_register()) {
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
	lsnr_ipv4_nfs_fd = io_lsnr_setup_ipv4(port);
	if (lsnr_ipv4_nfs_fd < 0) {
		LOG("Failed to setup listener on port %d", port);
		exit_code = 1;
		goto out;
	}

	io_add_listener(lsnr_ipv4_nfs_fd);
	io_request_accept_op(lsnr_ipv4_nfs_fd, NULL, &rc);

	lsnr_ipv6_nfs_fd = io_lsnr_setup_ipv6(port);
	if (lsnr_ipv6_nfs_fd < 0) {
		LOG("Failed to setup listener on port %d", port);
		exit_code = 1;
		goto out;
	}

	io_add_listener(lsnr_ipv6_nfs_fd);
	io_request_accept_op(lsnr_ipv6_nfs_fd, NULL, &rc);

	lsnr_ipv4_probe_fd = io_lsnr_setup_ipv4(PROBE_PORT);
	if (lsnr_ipv4_probe_fd < 0) {
		LOG("Failed to setup listener on port %d", PROBE_PORT);
		exit_code = 1;
		goto out;
	}

	io_add_listener(lsnr_ipv4_probe_fd);
	io_request_accept_op(lsnr_ipv4_probe_fd, NULL, &rc);

	lsnr_ipv6_probe_fd = io_lsnr_setup_ipv6(PROBE_PORT);
	if (lsnr_ipv6_probe_fd < 0) {
		LOG("Failed to setup listener on port %d", PROBE_PORT);
		exit_code = 1;
		goto out;
	}

	io_add_listener(lsnr_ipv6_probe_fd);
	io_request_accept_op(lsnr_ipv6_probe_fd, NULL, &rc);

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

	__atomic_store(&running, &(int){ 1 }, __ATOMIC_SEQ_CST);

	// Run the main IO processing loop
	io_handler_main_loop(&running, &rc);

	TRACE("Main loop exited, cleaning up...");

	// Cleanup listener socket
	if (lsnr_ipv4_probe_fd > 0) {
		close(lsnr_ipv4_probe_fd);
		lsnr_ipv4_probe_fd = -1;
	}

	if (lsnr_ipv4_nfs_fd > 0) {
		close(lsnr_ipv4_nfs_fd);
		lsnr_ipv4_nfs_fd = -1;
	}

	if (lsnr_ipv6_probe_fd > 0) {
		close(lsnr_ipv6_probe_fd);
		lsnr_ipv6_probe_fd = -1;
	}

	if (lsnr_ipv6_nfs_fd > 0) {
		close(lsnr_ipv6_nfs_fd);
		lsnr_ipv6_nfs_fd = -1;
	}

	io_handler_fini(&rc);

	TRACE("Unregistering Port Mapper");
	pmap_unset(MOUNT_PROGRAM, MOUNT_V3);
	pmap_unset(NFS3_PROGRAM, NFS_V3);

out:
	TRACE("Final io_context statistics: created=%ld, freed=%ld, difference=%ld",
	      io_context_get_created(), io_context_get_freed(),
	      io_context_get_created() - io_context_get_freed());

	// Wait for RCU grace period
	TRACE("Calling rcu_barrier()...");
	rcu_barrier();

	reffs_ns_fini();

	// Let inodes clear out of memory
	TRACE("Calling rcu_barrier()...");
	rcu_barrier();

	probe1_protocol_deregister();
	mount3_protocol_deregister();
	nfs3_protocol_deregister();

	LOG("Shutdown complete");
	return exit_code;
}
