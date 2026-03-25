/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rpc/xdr.h>

#include "nfsv42_xdr.h"
#include "nfsv42_names.h"
#include "reffs/dstore.h"
#include "reffs/dstore_fanout.h"
#include "reffs/dstore_ops.h"
#include "reffs/inode.h"
#include "reffs/layout_segment.h"
#include "reffs/log.h"
#include "reffs/rpc.h"
#include "reffs/runway.h"
#include "reffs/server.h"
#include "reffs/settings.h"
#include "reffs/stateid.h"
#include "reffs/task.h"
#include "nfs4/client.h"
#include "nfs4/compound.h"
#include "nfs4/ops.h"
#include "nfs4/errors.h"
#include "nfs4/stateid.h"

/* ------------------------------------------------------------------ */
/* Device ID encoding                                                  */
/*                                                                     */
/* 16 bytes: first 12 zero, last 4 are the dstore_id in network       */
/* byte order.                                                         */
/* ------------------------------------------------------------------ */

static void deviceid_from_dstore(deviceid4 out, uint32_t dstore_id)
{
	uint32_t net_id = htonl(dstore_id);

	memset(out, 0, NFS4_DEVICEID4_SIZE);
	memcpy(out + 12, &net_id, sizeof(net_id));
}

static uint32_t deviceid_to_dstore(const deviceid4 devid)
{
	uint32_t net_id;

	memcpy(&net_id, devid + 12, sizeof(net_id));
	return ntohl(net_id);
}

/* ------------------------------------------------------------------ */
/* GETDEVICEINFO                                                       */
/*                                                                     */
/* Returns the NFSv3 data server address for a given device ID.        */
/* The device address body is XDR-encoded ff_device_addr4.             */
/* ------------------------------------------------------------------ */

uint32_t nfs4_op_getdeviceinfo(struct compound *compound)
{
	GETDEVICEINFO4args *args = NFS4_OP_ARG_SETUP(compound, opgetdeviceinfo);
	GETDEVICEINFO4res *res = NFS4_OP_RES_SETUP(compound, opgetdeviceinfo);
	nfsstat4 *status = &res->gdir_status;
	GETDEVICEINFO4resok *resok =
		NFS4_OP_RESOK_SETUP(res, GETDEVICEINFO4res_u, gdir_resok4);

	if (args->gdia_layout_type != LAYOUT4_FLEX_FILES &&
	    args->gdia_layout_type != LAYOUT4_FLEX_FILES_V2) {
		*status = NFS4ERR_UNKNOWN_LAYOUTTYPE;
		return 0;
	}

	uint32_t dstore_id = deviceid_to_dstore(args->gdia_device_id);
	struct dstore *ds = dstore_find(dstore_id);

	if (!ds) {
		*status = NFS4ERR_NOENT;
		return 0;
	}

	/*
	 * Build the ff_device_addr4 structure:
	 *   ffda_netaddrs: one netaddr4 with the DS address
	 *   ffda_versions: one entry for NFSv3
	 *
	 * The universal address format for TCP/IPv4 is:
	 *   "h1.h2.h3.h4.p1.p2" where p1.p2 is port 2049 = 8.1
	 */
	char uaddr[64];

	snprintf(uaddr, sizeof(uaddr), "%s.8.1", ds->ds_ip);

	/*
	 * XDR-encode ff_device_addr4 into the da_addr_body opaque.
	 */
	ff_device_addr4 ffda;

	memset(&ffda, 0, sizeof(ffda));

	/* One netaddr4 in the multipath list. */
	netaddr4 na;
	char *netid = "tcp";

	na.na_r_netid = netid;
	na.na_r_addr = uaddr;

	ffda.ffda_netaddrs.multipath_list4_len = 1;
	ffda.ffda_netaddrs.multipath_list4_val = &na;

	/* One version entry: NFSv3. */
	ff_device_versions4 ver;

	memset(&ver, 0, sizeof(ver));
	ver.ffdv_version = 3;
	ver.ffdv_minorversion = 0;
	ver.ffdv_rsize = 1048576;
	ver.ffdv_wsize = 1048576;
	ver.ffdv_tightly_coupled = false;

	ffda.ffda_versions.ffda_versions_len = 1;
	ffda.ffda_versions.ffda_versions_val = &ver;

	/* XDR-encode into an opaque buffer. */
	u_long xdr_size = xdr_sizeof((xdrproc_t)xdr_ff_device_addr4, &ffda);

	resok->gdir_device_addr.da_layout_type = args->gdia_layout_type;
	resok->gdir_device_addr.da_addr_body.da_addr_body_val =
		calloc(1, xdr_size);
	if (!resok->gdir_device_addr.da_addr_body.da_addr_body_val) {
		dstore_put(ds);
		*status = NFS4ERR_DELAY;
		return 0;
	}
	resok->gdir_device_addr.da_addr_body.da_addr_body_len = (u_int)xdr_size;

	XDR xdrs;

	xdrmem_create(&xdrs,
		      resok->gdir_device_addr.da_addr_body.da_addr_body_val,
		      xdr_size, XDR_ENCODE);
	if (!xdr_ff_device_addr4(&xdrs, &ffda)) {
		xdr_destroy(&xdrs);
		free(resok->gdir_device_addr.da_addr_body.da_addr_body_val);
		resok->gdir_device_addr.da_addr_body.da_addr_body_val = NULL;
		dstore_put(ds);
		*status = NFS4ERR_SERVERFAULT;
		return 0;
	}
	xdr_destroy(&xdrs);

	TRACE("GETDEVICEINFO: dstore[%u] addr=%s", dstore_id, uaddr);
	dstore_put(ds);

	return 0;
}

