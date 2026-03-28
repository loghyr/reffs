/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * Unit tests for GSS sequence replay detection (RFC 2203 §5.2.1).
 *
 * Tests the 128-bit sliding window bitmap in gss_ctx_seq_check()
 * without requiring a Kerberos environment.
 */

#ifdef HAVE_CONFIG_H
#include "config.h" /* IWYU pragma: keep */
#endif

#include <stdlib.h>

#include <urcu.h>
#include <check.h>

#include "reffs/gss_context.h"

/*
 * Create a minimal gss_ctx_entry for testing.
 * Only the seq fields and mutex are needed.
 */
static struct gss_ctx_entry *make_test_ctx(void)
{
	struct gss_ctx_entry *e = calloc(1, sizeof(*e));

	ck_assert_ptr_nonnull(e);
	e->gc_seq_window = GSS_SEQ_WINDOW;
	pthread_mutex_init(&e->gc_seq_lock, NULL);
	return e;
}

static void free_test_ctx(struct gss_ctx_entry *e)
{
	pthread_mutex_destroy(&e->gc_seq_lock);
	free(e);
}

/* Sequence number 0 is never valid (RFC 2203: seqs start at 1). */
START_TEST(test_seq_zero_rejected)
{
	struct gss_ctx_entry *e = make_test_ctx();

	ck_assert_int_ne(gss_ctx_seq_check(e, 0), 0);

	/* Also reject after advancing the window. */
	ck_assert_int_eq(gss_ctx_seq_check(e, 1), 0);
	ck_assert_int_ne(gss_ctx_seq_check(e, 0), 0);

	free_test_ctx(e);
}
END_TEST

/* Sequential sequence numbers should all be accepted. */
START_TEST(test_sequential)
{
	struct gss_ctx_entry *e = make_test_ctx();

	for (uint32_t i = 1; i <= 256; i++)
		ck_assert_int_eq(gss_ctx_seq_check(e, i), 0);

	free_test_ctx(e);
}
END_TEST

/* Duplicate sequence number must be rejected. */
START_TEST(test_duplicate_rejected)
{
	struct gss_ctx_entry *e = make_test_ctx();

	ck_assert_int_eq(gss_ctx_seq_check(e, 1), 0);
	ck_assert_int_eq(gss_ctx_seq_check(e, 2), 0);
	ck_assert_int_ne(gss_ctx_seq_check(e, 1), 0); /* replay */
	ck_assert_int_ne(gss_ctx_seq_check(e, 2), 0); /* replay */

	free_test_ctx(e);
}
END_TEST

/* Sequence number below the window must be rejected. */
START_TEST(test_below_window_rejected)
{
	struct gss_ctx_entry *e = make_test_ctx();

	/* Advance to seq 200 — window covers [73..200]. */
	for (uint32_t i = 1; i <= 200; i++)
		ck_assert_int_eq(gss_ctx_seq_check(e, i), 0);

	/* Seq 72 is below window. */
	ck_assert_int_ne(gss_ctx_seq_check(e, 72), 0);
	/* Seq 1 is way below window. */
	ck_assert_int_ne(gss_ctx_seq_check(e, 1), 0);

	free_test_ctx(e);
}
END_TEST

/* Out-of-order within the window should be accepted. */
START_TEST(test_out_of_order_accepted)
{
	struct gss_ctx_entry *e = make_test_ctx();

	ck_assert_int_eq(gss_ctx_seq_check(e, 1), 0);
	ck_assert_int_eq(gss_ctx_seq_check(e, 5), 0);
	ck_assert_int_eq(gss_ctx_seq_check(e, 3), 0); /* within window */
	ck_assert_int_eq(gss_ctx_seq_check(e, 2), 0); /* within window */
	ck_assert_int_eq(gss_ctx_seq_check(e, 4), 0); /* within window */

	/* Now they're all seen — replays must fail. */
	ck_assert_int_ne(gss_ctx_seq_check(e, 3), 0);
	ck_assert_int_ne(gss_ctx_seq_check(e, 5), 0);

	free_test_ctx(e);
}
END_TEST

/* Large jump should clear the bitmap and accept the new seq. */
START_TEST(test_large_jump)
{
	struct gss_ctx_entry *e = make_test_ctx();

	ck_assert_int_eq(gss_ctx_seq_check(e, 1), 0);
	ck_assert_int_eq(gss_ctx_seq_check(e, 2), 0);

	/* Jump by 500 — well beyond the 128-bit window. */
	ck_assert_int_eq(gss_ctx_seq_check(e, 502), 0);

	/* Old seqs are below window. */
	ck_assert_int_ne(gss_ctx_seq_check(e, 1), 0);
	ck_assert_int_ne(gss_ctx_seq_check(e, 2), 0);

	/* Seqs just below 502 but within window should be accepted. */
	ck_assert_int_eq(gss_ctx_seq_check(e, 500), 0);
	ck_assert_int_eq(gss_ctx_seq_check(e, 375), 0); /* 502-128+1=375 */

	/* 374 is below window. */
	ck_assert_int_ne(gss_ctx_seq_check(e, 374), 0);

	free_test_ctx(e);
}
END_TEST

/* Window boundary: exactly at window edge. */
START_TEST(test_window_boundary)
{
	struct gss_ctx_entry *e = make_test_ctx();

	/* Advance to 128. Window covers [1..128]. */
	for (uint32_t i = 1; i <= 128; i++)
		ck_assert_int_eq(gss_ctx_seq_check(e, i), 0);

	/* Advance to 129. Window covers [2..129]. Seq 1 is now out. */
	ck_assert_int_eq(gss_ctx_seq_check(e, 129), 0);
	ck_assert_int_ne(gss_ctx_seq_check(e, 1), 0);

	/* Seq 2 is still at the edge of the window. */
	ck_assert_int_ne(gss_ctx_seq_check(e, 2), 0); /* already seen */

	free_test_ctx(e);
}
END_TEST

static Suite *gss_seq_suite(void)
{
	Suite *s = suite_create("gss_seq");
	TCase *tc = tcase_create("replay_window");

	tcase_add_test(tc, test_seq_zero_rejected);
	tcase_add_test(tc, test_sequential);
	tcase_add_test(tc, test_duplicate_rejected);
	tcase_add_test(tc, test_below_window_rejected);
	tcase_add_test(tc, test_out_of_order_accepted);
	tcase_add_test(tc, test_large_jump);
	tcase_add_test(tc, test_window_boundary);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int nfailed;
	Suite *s = gss_seq_suite();
	SRunner *sr = srunner_create(s);

	srunner_run_all(sr, CK_NORMAL);
	nfailed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return nfailed ? 1 : 0;
}
