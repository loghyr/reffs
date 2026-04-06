/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_SERVER_PERSIST_H
#define _REFFS_SERVER_PERSIST_H

#include <stdint.h>
#include <uuid/uuid.h>

/*
 * On-disk server state record.
 *
 * Written to stable storage (fdatasync) before boot_seq or slot_next
 * are used.  On read, validate magic and version before trusting
 * contents.  All fields are host byte order -- this file is not
 * portable across machines, which is fine for a single-server
 * persistent state record.
 *
 * The caller supplies a state directory (e.g. /var/lib/reffs/mds or
 * /var/lib/reffs/ds0); individual state files (server_state,
 * client_state, ...) are created beneath it.  MDS and DS instances on
 * the same machine have independent directories and hence independent
 * records.
 */

#define REFFS_SERVER_STATE_MAGIC 0x52454646U /* "REFF" */
#define REFFS_SERVER_STATE_VERSION 1

struct server_persistent_state {
	uint32_t sps_magic; /* REFFS_SERVER_STATE_MAGIC */
	uint32_t sps_version; /* REFFS_SERVER_STATE_VERSION */
	uint16_t sps_boot_seq; /* monotonic restart counter */
	uint8_t sps_clean_shutdown; /* 1 if last shutdown was clean */
	uint8_t sps_pad;
	uint32_t sps_slot_next; /* next unallocated client slot */
	uint32_t sps_lease_time; /* in seconds */
	uuid_t sps_uuid; /* stable server identity across reboots */
};

/* ------------------------------------------------------------------ */
/* Persistence I/O                                                     */

/*
 * server_persist_load - read and validate the server_state file in dir.
 * Returns 0 on success, -ENOENT if not found (fresh start),
 * -EINVAL if corrupt, other -errno on I/O error.
 */
int server_persist_load(const char *dir, struct server_persistent_state *sps);

/*
 * server_persist_save - write and fdatasync the server_state file in dir.
 * Returns 0 on success, -errno on failure.
 */
int server_persist_save(const char *dir,
			const struct server_persistent_state *sps);

#endif /* _REFFS_SERVER_PERSIST_H */
