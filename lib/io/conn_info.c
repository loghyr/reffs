/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Connection-info bookkeeping: per-fd hash of pending I/O op counts,
 * state transitions, timeout sweeps, and the per-fd write
 * serialization gate.  Backend-agnostic -- no io_uring or kqueue
 * dependencies -- so both backends share a single implementation.
 *
 * Extracted from lib/io/connect.c (which retains the io_uring-specific
 * connect-operation logic).
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <openssl/ssl.h>
#include <openssl/sha.h>
#include <openssl/x509.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "reffs/log.h"
#include "reffs/io.h"
#include "reffs/network.h"
#include "trace/io.h"

#include "io_internal.h"

/* Array to track connection states, keyed by (fd % MAX_CONNECTIONS). */
static struct conn_info *connections[MAX_CONNECTIONS];
static pthread_mutex_t conn_mutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * Per-SSL I/O serialisation lock (Stage 3 Slice 3, INV-6).
 *
 * Memory safety for ci_ssl is in Slice 1; this lock serialises the
 * SSL state machine itself.  An ex_data index minted once at
 * io_conn_init() time tags each conn_info-owned SSL with a
 * reffs_ssl_io_lock that the install/lock/unlock/drop helpers
 * consult.  The free callback runs when OpenSSL's own SSL refcount
 * finally reaches zero, so the lock outlives every use-ref taken
 * by io_conn_ssl_acquire.
 */
struct reffs_ssl_io_lock {
	pthread_mutex_t rsil_mu;
};

static int reffs_ssl_io_lock_index = -1;

/* Stage 3 Slice 4 drain helper (definition below); forward declared
 * here because the remove_*_op paths call it before its definition. */
static void conn_drain_if_idle_locked(struct conn_info *ci);

static void ssl_io_lock_free(void *parent __attribute__((unused)), void *ptr,
			     CRYPTO_EX_DATA *ad __attribute__((unused)),
			     int idx __attribute__((unused)),
			     long argl __attribute__((unused)),
			     void *argp __attribute__((unused)))
{
	struct reffs_ssl_io_lock *lock = ptr;

	if (!lock)
		return;
	/*
	 * OpenSSL invokes this callback when the SSL's ref count reaches
	 * zero, so no other thread can still be using the lock by
	 * happens-before through the SSL ref counter.  But ThreadSanitizer
	 * does not always track the OpenSSL atomic refcount as a
	 * synchronisation primitive, and when the heap reuses this struct's
	 * memory for the next SSL's lock it cannot distinguish the new
	 * object from the old.  Re-lock and unlock the mutex here: the
	 * pthread_mutex_lock acquire forms an explicit happens-before with
	 * every prior pthread_mutex_unlock on this address (including
	 * across threads), which is the synchronisation TSAN can see.
	 * Uncontended at this point, so the cost is one cache miss.
	 */
	pthread_mutex_lock(&lock->rsil_mu);
	pthread_mutex_unlock(&lock->rsil_mu);
	pthread_mutex_destroy(&lock->rsil_mu);
	free(lock);
}

int io_conn_init(void)
{
	pthread_mutex_lock(&conn_mutex);
	memset(connections, 0, sizeof(connections));
	/*
	 * OpenSSL ex_data indices are global to the library, not the
	 * SSL_CTX; mint once per process and reuse across every
	 * io_conn_init() the test suite issues.  The free callback
	 * lives in this TU so the destroy is in lock-step with the
	 * acquisition discipline.
	 */
	if (reffs_ssl_io_lock_index < 0) {
		reffs_ssl_io_lock_index = SSL_get_ex_new_index(
			0, NULL, NULL, NULL, ssl_io_lock_free);
		if (reffs_ssl_io_lock_index < 0) {
			pthread_mutex_unlock(&conn_mutex);
			LOG("SSL_get_ex_new_index failed");
			return -1;
		}
	}
	pthread_mutex_unlock(&conn_mutex);
	return 0;
}

struct conn_info *io_conn_register(int fd, enum conn_state initial_state,
				   enum conn_role role)
{
	struct conn_info *ci = NULL;

	pthread_mutex_lock(&conn_mutex);

	int idx = fd % MAX_CONNECTIONS;
	if (connections[idx] && connections[idx]->ci_state == CONN_CLOSING) {
		/*
		 * Stage 3 Slice 4 (INV-6): a prior connection on this slot
		 * is still draining in-flight CQEs.  Refuse to reuse the
		 * slot now -- if we did, a stale completion for the old
		 * connection would land on the new one and corrupt state.
		 * The accept path is expected to retry; if no retry exists
		 * the kernel's next accept() will give us the same fd and
		 * we'll succeed once the drain completes.
		 */
		pthread_mutex_unlock(&conn_mutex);
		return NULL;
	}
	if (connections[idx] && connections[idx]->ci_fd == fd) {
		ci = connections[idx];
	} else if (!connections[idx] ||
		   connections[idx]->ci_state == CONN_UNUSED) {
		uint32_t saved_gen =
			connections[idx] ? connections[idx]->ci_generation : 0;
		if (!connections[idx]) {
			connections[idx] = malloc(sizeof(struct conn_info));
			if (!connections[idx]) {
				pthread_mutex_unlock(&conn_mutex);
				return NULL;
			}
		}

		ci = connections[idx];
		memset(ci, 0, sizeof(struct conn_info));
		ci->ci_generation = saved_gen + 1;
		ci->ci_fd = fd;
	} else {
		LOG("Connection slot collision for fd=%d", fd);
	}

	if (ci) {
		ci->ci_state = initial_state;
		ci->ci_role = role;
		ci->ci_last_activity = time(NULL);

		ci->ci_read_count = 0;
		ci->ci_write_count = 0;
		ci->ci_accept_count = 0;
		ci->ci_connect_count = 0;

		ci->ci_write_active = false;
		ci->ci_write_pending_head = NULL;
		ci->ci_write_pending_tail = NULL;
	}

	pthread_mutex_unlock(&conn_mutex);
	return ci;
}

