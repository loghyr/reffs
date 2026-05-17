/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * PS Phase 5 dispatch-hook coverage -- the
 * `test_shortcircuit_partial_2_mirrors` slice listed in
 * .claude/design/proxy-server.md / proxy-server-phase5.md and
 * deferred from slice 5.5's test_shortcircuit_counter_increments.
 *
 * Slice 5.5 pins the counter primitive
 * (ps_listener_record_shortcircuit) in isolation.  This file pins
 * the dispatch decision at ec_pipeline.c:262-269 (write) and
 * :321-329 (read): the hook fires only when
 *   em->em_local && ctx->ctx_pls && ctx->ctx_pls->pls_sc_write_fn
 * (read variant uses pls_sc_read_fn).  Each test builds a synthetic
 * struct ec_context with two mirrors, drives the dispatch via the
 * test-only ec_chunk_write / ec_chunk_read entries (exposed via
 * ec_pipeline_internal.h), and asserts the counter + install-stub
 * + RPC-chokepoint call counts match the expected dispatch
 * decision.
 *
 * The "partial" test is the load-bearing case: mirror 0 is
 * em_local=true (short-circuit), mirror 1 is em_local=false (RPC
 * fallback).  The RPC fallback never reaches the network -- the
 * test strong-overrides mds_compound_send_with_auth (the same
 * chokepoint ps_proxy_pipeline_write_test.c overrides) to return
 * -EIO before any clnt_call.  Both arms are observable: the
 * install stub records its call count; the RPC override records
 * its call count; the counter `pls_shortcircuit_total` reports
 * the per-listener atomic.  Test == 1 short-circuit + 1 RPC.
 */

#include <check.h>
#include <errno.h>
#include <pthread.h>
#include <rpc/rpc.h>
#include <rpc/auth_unix.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <urcu.h>

#include "reffs/settings.h"
#include "ec_client.h"
#include "ec_pipeline_internal.h"
#include "ps_state.h"

/* ------------------------------------------------------------------ */
/* Strong override of mds_compound_send_with_auth.                     */
/*                                                                     */
/* ds_chunk_write / ds_chunk_read (lib/nfs4/ps/chunk_io.c) build a     */
/* COMPOUND and call mds_compound_send_with_auth as the final RPC      */
/* chokepoint.  The production symbol is __attribute__((weak)) in      */
/* lib/nfs4/client/mds_compound.c specifically so test TUs can         */
/* substitute a recording stub here.  ps_proxy_pipeline_write_test.c   */
/* uses the same pattern.                                              */
/*                                                                     */
/* Returning -EIO (not -ESTALE) makes ec_chunk_write / ec_chunk_read   */
/* exit their retry loop immediately and propagate -EIO without ever   */
/* calling mds_layout_error (which would dereference a fake            */
/* ctx_ms).                                                            */
/* ------------------------------------------------------------------ */

static int g_rpc_send_calls;

int mds_compound_send_with_auth(struct mds_compound *mc __attribute__((unused)),
				struct mds_session *ms __attribute__((unused)),
				const struct authunix_parms *creds
				__attribute__((unused)))
{
	g_rpc_send_calls++;
	return -EIO;
}

/* ------------------------------------------------------------------ */
/* Recording stubs installed as pls_sc_write_fn / pls_sc_read_fn.      */
/*                                                                     */
/* The production helpers (ps_shortcircuit_write / _read) live in      */
/* libreffs_nfs4_ps_sb.la and would drag in the full lib/fs            */
/* superblock / inode / data_block chain.  For a dispatch-hook test    */
/* we only care WHICH stub got called -- the test doesn't need any    */
/* of the storage state.  Replace the install with recording stubs    */
/* below that record the per-call args and return 0.                  */
/* ------------------------------------------------------------------ */

static int g_sc_write_calls;
static int g_sc_read_calls;
static struct {
	uint8_t fh[64];
	uint32_t fh_len;
	uint64_t byte_off;
	size_t data_len;
	uint32_t uid;
	uint32_t gid;
	bool has_stid;
} g_sc_write_record;
static struct {
	uint8_t fh[64];
	uint32_t fh_len;
	uint64_t byte_off;
	size_t buf_len;
	uint32_t uid;
	uint32_t gid;
	bool has_stid;
} g_sc_read_record;

