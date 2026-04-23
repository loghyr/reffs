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
 * Upper bound on filehandle length used by proxy-server storage.
 * NFSv4 caps at 128 bytes (RFC 8881).  Defining the limit locally
 * keeps ps_state.h free of the generated XDR header.
 */
#define PS_MAX_FH_SIZE 128

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
 * pls_mds_root_fh_len == 0 means the MDS root FH has not been
 * discovered yet.  Populated by ps_state_set_mds_root_fh() after a
 * successful PUTROOTFH+GETFH round-trip on the session.  Note that
 * this is distinct from "session not open": a listener can have a
 * valid pls_session but a zero pls_mds_root_fh_len if discovery
 * failed at startup.  Future op handlers / forwarding paths must
 * treat (session open && root FH empty) as an explicit error rather
 * than blindly dereferencing an empty FH.  A `ps_state_discovery_complete()`
 * helper will live here once the first consumer lands.
 * NOT_NOW_BROWN_COW: discovery-complete helper.
 *
 * See `.claude/design/proxy-server.md` phase 2.
 */
struct ps_listener_state {
	uint32_t pls_listener_id; /* matches compound->c_listener_id */
	char pls_upstream[REFFS_CONFIG_MAX_HOST];
	uint16_t pls_upstream_port;
	uint16_t pls_upstream_probe;
	struct mds_session *pls_session; /* NULL until session opens */
	uint8_t pls_mds_root_fh[PS_MAX_FH_SIZE];
	uint32_t pls_mds_root_fh_len; /* 0 = not yet discovered */
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
 * Record the MDS's root filehandle for this listener.  Fetched once
 * at startup via ps_discovery_fetch_root_fh() and stored so future
 * LOOKUP / GETATTR forwarding does not need to re-ask.  `fh_len == 0`
 * is legal (clears the stored FH -- useful for test cleanup).
 *
 * Returns:
 *   0        success
 *   -ENOENT  no listener with this id is registered
 *   -E2BIG   fh_len exceeds PS_MAX_FH_SIZE
 */
int ps_state_set_mds_root_fh(uint32_t listener_id, const uint8_t *fh,
			     uint32_t fh_len);

/*
 * Tear down the registry at shutdown.  Must run after all worker
 * threads have stopped, i.e. after io_handler_fini().  No
 * ps_state_find() call may be in flight.  Does NOT destroy any
 * attached mds_session -- the caller MUST drain sessions via
 * ps_state_set_session(id, NULL) + their own destroy first.
 */
void ps_state_fini(void);

#endif /* _REFFS_PS_STATE_H */