struct conn_info *io_conn_get(int fd)
{
	struct conn_info *ci = NULL;
	pthread_mutex_lock(&conn_mutex);

	int idx = fd % MAX_CONNECTIONS;
	/*
	 * Stage 3 Slice 4 (INV-6): a slot in CONN_CLOSING is unregistered
	 * from the caller's point of view; only in-flight count
	 * bookkeeping (io_conn_remove_*_op / io_conn_write_done) is allowed
	 * to touch it, and those use the internal *_locked helpers below.
	 */
	if (connections[idx] && connections[idx]->ci_fd == fd &&
	    connections[idx]->ci_state != CONN_CLOSING) {
		ci = connections[idx];
	}

	pthread_mutex_unlock(&conn_mutex);
	return ci;
}

struct conn_info *io_listener_register(int fd, uint32_t listener_id)
{
	struct conn_info *ci =
		io_conn_register(fd, CONN_LISTENING, CONN_ROLE_SERVER);
	if (!ci)
		return NULL;

	pthread_mutex_lock(&conn_mutex);
	ci->ci_listener_id = listener_id;
	pthread_mutex_unlock(&conn_mutex);
	return ci;
}

uint32_t io_conn_listener_id(int fd)
{
	uint32_t id = 0;

	pthread_mutex_lock(&conn_mutex);
	int idx = fd % MAX_CONNECTIONS;
	if (connections[idx] && connections[idx]->ci_fd == fd)
		id = connections[idx]->ci_listener_id;
	pthread_mutex_unlock(&conn_mutex);
	return id;
}

void io_conn_set_listener_id(int fd, uint32_t listener_id)
{
	pthread_mutex_lock(&conn_mutex);
	int idx = fd % MAX_CONNECTIONS;
	if (connections[idx] && connections[idx]->ci_fd == fd)
		connections[idx]->ci_listener_id = listener_id;
	pthread_mutex_unlock(&conn_mutex);
}

void io_conn_update_state(int fd)
{
	pthread_mutex_lock(&conn_mutex);

	int idx = fd % MAX_CONNECTIONS;
	if (connections[idx] && connections[idx]->ci_fd == fd) {
		enum conn_state old_state = connections[idx]->ci_state;
		enum conn_state new_state;

		if (connections[idx]->ci_read_count > 0) {
			if (connections[idx]->ci_write_count > 0) {
				new_state = CONN_READWRITE;
			} else {
				new_state = CONN_READING;
			}
		} else if (connections[idx]->ci_write_count > 0) {
			new_state = CONN_WRITING;
		} else if (connections[idx]->ci_accept_count > 0) {
			new_state = CONN_ACCEPTING;
		} else if (connections[idx]->ci_connect_count > 0) {
			new_state = CONN_CONNECTING;
		} else if (old_state == CONN_ERROR ||
			   old_state == CONN_DISCONNECTING ||
			   old_state == CONN_UNUSED) {
			new_state = old_state;
		} else {
			new_state = CONN_CONNECTED;
		}

		if (old_state != new_state) {
			trace_io_connection_state_change(
				fd, old_state, new_state, __func__, __LINE__);
			connections[idx]->ci_state = new_state;
		}
	}

	pthread_mutex_unlock(&conn_mutex);
}

int io_conn_add_read_op(int fd)
{
	int ret = -1;
	pthread_mutex_lock(&conn_mutex);

	int idx = fd % MAX_CONNECTIONS;
	/*
	 * Stage 3 Slice 4 (INV-6): a late add_read_op racing
	 * io_conn_unregister must not resurrect a CLOSING slot out of
	 * its drain.  Caller gating via io_conn_get / tls_snapshot
	 * catches the typical case but the check/add pair is not
	 * atomic, so the predicate here is the authoritative gate.
	 */
	if (connections[idx] && connections[idx]->ci_fd == fd &&
	    connections[idx]->ci_state != CONN_CLOSING) {
		connections[idx]->ci_read_count++;
		connections[idx]->ci_last_activity = time(NULL);

		enum conn_state old_state = connections[idx]->ci_state;
		enum conn_state new_state;

		if (connections[idx]->ci_write_count > 0) {
			new_state = CONN_READWRITE;
		} else {
			new_state = CONN_READING;
		}

		if (old_state != new_state) {
			trace_io_connection_state_change(
				fd, old_state, new_state, __func__, __LINE__);
			connections[idx]->ci_state = new_state;
		}

		ret = 0;
	}

	pthread_mutex_unlock(&conn_mutex);
	return ret;
}

int io_conn_remove_read_op(int fd)
{
	int ret = -1;
	pthread_mutex_lock(&conn_mutex);

	int idx = fd % MAX_CONNECTIONS;
	if (connections[idx] && connections[idx]->ci_fd == fd) {
		if (connections[idx]->ci_read_count > 0) {
			connections[idx]->ci_read_count--;
			trace_io_connection_count(
				fd, connections[idx]->ci_read_count, __func__,
				__LINE__);

			/*
			 * Skip the state-machine transitions for CONN_CLOSING
			 * (Stage 3 Slice 4) -- the slot is draining toward
			 * CONN_UNUSED, not into a live state.  Decrement the
			 * counter and let conn_drain_if_idle_locked complete
			 * the transition.
			 */
			if (connections[idx]->ci_state != CONN_CLOSING) {
				enum conn_state old_state =
					connections[idx]->ci_state;
				enum conn_state new_state;

				if (connections[idx]->ci_read_count > 0) {
					if (connections[idx]->ci_write_count >
					    0) {
						new_state = CONN_READWRITE;
					} else {
						new_state = CONN_READING;
					}
				} else if (connections[idx]->ci_write_count >
					   0) {
					new_state = CONN_WRITING;
				} else {
					new_state = CONN_CONNECTED;
				}

				if (old_state != new_state) {
					trace_io_connection_state_change(
						fd, old_state, new_state,
						__func__, __LINE__);
					connections[idx]->ci_state = new_state;
				}
			}
		} else {
			LOG("Warning: Attempt to remove read op when count is zero for fd=%d",
			    fd);
		}
		connections[idx]->ci_last_activity = time(NULL);
		conn_drain_if_idle_locked(connections[idx]);
		ret = 0;
	}

	pthread_mutex_unlock(&conn_mutex);
	return ret;
}

