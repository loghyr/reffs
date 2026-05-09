/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef PS_RENEWAL_INTERNAL_H
#define PS_RENEWAL_INTERNAL_H

#include <stdint.h>

struct ps_listener_state;

/*
 * Internal surface of ps_renewal.c -- exposed for whitebox unit
 * tests of the renewal-tick decision tree (no-session/no-upstream,
 * in-backoff skip, eligible-attempt).  Production callers (the
 * renewal thread) reach renewal_tick_one indirectly via
 * ps_state_listeners_for_each.
 */

struct renewal_tick_ctx {
	uint32_t renewed;
	uint32_t failed;
	uint32_t skipped_no_session;
	uint32_t reconnect_attempted;
	uint32_t reconnect_succeeded;
	uint32_t reconnect_skipped_backoff;
};

int renewal_tick_one(const struct ps_listener_state *pls_const, void *arg);

#endif /* PS_RENEWAL_INTERNAL_H */