/* ------------------------------------------------------------------ */
/* Layout queries                                                      */
/* ------------------------------------------------------------------ */

/*
 * Check if any client holds a write layout on this inode.
 */
bool inode_has_write_layout(struct inode *inode)
{
	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;

	if (!inode || !inode->i_stateids)
		return false;

	rcu_read_lock();
	cds_lfht_first(inode->i_stateids, &iter);
	while ((node = cds_lfht_iter_get_node(&iter)) != NULL) {
		struct stateid *stid =
			caa_container_of(node, struct stateid, s_inode_node);
		if (stid->s_tag == Layout_Stateid) {
			struct layout_stateid *ls = stid_to_layout(stid);
			uint64_t state = __atomic_load_n(&ls->ls_state,
							 __ATOMIC_ACQUIRE);
			if (state & LAYOUT_STATEID_IOMODE_RW) {
				rcu_read_unlock();
				return true;
			}
		}
		cds_lfht_next(inode->i_stateids, &iter);
	}
	rcu_read_unlock();
	return false;
}

/* ------------------------------------------------------------------ */
/* Layout stateid helpers                                              */
/* ------------------------------------------------------------------ */

/*
 * Find an existing layout stateid for this client on this inode,
 * or allocate a new one.  Returns a ref-bumped pointer or NULL.
 */
static struct layout_stateid *
layout_stateid_find_or_create(struct inode *inode, struct compound *compound)
{
	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;
	struct client *client =
		compound->c_nfs4_client ?
			nfs4_client_to_client(compound->c_nfs4_client) :
			NULL;

	if (!client || !inode->i_stateids)
		return NULL;

	/* Search for an existing layout stateid on this inode. */
	rcu_read_lock();
	cds_lfht_first(inode->i_stateids, &iter);
	while ((node = cds_lfht_iter_get_node(&iter)) != NULL) {
		struct stateid *stid =
			caa_container_of(node, struct stateid, s_inode_node);
		if (stid->s_tag == Layout_Stateid && stid->s_client == client) {
			struct stateid *got = stateid_get(stid);

			rcu_read_unlock();
			if (got)
				return stid_to_layout(got);
			/* Lost race — retry. */
			rcu_read_lock();
			cds_lfht_first(inode->i_stateids, &iter);
			continue;
		}
		cds_lfht_next(inode->i_stateids, &iter);
	}
	rcu_read_unlock();

	/* None found — allocate a new one. */
	struct layout_stateid *new_ls = layout_stateid_alloc(inode, client);

	if (new_ls) {
		/* Bump ref for the caller (hash table holds the other). */
		stateid_get(&new_ls->ls_stid);
		TRACE("LAYOUTGET: created layout stateid id=%u cookie=%u "
		      "on ino=%lu",
		      new_ls->ls_stid.s_id, new_ls->ls_stid.s_cookie,
		      inode->i_ino);
	}
	return new_ls;
}

/* ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ */
/* Layout body builders — one per layout type.                         */
/*                                                                     */
/* Each builder XDR-encodes the layout body and returns it in *body.   */
/* The caller owns the allocated buffer.  Returns NFS4_OK or an error. */
/* ------------------------------------------------------------------ */