int io_conn_add_write_op(int fd)
{
	int ret = -1;
	pthread_mutex_lock(&conn_mutex);

	int idx = fd % MAX_CONNECTIONS;
	/* See add_read_op above for the CLOSING gate rationale. */
	if (connections[idx] && connections[idx]->ci_fd == fd &&
	    connections[idx]->ci_state != CONN_CLOSING) {
		connections[idx]->ci_write_count++;
		connections[idx]->ci_last_activity = time(NULL);
		trace_io_connection_count(fd, connections[idx]->ci_write_count,
					  __func__, __LINE__);

		enum conn_state old_state = connections[idx]->ci_state;
		enum conn_state new_state;

		if (connections[idx]->ci_read_count > 0) {
			new_state = CONN_READWRITE;
		} else {
			new_state = CONN_WRITING;
		}

		if (old_state != new_state) {
			trace_io_connection_state_change(
				fd, old_state, new_state, __func__, __LINE__);
			connections[idx]->ci_state = new_state;
		}

		ret = 0;
	}

	pthread_mutex_unlock(&conn_mutex);
	return ret;
}

int io_conn_remove_write_op(int fd)
{
	int ret = -1;
	pthread_mutex_lock(&conn_mutex);

	int idx = fd % MAX_CONNECTIONS;
	if (connections[idx] && connections[idx]->ci_fd == fd) {
		if (connections[idx]->ci_write_count > 0) {
			connections[idx]->ci_write_count--;

			trace_io_connection_count(
				fd, connections[idx]->ci_write_count, __func__,
				__LINE__);

			/* Skip state transitions for draining slots; see
			 * matching comment in io_conn_remove_read_op. */
			if (connections[idx]->ci_state != CONN_CLOSING) {
				enum conn_state old_state =
					connections[idx]->ci_state;
				enum conn_state new_state;

				if (connections[idx]->ci_write_count > 0) {
					if (connections[idx]->ci_read_count >
					    0) {
						new_state = CONN_READWRITE;
					} else {
						new_state = CONN_WRITING;
					}
				} else if (connections[idx]->ci_read_count >
					   0) {
					new_state = CONN_READING;
				} else {
					new_state = CONN_CONNECTED;
				}

				if (old_state != new_state) {
					trace_io_connection_state_change(
						fd, old_state, new_state,
						__func__, __LINE__);
					connections[idx]->ci_state = new_state;
				}
			}
		} else {
			LOG("Warning: Attempt to remove write op when count is zero for fd=%d",
			    fd);
		}
		connections[idx]->ci_last_activity = time(NULL);
		conn_drain_if_idle_locked(connections[idx]);
		ret = 0;
	}

	pthread_mutex_unlock(&conn_mutex);
	return ret;
}

int io_conn_add_accept_op(int fd)
{
	int ret = -1;
	pthread_mutex_lock(&conn_mutex);

	int idx = fd % MAX_CONNECTIONS;
	/* See add_read_op above for the CLOSING gate rationale. */
	if (connections[idx] && connections[idx]->ci_fd == fd &&
	    connections[idx]->ci_state != CONN_CLOSING) {
		connections[idx]->ci_accept_count++;
		connections[idx]->ci_last_activity = time(NULL);
		trace_io_connection_count(fd, connections[idx]->ci_accept_count,
					  __func__, __LINE__);

		if (connections[idx]->ci_state != CONN_ACCEPTING) {
			trace_io_connection_state_change(
				fd, connections[idx]->ci_state, CONN_ACCEPTING,
				__func__, __LINE__);
			connections[idx]->ci_state = CONN_ACCEPTING;
		}

		ret = 0;
	}

	pthread_mutex_unlock(&conn_mutex);
	return ret;
}

int io_conn_remove_accept_op(int fd)
{
	int ret = -1;
	pthread_mutex_lock(&conn_mutex);

	int idx = fd % MAX_CONNECTIONS;
	if (connections[idx] && connections[idx]->ci_fd == fd) {
		if (connections[idx]->ci_accept_count > 0) {
			connections[idx]->ci_accept_count--;
			trace_io_connection_count(
				fd, connections[idx]->ci_accept_count, __func__,
				__LINE__);

			if (connections[idx]->ci_accept_count == 0 &&
			    connections[idx]->ci_state == CONN_ACCEPTING) {
				trace_io_connection_state_change(
					fd, CONN_ACCEPTING, CONN_LISTENING,
					__func__, __LINE__);
				connections[idx]->ci_state = CONN_LISTENING;
			}
		} else {
			LOG("Warning: Attempt to remove accept op when count is zero for fd=%d",
			    fd);
		}
		connections[idx]->ci_last_activity = time(NULL);
		conn_drain_if_idle_locked(connections[idx]);
		ret = 0;
	}

	pthread_mutex_unlock(&conn_mutex);
	return ret;
}

int io_conn_add_connect_op(int fd)
{
	int ret = -1;
	pthread_mutex_lock(&conn_mutex);

	int idx = fd % MAX_CONNECTIONS;
	/* See add_read_op above for the CLOSING gate rationale. */
	if (connections[idx] && connections[idx]->ci_fd == fd &&
	    connections[idx]->ci_state != CONN_CLOSING) {
		connections[idx]->ci_connect_count++;
		connections[idx]->ci_last_activity = time(NULL);
		trace_io_connection_count(fd,
					  connections[idx]->ci_connect_count,
					  __func__, __LINE__);

		if (connections[idx]->ci_state != CONN_CONNECTING) {
			trace_io_connection_state_change(
				fd, connections[idx]->ci_state, CONN_CONNECTING,
				__func__, __LINE__);
			connections[idx]->ci_state = CONN_CONNECTING;
		}

		ret = 0;
	}

	pthread_mutex_unlock(&conn_mutex);
	return ret;
}

