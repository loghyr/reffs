/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <ftw.h>
#include <urcu.h>

#include "reffs/rcu.h"
#include "reffs/trace/common.h"
#include "reffs/log.h"
#include "libreffs_test.h"

/* Weak symbols to allow linking without all module-specific libraries */
extern void reffs_test_setup_fs(void) __attribute__((weak));
extern void reffs_test_teardown_fs(void) __attribute__((weak));

extern void reffs_test_setup_server(void) __attribute__((weak));
extern void reffs_test_teardown_server(void) __attribute__((weak));

void reffs_test_global_init(void)
{
	rcu_register_thread();
	reffs_trace_init(NULL);
	reffs_trace_enable_all_categories();
	reffs_log_file = stderr;
}

void reffs_test_global_fini(void)
{
	reffs_trace_close();
	synchronize_rcu();
	rcu_barrier();
	rcu_unregister_thread();
}

int reffs_test_run_suite(Suite *s, void (*setup)(void), void (*teardown)(void))
{
	int failed;
	SRunner *sr;

	reffs_test_global_init();

	if (setup)
		setup();

	sr = srunner_create(s);
	srunner_set_fork_status(sr, CK_NOFORK);
	srunner_run_all(sr, CK_NORMAL);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);

	if (teardown)
		teardown();

	reffs_test_global_fini();

	return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}

char *reffs_test_create_state_dir(void)
{
	char template[] = "/tmp/reffs.state.XXXXXX";
	char *path = mkdtemp(template);
	if (path)
		return strdup(path);
	return NULL;
}

static int remove_obj(const char *fpath,
		      const struct stat __attribute__((unused)) * sb,
		      int __attribute__((unused)) typeflag,
		      struct FTW __attribute__((unused)) * ftwbuf)
{
	return remove(fpath);
}

void reffs_test_remove_state_dir(char *path)
{
	if (path) {
		nftw(path, remove_obj, 64, FTW_DEPTH | FTW_PHYS);
		free(path);
	}
}