static nfsstat4 layoutget_build_v1(struct layout_segment *seg, char **out_body,
				   u_long *out_size)
{
	ff_layout4 ffl;

	memset(&ffl, 0, sizeof(ffl));
	ffl.ffl_stripe_unit = seg->ls_stripe_unit;
	ffl.ffl_flags = FF_FLAGS_NO_LAYOUTCOMMIT | FF_FLAGS_NO_IO_THRU_MDS;
	ffl.ffl_stats_collect_hint = 0;

	ffl.ffl_mirrors.ffl_mirrors_len = seg->ls_nfiles;
	ffl.ffl_mirrors.ffl_mirrors_val =
		calloc(seg->ls_nfiles, sizeof(ff_mirror4));
	if (!ffl.ffl_mirrors.ffl_mirrors_val)
		return NFS4ERR_DELAY;

	nfsstat4 ret = NFS4_OK; /* used only for error path */

	for (uint32_t i = 0; i < seg->ls_nfiles; i++) {
		struct layout_data_file *ldf = &seg->ls_files[i];
		ff_mirror4 *mirror = &ffl.ffl_mirrors.ffl_mirrors_val[i];

		mirror->ffm_data_servers.ffm_data_servers_len = 1;
		mirror->ffm_data_servers.ffm_data_servers_val =
			calloc(1, sizeof(ff_data_server4));
		if (!mirror->ffm_data_servers.ffm_data_servers_val) {
			ret = NFS4ERR_DELAY;
			goto out_v1;
		}

		ff_data_server4 *ffds =
			mirror->ffm_data_servers.ffm_data_servers_val;

		deviceid_from_dstore(ffds->ffds_deviceid, ldf->ldf_dstore_id);

		struct dstore *ds = dstore_find(ldf->ldf_dstore_id);

		ffds->ffds_efficiency =
			(ds && ds->ds_ops == &dstore_ops_local) ? 255 : 1;
		dstore_put(ds);

		ffds->ffds_fh_vers.ffds_fh_vers_len = 1;
		ffds->ffds_fh_vers.ffds_fh_vers_val =
			calloc(1, sizeof(nfs_fh4));
		if (!ffds->ffds_fh_vers.ffds_fh_vers_val) {
			ret = NFS4ERR_DELAY;
			goto out_v1;
		}

		nfs_fh4 *fh = &ffds->ffds_fh_vers.ffds_fh_vers_val[0];

		fh->nfs_fh4_len = ldf->ldf_fh_len;
		fh->nfs_fh4_val = calloc(1, ldf->ldf_fh_len);
		if (!fh->nfs_fh4_val) {
			ret = NFS4ERR_DELAY;
			goto out_v1;
		}
		memcpy(fh->nfs_fh4_val, ldf->ldf_fh, ldf->ldf_fh_len);

		char uid_str[16], gid_str[16];

		snprintf(uid_str, sizeof(uid_str), "%u", ldf->ldf_uid);
		snprintf(gid_str, sizeof(gid_str), "%u", ldf->ldf_gid);
		ffds->ffds_user.utf8string_len = strlen(uid_str);
		ffds->ffds_user.utf8string_val = strdup(uid_str);
		ffds->ffds_group.utf8string_len = strlen(gid_str);
		ffds->ffds_group.utf8string_val = strdup(gid_str);
	}

	u_long xdr_size = xdr_sizeof((xdrproc_t)xdr_ff_layout4, &ffl);
	char *body = calloc(1, xdr_size);

	if (!body) {
		ret = NFS4ERR_DELAY;
		goto out_v1;
	}

	XDR xdrs;

	xdrmem_create(&xdrs, body, xdr_size, XDR_ENCODE);
	if (!xdr_ff_layout4(&xdrs, &ffl)) {
		xdr_destroy(&xdrs);
		free(body);
		ret = NFS4ERR_SERVERFAULT;
		goto out_v1;
	}
	xdr_destroy(&xdrs);

	*out_body = body;
	*out_size = xdr_size;

out_v1:
	for (uint32_t i = 0; i < seg->ls_nfiles; i++) {
		ff_data_server4 *ffds =
			ffl.ffl_mirrors.ffl_mirrors_val[i]
				.ffm_data_servers.ffm_data_servers_val;
		if (ffds) {
			if (ffds->ffds_fh_vers.ffds_fh_vers_val) {
				free(ffds->ffds_fh_vers.ffds_fh_vers_val[0]
					     .nfs_fh4_val);
				free(ffds->ffds_fh_vers.ffds_fh_vers_val);
			}
			free(ffds->ffds_user.utf8string_val);
			free(ffds->ffds_group.utf8string_val);
			free(ffds);
		}
	}
	free(ffl.ffl_mirrors.ffl_mirrors_val);
	return ret;
}

