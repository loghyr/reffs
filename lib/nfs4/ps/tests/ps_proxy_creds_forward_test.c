/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * Wire-level coverage for the credential-forwarding plumbing
 * (slice 2e-iv-c-iii).  c-ii threaded a const struct
 * authunix_parms *creds parameter through every PS forwarder, and
 * c-iii made the hooks pass &compound->c_ap so the end client's
 * AUTH_SYS uid reaches the upstream MDS.
 *
 * The arg-validation tests in ps_proxy_ops_test.c only exercise
 * the early-return guards.  This file fills the gap by overriding
 * the production mds_compound_send_with_auth() (declared weak in
 * lib/nfs4/client/mds_compound.c) with a stub that captures the
 * creds pointer and returns -EIO.  The forwarder bails before
 * touching the (synthetic) reply, so we don't need to construct
 * a full XDR tree -- the assertion is purely "did the forwarder
 * propagate its creds argument verbatim?"
 *
 * Coverage: one test per primitive (7) plus one NULL-creds test
 * to confirm the fallback path is undisturbed.
 */

#include <check.h>
#include <errno.h>
#include <rpc/rpc.h>
#include <rpc/auth_unix.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ec_client.h"
#include "ps_proxy_ops.h"
#include "ps_state.h" /* PS_MAX_FH_SIZE */

/* ------------------------------------------------------------------ */
/* Strong override of mds_compound_send_with_auth.  The production    */
/* version in lib/nfs4/client/mds_compound.c is __attribute__((weak)) */
/* so this file's strong symbol wins at link time.                     */
/* ------------------------------------------------------------------ */

static const struct authunix_parms *g_captured_creds;
static int g_send_call_count;

int mds_compound_send_with_auth(struct mds_compound *mc __attribute__((unused)),
				struct mds_session *ms __attribute__((unused)),
				const struct authunix_parms *creds)
{
	g_captured_creds = creds;
	g_send_call_count++;
	/*
	 * Return -EIO so the forwarder treats this as a hard wire
	 * failure and bails before per-op-result inspection.  The
	 * forwarder's "if (ret && ret != -EREMOTEIO) goto out;"
	 * gate routes this to immediate cleanup, no XDR-tree access.
	 */
	return -EIO;
}

static void capture_reset(void)
{
	g_captured_creds = NULL;
	g_send_call_count = 0;
}

/*
 * Sentinel uid value chosen to be visibly non-default (not 0, not
 * 65534) so a regression that passed PS service identity instead of
 * the end-client identity would be obvious.
 */
#define TEST_UID 1066u
#define TEST_GID 2077u

static struct authunix_parms g_test_creds = {
	.aup_machname = (char *)"ps-test",
	.aup_uid = TEST_UID,
	.aup_gid = TEST_GID,
	.aup_len = 0,
	.aup_gids = NULL,
};

/*
 * Minimal mds_session fixture for the add_sequence hop the
 * forwarders make BEFORE reaching our overridden send.  Only the
 * sessionid bytes and slot_seqid are read; cl_auth swap is skipped
 * by the override so ms_clnt / ms_call_mutex stay untouched.
 */
static struct mds_session g_test_session;

static struct mds_session *test_session(void)
{
	memset(&g_test_session, 0, sizeof(g_test_session));
	memset(g_test_session.ms_sessionid, 0xAB, sizeof(sessionid4));
	g_test_session.ms_slot_seqid = 1;
	return &g_test_session;
}

/* ------------------------------------------------------------------ */
/* Per-primitive forwarding tests                                      */
/* ------------------------------------------------------------------ */

START_TEST(test_forward_getattr_propagates_creds)
{
	uint8_t fh[] = { 0x01, 0x02 };
	uint32_t mask[] = { 0x1u };
	struct ps_proxy_getattr_reply reply;

	memset(&reply, 0, sizeof(reply));
	capture_reset();

	int ret = ps_proxy_forward_getattr(test_session(), fh, sizeof(fh), mask,
					   1, &g_test_creds, &reply);

	ck_assert_int_eq(ret, -EIO);
	ck_assert_int_eq(g_send_call_count, 1);
	ck_assert_ptr_eq(g_captured_creds, &g_test_creds);
	ck_assert_uint_eq(g_captured_creds->aup_uid, TEST_UID);
	ck_assert_uint_eq(g_captured_creds->aup_gid, TEST_GID);
}
END_TEST

