/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" /* IWYU pragma: keep */
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "mini_kdc.h"

/* Run a shell command, return 0 on success. */
static int run(const char *cmd)
{
	int rc = system(cmd);

	return WIFEXITED(rc) && WEXITSTATUS(rc) == 0 ? 0 : -1;
}

/* Check if a command exists in PATH. */
static int have_cmd(const char *cmd)
{
	char check[512];

	snprintf(check, sizeof(check), "command -v %s >/dev/null 2>&1", cmd);
	return run(check);
}

/*
 * Grab a free loopback TCP port for the KDC to bind.  The port must
 * be discoverable by the client, so it is written into both
 * kdc.conf (where krb5kdc binds) and krb5.conf (where kinit looks).
 * Returns the port, or -1 on failure.
 *
 * There is a small TOCTOU window between closing the probe socket
 * and krb5kdc binding the port -- acceptable for a test fixture.
 */
static int find_free_port(void)
{
	struct sockaddr_in sa;
	socklen_t len = sizeof(sa);
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	int port = -1;

	if (fd < 0)
		return -1;

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sa.sin_port = 0;

	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0 &&
	    getsockname(fd, (struct sockaddr *)&sa, &len) == 0)
		port = ntohs(sa.sin_port);

	close(fd);
	return port;
}

int mini_kdc_start(struct mini_kdc *kdc, const char *service,
		   const char *hostname)
{
	memset(kdc, 0, sizeof(*kdc));

	/* Check prerequisites. */
	if (have_cmd("krb5kdc") != 0 || have_cmd("kdb5_util") != 0 ||
	    have_cmd("kadmin.local") != 0 || have_cmd("kinit") != 0) {
		fprintf(stderr, "mini_kdc: krb5 tools not found -- skipping\n");
		return -1;
	}

	/* Create temporary directory. */
	snprintf(kdc->kdc_dir, sizeof(kdc->kdc_dir), "/tmp/mini_kdc_XXXXXX");
	if (!mkdtemp(kdc->kdc_dir)) {
		perror("mini_kdc: mkdtemp");
		return -1;
	}

	snprintf(kdc->kdc_keytab, sizeof(kdc->kdc_keytab), "%s/keytab",
		 kdc->kdc_dir);
	snprintf(kdc->kdc_ccache, sizeof(kdc->kdc_ccache), "%s/ccache",
		 kdc->kdc_dir);
	snprintf(kdc->kdc_krb5conf, sizeof(kdc->kdc_krb5conf), "%s/krb5.conf",
		 kdc->kdc_dir);

	/*
	 * Pick the KDC's port up front: it has to agree between
	 * kdc.conf (krb5kdc binds it) and krb5.conf (kinit dials it).
	 */
	int kdc_port = find_free_port();

	if (kdc_port < 0) {
		fprintf(stderr, "mini_kdc: no free port for the KDC\n");
		goto err;
	}

	/* Write krb5.conf. */
	char path[512];

	snprintf(path, sizeof(path), "%s/krb5.conf", kdc->kdc_dir);
	FILE *fp = fopen(path, "w");

	if (!fp)
		goto err;
	fprintf(fp,
		"[libdefaults]\n"
		"    default_realm = %s\n"
		"    dns_lookup_realm = false\n"
		"    dns_lookup_kdc = false\n"
		"    rdns = false\n"
		"[realms]\n"
		"    %s = {\n"
		"        kdc = localhost:%d\n"
		"        admin_server = localhost\n"
		"    }\n",
		MINI_KDC_REALM, MINI_KDC_REALM, kdc_port);
	fclose(fp);

	/* Write kdc.conf. */
	snprintf(path, sizeof(path), "%s/kdc.conf", kdc->kdc_dir);
	fp = fopen(path, "w");
	if (!fp)
		goto err;
	fprintf(fp,
		"[kdcdefaults]\n"
		"    kdc_ports = %d\n"
		"    kdc_tcp_ports = %d\n"
		"[realms]\n"
		"    %s = {\n"
		"        database_name = %s/principal\n"
		"        key_stash_file = %s/.k5.%s\n"
		"        kdc_ports = %d\n"
		"        kdc_tcp_ports = %d\n"
		"    }\n",
		kdc_port, kdc_port, MINI_KDC_REALM, kdc->kdc_dir, kdc->kdc_dir,
		MINI_KDC_REALM, kdc_port, kdc_port);
	fclose(fp);

	/* Set environment for all krb5 commands. */
	setenv("KRB5_CONFIG", kdc->kdc_krb5conf, 1);
	setenv("KRB5_KDC_PROFILE", path, 1);
	setenv("KRB5CCNAME", kdc->kdc_ccache, 1);

	/* Create realm database. */
	char cmd[1024];

	snprintf(cmd, sizeof(cmd),
		 "kdb5_util create -s -r %s -P masterpass 2>/dev/null",
		 MINI_KDC_REALM);
	if (run(cmd))
		goto err;

	/* Create service principal + keytab. */
	snprintf(cmd, sizeof(cmd),
		 "kadmin.local -r %s addprinc -randkey %s/%s@%s 2>/dev/null",
		 MINI_KDC_REALM, service, hostname, MINI_KDC_REALM);
	if (run(cmd))
		goto err;

	snprintf(cmd, sizeof(cmd),
		 "kadmin.local -r %s ktadd -k %s %s/%s@%s 2>/dev/null",
		 MINI_KDC_REALM, kdc->kdc_keytab, service, hostname,
		 MINI_KDC_REALM);
	if (run(cmd))
		goto err;

	/* Create test user. */
	snprintf(cmd, sizeof(cmd),
		 "kadmin.local -r %s addprinc -pw %s testuser@%s 2>/dev/null",
		 MINI_KDC_REALM, MINI_KDC_PASS, MINI_KDC_REALM);
	if (run(cmd))
		goto err;

	/* Start KDC. */
	snprintf(cmd, sizeof(cmd), "krb5kdc -n -P %s/kdc.pid 2>/dev/null &",
		 kdc->kdc_dir);
	if (run(cmd))
		goto err;

	/* Read the PID. */
	usleep(500000); /* give krb5kdc time to write PID file */
	snprintf(path, sizeof(path), "%s/kdc.pid", kdc->kdc_dir);
	fp = fopen(path, "r");
	if (fp) {
		if (fscanf(fp, "%d", &kdc->kdc_pid) == 1)
			kdc->kdc_started = true;
		fclose(fp);
	}

	if (!kdc->kdc_started) {
		fprintf(stderr, "mini_kdc: krb5kdc did not start\n");
		goto err;
	}

	/* Get a TGT for the test user. */
	snprintf(cmd, sizeof(cmd), "echo %s | kinit testuser@%s 2>/dev/null",
		 MINI_KDC_PASS, MINI_KDC_REALM);
	if (run(cmd)) {
		fprintf(stderr, "mini_kdc: kinit failed\n");
		goto err;
	}

	fprintf(stderr, "mini_kdc: realm=%s keytab=%s ccache=%s pid=%d\n",
		MINI_KDC_REALM, kdc->kdc_keytab, kdc->kdc_ccache, kdc->kdc_pid);
	return 0;

err:
	mini_kdc_stop(kdc);
	return -1;
}

