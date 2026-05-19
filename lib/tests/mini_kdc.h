/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * mini_kdc -- embedded Kerberos KDC for unit testing.
 *
 * Creates a temporary realm with a service principal and test user,
 * starts krb5kdc, and obtains a TGT.  Teardown kills the KDC and
 * cleans up all files.
 *
 * Usage in a test:
 *   struct mini_kdc kdc;
 *   if (mini_kdc_start(&kdc, "nfs", "localhost") == 0) {
 *       // kdc.kdc_keytab has the keytab path
 *       // kdc.kdc_ccache has the credential cache path
 *       // run tests...
 *       mini_kdc_stop(&kdc);
 *   }
 *
 * Requires: krb5-kdc, krb5-admin-server (Ubuntu) or
 *           krb5-server (Fedora) installed.
 */

#ifndef _REFFS_MINI_KDC_H
#define _REFFS_MINI_KDC_H

#include <stdbool.h>
#include <sys/types.h>

#define MINI_KDC_REALM "TEST.REFFS"
#define MINI_KDC_PASS "testpass"

struct mini_kdc {
	char kdc_dir[256]; /* temporary directory */
	char kdc_keytab[280]; /* keytab path */
	char kdc_ccache[280]; /* credential cache path */
	char kdc_krb5conf[280]; /* krb5.conf path */
	pid_t kdc_pid; /* krb5kdc process */
	bool kdc_started;
};

/*
 * Start a mini KDC.  Creates realm TEST.REFFS, service principal
 * <service>/<hostname>@TEST.REFFS, and test user testuser@TEST.REFFS.
 *
 * Returns 0 on success, -1 if krb5kdc is not available (test should
 * be skipped, not failed).
 */
int mini_kdc_start(struct mini_kdc *kdc, const char *service,
		   const char *hostname);

/*
 * Provision an additional user principal <name>@TEST.REFFS on an
 * already-started KDC and kinit it into a dedicated credential
 * cache.  ccache_out receives that cache's path; hand it to a
 * worker as KRB5CCNAME to select this identity.
 *
 * The cache lives inside the KDC's temp directory, so mini_kdc_stop
 * reaps it along with everything else.
 *
 * Returns 0 on success, -1 on failure (KDC not started, addprinc
 * or kinit failed).
 */
int mini_kdc_add_user(struct mini_kdc *kdc, const char *name, char *ccache_out,
		      size_t ccache_sz);

/*
 * Obtain a fresh TGT for an already-existing principal into its own
 * dedicated credential cache.  Unlike mini_kdc_add_user this does not
 * create the principal.  @tag makes the cache file name unique, so
 * several caches can each hold a ticket for the same principal: the
 * MIT krb5 FILE: ccache is not safe for concurrent use by multiple
 * processes, so callers that fork many workers under one identity
 * must give each worker its own cache.
 *
 * Returns 0 on success, -1 on failure (KDC not started, kinit failed).
 */
int mini_kdc_kinit(struct mini_kdc *kdc, const char *principal, const char *tag,
		   char *ccache_out, size_t ccache_sz);

/*
 * Stop the KDC and clean up all temporary files.
 */
void mini_kdc_stop(struct mini_kdc *kdc);

#endif /* _REFFS_MINI_KDC_H */