START_TEST(test_forward_getattr_null_creds)
{
	uint8_t fh[] = { 0x01 };
	uint32_t mask[] = { 0x1u };
	struct ps_proxy_getattr_reply reply;

	memset(&reply, 0, sizeof(reply));
	capture_reset();

	int ret = ps_proxy_forward_getattr(test_session(), fh, sizeof(fh), mask,
					   1, NULL, &reply);

	ck_assert_int_eq(ret, -EIO);
	ck_assert_int_eq(g_send_call_count, 1);
	ck_assert_ptr_null(g_captured_creds);
}
END_TEST

START_TEST(test_forward_lookup_propagates_creds)
{
	uint8_t parent[] = { 0x11 };
	uint8_t child[PS_MAX_FH_SIZE];
	uint32_t child_len = 0;

	capture_reset();

	int ret = ps_proxy_forward_lookup(test_session(), parent,
					  sizeof(parent), "x", 1, child,
					  sizeof(child), &child_len, NULL, 0,
					  &g_test_creds, NULL);

	ck_assert_int_eq(ret, -EIO);
	ck_assert_int_eq(g_send_call_count, 1);
	ck_assert_ptr_eq(g_captured_creds, &g_test_creds);
	ck_assert_uint_eq(g_captured_creds->aup_uid, TEST_UID);
}
END_TEST

START_TEST(test_forward_read_propagates_creds)
{
	uint8_t fh[] = { 0x22 };
	uint8_t other[PS_STATEID_OTHER_SIZE] = { 0 };
	struct ps_proxy_read_reply reply;

	memset(&reply, 0, sizeof(reply));
	capture_reset();

	int ret = ps_proxy_forward_read(test_session(), fh, sizeof(fh), 0,
					other, 0, 4096, &g_test_creds, &reply);

	ck_assert_int_eq(ret, -EIO);
	ck_assert_int_eq(g_send_call_count, 1);
	ck_assert_ptr_eq(g_captured_creds, &g_test_creds);
	ck_assert_uint_eq(g_captured_creds->aup_uid, TEST_UID);
}
END_TEST

START_TEST(test_forward_write_propagates_creds)
{
	uint8_t fh[] = { 0x33 };
	uint8_t other[PS_STATEID_OTHER_SIZE] = { 0 };
	uint8_t data[] = { 0xAB };
	struct ps_proxy_write_reply reply;

	memset(&reply, 0, sizeof(reply));
	capture_reset();

	int ret = ps_proxy_forward_write(test_session(), fh, sizeof(fh), 0,
					 other, 0, 0, data, sizeof(data),
					 &g_test_creds, &reply);

	ck_assert_int_eq(ret, -EIO);
	ck_assert_int_eq(g_send_call_count, 1);
	ck_assert_ptr_eq(g_captured_creds, &g_test_creds);
	ck_assert_uint_eq(g_captured_creds->aup_uid, TEST_UID);
}
END_TEST

START_TEST(test_forward_close_propagates_creds)
{
	uint8_t fh[] = { 0x44 };
	uint8_t other[PS_STATEID_OTHER_SIZE] = { 0 };
	struct ps_proxy_close_reply reply;

	memset(&reply, 0, sizeof(reply));
	capture_reset();

	int ret = ps_proxy_forward_close(test_session(), fh, sizeof(fh), 0, 0,
					 other, &g_test_creds, &reply);

	ck_assert_int_eq(ret, -EIO);
	ck_assert_int_eq(g_send_call_count, 1);
	ck_assert_ptr_eq(g_captured_creds, &g_test_creds);
	ck_assert_uint_eq(g_captured_creds->aup_uid, TEST_UID);
}
END_TEST

