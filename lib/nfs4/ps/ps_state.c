/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <errno.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "reffs/settings.h"

#include "ec_client.h" /* mds_session_destroy */
#include "ps_local_addr.h"
#include "ps_renewal.h" /* ps_renewal_kick */
#include "ps_state.h"
#include "ps_write_buffer.h"

#define PS_MAX_LISTENERS REFFS_CONFIG_MAX_PROXY_MDS

static struct ps_listener_state ps_listeners[PS_MAX_LISTENERS];

/*
 * Monotonic count of registered entries.  Release-store here fences
 * all field writes inside ps_listeners[n] behind the count update, so
 * readers that acquire-load the count are guaranteed to see fully
 * populated slots.  Unused slots (indices >= ps_nlisteners) are not
 * touched by readers.
 */
static _Atomic unsigned int ps_nlisteners;

static struct ps_listener_state *ps_listener_by_id(uint32_t listener_id);

int ps_state_init(void)
{
	memset(ps_listeners, 0, sizeof(ps_listeners));
	atomic_store_explicit(&ps_nlisteners, 0, memory_order_release);
	return 0;
}

int ps_state_register(const struct reffs_proxy_mds_config *cfg)
{
	if (!cfg || cfg->id == 0)
		return -EINVAL;

	unsigned int n =
		atomic_load_explicit(&ps_nlisteners, memory_order_acquire);

	for (unsigned int i = 0; i < n; i++) {
		if (ps_listeners[i].pls_listener_id == cfg->id)
			return -EEXIST;
	}

	if (n >= PS_MAX_LISTENERS)
		return -ENOSPC;

	struct ps_listener_state *pls = &ps_listeners[n];

	pls->pls_listener_id = cfg->id;
	strncpy(pls->pls_upstream, cfg->address, sizeof(pls->pls_upstream) - 1);
	pls->pls_upstream[sizeof(pls->pls_upstream) - 1] = '\0';
	pls->pls_upstream_port = cfg->mds_port;
	pls->pls_upstream_probe = cfg->mds_probe;

	/*
	 * Cache TLS bring-up parameters so the renewal thread's reconnect
	 * path can replay mds_session_create_tls() after the original
	 * session dies.  Empty paths are legal (cleartext fallback) --
	 * mds_session_create_tls treats all-empty as "no TLS, plain TCP".
	 */
	strncpy(pls->pls_tls_cert, cfg->tls_cert,
		sizeof(pls->pls_tls_cert) - 1);
	pls->pls_tls_cert[sizeof(pls->pls_tls_cert) - 1] = '\0';
	strncpy(pls->pls_tls_key, cfg->tls_key, sizeof(pls->pls_tls_key) - 1);
	pls->pls_tls_key[sizeof(pls->pls_tls_key) - 1] = '\0';
	strncpy(pls->pls_tls_ca, cfg->tls_ca, sizeof(pls->pls_tls_ca) - 1);
	pls->pls_tls_ca[sizeof(pls->pls_tls_ca) - 1] = '\0';
	pls->pls_tls_mode = (int)cfg->tls_mode;
	pls->pls_tls_insecure_no_verify = cfg->tls_insecure_no_verify;
	pls->pls_registration_id_len = 0; /* boot path stashes it later */

	/*
	 * Initialize the per-listener discovery mutex before publish so
	 * any caller that acquire-loads pls_nlisteners and proceeds to
	 * ps_state_discovery_lock() sees a fully constructed mutex.
	 * Default attrs are fine -- no priority inheritance, no recursion.
	 * Preserve the real POSIX errno (EAGAIN / EPERM / EINVAL / ENOMEM)
	 * rather than flattening, so a future LOG line at the caller can
	 * report something diagnostic.  The slot stays invisible because
	 * pls_nlisteners has not been release-stored yet.
	 */
	int merr = pthread_mutex_init(&pls->pls_discovery_mutex, NULL);

	if (merr != 0)
		return -merr;

	/*
	 * Same rationale for the session-lifetime rwlock: must be fully
	 * constructed before any reader can acquire it via the borrow
	 * helper, which only becomes reachable once the slot is published.
	 */
	int rerr = pthread_rwlock_init(&pls->pls_session_rwlock, NULL);

	if (rerr != 0) {
		pthread_mutex_destroy(&pls->pls_discovery_mutex);
		return -rerr;
	}

	/*
	 * Init-before-publish: relaxed is sufficient because the
	 * release-store on ps_nlisteners below provides the publish edge
	 * for every field in this slot.  Use the explicit atomic store
	 * (rather than plain assignment) for consistency with every other
	 * access to these fields elsewhere -- a future grep audit should
	 * find no plain reads or writes of either atomic.
	 */
	atomic_store_explicit(&pls->pls_reconnect_backoff_sec, 0,
			      memory_order_relaxed);
	atomic_store_explicit(&pls->pls_reconnect_next_attempt_ns, 0,
			      memory_order_relaxed);

	/*
	 * Phase 4a per-listener buffer table + quiesce primitives.
	 * Sets pls_state = RUNNING and pls_boot_gen = 1 internally.
	 * Init-before-publish discipline same as the locks above --
	 * the release-store on ps_nlisteners below fences these.
	 */
	int werr = ps_write_buffer_table_init(pls);

	if (werr != 0) {
		pthread_rwlock_destroy(&pls->pls_session_rwlock);
		pthread_mutex_destroy(&pls->pls_discovery_mutex);
		return werr;
	}

	/*
	 * Phase 5 short-circuit table.  Best-effort -- a getifaddrs(3)
	 * failure leaves the table empty, and ps_local_addr_match() then
	 * returns false for every probe so the RPC path runs.  That is
	 * always safe; the only consequence is missing the co-resident DS
	 * fast-path win.  Registration must not fail on a seed error.
	 */
	(void)ps_local_addr_seed(pls);

	/* Publish: release-store pairs with acquire-load in ps_state_find. */
	atomic_store_explicit(&ps_nlisteners, n + 1, memory_order_release);
	return 0;
}