int mini_kdc_kinit(struct mini_kdc *kdc, const char *principal, const char *tag,
		   char *ccache_out, size_t ccache_sz)
{
	char cmd[1024];
	char ccache[320];

	if (!kdc->kdc_started) {
		fprintf(stderr, "mini_kdc: kinit before KDC started\n");
		return -1;
	}

	/*
	 * Dedicated credential cache inside the KDC temp tree, so
	 * mini_kdc_stop's rm -rf reaps it.  The name is keyed on @tag
	 * (not the principal) so several caches can hold tickets for
	 * the same principal.  kinit -c targets this cache explicitly
	 * rather than the process-global one set by mini_kdc_start.
	 */
	snprintf(ccache, sizeof(ccache), "%s/cc_%s", kdc->kdc_dir, tag);

	snprintf(cmd, sizeof(cmd), "echo %s | kinit -c %s %s@%s 2>/dev/null",
		 MINI_KDC_PASS, ccache, principal, MINI_KDC_REALM);
	if (run(cmd)) {
		fprintf(stderr, "mini_kdc: kinit %s (%s) failed\n", principal,
			tag);
		return -1;
	}

	snprintf(ccache_out, ccache_sz, "%s", ccache);
	return 0;
}

int mini_kdc_add_user(struct mini_kdc *kdc, const char *name, char *ccache_out,
		      size_t ccache_sz)
{
	char cmd[1024];

	if (!kdc->kdc_started) {
		fprintf(stderr, "mini_kdc: add_user before KDC started\n");
		return -1;
	}

	/* Create the user principal with the shared test password. */
	snprintf(cmd, sizeof(cmd),
		 "kadmin.local -r %s addprinc -pw %s %s@%s 2>/dev/null",
		 MINI_KDC_REALM, MINI_KDC_PASS, name, MINI_KDC_REALM);
	if (run(cmd)) {
		fprintf(stderr, "mini_kdc: addprinc %s failed\n", name);
		return -1;
	}

	return mini_kdc_kinit(kdc, name, name, ccache_out, ccache_sz);
}

void mini_kdc_stop(struct mini_kdc *kdc)
{
	if (kdc->kdc_pid > 0) {
		kill(kdc->kdc_pid, SIGTERM);
		waitpid(kdc->kdc_pid, NULL, 0);
		kdc->kdc_pid = 0;
	}
	kdc->kdc_started = false;

	/* Clean up the temp directory. */
	if (kdc->kdc_dir[0]) {
		char cmd[512];

		snprintf(cmd, sizeof(cmd), "rm -rf %s", kdc->kdc_dir);
		system(cmd);
		kdc->kdc_dir[0] = '\0';
	}

	/* Unset environment. */
	unsetenv("KRB5_CONFIG");
	unsetenv("KRB5_KDC_PROFILE");
	unsetenv("KRB5CCNAME");
}