static nfsstat4 layoutget_build_v2(struct layout_segment *seg, char **out_body,
				   u_long *out_size)
{
	ffv2_layout4 ffl;

	memset(&ffl, 0, sizeof(ffl));
	ffl.ffl_flags = FFV2_FLAGS_NO_LAYOUTCOMMIT | FFV2_FLAGS_NO_IO_THRU_MDS;
	ffl.ffl_stats_collect_hint = 0;

	ffl.ffl_mirrors.ffl_mirrors_len = 1;
	ffl.ffl_mirrors.ffl_mirrors_val = calloc(1, sizeof(ffv2_mirror4));
	if (!ffl.ffl_mirrors.ffl_mirrors_val)
		return NFS4ERR_DELAY;

	nfsstat4 ret = NFS4_OK;
	ffv2_mirror4 *mirror = &ffl.ffl_mirrors.ffl_mirrors_val[0];

	if (seg->ls_m == 0)
		mirror->ffm_coding_type_data.fctd_coding = FFV2_CODING_MIRRORED;
	else
		mirror->ffm_coding_type_data.fctd_coding =
			FFV2_ENCODING_RS_VANDERMONDE;

	mirror->ffm_striping = FFV2_STRIPING_DENSE;
	mirror->ffm_striping_unit_size = 4096;
	mirror->ffm_client_id = 0;

	mirror->ffm_stripes.ffm_stripes_len = 1;
	mirror->ffm_stripes.ffm_stripes_val = calloc(1, sizeof(ffv2_stripes4));
	if (!mirror->ffm_stripes.ffm_stripes_val) {
		ret = NFS4ERR_DELAY;
		goto out_v2;
	}

	ffv2_stripes4 *stripe = &mirror->ffm_stripes.ffm_stripes_val[0];

	stripe->ffs_data_servers.ffs_data_servers_len = seg->ls_nfiles;
	stripe->ffs_data_servers.ffs_data_servers_val =
		calloc(seg->ls_nfiles, sizeof(ffv2_data_server4));
	if (!stripe->ffs_data_servers.ffs_data_servers_val) {
		ret = NFS4ERR_DELAY;
		goto out_v2;
	}

	for (uint32_t i = 0; i < seg->ls_nfiles; i++) {
		struct layout_data_file *ldf = &seg->ls_files[i];
		ffv2_data_server4 *ffds =
			&stripe->ffs_data_servers.ffs_data_servers_val[i];

		deviceid_from_dstore(ffds->ffv2ds_deviceid, ldf->ldf_dstore_id);

		struct dstore *ds = dstore_find(ldf->ldf_dstore_id);

		ffds->ffv2ds_efficiency =
			(ds && ds->ds_ops == &dstore_ops_local) ? 255 : 1;
		dstore_put(ds);

		ffds->ffv2ds_file_info.ffv2ds_file_info_len = 1;
		ffds->ffv2ds_file_info.ffv2ds_file_info_val =
			calloc(1, sizeof(ffv2_file_info4));
		if (!ffds->ffv2ds_file_info.ffv2ds_file_info_val) {
			ret = NFS4ERR_DELAY;
			goto out_v2;
		}

		ffv2_file_info4 *fi =
			&ffds->ffv2ds_file_info.ffv2ds_file_info_val[0];
		memset(&fi->fffi_stateid, 0, sizeof(fi->fffi_stateid));
		fi->fffi_fh_vers.nfs_fh4_len = ldf->ldf_fh_len;
		fi->fffi_fh_vers.nfs_fh4_val = calloc(1, ldf->ldf_fh_len);
		if (!fi->fffi_fh_vers.nfs_fh4_val) {
			ret = NFS4ERR_DELAY;
			goto out_v2;
		}
		memcpy(fi->fffi_fh_vers.nfs_fh4_val, ldf->ldf_fh,
		       ldf->ldf_fh_len);

		char uid_str[16], gid_str[16];

		snprintf(uid_str, sizeof(uid_str), "%u", ldf->ldf_uid);
		snprintf(gid_str, sizeof(gid_str), "%u", ldf->ldf_gid);
		ffds->ffv2ds_user.utf8string_len = strlen(uid_str);
		ffds->ffv2ds_user.utf8string_val = strdup(uid_str);
		ffds->ffv2ds_group.utf8string_len = strlen(gid_str);
		ffds->ffv2ds_group.utf8string_val = strdup(gid_str);
		ffds->ffv2ds_flags = (i < seg->ls_k) ? FFV2_DS_FLAGS_ACTIVE :
						       FFV2_DS_FLAGS_PARITY;
	}

	u_long xdr_size = xdr_sizeof((xdrproc_t)xdr_ffv2_layout4, &ffl);
	char *body = calloc(1, xdr_size);

	if (!body) {
		ret = NFS4ERR_DELAY;
		goto out_v2;
	}

	XDR xdrs;

	xdrmem_create(&xdrs, body, xdr_size, XDR_ENCODE);
	if (!xdr_ffv2_layout4(&xdrs, &ffl)) {
		xdr_destroy(&xdrs);
		free(body);
		ret = NFS4ERR_SERVERFAULT;
		goto out_v2;
	}
	xdr_destroy(&xdrs);

	*out_body = body;
	*out_size = xdr_size;

out_v2:
	if (ffl.ffl_mirrors.ffl_mirrors_val) {
		ffv2_mirror4 *m0 = &ffl.ffl_mirrors.ffl_mirrors_val[0];

		if (m0->ffm_stripes.ffm_stripes_val) {
			ffv2_stripes4 *st = &m0->ffm_stripes.ffm_stripes_val[0];

			for (uint32_t i = 0;
			     i < st->ffs_data_servers.ffs_data_servers_len;
			     i++) {
				ffv2_data_server4 *f2 =
					&st->ffs_data_servers
						 .ffs_data_servers_val[i];

				if (f2->ffv2ds_file_info.ffv2ds_file_info_val) {
					free(f2->ffv2ds_file_info
						     .ffv2ds_file_info_val[0]
						     .fffi_fh_vers.nfs_fh4_val);
					free(f2->ffv2ds_file_info
						     .ffv2ds_file_info_val);
				}
				free(f2->ffv2ds_user.utf8string_val);
				free(f2->ffv2ds_group.utf8string_val);
			}
			free(st->ffs_data_servers.ffs_data_servers_val);
			free(m0->ffm_stripes.ffm_stripes_val);
		}
		free(ffl.ffl_mirrors.ffl_mirrors_val);
	}
	return ret;
}

/* ------------------------------------------------------------------ */
/* LAYOUTGET                                                           */
/*                                                                     */
/* Returns a Flex Files layout for the current filehandle.  Clients    */
/* may request LAYOUT4_FLEX_FILES (v1) or LAYOUT4_FLEX_FILES_V2 (v2). */
/* ------------------------------------------------------------------ */

