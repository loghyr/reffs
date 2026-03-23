/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <getopt.h>
#include <limits.h>
#include <sys/stat.h>
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
#include "reffs/client.h"
#include "reffs/dstore.h"
#include "reffs/runway.h"
#include "reffs/settings.h"

#define NFS_PORT 2049

/*
 * Sanitizer option callbacks: compiled in only when the sanitizer is active.
 * These are weak symbols consulted before main() and before the ASAN_OPTIONS /
 * UBSAN_OPTIONS environment variables, so they act as a built-in default.
 *
 * detect_leaks=0  – pmap_set() (TIRPC) and pthread stacks produce
 *                   process-lifetime LSan false positives we cannot fix.
 * halt_on_error=0 – continue after the first error so the full run is visible
 *                   in the log rather than stopping at the first finding.
 */
#ifdef ASAN_ENABLED
const char *__asan_default_options(void)
{
	return "detect_leaks=0:halt_on_error=0";
}
#endif

#ifdef UBSAN_ENABLED
const char *__ubsan_default_options(void)
{
	return "halt_on_error=0";
}
#endif

// Global flag for clean shutdown
volatile sig_atomic_t running = 1;

// Signal handler
static void signal_handler(int __attribute__((unused)) sig)
{
	running = 0;
}

struct backend_thread_args {
	volatile sig_atomic_t *running;
	struct ring_context *rc;
};

static void *backend_thread_fn(void *arg)
{
	struct backend_thread_args *a = arg;
	io_backend_main_loop(a->running, a->rc);
	return NULL;
}

static void usage(const char *prog)
{
	printf("Usage: %s [options]\n", prog);
	printf("Options:\n");
	printf("  -h  --help                   Print this usage and exit\n");
	printf("  -r  --rpc_dump               Dump RPC msg bodies\n");
	printf("  -C  --config=path            Load configuration from TOML file\n");
	printf("  -p  --port=id                Serve NFS traffic from this \"port\"\n");
	printf("  -f  --file=fname             Save tracing data to this file \"fname\"\n");
	printf("  -b  --backend=type           Storage backend (ram, posix, rocksdb)\n");
	printf("  -B  --backend-path=path      Path for POSIX backend\n");
	printf("  -i  --case-insensitive       Enable case-insensitive name lookups\n");
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
	{ "config", required_argument, 0, 'C' },
	{ "file", required_argument, 0, 'f' },
	{ "help", no_argument, 0, 'h' },
	{ "case-insensitive", no_argument, 0, 'i' },
	{ "rpc_dump", no_argument, 0, 'r' },
	{ "port", required_argument, 0, 'p' },
	{ "backend", required_argument, 0, 'b' },
	{ "backend-path", required_argument, 0, 'B' },
	{ "state-path", required_argument, 0, 'S' },
	{ NULL, 0, NULL, 0 },
};

int main(int argc, char *argv[])

