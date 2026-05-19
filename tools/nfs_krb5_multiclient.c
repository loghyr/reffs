/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * nfs_krb5_multiclient -- multi-client Kerberos NFS test driver.
 *
 * Owns the entire lifecycle: stands up a krb5 environment (an
 * embedded mini-KDC by default, or an external pre-provisioned KDC
 * with --external-kdc), spawns a reffsd with a krb5 root export,
 * provisions N client identities, forks N client workers that each
 * authenticate with their own krb5 identity and run a
 * write/read/CRC exchange, then tears everything down.
 *
 * See .claude/design/krb5-multiclient-test.md.  --help lists the
 * full flag set, including the --external-kdc family.
 *
 * Exit: 0  all workers passed
 *       1  a worker or a setup step failed
 *       2  usage error
 *       77 skipped (embedded KDC unavailable -- krb5kdc not installed)
 */

#ifdef HAVE_CONFIG_H
#include "config.h" /* IWYU pragma: keep */
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ec_client.h"

#include "krb5_env.h"
#include "krb5_client_core.h"

#define DEFAULT_CLIENTS 2
/*
 * 22049, not 2049 (privileged, collides with a real NFS server) and
 * not 20490 (reffsd's own probe-protocol listener -- a same-process
 * EADDRINUSE).
 */
#define DEFAULT_PORT 22049
#define READY_TIMEOUT_SEC 20
#define SKIP_EXIT 77

/*
 * Per-worker credential cache path.  Wrapping the buffer in a struct
 * avoids the `char (*ccache)[N]` pointer-to-array declarator, which
 * different clang-format versions disagree about (some space the type
 * before the `(`, some don't) and so flap between local and CI style
 * checks.  Plain `struct ... *` is version-stable.
 */
struct krb5mc_ccache {
	char path[320];
};

static void usage(void)
{
	fprintf(stderr,
		"Usage: nfs_krb5_multiclient --reffsd <path> [options]\n"
		"\n"
		"Options:\n"
		"  --reffsd <path>           reffsd binary to spawn (required)\n"
		"  --clients, -n <N>         client workers (default 2)\n"
		"  --port, -p <n>            NFS port for reffsd (default 22049)\n"
		"  --sec <krb5|krb5i|krb5p>  security level (default krb5)\n"
		"  --same-principal          all workers share one principal\n"
		"  --server-host <host>      host workers connect to and the\n"
		"                            GSS service name (default localhost)\n"
		"\n"
		"External KDC (default is a self-contained embedded KDC):\n"
		"  --external-kdc            use an external, pre-provisioned KDC\n"
		"  --krb5-conf <path>        krb5.conf for the realm\n"
		"  --service-keytab <path>   keytab for reffsd's nfs/<host> SPN\n"
		"  --principals <path>       file of '<principal> <password>'\n"
		"                            lines, one identity per line\n"
		"  --expect-map <path>       file of '<principal> <owner>' to\n"
		"                            assert each file's GETATTR owner\n"
		"  --help, -h                this help\n");
}

/* Write the reffsd TOML config for the krb5 test server. */
static int write_reffsd_toml(const char *path, int port, const char *datadir,
			     const char *statedir)
{
	FILE *fp = fopen(path, "w");

	if (!fp)
		return -1;

	fprintf(fp,
		"[server]\n"
		"port            = %d\n"
		"bind            = \"*\"\n"
		"role            = \"standalone\"\n"
		"minor_versions  = [1, 2]\n"
		"workers         = 4\n"
		"log_level       = \"info\"\n"
		"\n"
		"[backend]\n"
		"type            = \"posix\"\n"
		"path            = \"%s\"\n"
		"state_file      = \"%s\"\n"
		"\n"
		"[[export]]\n"
		"path        = \"/\"\n"
		"\n"
		"    [[export.clients]]\n"
		"    match       = \"*\"\n"
		"    access      = \"rw\"\n"
		"    root_squash = false\n"
		"    flavors     = [\"krb5\"]\n",
		port, datadir, statedir);

	fclose(fp);
	return 0;
}