uint32_t nfs4_op_layoutget(struct compound *compound)
{
	LAYOUTGET4args *args = NFS4_OP_ARG_SETUP(compound, oplayoutget);
	LAYOUTGET4res *res = NFS4_OP_RES_SETUP(compound, oplayoutget);
	nfsstat4 *status = &res->logr_status;
	LAYOUTGET4resok *resok =
		NFS4_OP_RESOK_SETUP(res, LAYOUTGET4res_u, logr_resok4);

	if (network_file_handle_empty(&compound->c_curr_nfh)) {
		*status = NFS4ERR_NOFILEHANDLE;
		return 0;
	}

	if (args->loga_layout_type != LAYOUT4_FLEX_FILES &&
	    args->loga_layout_type != LAYOUT4_FLEX_FILES_V2) {
		*status = NFS4ERR_UNKNOWN_LAYOUTTYPE;
		return 0;
	}

	layouttype4 layout_type = args->loga_layout_type;

	if (!compound->c_inode) {
		*status = NFS4ERR_NOFILEHANDLE;
		return 0;
	}

	struct layout_segments *lss;

	/*
	 * On-demand layout creation: if the inode has no layout
	 * segments, pop FHs from dstore runways and build one.
	 * Hold i_attr_mutex to prevent concurrent LAYOUTGETs from
	 * racing on the same inode.
	 */
	pthread_mutex_lock(&compound->c_inode->i_attr_mutex);
	lss = compound->c_inode->i_layout_segments;

	if (!lss || lss->lss_count == 0) {
		struct dstore *dstores[LAYOUT_SEG_MAX_FILES];
		uint32_t nds;

		nds = dstore_collect_available(dstores, LAYOUT_SEG_MAX_FILES);
		if (nds == 0) {
			pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);
			*status = NFS4ERR_LAYOUTUNAVAILABLE;
			return 0;
		}

		/*
		 * Target layout_width data files, round-robin across
		 * available dstores.  When fewer dstores than the
		 * target width are available, the same dstore gets
		 * multiple data files (each with its own runway FH).
		 */
		struct server_state *ss = server_state_find();
		uint32_t target = ss ? ss->ss_layout_width :
				       REFFS_LAYOUT_WIDTH_DEFAULT;
		uint32_t fence_min = ss ? ss->ss_fence_uid_min :
					  REFFS_FENCE_UID_MIN_DEFAULT;
		uint32_t fence_max = ss ? ss->ss_fence_uid_max :
					  REFFS_FENCE_UID_MAX_DEFAULT;

		server_state_put(ss);

		struct layout_data_file *files =
			calloc(target, sizeof(struct layout_data_file));
		if (!files) {
			for (uint32_t i = 0; i < nds; i++)
				dstore_put(dstores[i]);
			pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);
			*status = NFS4ERR_DELAY;
			return 0;
		}

		uint32_t nfiles = 0;

		for (uint32_t i = 0; i < target; i++) {
			struct dstore *ds = dstores[i % nds];

			if (!ds->ds_runway ||
			    runway_pop(ds->ds_runway, files[nfiles].ldf_fh,
				       &files[nfiles].ldf_fh_len) < 0) {
				TRACE("LAYOUTGET: dstore[%u] runway empty",
				      ds->ds_id);
				continue;
			}
			files[nfiles].ldf_dstore_id = ds->ds_id;
			files[nfiles].ldf_uid = fence_min;
			files[nfiles].ldf_gid = fence_min;
			files[nfiles].ldf_mode = 0640;

			dstore_data_file_fence(ds, files[nfiles].ldf_fh,
					       files[nfiles].ldf_fh_len,
					       &files[nfiles], fence_min,
					       fence_max, NULL);
			dstore_data_file_chmod(ds, files[nfiles].ldf_fh,
					       files[nfiles].ldf_fh_len, NULL);

			nfiles++;
		}

		for (uint32_t i = 0; i < nds; i++)
			dstore_put(dstores[i]);

		if (nfiles == 0) {
			free(files);
			pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);
			*status = NFS4ERR_LAYOUTUNAVAILABLE;
			return 0;
		}

		/* Build the layout segment. */
		struct layout_segment seg = {
			.ls_offset = 0,
			.ls_length = 0, /* entire file */
			.ls_stripe_unit = 0,
			.ls_k = (uint16_t)nfiles,
			.ls_m = 0,
			.ls_nfiles = nfiles,
			.ls_files = files,
		};

		if (!lss) {
			lss = layout_segments_alloc();
			if (!lss) {
				free(files);
				pthread_mutex_unlock(
					&compound->c_inode->i_attr_mutex);
				*status = NFS4ERR_DELAY;
				return 0;
			}
			compound->c_inode->i_layout_segments = lss;
		}

		if (layout_segments_add(lss, &seg) < 0) {
			free(files);
			pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);
			*status = NFS4ERR_DELAY;
			return 0;
		}

		inode_sync_to_disk(compound->c_inode);

		TRACE("LAYOUTGET: created layout for ino=%lu with %u mirrors",
		      compound->c_inode->i_ino, nfiles);

		static bool first_layout = true;

		if (first_layout) {
			first_layout = false;
			LOG("NFSv4.2 Flex File v2 Layout Driver: "
			    "first layout issued");
		}
	}
	pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);

	/* Find or create a layout stateid for this client + inode. */
	struct layout_stateid *ls =
		layout_stateid_find_or_create(compound->c_inode, compound);
	if (!ls) {
		*status = NFS4ERR_DELAY;
		return 0;
	}

	/* Track the granted iomode. */
	uint64_t mode_bit = (args->loga_iomode == LAYOUTIOMODE4_RW) ?
				    LAYOUT_STATEID_IOMODE_RW :
				    LAYOUT_STATEID_IOMODE_READ;
	__atomic_or_fetch(&ls->ls_state, mode_bit, __ATOMIC_RELEASE);

	/*
	 * For now, return the first segment as a single layout.
	 * Future: match args->loga_offset/length to find the right
	 * segment(s) and support continuations.
	 */
	struct layout_segment *seg = &lss->lss_segs[0];

	/*
	 * Build the layout body.  Dispatch based on what the client
	 * requested: ff_layout4 (v1) or ffv2_layout4 (v2).
	 */
	char *body = NULL;
	u_long xdr_size = 0;

	if (layout_type == LAYOUT4_FLEX_FILES_V2)
		*status = layoutget_build_v2(seg, &body, &xdr_size);
	else
		*status = layoutget_build_v1(seg, &body, &xdr_size);

	if (*status != NFS4_OK)
		goto out_stateid;

	/* Build the response. */
	resok->logr_return_on_close = true;

	/* Bump seqid and pack the layout stateid. */
	__atomic_add_fetch(&ls->ls_stid.s_seqid, 1, __ATOMIC_RELAXED);
	pack_stateid4(&resok->logr_stateid, &ls->ls_stid);

	resok->logr_layout.logr_layout_len = 1;
	resok->logr_layout.logr_layout_val = calloc(1, sizeof(layout4));
	if (!resok->logr_layout.logr_layout_val) {
		free(body);
		*status = NFS4ERR_DELAY;
		goto out_stateid;
	}

	layout4 *lo = &resok->logr_layout.logr_layout_val[0];

	lo->lo_offset = seg->ls_offset;
	lo->lo_length = seg->ls_length ? seg->ls_length : NFS4_UINT64_MAX;
	lo->lo_iomode = args->loga_iomode;
	lo->lo_content.loc_type = layout_type;
	lo->lo_content.loc_body.loc_body_val = body;
	lo->lo_content.loc_body.loc_body_len = (u_int)xdr_size;