{
	int lsnr_ipv4_nfs_fd = -1;
	int lsnr_ipv6_nfs_fd = -1;
	int lsnr_ipv4_probe_fd = -1;
	int lsnr_ipv6_probe_fd = -1;

	int exit_code = 0;
	int port = NFS_PORT;
	int opt;

	struct ring_context rc;
	struct ring_context rc_backend;
	bool rc_backend_inited = false;
	pthread_t backend_thread;
	bool backend_thread_started = false;

	char *trace_file = "./reffsd.log";

	struct server_state *ss = NULL;

	struct reffs_config cfg;
	const char *config_file = NULL;

	/* CLI overrides: -1/NULL means "not set on command line, use config" */
	int cli_port = -1;
	enum reffs_storage_type cli_storage_type =
		(enum reffs_storage_type) - 1;
	char *cli_backend_path = NULL;
	char *cli_state_path = NULL;

	char *opts = "p:hriC:c:f:b:B:S:";
	enum reffs_text_case case_mode = reffs_text_case_sensitive;

#ifdef HAVE_JEMALLOC
#ifdef HAVE_VM
	/* Release virtual address space immediately on free */
	mallctl("dirty_decay_ms", NULL, NULL, &(ssize_t){ 0 }, sizeof(ssize_t));
	mallctl("muzzy_decay_ms", NULL, NULL, &(ssize_t){ 0 }, sizeof(ssize_t));
#endif
#endif

	// Initialize userspace RCU
	rcu_init();

	while ((opt = getopt_long(argc, argv, opts, long_opts, NULL)) != -1) {
		switch (opt) {
		case 'C':
			config_file = optarg;
			break;
		case 'p': {
			char *endptr;
			long val = strtol(optarg, &endptr, 10);
			if (*endptr != '\0' || val <= 0 || val > 65535) {
				fprintf(stderr, "Invalid port: %s\n", optarg);
				return 1;
			}
			cli_port = (int)val;
			break;
		}
		case 'b':
			if (strcasecmp(optarg, "posix") == 0)
				cli_storage_type = REFFS_STORAGE_POSIX;
			else if (strcasecmp(optarg, "rocksdb") == 0)
				cli_storage_type = REFFS_STORAGE_ROCKSDB;
			else
				cli_storage_type = REFFS_STORAGE_RAM;
			break;
		case 'B':
			cli_backend_path = optarg;
			break;
		case 'i':
			case_mode = reffs_text_case_insensitive;
			break;
		case 'S':
			cli_state_path = optarg;
			break;
		case 'c': {
			char *endptr;
			long tracing = strtol(optarg, &endptr, 10);
			if (*endptr == '\0' && tracing >= 0 &&
			    tracing < REFFS_TRACE_CAT_ALL) {
				reffs_trace_enable_category((int)tracing);
			} else {
				fprintf(stderr,
					"Invalid tracing category: %s\n",
					optarg);
				return 1;
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

	/* Load config (defaults first, then file if given) */
	reffs_config_defaults(&cfg);
	if (config_file && reffs_config_load(&cfg, config_file) < 0)
		return 1;

	/* CLI args override config file */
	if (cli_port >= 0)
		cfg.port = (uint16_t)cli_port;
	if ((int)cli_storage_type >= 0)
		cfg.backend_type = (enum reffs_backend_type)cli_storage_type;
	if (cli_backend_path)
		strncpy(cfg.backend_path, cli_backend_path,
			sizeof(cfg.backend_path) - 1);
	if (cli_state_path)
		strncpy(cfg.state_file, cli_state_path,
			sizeof(cfg.state_file) - 1);

	port = cfg.port;

	setvbuf(stdout, NULL, _IOLBF, 0);
	reffs_trace_init(trace_file);
	reffs_fs_set_storage((enum reffs_storage_type)cfg.backend_type,
			     cfg.backend_path[0] ? cfg.backend_path : NULL);

	sigset_t mask;

	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);

	// Block signals in main thread temporarily
	pthread_sigmask(SIG_BLOCK, &mask, NULL);

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

	ss = server_state_init(cfg.state_file, port, case_mode);
	if (!ss) {
		return 1;
	}
	ss->ss_exchgid_flags = reffs_role_exchgid_flags(cfg.role);

	// Initialize IO handler
	if (io_handler_init(&rc) < 0) {
		return 1;
	}

	// Initialize backend file-I/O ring
	if (io_backend_init(&rc_backend) < 0) {
		io_handler_fini(&rc);
		return 1;
	}
	rc_backend_inited = true;
	io_backend_set_global(&rc_backend);

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

	/*
	 * Combined/MDS mode: create a separate DS super_block for
	 * local data store files, isolated from the MDS namespace.
	 */
	if (cfg.role == REFFS_ROLE_COMBINED && cfg.ds_backend_path[0]) {
		/* posix_sb_alloc creates sb_2/ under ds_path. */
		struct super_block *ds_sb = super_block_alloc(
			SUPER_BLOCK_DS_ID, "/ds",
			(enum reffs_storage_type)cfg.backend_type,
			cfg.ds_backend_path);
		if (!ds_sb) {
			LOG("Failed to create DS super_block");
			exit_code = 1;
			goto out;
		}
		super_block_dirent_create(ds_sb, NULL,
					  reffs_life_action_birth);
		reffs_fs_recover(ds_sb);
		/* Do NOT put ds_sb here — release_all_fs_dirents()
		 * in reffs_ns_fini() puts the alloc ref for all sbs. */

		TRACE("DS super_block %lu at %s",
		      (unsigned long)SUPER_BLOCK_DS_ID,
		      cfg.ds_backend_path);
	}

	/*
	 * MDS mode: initialize dstore hash table, connect to each
	 * configured data server via MOUNT, and obtain root FHs.
	 */
	if (cfg.role == REFFS_ROLE_MDS || cfg.role == REFFS_ROLE_COMBINED) {
		if (dstore_init() < 0) {
			LOG("Failed to create dstore hash table");
			exit_code = 1;
			goto out;
		}
		if (cfg.ndata_servers > 0) {
			if (dstore_load_config(&cfg) < 0)
				LOG("Warning: some data stores unavailable");

			/* Pre-create file runway on each dstore. */
			for (unsigned int i = 0; i < cfg.ndata_servers; i++) {
				struct dstore *ds =
					dstore_find(cfg.data_servers[i].id);
				if (!ds || !dstore_is_available(ds)) {
					dstore_put(ds);
					continue;
				}
				ds->ds_runway =
					runway_create(ds, RUNWAY_DEFAULT_SIZE);
				dstore_put(ds);
			}
		}
	}

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
		TRACE("Registered NFSv4 TCP on port %d", port);
	} else {
		LOG("Failed to register NFSv4 TCP");
	}

	/* NFSv3 */
	if (pmap_set(NFS3_PROGRAM, NFS_V3, IPPROTO_TCP, port)) {
		TRACE("Registered NFSv3 TCP on port %d", port);
	} else {
		LOG("Failed to register NFSv3 TCP");
	}
	if (pmap_set(NFS3_PROGRAM, NFS_V3, IPPROTO_UDP, port)) {
		TRACE("Registered NFSv3 UDP on port %d", port);
	} else {
		LOG("Failed to register NFSv3 UDP");
	}

	/* MOUNTv3 */
	if (pmap_set(MOUNT_PROGRAM, MOUNT_V3, IPPROTO_TCP, port)) {
		TRACE("Registered MOUNTv3 TCP on port %d", port);
	} else {
		LOG("Failed to register MOUNTv3 TCP");
	}
	if (pmap_set(MOUNT_PROGRAM, MOUNT_V3, IPPROTO_UDP, port)) {
		TRACE("Registered MOUNTv3 UDP on port %d", port);
	} else {
		LOG("Failed to register MOUNTv3 UDP");
	}

	/* NLMv4 */
	if (pmap_set(NLM_PROG, NLM4_VERS, IPPROTO_TCP, port)) {
		TRACE("Registered NLMv4 TCP on port %d", port);
	} else {
		LOG("Failed to register NLMv4 TCP");
	}
	if (pmap_set(NLM_PROG, NLM4_VERS, IPPROTO_UDP, port)) {
		TRACE("Registered NLMv4 UDP on port %d", port);
	} else {
		LOG("Failed to register NLMv4 UDP");
	}

	/* NLMv3 */
	if (pmap_set(NLM_PROG, NLM_VERSX, IPPROTO_TCP, port)) {
		TRACE("Registered NLMv3 TCP on port %d", port);
	} else {
		LOG("Failed to register NLMv3 TCP");
	}
	if (pmap_set(NLM_PROG, NLM_VERSX, IPPROTO_UDP, port)) {
		TRACE("Registered NLMv3 UDP on port %d", port);
	} else {
		LOG("Failed to register NLMv3 UDP");
	}

	/* NLMv1 */
	if (pmap_set(NLM_PROG, NLM_VERS, IPPROTO_TCP, port)) {
		TRACE("Registered NLMv1 TCP on port %d", port);
	} else {
		LOG("Failed to register NLMv1 TCP");
	}
	if (pmap_set(NLM_PROG, NLM_VERS, IPPROTO_UDP, port)) {
		TRACE("Registered NLMv1 UDP on port %d", port);
	} else {
		LOG("Failed to register NLMv1 UDP");
	}

	/* NSM */
	if (pmap_set(SM_PROG, SM_VERS, IPPROTO_TCP, port)) {
		TRACE("Registered NSM TCP on port %d", port);
	} else {
		LOG("Failed to register NSM TCP");
	}
	if (pmap_set(SM_PROG, SM_VERS, IPPROTO_UDP, port)) {
		TRACE("Registered NSM UDP on port %d", port);
	} else {
		LOG("Failed to register NSM UDP");
	}

	// Spawn backend file-I/O ring thread
	struct backend_thread_args bta = { .running = &running,
					   .rc = &rc_backend };
	if (pthread_create(&backend_thread, NULL, backend_thread_fn, &bta) !=
	    0) {
		LOG("Failed to create backend io_uring thread");
		exit_code = 1;
		goto out;
	}
	backend_thread_started = true;

	// Run the main IO processing loop

	io_handler_main_loop(&running, &rc);

	pthread_join(backend_thread, NULL);
	backend_thread_started = false;

	TRACE("Main loop exited, cleaning up...");

out:
	if (backend_thread_started) {
		pthread_join(backend_thread, NULL);
		backend_thread_started = false;
	}

	TRACE("Unregistering Port Mapper");
	pmap_unset(NLM_PROG, NLM4_VERS);
	pmap_unset(NLM_PROG, NLM_VERSX);
	pmap_unset(NLM_PROG, NLM_VERS);
	pmap_unset(SM_PROG, SM_VERS);
	pmap_unset(MOUNT_PROGRAM, MOUNT_V3);
	pmap_unset(NFS3_PROGRAM, NFS_V3);
	pmap_unset(NFS4_PROGRAM, NFS_V4);

	// Cleanup listener sockets
	if (lsnr_ipv4_probe_fd >= 0) {
		close(lsnr_ipv4_probe_fd);
		lsnr_ipv4_probe_fd = -1;
	}

	if (lsnr_ipv4_nfs_fd >= 0) {
		close(lsnr_ipv4_nfs_fd);
		lsnr_ipv4_nfs_fd = -1;
	}

	if (lsnr_ipv6_probe_fd >= 0) {
		close(lsnr_ipv6_probe_fd);
		lsnr_ipv6_probe_fd = -1;
	}

	if (lsnr_ipv6_nfs_fd >= 0) {
		close(lsnr_ipv6_nfs_fd);
		lsnr_ipv6_nfs_fd = -1;
	}

	io_handler_fini(&rc);

	if (rc_backend_inited)
		io_backend_fini(&rc_backend);

	pthread_sigmask(SIG_UNBLOCK, &mask, NULL);

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

	if (cfg.role == REFFS_ROLE_MDS || cfg.role == REFFS_ROLE_COMBINED)
		dstore_fini();
	client_unload_all_clients();

	server_state_fini(ss);

	TRACE("Shutdown complete");
	return exit_code;
}