const struct ps_listener_state *ps_state_find(uint32_t listener_id)
{
	unsigned int n =
		atomic_load_explicit(&ps_nlisteners, memory_order_acquire);

	for (unsigned int i = 0; i < n; i++) {
		if (ps_listeners[i].pls_listener_id == listener_id)
			return &ps_listeners[i];
	}
	return NULL;
}

int ps_state_set_session(uint32_t listener_id, struct mds_session *session)
{
	unsigned int n =
		atomic_load_explicit(&ps_nlisteners, memory_order_acquire);

	for (unsigned int i = 0; i < n; i++) {
		if (ps_listeners[i].pls_listener_id == listener_id) {
			/*
			 * Take the write lock so this initial-publish path
			 * (boot, and the shutdown clear) cannot race with a
			 * worker borrow.  Unlike ps_listener_session_replace,
			 * this helper does NOT destroy any prior session --
			 * the caller retains ownership (boot owns the new
			 * session it just built; shutdown destroys after
			 * clearing).
			 */
			pthread_rwlock_wrlock(
				&ps_listeners[i].pls_session_rwlock);
			ps_listeners[i].pls_session = session;
			pthread_rwlock_unlock(
				&ps_listeners[i].pls_session_rwlock);
			return 0;
		}
	}
	return -ENOENT;
}

int ps_state_set_registration_id(uint32_t listener_id, const uint8_t *id,
				 uint32_t len)
{
	struct ps_listener_state *pls = ps_listener_by_id(listener_id);

	if (!pls)
		return -ENOENT;
	if (len > sizeof(pls->pls_registration_id))
		return -EINVAL;
	if (len > 0 && !id)
		return -EINVAL;
	if (len > 0)
		memcpy(pls->pls_registration_id, id, len);
	pls->pls_registration_id_len = len;
	return 0;
}

int ps_state_set_mds_root_fh(uint32_t listener_id, const uint8_t *fh,
			     uint32_t fh_len)
{
	if (fh_len > PS_MAX_FH_SIZE)
		return -E2BIG;
	if (fh_len > 0 && !fh)
		return -EINVAL;

	unsigned int n =
		atomic_load_explicit(&ps_nlisteners, memory_order_acquire);

	for (unsigned int i = 0; i < n; i++) {
		if (ps_listeners[i].pls_listener_id == listener_id) {
			if (fh_len > 0)
				memcpy(ps_listeners[i].pls_mds_root_fh, fh,
				       fh_len);
			ps_listeners[i].pls_mds_root_fh_len = fh_len;
			return 0;
		}
	}
	return -ENOENT;
}

static struct ps_listener_state *ps_listener_by_id(uint32_t listener_id)
{
	unsigned int n =
		atomic_load_explicit(&ps_nlisteners, memory_order_acquire);

	for (unsigned int i = 0; i < n; i++) {
		if (ps_listeners[i].pls_listener_id == listener_id)
			return &ps_listeners[i];
	}
	return NULL;
}

