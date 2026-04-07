/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <arpa/inet.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

	if (args->gdia_layout_type != LAYOUT4_NFSV4_1_FILES &&
	    args->gdia_layout_type != LAYOUT4_FLEX_FILES &&
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
	 * The universal address format for TCP/IPv4 is:
	 *   "h1.h2.h3.h4.p1.p2" where p1.p2 is port 2049 = 8.1
	 */
	char uaddr[64];

	snprintf(uaddr, sizeof(uaddr), "%s.8.1", ds->ds_ip);

	netaddr4 na;
	char *netid = "tcp";

	na.na_r_netid = netid;
	na.na_r_addr = uaddr;

	/* File layouts: nfsv4_1_file_layout_ds_addr4 */
	nfsv4_1_file_layout_ds_addr4 flda;
	uint32_t stripe_idx = 0;
	multipath_list4 mpl;

	/* Flex files: ff_device_addr4 */
	ff_device_addr4 ffda;
	ff_device_versions4 ver;

	if (args->gdia_layout_type == LAYOUT4_NFSV4_1_FILES) {
		/*
		 * File layout device address: stripe indices map 1:1
		 * to the multipath DS list (one DS per stripe index).
		 */
		memset(&flda, 0, sizeof(flda));

		mpl.multipath_list4_len = 1;
		mpl.multipath_list4_val = &na;

		flda.nflda_stripe_indices.nflda_stripe_indices_len = 1;
		flda.nflda_stripe_indices.nflda_stripe_indices_val =
			&stripe_idx;

		flda.nflda_multipath_ds_list.nflda_multipath_ds_list_len = 1;
		flda.nflda_multipath_ds_list.nflda_multipath_ds_list_val = &mpl;

	} else {
		/*
		 * Flex files device address: multipath netaddrs + version.
		 */
		memset(&ffda, 0, sizeof(ffda));

		ffda.ffda_netaddrs.multipath_list4_len = 1;
		ffda.ffda_netaddrs.multipath_list4_val = &na;

		memset(&ver, 0, sizeof(ver));
		ver.ffdv_version = 3;
		ver.ffdv_minorversion = 0;
		ver.ffdv_rsize = 1048576;
		ver.ffdv_wsize = 1048576;
		ver.ffdv_tightly_coupled = ds->ds_tight_coupled;

		ffda.ffda_versions.ffda_versions_len = 1;
		ffda.ffda_versions.ffda_versions_val = &ver;
	}

	/* XDR-encode into an opaque buffer. */
	u_long xdr_size;
	bool encode_ok;

	if (args->gdia_layout_type == LAYOUT4_NFSV4_1_FILES)
		xdr_size = xdr_sizeof(
			(xdrproc_t)xdr_nfsv4_1_file_layout_ds_addr4, &flda);
	else
		xdr_size = xdr_sizeof((xdrproc_t)xdr_ff_device_addr4, &ffda);

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
	if (args->gdia_layout_type == LAYOUT4_NFSV4_1_FILES)
		encode_ok = xdr_nfsv4_1_file_layout_ds_addr4(&xdrs, &flda);
	else
		encode_ok = xdr_ff_device_addr4(&xdrs, &ffda);
	if (!encode_ok) {
		xdr_destroy(&xdrs);
		free(resok->gdir_device_addr.da_addr_body.da_addr_body_val);
		resok->gdir_device_addr.da_addr_body.da_addr_body_val = NULL;
		dstore_put(ds);
		*status = NFS4ERR_SERVERFAULT;
		return 0;
	}
	xdr_destroy(&xdrs);

	TRACE("GETDEVICEINFO: dstore[%u] addr=%s type=%u", dstore_id, uaddr,
	      args->gdia_layout_type);
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
			/* Lost race -- retry. */
			rcu_read_lock();
			cds_lfht_first(inode->i_stateids, &iter);
			continue;
		}
		cds_lfht_next(inode->i_stateids, &iter);
	}
	rcu_read_unlock();

	/* None found -- allocate a new one. */
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
/* Layout body builders -- one per layout type.                         */
/*                                                                     */
/* Each builder XDR-encodes the layout body and returns it in *body.   */
/* The caller owns the allocated buffer.  Returns NFS4_OK or an error. */
/* ------------------------------------------------------------------ */

