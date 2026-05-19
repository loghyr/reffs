/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * krb5_env -- Kerberos environment provider for the krb5 NFS test
 * drivers.  Two providers behind one interface:
 *
 *   embedded -- stands up a private mini-KDC (the self-contained CI
 *               mode); wraps the mini_kdc fixture.
 *   external -- consumes a pre-provisioned KDC (a real AD-joined
 *               realm): a krb5.conf, a service keytab for reffsd, and
 *               a file of client principals + passwords to kinit.
 *
 * See the External-KDC mode section of
 * .claude/design/krb5-multiclient-test.md.
 */

#ifdef HAVE_CONFIG_H
#include "config.h" /* IWYU pragma: keep */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "mini_kdc.h"
#include "krb5_env.h"

#define KRB5_ENV_PRINC_MAX 256
#define KRB5_ENV_PW_MAX 128

enum krb5_env_kind {
	KRB5_ENV_EMBEDDED,
	KRB5_ENV_EXTERNAL,
};

struct krb5_env_principal {
	char kp_name[KRB5_ENV_PRINC_MAX];
	char kp_pw[KRB5_ENV_PW_MAX];
};

struct krb5_env {
	enum krb5_env_kind ke_kind;

	/* KRB5_ENV_EMBEDDED */
	struct mini_kdc ke_kdc;
	bool ke_kdc_up;

	/* KRB5_ENV_EXTERNAL */
	struct krb5_env_external_cfg ke_ext;
	char ke_ext_ccdir[64]; /* mkdtemp'd dir for per-worker ccaches */
	bool ke_ext_ccdir_made;
	struct krb5_env_principal *ke_ext_princ;
	int ke_ext_nprinc;
};

