/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" /* IWYU pragma: keep */
#endif

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

int mini_kdc_start(struct mini_kdc *kdc, const char *service,
		   const char *hostname)
{
	memset(kdc, 0, sizeof(*kdc));

	/* Check prerequisites. */
	if (have_cmd("krb5kdc") != 0 || have_cmd("kdb5_util") != 0 ||
	    have_cmd("kadmin.local") != 0 || have_cmd("kinit") != 0) {
		fprintf(stderr, "mini_kdc: krb5 tools not found — skipping\n");
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
		"        kdc = localhost\n"
		"        admin_server = localhost\n"
		"    }\n",
		MINI_KDC_REALM, MINI_KDC_REALM);
	fclose(fp);

	/* Write kdc.conf. */
	snprintf(path, sizeof(path), "%s/kdc.conf", kdc->kdc_dir);
	fp = fopen(path, "w");
	if (!fp)
		goto err;
	fprintf(fp,
		"[kdcdefaults]\n"
		"    kdc_ports = 0\n"
		"    kdc_tcp_ports = 0\n"
		"[realms]\n"
		"    %s = {\n"
		"        database_name = %s/principal\n"
		"        key_stash_file = %s/.k5.%s\n"
		"        kdc_ports = 0\n"
		"        kdc_tcp_ports = 0\n"
		"    }\n",
		MINI_KDC_REALM, kdc->kdc_dir, kdc->kdc_dir, MINI_KDC_REALM);
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