/*
 * File layout (RFC 5661 S13): single device, stripe across DSes.
 * nfl_util encodes the stripe size and DENSE/SPARSE commit model.
 * nfl_fh_list has one FH per DS in the stripe -- the client uses
 * (offset / stripe_unit) % nfl_fh_list_len to pick the DS.
 */
static nfsstat4 layoutget_build_file(struct layout_segment *seg,
				     char **out_body, u_long *out_size)
{
	nfsv4_1_file_layout4 nfl;

	memset(&nfl, 0, sizeof(nfl));

	/* All data files share a single device (one DS). */
	deviceid_from_dstore(nfl.nfl_deviceid, seg->ls_files[0].ldf_dstore_id);

	/*
	 * nfl_util: stripe size in low 31 bits.  Bit 31 = DENSE mode.
	 * Use SPARSE (bit 31 clear) -- each DS FH maps to a separate
	 * file, offsets on the DS match the MDS file offsets within
	 * the stripe.
	 *
	 * Stripe size 0 means entire file on one DS (no striping).
	 */
	uint32_t stripe_size = seg->ls_stripe_unit;

	if (stripe_size == 0)
		stripe_size = 1048576; /* 1MB default stripe */
	nfl.nfl_util = stripe_size; /* SPARSE mode (bit 31 clear) */
	nfl.nfl_first_stripe_index = 0;
	nfl.nfl_pattern_offset = 0;

	/* One FH per data file in the stripe. */
	nfl.nfl_fh_list.nfl_fh_list_len = seg->ls_nfiles;
	nfl.nfl_fh_list.nfl_fh_list_val =
		calloc(seg->ls_nfiles, sizeof(nfs_fh4));
	if (!nfl.nfl_fh_list.nfl_fh_list_val)
		return NFS4ERR_DELAY;

	for (uint32_t i = 0; i < seg->ls_nfiles; i++) {
		struct layout_data_file *ldf = &seg->ls_files[i];
		nfs_fh4 *fh = &nfl.nfl_fh_list.nfl_fh_list_val[i];

		fh->nfs_fh4_len = ldf->ldf_fh_len;
		fh->nfs_fh4_val = calloc(1, ldf->ldf_fh_len);
		if (!fh->nfs_fh4_val) {
			for (uint32_t j = 0; j < i; j++)
				free(nfl.nfl_fh_list.nfl_fh_list_val[j]
					     .nfs_fh4_val);
			free(nfl.nfl_fh_list.nfl_fh_list_val);
			return NFS4ERR_DELAY;
		}
		memcpy(fh->nfs_fh4_val, ldf->ldf_fh, ldf->ldf_fh_len);
	}

	u_long xdr_size = xdr_sizeof((xdrproc_t)xdr_nfsv4_1_file_layout4, &nfl);
	char *body = calloc(1, xdr_size);

	if (!body) {
		for (uint32_t i = 0; i < seg->ls_nfiles; i++)
			free(nfl.nfl_fh_list.nfl_fh_list_val[i].nfs_fh4_val);
		free(nfl.nfl_fh_list.nfl_fh_list_val);
		return NFS4ERR_DELAY;
	}

	XDR xdrs;

	xdrmem_create(&xdrs, body, xdr_size, XDR_ENCODE);
	if (!xdr_nfsv4_1_file_layout4(&xdrs, &nfl)) {
		xdr_destroy(&xdrs);
		free(body);
		for (uint32_t i = 0; i < seg->ls_nfiles; i++)
			free(nfl.nfl_fh_list.nfl_fh_list_val[i].nfs_fh4_val);
		free(nfl.nfl_fh_list.nfl_fh_list_val);
		return NFS4ERR_SERVERFAULT;
	}
	xdr_destroy(&xdrs);

	*out_body = body;
	*out_size = xdr_size;

	for (uint32_t i = 0; i < seg->ls_nfiles; i++)
		free(nfl.nfl_fh_list.nfl_fh_list_val[i].nfs_fh4_val);
	free(nfl.nfl_fh_list.nfl_fh_list_val);
	return NFS4_OK;
}

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
		mirror->ffm_coding_type = FFV2_CODING_MIRRORED;
	else
		mirror->ffm_coding_type = FFV2_ENCODING_RS_VANDERMONDE;
	mirror->ffm_protection.fdp_data = seg->ls_k;
	mirror->ffm_protection.fdp_parity = seg->ls_m;

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
/* LAYOUTGET trust resume                                              */
/* ------------------------------------------------------------------ */

