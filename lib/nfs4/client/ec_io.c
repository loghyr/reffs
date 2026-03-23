/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * Erasure-coded I/O for the EC demo client.
 *
 * Combines the MDS session, layout, DS I/O, and Reed-Solomon codec
 * into high-level write/read/verify operations.
 *
 * Write path:
 *   1. Open file on MDS, get layout (LAYOUTGET + GETDEVICEINFO)
 *   2. Pad input to multiple of k * shard_len
 *   3. For each stripe: RS-encode k data → m parity, write k+m to DSes
 *   4. LAYOUTRETURN, CLOSE
 *
 * Read path:
 *   1. Open file on MDS, get layout
 *   2. For each stripe: read available shards, RS-decode, reassemble
 *   3. LAYOUTRETURN, CLOSE
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nfsv42_xdr.h"
#include "ec_client.h"
#include "reffs/ec.h"

/* Default shard size: 64 KiB. */
#define EC_SHARD_SIZE (64 * 1024)

/* ------------------------------------------------------------------ */
/* Resolve all mirrors to DS connections                                */
/* ------------------------------------------------------------------ */

struct ec_context {
	struct mds_session *ctx_ms;
	struct mds_file ctx_file;
	struct ec_layout ctx_layout;
	struct ec_device *ctx_devs;
	struct ds_conn *ctx_conns;
	struct ec_codec *ctx_codec;
	uint32_t ctx_k;
	uint32_t ctx_m;
};

static int ec_resolve_mirrors(struct ec_context *ctx)
{
	uint32_t n = ctx->ctx_layout.el_nmirrors;

	ctx->ctx_devs = calloc(n, sizeof(struct ec_device));
	ctx->ctx_conns = calloc(n, sizeof(struct ds_conn));
	if (!ctx->ctx_devs || !ctx->ctx_conns)
		return -ENOMEM;

	for (uint32_t i = 0; i < n; i++) {
		struct ec_mirror *em = &ctx->ctx_layout.el_mirrors[i];
		int ret;

		ret = mds_getdeviceinfo(ctx->ctx_ms, em->em_deviceid,
					&ctx->ctx_devs[i]);
		if (ret)
			return ret;

		ret = ds_connect(&ctx->ctx_conns[i], &ctx->ctx_devs[i],
				 em->em_uid, em->em_gid);
		if (ret)
			return ret;
	}

	return 0;
}

static void ec_disconnect_all(struct ec_context *ctx)
{
	if (!ctx->ctx_conns)
		return;

	for (uint32_t i = 0; i < ctx->ctx_layout.el_nmirrors; i++)
		ds_disconnect(&ctx->ctx_conns[i]);

	free(ctx->ctx_conns);
	ctx->ctx_conns = NULL;
	free(ctx->ctx_devs);
	ctx->ctx_devs = NULL;
}

/* ------------------------------------------------------------------ */
/* Plain (non-EC) write                                                */
/* ------------------------------------------------------------------ */

int plain_write(struct mds_session *ms, const char *path, const uint8_t *data,
		size_t data_len)
{
	struct mds_file mf;
	struct ec_layout layout;
	int ret;

	ret = mds_file_open(ms, path, &mf);
	if (ret)
		return ret;

	ret = mds_layout_get(ms, &mf, LAYOUTIOMODE4_RW, &layout);
	if (ret)
		goto out_close;

	if (layout.el_nmirrors < 1) {
		ret = -EINVAL;
		goto out_layout;
	}

	/* Resolve the first mirror. */
	struct ec_device dev;

	ret = mds_getdeviceinfo(ms, layout.el_mirrors[0].em_deviceid, &dev);
	if (ret)
		goto out_layout;

	struct ds_conn dc;

	ret = ds_connect(&dc, &dev, layout.el_mirrors[0].em_uid,
			 layout.el_mirrors[0].em_gid);
	if (ret)
		goto out_layout;

	/* Write in chunks up to 1 MB. */
	size_t off = 0;

	while (off < data_len) {
		uint32_t chunk = (uint32_t)(data_len - off);

		if (chunk > 1048576)
			chunk = 1048576;
		ret = ds_write(&dc, layout.el_mirrors[0].em_fh,
			       layout.el_mirrors[0].em_fh_len, (uint64_t)off,
			       data + off, chunk);
		if (ret)
			break;
		off += chunk;
	}

	ds_disconnect(&dc);
out_layout:
	mds_layout_return(ms, &mf, &layout);
	ec_layout_free(&layout);
out_close:
	mds_file_close(ms, &mf);
	return ret;
}

/* ------------------------------------------------------------------ */
/* Plain (non-EC) read                                                 */
/* ------------------------------------------------------------------ */