out_stateid:
	if (*status == NFS4_OK)
		TRACE("LAYOUTGET: ino=%lu nfiles=%u stripe_unit=%u seqid=%u",
		      compound->c_inode->i_ino, seg->ls_nfiles,
		      seg->ls_stripe_unit, ls->ls_stid.s_seqid);

	stateid_put(&ls->ls_stid); /* drop find/create ref */

	return 0;
}

/* ------------------------------------------------------------------ */
/* Stubs                                                               */
/* ------------------------------------------------------------------ */

uint32_t nfs4_op_layoutcommit(struct compound *compound)
{
	LAYOUTCOMMIT4args *args = NFS4_OP_ARG_SETUP(compound, oplayoutcommit);
	LAYOUTCOMMIT4res *res = NFS4_OP_RES_SETUP(compound, oplayoutcommit);
	LAYOUTCOMMIT4resok *resok =
		NFS4_OP_RESOK_SETUP(res, LAYOUTCOMMIT4res_u, locr_resok4);

	if (network_file_handle_empty(&compound->c_curr_nfh) ||
	    !compound->c_inode) {
		res->locr_status = NFS4ERR_NOFILEHANDLE;
		return 0;
	}

	/*
	 * Update inode size if the client reports a new last write offset.
	 */
	if (args->loca_last_write_offset.no_newoffset) {
		uint64_t new_end =
			args->loca_last_write_offset.newoffset4_u.no_offset + 1;

		pthread_mutex_lock(&compound->c_inode->i_attr_mutex);
		if ((int64_t)new_end > compound->c_inode->i_size)
			compound->c_inode->i_size = (int64_t)new_end;
		pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);

		resok->locr_newsize.ns_sizechanged = true;
		resok->locr_newsize.newsize4_u.ns_size =
			(uint64_t)compound->c_inode->i_size;
	}

	inode_sync_to_disk(compound->c_inode);

	return 0;
}

/*
 * Resume callback for LAYOUTRETURN after reflected GETATTR fan-out.
 * Updates the inode's cached attrs from the DS responses.
 */
static uint32_t nfs4_op_layoutreturn_resume(struct rpc_trans *rt)
{
	struct compound *compound = rt->rt_compound;
	struct dstore_fanout *df = rt->rt_async_data;
	struct inode *inode = compound->c_inode;

	rt->rt_async_data = NULL;

	/* Best-effort: if fan-out failed, we still completed the
	 * LAYOUTRETURN successfully.  Just update what we can. */
	if (dstore_fanout_result(df) == 0 && inode->i_layout_segments &&
	    inode->i_layout_segments->lss_count > 0) {
		struct layout_segment *seg =
			&inode->i_layout_segments->lss_segs[0];
		int64_t max_size = 0;

		for (uint32_t i = 0; i < seg->ls_nfiles; i++) {
			struct layout_data_file *ldf = &seg->ls_files[i];

			if (!ldf->ldf_stale && ldf->ldf_size > max_size)
				max_size = ldf->ldf_size;
		}

		pthread_mutex_lock(&inode->i_attr_mutex);
		if (max_size > inode->i_size) {
			struct timespec now;

			clock_gettime(CLOCK_REALTIME, &now);
			inode->i_size = max_size;
			inode->i_mtime = now;
			inode->i_ctime = now;
		}
		pthread_mutex_unlock(&inode->i_attr_mutex);

		inode_sync_to_disk(inode);
	}

	dstore_fanout_free(df);
	return 0;
}

