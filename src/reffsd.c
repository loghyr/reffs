/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <getopt.h>
#include <netinet/in.h>
#include <rpc/pmap_clnt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "mntv3_xdr.h"
#include "nfsv3_xdr.h"
#include "nfsv42_xdr.h"
#include "nlm4_prot.h"
#include "nlm_prot.h"
#include "probe1_xdr.h"
#include "reffs/fs.h"
#include "reffs/log.h"
#include "reffs/io.h"
#include "reffs/log.h"
#include "reffs/mount3.h"
#include "reffs/nfs3.h"
#include "reffs/nfs4.h"
#include "reffs/nlm.h"
#include "reffs/ns.h"
#include "reffs/nsm.h"
#include "reffs/probe1.h"
#include "reffs/rcu.h"
#include "reffs/ring.h"
#include "reffs/rpc.h"
#include "reffs/server.h"
#include "reffs/super_block.h"
#include "reffs/trace/common.h"
#include "reffs/trace/types.h"
#include "reffs/types.h"
#include "sm_inter.h"
#include "reffs/fs.h"
#include "reffs/client.h"
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
	printf("  -b  --backend=type           Storage backend (ram, posix)\n");
	printf("  -B  --backend-path=path      Path for POSIX backend\n");
	printf("  -S  --state-path=path        Path for storing state\n");
	printf("  -c  --category=cat           Enable tracing for a category\n");
	printf("                                     0 - General\n");
	printf("                                     1 - IO\n");
	printf("                                     2 - RPC\n");
	printf("                                     3 - NFS\n");
	printf("                                     4 - NLM\n");
	printf("                                     5 - FS\n");
	printf("                                     6 - LOG\n");
}

static struct option long_opts[] = {
	{ "category", required_argument, 0, 'c' },
	{ "file", required_argument, 0, 'f' },
	{ "help", no_argument, 0, 'h' },
	{ "rpc_dump", no_argument, 0, 'r' },
	{ "port", required_argument, 0, 'p' },
	{ "backend", required_argument, 0, 'b' },
	{ "backend-path", required_argument, 0, 'B' },
	{ "state-file", required_argument, 0, 'S' },
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

	char *trace_file = "./reffsd.log";

	struct server_state *ss = NULL;

#ifdef HAVE_JEMALLOC
#ifdef HAVE_VM
	/* Release virtual address space immediately on free */
	mallctl("dirty_decay_ms", NULL, NULL, &(ssize_t){ 0 }, sizeof(ssize_t));
	mallctl("muzzy_decay_ms", NULL, NULL, &(ssize_t){ 0 }, sizeof(ssize_t));
#endif
#endif

	// Initialize userspace RCU
	rcu_init();

	char *opts = "p:hrt:c:f:b:B:S:";
	enum reffs_storage_type storage_type = REFFS_STORAGE_RAM;
	char *backend_path = NULL;
	char *state_path = "/tmp/reffs.state";