int io_conn_remove_connect_op(int fd)
{
	int ret = -1;

	pthread_mutex_lock(&conn_mutex);

	int idx = fd % MAX_CONNECTIONS;
	if (connections[idx] && connections[idx]->ci_fd == fd) {
		if (connections[idx]->ci_connect_count > 0) {
			connections[idx]->ci_connect_count--;
			trace_io_connection_count(
				fd, connections[idx]->ci_connect_count,
				__func__, __LINE__);

			if (connections[idx]->ci_connect_count == 0 &&
			    connections[idx]->ci_state == CONN_CONNECTING) {
				trace_io_connection_state_change(
					fd, CONN_CONNECTING, CONN_CONNECTED,
					__func__, __LINE__);
				connections[idx]->ci_state = CONN_CONNECTED;
			}
		} else {
			LOG("Warning: Attempt to remove connect op when count is zero for fd=%d",
			    fd);
		}
		connections[idx]->ci_last_activity = time(NULL);
		conn_drain_if_idle_locked(connections[idx]);
		ret = 0;
	}

	pthread_mutex_unlock(&conn_mutex);
	return ret;
}

int io_conn_set_error(int fd, int error_code)
{
	int ret = -1;
	pthread_mutex_lock(&conn_mutex);

	int idx = fd % MAX_CONNECTIONS;
	if (connections[idx] && connections[idx]->ci_fd == fd) {
		trace_io_connection_state_change(fd, connections[idx]->ci_state,
						 CONN_ERROR, __func__,
						 __LINE__);

		connections[idx]->ci_state = CONN_ERROR;
		connections[idx]->ci_error = error_code;
		connections[idx]->ci_last_activity = time(NULL);
		ret = 0;
	}

	pthread_mutex_unlock(&conn_mutex);
	return ret;
}

bool io_conn_is_state(int fd, enum conn_state state)
{
	bool result = false;
	pthread_mutex_lock(&conn_mutex);

	int idx = fd % MAX_CONNECTIONS;
	if (connections[idx] && connections[idx]->ci_fd == fd) {
		result = (connections[idx]->ci_state == state);
	}

	pthread_mutex_unlock(&conn_mutex);
	return result;
}

/*
 * conn_ssl_detach_locked -- detach the SSL object and clear the TLS
 * flags on a slot, with conn_mutex already held by the caller.
 *
 * Returns the detached SSL (the caller MUST drop the slot's ref via
 * conn_ssl_drop() *after* releasing conn_mutex -- SSL_shutdown /
 * SSL_free must never run under the process-wide conn_mutex), or NULL
 * if the slot had no SSL.  Idempotent: a second call returns NULL.
 */
static SSL *conn_ssl_detach_locked(struct conn_info *ci)
{
	SSL *ssl = ci->ci_ssl;

	ci->ci_ssl = NULL;
	ci->ci_tls_enabled = false;
	ci->ci_tls_handshaking = false;
	return ssl;
}

/*
 * conn_ssl_drop -- drop the slot's ref on an SSL object.  SSL objects
 * carry their own atomic refcount; SSL_free decrements it and frees at
 * zero, so an outstanding use-ref (io_conn_ssl_acquire) keeps the
 * object alive past this call.  SSL_shutdown queues the close_notify
 * the same way the pre-refcount teardown did.  NULL-tolerant.  Must be
 * called with conn_mutex NOT held.
 *
 * SSL_shutdown writes the close_notify record into libssl state, so
 * it MUST NOT run concurrent with a worker SSL_write or the event
 * loop SSL_read on the same SSL (Stage 3 Slice 3).  Take the per-SSL
 * io_lock first: outstanding use-ref holders that have entered an
 * SSL_ or BIO_ call hold the lock; we wait for them to finish before
 * issuing the shutdown.
 */
static void conn_ssl_drop(SSL *ssl)
{
	if (!ssl)
		return;
	io_conn_ssl_io_lock(ssl);
	SSL_shutdown(ssl);
	io_conn_ssl_io_unlock(ssl);
	SSL_free(ssl);
}

void io_conn_destroy(struct conn_info *ci)
{
	if (!ci)
		return;

	pthread_mutex_lock(&conn_mutex);
	SSL *ssl = conn_ssl_detach_locked(ci);
	pthread_mutex_unlock(&conn_mutex);

	conn_ssl_drop(ssl);
	free(ci);
}

/*
 * conn_drain_if_idle_locked -- if a slot in CONN_CLOSING has drained
 * (no in-flight read/write/accept/connect ops, write gate idle),
 * complete its transition to CONN_UNUSED so it can be reused.  Stage
 * 3 Slice 4 (INV-6) -- complements io_conn_unregister, which now
 * leaves the slot in CONN_CLOSING so stale CQEs land on the correct
 * conn_info rather than corrupting whatever connection has reused
 * the fd in the meantime.  Caller holds conn_mutex.
 */
static void conn_drain_if_idle_locked(struct conn_info *ci)
{
	if (ci->ci_state != CONN_CLOSING)
		return;
	if (ci->ci_read_count || ci->ci_write_count || ci->ci_accept_count ||
	    ci->ci_connect_count)
		return;
	if (ci->ci_write_active)
		return;
	ci->ci_state = CONN_UNUSED;
	ci->ci_fd = -1;
}

