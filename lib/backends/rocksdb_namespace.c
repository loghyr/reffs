/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * RocksDB namespace database — server-wide persistence.
 *
 * Replaces the flatfile persistence functions (server_state,
 * client identity/incarnation, sb registry) with a single RocksDB
 * database at <state_dir>/namespace.rocksdb/.
 *
 * Column families:
 *   default      — server_state, sb_registry_header
 *   registry     — per-sb registry entries keyed by BE64(sb_id)
 *   clients      — client identity records keyed by BE32(slot)
 *   incarnations — client incarnation records keyed by BE32(slot)
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <rocksdb/c.h>

#include "reffs/log.h"
#include "reffs/persist_ops.h"
#include "reffs/server_persist.h"
#include "reffs/client_persist.h"
#include "reffs/sb_registry.h"

/* ------------------------------------------------------------------ */
/* Error handling                                                      */
/* ------------------------------------------------------------------ */

#define ROCKSDB_CHECK_ERR(err, ret_val, label)      \
	do {                                        \
		if (err) {                          \
			LOG("rocksdb_ns: %s", err); \
			rocksdb_free(err);          \
			err = NULL;                 \
			ret = (ret_val);            \
			goto label;                 \
		}                                   \
	} while (0)

/* ------------------------------------------------------------------ */
/* Column families                                                     */
/* ------------------------------------------------------------------ */

enum ns_cf {
	NS_CF_DEFAULT = 0,
	NS_CF_REGISTRY,
	NS_CF_CLIENTS,
	NS_CF_INCARNATIONS,
	NS_CF_COUNT,
};

static const char *ns_cf_names[NS_CF_COUNT] = {
	[NS_CF_DEFAULT] = "default",
	[NS_CF_REGISTRY] = "registry",
	[NS_CF_CLIENTS] = "clients",
	[NS_CF_INCARNATIONS] = "incarnations",
};

/* Well-known keys in default CF */
static const char key_server_state[] = "server_state";
#define KEY_SERVER_STATE_LEN (sizeof(key_server_state) - 1)
static const char key_registry_header[] = "sb_registry_header";
#define KEY_REGISTRY_HEADER_LEN (sizeof(key_registry_header) - 1)

/* ------------------------------------------------------------------ */
/* Context                                                             */
/* ------------------------------------------------------------------ */

struct rocksdb_ns_ctx {
	rocksdb_t *rn_db;
	rocksdb_column_family_handle_t *rn_cf[NS_CF_COUNT];
	rocksdb_options_t *rn_opts;
	rocksdb_writeoptions_t *rn_wopts;
	rocksdb_readoptions_t *rn_ropts;
	char *rn_path; /* namespace.rocksdb/ path */
	char *rn_state_dir; /* parent state_dir for registry flatfile */
};

/* ------------------------------------------------------------------ */
/* Slot key encoding (4-byte big-endian for client keys)               */
/* ------------------------------------------------------------------ */

static inline void encode_be32(uint8_t *buf, uint32_t val)
{
	buf[0] = (uint8_t)(val >> 24);
	buf[1] = (uint8_t)(val >> 16);
	buf[2] = (uint8_t)(val >> 8);
	buf[3] = (uint8_t)(val);
}

static inline uint32_t __attribute__((unused)) decode_be32(const uint8_t *buf)
{
	return ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
	       ((uint32_t)buf[2] << 8) | ((uint32_t)buf[3]);
}

/* ------------------------------------------------------------------ */
/* Server state                                                        */
/* ------------------------------------------------------------------ */

static int rns_server_state_save(void *ctx,
				 const struct server_persistent_state *sps)
{
	struct rocksdb_ns_ctx *rn = ctx;
	char *err = NULL;
	int ret = 0;

	rocksdb_put_cf(rn->rn_db, rn->rn_wopts, rn->rn_cf[NS_CF_DEFAULT],
		       key_server_state, KEY_SERVER_STATE_LEN,
		       (const char *)sps, sizeof(*sps), &err);
	ROCKSDB_CHECK_ERR(err, -EIO, out);
out:
	return ret;
}