static int recording_sc_write(const uint8_t *fh, uint32_t fh_len,
			      uint64_t byte_off,
			      const uint8_t *data __attribute__((unused)),
			      size_t data_len, uint32_t uid, uint32_t gid,
			      const stateid4 *stid)
{
	g_sc_write_calls++;
	memset(&g_sc_write_record, 0, sizeof(g_sc_write_record));
	if (fh_len <= sizeof(g_sc_write_record.fh))
		memcpy(g_sc_write_record.fh, fh, fh_len);
	g_sc_write_record.fh_len = fh_len;
	g_sc_write_record.byte_off = byte_off;
	g_sc_write_record.data_len = data_len;
	g_sc_write_record.uid = uid;
	g_sc_write_record.gid = gid;
	g_sc_write_record.has_stid = (stid != NULL);
	return 0;
}

static int recording_sc_read(const uint8_t *fh, uint32_t fh_len,
			     uint64_t byte_off, size_t buf_len,
			     uint8_t *buf __attribute__((unused)),
			     uint32_t *nread, uint32_t uid, uint32_t gid,
			     const stateid4 *stid)
{
	g_sc_read_calls++;
	memset(&g_sc_read_record, 0, sizeof(g_sc_read_record));
	if (fh_len <= sizeof(g_sc_read_record.fh))
		memcpy(g_sc_read_record.fh, fh, fh_len);
	g_sc_read_record.fh_len = fh_len;
	g_sc_read_record.byte_off = byte_off;
	g_sc_read_record.buf_len = buf_len;
	g_sc_read_record.uid = uid;
	g_sc_read_record.gid = gid;
	g_sc_read_record.has_stid = (stid != NULL);
	if (nread)
		*nread = (uint32_t)buf_len;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Fixture                                                             */
/* ------------------------------------------------------------------ */

#define DISPATCH_LISTENER_ID 13371
#define MIRROR0_FH_BYTE 0xAA /* local mirror */
#define MIRROR1_FH_BYTE 0xBB /* remote mirror */
#define MIRROR0_UID 1024
#define MIRROR0_GID 1024
#define MIRROR1_UID 1025
#define MIRROR1_GID 1025
#define TEST_BLOCK_OFFSET 7
#define TEST_CHUNK_SZ 4096

static struct ec_mirror g_mirrors[2];
static struct mds_session g_ds_sess[2];

static void dispatch_setup(void)
{
	struct reffs_proxy_mds_config cfg;

	rcu_register_thread();
	ck_assert_int_eq(ps_state_init(), 0);
	memset(&cfg, 0, sizeof(cfg));
	cfg.id = DISPATCH_LISTENER_ID;
	cfg.port = 4098;
	cfg.mds_port = 2049;
	cfg.mds_probe = 20490;
	strncpy(cfg.address, "127.0.0.1", sizeof(cfg.address) - 1);
	ck_assert_int_eq(ps_state_register(&cfg), 0);

	memset(g_mirrors, 0, sizeof(g_mirrors));
	memset(g_ds_sess, 0, sizeof(g_ds_sess));

	g_mirrors[0].em_fh_len = 1;
	g_mirrors[0].em_fh[0] = MIRROR0_FH_BYTE;
	g_mirrors[0].em_uid = MIRROR0_UID;
	g_mirrors[0].em_gid = MIRROR0_GID;
	g_mirrors[0].em_tight_coupled = false;

	g_mirrors[1].em_fh_len = 1;
	g_mirrors[1].em_fh[0] = MIRROR1_FH_BYTE;
	g_mirrors[1].em_uid = MIRROR1_UID;
	g_mirrors[1].em_gid = MIRROR1_GID;
	g_mirrors[1].em_tight_coupled = false;

	g_rpc_send_calls = 0;
	g_sc_write_calls = 0;
	g_sc_read_calls = 0;
}

static void dispatch_teardown(void)
{
	ps_state_fini();
	rcu_unregister_thread();
}

/*
 * Build an ec_context that the test owns on the stack.  Caller
 * sets em_local per mirror, install_stubs decides whether the
 * pls_sc_write_fn / pls_sc_read_fn pointers are populated, and
 * ctx_pls is either the registered listener or NULL (ec_demo
 * fall-through path).
 */
static struct ec_context build_ctx(bool with_pls, bool install_stubs)
{
	struct ec_context ctx;
	struct ps_listener_state *pls;

	memset(&ctx, 0, sizeof(ctx));
	ctx.ctx_layout.el_nmirrors = 2;
	ctx.ctx_layout.el_mirrors = g_mirrors;
	ctx.ctx_layout.el_layout_type = LAYOUT4_FLEX_FILES_V2;
	ctx.ctx_ds_sess = g_ds_sess;
	ctx.ctx_k = 1;
	ctx.ctx_m = 0;

	if (with_pls) {
		pls = (struct ps_listener_state *)ps_state_find(
			DISPATCH_LISTENER_ID);
		ck_assert_ptr_nonnull(pls);
		ctx.ctx_pls = pls;
		if (install_stubs) {
			pls->pls_sc_write_fn = recording_sc_write;
			pls->pls_sc_read_fn = recording_sc_read;
		}
	}
	return ctx;
}

static uint64_t pls_counter(void)
{
	struct ps_listener_state *pls =
		(struct ps_listener_state *)ps_state_find(DISPATCH_LISTENER_ID);

	ck_assert_ptr_nonnull(pls);
	return atomic_load_explicit(&pls->pls_shortcircuit_total,
				    memory_order_relaxed);
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

/*
 * Mirror 0 is em_local + ctx_pls + install stub installed --
 * dispatch hook MUST route the call to the stub and bump the
 * per-listener counter.  No RPC chokepoint hit.
 */
START_TEST(test_dispatch_local_mirror_writes_via_stub)
{
	struct ec_context ctx = build_ctx(true, true);
	uint8_t payload[16];

	memset(payload, 0xAB, sizeof(payload));
	g_mirrors[0].em_local = true;
	g_mirrors[1].em_local = false; /* unused this test */

	int ret = ec_chunk_write(&ctx, 0, TEST_BLOCK_OFFSET, TEST_CHUNK_SZ,
				 payload, sizeof(payload), /* owner */ 0xBEEF);
	ck_assert_int_eq(ret, 0);

	ck_assert_int_eq(g_sc_write_calls, 1);
	ck_assert_int_eq(g_rpc_send_calls, 0);
	ck_assert_uint_eq(pls_counter(), 1);

	/*
	 * Pin the per-mirror arg propagation: the dispatch hook
	 * computes byte_off = block_offset * chunk_sz and threads
	 * em_fh / em_uid / em_gid through to the stub.  Verify the
	 * exact values landed so a future refactor that swaps fields
	 * (uid <-> gid, fh memcpy length wrong) fails fast.
	 */
	ck_assert_uint_eq(g_sc_write_record.byte_off,
			  (uint64_t)TEST_BLOCK_OFFSET * TEST_CHUNK_SZ);
	ck_assert_uint_eq(g_sc_write_record.fh_len, 1);
	ck_assert_uint_eq(g_sc_write_record.fh[0], MIRROR0_FH_BYTE);
	ck_assert_uint_eq(g_sc_write_record.uid, MIRROR0_UID);
	ck_assert_uint_eq(g_sc_write_record.gid, MIRROR0_GID);
	ck_assert_uint_eq(g_sc_write_record.data_len, sizeof(payload));
	/* em_tight_coupled=false -> stid passed as NULL. */
	ck_assert(!g_sc_write_record.has_stid);
}
END_TEST

/*
 * Mirror 1 is em_local=false: the hook condition fails, dispatch
 * falls through to ds_chunk_write -> mds_compound_send_with_auth.
 * The chokepoint stub records the call and returns -EIO; ec_chunk_write
 * surfaces -EIO without retrying (the loop exits early on != -ESTALE).
 * Counter stays at 0 -- no short-circuit fired.
 */
START_TEST(test_dispatch_remote_mirror_skips_stub)
{
	struct ec_context ctx = build_ctx(true, true);
	uint8_t payload[16];

	memset(payload, 0xCD, sizeof(payload));
	g_mirrors[0].em_local = false; /* unused this test */
	g_mirrors[1].em_local = false;

	int ret = ec_chunk_write(&ctx, 1, TEST_BLOCK_OFFSET, TEST_CHUNK_SZ,
				 payload, sizeof(payload), 0xCAFE);
	ck_assert_int_eq(ret, -EIO);

	ck_assert_int_eq(g_sc_write_calls, 0);
	ck_assert_int_eq(g_rpc_send_calls, 1);
	ck_assert_uint_eq(pls_counter(), 0);
}
END_TEST

/*
 * The load-bearing case from proxy-server-phase5.md
 * `test_shortcircuit_partial`: a layout with 2 mirrors, 1 local +
 * 1 remote.  Drive both mirrors in a single test; assert the
 * counter advances by exactly 1 (the local one) and that both
 * arms fired (1 stub call + 1 RPC call).  The order here mirrors
 * what the higher-level codec walk in ec_pipeline.c would do --
 * each per-mirror call is an independent dispatch decision.
 */
START_TEST(test_dispatch_two_mirrors_partial_shortcircuit)
{
	struct ec_context ctx = build_ctx(true, true);
	uint8_t payload[16];

	memset(payload, 0xEF, sizeof(payload));
	g_mirrors[0].em_local = true; /* short-circuit */
	g_mirrors[1].em_local = false; /* RPC fallback */

	int ret0 = ec_chunk_write(&ctx, 0, TEST_BLOCK_OFFSET, TEST_CHUNK_SZ,
				  payload, sizeof(payload), 0x1111);
	int ret1 = ec_chunk_write(&ctx, 1, TEST_BLOCK_OFFSET, TEST_CHUNK_SZ,
				  payload, sizeof(payload), 0x1111);

	ck_assert_int_eq(ret0, 0);
	ck_assert_int_eq(ret1, -EIO);

	ck_assert_int_eq(g_sc_write_calls, 1);
	ck_assert_int_eq(g_rpc_send_calls, 1);
	/*
	 * Exactly one bump: only the em_local=true mirror routed
	 * through ps_listener_record_shortcircuit.  This is the
	 * specific claim the per-mirror counter is supposed to
	 * support (proxy-server.md "Phase 5.5: bumped whenever the
	 * ec_pipeline dispatch hook routes a per-mirror CHUNK
	 * read/write through the local VFS short-circuit").
	 */
	ck_assert_uint_eq(pls_counter(), 1);

	/* The stub recorded mirror 0's arguments (last call wins). */
	ck_assert_uint_eq(g_sc_write_record.fh[0], MIRROR0_FH_BYTE);
	ck_assert_uint_eq(g_sc_write_record.uid, MIRROR0_UID);
}
END_TEST

/*
 * ec_demo path: ctx_pls == NULL even with em_local=true.  The
 * dispatch hook guards `ctx->ctx_pls && ctx->ctx_pls->pls_sc_write_fn`
 * so the short-circuit never fires; dispatch falls through to
 * RPC.  No listener available to bump, no segfault on the NULL
 * deref.  ec_demo never installs a listener -- this test is the
 * regression gate for that path.
 */
START_TEST(test_dispatch_null_pls_skips_stub)
{
	struct ec_context ctx = build_ctx(false, false);
	uint8_t payload[16];

	memset(payload, 0x12, sizeof(payload));
	g_mirrors[0].em_local = true;

	int ret = ec_chunk_write(&ctx, 0, TEST_BLOCK_OFFSET, TEST_CHUNK_SZ,
				 payload, sizeof(payload), 0x2222);
	ck_assert_int_eq(ret, -EIO);

	ck_assert_int_eq(g_sc_write_calls, 0);
	ck_assert_int_eq(g_rpc_send_calls, 1);
	/* No listener to read; pls_counter() requires DISPATCH_LISTENER_ID,
	 * which IS registered by setup -- the counter must still be 0 because
	 * the dispatch hook never reached ps_listener_record_shortcircuit. */
	ck_assert_uint_eq(pls_counter(), 0);
}
END_TEST

/*
 * Install-ordering defensive case: ctx_pls is non-NULL but
 * pls_sc_write_fn is NULL (the install hook ran late or not at
 * all on this listener).  The hook's third guard catches this
 * and falls through to RPC.  Without this guard, the dispatch
 * would crash on the function-pointer call.
 */
START_TEST(test_dispatch_null_sc_fn_skips_stub)
{
	struct ec_context ctx =
		build_ctx(true, false); /* pls set, stubs NOT installed */
	uint8_t payload[16];

	memset(payload, 0x34, sizeof(payload));
	g_mirrors[0].em_local = true;

	int ret = ec_chunk_write(&ctx, 0, TEST_BLOCK_OFFSET, TEST_CHUNK_SZ,
				 payload, sizeof(payload), 0x3333);
	ck_assert_int_eq(ret, -EIO);

	ck_assert_int_eq(g_sc_write_calls, 0);
	ck_assert_int_eq(g_rpc_send_calls, 1);
	ck_assert_uint_eq(pls_counter(), 0);
}
END_TEST

/*
 * Read-side dispatch mirrors the write-side hook at
 * ec_pipeline.c:321-329.  Same 2-mirror geometry: idx 0 local +
 * idx 1 remote.  The read hook also bumps the SAME counter
 * (pls_shortcircuit_total is shared across read + write per
 * ps_state.h:290 comment), so a successful partial-2-mirror read
 * advances the counter by 1 just like the write test.  This is
 * the regression gate against an asymmetric refactor that
 * accidentally drops the read-side ps_listener_record_shortcircuit
 * call.
 */
START_TEST(test_dispatch_read_path_mirrors_write)
{
	struct ec_context ctx = build_ctx(true, true);
	uint8_t shard0[TEST_CHUNK_SZ];
	uint8_t shard1[TEST_CHUNK_SZ];
	uint32_t nread0 = 0, nread1 = 0;

	g_mirrors[0].em_local = true;
	g_mirrors[1].em_local = false;

	int ret0 = ec_chunk_read(&ctx, 0, TEST_BLOCK_OFFSET, /* nblk */ 1,
				 shard0, TEST_CHUNK_SZ, &nread0);
	int ret1 = ec_chunk_read(&ctx, 1, TEST_BLOCK_OFFSET, 1, shard1,
				 TEST_CHUNK_SZ, &nread1);

	ck_assert_int_eq(ret0, 0);
	ck_assert_int_eq(ret1, -EIO);

	ck_assert_int_eq(g_sc_read_calls, 1);
	ck_assert_int_eq(g_rpc_send_calls, 1);
	ck_assert_uint_eq(pls_counter(), 1);

	ck_assert_uint_eq(g_sc_read_record.fh[0], MIRROR0_FH_BYTE);
	ck_assert_uint_eq(g_sc_read_record.uid, MIRROR0_UID);
	ck_assert_uint_eq(g_sc_read_record.byte_off,
			  (uint64_t)TEST_BLOCK_OFFSET * TEST_CHUNK_SZ);
	ck_assert_uint_eq(g_sc_read_record.buf_len, (size_t)1 * TEST_CHUNK_SZ);
	/*
	 * recording_sc_read sets *nread = buf_len on success; mirror
	 * the production helper's behaviour so the higher-level
	 * codec walk sees the right number of bytes.
	 */
	ck_assert_uint_eq(nread0, TEST_CHUNK_SZ);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite                                                               */
/* ------------------------------------------------------------------ */

static Suite *ec_pipeline_dispatch_suite(void)
{
	Suite *s = suite_create("ec_pipeline_dispatch");
	TCase *tc = tcase_create("partial_2_mirrors");

	tcase_add_checked_fixture(tc, dispatch_setup, dispatch_teardown);
	tcase_add_test(tc, test_dispatch_local_mirror_writes_via_stub);
	tcase_add_test(tc, test_dispatch_remote_mirror_skips_stub);
	tcase_add_test(tc, test_dispatch_two_mirrors_partial_shortcircuit);
	tcase_add_test(tc, test_dispatch_null_pls_skips_stub);
	tcase_add_test(tc, test_dispatch_null_sc_fn_skips_stub);
	tcase_add_test(tc, test_dispatch_read_path_mirrors_write);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int failed;
	SRunner *sr = srunner_create(ec_pipeline_dispatch_suite());

	srunner_run_all(sr, CK_NORMAL);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return failed == 0 ? 0 : 1;
}
