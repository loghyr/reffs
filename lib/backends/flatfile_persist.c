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

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#include "reffs/persist_ops.h"
#include "reffs/posix_shims.h"
#include "reffs/server_persist.h"
#include "reffs/client_persist.h"
#include "reffs/sb_registry.h"
#include "reffs/migration_persist.h"

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

static int ff_client_incarnation_remove(void *ctx, uint32_t slot)
{
	return client_incarnation_remove((const char *)ctx, slot);
}

static int ff_client_incarnation_load(void *ctx,
				      struct client_incarnation_record *recs,
				      size_t max_recs, size_t *count)
{
	return client_incarnation_load((const char *)ctx, recs, max_recs,
				       count);
}

/*
 * Migration records (slice 6c-zz) live in <state_dir>/migration_records.
 * Append-on-save, full-rewrite-on-remove (a record's lifetime is short
 * relative to the file size cap so periodic compaction is not needed
 * at this slice's scale).  Fixed-size records via
 * sizeof(struct migration_record_persistent) so the load path strides
 * over the file without variable-length parsing.
 */
#define FF_MR_FILENAME "migration_records"

static int ff_mr_path(void *ctx, char *out, size_t out_sz)
{
	int n = snprintf(out, out_sz, "%s/" FF_MR_FILENAME, (const char *)ctx);

	if (n < 0 || (size_t)n >= out_sz)
		return -ENAMETOOLONG;
	return 0;
}

static int
ff_migration_record_save(void *ctx,
			 const struct migration_record_persistent *mrp)
{
	if (!ctx || !mrp)
		return -EINVAL;
	if (mrp->mrp_ndeltas > MR_PERSIST_MAX_DELTAS)
		return -E2BIG;

	char path[512];

	if (ff_mr_path(ctx, path, sizeof(path)) < 0)
		return -ENAMETOOLONG;

	int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);

	if (fd < 0)
		return -errno;
	ssize_t w = write(fd, mrp, sizeof(*mrp));

	if (w != (ssize_t)sizeof(*mrp)) {
		int e = (w < 0) ? -errno : -EIO;

		close(fd);
		return e;
	}
	if (reffs_fdatasync(fd) < 0) {
		int e = -errno;

		close(fd);
		return e;
	}
	close(fd);
	return 0;
}

static int ff_migration_record_remove(void *ctx, const uint8_t *stateid_other)
{
	if (!ctx || !stateid_other)
		return -EINVAL;

	char path[512];

	if (ff_mr_path(ctx, path, sizeof(path)) < 0)
		return -ENAMETOOLONG;

	int fd = open(path, O_RDONLY);

	if (fd < 0)
		return (errno == ENOENT) ? 0 : -errno;

	/*
	 * Read the file into memory, drop the matching record, write
	 * back via a temp file + rename.  File is small (one record per
	 * in-flight migration; cap << 1 MiB at any realistic load) so a
	 * full rewrite is acceptable; a future slice can switch to a
	 * tombstone-then-compact scheme if the in-flight count grows.
	 */
	struct migration_record_persistent buf;
	struct migration_record_persistent *kept = NULL;
	size_t nkept = 0;
	ssize_t r;

	while ((r = read(fd, &buf, sizeof(buf))) == (ssize_t)sizeof(buf)) {
		if (memcmp(buf.mrp_stateid_other, stateid_other,
			   MR_PERSIST_NFS4_OTHER_SIZE) == 0)
			continue; /* drop */
		struct migration_record_persistent *grow =
			realloc(kept, (nkept + 1) * sizeof(buf));

		if (!grow) {
			free(kept);
			close(fd);
			return -ENOMEM;
		}
		kept = grow;
		kept[nkept++] = buf;
	}
	int read_err = (r < 0) ? -errno : 0;

	close(fd);
	if (read_err) {
		free(kept);
		return read_err;
	}

	char tmp[640];
	int n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);

	if (n < 0 || (size_t)n >= sizeof(tmp)) {
		free(kept);
		return -ENAMETOOLONG;
	}
	int wfd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);

	if (wfd < 0) {
		free(kept);
		return -errno;
	}
	for (size_t i = 0; i < nkept; i++) {
		ssize_t w = write(wfd, &kept[i], sizeof(kept[i]));

		if (w != (ssize_t)sizeof(kept[i])) {
			int e = (w < 0) ? -errno : -EIO;

			close(wfd);
			unlink(tmp);
			free(kept);
			return e;
		}
	}
	if (reffs_fdatasync(wfd) < 0) {
		int e = -errno;

		close(wfd);
		unlink(tmp);
		free(kept);
		return e;
	}
	close(wfd);
	free(kept);
	if (rename(tmp, path) < 0) {
		int e = -errno;

		unlink(tmp);
		return e;
	}
	return 0;
}

static int ff_migration_record_load(
	void *ctx,
	int (*cb)(const struct migration_record_persistent *mrp, void *arg),
	void *arg)
{
	if (!ctx || !cb)
		return -EINVAL;

	char path[512];

	if (ff_mr_path(ctx, path, sizeof(path)) < 0)
		return -ENAMETOOLONG;

	int fd = open(path, O_RDONLY);

	if (fd < 0)
		return (errno == ENOENT) ? 0 : -errno;

	struct migration_record_persistent buf;
	ssize_t r;

	while ((r = read(fd, &buf, sizeof(buf))) == (ssize_t)sizeof(buf)) {
		int cret = cb(&buf, arg);

		if (cret < 0) {
			close(fd);
			return cret;
		}
	}
	int err = (r < 0) ? -errno : 0;

	close(fd);
	return err;
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
	.migration_record_save = ff_migration_record_save,
	.migration_record_remove = ff_migration_record_remove,
	.migration_record_load = ff_migration_record_load,
	.fini = ff_fini,
};

const struct persist_ops *flatfile_persist_ops_get(void)
{
	return &flatfile_ops;
}
