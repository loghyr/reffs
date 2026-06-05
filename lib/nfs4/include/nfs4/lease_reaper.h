/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifndef NFS4_LEASE_REAPER_H
#define NFS4_LEASE_REAPER_H

#include <stdint.h>

int lease_reaper_init(void);
void lease_reaper_fini(void);

/*
 * Sweep the server's session hash table once for "trunking probe"
 * sessions -- ones that received CREATE_SESSION but have never been
 * observed in SEQUENCE traffic -- and unhash any whose age at `now`
 * exceeds the probe-reap threshold.  Used by the reaper thread on
 * every tick; exposed so tests can drive the sweep with synthetic
 * `now` values without waiting for wall-clock time.  Caller must
 * pass a server_state with a valid ss_session_ht.
 *
 * Returns the number of sessions unhashed.
 */
struct server_state;
unsigned int lease_reaper_sweep_probe_sessions(struct server_state *ss,
					       uint64_t now_ns);

#endif /* NFS4_LEASE_REAPER_H */
