/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_CLIENT_PERSIST_H
#define _REFFS_CLIENT_PERSIST_H

#include <stdint.h>
#include <netinet/in.h>
#include "reffs/network.h"
#include "nfsv42_xdr.h"

/*
 * Persistent client storage.
 *
 * Two files live under the server state directory:
 *
 *   clients
 *     Append-only log of every client identity ever seen.  One
 *     fixed-size record per unique co_ownerid.  Never shrunk.
 *     Read at startup to rebuild the ownerid->slot map and find
 *     the high-water slot for slot_next.
 *
 *   client_incarnations -> client_incarnations.A (or .B)
 *     Symlink pointing to the current active-client file.
 *     Records which slots have confirmed state this boot and carries
 *     enough info (verifier, addr) to drive the EXCHANGE_ID decision
 *     tree.  Rewritten atomically via write-tmp + fdatasync +
 *     symlink-swap on every add or remove.
 *
 * All records are fixed-size so files are seekable by index.
 * All fields are host byte order -- not portable across machines.
 */

#define CLIENT_IDENTITY_MAGIC 0x434C4944U /* "CLID" */
#define CLIENT_INCARNATION_MAGIC 0x434C4943U /* "CLIC" */

#define CLIENT_OWNERID_MAX NFS4_OPAQUE_LIMIT
#define CLIENT_DOMAIN_MAX 256
#define CLIENT_NAME_MAX 256

/*
 * One record in the `clients` file.
 * Stable identity -- written once, never modified.
 */
struct client_identity_record {
	uint32_t cir_magic; /* CLIENT_IDENTITY_MAGIC */
	uint32_t cir_slot; /* stable 32-bit id */
	uint16_t cir_ownerid_len; /* bytes in cir_ownerid */
	uint8_t cir_ownerid[CLIENT_OWNERID_MAX]; /* co_ownerid opaque */
	char cir_domain[CLIENT_DOMAIN_MAX]; /* nii_domain, NUL-term */
	char cir_name[CLIENT_NAME_MAX]; /* nii_name, NUL-term */
	uint8_t cir_pad[2]; /* reserved, must be zero */
};

/*
 * One record in the `client_incarnations` file.
 * Volatile -- the whole file is rewritten atomically on each add/remove.
 */
struct client_incarnation_record {
	uint32_t crc_magic; /* CLIENT_INCARNATION_MAGIC */
	uint32_t crc_slot; /* matches cir_slot */
	uint16_t crc_boot_seq; /* server boot_seq at confirm */
	uint16_t crc_incarnation; /* per-slot reconnect counter */
	uint8_t crc_verifier[NFS4_VERIFIER_SIZE];
	char crc_addr[REFFS_ADDR_LEN]; /* from sockaddr_in_to_full_str */
	uint8_t crc_pad[2]; /* reserved, must be zero */
};

/* ------------------------------------------------------------------ */
/* clients file                                                        */

/*
 * client_identity_append - append one identity record to the clients
 * file, fdatasyncing before returning.  The slot in cir must already
 * be assigned by the caller.
 * Returns 0 on success, -errno on failure.
 */
int client_identity_append(const char *state_dir,
			   const struct client_identity_record *cir);

/*
 * client_identity_load - read the entire clients file and call cb()
 * for each valid record.  Used at startup to rebuild the in-memory map.
 * Iteration stops if cb() returns non-zero (that value is returned).
 * Returns 0 on success, -ENOENT if no clients file exists yet.
 */
int client_identity_load(const char *state_dir,
			 int (*cb)(const struct client_identity_record *cir,
				   void *arg),
			 void *arg);

/* ------------------------------------------------------------------ */
/* client_incarnations file                                            */

/*
 * Maximum number of incarnation records that can be loaded in one pass.
 * Sized to comfortably exceed any realistic client count per server instance.
 */
#define CLIENT_INCARNATION_MAX 1024

/*
 * client_incarnation_load - read all incarnation records into a
 * caller-supplied array.  *count is set to the number of records read.
 * Returns 0 on success, -ENOENT if no incarnations file exists yet
 * (fresh start -- not an error).
 */
int client_incarnation_load(const char *state_dir,
			    struct client_incarnation_record *recs,
			    size_t max_recs, size_t *count);

/*
 * client_incarnation_add - add one record to the active set.
 * Reads the current file, appends the new record, writes the new file,
 * and symlink-swaps it into place atomically.
 * Returns 0 on success, -errno on failure.
 */
int client_incarnation_add(const char *state_dir,
			   const struct client_incarnation_record *crc);

/*
 * client_incarnation_remove - remove the record for slot from the
 * active set.  Reads current file, writes new file without the slot,
 * symlink-swaps atomically.
 * Returns 0 on success, -ENOENT if slot was not present, -errno on
 * I/O failure.
 */
int client_incarnation_remove(const char *state_dir, uint32_t slot);

#endif /* _REFFS_CLIENT_PERSIST_H */
