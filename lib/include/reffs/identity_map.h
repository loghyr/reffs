/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * Identity domain table and bidirectional mapping table.
 *
 * The domain table maps a small index (0..N) to an external domain
 * descriptor (realm name, SID authority, etc.).  Domain 0 is always
 * the local UNIX namespace.
 *
 * The mapping table provides bidirectional lookup between reffs_id
 * values of different types.  For example, a Kerberos principal
 * reffs_id(KRB5, 1, 42) may be mapped to reffs_id(UNIX, 0, 1000).
 */

#ifndef _REFFS_IDENTITY_MAP_H
#define _REFFS_IDENTITY_MAP_H

#include <stdint.h>

#include "reffs/identity_types.h"

/* ------------------------------------------------------------------ */
/* Domain table                                                        */
/* ------------------------------------------------------------------ */

#define IDENTITY_DOMAIN_NAME_MAX 256
#define IDENTITY_DOMAIN_MAX 64

struct identity_domain {
	uint32_t id_index;
	enum reffs_id_type id_type;
	uint32_t id_flags;
	char id_name[IDENTITY_DOMAIN_NAME_MAX];
};

#define ID_DOMAIN_FLAG_ACTIVE (1U << 0)

int identity_domain_init(void);
void identity_domain_fini(void);

/*
 * Find a domain by name, or create one if it doesn't exist.
 * Returns the domain index (>= 0) or -errno.
 */
int identity_domain_find_or_create(const char *name, enum reffs_id_type type);

/* Look up a domain by index.  Returns NULL if not found. */
const struct identity_domain *identity_domain_get(uint32_t index);

/* Persist the domain table to disk. */
int identity_domain_persist(const char *state_dir);

/* Load the domain table from disk. */
int identity_domain_load(const char *state_dir);

/* ------------------------------------------------------------------ */
/* Mapping table                                                       */
/* ------------------------------------------------------------------ */

int identity_map_init(void);
void identity_map_fini(void);

/*
 * Create a bidirectional mapping between two reffs_id values.
 * Either direction can be looked up afterward.
 */
int identity_map_add(reffs_id a, reffs_id b);

/*
 * Look up the UNIX alias for a reffs_id.
 * Returns the UNIX reffs_id, or REFFS_ID_NOBODY_VAL if none found.
 */
reffs_id identity_map_unix_for(reffs_id id);

/*
 * Look up any mapped alias for a reffs_id.
 * Returns the mapped reffs_id, or the input id if no mapping exists.
 */
reffs_id identity_map_lookup(reffs_id id);

/*
 * Remove a mapping by key.  Removes both directions (A-->B and B-->A).
 * Returns 0 on success, -ENOENT if not found.
 */
int identity_map_remove(reffs_id key);

/*
 * Iterate all mappings.  Calls cb(key, value, arg) for each entry.
 * Returns 0 on success, or the first non-zero return from cb.
 */
int identity_map_iterate(int (*cb)(reffs_id key, reffs_id value, void *arg),
			 void *arg);

/* Persist / load the mapping table. */
int identity_map_persist(const char *state_dir);
int identity_map_load(const char *state_dir);

#endif /* _REFFS_IDENTITY_MAP_H */