int plain_read(struct mds_session *ms, const char *path, uint8_t *buf,
	       size_t buf_len, size_t *out_len)
{
	struct mds_file mf;
	struct ec_layout layout;
	int ret;

	ret = mds_file_open(ms, path, &mf);
	if (ret)
		return ret;

	ret = mds_layout_get(ms, &mf, LAYOUTIOMODE4_READ, &layout);
	if (ret)
		goto out_close;

	if (layout.el_nmirrors < 1) {
		ret = -EINVAL;
		goto out_layout;
	}

	struct ec_device dev;

	ret = mds_getdeviceinfo(ms, layout.el_mirrors[0].em_deviceid, &dev);
	if (ret)
		goto out_layout;

	struct ds_conn dc;

	ret = ds_connect(&dc, &dev, layout.el_mirrors[0].em_uid,
			 layout.el_mirrors[0].em_gid);
	if (ret)
		goto out_layout;

	/* Read in chunks up to 1 MB. */
	size_t total = 0;

	while (total < buf_len) {
		uint32_t want = (uint32_t)(buf_len - total);
		uint32_t nread = 0;

		if (want > 1048576)
			want = 1048576;
		ret = ds_read(&dc, layout.el_mirrors[0].em_fh,
			      layout.el_mirrors[0].em_fh_len, (uint64_t)total,
			      buf + total, want, &nread);
		if (ret)
			break;
		total += nread;
		if (nread < want)
			break; /* EOF */
	}

	if (ret == 0 && out_len)
		*out_len = total;

	ds_disconnect(&dc);
out_layout:
	mds_layout_return(ms, &mf, &layout);
	ec_layout_free(&layout);
out_close:
	mds_file_close(ms, &mf);
	return ret;
}

/* ------------------------------------------------------------------ */
/* EC Write                                                            */
/* ------------------------------------------------------------------ */

int ec_write(struct mds_session *ms, const char *path, const uint8_t *data,
	     size_t data_len, int k, int m)
{
	struct ec_context ctx;
	int ret;

	memset(&ctx, 0, sizeof(ctx));
	ctx.ctx_ms = ms;
	ctx.ctx_k = k;
	ctx.ctx_m = m;

	ctx.ctx_codec = ec_rs_create(k, m);
	if (!ctx.ctx_codec)
		return -ENOMEM;

	/* Open file on MDS. */
	ret = mds_file_open(ms, path, &ctx.ctx_file);
	if (ret)
		goto out_codec;

	/* Get layout. */
	ret = mds_layout_get(ms, &ctx.ctx_file, LAYOUTIOMODE4_RW,
			     &ctx.ctx_layout);
	if (ret)
		goto out_close;

	if (ctx.ctx_layout.el_nmirrors < (uint32_t)(k + m)) {
		fprintf(stderr, "ec_write: need %d mirrors, got %u\n", k + m,
			ctx.ctx_layout.el_nmirrors);
		ret = -EINVAL;
		goto out_layout;
	}

	/* Resolve device IDs → DS addresses, connect. */
	ret = ec_resolve_mirrors(&ctx);
	if (ret)
		goto out_layout;

	/*
	 * Pad data to a multiple of k * shard_size.  Each stripe
	 * encodes k shards of shard_size bytes.
	 */
	size_t shard_size = EC_SHARD_SIZE;
	size_t stripe_data = (size_t)k * shard_size;
	size_t padded_len =
		((data_len + stripe_data - 1) / stripe_data) * stripe_data;
	uint8_t *padded = calloc(1, padded_len);

	if (!padded) {
		ret = -ENOMEM;
		goto out_conns;
	}
	memcpy(padded, data, data_len);

	/* Allocate shard pointer arrays. */
	uint8_t **data_shards = calloc(k, sizeof(uint8_t *));
	uint8_t **parity_shards = calloc(m, sizeof(uint8_t *));

	if (!data_shards || !parity_shards) {
		free(data_shards);
		free(parity_shards);
		free(padded);
		ret = -ENOMEM;
		goto out_conns;
	}

	for (int i = 0; i < m; i++) {
		parity_shards[i] = calloc(1, shard_size);
		if (!parity_shards[i]) {
			for (int j = 0; j < i; j++)
				free(parity_shards[j]);
			free(data_shards);
			free(parity_shards);
			free(padded);
			ret = -ENOMEM;
			goto out_conns;
		}
	}

	/* Encode and write each stripe. */
	size_t nstripes = padded_len / stripe_data;

	for (size_t s = 0; s < nstripes; s++) {
		/* Point data shards into the padded buffer. */
		for (int i = 0; i < k; i++)
			data_shards[i] = padded + s * stripe_data +
					 (size_t)i * shard_size;

		ret = ctx.ctx_codec->ec_encode(ctx.ctx_codec, data_shards,
					       parity_shards, shard_size);
		if (ret)
			break;

		/* Write data shards to mirrors 0..k-1. */
		for (int i = 0; i < k; i++) {
			struct ec_mirror *em = &ctx.ctx_layout.el_mirrors[i];
			uint64_t off = s * shard_size;

			ret = ds_write(&ctx.ctx_conns[i], em->em_fh,
				       em->em_fh_len, off, data_shards[i],
				       shard_size);
			if (ret)
				break;
		}
		if (ret)
			break;

		/* Write parity shards to mirrors k..k+m-1. */
		for (int i = 0; i < m; i++) {
			struct ec_mirror *em =
				&ctx.ctx_layout.el_mirrors[k + i];
			uint64_t off = s * shard_size;

			ret = ds_write(&ctx.ctx_conns[k + i], em->em_fh,
				       em->em_fh_len, off, parity_shards[i],
				       shard_size);
			if (ret)
				break;
		}
		if (ret)
			break;
	}

	for (int i = 0; i < m; i++)
		free(parity_shards[i]);
	free(parity_shards);
	free(data_shards);
	free(padded);

out_conns:
	ec_disconnect_all(&ctx);
out_layout:
	mds_layout_return(ms, &ctx.ctx_file, &ctx.ctx_layout);
	ec_layout_free(&ctx.ctx_layout);
out_close:
	mds_file_close(ms, &ctx.ctx_file);
out_codec:
	ec_codec_destroy(ctx.ctx_codec);
	return ret;
}

