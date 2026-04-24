/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <check.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ps_proxy_ops.h"
#include "ps_state.h" /* PS_MAX_FH_SIZE */

/*
 * The forwarder requires a live mds_session for a happy-path run
 * (CI integration covers that).  These unit tests cover only the
 * argument-validation shortcuts that fire before any compound is
 * built -- so a NULL / (void *)1 sentinel for the session pointer
 * is harmless because the function never dereferences it on a bad
 * argument path.
 */

START_TEST(test_forward_getattr_null_args)
{
	uint8_t fh[] = { 0x01, 0x02, 0x03 };
	uint32_t mask[] = { 0x00000001 };
	struct ps_proxy_getattr_reply reply;

	memset(&reply, 0, sizeof(reply));

	ck_assert_int_eq(ps_proxy_forward_getattr(NULL, fh, sizeof(fh), mask, 1,
						  &reply),
			 -EINVAL);
	ck_assert_int_eq(ps_proxy_forward_getattr((void *)1, NULL, sizeof(fh),
						  mask, 1, &reply),
			 -EINVAL);
	ck_assert_int_eq(ps_proxy_forward_getattr((void *)1, fh, sizeof(fh),
						  NULL, 1, &reply),
			 -EINVAL);
	ck_assert_int_eq(ps_proxy_forward_getattr((void *)1, fh, sizeof(fh),
						  mask, 1, NULL),
			 -EINVAL);
}
END_TEST

/*
 * Zero-length FH and zero-length mask are both programmer errors
 * -- the MDS would reject the compound but the forward call should
 * refuse to send it.
 */
START_TEST(test_forward_getattr_zero_lengths)
{
	uint8_t fh[] = { 0x01 };
	uint32_t mask[] = { 0x00000001 };
	struct ps_proxy_getattr_reply reply;

	memset(&reply, 0, sizeof(reply));

	ck_assert_int_eq(ps_proxy_forward_getattr((void *)1, fh, 0, mask, 1,
						  &reply),
			 -EINVAL);
	ck_assert_int_eq(ps_proxy_forward_getattr((void *)1, fh, sizeof(fh),
						  mask, 0, &reply),
			 -EINVAL);
}
END_TEST

/*
 * FH lengths above NFS4_FHSIZE (128 bytes, per RFC 8881) are
 * protocol-invalid and short-circuit before the compound is
 * built.  Matches the other PS primitives (ps_sb_binding_alloc,
 * ps_state_set_mds_root_fh) that use the same cap.
 */
START_TEST(test_forward_getattr_fh_too_big)
{
	uint8_t big_fh[PS_MAX_FH_SIZE + 1];
	uint32_t mask[] = { 0x00000001 };
	struct ps_proxy_getattr_reply reply;

	memset(big_fh, 0xAB, sizeof(big_fh));
	memset(&reply, 0, sizeof(reply));

	ck_assert_int_eq(ps_proxy_forward_getattr((void *)1, big_fh,
						  sizeof(big_fh), mask, 1,
						  &reply),
			 -E2BIG);
}
END_TEST

/*
 * Freeing a never-populated reply (all-zero struct from memset)
 * and a NULL reply are both safe.  Matches the project convention
 * for NULL-tolerant release helpers.
 */
START_TEST(test_reply_free_null_safe)
{
	struct ps_proxy_getattr_reply reply = { 0 };

	ps_proxy_getattr_reply_free(NULL);
	ps_proxy_getattr_reply_free(&reply);

	/*
	 * Verifiable post-condition: the struct stays zero and a
	 * second free on the same struct is still safe (idempotent).
	 */
	ck_assert_ptr_null(reply.attrmask);
	ck_assert_uint_eq(reply.attrmask_len, 0);
	ck_assert_ptr_null(reply.attr_vals);
	ck_assert_uint_eq(reply.attr_vals_len, 0);
	ps_proxy_getattr_reply_free(&reply);
}
END_TEST

/*
 * Freeing a populated reply releases both buffers and zeroes the
 * struct -- the common path after a successful forward.  Heap
 * buffers simulate what ps_proxy_forward_getattr would have
 * allocated; LSan backstop catches a missing free if the helper
 * ever stops releasing one of the two fields.
 */
START_TEST(test_reply_free_populated)
{
	struct ps_proxy_getattr_reply reply;

	reply.attrmask = calloc(2, sizeof(*reply.attrmask));
	ck_assert_ptr_nonnull(reply.attrmask);
	reply.attrmask[0] = 0xDEADBEEF;
	reply.attrmask[1] = 0xCAFEBABE;
	reply.attrmask_len = 2;

	reply.attr_vals = calloc(8, 1);
	ck_assert_ptr_nonnull(reply.attr_vals);
	memset(reply.attr_vals, 0x55, 8);
	reply.attr_vals_len = 8;

	ps_proxy_getattr_reply_free(&reply);

	ck_assert_ptr_null(reply.attrmask);
	ck_assert_uint_eq(reply.attrmask_len, 0);
	ck_assert_ptr_null(reply.attr_vals);
	ck_assert_uint_eq(reply.attr_vals_len, 0);
}
END_TEST

static Suite *ps_proxy_ops_suite(void)
{
	Suite *s = suite_create("ps_proxy_ops");
	TCase *tc = tcase_create("core");

	tcase_add_test(tc, test_forward_getattr_null_args);
	tcase_add_test(tc, test_forward_getattr_zero_lengths);
	tcase_add_test(tc, test_forward_getattr_fh_too_big);
	tcase_add_test(tc, test_reply_free_null_safe);
	tcase_add_test(tc, test_reply_free_populated);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int failed;
	SRunner *sr = srunner_create(ps_proxy_ops_suite());

	srunner_run_all(sr, CK_NORMAL);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return failed == 0 ? 0 : 1;
}
