/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_PS_STATE_H
#define _REFFS_PS_STATE_H

#include <stdint.h>

#include "reffs/settings.h"

/*
 * Per-listener proxy-server runtime state.
 *
 * Populated from cfg.proxy_mds[] entries at reffsd startup, one entry
 * per [[proxy_mds]] config block.  Looked up at compound dispatch
 * time via the compound's c_listener_id so op handlers can reach the
 * upstream-MDS binding for the listener the client connected on.
 *
 * This slice only stores the upstream binding.  The mds_session
 * handle (actual connection to the upstream) is added in a follow-up
 * slice; empty pls_upstream means "no upstream configured for this
 * listener -- keep the namespace dark".
 *
 * See `.claude/design/proxy-server.md` phase 2.
 */
struct ps_listener_state {
	uint32_t pls_listener_id; /* matches compound->c_listener_id */
	char pls_upstream[REFFS_CONFIG_MAX_HOST];
	uint16_t pls_upstream_port;
	uint16_t pls_upstream_probe;
};

/*
 * Initialize the registry.  Call once during reffsd startup before
 * any ps_state_register() / ps_state_find() call.  Single-threaded.
 * Returns 0 on success, -errno on failure.
 */
int ps_state_init(void);

/*
 * Register one listener.  Called from reffsd main for each
 * cfg.proxy_mds[] entry, regardless of whether address is empty
 * (an empty-address entry still gets registered so a future
 * ps_state_find() doesn't return NULL for an unconfigured-upstream
 * listener -- callers can distinguish via pls_upstream[0] == '\0').
 *
 * Returns:
 *   0        success
 *   -EINVAL  cfg is NULL, or cfg->id == 0 (reserved for native)
 *   -EEXIST  a listener with this id is already registered
 *   -ENOSPC  the registry is full
 */
int ps_state_register(const struct reffs_proxy_mds_config *cfg);

/*
 * Look up a listener by id.  Returns NULL if not found.  The
 * returned pointer is valid until ps_state_fini().
 *
 * Safe to call from any thread after ps_state_init() has returned.
 * The registry is populated during startup (single-threaded) and is
 * read-only in steady state -- updates publish via release-store on
 * the count, readers use acquire-load.
 */
const struct ps_listener_state *ps_state_find(uint32_t listener_id);

/*
 * Tear down the registry at shutdown.  Must run after all worker
 * threads have stopped, i.e. after io_handler_fini().  No
 * ps_state_find() call may be in flight.
 */
void ps_state_fini(void);

#endif /* _REFFS_PS_STATE_H */
