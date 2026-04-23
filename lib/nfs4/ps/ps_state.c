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

void ps_state_fini(void)
{
	atomic_store_explicit(&ps_nlisteners, 0, memory_order_release);
	memset(ps_listeners, 0, sizeof(ps_listeners));
}
