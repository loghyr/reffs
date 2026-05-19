/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * conn_info connection-lifecycle race reproducer (INV-5 / INV-6).
 *
 * This is NOT a passing make-check test.  It is a deliberate red
 * reproducer that anchors the lib/io/ connection-lifecycle fix slice.
 * It is built (check_PROGRAMS) but is NOT listed in TESTS, so
 * `make check` compiles it without running it.  Run it by hand under
 * a sanitizer build:
 *
 *     ../configure --enable-tsan && make
 *     ./lib/io/tests/conn_lifecycle_race_test  # expect TSAN race on ci_ssl
 *
 *     ../configure --enable-asan && make
 *     ./lib/io/tests/conn_lifecycle_race_test  # expect ASAN UAF on the SSL
 *
 * The defect (see .claude/design/experiments.md, "INV-5/INV-6 root
 * cause"): io_conn_get() returns a struct conn_info * after dropping
 * conn_mutex.  Every consumer then reads ci->ci_ssl and uses the SSL
 * object outside the lock -- handlers.c:281 is the live example.  A
 * concurrent io_conn_unregister() does SSL_shutdown + SSL_free on
 * ci_ssl and NULLs the pointer under conn_mutex.  The reader therefore
 * races the free:
 *   - TSAN: a data race on the ci_ssl field itself (unlocked plain
 *     read in the reader vs. locked write in unregister/churn).
 *   - ASAN: a heap-use-after-free when the reader dereferences an SSL
 *     object that unregister already freed.
 *
 * INV-5 (concurrent PS->MDS establishment) and INV-6 (mid-load
 * teardown) are the same defect class: conn_info has no lifecycle
 * discipline.  This reproducer exercises the teardown/reuse window
 * directly.
 *
 * When the fix lands (refcount ci_ssl, a CONN_CLOSING state, and a
 * generation check beyond the write gate), this file is rewritten as
 * a passing concurrency test and moved into TESTS.
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

#include "reffs/io.h"

#include "io_internal.h"

/*
 * Four fd numbers, well clear of stdio and of conn_info_test's
 * 201-203.  One churn thread and several reader threads per fd; the
 * churn thread owns the fd's register/unregister cycle so io_conn_*
 * table mutation is serialized per fd -- the race under test is purely
 * the unlocked use of the returned conn_info's ci_ssl field.
 */
#define NR_FDS 4
#define FD_BASE 311
#define CHURN_ITERS 8000
#define READERS_PER_FD 3

static SSL_CTX *test_ctx;

/*
 * Count of churn threads still running.  Readers spin until it hits
 * zero.  C11 atomic per .claude/standards.md (new code uses C11).
 */
static _Atomic int churn_alive;

/*
 * Churn thread: repeatedly establish and tear down the connection on
 * one fd, mirroring a PS<->MDS connection that flaps under load.  Each
 * register hangs a fresh SSL object off ci_ssl the way the TLS accept
 * path does; io_conn_unregister() then SSL_free()s it under conn_mutex.
 */
static void *churn_thread(void *arg)
{
	int fd = (int)(intptr_t)arg;

	for (int i = 0; i < CHURN_ITERS; i++) {
		struct conn_info *ci =
			io_conn_register(fd, CONN_CONNECTED, CONN_ROLE_SERVER);
		if (ci) {
			ci->ci_ssl = SSL_new(test_ctx);
			ci->ci_tls_enabled = true;
		}
		/*
		 * io_conn_unregister() does SSL_shutdown + SSL_free(ci_ssl)
		 * + ci_ssl = NULL under conn_mutex.  A reader that already
		 * pulled this conn_info out of io_conn_get() and is using
		 * ci_ssl outside the lock now races the free.
		 */
		io_conn_unregister(fd);
	}

	atomic_fetch_sub_explicit(&churn_alive, 1, memory_order_acq_rel);
	return NULL;
}

/*
 * Reader thread: the handlers.c:281 pattern verbatim in shape -- pull
 * the conn_info out of io_conn_get(), test ci_ssl + ci_tls_enabled,
 * then dereference the SSL.  None of it is under conn_mutex, because
 * io_conn_get() dropped the lock before returning.
 */
static void *reader_thread(void *arg)
{
	int fd = (int)(intptr_t)arg;

	while (atomic_load_explicit(&churn_alive, memory_order_acquire) > 0) {
		struct conn_info *ci = io_conn_get(fd);
		if (!ci)
			continue;
		if (ci->ci_ssl && ci->ci_tls_enabled) {
			SSL *s = ci->ci_ssl;
			/* Dereferences the SSL -- UAF if it was freed. */
			(void)SSL_get_version(s);
		}
	}
	return NULL;
}

int main(void)
{
	test_ctx = SSL_CTX_new(TLS_method());
	if (!test_ctx) {
		fprintf(stderr, "SSL_CTX_new failed\n");
		return 2;
	}

	io_conn_init();

	fprintf(stderr,
		"conn_lifecycle_race_test: reproducing the INV-5/INV-6\n"
		"unguarded-ci_ssl race.  Under --enable-tsan expect a data\n"
		"race report on conn_info.ci_ssl; under --enable-asan expect\n"
		"a heap-use-after-free on the SSL object.  A non-sanitizer\n"
		"run may pass silently or crash.\n");

	pthread_t churn[NR_FDS];
	pthread_t readers[NR_FDS][READERS_PER_FD];

	atomic_store_explicit(&churn_alive, NR_FDS, memory_order_release);

	for (int f = 0; f < NR_FDS; f++) {
		intptr_t fd = FD_BASE + f;

		for (int r = 0; r < READERS_PER_FD; r++)
			pthread_create(&readers[f][r], NULL, reader_thread,
				       (void *)fd);
		pthread_create(&churn[f], NULL, churn_thread, (void *)fd);
	}

	for (int f = 0; f < NR_FDS; f++) {
		pthread_join(churn[f], NULL);
		for (int r = 0; r < READERS_PER_FD; r++)
			pthread_join(readers[f][r], NULL);
	}

	io_conn_cleanup();
	SSL_CTX_free(test_ctx);

	fprintf(stderr, "conn_lifecycle_race_test: churn complete.\n");
	return 0;
}
