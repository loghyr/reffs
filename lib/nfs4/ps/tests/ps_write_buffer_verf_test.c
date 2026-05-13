/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * Group C tests (PS Phase 4b slice 4b.4): composed write verifier.
 *
 * The composer ps_compose_write_verf folds two halves into the
 * 8-byte verifier returned in WRITE / COMMIT replies:
 *   - Listener half: pls_boot_gen (changes on PS restart).
 *   - MDS half: captured CHUNK_COMMIT writeverf (changes on
 *     upstream DS reboot).
 *
 * These tests pin: both halves contribute, the MDS half toggles
 * the composed value, and the no-MDS-verf-yet path falls back to
 * listener-only.  See .claude/design/proxy-server-phase4b.md
 * "Tests first" Group C and "Composed verifier".
 *
 * The actual capture from CHUNK_COMMIT into the buffer is
 * exercised end-to-end by scripts/ci_ps_phase4b_test.sh against
 * a live MDS+DS topology; here we drive the composer directly
 * and inject the buffer state via the whitebox surface.
 */

#include <check.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <urcu.h>

#include "reffs/settings.h"
#include "ps_state.h"
#include "ps_write_buffer.h"
#include "ps_write_buffer_internal.h"

/* ------------------------------------------------------------------ */
/* Fixture                                                             */
/* ------------------------------------------------------------------ */

#define TEST_LISTENER_ID 17

static void setup(void)
{
	rcu_register_thread();
	ps_state_init();
}

static void teardown(void)
{
	ps_state_fini();
	rcu_unregister_thread();
}

static struct reffs_proxy_mds_config make_cfg(uint32_t id)
{
	struct reffs_proxy_mds_config cfg;

	memset(&cfg, 0, sizeof(cfg));
	cfg.id = id;
	cfg.port = 4098;
	cfg.mds_port = 2049;
	cfg.mds_probe = 20490;
	strncpy(cfg.address, "10.0.0.1", sizeof(cfg.address) - 1);
	return cfg;
}

static struct ps_listener_state *mut_listener(uint32_t id)
{
	return (struct ps_listener_state *)ps_state_find(id);
}

/*
 * Force pls_boot_gen to a known non-trivial value so the composed
 * output is identifiable across tests.  ps_state_register starts
 * pls_boot_gen at 1; we bump it explicitly here.
 */