static int rns_server_state_load(void *ctx, struct server_persistent_state *sps)
{
	struct rocksdb_ns_ctx *rn = ctx;
	char *err = NULL;
	int ret = 0;
	size_t vlen = 0;

	char *val = rocksdb_get_cf(rn->rn_db, rn->rn_ropts,
				   rn->rn_cf[NS_CF_DEFAULT], key_server_state,
				   KEY_SERVER_STATE_LEN, &vlen, &err);
	ROCKSDB_CHECK_ERR(err, -EIO, out);

	if (!val)
		return -ENOENT;

	if (vlen != sizeof(*sps)) {
		LOG("rocksdb_ns: bad server_state size: %zu", vlen);
		rocksdb_free(val);
		return -EINVAL;
	}

	memcpy(sps, val, sizeof(*sps));
	rocksdb_free(val);
out:
	return ret;
}

/* ------------------------------------------------------------------ */
/* SB Registry                                                         */
/* ------------------------------------------------------------------ */

/*
 * Registry save/load: delegate to flatfile sb_registry code.
 *
 * NOT_NOW_BROWN_COW: Direct RocksDB registry persistence.
 * Full RocksDB registry requires refactoring sb_registry.c to
 * separate logic (walk sb list) from I/O (read/write entries).
 * For now, the flatfile registry coexists with the namespace DB.
 */
static int rns_registry_save(void *ctx)
{
	struct rocksdb_ns_ctx *rn = ctx;

	return sb_registry_save(rn->rn_state_dir);
}

static int rns_registry_load(void *ctx)
{
	struct rocksdb_ns_ctx *rn = ctx;

	return sb_registry_load(rn->rn_state_dir);
}

static uint64_t rns_registry_alloc_id(void *ctx)
{
	struct rocksdb_ns_ctx *rn = ctx;
	char *err = NULL;
	size_t vlen = 0;

	/* Load current header */
	char *val = rocksdb_get_cf(rn->rn_db, rn->rn_ropts,
				   rn->rn_cf[NS_CF_DEFAULT],
				   key_registry_header, KEY_REGISTRY_HEADER_LEN,
				   &vlen, &err);
	if (err) {
		LOG("rocksdb_ns: registry_alloc_id load: %s", err);
		rocksdb_free(err);
		return 0;
	}

	struct sb_registry_header hdr;
	if (val && vlen == sizeof(hdr)) {
		memcpy(&hdr, val, sizeof(hdr));
		rocksdb_free(val);
	} else {
		if (val)
			rocksdb_free(val);
		/* Fresh — initialize */
		hdr.srh_magic = SB_REGISTRY_MAGIC;
		hdr.srh_version = SB_REGISTRY_VERSION;
		hdr.srh_count = 0;
		hdr.srh_next_id = SB_REGISTRY_FIRST_ID;
	}

	uint64_t id = hdr.srh_next_id++;

	/* Persist incremented counter */
	rocksdb_put_cf(rn->rn_db, rn->rn_wopts, rn->rn_cf[NS_CF_DEFAULT],
		       key_registry_header, KEY_REGISTRY_HEADER_LEN,
		       (const char *)&hdr, sizeof(hdr), &err);
	if (err) {
		LOG("rocksdb_ns: registry_alloc_id save: %s", err);
		rocksdb_free(err);
		return 0;
	}

	return id;
}

/* ------------------------------------------------------------------ */
/* Client identity                                                     */
/* ------------------------------------------------------------------ */

static int rns_client_identity_append(void *ctx,
				      const struct client_identity_record *cir)
{
	struct rocksdb_ns_ctx *rn = ctx;
	char *err = NULL;
	int ret = 0;

	uint8_t key[4];
	encode_be32(key, cir->cir_slot);

	rocksdb_put_cf(rn->rn_db, rn->rn_wopts, rn->rn_cf[NS_CF_CLIENTS],
		       (const char *)key, sizeof(key), (const char *)cir,
		       sizeof(*cir), &err);
	ROCKSDB_CHECK_ERR(err, -EIO, out);
out:
	return ret;
}

