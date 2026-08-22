/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef NFS4_CHUNK_EPOCH_H
#define NFS4_CHUNK_EPOCH_H

#include <stdint.h>

#define CHUNK_MDS_EPOCH_MAGIC 0x43455048U /* "CEPH" */
#define CHUNK_MDS_EPOCH_VERSION 1

struct chunk_mds_epoch {
	uint64_t epoch;
	uint64_t expires_at_ns;
	uint64_t issuer_clientid;
};

/*
 * Read or atomically replace the data server's MDS epoch record.
 * The caller serializes concurrent updates and performs authorization and
 * epoch comparison before calling persist.
 */
int chunk_mds_epoch_load(const char *state_dir, struct chunk_mds_epoch *out);
int chunk_mds_epoch_persist(const char *state_dir,
			    const struct chunk_mds_epoch *epoch);

#endif /* NFS4_CHUNK_EPOCH_H */