int ps_state_add_export(uint32_t listener_id, const char *path,
			const uint8_t *fh, uint32_t fh_len)
{
	if (!path || path[0] == '\0' || !fh || fh_len == 0)
		return -EINVAL;
	if (fh_len > PS_MAX_FH_SIZE)
		return -E2BIG;

	struct ps_listener_state *pls = ps_listener_by_id(listener_id);

	if (!pls)
		return -ENOENT;

	size_t path_len = strlen(path);

	if (path_len >= sizeof(pls->pls_exports[0].ple_path))
		return -E2BIG;

	/*
	 * Update-in-place if this path is already in the table.  Lets the
	 * on-demand re-discovery path refresh an FH after the upstream
	 * rebooted without growing pls_nexports.  Single-writer path, so
	 * a relaxed load of pls_nexports is sufficient here -- the writer
	 * is the only producer and synchronises with readers through the
	 * release-store at the end.
	 */
	uint32_t n =
		atomic_load_explicit(&pls->pls_nexports, memory_order_relaxed);

	for (uint32_t i = 0; i < n; i++) {
		uint32_t cur_len = atomic_load_explicit(
			&pls->pls_exports[i].ple_fh_len, memory_order_acquire);
		if (cur_len == 0)
			continue;
		if (strcmp(pls->pls_exports[i].ple_path, path) == 0) {
			/*
			 * Retire to 0 before rewriting ple_fh so a
			 * concurrent reader cannot observe a half-copied
			 * FH against the stale-but-nonzero length.  The
			 * reader's ple_fh_len==0 guard already treats
			 * retirement as "skip this slot until republished."
			 */
			atomic_store_explicit(&pls->pls_exports[i].ple_fh_len,
					      0, memory_order_release);
			memcpy(pls->pls_exports[i].ple_fh, fh, fh_len);
			atomic_store_explicit(&pls->pls_exports[i].ple_fh_len,
					      fh_len, memory_order_release);
			return 0;
		}
	}

	if (n >= PS_MAX_EXPORTS_PER_LISTENER)
		return -ENOSPC;

	struct ps_export *slot = &pls->pls_exports[n];

	memcpy(slot->ple_path, path, path_len + 1);
	memcpy(slot->ple_fh, fh, fh_len);
	/*
	 * Two-step publish.  The release-store on pls_nexports is what
	 * makes the memcpy'd fields visible to readers that acquire-load
	 * the count.  The inner release on ple_fh_len carries the slot
	 * through the update-in-place re-discovery path above (a reader
	 * already past the old count still consults the per-slot empty-
	 * sentinel).
	 */
	atomic_store_explicit(&slot->ple_fh_len, fh_len, memory_order_release);
	atomic_store_explicit(&pls->pls_nexports, n + 1, memory_order_release);
	return 0;
}

const struct ps_export *ps_state_find_export(uint32_t listener_id,
					     const char *path)
{
	if (!path || path[0] == '\0')
		return NULL;

	struct ps_listener_state *pls = ps_listener_by_id(listener_id);

	if (!pls)
		return NULL;

	uint32_t n =
		atomic_load_explicit(&pls->pls_nexports, memory_order_acquire);

	for (uint32_t i = 0; i < n; i++) {
		const struct ps_export *slot = &pls->pls_exports[i];
		uint32_t fh_len = atomic_load_explicit(&slot->ple_fh_len,
						       memory_order_acquire);

		if (fh_len == 0)
			continue;
		if (strcmp(slot->ple_path, path) == 0)
			return slot;
	}
	return NULL;
}

int ps_state_exports_for_each(uint32_t listener_id, ps_state_export_cb cb,
			      void *ctx)
{
	if (!cb)
		return -EINVAL;

	struct ps_listener_state *pls = ps_listener_by_id(listener_id);

	if (!pls)
		return -ENOENT;

	uint32_t n =
		atomic_load_explicit(&pls->pls_nexports, memory_order_acquire);
	unsigned int seen = 0;

	for (uint32_t i = 0; i < n; i++) {
		const struct ps_export *slot = &pls->pls_exports[i];
		uint32_t fh_len = atomic_load_explicit(&slot->ple_fh_len,
						       memory_order_acquire);

		if (fh_len == 0)
			continue;
		cb(slot, ctx);
		seen++;
	}
	return (int)seen;
}

int ps_state_listeners_for_each(ps_state_listener_cb cb, void *arg)
{
	if (!cb)
		return -EINVAL;

	unsigned int n =
		atomic_load_explicit(&ps_nlisteners, memory_order_acquire);

	for (unsigned int i = 0; i < n; i++) {
		int r = cb(&ps_listeners[i], arg);

		if (r)
			return r;
	}
	return 0;
}

