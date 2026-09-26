/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifndef _REFFS_FFV2_PROTOTYPE_H
#define _REFFS_FFV2_PROTOTYPE_H

#include <stddef.h>
#include <stdint.h>

#include "reffs/settings.h"

struct dstore;
struct layout_segment;

struct ffv2_prototype_member {
	uint8_t ordinary_handle[REFFS_CONFIG_MAX_PROTOTYPE_FH];
	uint32_t ordinary_handle_len;
	uint8_t mapped_handle[REFFS_CONFIG_MAX_PROTOTYPE_FH];
	uint32_t mapped_handle_len;
	/* Netlink returns seqid in host order, matching stateid4 in memory. */
	uint8_t stateid[16];
};

struct ffv2_prototype_snapshot {
	uint32_t dstore_id;
	uint32_t chunk_size;
	uint32_t data_count;
	uint32_t parity_count;
	uint32_t writer_id;
	uint64_t pnfs_clientid;
	uint64_t generation;
	uint32_t object_count;
	uint8_t source_uuid[16];
	uint8_t service_uuid[16];
	uint8_t replay_uuid[16];
	struct ffv2_prototype_member members[REFFS_CONFIG_MAX_PROTOTYPE_OBJECTS];
};

struct ffv2_prototype_transport {
	void *context;
	int (*open)(void *context);
	int (*disable)(void *context);
	int (*enable)(void *context);
	int (*challenge)(void *context, uint8_t nonce[32]);
	int (*register_vector)(
		void *context,
		const struct reffs_prototype_registration_config *config,
		uint32_t dstore_id, const uint8_t nonce[32],
		struct ffv2_prototype_snapshot **snapshot_out);
	void (*close)(void *context);
};

/* Install the configured provider vector for one dstore. */
int ffv2_prototype_register_dstore(struct dstore *ds);

/* Shared registration state machine; exposed for fault-injection tests. */
int ffv2_prototype_register_transport(
	struct dstore *ds, const struct ffv2_prototype_transport *transport);

/* Tear down the provider gate and remove the locally published vector. */
void ffv2_prototype_unregister_dstore(struct dstore *ds);

/* Read-side lifetime is the dstore prototype rwlock. */
const struct ffv2_prototype_snapshot *
ffv2_prototype_snapshot_borrow(struct dstore *ds);
void ffv2_prototype_snapshot_release(struct dstore *ds);
int ffv2_prototype_snapshot_replace(struct dstore *ds,
				    struct ffv2_prototype_snapshot *snapshot);

/* Exact prototype selection.  Zero means ordinary path; negative fails shut. */
int ffv2_prototype_snapshot_select(
	const struct layout_segment *seg, uint32_t writer_id,
	uint64_t pnfs_clientid, struct dstore **ds_out,
	const struct ffv2_prototype_snapshot **snapshot_out);

/* Test seams for canonical request and strict reply validation. */
int ffv2_prototype_challenge_parse(const uint8_t *attributes,
				   size_t attributes_len, uint8_t nonce[32]);
int ffv2_prototype_request_build(
	const struct reffs_prototype_registration_config *config,
	const uint8_t nonce[32], uint16_t family, uint32_t seq, uint8_t *buffer,
	size_t buffer_size, size_t *message_size);
int ffv2_prototype_reply_parse(
	const struct reffs_prototype_registration_config *config,
	uint32_t dstore_id, const uint8_t *attributes, size_t attributes_len,
	struct ffv2_prototype_snapshot **snapshot_out);

#endif /* _REFFS_FFV2_PROTOTYPE_H */
