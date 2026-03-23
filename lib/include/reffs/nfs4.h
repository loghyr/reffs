/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_NFS4_H
#define _REFFS_NFS4_H

int nfs4_protocol_deregister(void);
int nfs4_protocol_register(void);

/*
 * Enable pNFS layout attributes in the supported_attributes bitmap.
 * Call AFTER nfs4_protocol_register() and server_state_init().
 */
void nfs4_attr_enable_layouts(void);

#endif
