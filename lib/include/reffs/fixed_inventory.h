/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifndef _REFFS_FIXED_INVENTORY_H
#define _REFFS_FIXED_INVENTORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <uuid/uuid.h>

#include "reffs/settings.h"

#define FFV2_FIXED_INVENTORY_VERSION 1
#define FFV2_FIXED_INVENTORY_DIGEST_SIZE 32

enum ffv2_fixed_inventory_state {
	FFV2_INVENTORY_FREE = 0,
	FFV2_INVENTORY_CLAIMED = 1,
	FFV2_INVENTORY_ASSIGNED = 2,
	FFV2_INVENTORY_RETIRED = 3,
	FFV2_INVENTORY_FENCED = 4,
};

enum ffv2_fixed_metadata_state {
	FFV2_METADATA_CLEAN = 0,
	FFV2_METADATA_DIRTY = 1,
	FFV2_METADATA_COMMITTED = 2,
};

struct ffv2_fixed_inventory_identity {
	uint32_t dstore_id;
	char address[REFFS_CONFIG_MAX_HOST];
	char export_path[REFFS_CONFIG_MAX_PATH];
	char auth_domain[REFFS_CONFIG_MAX_AUTH_DOMAIN];
	uint8_t store_uuid[REFFS_CONFIG_PROTOTYPE_STORE_UUID_SIZE];
	uint8_t token_digest[FFV2_FIXED_INVENTORY_DIGEST_SIZE];
	uint32_t chunk_size;
	uint32_t data_count;
	uint32_t parity_count;
	uint32_t writer_id;
	uint64_t pnfs_clientid;
	uint32_t object_count;
	struct reffs_prototype_object_config
		objects[REFFS_CONFIG_MAX_PROTOTYPE_OBJECTS];
	uint8_t digest[FFV2_FIXED_INVENTORY_DIGEST_SIZE];
};

struct ffv2_fixed_inventory_record {
	struct ffv2_fixed_inventory_identity identity;
	enum ffv2_fixed_inventory_state state;
	uuid_t owner_sb_uuid;
	uint64_t owner_ino;
	uint64_t segment_offset;
	uint64_t segment_length;
	uint32_t segment_stripe_unit;
	uint32_t segment_layout_type;
	uint32_t segment_checksum_algorithm;
	enum ffv2_fixed_metadata_state metadata_state;
	uint64_t metadata_epoch;
	uint64_t generation;
};

int ffv2_fixed_inventory_identity_init(
	struct ffv2_fixed_inventory_identity *identity, uint32_t dstore_id,
	const char *address, const char *export_path,
	const struct reffs_prototype_registration_config *config);
bool ffv2_fixed_inventory_identity_equal(
	const struct ffv2_fixed_inventory_identity *left,
	const struct ffv2_fixed_inventory_identity *right);

int ffv2_fixed_inventory_transition(struct ffv2_fixed_inventory_record *record,
				    enum ffv2_fixed_inventory_state next,
				    const uuid_t owner_sb_uuid,
				    uint64_t owner_ino);
int ffv2_fixed_metadata_transition(struct ffv2_fixed_inventory_record *record,
				   enum ffv2_fixed_metadata_state next);

int ffv2_fixed_inventory_encode(const struct ffv2_fixed_inventory_record *record,
				uint8_t **data_out, size_t *len_out);
int ffv2_fixed_inventory_decode(const uint8_t *data, size_t len,
				struct ffv2_fixed_inventory_record *record);
int ffv2_fixed_inventory_save(const char *state_dir,
			      const struct ffv2_fixed_inventory_record *record);
int ffv2_fixed_inventory_load(const char *state_dir, uint32_t dstore_id,
			      struct ffv2_fixed_inventory_record *record);

#endif /* _REFFS_FIXED_INVENTORY_H */
