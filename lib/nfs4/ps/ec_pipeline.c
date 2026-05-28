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
 *   3. For each stripe: RS-encode k data --> m parity, write k+m to DSes
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
#include "ps_local_addr.h"
#include "ps_state.h"
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

/*
 * EC_SHARD_SIZE_DEFAULT is exported by ec_client.h so callers that
 * want the historical 4 KiB benchmark geometry have a named
 * constant.  ec_write_codec / ec_read_codec take shard_size as a
 * parameter; only the back-compat ec_write / ec_read wrappers
 * below pin to the default.
 *
 * Per-RPC chunk cap on the plain (non-EC) path.  This is the
 * NFSv3 READ/WRITE round-trip size, not a stripe shard, and is
 * unrelated to the EC pipeline's shard_size knob.
 */
#define PLAIN_RPC_CHUNK (4 * 1024)

/* Ceiling division -- needed for chunk block counts with variable-size shards. */
#define DIV_CEIL(a, b) (((a) + (b) - 1) / (b))

/* ------------------------------------------------------------------ */
/* Resolve all mirrors to DS connections                                */
/* ------------------------------------------------------------------ */

struct ec_context {
	struct mds_session *ctx_ms;
	struct mds_file ctx_file;
	struct ec_layout ctx_layout;
	struct ec_device *ctx_devs;
	struct ds_conn *ctx_conns; /* NFSv3 DS connections (v1) */
	/*
	 * Array of pointers, one per mirror.  See the matching
	 * declaration in ec_pipeline_internal.h for the lock-step
	 * binary-layout contract and the dedup rationale.
	 */
	struct mds_session **ctx_ds_sess;
	struct ec_codec *ctx_codec;
	uint32_t ctx_k;
	uint32_t ctx_m;
	/*
	 * Phase 5 short-circuit: caller-provided PS listener state.
	 * NULL for ec_demo and standalone callers; non-NULL only on
	 * the PS pipeline path.  Threaded through ec_resolve_mirrors
	 * to ps_local_addr_match() so each mirror's em_local flag is
	 * set correctly.
	 */
	struct ps_listener_state *ctx_pls;
};

/*
 * Forward declaration so ec_resolve_mirrors' failure unwind can
 * reach the cleanup helper (defined below).
 */
