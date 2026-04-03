/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * Flatfile persistence implementation.
 *
 * Thin wrappers around the existing server_persist, client_persist,
 * and sb_registry functions.  ctx is a strdup'd state_dir path.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <string.h>

#include "reffs/persist_ops.h"
#include "reffs/server_persist.h"
#include "reffs/client_persist.h"
#include "reffs/sb_registry.h"

static int ff_server_state_save(void *ctx,
				const struct server_persistent_state *sps)
{
	return server_persist_save((const char *)ctx, sps);
}

static int ff_server_state_load(void *ctx, struct server_persistent_state *sps)
{
	return server_persist_load((const char *)ctx, sps);
}

static int ff_registry_save(void *ctx)
{
	return sb_registry_save((const char *)ctx);
}

static int ff_registry_load(void *ctx)
{
	return sb_registry_load((const char *)ctx);
}

static uint64_t ff_registry_alloc_id(void *ctx)
{
	return sb_registry_alloc_id((const char *)ctx);
}

static int ff_client_identity_append(void *ctx,
				     const struct client_identity_record *cir)
{
	return client_identity_append((const char *)ctx, cir);
}

static int ff_client_identity_load(
	void *ctx,
	int (*cb)(const struct client_identity_record *cir, void *arg),
	void *arg)
{
	return client_identity_load((const char *)ctx, cb, arg);
}

static int
ff_client_incarnation_add(void *ctx,
			  const struct client_incarnation_record *crc)
{
	return client_incarnation_add((const char *)ctx, crc);
}

static int ff_client_incarnation_remove(void *ctx, uint32_t slot,
					uint16_t incarnation)
{
	return client_incarnation_remove((const char *)ctx, slot, incarnation);
}

static int ff_client_incarnation_load(void *ctx,
				      struct client_incarnation_record *recs,
				      size_t max_recs, size_t *count)
{
	return client_incarnation_load((const char *)ctx, recs, max_recs,
				       count);
}

static void ff_fini(void *ctx)
{
	free(ctx);
}

static const struct persist_ops flatfile_ops = {
	.server_state_save = ff_server_state_save,
	.server_state_load = ff_server_state_load,
	.registry_save = ff_registry_save,
	.registry_load = ff_registry_load,
	.registry_alloc_id = ff_registry_alloc_id,
	.client_identity_append = ff_client_identity_append,
	.client_identity_load = ff_client_identity_load,
	.client_incarnation_add = ff_client_incarnation_add,
	.client_incarnation_remove = ff_client_incarnation_remove,
	.client_incarnation_load = ff_client_incarnation_load,
	.fini = ff_fini,
};

const struct persist_ops *flatfile_persist_ops_get(void)
{
	return &flatfile_ops;
}
