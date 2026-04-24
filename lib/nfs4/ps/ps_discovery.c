/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nfsv42_xdr.h"

#include "ec_client.h"
#include "ps_discovery.h"
#include "ps_mount_client.h"
#include "ps_state.h"

int ps_discovery_fetch_root_fh(struct mds_session *ms, uint8_t *fh_buf,
			       uint32_t buf_size, uint32_t *fh_len_out)
{
	struct mds_compound mc;
	nfs_argop4 *slot;
	nfs_resop4 *res;
	int ret;

	if (!ms || !fh_buf || !fh_len_out)
		return -EINVAL;

	/* SEQUENCE + PUTROOTFH + GETFH = 3 ops */
	ret = mds_compound_init(&mc, 3, "ps-discover-root");
	if (ret)
		return ret;

	ret = mds_compound_add_sequence(&mc, ms);
	if (ret)
		goto out;

	slot = mds_compound_add_op(&mc, OP_PUTROOTFH);
	if (!slot) {
		ret = -ENOSPC;
		goto out;
	}

	slot = mds_compound_add_op(&mc, OP_GETFH);
	if (!slot) {
		ret = -ENOSPC;
		goto out;
	}

	ret = mds_compound_send(&mc, ms);
	if (ret)
		goto out;

	/* PUTROOTFH status (op index 1) */
	res = mds_compound_result(&mc, 1);
	if (!res || res->nfs_resop4_u.opputrootfh.status != NFS4_OK) {
		ret = -EREMOTEIO;
		goto out;
	}

	/* GETFH result (op index 2) */
	res = mds_compound_result(&mc, 2);
	if (!res || res->nfs_resop4_u.opgetfh.status != NFS4_OK) {
		ret = -EREMOTEIO;
		goto out;
	}

	GETFH4resok *fhresok = &res->nfs_resop4_u.opgetfh.GETFH4res_u.resok4;

	/*
	 * Zero-length FH would be a broken MDS response (no meaningful
	 * anchor) and guards against memcpy(dst, NULL, 0) UB if the XDR
	 * decoder left nfs_fh4_val NULL on a 0-length read.
	 */
	if (fhresok->object.nfs_fh4_len == 0) {
		ret = -EREMOTEIO;
		goto out;
	}
	if (fhresok->object.nfs_fh4_len > buf_size) {
		ret = -ENOSPC;
		goto out;
	}

	memcpy(fh_buf, fhresok->object.nfs_fh4_val,
	       fhresok->object.nfs_fh4_len);
	*fh_len_out = fhresok->object.nfs_fh4_len;
	ret = 0;

out:
	mds_compound_fini(&mc);
	return ret;
}

/*
 * Split `path` into "/"-separated components into `scratch` (modified
 * in place).  `scratch` must be large enough to hold path plus NUL;
 * component pointers + lengths are returned in the caller's arrays.
 *
 * Empty components (leading / trailing / doubled slashes) are skipped.
 * Returns the number of components on success, or -E2BIG if there
 * are more than PS_DISCOVERY_MAX_DEPTH components or any component
 * exceeds PS_DISCOVERY_COMPONENT_MAX bytes.
 */
static int split_path(char *scratch, const char **components,
		      uint32_t *component_lens)
{
	unsigned int n = 0;
	char *p = scratch;

	while (*p) {
		/* Skip any run of slashes (leading, trailing, doubled). */
		while (*p == '/')
			p++;
		if (!*p)
			break;

		if (n >= PS_DISCOVERY_MAX_DEPTH)
			return -E2BIG;

		char *start = p;

		while (*p && *p != '/')
			p++;

		size_t len = (size_t)(p - start);

		if (len > PS_DISCOVERY_COMPONENT_MAX)
			return -E2BIG;

		components[n] = start;
		component_lens[n] = (uint32_t)len;
		n++;
	}
	return (int)n;
}