static int rns_client_identity_load(
	void *ctx,
	int (*cb)(const struct client_identity_record *cir, void *arg),
	void *arg)
{
	struct rocksdb_ns_ctx *rn = ctx;

	rocksdb_iterator_t *it = rocksdb_create_iterator_cf(
		rn->rn_db, rn->rn_ropts, rn->rn_cf[NS_CF_CLIENTS]);
	rocksdb_iter_seek_to_first(it);

	int ret = 0;
	bool found = false;

	while (rocksdb_iter_valid(it)) {
		size_t vlen;
		const char *val = rocksdb_iter_value(it, &vlen);

		if (vlen == sizeof(struct client_identity_record)) {
			struct client_identity_record cir;
			memcpy(&cir, val, sizeof(cir));
			found = true;
			ret = cb(&cir, arg);
			if (ret)
				break;
		}

		rocksdb_iter_next(it);
	}

	rocksdb_iter_destroy(it);

	if (!found && ret == 0)
		ret = -ENOENT;

	return ret;
}

/* ------------------------------------------------------------------ */
/* Client incarnations                                                 */
/* ------------------------------------------------------------------ */

static int
rns_client_incarnation_add(void *ctx,
			   const struct client_incarnation_record *crc)
{
	struct rocksdb_ns_ctx *rn = ctx;
	char *err = NULL;
	int ret = 0;

	uint8_t key[6];
	encode_be32(key, crc->crc_slot);
	key[4] = (uint8_t)(crc->crc_incarnation >> 8);
	key[5] = (uint8_t)(crc->crc_incarnation & 0xFF);

	rocksdb_put_cf(rn->rn_db, rn->rn_wopts, rn->rn_cf[NS_CF_INCARNATIONS],
		       (const char *)key, sizeof(key), (const char *)crc,
		       sizeof(*crc), &err);
	ROCKSDB_CHECK_ERR(err, -EIO, out);
out:
	return ret;
}

static int rns_client_incarnation_remove(void *ctx, uint32_t slot,
					 uint16_t incarnation)
{
	struct rocksdb_ns_ctx *rn = ctx;
	char *err = NULL;
	int ret = 0;

	if (incarnation == UINT16_MAX) {
		/*
		 * Remove ALL incarnations for this slot (reclaim path).
		 * Prefix-scan by slot and delete each matching key.
		 */
		uint8_t prefix[4];
		encode_be32(prefix, slot);
		rocksdb_iterator_t *it = rocksdb_create_iterator_cf(
			rn->rn_db, rn->rn_ropts, rn->rn_cf[NS_CF_INCARNATIONS]);
		rocksdb_iter_seek(it, (const char *)prefix, sizeof(prefix));
		while (rocksdb_iter_valid(it)) {
			size_t klen;
			const char *k = rocksdb_iter_key(it, &klen);
			if (klen < 4 || memcmp(k, prefix, 4) != 0)
				break;
			rocksdb_delete_cf(rn->rn_db, rn->rn_wopts,
					  rn->rn_cf[NS_CF_INCARNATIONS], k,
					  klen, &err);
			if (err) {
				rocksdb_free(err);
				err = NULL;
			}
			rocksdb_iter_next(it);
		}
		rocksdb_iter_destroy(it);
	} else {
		uint8_t key[6];
		encode_be32(key, slot);
		key[4] = (uint8_t)(incarnation >> 8);
		key[5] = (uint8_t)(incarnation & 0xFF);

		rocksdb_delete_cf(rn->rn_db, rn->rn_wopts,
				  rn->rn_cf[NS_CF_INCARNATIONS],
				  (const char *)key, sizeof(key), &err);
		ROCKSDB_CHECK_ERR(err, -EIO, out);
	}
out:
	return ret;
}