/* ------------------------------------------------------------------ */
/* Read                                                                */
/* ------------------------------------------------------------------ */

int ec_read(struct mds_session *ms, const char *path, uint8_t *buf,
	    size_t buf_len, size_t *out_len, int k, int m)
{
	struct ec_context ctx;
	int ret;

	memset(&ctx, 0, sizeof(ctx));
	ctx.ctx_ms = ms;
	ctx.ctx_k = k;
	ctx.ctx_m = m;

	ctx.ctx_codec = ec_rs_create(k, m);
	if (!ctx.ctx_codec)
		return -ENOMEM;

	ret = mds_file_open(ms, path, &ctx.ctx_file);
	if (ret)
		goto out_codec;

	ret = mds_layout_get(ms, &ctx.ctx_file, LAYOUTIOMODE4_READ,
			     &ctx.ctx_layout);
	if (ret)
		goto out_close;

	if (ctx.ctx_layout.el_nmirrors < (uint32_t)(k + m)) {
		ret = -EINVAL;
		goto out_layout;
	}

	ret = ec_resolve_mirrors(&ctx);
	if (ret)
		goto out_layout;

	size_t shard_size = EC_SHARD_SIZE;
	size_t stripe_data = (size_t)k * shard_size;

	/*
	 * We don't know the original data length, so we read as many
	 * stripes as fit in buf_len (rounded up).
	 */
	size_t nstripes = (buf_len + stripe_data - 1) / stripe_data;

	/* Allocate shard buffers. */
	int total = k + m;
	uint8_t **shards = calloc(total, sizeof(uint8_t *));
	bool *present = calloc(total, sizeof(bool));

	if (!shards || !present) {
		free(shards);
		free(present);
		ret = -ENOMEM;
		goto out_conns;
	}

	for (int i = 0; i < total; i++) {
		shards[i] = calloc(1, shard_size);
		if (!shards[i]) {
			for (int j = 0; j < i; j++)
				free(shards[j]);
			free(shards);
			free(present);
			ret = -ENOMEM;
			goto out_conns;
		}
	}

	size_t total_read = 0;

	for (size_t s = 0; s < nstripes && total_read < buf_len; s++) {
		/* Read all k+m shards (tolerate failures up to m). */
		for (int i = 0; i < total; i++) {
			struct ec_mirror *em = &ctx.ctx_layout.el_mirrors[i];
			uint32_t nread = 0;
			uint64_t off = s * shard_size;

			ret = ds_read(&ctx.ctx_conns[i], em->em_fh,
				      em->em_fh_len, off, shards[i], shard_size,
				      &nread);
			present[i] = (ret == 0 && nread == shard_size);
		}

		/* RS-decode to reconstruct any missing shards. */
		ret = ctx.ctx_codec->ec_decode(ctx.ctx_codec, shards, present,
					       shard_size);
		if (ret)
			break;

		/* Copy data shards (0..k-1) to output buffer. */
		for (int i = 0; i < k && total_read < buf_len; i++) {
			size_t copy = shard_size;

			if (total_read + copy > buf_len)
				copy = buf_len - total_read;
			memcpy(buf + total_read, shards[i], copy);
			total_read += copy;
		}
	}

	for (int i = 0; i < total; i++)
		free(shards[i]);
	free(shards);
	free(present);

	if (ret == 0 && out_len)
		*out_len = total_read;

out_conns:
	ec_disconnect_all(&ctx);
out_layout:
	mds_layout_return(ms, &ctx.ctx_file, &ctx.ctx_layout);
	ec_layout_free(&ctx.ctx_layout);
out_close:
	mds_file_close(ms, &ctx.ctx_file);
out_codec:
	ec_codec_destroy(ctx.ctx_codec);
	return ret;
}