/* Run a shell command; return 0 on success. */
static int run(const char *cmd)
{
	int rc = system(cmd);

	return WIFEXITED(rc) && WEXITSTATUS(rc) == 0 ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Embedded provider                                                   */

struct krb5_env *krb5_env_embedded(void)
{
	struct krb5_env *env = calloc(1, sizeof(*env));

	if (env)
		env->ke_kind = KRB5_ENV_EMBEDDED;
	return env;
}

static int krb5_env_embedded_start(struct krb5_env *env)
{
	/*
	 * mini_kdc_start returns -1 both when the krb5 server tools are
	 * absent and on a genuine failure; either way the test cannot
	 * run, so report SKIP and let the caller exit accordingly
	 * rather than failing the run.
	 */
	if (mini_kdc_start(&env->ke_kdc, "nfs", "localhost") != 0)
		return KRB5_ENV_SKIP;
	env->ke_kdc_up = true;
	return KRB5_ENV_OK;
}

static int krb5_env_embedded_client_ccache(struct krb5_env *env, int idx,
					   bool same_principal,
					   char *ccache_out, size_t ccache_sz)
{
	/*
	 * Distinct-principal: worker @idx authenticates as ecuser<idx>.
	 * Same-principal: every worker authenticates as ecuser0 -- worker
	 * 0 creates the principal, the rest kinit a fresh ticket for it
	 * into their own tagged cache.
	 */
	if (same_principal && idx > 0) {
		char tag[32];

		snprintf(tag, sizeof(tag), "sp%d", idx);
		if (mini_kdc_kinit(&env->ke_kdc, "ecuser0", tag, ccache_out,
				   ccache_sz) != 0)
			return KRB5_ENV_ERR;
	} else {
		char name[64];

		snprintf(name, sizeof(name), "ecuser%d",
			 same_principal ? 0 : idx);
		if (mini_kdc_add_user(&env->ke_kdc, name, ccache_out,
				      ccache_sz) != 0)
			return KRB5_ENV_ERR;
	}
	return KRB5_ENV_OK;
}

/* ------------------------------------------------------------------ */
/* External provider                                                   */

struct krb5_env *krb5_env_external(const struct krb5_env_external_cfg *cfg)
{
	struct krb5_env *env = calloc(1, sizeof(*env));

	if (env) {
		env->ke_kind = KRB5_ENV_EXTERNAL;
		env->ke_ext = *cfg;
	}
	return env;
}

/* Parse the "<principal> <password>" file into ke_ext_princ[]. */
static int krb5_env_load_principals(struct krb5_env *env)
{
	char line[KRB5_ENV_PRINC_MAX + KRB5_ENV_PW_MAX + 8];
	FILE *fp = fopen(env->ke_ext.principals_file, "r");
	int cap = 0;

	if (!fp) {
		fprintf(stderr, "krb5_env: cannot open principals file: %s\n",
			env->ke_ext.principals_file);
		return -1;
	}
	while (fgets(line, sizeof(line), fp)) {
		char name[KRB5_ENV_PRINC_MAX];
		char pw[KRB5_ENV_PW_MAX];

		if (line[0] == '#' || line[0] == '\n')
			continue;
		if (sscanf(line, "%255s %127s", name, pw) != 2) {
			fprintf(stderr,
				"krb5_env: malformed principals line: %s",
				line);
			fclose(fp);
			return -1;
		}
		if (strchr(pw, '\'')) {
			fprintf(stderr,
				"krb5_env: %s: password contains a single "
				"quote, which the kinit shell pipeline "
				"cannot carry\n",
				name);
			fclose(fp);
			return -1;
		}
		if (env->ke_ext_nprinc == cap) {
			int ncap = cap ? cap * 2 : 16;
			struct krb5_env_principal *p = realloc(
				env->ke_ext_princ, (size_t)ncap * sizeof(*p));

			if (!p) {
				fclose(fp);
				return -1;
			}
			env->ke_ext_princ = p;
			cap = ncap;
		}
		snprintf(env->ke_ext_princ[env->ke_ext_nprinc].kp_name,
			 KRB5_ENV_PRINC_MAX, "%s", name);
		snprintf(env->ke_ext_princ[env->ke_ext_nprinc].kp_pw,
			 KRB5_ENV_PW_MAX, "%s", pw);
		env->ke_ext_nprinc++;
	}
	fclose(fp);
	if (env->ke_ext_nprinc == 0) {
		fprintf(stderr, "krb5_env: principals file is empty: %s\n",
			env->ke_ext.principals_file);
		return -1;
	}
	return 0;
}

static int krb5_env_external_start(struct krb5_env *env)
{
	const struct krb5_env_external_cfg *cfg = &env->ke_ext;

	if (!cfg->krb5_conf || !cfg->service_keytab || !cfg->principals_file) {
		fprintf(stderr, "krb5_env: external mode needs a krb5.conf, "
				"a service keytab and a principals file\n");
		return KRB5_ENV_ERR;
	}
	if (access(cfg->krb5_conf, R_OK) != 0) {
		fprintf(stderr, "krb5_env: krb5.conf not readable: %s\n",
			cfg->krb5_conf);
		return KRB5_ENV_ERR;
	}
	if (access(cfg->service_keytab, R_OK) != 0) {
		fprintf(stderr, "krb5_env: service keytab not readable: %s\n",
			cfg->service_keytab);
		return KRB5_ENV_ERR;
	}
	if (krb5_env_load_principals(env) != 0)
		return KRB5_ENV_ERR;

	/* reffsd and the workers inherit the realm config. */
	setenv("KRB5_CONFIG", cfg->krb5_conf, 1);

	/* Per-worker credential caches live in a private temp dir. */
	snprintf(env->ke_ext_ccdir, sizeof(env->ke_ext_ccdir),
		 "/tmp/krb5env_XXXXXX");
	if (!mkdtemp(env->ke_ext_ccdir)) {
		perror("krb5_env: mkdtemp");
		return KRB5_ENV_ERR;
	}
	env->ke_ext_ccdir_made = true;
	return KRB5_ENV_OK;
}

static int krb5_env_external_client_ccache(struct krb5_env *env, int idx,
					   bool same_principal,
					   char *ccache_out, size_t ccache_sz)
{
	int which = same_principal ? 0 : idx;
	const struct krb5_env_principal *p;
	char ccache[320];
	char cmd[1024];

	if (which >= env->ke_ext_nprinc) {
		fprintf(stderr,
			"krb5_env: worker %d needs principal %d, but the "
			"principals file has only %d\n",
			idx, which, env->ke_ext_nprinc);
		return KRB5_ENV_ERR;
	}
	p = &env->ke_ext_princ[which];

	snprintf(ccache, sizeof(ccache), "%s/cc_%d", env->ke_ext_ccdir, idx);
	snprintf(cmd, sizeof(cmd), "echo '%s' | kinit -c %s %s 2>/dev/null",
		 p->kp_pw, ccache, p->kp_name);
	if (run(cmd) != 0) {
		fprintf(stderr, "krb5_env: kinit %s failed\n", p->kp_name);
		return KRB5_ENV_ERR;
	}
	snprintf(ccache_out, ccache_sz, "%s", ccache);
	return KRB5_ENV_OK;
}

static void krb5_env_external_stop(struct krb5_env *env)
{
	free(env->ke_ext_princ);
	if (env->ke_ext_ccdir_made) {
		char cmd[128];

		snprintf(cmd, sizeof(cmd), "rm -rf %s", env->ke_ext_ccdir);
		if (system(cmd) != 0)
			fprintf(stderr, "krb5_env: cleanup of %s failed\n",
				env->ke_ext_ccdir);
		unsetenv("KRB5_CONFIG");
	}
}

/* ------------------------------------------------------------------ */
/* Provider-agnostic interface                                          */

int krb5_env_start(struct krb5_env *env)
{
	switch (env->ke_kind) {
	case KRB5_ENV_EMBEDDED:
		return krb5_env_embedded_start(env);
	case KRB5_ENV_EXTERNAL:
		return krb5_env_external_start(env);
	}
	return KRB5_ENV_ERR;
}

const char *krb5_env_service_keytab(const struct krb5_env *env)
{
	switch (env->ke_kind) {
	case KRB5_ENV_EMBEDDED:
		return env->ke_kdc.kdc_keytab;
	case KRB5_ENV_EXTERNAL:
		return env->ke_ext.service_keytab;
	}
	return NULL;
}

int krb5_env_client_ccache(struct krb5_env *env, int idx, bool same_principal,
			   char *ccache_out, size_t ccache_sz)
{
	switch (env->ke_kind) {
	case KRB5_ENV_EMBEDDED:
		return krb5_env_embedded_client_ccache(env, idx, same_principal,
						       ccache_out, ccache_sz);
	case KRB5_ENV_EXTERNAL:
		return krb5_env_external_client_ccache(env, idx, same_principal,
						       ccache_out, ccache_sz);
	}
	return KRB5_ENV_ERR;
}

int krb5_env_client_principal(const struct krb5_env *env, int idx,
			      bool same_principal, char *out, size_t out_sz)
{
	int which = same_principal ? 0 : idx;

	switch (env->ke_kind) {
	case KRB5_ENV_EMBEDDED:
		snprintf(out, out_sz, "ecuser%d@%s", which, MINI_KDC_REALM);
		return KRB5_ENV_OK;
	case KRB5_ENV_EXTERNAL:
		if (which >= env->ke_ext_nprinc)
			return KRB5_ENV_ERR;
		snprintf(out, out_sz, "%s", env->ke_ext_princ[which].kp_name);
		return KRB5_ENV_OK;
	}
	return KRB5_ENV_ERR;
}

void krb5_env_stop(struct krb5_env *env)
{
	if (!env)
		return;
	switch (env->ke_kind) {
	case KRB5_ENV_EMBEDDED:
		if (env->ke_kdc_up)
			mini_kdc_stop(&env->ke_kdc);
		break;
	case KRB5_ENV_EXTERNAL:
		krb5_env_external_stop(env);
		break;
	}
	free(env);
}
