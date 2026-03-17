/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_TEST_NFS4_HARNESS_H
#define _REFFS_TEST_NFS4_HARNESS_H

#include "libreffs_test.h"
#include "fs_test_harness.h"
#include "reffs/nfs4.h"

static inline void nfs4_test_setup(void)
{
	reffs_test_setup_server();
	fs_test_setup();
	nfs4_protocol_register();
}

static inline void nfs4_test_teardown(void)
{
	nfs4_protocol_deregister();
	fs_test_teardown();
	reffs_test_teardown_server();
}

/* Run an nfs4 test suite - bare by default */
static inline int nfs4_test_run(Suite *s)
{
	return reffs_test_run_suite(s, NULL, NULL);
}

#endif /* _REFFS_TEST_NFS4_HARNESS_H */
