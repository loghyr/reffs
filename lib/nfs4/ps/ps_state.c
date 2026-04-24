/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "reffs/settings.h"

#include "ps_state.h"

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
			ps_listeners[i].pls_session = session;
			return 0;
		}
	}
	return -ENOENT;
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

void ps_state_fini(void)
{
	atomic_store_explicit(&ps_nlisteners, 0, memory_order_release);
	memset(ps_listeners, 0, sizeof(ps_listeners));
}
