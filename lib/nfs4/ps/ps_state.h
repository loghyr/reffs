/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_PS_STATE_H
#define _REFFS_PS_STATE_H

#include <pthread.h>
#include <stdatomic.h>
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
 * Maximum exported paths the PS tracks per listener.  Matches
 * MNTPATHLEN-shaped upstream exports at reasonable scale; a real
 * deployment rarely exceeds a handful.  If an upstream advertises
 * more, the extras are logged and skipped.
 */
#define PS_MAX_EXPORTS_PER_LISTENER 32

/*
 * Per-discovered-export record: the upstream path and the FH we
 * obtained by walking it at discovery time.  Held inside
 * ps_listener_state.pls_exports[] so compound dispatch can look
 * up {listener_id, path} in O(n_exports) to find the upstream FH.
 *
 * ple_fh_len == 0 marks the slot empty.  ple_path is the absolute
 * path as the upstream announced it (NUL-terminated).
 */
struct ps_export {
	char ple_path[1025]; /* MNTPATHLEN + 1 for NUL */
	uint8_t ple_fh[PS_MAX_FH_SIZE];
	/*
	 * Atomic so the re-discovery path can retire a slot to 0,
	 * rewrite ple_fh, and republish the length without readers
	 * ever observing torn FH bytes against a stale-but-nonzero
	 * length.  See ps_state_add_export() for the protocol.
	 */
	_Atomic uint32_t ple_fh_len; /* 0 = slot empty */
};

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

	/*
	 * Discovered upstream exports, indexed by insertion order.  Slots
	 * with ple_fh_len == 0 are empty.  Updates publish via release-
	 * store on pls_nexports; readers use acquire-load (same pattern
	 * as ps_nlisteners at the table level).  Single-writer discipline:
	 * only the discovery coordinator populates this; op handlers read.
	 */
	struct ps_export pls_exports[PS_MAX_EXPORTS_PER_LISTENER];
	_Atomic uint32_t pls_nexports;

	/*
	 * Serializes discovery runs for this listener.  Held across the
	 * whole body of ps_discovery_run() so two writers (reffsd startup
	 * + an on-demand LOOKUP-triggered re-discovery) cannot race on
	 * pls_exports[] / pls_nexports.  Initialized by ps_state_register
	 * and destroyed by ps_state_fini; callers go through
	 * ps_state_discovery_lock() / _unlock() rather than reaching
	 * into the mutex directly so the "no lookup miss on the id"
	 * invariant stays with the registry.
	 */
	pthread_mutex_t pls_discovery_mutex;
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
 * Take (resp. release) the per-listener discovery mutex.  Used by
 * ps_discovery_run() and any future on-demand re-discovery path to
 * serialize writers on pls_exports[] while still letting readers
 * (op handlers calling ps_state_find_export) proceed via the
 * release/acquire atomics.  Returns:
 *
 *   0        success
 *   -ENOENT  no listener with this id is registered
 */
int ps_state_discovery_lock(uint32_t listener_id);
int ps_state_discovery_unlock(uint32_t listener_id);

/*
 * Record a discovered upstream export on this listener.  Called by
 * the discovery coordinator after MOUNT3 EXPORT lists a path and
 * ps_discovery_walk_path() resolves it to an FH on the upstream.
 *
 * Appends to pls_exports[].  If `path` already has an entry the
 * existing slot is updated in place (lets on-demand re-discovery
 * refresh an FH after an upstream restart without growing the
 * table).  `fh_len == 0` is rejected -- a zero-length FH is
 * semantically meaningless and indistinguishable from the
 * empty-slot sentinel.
 *
 * Returns:
 *   0        success
 *   -ENOENT  no listener with this id is registered
 *   -EINVAL  path is NULL / empty, or fh is NULL, or fh_len == 0
 *   -E2BIG   path length >= sizeof(ple_path), or fh_len > PS_MAX_FH_SIZE
 *   -ENOSPC  pls_exports[] is full (PS_MAX_EXPORTS_PER_LISTENER)
 */
int ps_state_add_export(uint32_t listener_id, const char *path,
			const uint8_t *fh, uint32_t fh_len);

/*
 * Look up a discovered export by path.  Returns a pointer to the
 * export slot, or NULL if not found.  The returned pointer is valid
 * until ps_state_fini() and must not be mutated by the caller.
 */
const struct ps_export *ps_state_find_export(uint32_t listener_id,
					     const char *path);

/*
 * Tear down the registry at shutdown.  Must run after all worker
 * threads have stopped, i.e. after io_handler_fini().  No
 * ps_state_find() call may be in flight.  Does NOT destroy any
 * attached mds_session -- the caller MUST drain sessions via
 * ps_state_set_session(id, NULL) + their own destroy first.
 */
void ps_state_fini(void);

#endif /* _REFFS_PS_STATE_H */