static int rns_client_incarnation_load(void *ctx,
				       struct client_incarnation_record *recs,
				       size_t max_recs, size_t *count)
{
	struct rocksdb_ns_ctx *rn = ctx;

	*count = 0;

	rocksdb_iterator_t *it = rocksdb_create_iterator_cf(
		rn->rn_db, rn->rn_ropts, rn->rn_cf[NS_CF_INCARNATIONS]);
	rocksdb_iter_seek_to_first(it);

	while (rocksdb_iter_valid(it) && *count < max_recs) {
		size_t vlen;
		const char *val = rocksdb_iter_value(it, &vlen);

		if (vlen == sizeof(struct client_incarnation_record)) {
			memcpy(&recs[*count], val, sizeof(recs[*count]));
			(*count)++;
		}

		rocksdb_iter_next(it);
	}

	rocksdb_iter_destroy(it);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

static void rns_fini(void *ctx)
{
	struct rocksdb_ns_ctx *rn = ctx;
	if (!rn)
		return;

	for (int i = 0; i < NS_CF_COUNT; i++) {
		if (rn->rn_cf[i])
			rocksdb_column_family_handle_destroy(rn->rn_cf[i]);
	}

	if (rn->rn_db)
		rocksdb_close(rn->rn_db);

	if (rn->rn_ropts)
		rocksdb_readoptions_destroy(rn->rn_ropts);
	if (rn->rn_wopts)
		rocksdb_writeoptions_destroy(rn->rn_wopts);
	if (rn->rn_opts)
		rocksdb_options_destroy(rn->rn_opts);

	free(rn->rn_state_dir);
	free(rn->rn_path);
	free(rn);
}

/* ------------------------------------------------------------------ */
/* Ops table                                                           */
/* ------------------------------------------------------------------ */

static const struct persist_ops rocksdb_ns_ops = {
	.server_state_save = rns_server_state_save,
	.server_state_load = rns_server_state_load,
	.registry_save = rns_registry_save,
	.registry_load = rns_registry_load,
	.registry_alloc_id = rns_registry_alloc_id,
	.client_identity_append = rns_client_identity_append,
	.client_identity_load = rns_client_identity_load,
	.client_incarnation_add = rns_client_incarnation_add,
	.client_incarnation_remove = rns_client_incarnation_remove,
	.client_incarnation_load = rns_client_incarnation_load,
	.fini = rns_fini,
};

/* ------------------------------------------------------------------ */
/* Public init                                                         */
/* ------------------------------------------------------------------ */

int rocksdb_namespace_init(const char *state_dir,
			   const struct persist_ops **ops_out, void **ctx_out)
{
	struct rocksdb_ns_ctx *rn = NULL;
	char *err = NULL;
	char db_path[PATH_MAX];
	int ret;
	int n;

	n = snprintf(db_path, sizeof(db_path), "%s/namespace.rocksdb",
		     state_dir);
	if (n < 0 || (size_t)n >= sizeof(db_path))
		return -ENAMETOOLONG;

	rn = calloc(1, sizeof(*rn));
	if (!rn)
		return -ENOMEM;

	rn->rn_path = strdup(db_path);
	if (!rn->rn_path) {
		free(rn);
		return -ENOMEM;
	}

	rn->rn_state_dir = strdup(state_dir);
	if (!rn->rn_state_dir) {
		free(rn->rn_path);
		free(rn);
		return -ENOMEM;
	}

	rn->rn_opts = rocksdb_options_create();
	rocksdb_options_set_create_if_missing(rn->rn_opts, 1);
	rocksdb_options_set_create_missing_column_families(rn->rn_opts, 1);

	rn->rn_wopts = rocksdb_writeoptions_create();
	rocksdb_writeoptions_set_sync(rn->rn_wopts, 1);

	rn->rn_ropts = rocksdb_readoptions_create();

	rocksdb_options_t *cf_opts[NS_CF_COUNT];
	for (int i = 0; i < NS_CF_COUNT; i++)
		cf_opts[i] = rn->rn_opts;

	rn->rn_db = rocksdb_open_column_families(
		rn->rn_opts, db_path, NS_CF_COUNT, ns_cf_names,
		(const rocksdb_options_t *const *)cf_opts, rn->rn_cf, &err);

	ret = -EIO;
	ROCKSDB_CHECK_ERR(err, -EIO, err_cleanup);

	*ops_out = &rocksdb_ns_ops;
	*ctx_out = rn;
	return 0;

err_cleanup:
	rns_fini(rn);
	return ret;
}
