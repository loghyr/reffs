/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_NFS4_H
#define _REFFS_NFS4_H

struct persist_ops;

int nfs4_protocol_deregister(void);
int nfs4_protocol_register(void);

/*
 * Enable pNFS layout attributes in the supported_attributes bitmap.
 * Call AFTER nfs4_protocol_register() and server_state_init().
 */
void nfs4_attr_enable_layouts(void);

/*
 * Wire the persistence backend into the migration_record table and
 * reload any records persisted by a previous boot.  Call AFTER
 * server_state_init() (so the persist_ops are available) and AFTER
 * nfs4_protocol_register() (so the table primitives exist).  No-op
 * when ops is NULL (RAM-backed servers leave persistence detached).
 *
 * Returns 0 on success or 0/-errno on reload failure.  A reload
 * failure is logged but does not abort startup -- a stale or corrupt
 * persist file should not prevent the MDS from coming up; missing
 * records will be cleaned up by the lease reaper after 1.5x lease.
 */
int nfs4_migration_persist_init(const struct persist_ops *ops, void *ctx);

#endif
