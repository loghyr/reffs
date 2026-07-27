/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * SnapRAID Cauchy erasure encoding, wrapping Andrea Mazzoleni's
 * vendored raid/ library (lib/ec/snapraid-raid/, GPL-2.0-or-later --
 * see snapraid-raid/NOTICE.md).
 *
 * Andrea's library operates in GF(2^8) with the primitive polynomial
 * 0x11d -- the same field reffs's ec_rs uses -- and constructs an
 * Extended Cauchy matrix that reproduces the Linux md RAID-5 / RAID-6
 * coefficients in its first two rows.  See raid/raid.c's top-of-file
 * theory comment for the full construction, and
 * ~/Documents/reffs-docs/snapraid-evaluation.md for the license +
 * adoption rationale.
 *
 * The vendored library caps at 251 data shards + 6 parity shards.
 * We enforce these caps at create time.
 */

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "reffs/ec.h"

#include "snapraid-raid/raid.h"

#define SNAPRAID_MAX_DATA 251
#define SNAPRAID_MAX_PARITY 6

/*
 * raid_init() must be called exactly once before any other raid_*
 * call, and raid_zero() must be set before any raid_rec() call.
 * pthread_once guarantees the init runs once across concurrent
 * ec_snapraid_create() calls.  The zero buffer is per-encoding so
 * decode paths on different shard sizes do not stomp on each other.
 */
static pthread_once_t g_init_once = PTHREAD_ONCE_INIT;

static void snapraid_global_init(void)
{
	raid_init();
	raid_mode(RAID_MODE_CAUCHY);
}

struct snapraid_private {
	void *zero_buf; /* filled with 0, sized to zero_len */
	size_t zero_len;
	pthread_mutex_t zero_mutex; /* protects zero_buf resize race */
};

static int snapraid_ensure_zero(struct snapraid_private *sp, size_t shard_len)
{
	int ret = 0;

	pthread_mutex_lock(&sp->zero_mutex);
	if (sp->zero_len < shard_len) {
		void *nb = realloc(sp->zero_buf, shard_len);

		if (!nb) {
			ret = -ENOMEM;
			goto out;
		}
		memset(nb, 0, shard_len);
		sp->zero_buf = nb;
		sp->zero_len = shard_len;
	}
	raid_zero(sp->zero_buf);
out:
	pthread_mutex_unlock(&sp->zero_mutex);
	return ret;
}

static int snapraid_encode(struct ec_encoding *encoding, uint8_t **data,
			   uint8_t **parity, size_t shard_len)
{
	int k = encoding->ec_k;
	int m = encoding->ec_m;

	/*
	 * raid_gen() requires size to be a multiple of 64.  Fail early
	 * rather than let the library corrupt state.
	 */
	if (shard_len == 0 || (shard_len & 63))
		return -EINVAL;

	int n = k + m;
	void **v = calloc((size_t)n, sizeof(void *));

	if (!v)
		return -ENOMEM;
	for (int i = 0; i < k; i++)
		v[i] = data[i];
	for (int i = 0; i < m; i++)
		v[k + i] = parity[i];

	raid_gen(k, m, shard_len, v);
	free(v);
	return 0;
}

static int snapraid_decode(struct ec_encoding *encoding, uint8_t **shards,
			   const bool *present, size_t shard_len)
{
	struct snapraid_private *sp = encoding->ec_private;
	int k = encoding->ec_k;
	int m = encoding->ec_m;
	int n = k + m;

	if (shard_len == 0 || (shard_len & 63))
		return -EINVAL;

	/*
	 * Count missing.  raid_rec requires nr <= np; if more shards
	 * are gone than we have parity, recovery is impossible and the
	 * ec_decode contract says -EIO.
	 */
	int nr = 0;

	for (int i = 0; i < n; i++)
		if (!present[i])
			nr++;
	if (nr == 0)
		return 0;
	if (nr > m)
		return -EIO;

	int ret = snapraid_ensure_zero(sp, shard_len);

	if (ret < 0)
		return ret;

	int *ir = calloc((size_t)nr, sizeof(int));
	void **v = calloc((size_t)n, sizeof(void *));

	if (!ir || !v) {
		free(ir);
		free(v);
		return -ENOMEM;
	}

	int j = 0;

	for (int i = 0; i < n; i++)
		if (!present[i])
			ir[j++] = i;
	for (int i = 0; i < n; i++)
		v[i] = shards[i];

	/*
	 * raid_rec reconstructs both missing data and missing parity
	 * shards in place.  ir must be sorted -- our left-to-right
	 * walk over present[] emits indexes in ascending order.
	 */
	raid_rec(nr, ir, k, m, shard_len, v);

	free(ir);
	free(v);
	return 0;
}

static void snapraid_destroy(struct ec_encoding *encoding)
{
	struct snapraid_private *sp = encoding->ec_private;

	if (sp) {
		free(sp->zero_buf);
		pthread_mutex_destroy(&sp->zero_mutex);
		free(sp);
	}
	free(encoding);
}

struct ec_encoding *ec_snapraid_create(int k, int m)
{
	if (k < 1 || k > SNAPRAID_MAX_DATA)
		return NULL;
	if (m < 1 || m > SNAPRAID_MAX_PARITY)
		return NULL;

	pthread_once(&g_init_once, snapraid_global_init);

	struct ec_encoding *encoding = calloc(1, sizeof(*encoding));

	if (!encoding)
		return NULL;

	struct snapraid_private *sp = calloc(1, sizeof(*sp));

	if (!sp) {
		free(encoding);
		return NULL;
	}
	if (pthread_mutex_init(&sp->zero_mutex, NULL) != 0) {
		free(sp);
		free(encoding);
		return NULL;
	}

	encoding->ec_name = "snapraid-cauchy";
	encoding->ec_k = k;
	encoding->ec_m = m;
	encoding->ec_encode = snapraid_encode;
	encoding->ec_decode = snapraid_decode;
	encoding->ec_shard_size = NULL; /* uniform: all shards shard_len */
	encoding->ec_destroy = snapraid_destroy;
	encoding->ec_private = sp;
	return encoding;
}
