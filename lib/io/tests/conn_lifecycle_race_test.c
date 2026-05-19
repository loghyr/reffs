/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * conn_info connection-lifecycle no-regression check (INV-5 / INV-6).
 *
 * Slice 1 of .claude/design/io-conn-lifecycle.md refcounts the SSL
 * object hung off conn_info and exposes a lock-safe accessor pair
 * (io_conn_ssl_install / _acquire / _release / _clear, plus
 * io_conn_tls_snapshot / _set_state).  With those primitives in
 * place the unguarded ci_ssl access pattern that produced the
 * INV-5 / INV-6 race no longer exists; this test stays in the
 * suite to keep it that way.
 *
 * The shape mirrors the original red reproducer -- a churn thread
 * per fd repeatedly establishes and tears down the connection, and
 * a pool of reader threads exercises the same fd in parallel.  Only
 * the access pattern changed: readers no longer pull a conn_info
 * out of io_conn_get() and dereference ci_ssl outside conn_mutex;
 * they pin the SSL via io_conn_ssl_acquire() (which takes a use ref
 * under conn_mutex), use it, then drop it via io_conn_ssl_release().
 *
 * Expected behaviour under sanitizer builds:
 *   - TSAN: no data race on conn_info.ci_ssl (the field is only ever
 *     read/written under conn_mutex by the lib/io API).
 *   - ASAN: no heap-use-after-free on the SSL object (an outstanding
 *     use ref keeps the SSL alive past a concurrent slot-ref drop).
 *
 * If a future change reintroduces unsynchronised access to ci_ssl
 * or drops the SSL while a use ref is outstanding, the corresponding
 * sanitizer will fire here and this test will go red.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <openssl/ssl.h>

#include <check.h>

#include "reffs/io.h"

/*
 * Four fd numbers, well clear of stdio and of conn_info_test's
 * 201-203.  CHURN_ITERS and the per-fd reader count are tuned so the
 * test fits the standards.md two-second budget even under TSAN; the
 * goal is enough register/unregister cycles to interleave with the
 * reader threads, not raw throughput.
 */
#define NR_FDS 4
#define FD_BASE 311
#define CHURN_ITERS 1000
#define READERS_PER_FD 2

static SSL_CTX *test_ctx;

/*
 * Count of churn threads still running.  Readers spin until it hits
 * zero.  C11 atomic per .claude/standards.md (new code uses C11).
 */
static _Atomic int churn_alive;

/*
 * Churn thread: repeatedly establish and tear down the connection on
 * one fd, mirroring a PS<->MDS connection that flaps under load.
 * Each register installs a fresh SSL via io_conn_ssl_install() (which
 * adopts SSL_new()'s +1 ref as the slot ref); io_conn_unregister()
 * detaches and drops it under the discipline added in Slice 1.
 */
static void *churn_thread(void *arg)
{
	int fd = (int)(intptr_t)arg;

	for (int i = 0; i < CHURN_ITERS; i++) {
		struct conn_info *ci =
			io_conn_register(fd, CONN_CONNECTED, CONN_ROLE_SERVER);
		(void)ci;
		SSL *ssl = SSL_new(test_ctx);
		if (ssl) {
			io_conn_ssl_install(fd, ssl);
			io_conn_tls_set_state(fd, true, false);
		}
		/*
		 * io_conn_unregister() detaches ci_ssl under conn_mutex
		 * and drops the slot ref outside the lock.  An acquire
		 * call racing the unregister either returns NULL (slot
		 * already detached) or returns a pinned SSL (slot still
		 * held the ref when acquire ran); either is safe.
		 */
		io_conn_unregister(fd);
	}

	atomic_fetch_sub_explicit(&churn_alive, 1, memory_order_acq_rel);
	return NULL;
}

/*
 * Reader thread: pin the SSL via the safe API, use it, drop it.
 * Snapshot the TLS flag first to mimic the handlers.c hot path --
 * io_handle_read / io_do_tls both gate on the flag before reaching
 * for the SSL.
 */
static void *reader_thread(void *arg)
{
	int fd = (int)(intptr_t)arg;

	while (atomic_load_explicit(&churn_alive, memory_order_acquire) > 0) {
		bool tls_enabled = false;
		(void)io_conn_tls_snapshot(fd, &tls_enabled, NULL);
		if (!tls_enabled)
			continue;
		SSL *s = io_conn_ssl_acquire(fd);
		if (!s)
			continue;
		/*
		 * SSL_get_version dereferences the SSL.  With the Slice 1
		 * fix in place the use ref taken by acquire keeps this
		 * memory live even if a concurrent unregister drops the
		 * slot ref between the snapshot and now.
		 */
		(void)SSL_get_version(s);
		io_conn_ssl_release(s);
	}
	return NULL;
}

START_TEST(test_no_race_churn_vs_readers)
{
	test_ctx = SSL_CTX_new(TLS_method());
	ck_assert_ptr_nonnull(test_ctx);

	io_conn_init();

	pthread_t churn[NR_FDS];
	pthread_t readers[NR_FDS][READERS_PER_FD];

	atomic_store_explicit(&churn_alive, NR_FDS, memory_order_release);

	for (int f = 0; f < NR_FDS; f++) {
		intptr_t fd = FD_BASE + f;

		for (int r = 0; r < READERS_PER_FD; r++)
			ck_assert_int_eq(pthread_create(&readers[f][r], NULL,
							reader_thread,
							(void *)fd),
					 0);
		ck_assert_int_eq(pthread_create(&churn[f], NULL, churn_thread,
						(void *)fd),
				 0);
	}

	for (int f = 0; f < NR_FDS; f++) {
		pthread_join(churn[f], NULL);
		for (int r = 0; r < READERS_PER_FD; r++)
			pthread_join(readers[f][r], NULL);
	}

	io_conn_cleanup();
	SSL_CTX_free(test_ctx);
	test_ctx = NULL;
}
END_TEST

static Suite *conn_lifecycle_race_suite(void)
{
	Suite *s = suite_create("conn_lifecycle_race");
	TCase *tc = tcase_create("core");

	/*
	 * The churn loop runs CHURN_ITERS * NR_FDS = 4000 register /
	 * unregister cycles against READERS_PER_FD * NR_FDS = 8 reader
	 * threads.  Under TSAN this comfortably fits two seconds on
	 * dreamer; bump the timeout slightly so a heavily loaded CI
	 * host does not false-positive.
	 */
	tcase_set_timeout(tc, 10);
	tcase_add_test(tc, test_no_race_churn_vs_readers);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	Suite *s = conn_lifecycle_race_suite();
	SRunner *sr = srunner_create(s);

	srunner_run_all(sr, CK_NORMAL);
	int failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
