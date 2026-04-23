/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_PS_STATE_H
#define _REFFS_PS_STATE_H

#include <stdint.h>

#include "reffs/settings.h"

struct mds_session; /* forward: from lib/nfs4/client/ec_client.h */

/*
 * Per-listener proxy-server runtime state.
 *
 * Populated from cfg.proxy_mds[] entries at reffsd startup, one entry
 * per [[proxy_mds]] config block.  Looked up at compound dispatch
 * time via the compound's c_listener_id so op handlers can reach the
 * upstream-MDS binding for the listener the client connected on.
 *
 * pls_session is NULL until reffsd opens a connection to the
 * upstream (ps_state_set_session()).  A NULL session means any of:
 * empty pls_upstream (no upstream configured), connect failed at
 * startup, or session has been torn down.  Op handlers that need the
 * session MUST check for NULL and fail gracefully
 * (NFS4ERR_NOTSUPP or NFS4ERR_DELAY as appropriate).
 *
 * See `.claude/design/proxy-server.md` phase 2.
 */
struct ps_listener_state {
	uint32_t pls_listener_id; /* matches compound->c_listener_id */
	char pls_upstream[REFFS_CONFIG_MAX_HOST];
	uint16_t pls_upstream_port;
	uint16_t pls_upstream_probe;
	struct mds_session *pls_session; /* NULL until session opens */
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
 * Attach an mds_session to an already-registered listener.  The
 * registry stores the pointer; ownership does not transfer -- the
 * caller is responsible for calling mds_session_destroy() + free()
 * before ps_state_fini() runs.  Passing NULL clears any stored
 * pointer (used at shutdown, after the caller has destroyed the
 * session).
 *
 * Returns:
 *   0        success
 *   -ENOENT  no listener with this id is registered
 */
int ps_state_set_session(uint32_t listener_id, struct mds_session *session);

/*
 * Tear down the registry at shutdown.  Must run after all worker
 * threads have stopped, i.e. after io_handler_fini().  No
 * ps_state_find() call may be in flight.  Does NOT destroy any
 * attached mds_session -- the caller MUST drain sessions via
 * ps_state_set_session(id, NULL) + their own destroy first.
 */
void ps_state_fini(void);

#endif /* _REFFS_PS_STATE_H */