int io_conn_unregister(int fd)
{
	int ret = -1;
	struct io_context *drain_head = NULL;
	SSL *dead_ssl = NULL;

	pthread_mutex_lock(&conn_mutex);

	int idx = fd % MAX_CONNECTIONS;
	if (connections[idx] && connections[idx]->ci_fd == fd) {
		TRACE("Unregistering connection fd=%d (state=%s, role=%s)", fd,
		      io_conn_state_to_str(connections[idx]->ci_state),
		      io_conn_role_to_str(connections[idx]->ci_role));

		/*
		 * Detach the SSL under conn_mutex; SSL_shutdown / SSL_free
		 * happen below, after the lock is released.
		 */
		dead_ssl = conn_ssl_detach_locked(connections[idx]);

		/*
		 * Drain the pending write queue before transitioning state.
		 * Extract under conn_mutex but call io_context_destroy()
		 * outside it (destroy takes conn_mutex internally --
		 * sequential, not nested -- safe).
		 */
		drain_head = connections[idx]->ci_write_pending_head;
		connections[idx]->ci_write_pending_head = NULL;
		connections[idx]->ci_write_pending_tail = NULL;
		connections[idx]->ci_write_active = false;

		/*
		 * Stage 3 Slice 4 (INV-6): mark the slot CONN_CLOSING but
		 * keep ci_fd and the in-flight counters.  Stale CQEs
		 * landing on this fd will still find this slot (rather
		 * than a freshly-reused one) and naturally decrement the
		 * counters via the *_remove_op / io_conn_write_done paths,
		 * which call conn_drain_if_idle_locked to transition to
		 * CONN_UNUSED once everything quiesces.  io_conn_register
		 * refuses to reuse a CONN_CLOSING slot, so a new accept
		 * on the same fd number waits for the old conn to drain.
		 */
		connections[idx]->ci_state = CONN_CLOSING;
		conn_drain_if_idle_locked(connections[idx]);
		ret = 0;
	} else {
		LOG("Failed to unregister connection fd=%d - not found or mismatch",
		    fd);
	}

	pthread_mutex_unlock(&conn_mutex);

	conn_ssl_drop(dead_ssl);

	while (drain_head) {
		struct io_context *next = drain_head->ic_write_next;
		drain_head->ic_write_next = NULL;
		io_context_destroy(drain_head);
		drain_head = next;
	}

	return ret;
}

bool io_conn_has_read_ops(int fd)
{
	bool has_ops = false;
	pthread_mutex_lock(&conn_mutex);

	int idx = fd % MAX_CONNECTIONS;
	if (connections[idx] && connections[idx]->ci_fd == fd) {
		has_ops = (connections[idx]->ci_read_count > 0);
	}

	pthread_mutex_unlock(&conn_mutex);
	return has_ops;
}

bool io_conn_has_write_ops(int fd)
{
	bool has_ops = false;
	pthread_mutex_lock(&conn_mutex);

	int idx = fd % MAX_CONNECTIONS;
	if (connections[idx] && connections[idx]->ci_fd == fd) {
		has_ops = (connections[idx]->ci_write_count > 0);
	}

	pthread_mutex_unlock(&conn_mutex);
	return has_ops;
}

void io_conn_cleanup(void)
{
	pthread_mutex_lock(&conn_mutex);

	for (int i = 0; i < MAX_CONNECTIONS; i++) {
		if (connections[i]) {
			/*
			 * Detach then drop the SSL.  conn_ssl_drop() runs
			 * under conn_mutex here, which io_conn_ssl_clear()
			 * deliberately avoids -- acceptable only because
			 * io_conn_cleanup() runs single-threaded at process
			 * shutdown, so there is no other connection's
			 * bookkeeping for the held lock to stall.
			 */
			SSL *ssl = conn_ssl_detach_locked(connections[i]);
			conn_ssl_drop(ssl);

			free(connections[i]);
			connections[i] = NULL;
		}
	}

	pthread_mutex_unlock(&conn_mutex);
}

int io_socket_close(int fd, int error)
{
	if (fd <= 0) {
		return -EBADF;
	}

	struct conn_info *conn = io_conn_get(fd);
	if (conn) {
		io_conn_set_error(fd, error);
		io_conn_unregister(fd);
	}

	TRACE("Closing %d", fd);

	io_client_fd_unregister(fd);
	return close(fd);
}

const char *io_conn_state_to_str(enum conn_state state)
{
	switch (state) {
	case CONN_UNUSED:
		return "UNUSED";
	case CONN_LISTENING:
		return "LISTENING";
	case CONN_ACCEPTING:
		return "ACCEPTING";
	case CONN_ACCEPTED:
		return "ACCEPTED";
	case CONN_CONNECTING:
		return "CONNECTING";
	case CONN_CONNECTED:
		return "CONNECTED";
	case CONN_READING:
		return "READING";
	case CONN_WRITING:
		return "WRITING";
	case CONN_READWRITE:
		return "READWRITE";
	case CONN_DISCONNECTING:
		return "DISCONNECTING";
	case CONN_ERROR:
		return "ERROR";
	case CONN_CLOSING:
		return "CLOSING";
	default:
		return "UNKNOWN";
	}
}

const char *io_conn_role_to_str(enum conn_role role)
{
	switch (role) {
	case CONN_ROLE_UNKNOWN:
		return "UNKNOWN";
	case CONN_ROLE_CLIENT:
		return "CLIENT";
	case CONN_ROLE_SERVER:
		return "SERVER";
	case CONN_ROLE_ACCEPTED:
		return "ACCEPTED";
	default:
		return "INVALID";
	}
}

void io_conn_dump(int fd)
{
	struct conn_info *ci = io_conn_get(fd);
	if (!ci) {
		LOG("No connection info for fd=%d", fd);
		return;
	}

	char peer_addr[INET6_ADDRSTRLEN] = { 0 };
	char local_addr[INET6_ADDRSTRLEN] = { 0 };
	uint16_t peer_port = 0, local_port = 0;
	time_t now = time(NULL);

	if (ci->ci_peer_len > 0) {
		addr_to_string((const struct sockaddr_storage *)&ci->ci_peer,
			       peer_addr, INET6_ADDRSTRLEN, &peer_port);
	}

	if (ci->ci_local_len > 0) {
		addr_to_string((const struct sockaddr_storage *)&ci->ci_local,
			       local_addr, INET6_ADDRSTRLEN, &local_port);
	}

	trace_io_active_connections(ci, peer_addr, peer_port, local_addr,
				    local_port, now, __func__, __LINE__);
}