int ps_discovery_walk_path(struct mds_session *ms, const char *path,
			   uint8_t *fh_buf, uint32_t buf_size,
			   uint32_t *fh_len_out)
{
	struct mds_compound mc;
	nfs_argop4 *slot;
	nfs_resop4 *res;
	int ret;
	int rc;
	unsigned int n;

	if (!ms || !path || !fh_buf || !fh_len_out)
		return -EINVAL;
	if (path[0] != '/')
		return -EINVAL;

	/*
	 * We mutate in place to carve components, so size the scratch
	 * exactly to hold the incoming NUL-terminated path.  Absolute
	 * paths over PS_MOUNT_PATH_MAX are already out-of-bounds for
	 * MNTPATHLEN; bail early rather than VLA-stack-overflowing.
	 */
	size_t plen = strlen(path);

	if (plen >= 4096)
		return -E2BIG;

	char scratch[4096];
	const char *components[PS_DISCOVERY_MAX_DEPTH];
	uint32_t component_lens[PS_DISCOVERY_MAX_DEPTH];

	memcpy(scratch, path, plen + 1);

	rc = split_path(scratch, components, component_lens);
	if (rc < 0)
		return rc;
	n = (unsigned int)rc;

	/*
	 * n == 0 ("/" or "") means "walk to root", which is exactly
	 * what fetch_root_fh already does -- delegate rather than
	 * duplicate.
	 */
	if (n == 0)
		return ps_discovery_fetch_root_fh(ms, fh_buf, buf_size,
						  fh_len_out);

	/* SEQUENCE + PUTROOTFH + n * LOOKUP + GETFH */
	ret = mds_compound_init(&mc, 3 + n, "ps-discover-walk");
	if (ret)
		return ret;

	ret = mds_compound_add_sequence(&mc, ms);
	if (ret)
		goto out;

	slot = mds_compound_add_op(&mc, OP_PUTROOTFH);
	if (!slot) {
		ret = -ENOSPC;
		goto out;
	}

	for (unsigned int i = 0; i < n; i++) {
		slot = mds_compound_add_op(&mc, OP_LOOKUP);
		if (!slot) {
			ret = -ENOSPC;
			goto out;
		}
		/*
		 * Cast away const: TIRPC's LOOKUP4args holds a non-const
		 * utf8string_val for encoding; we never expose this pointer
		 * back out.  `scratch` stays alive for the duration of
		 * mds_compound_send() below.
		 */
		slot->nfs_argop4_u.oplookup.objname.utf8string_val =
			(char *)components[i];
		slot->nfs_argop4_u.oplookup.objname.utf8string_len =
			component_lens[i];
	}

	slot = mds_compound_add_op(&mc, OP_GETFH);
	if (!slot) {
		ret = -ENOSPC;
		goto out;
	}

	ret = mds_compound_send(&mc, ms);
	if (ret)
		goto out;

	/* Check PUTROOTFH + each LOOKUP for NFS4_OK. */
	for (unsigned int i = 0; i <= n; i++) {
		res = mds_compound_result(&mc, 1 + i);
		if (!res) {
			ret = -EREMOTEIO;
			goto out;
		}
		/*
		 * All ops up through the last LOOKUP use the same status
		 * field ordering: the union fields share the `status`
		 * prefix at offset 0 by C language XDR convention.  Read
		 * the PUTROOTFH status for op 0 and the LOOKUP status for
		 * ops 1..n explicitly; don't type-pun across the union.
		 */
		if (i == 0) {
			if (res->nfs_resop4_u.opputrootfh.status != NFS4_OK) {
				ret = -EREMOTEIO;
				goto out;
			}
		} else if (res->nfs_resop4_u.oplookup.status != NFS4_OK) {
			/*
			 * A missing intermediate component returns
			 * NFS4ERR_NOENT; surface as -ENOENT so the caller
			 * (ps_discovery_run) can distinguish "upstream
			 * doesn't have this export anymore" from network
			 * errors.
			 */
			if (res->nfs_resop4_u.oplookup.status == NFS4ERR_NOENT)
				ret = -ENOENT;
			else
				ret = -EREMOTEIO;
			goto out;
		}
	}

	/* GETFH is the last op (index 2 + n). */
	res = mds_compound_result(&mc, 2 + n);
	if (!res || res->nfs_resop4_u.opgetfh.status != NFS4_OK) {
		ret = -EREMOTEIO;
		goto out;
	}

	GETFH4resok *fhresok = &res->nfs_resop4_u.opgetfh.GETFH4res_u.resok4;

	if (fhresok->object.nfs_fh4_len == 0) {
		ret = -EREMOTEIO;
		goto out;
	}
	if (fhresok->object.nfs_fh4_len > buf_size) {
		ret = -ENOSPC;
		goto out;
	}

	memcpy(fh_buf, fhresok->object.nfs_fh4_val,
	       fhresok->object.nfs_fh4_len);
	*fh_len_out = fhresok->object.nfs_fh4_len;
	ret = 0;

out:
	mds_compound_fini(&mc);
	return ret;
}

