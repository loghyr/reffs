/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * Unit tests for RPC reply encoding helpers.
 *
 * Verifies that rpc_build_accepted_header() and rpc_build_denied_header()
 * produce wire-correct replies per RFC 5531.  Each test compares the
 * output buffer against hand-verified network-byte-order expected values.
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

#include <check.h>

#include "reffs/rpc.h"

/*
 * Helper: create a minimal rpc_trans for testing.
 * calloc ensures all fields (including ri_verifier_body) start as 0/NULL.
 */
static struct rpc_trans *make_test_rt(uint32_t xid)
{
	struct rpc_trans *rt = calloc(1, sizeof(*rt));

	ck_assert_ptr_nonnull(rt);
	rt->rt_info.ri_xid = xid;
	rt->rt_info.ri_cred.rc_flavor = AUTH_SYS;
	return rt;
}

static void free_test_rt(struct rpc_trans *rt)
{
	free(rt->rt_reply);
	free(rt->rt_info.ri_verifier_body);
	free(rt);
}

/* Read a network-order uint32 from a byte offset in the reply buffer. */
static uint32_t reply_word(struct rpc_trans *rt, unsigned int index)
{
	uint32_t *p = (uint32_t *)rt->rt_reply;

	return ntohl(p[index]);
}

/* ------------------------------------------------------------------ */
/* MSG_ACCEPTED tests                                                  */
/* ------------------------------------------------------------------ */

/*
 * RFC 5531 MSG_ACCEPTED SUCCESS reply (AUTH_NONE verifier, no body):
 *   [0] record_mark (msg_len | 0x80000000)
 *   [1] xid
 *   [2] MSG_REPLY (1)
 *   [3] MSG_ACCEPTED (0)
 *   [4] verf_flavor (AUTH_NONE = 0)
 *   [5] verf_len (0)
 *   [6] accept_stat (SUCCESS = 0)
 * Total: 7 words = 28 bytes.
 */
START_TEST(test_reply_accepted_success)
{
	struct rpc_trans *rt = make_test_rt(0x12345678);
	int ret;

	ret = rpc_alloc_accepted_reply(rt, 0);
	ck_assert_int_eq(ret, 0);
	ck_assert_uint_eq(rt->rt_reply_len, 7 * sizeof(uint32_t));

	uint32_t *p = rpc_build_accepted_header(rt, 0);

	ck_assert_ptr_nonnull(p);

	/* Verify each word. */
	uint32_t msg_len = 7 * sizeof(uint32_t) - sizeof(uint32_t);

	ck_assert_uint_eq(reply_word(rt, 0), msg_len | 0x80000000);
	ck_assert_uint_eq(reply_word(rt, 1), 0x12345678); /* xid */
	ck_assert_uint_eq(reply_word(rt, 2), 1); /* MSG_REPLY */
	ck_assert_uint_eq(reply_word(rt, 3), 0); /* MSG_ACCEPTED */
	ck_assert_uint_eq(reply_word(rt, 4), 0); /* AUTH_NONE */
	ck_assert_uint_eq(reply_word(rt, 5), 0); /* verf_len */
	ck_assert_uint_eq(reply_word(rt, 6), 0); /* SUCCESS */

	/* p should point exactly to end of buffer (no body). */
	ck_assert_ptr_eq(p, (uint32_t *)rt->rt_reply + 7);

	free_test_rt(rt);
}
END_TEST

/*
 * MSG_ACCEPTED with error status (e.g., GARBAGE_ARGS).
 * Same 7-word structure but accept_stat != 0.
 */
START_TEST(test_reply_accepted_error)
{
	struct rpc_trans *rt = make_test_rt(0xAABBCCDD);
	int ret;

	ret = rpc_alloc_accepted_reply(rt, 0);
	ck_assert_int_eq(ret, 0);

	uint32_t *p = rpc_build_accepted_header(rt, GARBAGE_ARGS);

	ck_assert_ptr_nonnull(p);

	ck_assert_uint_eq(reply_word(rt, 3), 0); /* MSG_ACCEPTED, NOT 1 */
	ck_assert_uint_eq(reply_word(rt, 6), GARBAGE_ARGS);

	free_test_rt(rt);
}
END_TEST

/*
 * MSG_ACCEPTED with a body.  Verify the pointer returned by
 * rpc_build_accepted_header points to the right offset for
 * body encoding, and that the buffer is the right size.
 */
START_TEST(test_reply_accepted_with_body)
{
	struct rpc_trans *rt = make_test_rt(0x11223344);
	size_t body_size = 16;
	int ret;

	ret = rpc_alloc_accepted_reply(rt, body_size);
	ck_assert_int_eq(ret, 0);
	ck_assert_uint_eq(rt->rt_reply_len, 7 * sizeof(uint32_t) + body_size);

	uint32_t *p = rpc_build_accepted_header(rt, 0);

	ck_assert_ptr_nonnull(p);

	/* p should be at byte offset 28 (7 words), with 16 bytes remaining. */
	size_t hdr_bytes = (char *)p - rt->rt_reply;

	ck_assert_uint_eq(hdr_bytes, 7 * sizeof(uint32_t));

	/* Write a test body and verify it fits. */
	memset(p, 0x42, body_size);
	ck_assert_uint_eq(hdr_bytes + body_size, rt->rt_reply_len);

	free_test_rt(rt);
}
END_TEST

/* ------------------------------------------------------------------ */
/* MSG_DENIED tests                                                    */
/* ------------------------------------------------------------------ */