void io_conn_dump_all(void)
{
	pthread_mutex_lock(&conn_mutex);

	TRACE("=== Active Connections ===");
	int active_count = 0;

	time_t now = time(NULL);

	for (int i = 0; i < MAX_CONNECTIONS; i++) {
		if (connections[i] && connections[i]->ci_state != CONN_UNUSED) {
			active_count++;
			struct conn_info *ci = connections[i];

			char peer_addr[INET6_ADDRSTRLEN] = { 0 };
			char local_addr[INET6_ADDRSTRLEN] = { 0 };
			uint16_t peer_port = 0, local_port = 0;

			if (ci->ci_peer_len > 0) {
				addr_to_string((const struct sockaddr_storage
							*)&ci->ci_peer,
					       peer_addr, INET6_ADDRSTRLEN,
					       &peer_port);
			}

			if (ci->ci_local_len > 0) {
				addr_to_string((const struct sockaddr_storage
							*)&ci->ci_local,
					       local_addr, INET6_ADDRSTRLEN,
					       &local_port);
			}

			trace_io_active_connections(ci, peer_addr, peer_port,
						    local_addr, local_port, now,
						    __func__, __LINE__);
		}
	}

	TRACE("Total active connections: %d", active_count);
	TRACE("==========================");

	pthread_mutex_unlock(&conn_mutex);
}

int io_conn_check_timeouts(time_t idle_timeout_seconds,
			   time_t closing_timeout_seconds)
{
	time_t now = time(NULL);
	/*
	 * Collect timed-out fds under conn_mutex, then release and close
	 * each via io_socket_close() outside the mutex.  Two reasons:
	 *
	 *  1. close(2) under conn_mutex can block indefinitely under
	 *     memory pressure or in the middle of kernel socket teardown;
	 *     every other conn_info lookup in the process stalls while we
	 *     wait.  Release the mutex first.
	 *
	 *  2. The original code used raw close(fd) + manual ci_state flip,
	 *     which left conn_buffers[fd % MAX_CONNECTIONS] populated --
	 *     a buffer_state leak.  io_socket_close() calls
	 *     io_client_fd_unregister() which frees the buffer_state
	 *     properly.
	 *
	 * Race note: between the scan and the close, another thread
	 * could accept a new connection whose fd hashes to the same
	 * conn_info slot.  For timed-out-idle connections this is
	 * vanishingly unlikely (no completions fire on a truly idle fd),
	 * and the worst case is closing a newly-accepted client which
	 * will reconnect.  Slice 4 added the CONN_CLOSING state for the
	 * other half of this hazard (slot reuse mid-drain); this sweep
	 * does not close CLOSING slots -- their fd was already closed at
	 * io_socket_close time and re-closing it here would risk
	 * double-close on a descriptor the OS may have handed to a new
	 * caller.  Stuck-CLOSING slots (drain never completes) are
	 * force-drained in a separate pass below, with a logged warning
	 * but no socket close.
	 *
	 * Stack array sized at MAX_CONNECTIONS is fine: ~4 KB on the
	 * heartbeat thread, which does not recurse.
	 */
	int to_close[MAX_CONNECTIONS];
	int n_to_close = 0;
	int n_force_drained = 0;

	pthread_mutex_lock(&conn_mutex);

	for (int i = 0; i < MAX_CONNECTIONS; i++) {
		if (!connections[i] ||
		    connections[i]->ci_state == CONN_UNUSED ||
		    connections[i]->ci_fd < 0)
			continue;

		time_t idle = now - connections[i]->ci_last_activity;

		/*
		 * CONN_CLOSING slots get their own, much shorter deadline
		 * (closing_timeout_seconds): a healthy drain finishes in
		 * milliseconds, so a slot still CLOSING seconds later is
		 * wedged -- a CQE incremented a count but the matching
		 * decrement never ran (io_uring cancellation, a worker
		 * giving up without calling remove_*_op, etc.).  The fd
		 * was already closed at the original io_conn_unregister;
		 * force the slot to UNUSED here so it can be reused, but
		 * do not call io_socket_close again.
		 */
		if (connections[i]->ci_state == CONN_CLOSING) {
			if (idle <= closing_timeout_seconds)
				continue;
			LOG("Connection fd=%d stuck in CLOSING for %ld seconds (counts: r=%d w=%d a=%d c=%d, write_active=%d); force-draining",
			    connections[i]->ci_fd, (long)idle,
			    connections[i]->ci_read_count,
			    connections[i]->ci_write_count,
			    connections[i]->ci_accept_count,
			    connections[i]->ci_connect_count,
			    connections[i]->ci_write_active);
			connections[i]->ci_read_count = 0;
			connections[i]->ci_write_count = 0;
			connections[i]->ci_accept_count = 0;
			connections[i]->ci_connect_count = 0;
			connections[i]->ci_write_active = false;
			connections[i]->ci_state = CONN_UNUSED;
			connections[i]->ci_fd = -1;
			n_force_drained++;
			continue;
		}

		/* Live connection: close it if idle past the idle deadline. */
		if (idle <= idle_timeout_seconds)
			continue;
		LOG("Connection fd=%d timed out (%ld seconds inactive)",
		    connections[i]->ci_fd, (long)idle);
		to_close[n_to_close++] = connections[i]->ci_fd;
	}

	pthread_mutex_unlock(&conn_mutex);

	for (int i = 0; i < n_to_close; i++)
		io_socket_close(to_close[i], ETIMEDOUT);

	return n_to_close + n_force_drained;
}

/*
 * io_conn_write_try_start -- claim the per-fd write serialization gate.
 *
 * If the gate is free (ci_write_active == false), set it true and return
 * true so the caller can submit a write SQE immediately.
 *
 * If the gate is held, append ic to ci_write_pending and return false.
 * The caller must return without touching ic further; io_conn_write_done()
 * will hand off the gate and call rpc_trans_writer() for the next ic.
 *
 * If fd is no longer registered we return true anyway -- the caller will
 * fail naturally on SQE submission.
 */
