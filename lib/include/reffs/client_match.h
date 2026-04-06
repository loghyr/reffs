/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifndef _REFFS_CLIENT_MATCH_H
#define _REFFS_CLIENT_MATCH_H

#include <rpc/xdr.h>
#include <rpc/auth.h>
#include <rpc/auth_unix.h>
#include <sys/socket.h>

#include "reffs/super_block.h"

/*
 * Match the connecting client against an ordered rule list.
 * Returns a pointer to the first matching rule, or NULL if nothing matches.
 *
 * Priority (lower = higher priority):
 *   1  single host (IPv4 or IPv6)
 *   2  CIDR prefix (IPv4 or IPv6)
 *   3  hostname wildcard (*.example.com)
 *   4  anonymous (*)
 *
 * Within the same priority level, list order determines the winner:
 * the first listed rule in the array wins.
 *
 * Hostname wildcard matching requires reverse DNS for the peer address.
 * If DNS lookup fails, the wildcard rule is skipped (never fail open).
 */
const struct sb_client_rule *
client_rule_match(const struct sb_client_rule *rules, unsigned int nrules,
		  const struct sockaddr_storage *peer);

/*
 * Apply root_squash / all_squash from the matched export rule to an
 * AUTH_SYS credential.  No-op when rule is NULL.
 *
 * Callers must hold the matched rule from client_rule_match() before
 * calling this -- squashing must happen before any inode_access_check().
 */
void rpc_cred_squash(struct authunix_parms *ap,
		     const struct sb_client_rule *rule);

#endif /* _REFFS_CLIENT_MATCH_H */