START_TEST(test_forward_open_propagates_creds)
{
	uint8_t parent[] = { 0x55 };
	struct ps_proxy_open_request req = {
		.claim_type = PS_PROXY_OPEN_CLAIM_NULL,
		.opentype = PS_PROXY_OPEN_OPENTYPE_NOCREATE,
		.share_access = 1,
	};
	struct ps_proxy_open_reply reply;

	memset(&reply, 0, sizeof(reply));
	capture_reset();

	int ret = ps_proxy_forward_open(test_session(), parent, sizeof(parent),
					"f", 1, &req, &g_test_creds, &reply);

	ck_assert_int_eq(ret, -EIO);
	ck_assert_int_eq(g_send_call_count, 1);
	ck_assert_ptr_eq(g_captured_creds, &g_test_creds);
	ck_assert_uint_eq(g_captured_creds->aup_uid, TEST_UID);
}
END_TEST

START_TEST(test_forward_symlink_propagates_creds)
{
	uint8_t parent[] = { 0xCC };
	struct ps_proxy_symlink_reply reply;

	memset(&reply, 0, sizeof(reply));
	capture_reset();

	int ret = ps_proxy_forward_symlink(test_session(), parent,
					   sizeof(parent), "lnk", 3, "/target",
					   7, NULL, 0, NULL, 0, &g_test_creds,
					   &reply);

	ck_assert_int_eq(ret, -EIO);
	ck_assert_int_eq(g_send_call_count, 1);
	ck_assert_ptr_eq(g_captured_creds, &g_test_creds);
	ck_assert_uint_eq(g_captured_creds->aup_uid, TEST_UID);
	ps_proxy_symlink_reply_free(&reply);
}
END_TEST

START_TEST(test_forward_link_propagates_creds)
{
	uint8_t src[] = { 0xDD };
	uint8_t dst[] = { 0xEE };
	struct ps_proxy_link_reply reply;

	memset(&reply, 0, sizeof(reply));
	capture_reset();

	int ret = ps_proxy_forward_link(test_session(), src, sizeof(src), dst,
					sizeof(dst), "newlink", 7,
					&g_test_creds, &reply);

	ck_assert_int_eq(ret, -EIO);
	ck_assert_int_eq(g_send_call_count, 1);
	ck_assert_ptr_eq(g_captured_creds, &g_test_creds);
	ck_assert_uint_eq(g_captured_creds->aup_uid, TEST_UID);
}
END_TEST

START_TEST(test_forward_mkdir_propagates_creds)
{
	uint8_t parent[] = { 0xBB };
	struct ps_proxy_mkdir_reply reply;

	memset(&reply, 0, sizeof(reply));
	capture_reset();

	int ret = ps_proxy_forward_mkdir(test_session(), parent, sizeof(parent),
					 "newdir", 6, NULL, 0, NULL, 0,
					 &g_test_creds, &reply);

	ck_assert_int_eq(ret, -EIO);
	ck_assert_int_eq(g_send_call_count, 1);
	ck_assert_ptr_eq(g_captured_creds, &g_test_creds);
	ck_assert_uint_eq(g_captured_creds->aup_uid, TEST_UID);
	ps_proxy_mkdir_reply_free(&reply);
}
END_TEST

START_TEST(test_forward_rename_propagates_creds)
{
	uint8_t src[] = { 0x99 };
	uint8_t dst[] = { 0xAA };
	struct ps_proxy_rename_reply reply;

	memset(&reply, 0, sizeof(reply));
	capture_reset();

	int ret = ps_proxy_forward_rename(test_session(), src, sizeof(src),
					  "old", 3, dst, sizeof(dst), "new", 3,
					  &g_test_creds, &reply);

	ck_assert_int_eq(ret, -EIO);
	ck_assert_int_eq(g_send_call_count, 1);
	ck_assert_ptr_eq(g_captured_creds, &g_test_creds);
	ck_assert_uint_eq(g_captured_creds->aup_uid, TEST_UID);
}
END_TEST