bool io_conn_write_try_start(int fd, struct io_context *ic)
{
	bool can_start = true;

	pthread_mutex_lock(&conn_mutex);

	int idx = fd % MAX_CONNECTIONS;
	/* Stage 3 Slice 4: do not start new writes on a draining slot. */
	if (connections[idx] && connections[idx]->ci_fd == fd &&
	    connections[idx]->ci_state != CONN_CLOSING) {
		struct conn_info *ci = connections[idx];
		if (!ci->ci_write_active) {
			ci->ci_write_active = true;
			ic->ic_write_gen = ci->ci_generation;
		} else {
			ic->ic_write_next = NULL;
			if (!ci->ci_write_pending_head) {
				ci->ci_write_pending_head = ic;
				ci->ci_write_pending_tail = ic;
			} else {
				ci->ci_write_pending_tail->ic_write_next = ic;
				ci->ci_write_pending_tail = ic;
			}
			can_start = false;
		}
	}

	pthread_mutex_unlock(&conn_mutex);
	return can_start;
}

/*
 * io_conn_write_done -- release the per-fd write gate after a write completes.
 *
 * gen must match ci->ci_generation.  A mismatch means a stale write CQE
 * arrived for a connection that has already been closed and whose fd was
 * reused by a new connection.  In that case the gate belongs to the new
 * connection and must not be touched; return NULL silently.
 *
 * If there are queued writers, dequeue the head, mark it WRITE_OWNED,
 * stamp its ic_write_gen, and return it.  ci_write_active remains true.
 *
 * If the queue is empty, clear ci_write_active and return NULL.
 */
struct io_context *io_conn_write_done(int fd, uint32_t gen)
{
	struct io_context *next_ic = NULL;

	pthread_mutex_lock(&conn_mutex);

	int idx = fd % MAX_CONNECTIONS;
	if (connections[idx] && connections[idx]->ci_fd == fd &&
	    connections[idx]->ci_generation == gen) {
		struct conn_info *ci = connections[idx];
		if (ci->ci_write_pending_head) {
			next_ic = ci->ci_write_pending_head;
			ci->ci_write_pending_head = next_ic->ic_write_next;
			next_ic->ic_write_next = NULL;
			if (!ci->ci_write_pending_head)
				ci->ci_write_pending_tail = NULL;
			next_ic->ic_state |= IO_CONTEXT_WRITE_OWNED;
			next_ic->ic_write_gen = ci->ci_generation;
		} else {
			ci->ci_write_active = false;
		}
		/*
		 * Stage 3 Slice 4 (INV-6): the write gate just emptied
		 * may have been the last in-flight op on a CLOSING slot.
		 */
		conn_drain_if_idle_locked(ci);
	}

	pthread_mutex_unlock(&conn_mutex);
	return next_ic;
}

bool io_conn_is_tls_enabled(int fd)
{
	bool enabled = false;

	pthread_mutex_lock(&conn_mutex);
	int idx = fd % MAX_CONNECTIONS;
	if (connections[idx] && connections[idx]->ci_fd == fd)
		enabled = connections[idx]->ci_tls_enabled;
	pthread_mutex_unlock(&conn_mutex);

	return enabled;
}

int io_conn_get_peer_cert_fingerprint(int fd, char *out_buf, size_t out_buf_len)
{
	if (!out_buf || out_buf_len == 0)
		return -EINVAL;
	/* SHA-256 colon-hex: 32 bytes * 2 hex chars + 31 colons + NUL = 96 */
	if (out_buf_len < 96)
		return -ENOSPC;

	out_buf[0] = '\0';

	X509 *cert = NULL;

	/*
	 * Slice plan-A.ii: pull the X509 reference INSIDE the locked
	 * window.  SSL_get_peer_certificate ref-bumps the X509 (we
	 * X509_free below to balance), but SSL itself is NOT
	 * refcounted by that call -- pulling a raw SSL pointer out of
	 * the conn table and using it after unlock would race with a
	 * concurrent SSL_free in io_conn_unregister / handlers.c
	 * teardown paths (UAF).  So read-and-extract under the lock,
	 * drop the lock, then operate on the (independently
	 * refcounted) X509 alone.
	 */
	pthread_mutex_lock(&conn_mutex);
	int idx = fd % MAX_CONNECTIONS;

	if (connections[idx] && connections[idx]->ci_fd == fd &&
	    connections[idx]->ci_tls_enabled && connections[idx]->ci_ssl)
		cert = SSL_get_peer_certificate(connections[idx]->ci_ssl);
	pthread_mutex_unlock(&conn_mutex);

	if (!cert)
		return -ENOENT;

	/* Serialize cert to DER, hash, hex-format. */
	int der_len = i2d_X509(cert, NULL);
	int ret = 0;

	if (der_len <= 0) {
		ret = -ENOENT;
		goto out;
	}

	unsigned char *der = malloc(der_len);

	if (!der) {
		ret = -ENOMEM;
		goto out;
	}
	unsigned char *p = der;

	if (i2d_X509(cert, &p) <= 0) {
		free(der);
		ret = -ENOENT;
		goto out;
	}

	unsigned char hash[SHA256_DIGEST_LENGTH];

	SHA256(der, der_len, hash);
	free(der);

	/* Format as colon-separated uppercase hex. */
	for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
		snprintf(out_buf + i * 3, 4, "%02X%s", hash[i],
			 (i == SHA256_DIGEST_LENGTH - 1) ? "" : ":");
	}

out:
	X509_free(cert);
	return ret;
}

void io_conn_set_tls_handshaking(int fd, bool handshaking)
{
	pthread_mutex_lock(&conn_mutex);
	int idx = fd % MAX_CONNECTIONS;
	if (connections[idx] && connections[idx]->ci_fd == fd)
		connections[idx]->ci_tls_handshaking = handshaking;
	pthread_mutex_unlock(&conn_mutex);
}

/*
 * TLS SSL-object lifecycle (INV-5 / INV-6 fix).  See reffs/io.h for
 * the contract.  The SSL object's own atomic refcount is the lifecycle
 * counter: the slot holds one ref, each io_conn_ssl_acquire() adds a
 * use-ref, and the object frees when the last ref is dropped.
 */
