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
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nfsv42_xdr.h"
#include "ec_client.h"
#include "reffs/ec.h"

#include <time.h>

__attribute__((format(printf, 1, 2))) static void ec_log(const char *fmt, ...)
{
	struct timespec ts;
	va_list ap;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	fprintf(stderr, "[%ld.%03ld] ", (long)ts.tv_sec % 1000,
		ts.tv_nsec / 1000000);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

/* Shard size: 4 KiB (capped for io_uring large-message workaround). */
#define EC_SHARD_SIZE (4 * 1024)

/* ------------------------------------------------------------------ */
/* Resolve all mirrors to DS connections                                */
/* ------------------------------------------------------------------ */

struct ec_context {
	struct mds_session *ctx_ms;
	struct mds_file ctx_file;
	struct ec_layout ctx_layout;
	struct ec_device *ctx_devs;
	struct ds_conn *ctx_conns; /* NFSv3 DS connections (v1) */
	struct mds_session *ctx_ds_sess; /* NFSv4.2 DS sessions (v2) */
	struct ec_codec *ctx_codec;
	uint32_t ctx_k;
	uint32_t ctx_m;
};

/*
 * Find an existing connection to the same DS host, or return -1.
 */
static int find_existing_conn(struct ec_context *ctx, uint32_t idx)
{
	for (uint32_t j = 0; j < idx; j++) {
		if (strcmp(ctx->ctx_devs[j].ed_host,
			   ctx->ctx_devs[idx].ed_host) == 0 &&
		    ctx->ctx_devs[j].ed_port == ctx->ctx_devs[idx].ed_port)
			return (int)j;
	}
	return -1;
}

static int ec_resolve_mirrors(struct ec_context *ctx)
{
	uint32_t n = ctx->ctx_layout.el_nmirrors;
	bool use_v2 = (ctx->ctx_layout.el_layout_type == LAYOUT4_FLEX_FILES_V2);

	ctx->ctx_devs = calloc(n, sizeof(struct ec_device));
	if (!ctx->ctx_devs)
		return -ENOMEM;

	if (use_v2) {
		ctx->ctx_ds_sess = calloc(n, sizeof(struct mds_session));
		if (!ctx->ctx_ds_sess) {
			free(ctx->ctx_devs);
			ctx->ctx_devs = NULL;
			return -ENOMEM;
		}
	} else {
		ctx->ctx_conns = calloc(n, sizeof(struct ds_conn));
		if (!ctx->ctx_conns) {
			free(ctx->ctx_devs);
			ctx->ctx_devs = NULL;
			return -ENOMEM;
		}
	}

	for (uint32_t i = 0; i < n; i++) {
		struct ec_mirror *em = &ctx->ctx_layout.el_mirrors[i];
		int ret;

		ret = mds_getdeviceinfo(ctx->ctx_ms, em->em_deviceid,
					ctx->ctx_layout.el_layout_type,
					&ctx->ctx_devs[i]);
		if (ret)
			return ret;

		if (use_v2) {
			/* NFSv4.2 session to each DS — unique owner per mirror. */
			int existing = find_existing_conn(ctx, i);

			if (existing >= 0) {
				ctx->ctx_ds_sess[i] =
					ctx->ctx_ds_sess[existing];
			} else {
				char ds_id[32];

				snprintf(ds_id, sizeof(ds_id), "ds%u-%u", i,
					 getpid());
				mds_session_set_owner(&ctx->ctx_ds_sess[i],
						      ds_id);
				ret = mds_session_create(
					&ctx->ctx_ds_sess[i],
					ctx->ctx_devs[i].ed_host);
			}
		} else {
			/*
			 * NFSv3: share one connection per unique DS.
			 * TIRPC's clnt_create may reuse/conflict when
			 * multiple CLIENTs target the same host:port.
			 */
			int existing = find_existing_conn(ctx, i);

			if (existing >= 0) {
				ctx->ctx_conns[i] = ctx->ctx_conns[existing];
			} else {
				ret = ds_connect(&ctx->ctx_conns[i],
						 &ctx->ctx_devs[i], em->em_uid,
						 em->em_gid);
			}
		}
		if (ret)
			return ret;
	}

	return 0;
}

/*
 * Report a DS I/O error to the MDS.  Best-effort — failure to
 * send the error report is not itself an error.
 */
static void ec_report_ds_error(struct ec_context *ctx, int mirror_idx,
			       nfs_opnum4 opnum)
{
	mds_layout_error(ctx->ctx_ms, &ctx->ctx_file, &ctx->ctx_layout,
			 (uint32_t)mirror_idx, NFS4ERR_IO, opnum);
}

static struct ec_codec *ec_create_codec(int k, int m,
					enum ec_codec_type codec_type)
{
	switch (codec_type) {
	case EC_CODEC_RS:
		return ec_rs_create(k, m);
	case EC_CODEC_MOJETTE_SYS:
		return ec_mojette_sys_create(k, m);
	case EC_CODEC_MOJETTE_NONSYS:
		return ec_mojette_nonsys_create(k, m);
	default:
		return NULL;
	}
}

/*
 * Return the write size for shard i.  For codecs with variable shard
 * sizes (Mojette non-systematic), parity shards may be larger than
 * data shards.
 */
static size_t shard_write_size(struct ec_codec *codec, int shard_idx,
			       size_t data_shard_len)
{
	if (codec->ec_shard_size)
		return codec->ec_shard_size(codec, shard_idx, data_shard_len);
	return data_shard_len;
}

static void ec_disconnect_all(struct ec_context *ctx)
{
	uint32_t n = ctx->ctx_layout.el_nmirrors;

	if (ctx->ctx_ds_sess) {
		for (uint32_t i = 0; i < n; i++) {
			/* Skip duplicates (shared sessions). */
			bool dup = false;

			for (uint32_t j = 0; j < i; j++) {
				if (ctx->ctx_ds_sess[j].ms_clnt ==
				    ctx->ctx_ds_sess[i].ms_clnt) {
					dup = true;
					break;
				}
			}
			if (!dup)
				mds_session_destroy(&ctx->ctx_ds_sess[i]);
		}
		free(ctx->ctx_ds_sess);
		ctx->ctx_ds_sess = NULL;
	}

	if (ctx->ctx_conns) {
		for (uint32_t i = 0; i < n; i++) {
			bool dup = false;

			for (uint32_t j = 0; j < i; j++) {
				if (ctx->ctx_conns[j].dc_clnt ==
				    ctx->ctx_conns[i].dc_clnt) {
					dup = true;
					break;
				}
			}
			if (!dup)
				ds_disconnect(&ctx->ctx_conns[i]);
		}
		free(ctx->ctx_conns);
		ctx->ctx_conns = NULL;
	}

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

	ret = mds_layout_get(ms, &mf, LAYOUTIOMODE4_RW, LAYOUT4_FLEX_FILES,
			     &layout);
	if (ret)
		goto out_close;

	if (layout.el_nmirrors < 1) {
		ret = -EINVAL;
		goto out_layout;
	}

	/* Resolve the first mirror. */
	struct ec_device dev;

	ret = mds_getdeviceinfo(ms, layout.el_mirrors[0].em_deviceid,
				layout.el_layout_type, &dev);
	if (ret)
		goto out_layout;

	struct ds_conn dc;

	/* DS auth uses synthetic uid/gid from the layout (fencing creds). */
	ret = ds_connect(&dc, &dev, layout.el_mirrors[0].em_uid,
			 layout.el_mirrors[0].em_gid);
	if (ret)
		goto out_layout;

	/*
	 * Write in EC_SHARD_SIZE chunks.  The io_uring read pipeline
	 * stalls on NFSv3 RPCs larger than ~32KB; cap at shard size.
	 */
	size_t off = 0;

	while (off < data_len) {
		uint32_t chunk = (uint32_t)(data_len - off);

		if (chunk > EC_SHARD_SIZE)
			chunk = EC_SHARD_SIZE;
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

	ret = mds_layout_get(ms, &mf, LAYOUTIOMODE4_READ, LAYOUT4_FLEX_FILES,
			     &layout);
	if (ret)
		goto out_close;

	if (layout.el_nmirrors < 1) {
		ret = -EINVAL;
		goto out_layout;
	}

	struct ec_device dev;

	ret = mds_getdeviceinfo(ms, layout.el_mirrors[0].em_deviceid,
				layout.el_layout_type, &dev);
	if (ret)
		goto out_layout;

	struct ds_conn dc;

	/* DS auth uses synthetic uid/gid from the layout (fencing creds). */
	ret = ds_connect(&dc, &dev, layout.el_mirrors[0].em_uid,
			 layout.el_mirrors[0].em_gid);
	if (ret)
		goto out_layout;

	/* Read in EC_SHARD_SIZE chunks (io_uring large message workaround). */
	size_t total = 0;

	while (total < buf_len) {
		uint32_t want = (uint32_t)(buf_len - total);
		uint32_t nread = 0;

		if (want > EC_SHARD_SIZE)
			want = EC_SHARD_SIZE;
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

int ec_write_codec(struct mds_session *ms, const char *path,
		   const uint8_t *data, size_t data_len, int k, int m,
		   enum ec_codec_type codec_type, layouttype4 layout_type)
{
	struct ec_context ctx;
	int ret;

	memset(&ctx, 0, sizeof(ctx));
	ctx.ctx_ms = ms;
	ctx.ctx_k = k;
	ctx.ctx_m = m;

	ctx.ctx_codec = ec_create_codec(k, m, codec_type);
	if (!ctx.ctx_codec)
		return -ENOMEM;

	/* Open file on MDS. */
	ret = mds_file_open(ms, path, &ctx.ctx_file);
	if (ret) {
		ec_log("ec_write: OPEN failed: %d\n", ret);
		goto out_codec;
	}

	ret = mds_layout_get(ms, &ctx.ctx_file, LAYOUTIOMODE4_RW, layout_type,
			     &ctx.ctx_layout);
	if (ret) {
		ec_log("ec_write: LAYOUTGET failed: %d\n", ret);
		goto out_close;
	}

	ec_log("ec_write: LAYOUTGET ok: %u mirrors, type=%u\n",
	       ctx.ctx_layout.el_nmirrors, ctx.ctx_layout.el_layout_type);

	if (ctx.ctx_layout.el_nmirrors < (uint32_t)(k + m)) {
		ec_log("ec_write: need %d mirrors, got %u\n", k + m,
		       ctx.ctx_layout.el_nmirrors);
		ret = -EINVAL;
		goto out_layout;
	}

	/* Resolve device IDs → DS addresses, connect. */
	ret = ec_resolve_mirrors(&ctx);
	if (ret) {
		ec_log("ec_write: resolve_mirrors failed: %d\n", ret);
		goto out_layout;
	}
	ec_log("ec_write: resolved %u mirrors\n", ctx.ctx_layout.el_nmirrors);

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
		size_t psz = shard_write_size(ctx.ctx_codec, k + i, shard_size);

		parity_shards[i] = calloc(1, psz);
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

	/*
	 * For non-systematic codecs, encode overwrites data[].
	 * Allocate separate buffers so we don't corrupt padded[].
	 */
	bool nonsys = (codec_type == EC_CODEC_MOJETTE_NONSYS);
	uint8_t **enc_data = data_shards;

	if (nonsys) {
		enc_data = calloc(k, sizeof(uint8_t *));
		if (!enc_data) {
			ret = -ENOMEM;
			goto out_parity;
		}
		for (int i = 0; i < k; i++) {
			size_t dsz =
				shard_write_size(ctx.ctx_codec, i, shard_size);

			enc_data[i] = calloc(1, dsz);
			if (!enc_data[i]) {
				for (int j = 0; j < i; j++)
					free(enc_data[j]);
				free(enc_data);
				ret = -ENOMEM;
				goto out_parity;
			}
		}
	}

	/* Encode and write each stripe. */
	size_t nstripes = padded_len / stripe_data;
	uint32_t chunk_sz = ctx.ctx_layout.el_chunk_size;

	if (chunk_sz == 0)
		chunk_sz = (uint32_t)shard_size;

	/*
	 * DS file offset stride per stripe.  For codecs with variable
	 * shard sizes (Mojette non-systematic), the largest projection
	 * determines the stride so that consecutive stripes don't
	 * overlap on disk.
	 */
	size_t ds_stride = shard_size;

	for (int i = 0; i < k + m; i++) {
		size_t sz = shard_write_size(ctx.ctx_codec, i, shard_size);

		if (sz > ds_stride)
			ds_stride = sz;
	}

	for (size_t s = 0; s < nstripes; s++) {
		/* Point data shards into the padded buffer. */
		for (int i = 0; i < k; i++)
			data_shards[i] = padded + s * stripe_data +
					 (size_t)i * shard_size;

		if (nonsys) {
			for (int i = 0; i < k; i++)
				memcpy(enc_data[i], data_shards[i], shard_size);
		}

		ret = ctx.ctx_codec->ec_encode(ctx.ctx_codec,
					       nonsys ? enc_data : data_shards,
					       parity_shards, shard_size);
		if (ret) {
			ec_log("ec_write: encode failed: %d\n", ret);
			break;
		}

		/* Write data shards to mirrors 0..k-1. */
		for (int i = 0; i < k; i++) {
			struct ec_mirror *em = &ctx.ctx_layout.el_mirrors[i];
			uint32_t wsz = (uint32_t)shard_write_size(
				ctx.ctx_codec, i, shard_size);
			uint8_t *src = nonsys ? enc_data[i] : data_shards[i];

			ec_log("ec_write: stripe %zu data[%d] "
			       "fh_len=%u wsz=%u\n",
			       s, i, em->em_fh_len, wsz);
			if (ctx.ctx_ds_sess) {
				ret = ds_chunk_write(
					&ctx.ctx_ds_sess[i], em->em_fh,
					em->em_fh_len,
					(uint64_t)s * (ds_stride / chunk_sz),
					chunk_sz, src, wsz, 1);
			} else {
				ret = ds_write(&ctx.ctx_conns[i], em->em_fh,
					       em->em_fh_len, s * ds_stride,
					       src, wsz);
			}
			if (ret) {
				ec_log("ec_write: data[%d] FAILED: %d\n", i,
				       ret);
				ec_report_ds_error(&ctx, i, OP_WRITE);
				break;
			}
			ec_log("ec_write: data[%d] ok\n", i);
		}
		if (ret)
			break;

		/* Write parity shards to mirrors k..k+m-1. */
		for (int i = 0; i < m; i++) {
			struct ec_mirror *em =
				&ctx.ctx_layout.el_mirrors[k + i];
			uint32_t wsz = (uint32_t)shard_write_size(
				ctx.ctx_codec, k + i, shard_size);

			ec_log("ec_write: stripe %zu parity[%d] "
			       "fh_len=%u wsz=%u\n",
			       s, i, em->em_fh_len, wsz);
			if (ctx.ctx_ds_sess) {
				ret = ds_chunk_write(
					&ctx.ctx_ds_sess[k + i], em->em_fh,
					em->em_fh_len,
					(uint64_t)s * (ds_stride / chunk_sz),
					chunk_sz, parity_shards[i], wsz, 1);
			} else {
				ret = ds_write(&ctx.ctx_conns[k + i], em->em_fh,
					       em->em_fh_len, s * ds_stride,
					       parity_shards[i], wsz);
			}
			if (ret) {
				ec_log("ec_write: parity[%d] FAILED: %d\n", i,
				       ret);
				ec_report_ds_error(&ctx, k + i, OP_WRITE);
				break;
			}
			ec_log("ec_write: parity[%d] ok\n", i);
		}
		if (ret)
			break;
	}

	/* FINALIZE + COMMIT for CHUNK ops (v2). */
	if (ret == 0 && ctx.ctx_ds_sess) {
		uint32_t total_blocks =
			(uint32_t)(nstripes * (ds_stride / chunk_sz));

		for (int i = 0; i < k + m && ret == 0; i++) {
			struct ec_mirror *em = &ctx.ctx_layout.el_mirrors[i];

			ret = ds_chunk_finalize(&ctx.ctx_ds_sess[i], em->em_fh,
						em->em_fh_len, 0, total_blocks,
						1);
		}
		for (int i = 0; i < k + m && ret == 0; i++) {
			struct ec_mirror *em = &ctx.ctx_layout.el_mirrors[i];

			ret = ds_chunk_commit(&ctx.ctx_ds_sess[i], em->em_fh,
					      em->em_fh_len, 0, total_blocks,
					      1);
		}
	}

	if (nonsys) {
		for (int i = 0; i < k; i++)
			free(enc_data[i]);
		free(enc_data);
	}

out_parity:
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

int ec_read_codec(struct mds_session *ms, const char *path, uint8_t *buf,
		  size_t buf_len, size_t *out_len, int k, int m,
		  enum ec_codec_type codec_type, layouttype4 layout_type,
		  uint64_t skip_ds_mask)
{
	struct ec_context ctx;
	int ret;
	/* Count only bits in the valid range [0, k+m). */
	uint64_t valid_mask = (k + m < 64) ? (1ULL << (k + m)) - 1 : ~0ULL;
	int nskip = __builtin_popcountll(skip_ds_mask & valid_mask);

	if (nskip >= m) {
		ec_log("ec_read: skip_ds_mask has %d bits set, need < m=%d\n",
		       nskip, m);
		return -EINVAL;
	}

	memset(&ctx, 0, sizeof(ctx));
	ctx.ctx_ms = ms;
	ctx.ctx_k = k;
	ctx.ctx_m = m;

	ctx.ctx_codec = ec_create_codec(k, m, codec_type);
	if (!ctx.ctx_codec)
		return -ENOMEM;

	ret = mds_file_open(ms, path, &ctx.ctx_file);
	if (ret)
		goto out_codec;

	ret = mds_layout_get(ms, &ctx.ctx_file, LAYOUTIOMODE4_READ, layout_type,
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
		size_t sz = shard_write_size(ctx.ctx_codec, i, shard_size);

		shards[i] = calloc(1, sz);
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

	uint32_t rd_chunk_sz = ctx.ctx_layout.el_chunk_size;

	if (rd_chunk_sz == 0)
		rd_chunk_sz = (uint32_t)shard_size;

	/* Match the write path's offset stride for variable-size shards. */
	size_t ds_stride = shard_size;

	for (int i = 0; i < total; i++) {
		size_t sz = shard_write_size(ctx.ctx_codec, i, shard_size);

		if (sz > ds_stride)
			ds_stride = sz;
	}

	for (size_t s = 0; s < nstripes && total_read < buf_len; s++) {
		/* Read all k+m shards (tolerate failures up to m). */
		for (int i = 0; i < total; i++) {
			struct ec_mirror *em = &ctx.ctx_layout.el_mirrors[i];
			uint32_t nread = 0;
			uint32_t rsz = (uint32_t)shard_write_size(
				ctx.ctx_codec, i, shard_size);

			if (skip_ds_mask & (1ULL << i)) {
				present[i] = false;
				continue;
			}

			if (ctx.ctx_ds_sess) {
				uint32_t nblk = rsz / rd_chunk_sz;

				ret = ds_chunk_read(
					&ctx.ctx_ds_sess[i], em->em_fh,
					em->em_fh_len,
					(uint64_t)s * (ds_stride / rd_chunk_sz),
					nblk, shards[i], rd_chunk_sz, &nread);
				present[i] = (ret == 0 && nread == nblk);
			} else {
				ret = ds_read(&ctx.ctx_conns[i], em->em_fh,
					      em->em_fh_len, s * ds_stride,
					      shards[i], rsz, &nread);
				present[i] = (ret == 0 && nread == rsz);
			}
			if (!present[i])
				ec_report_ds_error(&ctx, i, OP_READ);
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

/* ------------------------------------------------------------------ */
/* Backward-compatible wrappers (default to Reed-Solomon)              */
/* ------------------------------------------------------------------ */

int ec_write(struct mds_session *ms, const char *path, const uint8_t *data,
	     size_t data_len, int k, int m)
{
	return ec_write_codec(ms, path, data, data_len, k, m, EC_CODEC_RS,
			      LAYOUT4_FLEX_FILES);
}

int ec_read(struct mds_session *ms, const char *path, uint8_t *buf,
	    size_t buf_len, size_t *out_len, int k, int m)
{
	return ec_read_codec(ms, path, buf, buf_len, out_len, k, m, EC_CODEC_RS,
			     LAYOUT4_FLEX_FILES, 0);
}