/*
 * nfs4_op_layoutget_trust_resume - resume after TRUST_STATEID fan-out.
 *
 * Best-effort: if the fan-out failed (DS unreachable or returned an
 * error), log and proceed -- the layout response has already been built
 * and the client will use anonymous stateids as a fallback.  Clearing
 * ds_tight_coupled on NFS4ERR_NOTSUPP is NOT_NOW_BROWN_COW.
 */
uint32_t nfs4_op_layoutget_trust_resume(struct rpc_trans *rt)
{
	struct dstore_fanout *df = rt->rt_async_data;

	rt->rt_async_data = NULL;

	int result = dstore_fanout_result(df);

	if (result != 0)
		TRACE("LAYOUTGET: TRUST_STATEID fan-out failed (%d) -- "
		      "client falls back to anonymous stateid",
		      result);

	dstore_fanout_free(df);
	return 0;
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

	if (args->loga_layout_type != LAYOUT4_NFSV4_1_FILES &&
	    args->loga_layout_type != LAYOUT4_FLEX_FILES &&
	    args->loga_layout_type != LAYOUT4_FLEX_FILES_V2) {
		*status = NFS4ERR_UNKNOWN_LAYOUTTYPE;
		return 0;
	}

	layouttype4 layout_type = args->loga_layout_type;

	if (!compound->c_inode) {
		*status = NFS4ERR_NOFILEHANDLE;
		return 0;
	}

	/*
	 * Per-export layout policy: only grant layouts for exports
	 * that have the requested layout type enabled.  The root
	 * export (/) and per-flavor exports (/krb5, /sys, etc.)
	 * default to sb_layout_types=0 (no layouts).  Only /ffv1
	 * and /ffv2 get layouts via explicit configuration.
	 */
	{
		struct super_block *sb = compound->c_inode->i_sb;
		uint32_t want = 0;

		if (layout_type == LAYOUT4_NFSV4_1_FILES)
			want = SB_LAYOUT_FILE;
		else if (layout_type == LAYOUT4_FLEX_FILES)
			want = SB_LAYOUT_FLEX_FILES;
		else if (layout_type == LAYOUT4_FLEX_FILES_V2)
			want = SB_LAYOUT_FLEX_FILES_V2;

		if (!(sb->sb_layout_types & want)) {
			*status = NFS4ERR_LAYOUTUNAVAILABLE;
			return 0;
		}
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
		struct super_block *isb = compound->c_inode->i_sb;

		/*
		 * Per-export dstore binding: use the export's
		 * configured dstores if set, else fall back to the
		 * global pool for backward compatibility.
		 */
		if (isb->sb_ndstores > 0) {
			nds = 0;
			for (uint32_t d = 0;
			     d < isb->sb_ndstores && d < LAYOUT_SEG_MAX_FILES;
			     d++) {
				struct dstore *ds =
					dstore_find(isb->sb_dstore_ids[d]);
				if (ds)
					dstores[nds++] = ds;
			}
		} else {
			nds = dstore_collect_available(dstores,
						       LAYOUT_SEG_MAX_FILES);
		}
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
		struct server_state *ss = compound->c_server_state;
		uint32_t target;

		/*
		 * File layouts: one FH per DS (no mirroring).
		 * Flex files: mirror across multiple FHs per layout_width.
		 */
		if (layout_type == LAYOUT4_NFSV4_1_FILES)
			target = nds;
		else
			target = ss->ss_layout_width ?
					 ss->ss_layout_width :
					 REFFS_LAYOUT_WIDTH_DEFAULT;
		uint32_t fence_min = ss->ss_fence_uid_min;
		uint32_t fence_max = ss->ss_fence_uid_max;

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
			.ls_layout_type = layout_type,
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

		static _Atomic bool first_layout = true;

		if (atomic_exchange(&first_layout, false)) {
			TRACE("NFSv4.2 Flex File v2 Layout Driver: "
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

	if (seg->ls_layout_type == LAYOUT4_NFSV4_1_FILES)
		*status = layoutget_build_file(seg, &body, &xdr_size);
	else if (seg->ls_layout_type == LAYOUT4_FLEX_FILES_V2)
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
	lo->lo_content.loc_type = seg->ls_layout_type;
	lo->lo_content.loc_body.loc_body_val = body;
	lo->lo_content.loc_body.loc_body_len = (u_int)xdr_size;

	/*
	 * Tight-coupling: propagate the layout stateid to each DS that
	 * supports TRUST_STATEID (ds_tight_coupled).  This is best-effort
	 * -- the layout is granted regardless of whether propagation
	 * succeeds.  If the fan-out fails, the client falls back to using
	 * the anonymous stateid for CHUNK I/O.
	 *
	 * Expire time: wall-clock "now + lease_time" so the DS trust entry
	 * expires with the lease.
	 */
	uint32_t ntight = 0;

	for (uint32_t f = 0; f < seg->ls_nfiles; f++) {
		struct dstore *tds =
			dstore_find(seg->ls_files[f].ldf_dstore_id);
		if (tds) {
			if (tds->ds_tight_coupled)
				ntight++;
			dstore_put(tds);
		}
	}

	if (ntight > 0) {
		struct dstore_fanout *df = dstore_fanout_alloc(ntight);

		if (df) {
			struct server_state *tss = compound->c_server_state;
			uint32_t lease_sec = tss ? server_lease_time(tss) : 90;
			struct timespec wall_now;

			clock_gettime(CLOCK_REALTIME, &wall_now);

			df->df_op = FANOUT_TRUST_STATEID;
			df->df_ts_seqid = resok->logr_stateid.seqid;
			memcpy(df->df_ts_other, resok->logr_stateid.other,
			       sizeof(df->df_ts_other));
			df->df_ts_iomode = (uint32_t)args->loga_iomode;
			df->df_ts_expire_sec =
				(int64_t)wall_now.tv_sec + (int64_t)lease_sec;
			df->df_ts_expire_nsec = (uint32_t)wall_now.tv_nsec;
			/*
			 * Record the layout client's clientid4 so the local
			 * dstore vtable can associate trust entries correctly.
			 * client->c_id == clientid4 in the NFSv4 layer.
			 */
			df->df_ts_clientid =
				compound->c_nfs4_client ?
					(uint64_t)nfs4_client_to_client(
						compound->c_nfs4_client)
						->c_id :
					0;
			df->df_ts_principal[0] =
				'\0'; /* NOT_NOW_BROWN_COW: GSS */

			uint32_t fi = 0;
			int setup_ok = 1;

			for (uint32_t f = 0; f < seg->ls_nfiles && fi < ntight;
			     f++) {
				struct layout_data_file *ldf =
					&seg->ls_files[f];
				struct dstore *tds =
					dstore_find(ldf->ldf_dstore_id);

				if (!tds)
					continue;
				if (!tds->ds_tight_coupled) {
					dstore_put(tds);
					continue;
				}

				struct fanout_slot *slot = &df->df_slots[fi++];

				slot->fs_ds =
					tds; /* fanout_free will dstore_put */
				memcpy(slot->fs_fh, ldf->ldf_fh,
				       ldf->ldf_fh_len);
				slot->fs_fh_len = ldf->ldf_fh_len;
				slot->fs_ldf = ldf;
			}

			/*
			 * If any tight-coupled dstore disappeared between the
			 * count loop and the setup loop, some slots are NULL.
			 * Abort the fan-out -- better to skip trust propagation
			 * than to crash in a fanout thread on a NULL dstore.
			 */
			if (fi != ntight)
				setup_ok = 0;

			if (setup_ok) {
				struct rpc_trans *rt = compound->c_rt;
				struct task *t = rt->rt_task;

				rt->rt_next_action =
					nfs4_op_layoutget_trust_resume;
				rt->rt_async_data = df;
				task_pause(t);
				dstore_fanout_launch(df, t);

				stateid_put(&ls->ls_stid);
				return NFS4_OP_FLAG_ASYNC;
			}

			dstore_fanout_free(df);
		}
		/* Fan-out alloc/setup failed -- proceed without trust. */
	}

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
		if ((int64_t)new_end > compound->c_inode->i_size) {
			compound->c_inode->i_size = (int64_t)new_end;
			resok->locr_newsize.ns_sizechanged = true;
			resok->locr_newsize.newsize4_u.ns_size = new_end;
		}
		pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);
	}

	inode_sync_to_disk(compound->c_inode);

	return 0;
}

/*
 * nfs4_layout_implicit_return_rw -- implicitly return the write layout
 * for this client on compound->c_inode.
 *
 * Called from CLOSE (RFC 8881 S12.5.5.1 SHOULD) and DELEGRETURN when
 * the client did not send an explicit LAYOUTRETURN.  Finds the layout
 * stateid owned by this client, clears LAYOUT_STATEID_IOMODE_RW, frees
 * the stateid if no iomode bits remain, then launches a T2 reflected
 * GETATTR to refresh cached size and mtime.
 *
 * Returns NFS4_OP_FLAG_ASYNC if a fan-out was launched and resume_fn
 * has been set as rt_next_action.  Returns 0 if there was no write
 * layout or the fan-out could not be set up (non-fatal).
 */
uint32_t
nfs4_layout_implicit_return_rw(struct compound *compound,
			       uint32_t (*resume_fn)(struct rpc_trans *))
{
	struct inode *inode = compound->c_inode;
	struct client *client =
		compound->c_nfs4_client ?
			nfs4_client_to_client(compound->c_nfs4_client) :
			NULL;

	if (!inode || !inode->i_stateids || !client)
		return 0;

	/*
	 * Find the layout stateid for this client.  Use stateid_get()
	 * inside rcu_read_lock to take a ref before we drop the lock.
	 */
	struct stateid *ls_stid = NULL;
	struct layout_stateid *ls = NULL;

	rcu_read_lock();
	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;

	cds_lfht_first(inode->i_stateids, &iter);
	while ((node = cds_lfht_iter_get_node(&iter)) != NULL) {
		struct stateid *stid =
			caa_container_of(node, struct stateid, s_inode_node);
		if (stid->s_tag == Layout_Stateid && stid->s_client == client) {
			struct layout_stateid *candidate = stid_to_layout(stid);
			uint64_t state = __atomic_load_n(&candidate->ls_state,
							 __ATOMIC_ACQUIRE);

			if (state & LAYOUT_STATEID_IOMODE_RW) {
				struct stateid *got = stateid_get(stid);

				if (got) {
					ls_stid = got;
					ls = candidate;
				}
				/* If got is NULL the stateid is dying --
				 * concurrent explicit LAYOUTRETURN beat us. */
			}
			break; /* at most one layout stateid per client/inode */
		}
		cds_lfht_next(inode->i_stateids, &iter);
	}
	rcu_read_unlock();

	if (!ls)
		return 0; /* no write layout to implicitly return */

	uint64_t remaining = __atomic_and_fetch(
		&ls->ls_state, ~LAYOUT_STATEID_IOMODE_RW, __ATOMIC_ACQ_REL);

	TRACE("implicit LAYOUTRETURN(RW): ino=%lu remaining=0x%lx",
	      inode->i_ino, (unsigned long)remaining);

	if (remaining == 0) {
		stateid_inode_unhash(ls_stid);
		stateid_client_unhash(ls_stid);
		stateid_put(ls_stid); /* state ref --> freed via RCU */
	}

	stateid_put(ls_stid); /* find ref from stateid_get() */

	/*
	 * T2 trigger: launch a reflected GETATTR to refresh cached
	 * size/mtime from the DSes.
	 */
	if (!(compound->c_flags & COMPOUND_DS_ATTRS_REFRESHED) &&
	    inode->i_layout_segments &&
	    inode->i_layout_segments->lss_count > 0) {
		struct layout_segment *seg =
			&inode->i_layout_segments->lss_segs[0];
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
				rt->rt_next_action = resume_fn;
				rt->rt_async_data = df;
				task_pause(t);
				dstore_fanout_launch(df, t);
				return NFS4_OP_FLAG_ASYNC;
			}

			dstore_fanout_free(df);
		}
		/* Fan-out alloc/setup failed -- non-fatal, continue. */
	}

	return 0;
}

