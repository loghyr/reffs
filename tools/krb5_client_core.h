/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * krb5_client_core -- one krb5 NFS client run: GSS session
 * establishment, WRITE, READ + CRC verify, optional GETATTR /
 * SETATTR owner round-trip, and cleanup.
 *
 * Shared by:
 *   - nfs_krb5_test         single client, CLI wrapper
 *   - nfs_krb5_multiclient  N forked workers (one identity each)
 *
 * The caller selects the initiator identity by setting KRB5CCNAME
 * in the environment before calling krb5_client_once().
 */

#ifndef _REFFS_KRB5_CLIENT_CORE_H
#define _REFFS_KRB5_CLIENT_CORE_H

#include <stdbool.h>
#include <stdint.h>

#include "ec_client.h"

struct krb5_client_args {
	const char *server; /* NFS server hostname -- required */
	enum ec_sec_flavor sec; /* EC_SEC_KRB5 / KRB5I / KRB5P */
	const char *owner; /* co_ownerid for EXCHANGE_ID; NULL = default */
	const char *file; /* GETATTR owner-test target; NULL skips */
	const char *setowner; /* SETATTR owner round-trip; NULL skips */
	uint32_t block_size; /* WRITE block size; 0 -> default 4096 */
	uint32_t nblocks; /* WRITE block count; 0 -> default 2 */
	bool getattr_self; /* GETATTR the file this run created, after write */
	const char *expect_owner; /* if getattr_self: assert that owner string;
				   * NULL just asserts a non-empty owner */
};

/*
 * Run one krb5 client: GSS session, WRITE, READ + CRC verify,
 * optional owner tests, cleanup.  KRB5CCNAME must already be set in
 * the environment to select the initiator identity.
 *
 * Prints per-test PASS/FAIL progress to stdout.  Returns 0 if every
 * test passed, 1 if any test failed or the session could not be
 * established.
 */
int krb5_client_once(const struct krb5_client_args *a);

/* Security flavor -> "krb5" / "krb5i" / "krb5p" / "sys" for messages. */
const char *krb5_sec_name(enum ec_sec_flavor sec);

#endif /* _REFFS_KRB5_CLIENT_CORE_H */