/* Poll connect() to 127.0.0.1:port until reffsd is listening. */
static int wait_for_listen(int port, int timeout_sec)
{
	struct sockaddr_in sa = {
		.sin_family = AF_INET,
		.sin_port = htons((uint16_t)port),
	};

	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	for (int waited = 0; waited < timeout_sec * 10; waited++) {
		int fd = socket(AF_INET, SOCK_STREAM, 0);

		if (fd < 0)
			return -1;
		if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
			close(fd);
			usleep(500000); /* brief grace post-listen */
			return 0;
		}
		close(fd);
		usleep(100000); /* 100 ms */
	}
	return -1;
}

/*
 * Spawn reffsd as a child.  KRB5_KTNAME is set in the child so the
 * GSS server side picks up the mini-KDC service keytab; KRB5_CONFIG
 * is already in the environment (set by mini_kdc_start) and is
 * inherited.  reffsd's stdout/stderr are redirected to @logpath so
 * the driver's own output stays clean.  Returns the child pid, or
 * -1 on fork failure.
 */
static pid_t spawn_reffsd(const char *reffsd, const char *toml,
			  const char *trace, const char *logpath,
			  const char *keytab)
{
	pid_t pid = fork();

	if (pid < 0)
		return -1;
	if (pid == 0) {
		char cfg[600];
		char trc[600];
		int logfd = open(logpath, O_WRONLY | O_CREAT | O_TRUNC, 0600);

		if (logfd >= 0) {
			dup2(logfd, STDOUT_FILENO);
			dup2(logfd, STDERR_FILENO);
			close(logfd);
		}

		snprintf(cfg, sizeof(cfg), "--config=%s", toml);
		snprintf(trc, sizeof(trc), "--file=%s", trace);

		setenv("KRB5_KTNAME", keytab, 1);

		execl(reffsd, reffsd, cfg, trc, (char *)NULL);
		_exit(127);
	}
	return pid;
}

/* --expect-map: principal -> expected GETATTR owner string. */
struct expect_entry {
	char ee_principal[256];
	char ee_owner[256];
};

/*
 * Load the --expect-map file: one "<principal> <owner>" pair per
 * line (blank lines and #-comments ignored).  On success returns the
 * entry count and sets *out to a heap array the caller frees.
 * Returns -1 on error.
 */
static int expect_map_load(const char *path, struct expect_entry **out)
{
	FILE *fp = fopen(path, "r");
	struct expect_entry *map = NULL;
	char line[600];
	int n = 0;
	int cap = 0;

	if (!fp) {
		fprintf(stderr,
			"nfs_krb5_multiclient: cannot open --expect-map "
			"file: %s\n",
			path);
		return -1;
	}
	while (fgets(line, sizeof(line), fp)) {
		char princ[256];
		char owner[256];

		if (line[0] == '#' || line[0] == '\n')
			continue;
		if (sscanf(line, "%255s %255s", princ, owner) != 2) {
			fprintf(stderr,
				"nfs_krb5_multiclient: malformed "
				"--expect-map line: %s",
				line);
			free(map);
			fclose(fp);
			return -1;
		}
		if (n == cap) {
			int ncap = cap ? cap * 2 : 16;
			struct expect_entry *m =
				realloc(map, (size_t)ncap * sizeof(*m));

			if (!m) {
				free(map);
				fclose(fp);
				return -1;
			}
			map = m;
			cap = ncap;
		}
		snprintf(map[n].ee_principal, sizeof(map[n].ee_principal), "%s",
			 princ);
		snprintf(map[n].ee_owner, sizeof(map[n].ee_owner), "%s", owner);
		n++;
	}
	fclose(fp);
	*out = map;
	return n;
}

/* Look up @principal in the expect-map; NULL if absent. */
static const char *expect_map_lookup(const struct expect_entry *map, int n,
				     const char *principal)
{
	for (int i = 0; i < n; i++) {
		if (strcmp(map[i].ee_principal, principal) == 0)
			return map[i].ee_owner;
	}
	return NULL;
}

