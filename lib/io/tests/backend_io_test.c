/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * Backend file-I/O framework smoke test.
 *
 * Exercises io_request_backend_pread / io_request_backend_pwrite end to
 * end: init the backend ring, run the main loop on a helper thread,
 * submit a write, submit a read, check the round-tripped contents.
 * Backend-agnostic (compiles and runs under any of IO_BACKEND_LIBURING,
 * IO_BACKEND_KQUEUE, or IO_BACKEND_DARWIN).
 *
 * Not a stress/error test; the goal is one happy-path assurance that
 * the submission -> worker -> completion -> handler chain is correctly
 * wired for the selected backend.  Follow-ups (short I/O, closed fd,
 * backpressure) go in a separate commit (#19).
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <check.h>

#include "reffs/io.h"
#include "reffs/log.h"
#include "reffs/ring.h"
#include "reffs/rpc.h"

struct rpc_trans *rpc_trans_create(void);
void rpc_protocol_free(struct rpc_trans *rt);

static struct ring_context *g_rc;
static pthread_t g_loop_thread;
static volatile sig_atomic_t g_running;

static void *backend_loop(void *arg)
{
	(void)arg;
	io_backend_main_loop(&g_running, g_rc);
	return NULL;
}

static void setup(void)
{
	g_rc = ring_context_alloc();
	ck_assert_ptr_nonnull(g_rc);
	ck_assert_int_eq(io_backend_init(g_rc), 0);
	io_backend_set_global(g_rc);
	g_running = 1;
	ck_assert_int_eq(
		pthread_create(&g_loop_thread, NULL, backend_loop, NULL), 0);
}

static void teardown(void)
{
	g_running = 0;
	pthread_join(g_loop_thread, NULL);
	io_backend_fini(g_rc);
	ring_context_free(g_rc);
	g_rc = NULL;
}

/*
 * Wait for rt->rt_io_result to transition from the sentinel value to
 * something else, signalling the completion handler ran.  Spin-wait with
 * a cheap poll; bounded by a generous timeout so a broken framework
 * fails the test cleanly rather than hanging CI.
 */
static bool wait_for_completion(struct rpc_trans *rt, int64_t sentinel,
				int timeout_ms)
{
	for (int waited = 0; waited < timeout_ms; waited += 5) {
		int64_t v =
			__atomic_load_n(&rt->rt_io_result, __ATOMIC_ACQUIRE);
		if (v != sentinel)
			return true;
		usleep(5000);
	}
	return false;
}

START_TEST(test_pwrite_then_pread_roundtrip)
{
	/* Temp file for I/O. */
	char path[] = "/tmp/reffs-backend-io-test-XXXXXX";
	int fd = mkstemp(path);
	ck_assert_int_ge(fd, 0);

	const char payload[] = "the framework works";
	const size_t len = sizeof(payload); /* include NUL for easy compare */

	/* --- pwrite path --- */
	struct rpc_trans *rt_w = rpc_trans_create();
	ck_assert_ptr_nonnull(rt_w);
	rt_w->rt_task = NULL; /* no task -- skip task_resume branch */
	rt_w->rt_io_result = -1; /* sentinel */

	ck_assert_int_eq(
		io_request_backend_pwrite(fd, payload, len, 0, rt_w, g_rc), 0);
	ck_assert_msg(wait_for_completion(rt_w, -1, 2000),
		      "pwrite completion did not fire within 2s");
	ck_assert_int_eq((int)rt_w->rt_io_result, (int)len);
	rpc_protocol_free(rt_w);

	/* --- pread path --- */
	char buf[64] = { 0 };
	struct rpc_trans *rt_r = rpc_trans_create();
	ck_assert_ptr_nonnull(rt_r);
	rt_r->rt_task = NULL;
	rt_r->rt_io_result = -1;

	ck_assert_int_eq(io_request_backend_pread(fd, buf, len, 0, rt_r, g_rc),
			 0);
	ck_assert_msg(wait_for_completion(rt_r, -1, 2000),
		      "pread completion did not fire within 2s");
	ck_assert_int_eq((int)rt_r->rt_io_result, (int)len);
	ck_assert_mem_eq(buf, payload, len);
	rpc_protocol_free(rt_r);

	close(fd);
	unlink(path);
}
END_TEST

static Suite *backend_io_suite(void)
{
	Suite *s = suite_create("backend_io");
	TCase *tc = tcase_create("roundtrip");
	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_set_timeout(tc, 10);
	tcase_add_test(tc, test_pwrite_then_pread_roundtrip);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	Suite *s = backend_io_suite();
	SRunner *sr = srunner_create(s);
	srunner_run_all(sr, CK_NORMAL);
	int failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