void io_conn_ssl_install(int fd, SSL *ssl)
{
	SSL *orphan = NULL;

	/*
	 * Attach the per-SSL io_lock now, while we still own the +1 ref
	 * SSL_new() handed us and before publishing into the slot.  If
	 * the allocation or attach fails we still install the SSL --
	 * io_conn_ssl_io_lock/_unlock are NULL-tolerant -- and rely on
	 * the in-flight write-gate to bound contention; logged so an
	 * operator can spot persistent allocation pressure.
	 */
	if (ssl && reffs_ssl_io_lock_index >= 0 &&
	    !SSL_get_ex_data(ssl, reffs_ssl_io_lock_index)) {
		struct reffs_ssl_io_lock *lock = calloc(1, sizeof(*lock));
		if (lock) {
			pthread_mutex_init(&lock->rsil_mu, NULL);
			if (SSL_set_ex_data(ssl, reffs_ssl_io_lock_index,
					    lock) == 0) {
				pthread_mutex_destroy(&lock->rsil_mu);
				free(lock);
				LOG("SSL_set_ex_data failed; SSL on fd=%d will run unserialised",
				    fd);
			}
		} else {
			LOG("OOM allocating reffs_ssl_io_lock for fd=%d", fd);
		}
	}

	pthread_mutex_lock(&conn_mutex);
	int idx = fd % MAX_CONNECTIONS;
	if (connections[idx] && connections[idx]->ci_fd == fd) {
		struct conn_info *ci = connections[idx];
		/*
		 * A fresh handshake should never find an SSL already
		 * installed; detach any stale one defensively so it
		 * cannot leak.
		 */
		orphan = ci->ci_ssl;
		ci->ci_ssl = ssl;
		ci->ci_tls_handshaking = true;
		ci->ci_tls_enabled = false;
		ci->ci_handshake_final_pending = false;
	} else {
		/*
		 * fd vanished between SSL_new() and install -- the +1 we
		 * were handed has no slot to live in.
		 */
		orphan = ssl;
	}
	pthread_mutex_unlock(&conn_mutex);

	conn_ssl_drop(orphan);
}

SSL *io_conn_ssl_acquire(int fd)
{
	SSL *ssl = NULL;

	pthread_mutex_lock(&conn_mutex);
	int idx = fd % MAX_CONNECTIONS;
	/*
	 * Stage 3 Slice 4: ci_ssl is detached in io_conn_unregister
	 * before the state moves to CONN_CLOSING, so the ssl pointer
	 * is already NULL here in practice; the explicit state check
	 * is defence in depth in case a future caller re-installs an
	 * SSL on a draining slot.
	 */
	if (connections[idx] && connections[idx]->ci_fd == fd &&
	    connections[idx]->ci_state != CONN_CLOSING &&
	    connections[idx]->ci_ssl) {
		ssl = connections[idx]->ci_ssl;
		SSL_up_ref(ssl);
	}
	pthread_mutex_unlock(&conn_mutex);
	return ssl;
}

void io_conn_ssl_release(SSL *ssl)
{
	/*
	 * Drop a use-ref.  Not SSL_shutdown -- a use-ref holder finishing
	 * with the object is a refcount event, not a protocol event; the
	 * close_notify is queued once by conn_ssl_drop() at teardown.
	 */
	if (ssl)
		SSL_free(ssl);
}

void io_conn_ssl_clear(int fd)
{
	SSL *ssl = NULL;

	pthread_mutex_lock(&conn_mutex);
	int idx = fd % MAX_CONNECTIONS;
	if (connections[idx] && connections[idx]->ci_fd == fd)
		ssl = conn_ssl_detach_locked(connections[idx]);
	pthread_mutex_unlock(&conn_mutex);

	conn_ssl_drop(ssl);
}

bool io_conn_tls_snapshot(int fd, bool *tls_enabled, bool *handshaking)
{
	bool found = false;

	pthread_mutex_lock(&conn_mutex);
	int idx = fd % MAX_CONNECTIONS;
	/*
	 * Stage 3 Slice 4 (INV-6): treat a CONN_CLOSING slot as gone --
	 * callers (notably io_rpc_trans_cb) use this snapshot as the
	 * gate for "is the connection still tracked", and the slot is
	 * not tracked from a fresh op's point of view once it has
	 * entered the drain state.
	 */
	if (connections[idx] && connections[idx]->ci_fd == fd &&
	    connections[idx]->ci_state != CONN_CLOSING) {
		if (tls_enabled)
			*tls_enabled = connections[idx]->ci_tls_enabled;
		if (handshaking)
			*handshaking = connections[idx]->ci_tls_handshaking;
		found = true;
	}
	pthread_mutex_unlock(&conn_mutex);
	return found;
}

void io_conn_tls_set_state(int fd, bool tls_enabled, bool handshaking)
{
	pthread_mutex_lock(&conn_mutex);
	int idx = fd % MAX_CONNECTIONS;
	if (connections[idx] && connections[idx]->ci_fd == fd) {
		connections[idx]->ci_tls_enabled = tls_enabled;
		connections[idx]->ci_tls_handshaking = handshaking;
	}
	pthread_mutex_unlock(&conn_mutex);
}

/*
 * Per-SSL I/O serialisation (Stage 3 Slice 3, INV-6).  The lock is
 * attached to the SSL itself via ex_data, so its life matches the
 * SSL's life and slot reuse cannot poison it.  NULL-tolerant on the
 * SSL pointer and on a missing ex_data lock (only logged once at
 * install time): callers must always be safe to invoke even if the
 * lock allocation failed, otherwise an OOM at install time would
 * turn into a deadlock here.
 */
void io_conn_ssl_io_lock(SSL *ssl)
{
	if (!ssl || reffs_ssl_io_lock_index < 0)
		return;
	struct reffs_ssl_io_lock *lock =
		SSL_get_ex_data(ssl, reffs_ssl_io_lock_index);
	if (lock)
		pthread_mutex_lock(&lock->rsil_mu);
}

void io_conn_ssl_io_unlock(SSL *ssl)
{
	if (!ssl || reffs_ssl_io_lock_index < 0)
		return;
	struct reffs_ssl_io_lock *lock =
		SSL_get_ex_data(ssl, reffs_ssl_io_lock_index);
	if (lock)
		pthread_mutex_unlock(&lock->rsil_mu);
}