uint32_t nfs4_op_layoutreturn(struct compound *compound)
{
	LAYOUTRETURN4args *args = NFS4_OP_ARG_SETUP(compound, oplayoutreturn);
	LAYOUTRETURN4res *res = NFS4_OP_RES_SETUP(compound, oplayoutreturn);
	nfsstat4 *status = &res->lorr_status;

	if (args->lora_layout_type != LAYOUT4_FLEX_FILES &&
	    args->lora_layout_type != LAYOUT4_FLEX_FILES_V2) {
		TRACE("LAYOUTRETURN: unknown layout type %d",
		      args->lora_layout_type);
		*status = NFS4ERR_UNKNOWN_LAYOUTTYPE;
		return 0;
	}

	/*
	 * LAYOUTRETURN4_FSID and LAYOUTRETURN4_ALL: bulk returns.
	 * For now, just acknowledge them without stateid tracking.
	 */
	if (args->lora_layoutreturn.lr_returntype != LAYOUTRETURN4_FILE) {
		TRACE("LAYOUTRETURN: non-FILE return type %d",
		      args->lora_layoutreturn.lr_returntype);
		res->LAYOUTRETURN4res_u.lorr_stateid.lrs_present = false;
		return 0;
	}

	layoutreturn_file4 *lrf =
		&args->lora_layoutreturn.layoutreturn4_u.lr_layout;

	if (network_file_handle_empty(&compound->c_curr_nfh) ||
	    !compound->c_inode) {
		TRACE("LAYOUTRETURN: no filehandle");
		*status = NFS4ERR_NOFILEHANDLE;
		return 0;
	}

	/*
	 * Find the layout stateid.  The client sends the stateid it
	 * received from LAYOUTGET.
	 */
	uint32_t seqid, id, type, cookie;

	unpack_stateid4(&lrf->lrf_stateid, &seqid, &id, &type, &cookie);

	TRACE("LAYOUTRETURN: stateid seqid=%u id=%u type=%u cookie=%u", seqid,
	      id, type, cookie);

	if (type != Layout_Stateid) {
		TRACE("LAYOUTRETURN: bad stateid type=%u (expected %u), "
		      "seqid=%u id=%u cookie=%u",
		      type, Layout_Stateid, seqid, id, cookie);
		*status = NFS4ERR_BAD_STATEID;
		return 0;
	}

	struct stateid *stid = stateid_find(compound->c_inode, id);

	if (!stid || stid->s_tag != Layout_Stateid ||
	    stid->s_cookie != cookie) {
		TRACE("LAYOUTRETURN: stateid lookup failed: stid=%p "
		      "tag=%u cookie=%u (expected cookie=%u) "
		      "seqid=%u id=%u ino=%lu",
		      (void *)stid, stid ? stid->s_tag : 0,
		      stid ? stid->s_cookie : 0, cookie, seqid, id,
		      compound->c_inode->i_ino);
		stateid_put(stid);
		*status = NFS4ERR_BAD_STATEID;
		return 0;
	}

	/* Verify client ownership. */
	if (compound->c_nfs4_client &&
	    stid->s_client != nfs4_client_to_client(compound->c_nfs4_client)) {
		TRACE("LAYOUTRETURN: client mismatch for stateid id=%u", id);
		stateid_put(stid);
		*status = NFS4ERR_BAD_STATEID;
		return 0;
	}

	struct layout_stateid *ls = stid_to_layout(stid);

	/*
	 * Clear the iomode bits for the returned mode.
	 * If both READ and RW are cleared, free the layout stateid.
	 */
	uint64_t clear_bit =
		(args->lora_iomode == LAYOUTIOMODE4_RW) ?
			LAYOUT_STATEID_IOMODE_RW :
		(args->lora_iomode == LAYOUTIOMODE4_READ) ?
			LAYOUT_STATEID_IOMODE_READ :
			(LAYOUT_STATEID_IOMODE_READ | LAYOUT_STATEID_IOMODE_RW);

	uint64_t remaining =
		__atomic_and_fetch(&ls->ls_state, ~clear_bit, __ATOMIC_ACQ_REL);

	if (remaining == 0) {
		/* No layouts left — free the stateid. */
		stateid_inode_unhash(stid);
		stateid_client_unhash(stid);
		stateid_put(stid); /* state ref → freed */

		/* Return no stateid (layout fully returned). */
		res->LAYOUTRETURN4res_u.lorr_stateid.lrs_present = false;
	} else {
		/* Still have a layout — bump seqid and return it. */
		__atomic_add_fetch(&stid->s_seqid, 1, __ATOMIC_RELAXED);
		res->LAYOUTRETURN4res_u.lorr_stateid.lrs_present = true;
		pack_stateid4(&res->LAYOUTRETURN4res_u.lorr_stateid
				       .layoutreturn_stateid_u.lrs_stateid,
			      stid);
	}

	TRACE("LAYOUTRETURN: ino=%lu iomode=%d remaining=0x%lx",
	      compound->c_inode->i_ino, args->lora_iomode,
	      (unsigned long)remaining);

	stateid_put(stid); /* find ref */

	/*
	 * If this was a write layout return, we need fresh attrs
	 * from the DSes (the client may have written without
	 * LAYOUTCOMMIT).  If there is a GETATTR later in this
	 * compound, it will handle the fan-out.  Otherwise, we
	 * trigger a reflected GETATTR now (async fan-out).
	 */
	if ((clear_bit & LAYOUT_STATEID_IOMODE_RW) &&
	    !(compound->c_flags & COMPOUND_DS_ATTRS_REFRESHED) &&
	    compound->c_inode->i_layout_segments &&
	    compound->c_inode->i_layout_segments->lss_count > 0) {
		struct layout_segment *seg =
			&compound->c_inode->i_layout_segments->lss_segs[0];
		struct dstore_fanout *df = dstore_fanout_alloc(seg->ls_nfiles);

		if (df) {
			df->df_op = FANOUT_GETATTR;
			int setup_ok = 1;

			for (uint32_t fi = 0; fi < seg->ls_nfiles; fi++) {
				struct layout_data_file *ldf =
					&seg->ls_files[fi];
				struct fanout_slot *slot = &df->df_slots[fi];

				slot->fs_ds = dstore_find(ldf->ldf_dstore_id);
				if (!slot->fs_ds) {
					setup_ok = 0;
					break;
				}
				memcpy(slot->fs_fh, ldf->ldf_fh,
				       ldf->ldf_fh_len);
				slot->fs_fh_len = ldf->ldf_fh_len;
				slot->fs_ldf = ldf;
			}

			if (setup_ok) {
				struct rpc_trans *rt = compound->c_rt;
				struct task *t = rt->rt_task;

				compound->c_flags |=
					COMPOUND_DS_ATTRS_REFRESHED;
				rt->rt_next_action =
					nfs4_op_layoutreturn_resume;
				rt->rt_async_data = df;
				task_pause(t);
				dstore_fanout_launch(df, t);
				return NFS4_OP_FLAG_ASYNC;
			}

			dstore_fanout_free(df);
		}
		/* Fan-out alloc/setup failed — continue without
		 * fresh attrs.  Not fatal. */
	}

	return 0;
}