int ps_discovery_run(const struct ps_listener_state *pls)
{
	struct ps_export_entry *entries = NULL;
	size_t n = 0;
	unsigned int ok = 0;
	unsigned int fail = 0;
	int ret;

	if (!pls || pls->pls_upstream[0] == '\0')
		return -EINVAL;
	if (!pls->pls_session)
		return -ENOTCONN;

	/*
	 * Serialize concurrent discovery runs on the same listener.
	 * Closes the single-writer gap flagged in slice 2e-iii-b: on-
	 * demand re-discovery from op-handler workers is now safe
	 * against two writers racing on pls_exports[] / pls_nexports.
	 * Readers (op handlers calling ps_state_find_export) still use
	 * the release/acquire atomics on ple_fh_len + pls_nexports and
	 * do not block here.
	 */
	ret = ps_state_discovery_lock(pls->pls_listener_id);
	if (ret < 0)
		return ret;

	ret = ps_mount_fetch_exports(pls->pls_upstream, &entries, &n);
	if (ret < 0) {
		/*
		 * stderr rather than LOG() on purpose: reffs/log.h's LOG
		 * pulls in libreffs_utils which transitively brings in the
		 * full urcu/xxhash/fs dep graph, blowing up the unit-test
		 * link.  The rest of the ps/ subsystem follows the same
		 * "caller does structured logging" discipline; reffsd.c
		 * will turn these lines into proper LOG events when it
		 * consumes the coordinator in slice 2e-iii-e.
		 */
		fprintf(stderr,
			"ps[%u]: MOUNT3 export enumeration against %s "
			"failed: %d\n",
			pls->pls_listener_id, pls->pls_upstream, ret);
		goto out;
	}

	for (size_t i = 0; i < n; i++) {
		uint8_t fh[PS_MAX_FH_SIZE];
		uint32_t fh_len = 0;
		int walk_r;
		int add_r;

		walk_r = ps_discovery_walk_path(pls->pls_session,
						entries[i].path, fh, sizeof(fh),
						&fh_len);
		if (walk_r < 0) {
			fprintf(stderr, "ps[%u]: walk of %s failed: %d\n",
				pls->pls_listener_id, entries[i].path, walk_r);
			fail++;
			continue;
		}

		add_r = ps_state_add_export(pls->pls_listener_id,
					    entries[i].path, fh, fh_len);
		if (add_r < 0) {
			fprintf(stderr, "ps[%u]: add export %s failed: %d\n",
				pls->pls_listener_id, entries[i].path, add_r);
			fail++;
			continue;
		}
		ok++;
	}

	ps_mount_free_exports(entries);

	fprintf(stderr, "ps[%u]: discovered %zu exports (%u ok, %u failed)\n",
		pls->pls_listener_id, n, ok, fail);
	ret = 0;

out:
	ps_state_discovery_unlock(pls->pls_listener_id);
	return ret;
}
