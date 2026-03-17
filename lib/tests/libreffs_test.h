/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_TEST_H_
#define _REFFS_TEST_H_

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <check.h>
#include <stdint.h>

/*
 * Standard fixtures for global environment (RCU, tracing, logging)
 */
void reffs_test_global_init(void);
void reffs_test_global_fini(void);

/*
 * Core test runner.
 * s: The test suite to run.
 * setup: Optional per-suite setup function (called before srunner_run_all).
 * teardown: Optional per-suite teardown function (called after srunner_run_all).
 */
int reffs_test_run_suite(Suite *s, void (*setup)(void), void (*teardown)(void));

/* 
 * Optional modules (may be provided by other libraries)
 * We declare them here so common runners can use them if linked.
 */
void reffs_test_setup_fs(void);
void reffs_test_teardown_fs(void);

void reffs_test_setup_server(void);
void reffs_test_teardown_server(void);

/*
 * State directory helpers
 */
char *reffs_test_create_state_dir(void);
void reffs_test_remove_state_dir(char *path);

#endif /* _REFFS_TEST_H_ */