START_TEST(test_forward_remove_propagates_creds)
{
	uint8_t parent[] = { 0x88 };
	struct ps_proxy_remove_reply reply;

	memset(&reply, 0, sizeof(reply));
	capture_reset();

	int ret = ps_proxy_forward_remove(test_session(), parent,
					  sizeof(parent), "victim", 6,
					  &g_test_creds, &reply);

	ck_assert_int_eq(ret, -EIO);
	ck_assert_int_eq(g_send_call_count, 1);
	ck_assert_ptr_eq(g_captured_creds, &g_test_creds);
	ck_assert_uint_eq(g_captured_creds->aup_uid, TEST_UID);
}
END_TEST

START_TEST(test_forward_commit_propagates_creds)
{
	uint8_t fh[] = { 0x77 };
	struct ps_proxy_commit_reply reply;

	memset(&reply, 0, sizeof(reply));
	capture_reset();

	int ret = ps_proxy_forward_commit(test_session(), fh, sizeof(fh), 0,
					  4096, &g_test_creds, &reply);

	ck_assert_int_eq(ret, -EIO);
	ck_assert_int_eq(g_send_call_count, 1);
	ck_assert_ptr_eq(g_captured_creds, &g_test_creds);
	ck_assert_uint_eq(g_captured_creds->aup_uid, TEST_UID);
}
END_TEST

START_TEST(test_forward_readdir_propagates_creds)
{
	uint8_t fh[] = { 0x66 };
	uint8_t cookieverf[PS_PROXY_VERIFIER_SIZE] = { 0 };
	struct ps_proxy_readdir_reply reply;

	memset(&reply, 0, sizeof(reply));
	capture_reset();

	int ret = ps_proxy_forward_readdir(test_session(), fh, sizeof(fh), 0,
					   cookieverf, 4096, 8192, NULL, 0,
					   &g_test_creds, &reply);

	ck_assert_int_eq(ret, -EIO);
	ck_assert_int_eq(g_send_call_count, 1);
	ck_assert_ptr_eq(g_captured_creds, &g_test_creds);
	ck_assert_uint_eq(g_captured_creds->aup_uid, TEST_UID);
}
END_TEST

/*
 * mknod forwarder covers the four "special file" object types in
 * one entry point.  Test each type individually to make sure the
 * specdata-vs-no-devdata branch in the implementation didn't lose
 * the cred argument along either path.
 */
START_TEST(test_forward_mknod_blk_propagates_creds)
{
	uint8_t parent[] = { 0xC1 };
	struct ps_proxy_mknod_reply reply;

	memset(&reply, 0, sizeof(reply));
	capture_reset();

	int ret = ps_proxy_forward_mknod(test_session(), parent, sizeof(parent),
					 "blkdev", 6, NF4BLK, 8u, 16u, NULL, 0,
					 NULL, 0, &g_test_creds, &reply);

	ck_assert_int_eq(ret, -EIO);
	ck_assert_int_eq(g_send_call_count, 1);
	ck_assert_ptr_eq(g_captured_creds, &g_test_creds);
	ck_assert_uint_eq(g_captured_creds->aup_uid, TEST_UID);
	ps_proxy_mknod_reply_free(&reply);
}
END_TEST

START_TEST(test_forward_mknod_chr_propagates_creds)
{
	uint8_t parent[] = { 0xC2 };
	struct ps_proxy_mknod_reply reply;

	memset(&reply, 0, sizeof(reply));
	capture_reset();

	int ret = ps_proxy_forward_mknod(test_session(), parent, sizeof(parent),
					 "chrdev", 6, NF4CHR, 4u, 65u, NULL, 0,
					 NULL, 0, &g_test_creds, &reply);

	ck_assert_int_eq(ret, -EIO);
	ck_assert_int_eq(g_send_call_count, 1);
	ck_assert_ptr_eq(g_captured_creds, &g_test_creds);
	ck_assert_uint_eq(g_captured_creds->aup_uid, TEST_UID);
	ps_proxy_mknod_reply_free(&reply);
}
END_TEST

