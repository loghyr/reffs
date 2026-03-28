/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * mini_kdc — embedded Kerberos KDC for unit testing.
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
 * Stop the KDC and clean up all temporary files.
 */
void mini_kdc_stop(struct mini_kdc *kdc);

#endif /* _REFFS_MINI_KDC_H */