int ps_state_discovery_lock(uint32_t listener_id)
{
	struct ps_listener_state *pls = ps_listener_by_id(listener_id);

	if (!pls)
		return -ENOENT;
	pthread_mutex_lock(&pls->pls_discovery_mutex);
	return 0;
}

int ps_state_discovery_unlock(uint32_t listener_id)
{
	struct ps_listener_state *pls = ps_listener_by_id(listener_id);

	if (!pls)
		return -ENOENT;
	pthread_mutex_unlock(&pls->pls_discovery_mutex);
	return 0;
}

int ps_listener_stop(uint32_t listener_id)
{
	struct ps_listener_state *pls = ps_listener_by_id(listener_id);
	enum ps_listener_state_kind expected;

	if (!pls)
		return -ENOENT;

	/*
	 * CAS-elect the destroyer: exactly one caller transitions
	 * RUNNING -> DRAINING and proceeds through the wait + destroy.
	 * Any concurrent / repeat caller either observes STOPPED (the
	 * destroyer already finished) or DRAINING (the destroyer is
	 * still running) and waits for STOPPED before returning.
	 *
	 * Without this election the original implementation would let
	 * a second caller fall through to a duplicate
	 * ps_write_buffer_table_destroy(), double-destroying the lfht
	 * + pls_drain_mutex + pls_drain_cv.  Reviewer caught this in
	 * verdict-1 of 4a.2a; the design's prose said "atomic
	 * exchange" but exchange does not gate followers.
	 */
	expected = PS_LISTENER_RUNNING;
	if (!atomic_compare_exchange_strong_explicit(
		    &pls->pls_state, &expected, PS_LISTENER_DRAINING,
		    memory_order_acq_rel, memory_order_acquire)) {
		if (expected == PS_LISTENER_STOPPED)
			return 0;
		/*
		 * Another thread won the CAS and is in the destroy
		 * path.  Wait for STOPPED.  sched_yield rather than
		 * cv_wait here -- the cv/mutex the destroyer uses are
		 * about to be destroyed; introducing a second cv just
		 * for this rare path would complicate the lifecycle.
		 * Destroy is bounded by REFFS_PS_FLUSH_TIMEOUT_NS in
		 * practice, so the spin is finite.
		 */
		while (atomic_load_explicit(&pls->pls_state,
					    memory_order_acquire) !=
		       PS_LISTENER_STOPPED)
			sched_yield();
		return 0;
	}

	/*
	 * We are the destroyer.  Wait for in-flight ops to drain.
	 * After the DRAINING store above, enter_quiesce_or_bail sees
	 * != RUNNING and bails without taking find refs; existing
	 * in-flight ops will drop theirs on their unwind.
	 */
	pthread_mutex_lock(&pls->pls_drain_mutex);
	while (atomic_load_explicit(&pls->pls_active_buffer_refs,
				    memory_order_acquire) > 0)
		pthread_cond_wait(&pls->pls_drain_cv, &pls->pls_drain_mutex);
	pthread_mutex_unlock(&pls->pls_drain_mutex);

	/* Drain the table + destroy the cv/mutex/lfht. */
	ps_write_buffer_table_destroy(pls);

	/* Publish STOPPED LAST so spinning followers see destroy-complete. */
	atomic_store_explicit(&pls->pls_state, PS_LISTENER_STOPPED,
			      memory_order_release);
	return 0;
}

void ps_state_fini(void)
{
	/*
	 * Drive every registered listener through DRAINING -> STOPPED
	 * first so the Phase 4a buffer table + drain CV + drain mutex
	 * get destroyed under the quiesce protocol (callers that might
	 * still be in op-handler context observe DRAINING and bail).
	 * Then destroy the long-lived discovery mutex + session rwlock.
	 *
	 * Sessions themselves are owned/freed by the caller via
	 * ps_state_set_session(id, NULL) (the contract on this
	 * function's docstring) so we do not destroy them here.
	 */
	unsigned int n =
		atomic_load_explicit(&ps_nlisteners, memory_order_acquire);

	for (unsigned int i = 0; i < n; i++)
		ps_listener_stop(ps_listeners[i].pls_listener_id);

	for (unsigned int i = 0; i < n; i++) {
		pthread_mutex_destroy(&ps_listeners[i].pls_discovery_mutex);
		pthread_rwlock_destroy(&ps_listeners[i].pls_session_rwlock);
	}

	atomic_store_explicit(&ps_nlisteners, 0, memory_order_release);
	memset(ps_listeners, 0, sizeof(ps_listeners));
}

/* ------------------------------------------------------------------ */
/* Reconnect support: borrow/release/replace + classifier + backoff    */
/* ------------------------------------------------------------------ */