START_TEST(test_forward_mknod_sock_propagates_creds)
{
	uint8_t parent[] = { 0xC3 };
	struct ps_proxy_mknod_reply reply;

	memset(&reply, 0, sizeof(reply));
	capture_reset();

	int ret = ps_proxy_forward_mknod(test_session(), parent, sizeof(parent),
					 "sock", 4, NF4SOCK, 0, 0, NULL, 0,
					 NULL, 0, &g_test_creds, &reply);

	ck_assert_int_eq(ret, -EIO);
	ck_assert_int_eq(g_send_call_count, 1);
	ck_assert_ptr_eq(g_captured_creds, &g_test_creds);
	ck_assert_uint_eq(g_captured_creds->aup_uid, TEST_UID);
	ps_proxy_mknod_reply_free(&reply);
}
END_TEST

START_TEST(test_forward_mknod_fifo_propagates_creds)
{
	uint8_t parent[] = { 0xC4 };
	struct ps_proxy_mknod_reply reply;

	memset(&reply, 0, sizeof(reply));
	capture_reset();

	int ret = ps_proxy_forward_mknod(test_session(), parent, sizeof(parent),
					 "fifo", 4, NF4FIFO, 0, 0, NULL, 0,
					 NULL, 0, &g_test_creds, &reply);

	ck_assert_int_eq(ret, -EIO);
	ck_assert_int_eq(g_send_call_count, 1);
	ck_assert_ptr_eq(g_captured_creds, &g_test_creds);
	ck_assert_uint_eq(g_captured_creds->aup_uid, TEST_UID);
	ps_proxy_mknod_reply_free(&reply);
}
END_TEST

/*
 * Wrong-type guard: the dispatcher in dir.c must never let a
 * NF4DIR / NF4LNK / NF4REG slip into ps_proxy_forward_mknod (those
 * have their own forwarders).  The forwarder rejects with -EINVAL
 * before any RPC fires, so g_send_call_count must stay 0.
 */
START_TEST(test_forward_mknod_rejects_wrong_type)
{
	uint8_t parent[] = { 0xC5 };
	struct ps_proxy_mknod_reply reply;

	memset(&reply, 0, sizeof(reply));
	capture_reset();

	int ret = ps_proxy_forward_mknod(test_session(), parent, sizeof(parent),
					 "wrong", 5, NF4DIR, 0, 0, NULL, 0,
					 NULL, 0, &g_test_creds, &reply);

	ck_assert_int_eq(ret, -EINVAL);
	ck_assert_int_eq(g_send_call_count, 0);
}
END_TEST

/*
 * ps_proxy_pipeline_read carries the end-client AUTH_SYS creds
 * through ec_read_codec_with_file -> mds_layout_get -> the wire
 * compound.  The mock above captures the creds pointer reaching
 * mds_compound_send_with_auth; the assertion is the same shape
 * as the per-forwarder tests above (-EIO short-circuits the
 * pipeline before any per-op result is touched, so no XDR tree
 * needs to be constructed).
 *
 * Closes slice (b2): forwarded creds on proxy READ path.
 */
START_TEST(test_pipeline_read_propagates_creds)
{
	uint8_t fh[] = { 0xCC, 0xDD };
	uint8_t other[PS_STATEID_OTHER_SIZE] = { 0xEE };
	struct ps_proxy_read_reply reply;

	memset(&reply, 0, sizeof(reply));
	capture_reset();

	int ret = ps_proxy_pipeline_read(test_session(), fh, sizeof(fh),
					 /* stateid_seqid */ 1, other,
					 /* offset */ 0, /* count */ 4096,
					 &g_test_creds, &reply);

	/*
	 * mds_compound_send_with_auth is mocked to return -EIO; the
	 * pipeline's mds_layout_get path bails on that, the codec is
	 * destroyed, and ps_proxy_pipeline_read returns the underlying
	 * error verbatim.  -EIO is the wire-level errno the mock
	 * returns; the pipeline's translation of "send failed" to
	 * "no layout available" surfaces as -EIO out the top.
	 */
	ck_assert_int_eq(ret, -EIO);
	/*
	 * Exactly one send fires: mds_layout_get's LAYOUTGET compound,
	 * which the mock fails with -EIO.  ec_read_codec_with_file's
	 * error path takes `goto out_codec` directly (skipping
	 * out_layout) because there is no layout grant to return when
	 * LAYOUTGET itself failed -- so mds_layout_return does not
	 * run in this synthetic failure scenario.
	 *
	 * The cleanup-path correctness ("when LAYOUTRETURN does run,
	 * it carries the same end-client creds") is enforced by
	 * inspection of the call site at lib/nfs4/ps/ec_pipeline.c
	 * (the out_layout label passes the same `creds` parameter).
	 * Driving that path through the test harness would require a
	 * mock that returns a synthetic LAYOUTGET success with a
	 * decodable layout body, which is well beyond a creds-
	 * propagation unit test.
	 */
	ck_assert_uint_eq(g_send_call_count, 1);
	ck_assert_ptr_eq(g_captured_creds, &g_test_creds);
	ck_assert_uint_eq(g_captured_creds->aup_uid, TEST_UID);
	ck_assert_uint_eq(g_captured_creds->aup_gid, TEST_GID);

	/* No data should be returned given the pipeline failed. */
	ck_assert_ptr_null(reply.data);
}
END_TEST