/*
 * RFC 5531 MSG_DENIED / AUTH_ERROR reply:
 *   [0] record_mark
 *   [1] xid
 *   [2] MSG_REPLY (1)
 *   [3] MSG_DENIED (1)
 *   body: reject_stat + auth_stat
 * Total: 4 header words + body.  NO verifier.
 */
START_TEST(test_reply_denied_auth_error)
{
	struct rpc_trans *rt = make_test_rt(0xDEADBEEF);
	/* AUTH_ERROR needs 2 words: reject_stat + auth_stat */
	size_t body_size = 2 * sizeof(uint32_t);
	int ret;

	ret = rpc_alloc_denied_reply(rt, body_size);
	ck_assert_int_eq(ret, 0);
	ck_assert_uint_eq(rt->rt_reply_len, 4 * sizeof(uint32_t) + body_size);

	uint32_t *p = rpc_build_denied_header(rt);

	ck_assert_ptr_nonnull(p);

	ck_assert_uint_eq(reply_word(rt, 2), 1); /* MSG_REPLY */
	ck_assert_uint_eq(reply_word(rt, 3), 1); /* MSG_DENIED */

	/* No verifier between MSG_DENIED and reject_stat. */
	size_t hdr_bytes = (char *)p - rt->rt_reply;

	ck_assert_uint_eq(hdr_bytes, 4 * sizeof(uint32_t));

	/* Encode reject_stat + auth_stat manually. */
	p = rpc_encode_uint32_t(rt, p, AUTH_ERROR);
	ck_assert_ptr_nonnull(p);
	p = rpc_encode_uint32_t(rt, p, AUTH_BADCRED);
	ck_assert_ptr_nonnull(p);

	ck_assert_uint_eq(reply_word(rt, 4), AUTH_ERROR);
	ck_assert_uint_eq(reply_word(rt, 5), AUTH_BADCRED);

	free_test_rt(rt);
}
END_TEST

/*
 * MSG_DENIED / RPC_MISMATCH: reject_stat + low + high version.
 */
START_TEST(test_reply_denied_rpc_mismatch)
{
	struct rpc_trans *rt = make_test_rt(0xCAFEBABE);
	/* RPC_MISMATCH needs 3 words: reject_stat + low + high */
	size_t body_size = 3 * sizeof(uint32_t);
	int ret;

	ret = rpc_alloc_denied_reply(rt, body_size);
	ck_assert_int_eq(ret, 0);
	ck_assert_uint_eq(rt->rt_reply_len, 4 * sizeof(uint32_t) + body_size);

	uint32_t *p = rpc_build_denied_header(rt);

	ck_assert_ptr_nonnull(p);

	p = rpc_encode_uint32_t(rt, p, RPC_MISMATCH);
	p = rpc_encode_uint32_t(rt, p, 2); /* low version */
	p = rpc_encode_uint32_t(rt, p, 2); /* high version */
	ck_assert_ptr_nonnull(p);

	ck_assert_uint_eq(reply_word(rt, 4), RPC_MISMATCH);
	ck_assert_uint_eq(reply_word(rt, 5), 2);
	ck_assert_uint_eq(reply_word(rt, 6), 2);

	/* Exactly at end of buffer. */
	size_t written = (char *)p - rt->rt_reply;

	ck_assert_uint_eq(written, rt->rt_reply_len);

	free_test_rt(rt);
}
END_TEST

/*
 * Buffer exact size: verify rt_offset matches rt_reply_len after
 * building each header type.  This catches the duplicate-encode bug
 * class (more encodes than the buffer was sized for).
 */
START_TEST(test_reply_buffer_exact_size)
{
	/* Accepted, no body. */
	struct rpc_trans *rt = make_test_rt(1);

	rpc_alloc_accepted_reply(rt, 0);
	uint32_t *p = rpc_build_accepted_header(rt, 0);

	ck_assert_ptr_nonnull(p);
	ck_assert_uint_eq(rt->rt_offset, rt->rt_reply_len);
	free_test_rt(rt);

	/* Denied, auth_error body. */
	rt = make_test_rt(2);
	rpc_alloc_denied_reply(rt, 2 * sizeof(uint32_t));
	p = rpc_build_denied_header(rt);
	p = rpc_encode_uint32_t(rt, p, AUTH_ERROR);
	p = rpc_encode_uint32_t(rt, p, AUTH_BADCRED);
	ck_assert_ptr_nonnull(p);
	ck_assert_uint_eq(rt->rt_offset, rt->rt_reply_len);
	free_test_rt(rt);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite setup                                                         */
/* ------------------------------------------------------------------ */

Suite *rpc_reply_suite(void)
{
	Suite *s = suite_create("RPC Reply Encoding");

	TCase *tc = tcase_create("wire_format");

	tcase_add_test(tc, test_reply_accepted_success);
	tcase_add_test(tc, test_reply_accepted_error);
	tcase_add_test(tc, test_reply_accepted_with_body);
	tcase_add_test(tc, test_reply_denied_auth_error);
	tcase_add_test(tc, test_reply_denied_rpc_mismatch);
	tcase_add_test(tc, test_reply_buffer_exact_size);
	suite_add_tcase(s, tc);

	return s;
}

int main(void)
{
	Suite *s = rpc_reply_suite();
	SRunner *sr = srunner_create(s);

	srunner_run_all(sr, CK_NORMAL);
	int failed = srunner_ntests_failed(sr);

	srunner_free(sr);
	return failed ? 1 : 0;
}