static void force_boot_gen(struct ps_listener_state *pls, uint64_t gen)
{
	atomic_store_explicit(&pls->pls_boot_gen, gen, memory_order_release);
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

START_TEST(test_listener_verf_combines_mds_and_pls)
{
	/*
	 * Verify both halves contribute to the output: the listener
	 * half is the boot-gen packed into 8 bytes, the MDS half is
	 * XOR'd on top.  We pick a boot-gen with non-zero high bits
	 * and an MDS verifier that flips half the bits in the listener
	 * half; the composed output differs from BOTH inputs in at
	 * least one byte each direction.
	 */
	struct reffs_proxy_mds_config cfg = make_cfg(TEST_LISTENER_ID);
	struct ps_listener_state *pls;

	ck_assert_int_eq(ps_state_register(&cfg), 0);
	pls = mut_listener(TEST_LISTENER_ID);
	ck_assert_ptr_nonnull(pls);

	/* Boot-gen = 0x0102030405060708 -- distinct bytes per position. */
	uint64_t gen = UINT64_C(0x0102030405060708);

	force_boot_gen(pls, gen);

	uint8_t listener_only[PS_WRITE_VERIFIER_SIZE];

	ps_compose_write_verf(pls, false, NULL, listener_only);
	/* Confirm listener-only encoding is pls_boot_gen packed into 8 bytes
	 * (memcpy of the host-native uint64 -- same encoding the 4a
	 * pwb_compose_listener_verifier used).
	 */
	ck_assert_int_eq(memcmp(listener_only, &gen, PS_WRITE_VERIFIER_SIZE),
			 0);

	uint8_t mds_verf[PS_WRITE_VERIFIER_SIZE] = { 0xFF, 0x00, 0xAA, 0x55,
						     0x12, 0x34, 0x56, 0x78 };
	uint8_t composed[PS_WRITE_VERIFIER_SIZE];

	ps_compose_write_verf(pls, true, mds_verf, composed);

	/*
	 * Composed must differ from listener-only (MDS half landed),
	 * differ from mds_verf (listener half landed), and equal the
	 * byte-wise XOR (composition contract).
	 */
	ck_assert_int_ne(
		memcmp(composed, listener_only, PS_WRITE_VERIFIER_SIZE), 0);
	ck_assert_int_ne(memcmp(composed, mds_verf, PS_WRITE_VERIFIER_SIZE), 0);
	for (size_t i = 0; i < PS_WRITE_VERIFIER_SIZE; i++)
		ck_assert_uint_eq(composed[i], listener_only[i] ^ mds_verf[i]);
}
END_TEST

START_TEST(test_listener_verf_change_on_mds_restart)
{
	/*
	 * Same listener (same boot-gen), two different MDS verifiers
	 * (V1 then V2 -- simulating an upstream DS reboot between
	 * captures).  Composed outputs differ, so the client sees a
	 * verifier mismatch on COMMIT -- closing Risk #3a from 4a.
	 */
	struct reffs_proxy_mds_config cfg = make_cfg(TEST_LISTENER_ID);
	struct ps_listener_state *pls;

	ck_assert_int_eq(ps_state_register(&cfg), 0);
	pls = mut_listener(TEST_LISTENER_ID);

	uint8_t v1[PS_WRITE_VERIFIER_SIZE] = { 0xDE, 0xAD, 0xBE, 0xEF,
					       0x00, 0x00, 0x00, 0x00 };
	uint8_t v2[PS_WRITE_VERIFIER_SIZE] = { 0xCA, 0xFE, 0xBA, 0xBE,
					       0x00, 0x00, 0x00, 0x01 };
	uint8_t composed_v1[PS_WRITE_VERIFIER_SIZE];
	uint8_t composed_v2[PS_WRITE_VERIFIER_SIZE];

	ps_compose_write_verf(pls, true, v1, composed_v1);
	ps_compose_write_verf(pls, true, v2, composed_v2);

	ck_assert_int_ne(
		memcmp(composed_v1, composed_v2, PS_WRITE_VERIFIER_SIZE), 0);
}
END_TEST

START_TEST(test_commit_returns_listener_verf_v1_then_v2)
{
	/*
	 * Sequenced flush across an MDS-verifier change: poke V1 into
	 * the buffer's pwb_mds_verf (whitebox), compose, then poke V2
	 * and compose again.  Composed outputs differ, mirroring what
	 * a real pipeline_commit sees when the captured MDS verifier
	 * changes between two successive flushes.
	 *
	 * pipeline_commit itself snapshots pwb_mds_verf under
	 * pwb_mutex before drop -- this test exercises the snapshot/
	 * compose helper path with whitebox-injected state.
	 */
	struct reffs_proxy_mds_config cfg = make_cfg(TEST_LISTENER_ID);
	struct ps_listener_state *pls;
	uint8_t stateid[PS_STATEID_OTHER_SIZE] = { 0xC4 };
	uint8_t fh[] = { 0xF4 };
	struct ps_write_buffer *buf;

	ck_assert_int_eq(ps_state_register(&cfg), 0);
	pls = mut_listener(TEST_LISTENER_ID);

	ck_assert(ps_write_buffer_enter_quiesce_or_bail(pls));
	buf = ps_write_buffer_lookup_or_alloc(pls, stateid, fh, sizeof(fh));
	ck_assert_ptr_nonnull(buf);

	/* Inject V1, snapshot, compose. */
	pthread_mutex_lock(&buf->pwb_mutex);
	memset(buf->pwb_mds_verf, 0, PS_WRITE_VERIFIER_SIZE);
	buf->pwb_mds_verf[0] = 0xAA;
	buf->pwb_mds_verf[7] = 0x01;
	buf->pwb_mds_verf_set = true;
	bool ms_set = buf->pwb_mds_verf_set;
	uint8_t snap[PS_WRITE_VERIFIER_SIZE];

	memcpy(snap, buf->pwb_mds_verf, PS_WRITE_VERIFIER_SIZE);
	pthread_mutex_unlock(&buf->pwb_mutex);

	uint8_t out_v1[PS_WRITE_VERIFIER_SIZE];

	ps_compose_write_verf(pls, ms_set, snap, out_v1);

	/* Now simulate an MDS-verifier change for the next flush. */
	pthread_mutex_lock(&buf->pwb_mutex);
	buf->pwb_mds_verf[0] = 0xBB;
	buf->pwb_mds_verf[7] = 0x02;
	ms_set = buf->pwb_mds_verf_set;
	memcpy(snap, buf->pwb_mds_verf, PS_WRITE_VERIFIER_SIZE);
	pthread_mutex_unlock(&buf->pwb_mutex);

	uint8_t out_v2[PS_WRITE_VERIFIER_SIZE];

	ps_compose_write_verf(pls, ms_set, snap, out_v2);

	ck_assert_int_ne(memcmp(out_v1, out_v2, PS_WRITE_VERIFIER_SIZE), 0);

	ps_write_buffer_drop(buf, pls);
}
END_TEST

START_TEST(test_listener_verf_mds_unknown_falls_back)
{
	/*
	 * No captured MDS verifier yet (pwb_mds_verf_set == false on a
	 * freshly-allocated buffer; or the legacy callers passing
	 * mds_verf_set=false directly).  Composer must produce the
	 * listener-only encoding -- a valid verifier that subsequent
	 * COMMITs on the same listener boot epoch will match.
	 */
	struct reffs_proxy_mds_config cfg = make_cfg(TEST_LISTENER_ID);
	struct ps_listener_state *pls;
	uint8_t stateid[PS_STATEID_OTHER_SIZE] = { 0xC5 };
	uint8_t fh[] = { 0xF5 };
	struct ps_write_buffer *buf;

	ck_assert_int_eq(ps_state_register(&cfg), 0);
	pls = mut_listener(TEST_LISTENER_ID);

	/* Fresh buffer: pwb_mds_verf_set defaults to false. */
	ck_assert(ps_write_buffer_enter_quiesce_or_bail(pls));
	buf = ps_write_buffer_lookup_or_alloc(pls, stateid, fh, sizeof(fh));
	ck_assert_ptr_nonnull(buf);

	pthread_mutex_lock(&buf->pwb_mutex);
	ck_assert(!buf->pwb_mds_verf_set);
	pthread_mutex_unlock(&buf->pwb_mutex);

	/* Compose with no MDS verfier. */
	uint8_t fallback[PS_WRITE_VERIFIER_SIZE];

	ps_compose_write_verf(pls, false, NULL, fallback);

	/* Output must equal the listener-only encoding. */
	uint64_t gen =
		atomic_load_explicit(&pls->pls_boot_gen, memory_order_acquire);
	uint8_t expected[PS_WRITE_VERIFIER_SIZE];

	memcpy(expected, &gen, PS_WRITE_VERIFIER_SIZE);
	ck_assert_int_eq(memcmp(fallback, expected, PS_WRITE_VERIFIER_SIZE), 0);

	/* And NULL mds_verf arg + mds_verf_set==false is the contract. */
	uint8_t fallback2[PS_WRITE_VERIFIER_SIZE] = { 0 };

	ps_compose_write_verf(pls, false, NULL, fallback2);
	ck_assert_int_eq(memcmp(fallback, fallback2, PS_WRITE_VERIFIER_SIZE),
			 0);

	ps_write_buffer_drop(buf, pls);
}
END_TEST

START_TEST(test_write_then_commit_verifier_equal_no_restart)
{
	/*
	 * Regression test for the slice 4b.4 contract: in the happy
	 * single-cycle case (WRITE on a fresh buffer, then a
	 * successful COMMIT that drops the buffer) the two reply
	 * verifiers MUST be byte-equal, otherwise clients see a
	 * spurious mismatch and rewrite on every cycle.
	 *
	 * Both reply paths call ps_compose_write_verf with
	 * mds_verf_set = false in this case:
	 *   - WRITE on a fresh buffer (calloc-zeroed; pwb_mds_verf_set
	 *     = false).
	 *   - COMMIT success-drop: pipeline_commit deliberately passes
	 *     mds_verf_set = false on the drop path, because the per-
	 *     stripe verifier capture only happens INSIDE the flush
	 *     loop -- AFTER the WRITE replies were already sent.
	 *     Folding the captured verifier on the COMMIT side would
	 *     trigger the unnecessary rewrite the reviewer flagged.
	 *
	 * We pin the equality via the composer directly: both paths
	 * with the same listener and mds_verf_set = false produce the
	 * same 8 bytes.  Also confirm that a HYPOTHETICAL fold
	 * (mds_verf_set = true with a non-zero verifier) would differ
	 * -- so the listener-only choice on COMMIT-drop is meaningful,
	 * not a no-op.
	 */
	struct reffs_proxy_mds_config cfg = make_cfg(TEST_LISTENER_ID);
	struct ps_listener_state *pls;
	uint8_t write_verf[PS_WRITE_VERIFIER_SIZE];
	uint8_t commit_verf[PS_WRITE_VERIFIER_SIZE];

	ck_assert_int_eq(ps_state_register(&cfg), 0);
	pls = mut_listener(TEST_LISTENER_ID);

	/*
	 * WRITE-reply equivalent: composer on a fresh buffer
	 * (pwb_mds_verf_set = false at the snapshot taken in
	 * ps_proxy_pipeline_write before the find ref is dropped).
	 */
	ps_compose_write_verf(pls, false, NULL, write_verf);

	/*
	 * COMMIT-success-drop equivalent: composer on the drop path
	 * (pipeline_commit passes false unconditionally on success).
	 */
	ps_compose_write_verf(pls, false, NULL, commit_verf);

	ck_assert_int_eq(
		memcmp(write_verf, commit_verf, PS_WRITE_VERIFIER_SIZE), 0);

	/* And confirm a hypothetical fold differs (the choice is not
	 * a no-op -- folding here WOULD cause V_w != V_c, which is
	 * exactly the bug the slice avoids on this path).
	 */
	uint8_t hypo_verf[PS_WRITE_VERIFIER_SIZE];
	uint8_t mds_v[PS_WRITE_VERIFIER_SIZE] = { 0xDE, 0xAD, 0xBE, 0xEF,
						  0x01, 0x02, 0x03, 0x04 };

	ps_compose_write_verf(pls, true, mds_v, hypo_verf);
	ck_assert_int_ne(memcmp(write_verf, hypo_verf, PS_WRITE_VERIFIER_SIZE),
			 0);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite                                                               */
/* ------------------------------------------------------------------ */

static Suite *ps_write_buffer_verf_suite(void)
{
	Suite *s = suite_create("ps_write_buffer_verf");
	TCase *tc = tcase_create("compose");

	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_listener_verf_combines_mds_and_pls);
	tcase_add_test(tc, test_listener_verf_change_on_mds_restart);
	tcase_add_test(tc, test_commit_returns_listener_verf_v1_then_v2);
	tcase_add_test(tc, test_listener_verf_mds_unknown_falls_back);
	tcase_add_test(tc, test_write_then_commit_verifier_equal_no_restart);

	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int failed;
	SRunner *sr = srunner_create(ps_write_buffer_verf_suite());

	srunner_run_all(sr, CK_NORMAL);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return failed == 0 ? 0 : 1;
}