int main(int argc, char *argv[])
{
	const char *reffsd = NULL;
	int clients = DEFAULT_CLIENTS;
	int port = DEFAULT_PORT;
	enum ec_sec_flavor sec = EC_SEC_KRB5;
	bool same_principal = false;
	bool external_kdc = false;
	const char *krb5_conf = NULL;
	const char *service_keytab = NULL;
	const char *principals_file = NULL;
	const char *server_host = "localhost";
	const char *expect_map_path = NULL;
	struct expect_entry *expect_map = NULL;
	int expect_map_n = 0;

	struct krb5_env *env = NULL;
	pid_t reffsd_pid = -1;
	char reffsd_dir[] = "/tmp/krb5mc_XXXXXX";
	bool dir_made = false;
	char datadir[280], statedir[280], toml[280], trace[280], logpath[280];
	struct krb5mc_ccache *ccache = NULL;
	pid_t *wpid = NULL;
	int rc = 1;

	static struct option opts[] = {
		{ "reffsd", required_argument, NULL, 'r' },
		{ "clients", required_argument, NULL, 'n' },
		{ "port", required_argument, NULL, 'p' },
		{ "sec", required_argument, NULL, 'x' },
		{ "same-principal", no_argument, NULL, 'S' },
		{ "server-host", required_argument, NULL, 'H' },
		{ "external-kdc", no_argument, NULL, 'E' },
		{ "krb5-conf", required_argument, NULL, 'C' },
		{ "service-keytab", required_argument, NULL, 'K' },
		{ "principals", required_argument, NULL, 'P' },
		{ "expect-map", required_argument, NULL, 'M' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	int opt;

	while ((opt = getopt_long(argc, argv, "r:n:p:x:SH:EC:K:P:M:h", opts,
				  NULL)) != -1) {
		switch (opt) {
		case 'r':
			reffsd = optarg;
			break;
		case 'n':
			clients = atoi(optarg);
			break;
		case 'p':
			port = atoi(optarg);
			break;
		case 'x':
			if (!strcasecmp(optarg, "krb5"))
				sec = EC_SEC_KRB5;
			else if (!strcasecmp(optarg, "krb5i"))
				sec = EC_SEC_KRB5I;
			else if (!strcasecmp(optarg, "krb5p"))
				sec = EC_SEC_KRB5P;
			else {
				fprintf(stderr, "Unknown sec: %s\n", optarg);
				return 2;
			}
			break;
		case 'S':
			same_principal = true;
			break;
		case 'H':
			server_host = optarg;
			break;
		case 'E':
			external_kdc = true;
			break;
		case 'C':
			krb5_conf = optarg;
			break;
		case 'K':
			service_keytab = optarg;
			break;
		case 'P':
			principals_file = optarg;
			break;
		case 'M':
			expect_map_path = optarg;
			break;
		case 'h':
			usage();
			return 0;
		default:
			usage();
			return 2;
		}
	}

	if (!reffsd) {
		fprintf(stderr, "Error: --reffsd is required\n\n");
		usage();
		return 2;
	}
	if (clients < 1 || clients > 4096) {
		fprintf(stderr, "Error: --clients out of range (1..4096)\n");
		return 2;
	}
	if (port < 1 || port > 65535) {
		fprintf(stderr, "Error: --port out of range\n");
		return 2;
	}
	if (external_kdc &&
	    (!krb5_conf || !service_keytab || !principals_file)) {
		fprintf(stderr, "Error: --external-kdc requires --krb5-conf, "
				"--service-keytab and --principals\n");
		return 2;
	}

	/* ---- 1. krb5 environment ---- */
	if (external_kdc) {
		struct krb5_env_external_cfg cfg = {
			.krb5_conf = krb5_conf,
			.service_keytab = service_keytab,
			.principals_file = principals_file,
		};

		env = krb5_env_external(&cfg);
	} else {
		env = krb5_env_embedded();
	}
	if (!env) {
		fprintf(stderr, "nfs_krb5_multiclient: out of memory\n");
		return 1;
	}
	switch (krb5_env_start(env)) {
	case KRB5_ENV_OK:
		break;
	case KRB5_ENV_SKIP:
		fprintf(stderr, "nfs_krb5_multiclient: krb5 environment "
				"unavailable -- skip\n");
		krb5_env_stop(env);
		return SKIP_EXIT;
	default:
		fprintf(stderr, "nfs_krb5_multiclient: krb5 environment "
				"failed to start\n");
		krb5_env_stop(env);
		return 1;
	}

	/* ---- 2. reffsd state dir + TOML ---- */
	if (!mkdtemp(reffsd_dir)) {
		perror("nfs_krb5_multiclient: mkdtemp");
		goto out;
	}
	dir_made = true;

	snprintf(datadir, sizeof(datadir), "%s/data", reffsd_dir);
	snprintf(statedir, sizeof(statedir), "%s/reffs.state", reffsd_dir);
	snprintf(toml, sizeof(toml), "%s/reffsd.toml", reffsd_dir);
	snprintf(trace, sizeof(trace), "%s/reffsd.trc", reffsd_dir);
	snprintf(logpath, sizeof(logpath), "%s/reffsd.log", reffsd_dir);

	/*
	 * Both must exist before reffsd starts: the posix backend's
	 * data dir, and the state dir -- reffsd's [backend] state_file
	 * is used as a directory (it writes server_state.tmp.* inside
	 * it), not a plain file.
	 */
	if (mkdir(datadir, 0700) != 0) {
		perror("nfs_krb5_multiclient: mkdir datadir");
		goto out;
	}
	if (mkdir(statedir, 0700) != 0) {
		perror("nfs_krb5_multiclient: mkdir statedir");
		goto out;
	}
	if (write_reffsd_toml(toml, port, datadir, statedir) != 0) {
		fprintf(stderr, "nfs_krb5_multiclient: cannot write TOML\n");
		goto out;
	}

	/* ---- 3. spawn reffsd, wait for it to listen ---- */
	reffsd_pid = spawn_reffsd(reffsd, toml, trace, logpath,
				  krb5_env_service_keytab(env));
	if (reffsd_pid < 0) {
		perror("nfs_krb5_multiclient: fork reffsd");
		goto out;
	}
	if (wait_for_listen(port, READY_TIMEOUT_SEC) != 0) {
		fprintf(stderr,
			"nfs_krb5_multiclient: reffsd did not listen on "
			"port %d within %ds (see %s)\n",
			port, READY_TIMEOUT_SEC, logpath);
		goto out;
	}
	fprintf(stderr, "nfs_krb5_multiclient: reffsd up on %s:%d\n",
		server_host, port);

	/* ---- 4. provision principals (one ccache per worker) ---- */
	ccache = calloc(clients, sizeof(*ccache));
	if (!ccache) {
		fprintf(stderr, "nfs_krb5_multiclient: out of memory\n");
		goto out;
	}
	for (int i = 0; i < clients; i++) {
		if (krb5_env_client_ccache(
			    env, i, same_principal, ccache[i].path,
			    sizeof(ccache[i].path)) != KRB5_ENV_OK) {
			fprintf(stderr,
				"nfs_krb5_multiclient: provisioning worker "
				"%d failed\n",
				i);
			goto out;
		}
	}

	/* ---- expect-map (optional) ---- */
	if (expect_map_path) {
		expect_map_n = expect_map_load(expect_map_path, &expect_map);
		if (expect_map_n < 0)
			goto out;
	}

	/* ---- 5. fork N workers ---- */
	wpid = calloc(clients, sizeof(pid_t));
	if (!wpid) {
		fprintf(stderr, "nfs_krb5_multiclient: out of memory\n");
		goto out;
	}

	char server[320];

	snprintf(server, sizeof(server), "%s:%d", server_host, port);

	for (int i = 0; i < clients; i++) {
		pid_t pid = fork();

		if (pid < 0) {
			perror("nfs_krb5_multiclient: fork worker");
			wpid[i] = -1;
			continue;
		}
		if (pid == 0) {
			const char *cc = ccache[i].path;
			char owner[64];
			char princ[256];
			struct krb5_client_args a;
			int r;

			setenv("KRB5CCNAME", cc, 1);
			snprintf(owner, sizeof(owner), "krb5mc-%d-%d",
				 (int)getpid(), i);

			memset(&a, 0, sizeof(a));
			a.server = server;
			a.sec = sec;
			a.owner = owner;
			a.getattr_self = true;
			if (expect_map && krb5_env_client_principal(
						  env, i, same_principal, princ,
						  sizeof(princ)) == KRB5_ENV_OK)
				a.expect_owner = expect_map_lookup(
					expect_map, expect_map_n, princ);

			r = krb5_client_once(&a);
			/*
			 * Flush before _exit: stdout is fully buffered
			 * when it is not a tty, and _exit() does not
			 * flush stdio.  Without this the worker's
			 * per-test PASS/FAIL lines and summary are lost.
			 * _exit (not exit) is still used so the worker
			 * skips the LSan atexit leak scan -- libtirpc /
			 * GSS process-lifetime allocations would
			 * otherwise show as false-positive leaks.
			 */
			fflush(NULL);
			_exit(r);
		}
		wpid[i] = pid;
	}

	/* ---- 6. collect ---- */
	int passed = 0;
	int failed = 0;

	for (int i = 0; i < clients; i++) {
		int status;
		pid_t w;

		if (wpid[i] < 0) {
			failed++;
			continue;
		}
		/*
		 * Retry on EINTR: a signal delivered to the driver
		 * (e.g. SIGCHLD as workers exit) must not abandon the
		 * wait.  Bailing here would fall through to the out:
		 * cleanup and SIGTERM reffsd while other workers are
		 * still mid-test.
		 */
		do {
			w = waitpid(wpid[i], &status, 0);
		} while (w < 0 && errno == EINTR);
		if (w < 0) {
			failed++;
			continue;
		}
		if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
			passed++;
		else
			failed++;
	}

	if (failed == 0) {
		printf("\nPASS %d/%d krb5 clients\n", passed, clients);
		rc = 0;
	} else {
		printf("\nFAIL %d/%d krb5 clients (%d failed)\n", passed,
		       clients, failed);
		rc = 1;
	}

out:
	free(wpid);
	free(ccache);
	free(expect_map);
	if (reffsd_pid > 0) {
		int rstatus = 0;

		kill(reffsd_pid, SIGTERM);
		waitpid(reffsd_pid, &rstatus, 0);

		/*
		 * reffsd installs a SIGTERM handler and exits cleanly, so
		 * a non-zero exit means it reported a problem -- including
		 * an ASan/LSan finding when reffsd is built with the
		 * sanitizers.  Fold that into the result even if every
		 * worker passed.  Death by our own SIGTERM is expected
		 * and not a failure; any other signal is a crash.
		 */
		if (WIFEXITED(rstatus) && WEXITSTATUS(rstatus) != 0) {
			fprintf(stderr,
				"nfs_krb5_multiclient: reffsd exited %d "
				"(sanitizer finding or error)\n",
				WEXITSTATUS(rstatus));
			rc = 1;
		} else if (WIFSIGNALED(rstatus) &&
			   WTERMSIG(rstatus) != SIGTERM) {
			fprintf(stderr,
				"nfs_krb5_multiclient: reffsd killed by "
				"signal %d\n",
				WTERMSIG(rstatus));
			rc = 1;
		}
	}
	if (dir_made) {
		if (rc == 0) {
			char cmd[320];

			snprintf(cmd, sizeof(cmd), "rm -rf %s", reffsd_dir);
			if (system(cmd) != 0)
				fprintf(stderr,
					"nfs_krb5_multiclient: cleanup of "
					"%s failed\n",
					reffsd_dir);
		} else {
			/*
			 * On failure dump reffsd's log to stderr.  Under a
			 * --rm CI container the work dir vanishes with the
			 * container, so echoing the log is the only way the
			 * failure reaches the CI output.  The dir is also
			 * kept, which helps a direct (non-container) run.
			 */
			FILE *lf = fopen(logpath, "r");

			if (lf) {
				char buf[4096];
				size_t n;

				fprintf(stderr, "===== reffsd.log (%s) =====\n",
					logpath);
				while ((n = fread(buf, 1, sizeof(buf), lf)) > 0)
					fwrite(buf, 1, n, stderr);
				fprintf(stderr, "===== end reffsd.log =====\n");
				fclose(lf);
			}
			fprintf(stderr,
				"nfs_krb5_multiclient: run failed -- reffsd "
				"work dir kept: %s\n",
				reffsd_dir);
		}
	}
	krb5_env_stop(env);
	return rc;
}