uint32_t nfs4_op_getdevicelist(struct compound *compound)
{
	GETDEVICELIST4res *res = NFS4_OP_RES_SETUP(compound, opgetdevicelist);
	nfsstat4 *status = &res->gdlr_status;

	*status = NFS4ERR_NOTSUPP;

	return 0;
}

uint32_t nfs4_op_layouterror(struct compound *compound)
{
	LAYOUTERROR4args *args = NFS4_OP_ARG_SETUP(compound, oplayouterror);
	LAYOUTERROR4res *res = NFS4_OP_RES_SETUP(compound, oplayouterror);

	if (network_file_handle_empty(&compound->c_curr_nfh) ||
	    !compound->c_inode) {
		res->ler_status = NFS4ERR_NOFILEHANDLE;
		return 0;
	}

	struct layout_segments *lss = compound->c_inode->i_layout_segments;

	struct server_state *ss = server_state_find();
	uint32_t fence_min = ss ? ss->ss_fence_uid_min :
				  REFFS_FENCE_UID_MIN_DEFAULT;
	uint32_t fence_max = ss ? ss->ss_fence_uid_max :
				  REFFS_FENCE_UID_MAX_DEFAULT;

	/*
	 * Scan reported errors.  Record stats at three scopes
	 * (global, per-dstore, per-client), then on access errors
	 * fence and chmod all mirror instances.
	 */
	for (u_int i = 0; i < args->lea_errors.lea_errors_len; i++) {
		device_error4 *de = &args->lea_errors.lea_errors_val[i];
		uint32_t ds_id = deviceid_to_dstore(de->de_deviceid);

		TRACE("LAYOUTERROR: ino=%lu dev=%u status=%d op=%d",
		      compound->c_inode->i_ino, ds_id, de->de_status,
		      de->de_opnum);

		/*
		 * Accumulate layout error stats: global, per-dstore,
		 * and per-client.  Categorize by error type.
		 */
		struct reffs_layout_error_stats *scopes[3];
		int nscopes = 0;

		if (ss)
			scopes[nscopes++] = &ss->ss_layout_errors;

		struct dstore *err_ds = dstore_find(ds_id);

		if (err_ds)
			scopes[nscopes++] = &err_ds->ds_layout_errors;

		struct nfs4_client *nc = compound->c_nfs4_client;

		if (nc)
			scopes[nscopes++] = &nc->nc_layout_errors;

		for (int s = 0; s < nscopes; s++) {
			atomic_fetch_add_explicit(&scopes[s]->les_total, 1,
						  memory_order_relaxed);
			if (de->de_status == NFS4ERR_ACCESS ||
			    de->de_status == NFS4ERR_PERM)
				atomic_fetch_add_explicit(
					&scopes[s]->les_access, 1,
					memory_order_relaxed);
			else if (de->de_status == NFS4ERR_IO)
				atomic_fetch_add_explicit(&scopes[s]->les_io, 1,
							  memory_order_relaxed);
			else
				atomic_fetch_add_explicit(&scopes[s]->les_other,
							  1,
							  memory_order_relaxed);
		}

		dstore_put(err_ds);

		if ((de->de_status == NFS4ERR_ACCESS ||
		     de->de_status == NFS4ERR_PERM) &&
		    lss && lss->lss_count > 0) {
			struct layout_segment *seg = &lss->lss_segs[0];

			for (uint32_t f = 0; f < seg->ls_nfiles; f++) {
				struct layout_data_file *ldf =
					&seg->ls_files[f];
				struct dstore *ds =
					dstore_find(ldf->ldf_dstore_id);
				if (!ds)
					continue;

				dstore_data_file_fence(ds, ldf->ldf_fh,
						       ldf->ldf_fh_len, ldf,
						       fence_min, fence_max,
						       NULL);
				dstore_data_file_chmod(ds, ldf->ldf_fh,
						       ldf->ldf_fh_len, NULL);
				dstore_put(ds);
			}

			inode_sync_to_disk(compound->c_inode);
			LOG("LAYOUTERROR: fenced + chmod all instances "
			    "for ino=%lu (access error from DS)",
			    compound->c_inode->i_ino);
		}
	}

	server_state_put(ss);

	return 0;
}

uint32_t nfs4_op_layoutstats(struct compound *compound)
{
	LAYOUTSTATS4res *res = NFS4_OP_RES_SETUP(compound, oplayoutstats);
	nfsstat4 *status = &res->lsr_status;

	*status = NFS4ERR_NOTSUPP;

	return 0;
}
