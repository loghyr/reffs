/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Unit tests for the task state machine primitives:
 *
 *   task_pause()      -- RUNNING -> PAUSED (atomic CAS)
 *   task_resume()     -- PAUSED  -> RUNNING + add_task()
 *   task_is_paused()  -- predicate on PAUSED state
 *
 * These tests exercise only the state transitions.  No worker threads are
 * running, so tasks enqueued by task_resume() sit in the queue harmlessly
 * until the process exits.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <stdatomic.h>
#include <stdbool.h>

#include <check.h>

#include "reffs/task.h"
#include "nfs4_test_harness.h"

/* Allocate a minimal task with the given initial state. */
static struct task *make_task(enum task_state initial)
{
	struct task *t = calloc(1, sizeof(*t));
	ck_assert_ptr_nonnull(t);
	atomic_store_explicit(&t->t_state, initial, memory_order_relaxed);
	return t;
}

/* ------------------------------------------------------------------ */
/* task_pause tests                                                    */
/* ------------------------------------------------------------------ */

START_TEST(test_pause_running_succeeds)
{
	struct task *t = make_task(TASK_RUNNING);
	bool ok = task_pause(t);
	ck_assert(ok);
	ck_assert_int_eq(atomic_load(&t->t_state), TASK_PAUSED);
	free(t);
}
END_TEST

START_TEST(test_pause_sets_is_paused)
{
	struct task *t = make_task(TASK_RUNNING);
	ck_assert(!task_is_paused(t));
	task_pause(t);
	ck_assert(task_is_paused(t));
	free(t);
}
END_TEST

START_TEST(test_pause_idle_fails)
{
	struct task *t = make_task(TASK_IDLE);
	bool ok = task_pause(t);
	ck_assert(!ok);
	ck_assert_int_eq(atomic_load(&t->t_state), TASK_IDLE);
	free(t);
}
END_TEST

START_TEST(test_pause_done_fails)
{
	struct task *t = make_task(TASK_DONE);
	bool ok = task_pause(t);
	ck_assert(!ok);
	ck_assert_int_eq(atomic_load(&t->t_state), TASK_DONE);
	free(t);
}
END_TEST

START_TEST(test_double_pause_second_fails)
{
	struct task *t = make_task(TASK_RUNNING);
	ck_assert(task_pause(t));
	bool ok = task_pause(t);
	ck_assert(!ok);
	ck_assert_int_eq(atomic_load(&t->t_state), TASK_PAUSED);
	free(t);
}
END_TEST

/* ------------------------------------------------------------------ */
/* task_resume tests                                                   */
/* ------------------------------------------------------------------ */

START_TEST(test_resume_paused_succeeds)
{
	struct task *t = make_task(TASK_RUNNING);
	ck_assert(task_pause(t));
	ck_assert(task_is_paused(t));
	/* task_resume transitions PAUSED -> RUNNING and calls add_task().
	 * No worker is running; the task sits in the queue until process exit. */
	task_resume(t);
	ck_assert_int_eq(atomic_load(&t->t_state), TASK_RUNNING);
	ck_assert(!task_is_paused(t));
	/* Do not free t: it is now owned by the task queue. */
}
END_TEST

START_TEST(test_resume_running_noop)
{
	/* Calling resume on a non-paused task must not change state. */
	struct task *t = make_task(TASK_RUNNING);
	task_resume(t); /* CAS fails: expected PAUSED, found RUNNING */
	ck_assert_int_eq(atomic_load(&t->t_state), TASK_RUNNING);
	free(t);
}
END_TEST

START_TEST(test_resume_idle_noop)
{
	struct task *t = make_task(TASK_IDLE);
	task_resume(t);
	ck_assert_int_eq(atomic_load(&t->t_state), TASK_IDLE);
	free(t);
}
END_TEST

/* ------------------------------------------------------------------ */
/* task_is_paused tests                                                */
/* ------------------------------------------------------------------ */

START_TEST(test_is_paused_false_for_running)
{
	struct task *t = make_task(TASK_RUNNING);
	ck_assert(!task_is_paused(t));
	free(t);
}
END_TEST

START_TEST(test_is_paused_false_for_idle)
{
	struct task *t = make_task(TASK_IDLE);
	ck_assert(!task_is_paused(t));
	free(t);
}
END_TEST

START_TEST(test_is_paused_false_for_done)
{
	struct task *t = make_task(TASK_DONE);
	ck_assert(!task_is_paused(t));
	free(t);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite assembly                                                      */
/* ------------------------------------------------------------------ */

Suite *task_state_suite(void)
{
	Suite *s = suite_create("Task State Machine");

	TCase *tc_pause = tcase_create("pause");
	tcase_add_test(tc_pause, test_pause_running_succeeds);
	tcase_add_test(tc_pause, test_pause_sets_is_paused);
	tcase_add_test(tc_pause, test_pause_idle_fails);
	tcase_add_test(tc_pause, test_pause_done_fails);
	tcase_add_test(tc_pause, test_double_pause_second_fails);
	suite_add_tcase(s, tc_pause);

	TCase *tc_resume = tcase_create("resume");
	tcase_add_test(tc_resume, test_resume_paused_succeeds);
	tcase_add_test(tc_resume, test_resume_running_noop);
	tcase_add_test(tc_resume, test_resume_idle_noop);
	suite_add_tcase(s, tc_resume);

	TCase *tc_pred = tcase_create("is_paused");
	tcase_add_test(tc_pred, test_is_paused_false_for_running);
	tcase_add_test(tc_pred, test_is_paused_false_for_idle);
	tcase_add_test(tc_pred, test_is_paused_false_for_done);
	suite_add_tcase(s, tc_pred);

	return s;
}

int main(void)
{
	return nfs4_test_run(task_state_suite());
}