/*
 * nfs4_op_layoutreturn_resume - resume callback after reflected GETATTR fan-out.
 *
 * Updates the inode's cached size/mtime from the DS responses and frees
 * the fanout.  Shared by LAYOUTRETURN, CLOSE (G3 implicit LR), and
 * DELEGRETURN (G4 implicit LR).
 */
uint32_t nfs4_op_layoutreturn_resume(struct rpc_trans *rt)
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

			/* Update space accounting -- MDS inode has no
			 * local data block, data lives on the DS. */
			struct super_block *sb = inode->i_sb;
			int64_t old_used = inode->i_used;

			inode->i_used =
				inode->i_size / sb->sb_block_size +
				(inode->i_size % sb->sb_block_size ? 1 : 0);

			int64_t delta =
				(inode->i_used - old_used) * sb->sb_block_size;
			if (delta > 0)
				atomic_fetch_add_explicit(&sb->sb_bytes_used,
							  (size_t)delta,
							  memory_order_relaxed);
			else if (delta < 0)
				atomic_fetch_sub_explicit(&sb->sb_bytes_used,
							  (size_t)(-delta),
							  memory_order_relaxed);
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

	if (args->lora_layout_type != LAYOUT4_NFSV4_1_FILES &&
	    args->lora_layout_type != LAYOUT4_FLEX_FILES &&
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
		/* No layouts left -- free the stateid. */
		stateid_inode_unhash(stid);
		stateid_client_unhash(stid);
		stateid_put(stid); /* state ref --> freed */

		/* Return no stateid (layout fully returned). */
		res->LAYOUTRETURN4res_u.lorr_stateid.lrs_present = false;
	} else {
		/* Still have a layout -- bump seqid and return it. */
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
		/* Fan-out alloc/setup failed -- continue without
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

/*
 * find_layout_stateid_for_client -- walk inode's stateid table and
 * return a ref-bumped Layout_Stateid for the given client, or NULL.
 *
 * Caller must stateid_put() the returned stateid when done.
 */
static struct stateid *find_layout_stateid_for_client(struct inode *inode,
						      struct nfs4_client *nc)
{
	if (!inode->i_stateids || !nc)
		return NULL;

	struct client *cl = nfs4_client_to_client(nc);
	struct stateid *ls_stid = NULL;
	struct cds_lfht_iter it;
	struct cds_lfht_node *n;

	rcu_read_lock();
	cds_lfht_first(inode->i_stateids, &it);
	while ((n = cds_lfht_iter_get_node(&it)) != NULL) {
		struct stateid *st =
			caa_container_of(n, struct stateid, s_inode_node);
		if (st->s_tag == Layout_Stateid && st->s_client == cl) {
			ls_stid = stateid_get(st);
			break;
		}
		cds_lfht_next(inode->i_stateids, &it);
	}
	rcu_read_unlock();

	return ls_stid;
}

/*
 * layouterror_fence_and_revoke -- revoke the layout stateid on all
 * tight-coupled DSes, then fence + chmod all mirror instances.
 *
 * Called for NFS4ERR_ACCESS / NFS4ERR_PERM from LAYOUTERROR, and as
 * a fallback when BAD_STATEID trust-gap recovery fails.
 */
static void layouterror_fence_and_revoke(struct compound *compound,
					 struct server_state *ss,
					 struct layout_segments *lss)
{
	struct layout_segment *seg = &lss->lss_segs[0];
	uint32_t fence_min = ss->ss_fence_uid_min;
	uint32_t fence_max = ss->ss_fence_uid_max;

	for (uint32_t f = 0; f < seg->ls_nfiles; f++) {
		struct layout_data_file *ldf = &seg->ls_files[f];
		struct dstore *ds = dstore_find(ldf->ldf_dstore_id);

		if (!ds)
			continue;

		/*
		 * Tight-coupling: revoke the layout stateid on the DS
		 * before fencing so the DS stops accepting I/O on the
		 * old credential.  Best-effort -- fencing proceeds even
		 * if the revoke fails.
		 */
		if (ds->ds_tight_coupled) {
			struct stateid *ls_stid = find_layout_stateid_for_client(
				compound->c_inode, compound->c_nfs4_client);
			if (ls_stid) {
				stateid4 w;

				pack_stateid4(&w, ls_stid);
				dstore_revoke_stateid(ds, ldf->ldf_fh,
						      ldf->ldf_fh_len, w.seqid,
						      (const uint8_t *)w.other);
				stateid_put(ls_stid);
			}
		}

		dstore_data_file_fence(ds, ldf->ldf_fh, ldf->ldf_fh_len, ldf,
				       fence_min, fence_max, NULL);
		dstore_data_file_chmod(ds, ldf->ldf_fh, ldf->ldf_fh_len, NULL);
		dstore_put(ds);
	}

	inode_sync_to_disk(compound->c_inode);
	LOG("LAYOUTERROR: fenced + chmod all instances for ino=%lu",
	    compound->c_inode->i_ino);
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
	struct server_state *ss = compound->c_server_state;

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
		 * per-client, and per-sb.  Categorize by error type.
		 */
		struct reffs_layout_error_stats *scopes[4];
		int nscopes = 0;

		if (ss)
			scopes[nscopes++] = &ss->ss_layout_errors;

		struct dstore *err_ds = dstore_find(ds_id);

		if (err_ds)
			scopes[nscopes++] = &err_ds->ds_layout_errors;

		struct nfs4_client *nc = compound->c_nfs4_client;

		if (nc)
			scopes[nscopes++] = &nc->nc_layout_errors;

		if (compound->c_curr_sb)
			scopes[nscopes++] =
				&compound->c_curr_sb->sb_layout_errors;

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

		if (de->de_status == NFS4ERR_BAD_STATEID && lss &&
		    lss->lss_count > 0) {
			/*
			 * Trust-gap recovery: the DS rejected a CHUNK op
			 * because the layout stateid is not in its trust
			 * table (e.g., DS was restarted).  Re-issue
			 * TRUST_STATEID to all tight-coupled DSes.
			 *
			 * If ALL tight-coupled DSes accept, the gap is
			 * healed and LAYOUTERROR returns NFS4_OK.
			 * If ANY fail, fall through to fence+revoke.
			 * If there are no tight-coupled DSes, skip.
			 */
			struct layout_segment *seg = &lss->lss_segs[0];
			struct stateid *ls_stid =
				find_layout_stateid_for_client(
					compound->c_inode, nc);

			if (!ls_stid) {
				/*
				 * No layout stateid on this inode for this
				 * client; nothing to re-register.  Skip.
				 */
				continue;
			}

			stateid4 w;

			pack_stateid4(&w, ls_stid);
			stateid_put(ls_stid);

			uint64_t clientid =
				nc ? (uint64_t)nfs4_client_to_client(nc)->c_id :
				     0;

			/*
			 * Compute a fresh expiry: wall-clock now +
			 * one lease period.  The DS stores it as a
			 * CLOCK_MONOTONIC deadline internally.
			 */
			struct timespec wall_now;

			clock_gettime(CLOCK_REALTIME, &wall_now);
			int64_t expire_sec = (int64_t)wall_now.tv_sec +
					     (int64_t)server_lease_time(ss);
			uint32_t expire_nsec = (uint32_t)wall_now.tv_nsec;

			int ntight = 0;
			int nfailed = 0;

			for (uint32_t f = 0; f < seg->ls_nfiles; f++) {
				struct layout_data_file *ldf =
					&seg->ls_files[f];
				struct dstore *ds =
					dstore_find(ldf->ldf_dstore_id);

				if (!ds)
					continue;

				if (!ds->ds_tight_coupled) {
					dstore_put(ds);
					continue;
				}

				ntight++;
				int ret = dstore_trust_stateid(
					ds, ldf->ldf_fh, ldf->ldf_fh_len,
					w.seqid, (const uint8_t *)w.other,
					LAYOUTIOMODE4_RW, clientid, expire_sec,
					expire_nsec,
					"" /* NOT_NOW_BROWN_COW: GSS */);
				if (ret != 0)
					nfailed++;

				dstore_put(ds);
			}

			if (ntight == 0) {
				/* No tight-coupled DSes; nothing to do. */
				continue;
			}

			if (nfailed == 0) {
				TRACE("LAYOUTERROR: trust gap healed for "
				      "ino=%lu (BAD_STATEID re-registered "
				      "on %d DS(es))",
				      compound->c_inode->i_ino, ntight);
				continue; /* NFS4_OK; gap healed */
			}

			/*
			 * At least one tight-coupled DS rejected
			 * TRUST_STATEID.  Fall through to fence+revoke
			 * to invalidate the client's access entirely.
			 */
			LOG("LAYOUTERROR: trust-gap recovery failed "
			    "(%d/%d DS(es)) for ino=%lu; "
			    "fencing",
			    nfailed, ntight, compound->c_inode->i_ino);
			layouterror_fence_and_revoke(compound, ss, lss);

		} else if ((de->de_status == NFS4ERR_ACCESS ||
			    de->de_status == NFS4ERR_PERM) &&
			   lss && lss->lss_count > 0) {
			/*
			 * DS rejected the client's I/O.
			 * layouterror_fence_and_revoke logs the fence action.
			 */
			layouterror_fence_and_revoke(compound, ss, lss);
		}
	}

	return 0;
}

uint32_t nfs4_op_layoutstats(struct compound *compound)
{
	LAYOUTSTATS4res *res = NFS4_OP_RES_SETUP(compound, oplayoutstats);
	nfsstat4 *status = &res->lsr_status;

	*status = NFS4ERR_NOTSUPP;

	return 0;
}
