/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifndef NFS4_LEASE_REAPER_H
#define NFS4_LEASE_REAPER_H

int lease_reaper_init(void);
void lease_reaper_fini(void);

#endif /* NFS4_LEASE_REAPER_H */