	while ((opt = getopt_long(argc, argv, opts, long_opts, NULL)) != -1) {
		switch (opt) {
		case 'p':
			port = atoi(optarg);
			break;
		case 'b':
			if (strcasecmp(optarg, "posix") == 0)
				storage_type = REFFS_STORAGE_POSIX;
			else if (strcasecmp(optarg, "ram") == 0)
				storage_type = REFFS_STORAGE_RAM;
			break;
		case 'B':
			backend_path = optarg;
			break;
		case 'S':
			state_path = optarg;
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
	reffs_fs_set_storage(storage_type, backend_path);

	// Ignore SIGPIPE
	signal(SIGPIPE, SIG_IGN);

	// Setup signal handlers
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = signal_handler;
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

	ss = server_state_init(state_path, port);
	if (!ss) {
		return 1;
	}

	// Initialize IO handler
	if (io_handler_init(&rc) < 0) {
		return 1;
	}

	// Set up protocol handlers
	if (nfs4_protocol_register()) {
		exit_code = 1;
		goto out;
	}

	if (nfs3_protocol_register()) {
		exit_code = 1;
		goto out;
	}

	if (mount3_protocol_register()) {
		exit_code = 1;
		goto out;
	}

	if (nlm4_protocol_register()) {
		exit_code = 1;
		goto out;
	}

	if (nlm_protocol_register()) {
		exit_code = 1;
		goto out;
	}

	if (sm_protocol_register()) {
		exit_code = 1;
		goto out;
	}

	if (probe1_protocol_register()) {
		exit_code = 1;
		goto out;
	}

	exit_code = reffs_ns_init();
	if (exit_code == 0) {
		struct super_block *sb = super_block_find(1);
		if (sb) {
			reffs_fs_recover(sb);
			super_block_put(sb);
		}
	}
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

	/* aggressive cleanup of old registrations */
	for (int v = 1; v <= 4; v++) {
		pmap_unset(NLM_PROG, v);
	}
	pmap_unset(SM_PROG, SM_VERS);
	pmap_unset(MOUNT_PROGRAM, MOUNT_V3);
	pmap_unset(NFS3_PROGRAM, NFS_V3);
	pmap_unset(NFS4_PROGRAM, NFS_V4);

	/* NFSv4 */
	if (pmap_set(NFS4_PROGRAM, NFS_V4, IPPROTO_TCP, port)) {
		LOG("Registered NFSv4 TCP on port %d", port);
	} else {
		LOG("Failed to register NFSv4 TCP");
	}

	/* NFSv3 */
	if (pmap_set(NFS3_PROGRAM, NFS_V3, IPPROTO_TCP, port)) {
		LOG("Registered NFSv3 TCP on port %d", port);
	} else {
		LOG("Failed to register NFSv3 TCP");
	}
	if (pmap_set(NFS3_PROGRAM, NFS_V3, IPPROTO_UDP, port)) {
		LOG("Registered NFSv3 UDP on port %d", port);
	} else {
		LOG("Failed to register NFSv3 UDP");
	}

	/* MOUNTv3 */
	if (pmap_set(MOUNT_PROGRAM, MOUNT_V3, IPPROTO_TCP, port)) {
		LOG("Registered MOUNTv3 TCP on port %d", port);
	} else {
		LOG("Failed to register MOUNTv3 TCP");
	}
	if (pmap_set(MOUNT_PROGRAM, MOUNT_V3, IPPROTO_UDP, port)) {
		LOG("Registered MOUNTv3 UDP on port %d", port);
	} else {
		LOG("Failed to register MOUNTv3 UDP");
	}

	/* NLMv4 */
	if (pmap_set(NLM_PROG, NLM4_VERS, IPPROTO_TCP, port)) {
		LOG("Registered NLMv4 TCP on port %d", port);
	} else {
		LOG("Failed to register NLMv4 TCP");
	}
	if (pmap_set(NLM_PROG, NLM4_VERS, IPPROTO_UDP, port)) {
		LOG("Registered NLMv4 UDP on port %d", port);
	} else {
		LOG("Failed to register NLMv4 UDP");
	}

	/* NLMv3 */
	if (pmap_set(NLM_PROG, NLM_VERSX, IPPROTO_TCP, port)) {
		LOG("Registered NLMv3 TCP on port %d", port);
	} else {
		LOG("Failed to register NLMv3 TCP");
	}
	if (pmap_set(NLM_PROG, NLM_VERSX, IPPROTO_UDP, port)) {
		LOG("Registered NLMv3 UDP on port %d", port);
	} else {
		LOG("Failed to register NLMv3 UDP");
	}

	/* NLMv1 */
	if (pmap_set(NLM_PROG, NLM_VERS, IPPROTO_TCP, port)) {
		LOG("Registered NLMv1 TCP on port %d", port);
	} else {
		LOG("Failed to register NLMv1 TCP");
	}
	if (pmap_set(NLM_PROG, NLM_VERS, IPPROTO_UDP, port)) {
		LOG("Registered NLMv1 UDP on port %d", port);
	} else {
		LOG("Failed to register NLMv1 UDP");
	}

	/* NSM */
	if (pmap_set(SM_PROG, SM_VERS, IPPROTO_TCP, port)) {
		LOG("Registered NSM TCP on port %d", port);
	} else {
		LOG("Failed to register NSM TCP");
	}
	if (pmap_set(SM_PROG, SM_VERS, IPPROTO_UDP, port)) {
		LOG("Registered NSM UDP on port %d", port);
	} else {
		LOG("Failed to register NSM UDP");
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
	pmap_unset(NFS4_PROGRAM, NFS_V4);

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
	sm_protocol_deregister();
	nlm4_protocol_deregister();
	nlm_protocol_deregister();
	mount3_protocol_deregister();
	nfs3_protocol_deregister();
	nfs4_protocol_deregister();

	client_unload_all_clients();

	server_state_fini(ss);

	LOG("Shutdown complete");
	return exit_code;
}
