/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * krb5_env -- the Kerberos environment a krb5 NFS test driver runs
 * against, behind one provider-agnostic interface.
 *
 * The embedded provider stands up a private mini-KDC (realm
 * TEST.REFFS) and owns its whole lifecycle -- the self-contained
 * mode.  A future external provider will consume a pre-provisioned
 * KDC (a real AD-joined realm); see the External-KDC mode section of
 * .claude/design/krb5-multiclient-test.md.
 */

#ifndef _REFFS_KRB5_ENV_H
#define _REFFS_KRB5_ENV_H

#include <stdbool.h>
#include <stddef.h>

struct krb5_env;

/* krb5_env_start / krb5_env_client_ccache return codes. */
#define KRB5_ENV_OK 0
#define KRB5_ENV_SKIP 1 /* environment unavailable; caller should skip */
#define KRB5_ENV_ERR (-1)

/*
 * Create the embedded provider.  krb5_env_start will spin up a
 * private mini-KDC with service principal nfs/localhost@TEST.REFFS.
 * Returns NULL on allocation failure.
 */
struct krb5_env *krb5_env_embedded(void);

/*
 * Configuration for the external provider: a pre-provisioned KDC,
 * typically a real AD-joined realm.  The strings are not copied --
 * they must outlive the krb5_env.
 *
 * @principals_file lists one "<principal> <password>" pair per line
 * (blank lines and #-comments ignored); principals are fully
 * qualified (user@REALM) and passwords must be free of single
 * quotes.  The provider kinit's each into its own credential cache.
 */
struct krb5_env_external_cfg {
	const char *krb5_conf; /* krb5.conf -> KRB5_CONFIG */
	const char *service_keytab; /* keytab for reffsd's nfs/<host> SPN */
	const char *principals_file; /* "<principal> <password>" per line */
};

/*
 * Create the external provider.  @cfg's contents are copied (the
 * pointers it holds are not).  Returns NULL on allocation failure.
 */
struct krb5_env *krb5_env_external(const struct krb5_env_external_cfg *cfg);

/*
 * Provision the realm.  On success the realm's krb5 config is placed
 * in the process environment (KRB5_CONFIG etc.), so a subsequently
 * forked reffsd and the worker processes inherit it.
 *
 * Returns KRB5_ENV_OK, KRB5_ENV_SKIP (the krb5 environment is not
 * available -- the caller should skip, not fail), or KRB5_ENV_ERR.
 */
int krb5_env_start(struct krb5_env *env);

/*
 * Path to the service keytab for reffsd's KRB5_KTNAME.  Valid only
 * after a successful krb5_env_start.
 */
const char *krb5_env_service_keytab(const struct krb5_env *env);

/*
 * Provision client worker @idx and write its credential cache path
 * (to be passed as KRB5CCNAME) into @ccache_out.  With
 * @same_principal every worker authenticates as one identity but
 * still gets its own cache -- the MIT krb5 FILE: ccache is not safe
 * for concurrent use by multiple processes.
 *
 * Returns KRB5_ENV_OK or KRB5_ENV_ERR.
 */
int krb5_env_client_ccache(struct krb5_env *env, int idx, bool same_principal,
			   char *ccache_out, size_t ccache_sz);

/*
 * Write the krb5 principal that worker @idx authenticates as into
 * @out (fully qualified, user@REALM).  With @same_principal every
 * worker shares one identity.  Lets the caller key a principal ->
 * expected-owner lookup.  Returns KRB5_ENV_OK or KRB5_ENV_ERR.
 */
int krb5_env_client_principal(const struct krb5_env *env, int idx,
			      bool same_principal, char *out, size_t out_sz);

/* Tear the environment down and free @env.  NULL-tolerant. */
void krb5_env_stop(struct krb5_env *env);

#endif /* _REFFS_KRB5_ENV_H */