struct mds_session *ps_listener_session_borrow(uint32_t listener_id)
{
	struct ps_listener_state *pls = ps_listener_by_id(listener_id);

	if (!pls)
		return NULL;

	pthread_rwlock_rdlock(&pls->pls_session_rwlock);
	/*
	 * Phase 4a listener-borrow contract: refuse the borrow when
	 * the listener is no longer RUNNING.  This is the gate that
	 * lets ps_listener_stop bound the forward path's lifetime
	 * too -- callers that observe NULL during DRAINING/STOPPED
	 * map to NFS4ERR_DELAY rather than touching about-to-be-freed
	 * session memory.  acquire-load pairs with the release-store
	 * in ps_listener_stop.  Loaded UNDER the rdlock so the
	 * session pointer we observe next cannot be swapped from
	 * under us in the same critical section.
	 */
	enum ps_listener_state_kind state =
		atomic_load_explicit(&pls->pls_state, memory_order_acquire);

	if (state != PS_LISTENER_RUNNING) {
		pthread_rwlock_unlock(&pls->pls_session_rwlock);
		return NULL;
	}
	struct mds_session *ms = pls->pls_session;

	if (!ms) {
		pthread_rwlock_unlock(&pls->pls_session_rwlock);
		return NULL;
	}
	return ms;
}

void ps_listener_session_release(uint32_t listener_id)
{
	struct ps_listener_state *pls = ps_listener_by_id(listener_id);

	if (!pls)
		return;
	pthread_rwlock_unlock(&pls->pls_session_rwlock);
}

void ps_listener_kick_reconnect(uint32_t listener_id)
{
	struct ps_listener_state *pls = ps_listener_by_id(listener_id);

	if (!pls)
		return;

	/*
	 * Reset the schedule to "next attempt is allowed immediately"
	 * BEFORE waking the renewal thread.  Order matters: if we woke
	 * first, the thread could read a stale (in-the-future) deadline
	 * and skip the reconnect on its first post-wake tick, costing
	 * us a full renewal interval before the schedule is observed
	 * cleared.  Each store is independent (no compound update); a
	 * release-store on each is sufficient because the renewal
	 * thread acquire-loads each field separately at tick time.
	 */
	atomic_store_explicit(&pls->pls_reconnect_next_attempt_ns, 0,
			      memory_order_release);
	atomic_store_explicit(&pls->pls_reconnect_backoff_sec, 0,
			      memory_order_release);
	ps_renewal_kick();
}

int ps_listener_session_replace(uint32_t listener_id,
				struct mds_session *new_session)
{
	struct ps_listener_state *pls = ps_listener_by_id(listener_id);

	if (!pls)
		return -ENOENT;

	pthread_rwlock_wrlock(&pls->pls_session_rwlock);
	struct mds_session *old_session = pls->pls_session;

	pls->pls_session = new_session;
	pthread_rwlock_unlock(&pls->pls_session_rwlock);

	/*
	 * Destroy the old session OUTSIDE the write lock.  Two reasons:
	 *   1. mds_session_destroy sends DESTROY_SESSION + DESTROY_CLIENTID
	 *      on the wire; even on an already-dead session that round-trip
	 *      may take RPC-timeout-many seconds.  Holding the wrlock that
	 *      whole time blocks every new borrower; new sessions installed
	 *      via this same call would also race with destroy on a
	 *      poorly-ordered call site.
	 *   2. clnt_destroy + SSL_CTX_free are reentrancy-tolerant once
	 *      the wire ops have completed; no other thread can reach the
	 *      old session because its publish-pointer was already cleared
	 *      under the wlock.
	 */
	if (old_session) {
		mds_session_destroy(old_session);
		free(old_session);
	}
	return 0;
}

/*
 * ps_session_is_dead / ps_reconnect_backoff_next / _reset are now
 * thin wrappers around the canonical lib/nfs4/client/mds_session
 * implementations (mds_session_is_dead etc.), promoted there so the
 * MDS-to-DS keep-alive thread (lib/nfs4/dstore/ds_renewal.c) shares
 * the same classifier and backoff schedule.  See
 * .claude/design/mds-ds-session-keepalive.md.  PS source untouched;
 * future cleanup may inline the call sites and delete these wrappers.
 */
bool ps_session_is_dead(int err, nfsstat4 sr_status)
{
	return mds_session_is_dead(err, sr_status);
}

uint32_t ps_reconnect_backoff_next(uint32_t *backoff_sec)
{
	return mds_reconnect_backoff_next(backoff_sec);
}

void ps_reconnect_backoff_reset(uint32_t *backoff_sec)
{
	mds_reconnect_backoff_reset(backoff_sec);
}