/*
 * NULL-creds companion: ps_proxy_pipeline_read with creds=NULL
 * must propagate the NULL through to mds_compound_send_with_auth
 * so the session's default auth is used (the back-compat path
 * ec_demo / dstore-MDS-to-DS rely on).  Mirrors the
 * test_forward_*_null_creds discipline applied to the per-forwarder
 * tests above so a future regression that tries to inject a fallback
 * AUTH_SYS principal here would fail loudly.
 */
START_TEST(test_pipeline_read_null_creds)
{
	uint8_t fh[] = { 0x77 };
	uint8_t other[PS_STATEID_OTHER_SIZE] = { 0x88 };
	struct ps_proxy_read_reply reply;

	memset(&reply, 0, sizeof(reply));
	capture_reset();

	int ret = ps_proxy_pipeline_read(test_session(), fh, sizeof(fh),
					 /* stateid_seqid */ 1, other,
					 /* offset */ 0, /* count */ 4096, NULL,
					 &reply);

	ck_assert_int_eq(ret, -EIO);
	/* Same single-send rationale as the propagates_creds test above. */
	ck_assert_uint_eq(g_send_call_count, 1);
	ck_assert_ptr_null(g_captured_creds);
	ck_assert_ptr_null(reply.data);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite                                                               */
/* ------------------------------------------------------------------ */

static Suite *ps_proxy_creds_forward_suite(void)
{
	Suite *s = suite_create("ps_proxy_creds_forward");
	TCase *tc = tcase_create("propagation");

	tcase_add_test(tc, test_forward_getattr_propagates_creds);
	tcase_add_test(tc, test_forward_getattr_null_creds);
	tcase_add_test(tc, test_forward_lookup_propagates_creds);
	tcase_add_test(tc, test_forward_read_propagates_creds);
	tcase_add_test(tc, test_forward_write_propagates_creds);
	tcase_add_test(tc, test_forward_close_propagates_creds);
	tcase_add_test(tc, test_forward_open_propagates_creds);
	tcase_add_test(tc, test_forward_mkdir_propagates_creds);
	tcase_add_test(tc, test_forward_symlink_propagates_creds);
	tcase_add_test(tc, test_forward_link_propagates_creds);
	tcase_add_test(tc, test_forward_rename_propagates_creds);
	tcase_add_test(tc, test_forward_remove_propagates_creds);
	tcase_add_test(tc, test_forward_commit_propagates_creds);
	tcase_add_test(tc, test_forward_readdir_propagates_creds);
	tcase_add_test(tc, test_forward_mknod_blk_propagates_creds);
	tcase_add_test(tc, test_forward_mknod_chr_propagates_creds);
	tcase_add_test(tc, test_forward_mknod_sock_propagates_creds);
	tcase_add_test(tc, test_forward_mknod_fifo_propagates_creds);
	tcase_add_test(tc, test_forward_mknod_rejects_wrong_type);
	tcase_add_test(tc, test_pipeline_read_propagates_creds);
	tcase_add_test(tc, test_pipeline_read_null_creds);

	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int failed;
	SRunner *sr = srunner_create(ps_proxy_creds_forward_suite());

	srunner_run_all(sr, CK_NORMAL);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return failed == 0 ? 0 : 1;
}