static void ec_disconnect_all(struct ec_context *ctx);

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
		/*
		 * Array of pointers; each unique session is calloc'd
		 * below.  Combined-mode duplicates (multiple mirrors on
		 * the same DS host:port) share the same pointer so a
		 * single ms_call_mutex + slot_seqid serialises all per-
		 * mirror compounds on that NFSv4.1 session, instead of
		 * three independent counters aliasing into the server's
		 * DRC cache (which silently no-op'd writes/reads on the
		 * 2nd and 3rd mirror).
		 */
		ctx->ctx_ds_sess = calloc(n, sizeof(struct mds_session *));
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

	/*
	 * Mid-loop failures unwind through `out_err` so partially
	 * allocated unique sessions (slice 6: heap-allocated, dedup'd by
	 * pointer) and ctx_devs / ctx_conns get reclaimed.  Routing
	 * direct returns would leak every successful per-mirror
	 * mds_session_create that ran before the first failure --
	 * ASAN-visible, and the pre-slice-6 inline-struct shape had the
	 * same kind of leak with a smaller surface (one inline struct
	 * per mirror) until this fix.
	 */
	int ret = 0;

	for (uint32_t i = 0; i < n; i++) {
		struct ec_mirror *em = &ctx->ctx_layout.el_mirrors[i];

		ret = mds_getdeviceinfo(ctx->ctx_ms, em->em_deviceid,
					ctx->ctx_layout.el_layout_type,
					&ctx->ctx_devs[i]);
		if (ret)
			goto out_err;

		/*
		 * Propagate the tight-coupling flag from the device to the
		 * mirror.  When set, ds_chunk_write/read use the real layout
		 * stateid (TRUST_STATEID path) instead of the anonymous stateid.
		 */
		em->em_tight_coupled = ctx->ctx_devs[i].ed_tight_coupled;

		/*
		 * Phase 5 short-circuit decision.  Always runs the match
		 * call (it returns false on NULL pls so non-PS callers see
		 * a no-op).  Numeric-address compare against the listener's
		 * pls_local_addrs[] table -- DNS-free hot path; hostnames
		 * fall back to the RPC path because the match primitive
		 * uses AI_NUMERICHOST.
		 */
		em->em_local = ps_local_addr_match(ctx->ctx_pls,
						   ctx->ctx_devs[i].ed_host);

		if (use_v2) {
			/* NFSv4.2 session to each DS -- unique owner per mirror. */
			int existing = find_existing_conn(ctx, i);

			if (existing >= 0) {
				/*
				 * Pointer dedup: share the SAME session
				 * struct, so ms_call_mutex + ms_slot_seqid
				 * are also shared and the server sees a
				 * single monotonic seqid stream on this
				 * sessionid.
				 */
				ctx->ctx_ds_sess[i] =
					ctx->ctx_ds_sess[existing];
			} else {
				char ds_id[32];
				char host_arg[280];

				ctx->ctx_ds_sess[i] =
					calloc(1, sizeof(struct mds_session));
				if (!ctx->ctx_ds_sess[i]) {
					ret = -ENOMEM;
					goto out_err;
				}
				snprintf(ds_id, sizeof(ds_id), "ds%u-%u", i,
					 getpid());
				mds_session_set_owner(ctx->ctx_ds_sess[i],
						      ds_id);
				/*
				 * Pass "host:port" when the layout's uaddr
				 * encoded a non-zero port -- mds_session_clnt_open
				 * bypasses portmap and connects directly to
				 * <host>:<port>.  Required for cross-host setups
				 * where DSes pack onto a single host network with
				 * register_with_rpcbind=false (shadow's rpcbind
				 * has no NFS service registered there).
				 */
				if (ctx->ctx_devs[i].ed_port > 0)
					snprintf(host_arg, sizeof(host_arg),
						 "%s:%u",
						 ctx->ctx_devs[i].ed_host,
						 ctx->ctx_devs[i].ed_port);
				else
					snprintf(host_arg, sizeof(host_arg),
						 "%s",
						 ctx->ctx_devs[i].ed_host);
				ret = mds_session_create(ctx->ctx_ds_sess[i],
							 host_arg);
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
			goto out_err;
	}

	return 0;

out_err:
	/*
	 * ec_disconnect_all is NULL-safe per-slot and dedup-aware
	 * (slot j < i sharing a pointer with slot i owns the destroy),
	 * so it cleanly reclaims whatever was allocated before the
	 * failing iteration.  Also frees ctx_devs / ctx_conns.
	 */
	ec_disconnect_all(ctx);
	return ret;
}

/*
 * Report a DS I/O error to the MDS.  Best-effort -- failure to
 * send the error report is not itself an error.
 */
static void ec_report_ds_error(struct ec_context *ctx, int mirror_idx,
			       nfs_opnum4 opnum)
{
	mds_layout_error(ctx->ctx_ms, &ctx->ctx_file, &ctx->ctx_layout,
			 (uint32_t)mirror_idx, NFS4ERR_IO, opnum);
}

/*
 * ec_chunk_write -- CHUNK_WRITE with BAD_STATEID retry for tight coupling.
 *
 * When the DS returns NFS4ERR_BAD_STATEID (trust entry expired or revoked),
 * report LAYOUTERROR to the MDS and retry with exponential backoff.
 * After 3 failed attempts, return -ESTALE so the caller can LAYOUTRETURN.
 */
/*
 * Non-static so ec_pipeline_dispatch_test (lib/nfs4/ps/tests/)
 * can drive the Phase 5 short-circuit dispatch hook below
 * without standing up a full LAYOUTGET stack.  See
 * ec_pipeline_internal.h for the test-only declaration.
 */
int ec_chunk_write(struct ec_context *ctx, int mirror_idx,
		   uint64_t block_offset, uint32_t chunk_sz, const uint8_t *src,
		   uint32_t wsz, uint32_t owner_id)
{
	struct ec_mirror *em = &ctx->ctx_layout.el_mirrors[mirror_idx];
	const stateid4 *stid =
		em->em_tight_coupled ? &ctx->ctx_layout.el_stateid : NULL;

	/*
	 * Phase 5 short-circuit: when the mirror lives on this same PS,
	 * the byte offset for db_write is `block_offset * chunk_sz`
	 * (the same arithmetic the local DS CHUNK_WRITE handler runs
	 * before calling data_block_write).  owner_id and stid are
	 * unused on this path -- the local sb's i_db_rwlock already
	 * serialises concurrent writers; trust-stateid is the slice
	 * 5.4 follow-up.
	 *
	 * Indirected through pls_sc_write_fn so this TU (linked into
	 * libreffs_nfs4_ps.la and pulled in by ec_demo) does not
	 * statically reference ps_shortcircuit_write -- the install
	 * hook lives in libreffs_nfs4_ps_sb.la and only reffsd / the
	 * PS unit tests link that.
	 */
	if (em->em_local && ctx->ctx_pls && ctx->ctx_pls->pls_sc_write_fn) {
		uint64_t byte_off = block_offset * (uint64_t)chunk_sz;

		ps_listener_record_shortcircuit(ctx->ctx_pls);
		return ctx->ctx_pls->pls_sc_write_fn(em->em_fh, em->em_fh_len,
						     byte_off, src, wsz,
						     em->em_uid, em->em_gid,
						     stid);
	}

	for (int attempt = 0; attempt < 3; attempt++) {
		int ret;

		ret = ds_chunk_write(ctx->ctx_ds_sess[mirror_idx], em->em_fh,
				     em->em_fh_len, block_offset, chunk_sz, src,
				     wsz, owner_id, stid);
		if (ret != -ESTALE)
			return ret;

		/*
		 * DS rejected our layout stateid (trust entry expired or
		 * revoked).  Report the error to the MDS and retry after
		 * a brief delay.  50ms, 100ms, 200ms backoff.
		 */
		mds_layout_error(ctx->ctx_ms, &ctx->ctx_file, &ctx->ctx_layout,
				 (uint32_t)mirror_idx, NFS4ERR_BAD_STATEID,
				 OP_CHUNK_WRITE);
		struct timespec delay = { 0, (long)(50 * 1000000) << attempt };

#ifdef __APPLE__
		/* Darwin lacks clock_nanosleep(3).  For a relative sleep
		 * (flags=0) on any clock, nanosleep() is equivalent. */
		nanosleep(&delay, NULL);
#else
		clock_nanosleep(CLOCK_MONOTONIC, 0, &delay, NULL);
#endif
	}

	return -ESTALE;
}

/*
 * ec_chunk_read -- CHUNK_READ with BAD_STATEID retry for tight coupling.
 *
 * Non-static for the same dispatch-test reason as ec_chunk_write
 * above; see ec_pipeline_internal.h.
 */
int ec_chunk_read(struct ec_context *ctx, int mirror_idx, uint64_t block_offset,
		  uint32_t nblk, uint8_t *shard, uint32_t rd_chunk_sz,
		  uint32_t *nread)
{
	struct ec_mirror *em = &ctx->ctx_layout.el_mirrors[mirror_idx];
	const stateid4 *stid =
		em->em_tight_coupled ? &ctx->ctx_layout.el_stateid : NULL;

	/*
	 * Phase 5 short-circuit.  Mirrors the write-side hook:
	 * byte_offset = block_offset * rd_chunk_sz, request `nblk *
	 * rd_chunk_sz` bytes.  The helper sets *nread to whatever the
	 * local data block returned (zero for a sparse / missing
	 * region, matching the RPC path's behaviour).
	 */
	if (em->em_local && ctx->ctx_pls && ctx->ctx_pls->pls_sc_read_fn) {
		uint64_t byte_off = block_offset * (uint64_t)rd_chunk_sz;
		size_t buf_len = (size_t)nblk * (size_t)rd_chunk_sz;

		ps_listener_record_shortcircuit(ctx->ctx_pls);
		return ctx->ctx_pls->pls_sc_read_fn(em->em_fh, em->em_fh_len,
						    byte_off, buf_len, shard,
						    nread, em->em_uid,
						    em->em_gid, stid);
	}

	for (int attempt = 0; attempt < 3; attempt++) {
		int ret;

		ret = ds_chunk_read(ctx->ctx_ds_sess[mirror_idx], em->em_fh,
				    em->em_fh_len, block_offset, nblk, shard,
				    rd_chunk_sz, nread, stid);
		if (ret != -ESTALE)
			return ret;

		mds_layout_error(ctx->ctx_ms, &ctx->ctx_file, &ctx->ctx_layout,
				 (uint32_t)mirror_idx, NFS4ERR_BAD_STATEID,
				 OP_CHUNK_READ);
		struct timespec delay = { 0, (long)(50 * 1000000) << attempt };

#ifdef __APPLE__
		/* Darwin lacks clock_nanosleep(3).  For a relative sleep
		 * (flags=0) on any clock, nanosleep() is equivalent. */
		nanosleep(&delay, NULL);
#else
		clock_nanosleep(CLOCK_MONOTONIC, 0, &delay, NULL);
#endif
	}

	return -ESTALE;
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
	case EC_CODEC_STRIPE:
		return ec_stripe_create(k);
	case EC_CODEC_MIRROR:
		/*
		 * Wire-side: FFV2_ENCODING_MIRRORED, N replicas, no
		 * parity.  Encode replicates data[0] verbatim,
		 * decode picks any present shard.  The pipeline
		 * branches on codec_type == EC_CODEC_MIRROR in
		 * ec_write_codec_with_file / ec_read_codec_with_file
		 * to lay every data_shards[i] at the same stripe
		 * offset and to emit one shard's worth per stripe
		 * on output.
		 */
		return ec_mirror_create(k);
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

	/*
	 * Iterate backward: ec_resolve_mirrors shallow-copies a
	 * resolved entry to higher indexes when find_existing_conn()
	 * finds the same host:port at a lower index, so duplicates
	 * always live AFTER their canonical entry.  Walking backward
	 * means every alias's dup-check sees its canonical at a lower
	 * j (still alive, not yet destroyed) and skips; only the
	 * canonical -- which has no earlier match -- reaches the
	 * actual destroy at index 0.  A forward walk would NULL the
	 * canonical's clnt mid-loop and then the alias's dup-check
	 * against NULL would falsely succeed, calling
	 * clnt_destroy / mds_session_destroy a second time on the
	 * already-freed CLIENT (#60).
	 */
	if (ctx->ctx_ds_sess) {
		for (uint32_t i = n; i-- > 0;) {
			if (!ctx->ctx_ds_sess[i])
				continue;
			/*
			 * Pointer dedup: combined-mode mirrors share a
			 * single heap-allocated mds_session via pointer
			 * copy in ec_resolve_mirrors.  Destroy each
			 * unique session exactly once -- the slot at
			 * index j (j < i) that aliases us still owns
			 * the destroy, so skip here.
			 */
			bool dup = false;

			for (uint32_t j = 0; j < i; j++) {
				if (ctx->ctx_ds_sess[j] ==
				    ctx->ctx_ds_sess[i]) {
					dup = true;
					break;
				}
			}
			if (!dup) {
				mds_session_destroy(ctx->ctx_ds_sess[i]);
				free(ctx->ctx_ds_sess[i]);
			}
			ctx->ctx_ds_sess[i] = NULL;
		}
		free(ctx->ctx_ds_sess);
		ctx->ctx_ds_sess = NULL;
	}

	if (ctx->ctx_conns) {
		for (uint32_t i = n; i-- > 0;) {
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
		size_t data_len, layouttype4 layout_type)
{
	struct mds_file mf;
	struct ec_layout layout;
	int ret;

	ret = mds_file_open(ms, path, &mf);
	if (ret)
		return ret;

	ret = mds_layout_get(ms, &mf, LAYOUTIOMODE4_RW, layout_type, NULL,
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
	 * Write in PLAIN_RPC_CHUNK-sized RPCs.  This is the NFSv3
	 * write loop on a single mirror; unrelated to the EC
	 * pipeline's per-stripe shard_size.
	 */
	size_t off = 0;

	while (off < data_len) {
		uint32_t chunk = (uint32_t)(data_len - off);

		if (chunk > PLAIN_RPC_CHUNK)
			chunk = PLAIN_RPC_CHUNK;
		ret = ds_write(&dc, layout.el_mirrors[0].em_fh,
			       layout.el_mirrors[0].em_fh_len, (uint64_t)off,
			       data + off, chunk);
		if (ret)
			break;
		off += chunk;
	}

	ds_disconnect(&dc);
out_layout:
	mds_layout_return(ms, &mf, NULL, &layout);
	ec_layout_free(&layout);
out_close:
	mds_file_close(ms, &mf);
	return ret;
}

/* ------------------------------------------------------------------ */
/* Plain (non-EC) read                                                 */
/* ------------------------------------------------------------------ */

int plain_read(struct mds_session *ms, const char *path, uint8_t *buf,
	       size_t buf_len, size_t *out_len, layouttype4 layout_type)
{
	struct mds_file mf;
	struct ec_layout layout;
	int ret;

	ret = mds_file_open(ms, path, &mf);
	if (ret)
		return ret;

	ret = mds_layout_get(ms, &mf, LAYOUTIOMODE4_READ, layout_type, NULL,
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

	/* Read in PLAIN_RPC_CHUNK-sized RPCs (single-mirror plain path). */
	size_t total = 0;

	while (total < buf_len) {
		uint32_t want = (uint32_t)(buf_len - total);
		uint32_t nread = 0;

		if (want > PLAIN_RPC_CHUNK)
			want = PLAIN_RPC_CHUNK;
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
	mds_layout_return(ms, &mf, NULL, &layout);
	ec_layout_free(&layout);
out_close:
	mds_file_close(ms, &mf);
	return ret;
}

/*
 * Slice 1.6: outer retry on -ESTALE from ec_chunk_write/_read.
 *
 * ec_chunk_write retries NFS4ERR_BAD_STATEID three times with
 * 50/100/200 ms backoff and on exhaustion returns -ESTALE.  When
 * the trust entry has actually been revoked (slice 1.5 race
 * scenario), the inner retry alone cannot recover -- the stateid
 * stays revoked until the client gets a fresh layout from the
 * MDS.  ec_layout_refresh is the outer retry the inner path was
 * designed to feed into: free the stale layout, back off briefly,
 * re-LAYOUTGET, re-resolve mirrors, and let the caller retry the
 * failed stripe with the new stateid.  Bounded retry (3 attempts)
 * prevents a layout ping-pong with a competing writer; on
 * exhaustion the original -ESTALE propagates and the write fails
 * cleanly -- which is the slice 1.5 "loud failure" property,
 * preserved.
 */
#define EC_OUTER_RETRY_MAX 3

static int ec_layout_refresh(struct ec_context *ctx, struct mds_session *ms,
			     layouttype4 layout_type, layoutiomode4 iomode,
			     int attempt)
{
	struct timespec delay = { 0, (long)(100 * 1000000) << (attempt - 1) };
	int ret;

#ifdef __APPLE__
	nanosleep(&delay, NULL);
#else
	clock_nanosleep(CLOCK_MONOTONIC, 0, &delay, NULL);
#endif

	ec_layout_free(&ctx->ctx_layout);
	/*
	 * ec_layout_refresh is the recovery path inside ec_chunk_write
	 * / ec_chunk_read after the DS rejected an op with -ESTALE.
	 * It re-fetches the layout using the session's default auth
	 * (NULL creds) -- the original LAYOUTGET's caller-supplied
	 * creds are not plumbed down into the per-shard recovery loop
	 * yet.  Acceptable for now: the recovery path runs against the
	 * same MDS the original LAYOUTGET hit, so a different cred
	 * would not unblock anything we care about.  Threading the
	 * caller's creds through is NOT_NOW_BROWN_COW for the same
	 * follow-on slice that does DS-side cred forwarding.
	 */
	ret = mds_layout_get(ms, &ctx->ctx_file, iomode, layout_type, NULL,
			     &ctx->ctx_layout);
	if (ret) {
		ec_log("ec_layout_refresh: LAYOUTGET failed: %d\n", ret);
		return ret;
	}
	if (ctx->ctx_layout.el_nmirrors < (uint32_t)(ctx->ctx_k + ctx->ctx_m)) {
		ec_log("ec_layout_refresh: insufficient mirrors %u\n",
		       ctx->ctx_layout.el_nmirrors);
		return -EINVAL;
	}

	/*
	 * Reuse the existing DS sessions (and their slot-sequence-ID
	 * state) -- a fresh ec_resolve_mirrors would calloc-overwrite
	 * ctx_ds_sess and reset every session to seqid=1, while the
	 * server side still has sl_seqid at the value reached during
	 * the prior failed CHUNK_WRITEs.  Mismatch surfaces as
	 * NFS4ERR_SEQ_MISORDERED on the very first op of the retry's
	 * COMPOUND and the trust check never runs.
	 *
	 * The bench-stack assumption is that the DS pool is stable
	 * across LAYOUTGET retries -- mirrors[i].em_deviceid resolves
	 * to the same DS as before -- so reusing ctx_devs (and the
	 * sessions hung off it) is safe.  Just propagate
	 * em_tight_coupled onto the new layout's mirrors so the
	 * stateid choice in ec_chunk_write/_read uses the right path.
	 */
	for (uint32_t i = 0; i < ctx->ctx_layout.el_nmirrors; i++) {
		struct ec_mirror *em = &ctx->ctx_layout.el_mirrors[i];

		em->em_tight_coupled = ctx->ctx_devs[i].ed_tight_coupled;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* EC Write                                                            */
/* ------------------------------------------------------------------ */

int ec_write_codec_with_file(struct mds_session *ms, struct mds_file *mf,
			     const uint8_t *data, size_t data_len, int k, int m,
			     enum ec_codec_type codec_type,
			     layouttype4 layout_type, size_t shard_size,
			     const struct authunix_parms *creds,
			     struct ps_listener_state *pls)
{
	struct ec_context ctx;
	int ret;

	if (!mf) {
		ec_log("ec_write_with_file: NULL mds_file\n");
		return -EINVAL;
	}

	if (shard_size == 0 || (shard_size % sizeof(uint64_t)) != 0 ||
	    shard_size > EC_SHARD_SIZE_MAX) {
		/*
		 * Mojette grids index columns as uint64_t.  A non-multiple
		 * of 8 leaves a fractional column at the tail and breaks
		 * the codec.  RS uses bytes directly but the same
		 * constraint costs nothing.  The upper bound bounds
		 * stripe_data = k * shard_size against (size_t) overflow
		 * for caller-passed garbage.
		 */
		return -EINVAL;
	}

	/*
	 * MIRROR semantics inside this function: every data_shards[i]
	 * points at the SAME stripe offset and every mirror replica
	 * receives the same bytes via CHUNK_WRITE / ds_write.  The
	 * encoder (ec_mirror_create) is the no-op for the aliased-
	 * buffer case; the actual replication is the per-mirror fan-
	 * out loop below.  stripe_data and the data_shards stride
	 * pick up the per-codec branch immediately below.
	 */
	const bool is_mirror = (codec_type == EC_CODEC_MIRROR);

	memset(&ctx, 0, sizeof(ctx));
	ctx.ctx_ms = ms;
	ctx.ctx_k = k;
	ctx.ctx_m = m;
	ctx.ctx_pls = pls;
	ctx.ctx_file = *mf; /* caller owns the underlying mf; we shallow-copy */

	ctx.ctx_codec = ec_create_codec(k, m, codec_type);
	if (!ctx.ctx_codec)
		return -ENOMEM;

	ret = mds_layout_get(ms, &ctx.ctx_file, LAYOUTIOMODE4_RW, layout_type,
			     creds, &ctx.ctx_layout);
	if (ret) {
		ec_log("ec_write: LAYOUTGET failed: %d\n", ret);
		goto out_codec;
	}

	ec_log("ec_write: LAYOUTGET ok: %u mirrors, type=%u\n",
	       ctx.ctx_layout.el_nmirrors, ctx.ctx_layout.el_layout_type);

	if (ctx.ctx_layout.el_nmirrors < (uint32_t)(k + m)) {
		ec_log("ec_write: need %d mirrors, got %u\n", k + m,
		       ctx.ctx_layout.el_nmirrors);
		ret = -EINVAL;
		goto out_layout;
	}

	/* Resolve device IDs --> DS addresses, connect. */
	ret = ec_resolve_mirrors(&ctx);
	if (ret) {
		ec_log("ec_write: resolve_mirrors failed: %d\n", ret);
		goto out_layout;
	}
	ec_log("ec_write: resolved %u mirrors\n", ctx.ctx_layout.el_nmirrors);

	/*
	 * Pad data to a multiple of stripe_data.  For striping codecs
	 * each stripe encodes k shards of shard_size bytes, so
	 * stripe_data = k * shard_size.  For MIRROR each "stripe" is
	 * one chunk's worth of bytes replicated to N data servers, so
	 * stripe_data = shard_size.
	 */
	size_t stripe_data = is_mirror ? shard_size : (size_t)k * shard_size;
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
		int outer_retry = 0;

retry_stripe:
		/*
		 * Point data shards into the padded buffer.  Striping
		 * codecs (RS / Mojette / STRIPE) give each shard a
		 * disjoint shard_size slice of the stripe.  MIRROR
		 * points every shard at the same shard_size slice;
		 * the replication happens when we fan out the writes
		 * to N mirrors below.
		 */
		for (int i = 0; i < k; i++)
			data_shards[i] =
				padded + s * stripe_data +
				(is_mirror ? 0 : (size_t)i * shard_size);

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
				ret = ec_chunk_write(&ctx, i,
						     (uint64_t)s *
							     DIV_CEIL(ds_stride,
								      chunk_sz),
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
		if (ret == -ESTALE && outer_retry < EC_OUTER_RETRY_MAX) {
			outer_retry++;
			ec_log("ec_write: stripe %zu data STALE, "
			       "outer retry %d/%d (re-LAYOUTGET)\n",
			       s, outer_retry, EC_OUTER_RETRY_MAX);
			ret = ec_layout_refresh(&ctx, ms, layout_type,
						LAYOUTIOMODE4_RW, outer_retry);
			if (ret == 0)
				goto retry_stripe;
			ec_log("ec_write: stripe %zu refresh failed: %d, "
			       "giving up\n",
			       s, ret);
			ret = -ESTALE;
			break;
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
				ret = ec_chunk_write(
					&ctx, k + i,
					(uint64_t)s *
						DIV_CEIL(ds_stride, chunk_sz),
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
		if (ret == -ESTALE && outer_retry < EC_OUTER_RETRY_MAX) {
			outer_retry++;
			ec_log("ec_write: stripe %zu parity STALE, "
			       "outer retry %d/%d (re-LAYOUTGET)\n",
			       s, outer_retry, EC_OUTER_RETRY_MAX);
			ret = ec_layout_refresh(&ctx, ms, layout_type,
						LAYOUTIOMODE4_RW, outer_retry);
			if (ret == 0)
				goto retry_stripe;
			ec_log("ec_write: stripe %zu refresh failed: %d, "
			       "giving up\n",
			       s, ret);
			ret = -ESTALE;
			break;
		}
		if (ret)
			break;
	}

	/* FINALIZE + COMMIT for CHUNK ops (v2). */
	if (ret == 0 && ctx.ctx_ds_sess) {
		/*
		 * Per-stripe block stride is DIV_CEIL(ds_stride, chunk_sz)
		 * (matches the write-side offset arithmetic at lines 656 +
		 * 689).  This range covers the *file space* the parity-
		 * largest shard occupies; sparse-writing shards (data
		 * shards under Mojette systematic, which write 1 block
		 * per stride; parity[0] which writes 3 of 4) leave holes
		 * within this range.  chunk_store_transition skips
		 * EMPTY blocks so the FINALIZE/COMMIT is correct for
		 * sparse layouts as well as RS's contiguous layout.
		 */
		uint32_t total_blocks =
			(uint32_t)(nstripes * DIV_CEIL(ds_stride, chunk_sz));

		for (int i = 0; i < k + m && ret == 0; i++) {
			struct ec_mirror *em = &ctx.ctx_layout.el_mirrors[i];

			ret = ds_chunk_finalize(ctx.ctx_ds_sess[i], em->em_fh,
						em->em_fh_len, 0, total_blocks,
						1);
		}
		for (int i = 0; i < k + m && ret == 0; i++) {
			struct ec_mirror *em = &ctx.ctx_layout.el_mirrors[i];

			ret = ds_chunk_commit(ctx.ctx_ds_sess[i], em->em_fh,
					      em->em_fh_len, 0, total_blocks, 1,
					      NULL);
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
	mds_layout_return(ms, &ctx.ctx_file, creds, &ctx.ctx_layout);
	ec_layout_free(&ctx.ctx_layout);
out_codec:
	ec_codec_destroy(ctx.ctx_codec);
	return ret;
}

/*
 * Per-stripe write primitive (PS Phase 4b slice 4b.2).
 *
 * Encodes and writes exactly one fully-dirty stripe at file-level
 * stripe number `stripe_no` using its own LAYOUTGET / FINALIZE /
 * COMMIT / LAYOUTRETURN cycle.  The DS-side block offset is
 * derived from `stripe_no` so two concurrent PSes writing
 * disjoint stripes of the same upstream file do not stomp on each
 * other's bytes -- that is the 4a multi-writer-shared-file fix.
 *
 * Caller MUST pass exactly k * shard_size bytes in
 * stripe_bytes -- partial-stripe RMW is the next slice (4b.3) and
 * lives outside this primitive.  The structure intentionally
 * mirrors ec_write_codec_with_file's per-stripe inner loop body
 * to keep the failure modes (encode error, DS write error,
 * NFS4ERR_BAD_STATEID retry via ec_chunk_write, ESTALE outer
 * retry via ec_layout_refresh) identical.  A refactor that
 * factors the shared inner loop into a helper is tracked in the
 * 4b.8 wrap-up; for now the duplication is contained.
 */
int ec_write_stripe_with_file(struct mds_session *ms, struct mds_file *mf,
			      uint64_t stripe_no, const uint8_t *stripe_bytes,
			      size_t stripe_len, int k, int m,
			      enum ec_codec_type codec_type,
			      layouttype4 layout_type, size_t shard_size,
			      const struct authunix_parms *creds,
			      uint8_t mds_verf_out[8], bool *mds_verf_set_out,
			      struct ps_listener_state *pls)
{
	struct ec_context ctx;
	uint8_t **data_shards = NULL;
	uint8_t **parity_shards = NULL;
	uint8_t **enc_data = NULL;
	bool nonsys;
	uint32_t chunk_sz;
	size_t ds_stride;
	int outer_retry = 0;
	int ret;

	if (!mf || !stripe_bytes) {
		ec_log("ec_write_stripe: NULL mf / stripe_bytes\n");
		return -EINVAL;
	}

	if (shard_size == 0 || (shard_size % sizeof(uint64_t)) != 0 ||
	    shard_size > EC_SHARD_SIZE_MAX) {
		/*
		 * Same constraint as ec_write_codec_with_file: Mojette
		 * grids index columns as uint64_t and the upper bound
		 * defends against caller-passed garbage when computing
		 * stripe_data = k * shard_size.
		 */
		return -EINVAL;
	}

	if (k <= 0 || m <= 0)
		return -EINVAL;
	if (stripe_len != (size_t)k * shard_size) {
		/*
		 * The dirty-bitmap walker only invokes this for entries
		 * with pds_partial_mask == NULL, which means every data
		 * shard is present.  A mismatch is a programmer error
		 * upstream -- the buffer's geometry diverged from what
		 * the caller passed.
		 */
		ec_log("ec_write_stripe: stripe_len %zu != k*shard %zu\n",
		       stripe_len, (size_t)k * shard_size);
		return -EINVAL;
	}

	memset(&ctx, 0, sizeof(ctx));
	ctx.ctx_ms = ms;
	ctx.ctx_k = k;
	ctx.ctx_m = m;
	ctx.ctx_pls = pls;
	ctx.ctx_file = *mf; /* shallow copy; caller owns mf */

	ctx.ctx_codec = ec_create_codec(k, m, codec_type);
	if (!ctx.ctx_codec)
		return -ENOMEM;

	ret = mds_layout_get(ms, &ctx.ctx_file, LAYOUTIOMODE4_RW, layout_type,
			     creds, &ctx.ctx_layout);
	if (ret) {
		ec_log("ec_write_stripe: LAYOUTGET failed: %d\n", ret);
		goto out_codec;
	}

	if (ctx.ctx_layout.el_nmirrors < (uint32_t)(k + m)) {
		ec_log("ec_write_stripe: need %d mirrors, got %u\n", k + m,
		       ctx.ctx_layout.el_nmirrors);
		ret = -EINVAL;
		goto out_layout;
	}

	ret = ec_resolve_mirrors(&ctx);
	if (ret) {
		ec_log("ec_write_stripe: resolve_mirrors failed: %d\n", ret);
		goto out_layout;
	}

	data_shards = calloc(k, sizeof(uint8_t *));
	parity_shards = calloc(m, sizeof(uint8_t *));
	if (!data_shards || !parity_shards) {
		ret = -ENOMEM;
		goto out_shards;
	}

	for (int i = 0; i < m; i++) {
		size_t psz = shard_write_size(ctx.ctx_codec, k + i, shard_size);

		parity_shards[i] = calloc(1, psz);
		if (!parity_shards[i]) {
			ret = -ENOMEM;
			goto out_shards;
		}
	}

	nonsys = (codec_type == EC_CODEC_MOJETTE_NONSYS);
	if (nonsys) {
		enc_data = calloc(k, sizeof(uint8_t *));
		if (!enc_data) {
			ret = -ENOMEM;
			goto out_shards;
		}
		for (int i = 0; i < k; i++) {
			size_t dsz =
				shard_write_size(ctx.ctx_codec, i, shard_size);

			enc_data[i] = calloc(1, dsz);
			if (!enc_data[i]) {
				ret = -ENOMEM;
				goto out_shards;
			}
		}
	}

	chunk_sz = ctx.ctx_layout.el_chunk_size;
	if (chunk_sz == 0)
		chunk_sz = (uint32_t)shard_size;

	ds_stride = shard_size;
	for (int i = 0; i < k + m; i++) {
		size_t sz = shard_write_size(ctx.ctx_codec, i, shard_size);

		if (sz > ds_stride)
			ds_stride = sz;
	}

retry_stripe:
	/* Point data shards into stripe_bytes (the encode call only
	 * reads from data_shards; the nonsys copy below provides a
	 * separate mutable buffer for codecs that overwrite their input).
	 */
	for (int i = 0; i < k; i++)
		data_shards[i] =
			(uint8_t *)stripe_bytes + (size_t)i * shard_size;

	if (nonsys) {
		for (int i = 0; i < k; i++)
			memcpy(enc_data[i], data_shards[i], shard_size);
	}

	ret = ctx.ctx_codec->ec_encode(ctx.ctx_codec,
				       nonsys ? enc_data : data_shards,
				       parity_shards, shard_size);
	if (ret) {
		ec_log("ec_write_stripe: encode failed: %d\n", ret);
		goto out_shards;
	}

	/* Write data shards to mirrors 0..k-1 at the stripe's DS offset. */
	for (int i = 0; i < k; i++) {
		struct ec_mirror *em = &ctx.ctx_layout.el_mirrors[i];
		uint32_t wsz = (uint32_t)shard_write_size(ctx.ctx_codec, i,
							  shard_size);
		uint8_t *src = nonsys ? enc_data[i] : data_shards[i];

		if (ctx.ctx_ds_sess) {
			ret = ec_chunk_write(&ctx, i,
					     stripe_no * DIV_CEIL(ds_stride,
								  chunk_sz),
					     chunk_sz, src, wsz, 1);
		} else {
			ret = ds_write(&ctx.ctx_conns[i], em->em_fh,
				       em->em_fh_len, stripe_no * ds_stride,
				       src, wsz);
		}
		if (ret) {
			ec_log("ec_write_stripe: stripe %llu data[%d] "
			       "FAILED: %d\n",
			       (unsigned long long)stripe_no, i, ret);
			ec_report_ds_error(&ctx, i, OP_WRITE);
			break;
		}
	}
	if (ret == -ESTALE && outer_retry < EC_OUTER_RETRY_MAX) {
		outer_retry++;
		ec_log("ec_write_stripe: stripe %llu data STALE, "
		       "outer retry %d/%d (re-LAYOUTGET)\n",
		       (unsigned long long)stripe_no, outer_retry,
		       EC_OUTER_RETRY_MAX);
		ret = ec_layout_refresh(&ctx, ms, layout_type, LAYOUTIOMODE4_RW,
					outer_retry);
		if (ret == 0)
			goto retry_stripe;
		ret = -ESTALE;
		goto out_shards;
	}
	if (ret)
		goto out_shards;

	/* Write parity shards to mirrors k..k+m-1. */
	for (int i = 0; i < m; i++) {
		struct ec_mirror *em = &ctx.ctx_layout.el_mirrors[k + i];
		uint32_t wsz = (uint32_t)shard_write_size(ctx.ctx_codec, k + i,
							  shard_size);

		if (ctx.ctx_ds_sess) {
			ret = ec_chunk_write(
				&ctx, k + i,
				stripe_no * DIV_CEIL(ds_stride, chunk_sz),
				chunk_sz, parity_shards[i], wsz, 1);
		} else {
			ret = ds_write(&ctx.ctx_conns[k + i], em->em_fh,
				       em->em_fh_len, stripe_no * ds_stride,
				       parity_shards[i], wsz);
		}
		if (ret) {
			ec_log("ec_write_stripe: stripe %llu parity[%d] "
			       "FAILED: %d\n",
			       (unsigned long long)stripe_no, i, ret);
			ec_report_ds_error(&ctx, k + i, OP_WRITE);
			break;
		}
	}
	if (ret == -ESTALE && outer_retry < EC_OUTER_RETRY_MAX) {
		outer_retry++;
		ec_log("ec_write_stripe: stripe %llu parity STALE, "
		       "outer retry %d/%d (re-LAYOUTGET)\n",
		       (unsigned long long)stripe_no, outer_retry,
		       EC_OUTER_RETRY_MAX);
		ret = ec_layout_refresh(&ctx, ms, layout_type, LAYOUTIOMODE4_RW,
					outer_retry);
		if (ret == 0)
			goto retry_stripe;
		ret = -ESTALE;
		goto out_shards;
	}
	if (ret)
		goto out_shards;

	/*
	 * FINALIZE + COMMIT for just this stripe's blocks (v2 only).
	 * blocks_per_stripe matches ec_write_codec_with_file's
	 * per-stripe block stride; the [base_block, base_block +
	 * blocks_per_stripe) range covers the file space the parity-
	 * largest shard occupies, with sparse codecs leaving holes
	 * that chunk_store_transition skips.
	 */
	if (ctx.ctx_ds_sess) {
		uint32_t blocks_per_stripe =
			(uint32_t)DIV_CEIL(ds_stride, chunk_sz);
		uint64_t base_block = stripe_no * (uint64_t)blocks_per_stripe;
		uint8_t first_verf[8];
		bool captured_verf = false;

		for (int i = 0; i < k + m && ret == 0; i++) {
			struct ec_mirror *em = &ctx.ctx_layout.el_mirrors[i];

			ret = ds_chunk_finalize(ctx.ctx_ds_sess[i], em->em_fh,
						em->em_fh_len, base_block,
						blocks_per_stripe, 1);
		}
		for (int i = 0; i < k + m && ret == 0; i++) {
			struct ec_mirror *em = &ctx.ctx_layout.el_mirrors[i];
			/*
			 * Capture the writeverf from mirror 0 only (PS
			 * Phase 4b slice 4b.4).  The loop exits on the
			 * first error (ret != 0 in the for-condition);
			 * if mirror 0 errors no verifier is captured.
			 * Otherwise mirror 0's writeverf is the sample
			 * we keep and we pass NULL for mirrors
			 * 1..k+m-1 to skip the per-call copy.  One
			 * verifier per stripe-flush is sufficient:
			 * monotonic per upstream boot epoch, fed by
			 * 4b.6's FILE_SYNC4 inline flush path into the
			 * WRITE-reply composer (Risk #7 in proxy-
			 * server-phase4b.md).
			 */
			ret = ds_chunk_commit(
				ctx.ctx_ds_sess[i], em->em_fh, em->em_fh_len,
				base_block, blocks_per_stripe, 1,
				captured_verf ? NULL : first_verf);
			if (ret == 0 && !captured_verf)
				captured_verf = true;
		}

		if (ret == 0 && captured_verf && mds_verf_out &&
		    mds_verf_set_out) {
			memcpy(mds_verf_out, first_verf, 8);
			*mds_verf_set_out = true;
		}
	}

out_shards:
	if (enc_data) {
		for (int i = 0; i < k; i++)
			free(enc_data[i]);
		free(enc_data);
	}
	if (parity_shards) {
		for (int i = 0; i < m; i++)
			free(parity_shards[i]);
		free(parity_shards);
	}
	free(data_shards);
	ec_disconnect_all(&ctx);
out_layout:
	mds_layout_return(ms, &ctx.ctx_file, creds, &ctx.ctx_layout);
	ec_layout_free(&ctx.ctx_layout);
out_codec:
	ec_codec_destroy(ctx.ctx_codec);
	return ret;
}

int ec_read_stripe_with_file(struct mds_session *ms, struct mds_file *mf,
			     uint64_t stripe_no, uint8_t *stripe_bytes,
			     size_t stripe_len, int k, int m,
			     enum ec_codec_type codec_type,
			     layouttype4 layout_type, size_t shard_size,
			     const struct authunix_parms *creds,
			     struct ps_listener_state *pls)
{
	struct ec_context ctx;
	uint8_t **shards = NULL;
	bool *present = NULL;
	uint32_t rd_chunk_sz;
	size_t ds_stride;
	int total;
	int outer_retry = 0;
	int ret;

	if (!mf || !stripe_bytes) {
		ec_log("ec_read_stripe: NULL mf / stripe_bytes\n");
		return -EINVAL;
	}

	if (k <= 0 || m <= 0)
		return -EINVAL;

	if (shard_size == 0 || (shard_size % sizeof(uint64_t)) != 0 ||
	    shard_size > EC_SHARD_SIZE_MAX)
		return -EINVAL;

	if (stripe_len != (size_t)k * shard_size) {
		/*
		 * Same defensive contract as ec_write_stripe_with_file: the
		 * caller MUST allocate exactly k*shard_size for the k data
		 * shards.  A mismatch is a programmer error in the RMW
		 * walker upstream -- the buffer's geometry diverged from
		 * what the caller passed.
		 */
		ec_log("ec_read_stripe: stripe_len %zu != k*shard %zu\n",
		       stripe_len, (size_t)k * shard_size);
		return -EINVAL;
	}

	memset(&ctx, 0, sizeof(ctx));
	ctx.ctx_ms = ms;
	ctx.ctx_k = k;
	ctx.ctx_m = m;
	ctx.ctx_pls = pls;
	ctx.ctx_file = *mf; /* shallow copy; caller owns mf */

	ctx.ctx_codec = ec_create_codec(k, m, codec_type);
	if (!ctx.ctx_codec)
		return -ENOMEM;

	ret = mds_layout_get(ms, &ctx.ctx_file, LAYOUTIOMODE4_READ, layout_type,
			     creds, &ctx.ctx_layout);
	if (ret) {
		ec_log("ec_read_stripe: LAYOUTGET failed: %d\n", ret);
		goto out_codec;
	}

	if (ctx.ctx_layout.el_nmirrors < (uint32_t)(k + m)) {
		ec_log("ec_read_stripe: need %d mirrors, got %u\n", k + m,
		       ctx.ctx_layout.el_nmirrors);
		ret = -EINVAL;
		goto out_layout;
	}

	ret = ec_resolve_mirrors(&ctx);
	if (ret) {
		ec_log("ec_read_stripe: resolve_mirrors failed: %d\n", ret);
		goto out_layout;
	}

	rd_chunk_sz = ctx.ctx_layout.el_chunk_size;
	if (rd_chunk_sz == 0)
		rd_chunk_sz = (uint32_t)shard_size;

	total = k + m;
	shards = calloc(total, sizeof(uint8_t *));
	present = calloc(total, sizeof(bool));
	if (!shards || !present) {
		ret = -ENOMEM;
		goto out_shards;
	}

	for (int i = 0; i < total; i++) {
		size_t sz = shard_write_size(ctx.ctx_codec, i, shard_size);

		/*
		 * v2 CHUNK reads return whole blocks; round up to a chunk
		 * boundary like ec_read_codec_with_file does.
		 */
		if (ctx.ctx_ds_sess)
			sz = DIV_CEIL(sz, rd_chunk_sz) * rd_chunk_sz;

		shards[i] = calloc(1, sz);
		if (!shards[i]) {
			ret = -ENOMEM;
			goto out_shards;
		}
	}

	/* DS offset stride matches the write side. */
	ds_stride = shard_size;
	for (int i = 0; i < total; i++) {
		size_t sz = shard_write_size(ctx.ctx_codec, i, shard_size);

		if (sz > ds_stride)
			ds_stride = sz;
	}

retry_stripe_read: {
	bool stale_seen = false;

	for (int i = 0; i < total; i++) {
		struct ec_mirror *em = &ctx.ctx_layout.el_mirrors[i];
		uint32_t nread = 0;
		uint32_t rsz = (uint32_t)shard_write_size(ctx.ctx_codec, i,
							  shard_size);
		uint32_t err_opnum = OP_READ;

		if (ctx.ctx_ds_sess) {
			uint32_t nblk = DIV_CEIL(rsz, rd_chunk_sz);

			err_opnum = OP_CHUNK_READ;
			ret = ec_chunk_read(
				&ctx, i,
				stripe_no * DIV_CEIL(ds_stride, rd_chunk_sz),
				nblk, shards[i], rd_chunk_sz, &nread);
			present[i] = (ret == 0 && nread == nblk);
		} else {
			ret = ds_read(&ctx.ctx_conns[i], em->em_fh,
				      em->em_fh_len, stripe_no * ds_stride,
				      shards[i], rsz, &nread);
			present[i] = (ret == 0 && nread == rsz);
		}
		if (ret == -ESTALE)
			stale_seen = true;
		if (!present[i])
			ec_report_ds_error(&ctx, i, err_opnum);
	}

	ret = ctx.ctx_codec->ec_decode(ctx.ctx_codec, shards, present,
				       shard_size);
	if (ret && stale_seen && outer_retry < EC_OUTER_RETRY_MAX) {
		outer_retry++;
		ec_log("ec_read_stripe: stripe %llu decode failed "
		       "with stale shards, outer retry %d/%d "
		       "(re-LAYOUTGET)\n",
		       (unsigned long long)stripe_no, outer_retry,
		       EC_OUTER_RETRY_MAX);
		ret = ec_layout_refresh(&ctx, ms, layout_type,
					LAYOUTIOMODE4_READ, outer_retry);
		if (ret == 0)
			goto retry_stripe_read;
		ec_log("ec_read_stripe: stripe %llu refresh failed: "
		       "%d\n",
		       (unsigned long long)stripe_no, ret);
		ret = -ESTALE;
		goto out_shards;
	}
	if (ret) {
		ec_log("ec_read_stripe: stripe %llu decode failed: %d\n",
		       (unsigned long long)stripe_no, ret);
		goto out_shards;
	}
}

	/* Copy the k reconstructed data shards into stripe_bytes. */
	for (int i = 0; i < k; i++)
		memcpy(stripe_bytes + (size_t)i * shard_size, shards[i],
		       shard_size);

out_shards:
	if (shards) {
		for (int i = 0; i < total; i++)
			free(shards[i]);
		free(shards);
	}
	free(present);
	ec_disconnect_all(&ctx);
out_layout:
	mds_layout_return(ms, &ctx.ctx_file, creds, &ctx.ctx_layout);
	ec_layout_free(&ctx.ctx_layout);
out_codec:
	ec_codec_destroy(ctx.ctx_codec);
	return ret;
}

int ec_write_codec(struct mds_session *ms, const char *path,
		   const uint8_t *data, size_t data_len, int k, int m,
		   enum ec_codec_type codec_type, layouttype4 layout_type,
		   size_t shard_size)
{
	struct mds_file mf;
	int ret;

	ret = mds_file_open(ms, path, &mf);
	if (ret) {
		ec_log("ec_write: OPEN failed: %d\n", ret);
		return ret;
	}

	/*
	 * Back-compat wrapper for ec_demo / direct callers: no per-call
	 * cred override, the session's default auth is used.  PS callers
	 * go through ec_write_codec_with_file directly so they can pass
	 * the end client's creds.  Mirror of ec_read_codec at the bottom
	 * of this file.
	 */
	ret = ec_write_codec_with_file(ms, &mf, data, data_len, k, m,
				       codec_type, layout_type, shard_size,
				       NULL, NULL);

	mds_file_close(ms, &mf);
	return ret;
}

/* ------------------------------------------------------------------ */
/* Read                                                                */
/* ------------------------------------------------------------------ */

int ec_read_codec_with_file(struct mds_session *ms, struct mds_file *mf,
			    uint8_t *buf, size_t buf_len, size_t *out_len,
			    int k, int m, enum ec_codec_type codec_type,
			    layouttype4 layout_type, uint64_t skip_ds_mask,
			    size_t shard_size,
			    const struct authunix_parms *creds,
			    struct ps_listener_state *pls)
{
	struct ec_context ctx;
	int ret;
	/* Count only bits in the valid range [0, k+m). */
	uint64_t valid_mask = (k + m < 64) ? (1ULL << (k + m)) - 1 : ~0ULL;
	int nskip = __builtin_popcountll(skip_ds_mask & valid_mask);

	if (!mf) {
		ec_log("ec_read_with_file: NULL mds_file\n");
		return -EINVAL;
	}

	/*
	 * Loss tolerance is codec-shaped:
	 *   - MIRROR replicates data[0] across all k mirrors; any single
	 *     surviving shard reconstructs.  Tolerate up to k-1 losses.
	 *   - Every other codec is erasure-coded (RS / Mojette / STRIPE)
	 *     and follows the usual "tolerate at most m losses" rule.
	 * Using one rule for both shapes (the old `nskip > m` gate)
	 * rejected legitimate degraded reads on MIRROR layouts where
	 * m == 0 by construction.
	 */
	int max_loss = (codec_type == EC_CODEC_MIRROR) ? (k - 1) : m;

	if (nskip > max_loss) {
		ec_log("ec_read: skip_ds_mask has %d bits set, "
		       "need <= %d for codec=%d (k=%d, m=%d)\n",
		       nskip, max_loss, (int)codec_type, k, m);
		return -EINVAL;
	}

	ec_log("ec_read: codec=%d k=%d m=%d shard_size=%zu skip_mask=0x%llx nskip=%d\n",
	       (int)codec_type, k, m, shard_size,
	       (unsigned long long)skip_ds_mask, nskip);

	if (shard_size == 0 || (shard_size % sizeof(uint64_t)) != 0 ||
	    shard_size > EC_SHARD_SIZE_MAX)
		return -EINVAL;

	/*
	 * MIRROR semantics: every replica holds the same chunk
	 * payload at the same file offset.  The pipeline reads from
	 * every present replica and the mirror_decode picks any one
	 * to populate the missing slots; the output-copy step
	 * downstream collapses the N identical shards to a single
	 * stripe-sized run in `buf`.
	 */
	const bool is_mirror = (codec_type == EC_CODEC_MIRROR);

	memset(&ctx, 0, sizeof(ctx));
	ctx.ctx_ms = ms;
	ctx.ctx_k = k;
	ctx.ctx_m = m;
	ctx.ctx_pls = pls;
	ctx.ctx_file = *mf; /* caller owns the underlying mf; we shallow-copy */

	ctx.ctx_codec = ec_create_codec(k, m, codec_type);
	if (!ctx.ctx_codec)
		return -ENOMEM;

	ret = mds_layout_get(ms, &ctx.ctx_file, LAYOUTIOMODE4_READ, layout_type,
			     creds, &ctx.ctx_layout);
	if (ret)
		goto out_codec;

	if (ctx.ctx_layout.el_nmirrors < (uint32_t)(k + m)) {
		ret = -EINVAL;
		goto out_layout;
	}

	ret = ec_resolve_mirrors(&ctx);
	if (ret)
		goto out_layout;

	size_t stripe_data = is_mirror ? shard_size : (size_t)k * shard_size;

	/*
	 * We don't know the original data length, so we read as many
	 * stripes as fit in buf_len (rounded up).  For MIRROR each
	 * stripe is one chunk worth of bytes; for striping codecs
	 * each stripe is k * shard_size bytes split across k shards.
	 */
	size_t nstripes = (buf_len + stripe_data - 1) / stripe_data;

	uint32_t rd_chunk_sz = ctx.ctx_layout.el_chunk_size;

	if (rd_chunk_sz == 0)
		rd_chunk_sz = (uint32_t)shard_size;

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

		/*
		 * For v2 CHUNK reads, the server returns whole blocks,
		 * so the buffer must be rounded up to a chunk boundary.
		 */
		if (ctx.ctx_ds_sess)
			sz = DIV_CEIL(sz, rd_chunk_sz) * rd_chunk_sz;

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

	/* Match the write path's offset stride for variable-size shards. */
	size_t ds_stride = shard_size;

	for (int i = 0; i < total; i++) {
		size_t sz = shard_write_size(ctx.ctx_codec, i, shard_size);

		if (sz > ds_stride)
			ds_stride = sz;
	}

	for (size_t s = 0; s < nstripes && total_read < buf_len; s++) {
		int outer_retry = 0;
		bool stale_seen;

retry_stripe_read:
		stale_seen = false;
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

			uint32_t err_opnum = OP_READ;

			if (ctx.ctx_ds_sess) {
				uint32_t nblk = DIV_CEIL(rsz, rd_chunk_sz);

				err_opnum = OP_CHUNK_READ;
				ret = ec_chunk_read(
					&ctx, i,
					(uint64_t)s * DIV_CEIL(ds_stride,
							       rd_chunk_sz),
					nblk, shards[i], rd_chunk_sz, &nread);
				present[i] = (ret == 0 && nread == nblk);
				ec_log("ec_read: stripe %zu shard[%d] CHUNK_READ rsz=%u nblk=%u rd_chunk_sz=%u ret=%d nread=%u present=%d\n",
				       s, i, rsz, nblk, rd_chunk_sz, ret, nread,
				       (int)present[i]);
			} else {
				ret = ds_read(&ctx.ctx_conns[i], em->em_fh,
					      em->em_fh_len, s * ds_stride,
					      shards[i], rsz, &nread);
				present[i] = (ret == 0 && nread == rsz);
				ec_log("ec_read: stripe %zu shard[%d] ds_read rsz=%u ret=%d nread=%u present=%d\n",
				       s, i, rsz, ret, nread, (int)present[i]);
			}
			if (ret == -ESTALE)
				stale_seen = true;
			if (!present[i])
				ec_report_ds_error(&ctx, i, err_opnum);
		}

		/* RS-decode to reconstruct any missing shards. */
		ret = ctx.ctx_codec->ec_decode(ctx.ctx_codec, shards, present,
					       shard_size);
		ec_log("ec_read: stripe %zu ec_decode ret=%d\n", s, ret);
		if (ret && stale_seen && outer_retry < EC_OUTER_RETRY_MAX) {
			outer_retry++;
			ec_log("ec_read: stripe %zu decode failed with stale "
			       "shards, outer retry %d/%d (re-LAYOUTGET)\n",
			       s, outer_retry, EC_OUTER_RETRY_MAX);
			ret = ec_layout_refresh(&ctx, ms, layout_type,
						LAYOUTIOMODE4_READ,
						outer_retry);
			if (ret == 0)
				goto retry_stripe_read;
			ec_log("ec_read: stripe %zu refresh failed: %d, "
			       "giving up\n",
			       s, ret);
			ret = -ESTALE;
			break;
		}
		if (ret)
			break;

		if (is_mirror) {
			/*
			 * Every replica's shard now holds the same
			 * stripe bytes (post-decode).  Emit one stripe
			 * worth (shard_size) to the output and move on.
			 */
			size_t copy = shard_size;

			if (total_read + copy > buf_len)
				copy = buf_len - total_read;
			memcpy(buf + total_read, shards[0], copy);
			total_read += copy;
		} else {
			/* Copy data shards (0..k-1) to output buffer. */
			for (int i = 0; i < k && total_read < buf_len; i++) {
				size_t copy = shard_size;

				if (total_read + copy > buf_len)
					copy = buf_len - total_read;
				memcpy(buf + total_read, shards[i], copy);
				total_read += copy;
			}
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
	mds_layout_return(ms, &ctx.ctx_file, creds, &ctx.ctx_layout);
	ec_layout_free(&ctx.ctx_layout);
out_codec:
	ec_codec_destroy(ctx.ctx_codec);
	return ret;
}

int ec_read_codec(struct mds_session *ms, const char *path, uint8_t *buf,
		  size_t buf_len, size_t *out_len, int k, int m,
		  enum ec_codec_type codec_type, layouttype4 layout_type,
		  uint64_t skip_ds_mask, size_t shard_size)
{
	struct mds_file mf;
	int ret;

	ret = mds_file_open(ms, path, &mf);
	if (ret)
		return ret;

	/*
	 * Back-compat wrapper for ec_demo / direct callers: no per-call
	 * cred override, the session's default auth is used.  PS callers
	 * go through ec_read_codec_with_file directly so they can pass
	 * the end client's creds.
	 */
	ret = ec_read_codec_with_file(ms, &mf, buf, buf_len, out_len, k, m,
				      codec_type, layout_type, skip_ds_mask,
				      shard_size, NULL, NULL);

	mds_file_close(ms, &mf);
	return ret;
}

/* ------------------------------------------------------------------ */
/* Partial-range I/O -- chunk-collision Track 1b                       */
/* ------------------------------------------------------------------ */

/*
 * Helper: compute the overlap of stripe `s` (covering
 * [s*stripe_data, (s+1)*stripe_data) in the file) with the
 * requested range [offset, offset+length).  Outputs are byte
 * positions within the stripe and within the caller's data
 * buffer; *sub_len is the count of bytes the caller cares about
 * in stripe `s`.  Always non-zero for any stripe in
 * [first_stripe, last_stripe].
 */
static void range_stripe_overlap(uint64_t s, size_t stripe_data,
				 uint64_t offset, size_t length,
				 size_t *sub_off_in_stripe,
				 size_t *sub_off_in_data, size_t *sub_len)
{
	uint64_t stripe_base = s * (uint64_t)stripe_data;
	uint64_t stripe_end = stripe_base + (uint64_t)stripe_data;
	uint64_t range_end = offset + (uint64_t)length;
	uint64_t sub_start = stripe_base > offset ? stripe_base : offset;
	uint64_t sub_endx = stripe_end < range_end ? stripe_end : range_end;

	*sub_off_in_stripe = (size_t)(sub_start - stripe_base);
	*sub_off_in_data = (size_t)(sub_start - offset);
	*sub_len = (size_t)(sub_endx - sub_start);
}

int ec_write_codec_range(struct mds_session *ms, const char *path,
			 const uint8_t *data, size_t length, uint64_t offset,
			 int k, int m, enum ec_codec_type codec_type,
			 layouttype4 layout_type, size_t shard_size)
{
	struct mds_file mf;
	uint8_t *scratch = NULL;
	size_t stripe_data;
	uint64_t end;
	uint64_t first_stripe;
	uint64_t last_stripe;
	int ret;

	if (length == 0)
		return 0;
	if (k < 1 || m < 0 || shard_size == 0 ||
	    (shard_size % sizeof(uint64_t)) != 0 ||
	    shard_size > EC_SHARD_SIZE_MAX)
		return -EINVAL;

	stripe_data = (size_t)k * shard_size;
	if (stripe_data / shard_size != (size_t)k)
		return -EINVAL; /* size_t overflow */
	if (length > UINT64_MAX - offset)
		return -EINVAL;
	end = offset + (uint64_t)length;

	first_stripe = offset / (uint64_t)stripe_data;
	last_stripe = (end - 1) / (uint64_t)stripe_data;

	ret = mds_file_open(ms, path, &mf);
	if (ret) {
		ec_log("ec_write_range: OPEN failed: %d\n", ret);
		return ret;
	}

	scratch = malloc(stripe_data);
	if (!scratch) {
		ret = -ENOMEM;
		goto out_close;
	}

	for (uint64_t s = first_stripe; s <= last_stripe; s++) {
		size_t sub_off_stripe;
		size_t sub_off_data;
		size_t sub_len;
		const uint8_t *write_payload;

		range_stripe_overlap(s, stripe_data, offset, length,
				     &sub_off_stripe, &sub_off_data, &sub_len);

		if (sub_off_stripe == 0 && sub_len == stripe_data) {
			/*
			 * Fully-dirty stripe -- the caller's data covers
			 * every byte of this stripe.  Hand straight to
			 * the per-stripe primitive; no RMW prefix read.
			 */
			write_payload = data + sub_off_data;
		} else {
			/*
			 * Partial stripe -- read the existing stripe,
			 * overwrite the dirty range in place, write the
			 * merged result.  Sparse RMW (file shorter than
			 * the stripe end) is NOT_NOW_BROWN_COW per
			 * ec_read_stripe_with_file's contract -- the
			 * harness pre-fills the file with a full-file
			 * write before any range writers start.
			 */
			ret = ec_read_stripe_with_file(ms, &mf, s, scratch,
						       stripe_data, k, m,
						       codec_type, layout_type,
						       shard_size, NULL, NULL);
			if (ret) {
				ec_log("ec_write_range: RMW read stripe "
				       "%llu failed: %d\n",
				       (unsigned long long)s, ret);
				goto out_free;
			}
			memcpy(scratch + sub_off_stripe, data + sub_off_data,
			       sub_len);
			write_payload = scratch;
		}

		ret = ec_write_stripe_with_file(ms, &mf, s, write_payload,
						stripe_data, k, m, codec_type,
						layout_type, shard_size, NULL,
						NULL, NULL, NULL);
		if (ret) {
			ec_log("ec_write_range: write stripe %llu failed: "
			       "%d\n",
			       (unsigned long long)s, ret);
			goto out_free;
		}
	}

out_free:
	free(scratch);
out_close:
	mds_file_close(ms, &mf);
	return ret;
}

int ec_read_codec_range(struct mds_session *ms, const char *path, uint8_t *buf,
			size_t length, uint64_t offset, int k, int m,
			enum ec_codec_type codec_type, layouttype4 layout_type,
			size_t shard_size)
{
	struct mds_file mf;
	uint8_t *scratch = NULL;
	size_t stripe_data;
	uint64_t end;
	uint64_t first_stripe;
	uint64_t last_stripe;
	int ret;

	/*
	 * skip_ds_mask is intentionally absent.  Track 1b's harness only
	 * uses healthy reads to assert the post-write state of each
	 * writer's range; degraded-read tolerance is the
	 * full-file ec_read_codec's concern.
	 * ec_read_stripe_with_file does not expose a per-stripe skip
	 * mask, so adding one here would be misleading.
	 */

	if (length == 0)
		return 0;
	if (k < 1 || m < 0 || shard_size == 0 ||
	    (shard_size % sizeof(uint64_t)) != 0 ||
	    shard_size > EC_SHARD_SIZE_MAX)
		return -EINVAL;

	stripe_data = (size_t)k * shard_size;
	if (stripe_data / shard_size != (size_t)k)
		return -EINVAL;
	if (length > UINT64_MAX - offset)
		return -EINVAL;
	end = offset + (uint64_t)length;

	first_stripe = offset / (uint64_t)stripe_data;
	last_stripe = (end - 1) / (uint64_t)stripe_data;

	ret = mds_file_open(ms, path, &mf);
	if (ret) {
		ec_log("ec_read_range: OPEN failed: %d\n", ret);
		return ret;
	}

	scratch = malloc(stripe_data);
	if (!scratch) {
		ret = -ENOMEM;
		goto out_close;
	}

	for (uint64_t s = first_stripe; s <= last_stripe; s++) {
		size_t sub_off_stripe;
		size_t sub_off_data;
		size_t sub_len;

		range_stripe_overlap(s, stripe_data, offset, length,
				     &sub_off_stripe, &sub_off_data, &sub_len);

		ret = ec_read_stripe_with_file(ms, &mf, s, scratch, stripe_data,
					       k, m, codec_type, layout_type,
					       shard_size, NULL, NULL);
		if (ret) {
			ec_log("ec_read_range: read stripe %llu failed: "
			       "%d\n",
			       (unsigned long long)s, ret);
			goto out_free;
		}

		memcpy(buf + sub_off_data, scratch + sub_off_stripe, sub_len);
	}

out_free:
	free(scratch);
out_close:
	mds_file_close(ms, &mf);
	return ret;
}

/* ------------------------------------------------------------------ */
/* Backward-compatible wrappers (default to Reed-Solomon)              */
/* ------------------------------------------------------------------ */

int ec_write(struct mds_session *ms, const char *path, const uint8_t *data,
	     size_t data_len, int k, int m)
{
	return ec_write_codec(ms, path, data, data_len, k, m, EC_CODEC_RS,
			      LAYOUT4_FLEX_FILES, EC_SHARD_SIZE_DEFAULT);
}

int ec_read(struct mds_session *ms, const char *path, uint8_t *buf,
	    size_t buf_len, size_t *out_len, int k, int m)
{
	return ec_read_codec(ms, path, buf, buf_len, out_len, k, m, EC_CODEC_RS,
			     LAYOUT4_FLEX_FILES, 0, EC_SHARD_SIZE_DEFAULT);
}
