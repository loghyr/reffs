/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * Layout operations for the EC demo client.
 *
 * LAYOUTGET returns a Flex Files layout with mirror/device info.
 * GETDEVICEINFO resolves device IDs to data server addresses.
 * LAYOUTRETURN returns the layout when done.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rpc/rpc.h>
#include <rpc/xdr.h>

#include "nfsv42_xdr.h"
#include "ec_client.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/*
 * Parse a universal address "h1.h2.h3.h4.p1.p2" into host:port.
 */
static int parse_uaddr(const char *uaddr, char *host, size_t hostsz,
		       uint16_t *port)
{
	unsigned int h1, h2, h3, h4, p1, p2;

	if (sscanf(uaddr, "%u.%u.%u.%u.%u.%u", &h1, &h2, &h3, &h4, &p1, &p2) !=
	    6)
		return -EINVAL;

	snprintf(host, hostsz, "%u.%u.%u.%u", h1, h2, h3, h4);
	*port = (uint16_t)((p1 << 8) | p2);
	return 0;
}

/*
 * Parse a numeric UID/GID string (e.g., "1024") to uint32_t.
 */
static uint32_t parse_owner_id(const char *str, uint32_t len)
{
	char buf[32];
	uint32_t copy_len = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;

	memcpy(buf, str, copy_len);
	buf[copy_len] = '\0';
	return (uint32_t)strtoul(buf, NULL, 10);
}

/* ------------------------------------------------------------------ */
/* LAYOUTGET                                                           */
/* ------------------------------------------------------------------ */

int mds_layout_get(struct mds_session *ms, struct mds_file *mf,
		   layoutiomode4 iomode, layouttype4 layout_type,
		   struct ec_layout *layout)
{
	struct mds_compound mc;
	nfs_argop4 *slot;
	int ret;

	memset(layout, 0, sizeof(*layout));

	/* SEQUENCE + PUTFH + LAYOUTGET = 3 ops */
	ret = mds_compound_init(&mc, 3, "layoutget");
	if (ret)
		return ret;

	ret = mds_compound_add_sequence(&mc, ms);
	if (ret) {
		mds_compound_fini(&mc);
		return ret;
	}

	slot = mds_compound_add_op(&mc, OP_PUTFH);
	if (!slot) {
		mds_compound_fini(&mc);
		return -ENOSPC;
	}
	slot->nfs_argop4_u.opputfh.object = mf->mf_fh;

	slot = mds_compound_add_op(&mc, OP_LAYOUTGET);
	if (!slot) {
		mds_compound_fini(&mc);
		return -ENOSPC;
	}

	LAYOUTGET4args *lg_args = &slot->nfs_argop4_u.oplayoutget;

	lg_args->loga_signal_layout_avail = false;
	lg_args->loga_layout_type = layout_type;
	lg_args->loga_iomode = iomode;
	lg_args->loga_offset = 0;
	lg_args->loga_length = 0xFFFFFFFFFFFFFFFFULL; /* entire file */
	lg_args->loga_minlength = 0;
	memcpy(&lg_args->loga_stateid, &mf->mf_stateid, sizeof(stateid4));
	lg_args->loga_maxcount = 65536;

	ret = mds_compound_send(&mc, ms);
	if (ret) {
		mds_compound_fini(&mc);
		return ret;
	}

	/* Parse LAYOUTGET result (op index 2). */
	nfs_resop4 *lg_res = mds_compound_result(&mc, 2);

	if (!lg_res ||
	    lg_res->nfs_resop4_u.oplayoutget.logr_status != NFS4_OK) {
		mds_compound_fini(&mc);
		return -EREMOTEIO;
	}

	LAYOUTGET4resok *resok =
		&lg_res->nfs_resop4_u.oplayoutget.LAYOUTGET4res_u.logr_resok4;

	memcpy(&layout->el_stateid, &resok->logr_stateid, sizeof(stateid4));
	layout->el_layout_type = layout_type;

	if (resok->logr_layout.logr_layout_len < 1) {
		mds_compound_fini(&mc);
		return -ENODATA;
	}

	layout4 *lo = &resok->logr_layout.logr_layout_val[0];
	layout_content4 *loc = &lo->lo_content;
	XDR xdrs;

	if (layout_type == LAYOUT4_FLEX_FILES_V2) {
		/* ---- Flex Files v2 ---- */
		ffv2_layout4 ffl;

		memset(&ffl, 0, sizeof(ffl));
		xdrmem_create(&xdrs, loc->loc_body.loc_body_val,
			      loc->loc_body.loc_body_len, XDR_DECODE);
		if (!xdr_ffv2_layout4(&xdrs, &ffl)) {
			xdr_destroy(&xdrs);
			mds_compound_fini(&mc);
			return -EIO;
		}
		xdr_destroy(&xdrs);

		if (ffl.ffl_mirrors.ffl_mirrors_len < 1) {
			xdr_free((xdrproc_t)xdr_ffv2_layout4, (caddr_t)&ffl);
			mds_compound_fini(&mc);
			return -ENODATA;
		}

		ffv2_mirror4 *m0 = &ffl.ffl_mirrors.ffl_mirrors_val[0];

		layout->el_coding_type = m0->ffm_coding_type;
		layout->el_chunk_size = m0->ffm_striping_unit_size;
		layout->el_stripe_unit = m0->ffm_striping_unit_size;

		/* Count total data servers across all stripes. */
		uint32_t nds = 0;

		for (uint32_t s = 0; s < m0->ffm_stripes.ffm_stripes_len; s++)
			nds += m0->ffm_stripes.ffm_stripes_val[s]
				       .ffs_data_servers.ffs_data_servers_len;

		layout->el_nmirrors = nds;
		layout->el_mirrors = calloc(nds, sizeof(struct ec_mirror));
		if (!layout->el_mirrors) {
			xdr_free((xdrproc_t)xdr_ffv2_layout4, (caddr_t)&ffl);
			mds_compound_fini(&mc);
			return -ENOMEM;
		}

		uint32_t idx = 0;

		for (uint32_t s = 0; s < m0->ffm_stripes.ffm_stripes_len; s++) {
			ffv2_stripes4 *st = &m0->ffm_stripes.ffm_stripes_val[s];

			for (uint32_t d = 0;
			     d < st->ffs_data_servers.ffs_data_servers_len;
			     d++) {
				ffv2_data_server4 *ds =
					&st->ffs_data_servers
						 .ffs_data_servers_val[d];
				struct ec_mirror *em =
					&layout->el_mirrors[idx++];

				memcpy(em->em_deviceid, ds->ffv2ds_deviceid,
				       sizeof(deviceid4));
				em->em_efficiency = ds->ffv2ds_efficiency;
				em->em_flags = ds->ffv2ds_flags;

				if (ds->ffv2ds_file_info.ffv2ds_file_info_len >
				    0) {
					ffv2_file_info4 *fi =
						&ds->ffv2ds_file_info
							 .ffv2ds_file_info_val[0];
					nfs_fh4 *fh = &fi->fffi_fh_vers;

					em->em_fh_len = fh->nfs_fh4_len;
					if (em->em_fh_len > sizeof(em->em_fh))
						em->em_fh_len =
							sizeof(em->em_fh);
					memcpy(em->em_fh, fh->nfs_fh4_val,
					       em->em_fh_len);
				}

				em->em_uid = parse_owner_id(
					ds->ffv2ds_user.utf8string_val,
					ds->ffv2ds_user.utf8string_len);
				em->em_gid = parse_owner_id(
					ds->ffv2ds_group.utf8string_val,
					ds->ffv2ds_group.utf8string_len);
			}
		}

		xdr_free((xdrproc_t)xdr_ffv2_layout4, (caddr_t)&ffl);
	} else {
		/* ---- Flex Files v1 ---- */
		ff_layout4 ffl;

		memset(&ffl, 0, sizeof(ffl));
		xdrmem_create(&xdrs, loc->loc_body.loc_body_val,
			      loc->loc_body.loc_body_len, XDR_DECODE);
		if (!xdr_ff_layout4(&xdrs, &ffl)) {
			xdr_destroy(&xdrs);
			mds_compound_fini(&mc);
			return -EIO;
		}
		xdr_destroy(&xdrs);

		layout->el_stripe_unit = ffl.ffl_stripe_unit;
		layout->el_nmirrors = ffl.ffl_mirrors.ffl_mirrors_len;
		layout->el_mirrors =
			calloc(layout->el_nmirrors, sizeof(struct ec_mirror));
		if (!layout->el_mirrors) {
			xdr_free((xdrproc_t)xdr_ff_layout4, (caddr_t)&ffl);
			mds_compound_fini(&mc);
			return -ENOMEM;
		}

		for (uint32_t i = 0; i < layout->el_nmirrors; i++) {
			ff_mirror4 *m = &ffl.ffl_mirrors.ffl_mirrors_val[i];

			if (m->ffm_data_servers.ffm_data_servers_len < 1)
				continue;

			ff_data_server4 *ds =
				&m->ffm_data_servers.ffm_data_servers_val[0];
			struct ec_mirror *em = &layout->el_mirrors[i];

			memcpy(em->em_deviceid, ds->ffds_deviceid,
			       sizeof(deviceid4));
			em->em_efficiency = ds->ffds_efficiency;

			if (ds->ffds_fh_vers.ffds_fh_vers_len > 0) {
				nfs_fh4 *fh =
					&ds->ffds_fh_vers.ffds_fh_vers_val[0];

				em->em_fh_len = fh->nfs_fh4_len;
				if (em->em_fh_len > sizeof(em->em_fh))
					em->em_fh_len = sizeof(em->em_fh);
				memcpy(em->em_fh, fh->nfs_fh4_val,
				       em->em_fh_len);
			}

			em->em_uid =
				parse_owner_id(ds->ffds_user.utf8string_val,
					       ds->ffds_user.utf8string_len);
			em->em_gid =
				parse_owner_id(ds->ffds_group.utf8string_val,
					       ds->ffds_group.utf8string_len);
		}

		xdr_free((xdrproc_t)xdr_ff_layout4, (caddr_t)&ffl);
	}

	mds_compound_fini(&mc);
	return 0;
}

/* ------------------------------------------------------------------ */
/* GETDEVICEINFO                                                       */
/* ------------------------------------------------------------------ */

int mds_getdeviceinfo(struct mds_session *ms, const deviceid4 devid,
		      layouttype4 layout_type, struct ec_device *dev)
{
	struct mds_compound mc;
	nfs_argop4 *slot;
	int ret;

	memset(dev, 0, sizeof(*dev));

	/* SEQUENCE + GETDEVICEINFO = 2 ops */
	ret = mds_compound_init(&mc, 2, "getdeviceinfo");
	if (ret)
		return ret;

	ret = mds_compound_add_sequence(&mc, ms);
	if (ret) {
		mds_compound_fini(&mc);
		return ret;
	}

	slot = mds_compound_add_op(&mc, OP_GETDEVICEINFO);
	if (!slot) {
		mds_compound_fini(&mc);
		return -ENOSPC;
	}

	GETDEVICEINFO4args *gdi_args = &slot->nfs_argop4_u.opgetdeviceinfo;
	memcpy(gdi_args->gdia_device_id, devid, sizeof(deviceid4));
	gdi_args->gdia_layout_type = layout_type;
	gdi_args->gdia_maxcount = 65536;
	memset(&gdi_args->gdia_notify_types, 0, sizeof(bitmap4));

	ret = mds_compound_send(&mc, ms);
	if (ret) {
		mds_compound_fini(&mc);
		return ret;
	}

	nfs_resop4 *gdi_res = mds_compound_result(&mc, 1);

	if (!gdi_res ||
	    gdi_res->nfs_resop4_u.opgetdeviceinfo.gdir_status != NFS4_OK) {
		mds_compound_fini(&mc);
		return -EREMOTEIO;
	}

	GETDEVICEINFO4resok *resok = &gdi_res->nfs_resop4_u.opgetdeviceinfo
					      .GETDEVICEINFO4res_u.gdir_resok4;

	device_addr4 *da = &resok->gdir_device_addr;

	/* Decode ff_device_addr4 from the opaque body. */
	XDR xdrs;
	ff_device_addr4 ffda;

	memset(&ffda, 0, sizeof(ffda));
	xdrmem_create(&xdrs, da->da_addr_body.da_addr_body_val,
		      da->da_addr_body.da_addr_body_len, XDR_DECODE);

	if (!xdr_ff_device_addr4(&xdrs, &ffda)) {
		xdr_destroy(&xdrs);
		mds_compound_fini(&mc);
		return -EIO;
	}
	xdr_destroy(&xdrs);

	/* Extract the first network address. */
	if (ffda.ffda_netaddrs.multipath_list4_len > 0) {
		netaddr4 *na = &ffda.ffda_netaddrs.multipath_list4_val[0];

		/*
		 * na_r_addr is a universal address like "192.168.1.1.8.1"
		 * where 8.1 means port 2049 (8*256 + 1).
		 */
		ret = parse_uaddr(na->na_r_addr, dev->ed_host,
				  sizeof(dev->ed_host), &dev->ed_port);
	} else {
		ret = -ENODATA;
	}

	/*
	 * Check whether the DS advertises tight coupling for NFSv4.2.
	 * The MDS sets ffdv_tightly_coupled when TRUST_STATEID is
	 * supported.  Only consider entries for NFSv4 minor version 2,
	 * since CHUNK ops are a v2-only path.  When set, the client
	 * must use the real layout stateid (not the anonymous stateid)
	 * for CHUNK I/O.
	 */
	if (ret == 0) {
		for (u_int v = 0; v < ffda.ffda_versions.ffda_versions_len;
		     v++) {
			ff_device_versions4 *ver =
				&ffda.ffda_versions.ffda_versions_val[v];

			if (ver->ffdv_version == 4 &&
			    ver->ffdv_minorversion == 2 &&
			    ver->ffdv_tightly_coupled) {
				dev->ed_tight_coupled = true;
				break;
			}
		}
	}

	xdr_free((xdrproc_t)xdr_ff_device_addr4, (caddr_t)&ffda);
	mds_compound_fini(&mc);
	return ret;
}

/* ------------------------------------------------------------------ */
/* LAYOUTRETURN                                                        */
/* ------------------------------------------------------------------ */

int mds_layout_return(struct mds_session *ms, struct mds_file *mf,
		      struct ec_layout *layout)
{
	struct mds_compound mc;
	nfs_argop4 *slot;
	int ret;

	/* SEQUENCE + PUTFH + LAYOUTRETURN = 3 ops */
	ret = mds_compound_init(&mc, 3, "layoutreturn");
	if (ret)
		return ret;

	ret = mds_compound_add_sequence(&mc, ms);
	if (ret) {
		mds_compound_fini(&mc);
		return ret;
	}

	slot = mds_compound_add_op(&mc, OP_PUTFH);
	if (!slot) {
		mds_compound_fini(&mc);
		return -ENOSPC;
	}
	slot->nfs_argop4_u.opputfh.object = mf->mf_fh;

	slot = mds_compound_add_op(&mc, OP_LAYOUTRETURN);
	if (!slot) {
		mds_compound_fini(&mc);
		return -ENOSPC;
	}

	LAYOUTRETURN4args *lr_args = &slot->nfs_argop4_u.oplayoutreturn;
	lr_args->lora_reclaim = false;
	lr_args->lora_layout_type = layout->el_layout_type;
	lr_args->lora_iomode = LAYOUTIOMODE4_ANY;
	lr_args->lora_layoutreturn.lr_returntype = LAYOUTRETURN4_FILE;

	layoutreturn_file4 *lrf =
		&lr_args->lora_layoutreturn.layoutreturn4_u.lr_layout;
	lrf->lrf_offset = 0;
	lrf->lrf_length = 0xFFFFFFFFFFFFFFFFULL;
	memcpy(&lrf->lrf_stateid, &layout->el_stateid, sizeof(stateid4));
	lrf->lrf_body.lrf_body_len = 0;
	lrf->lrf_body.lrf_body_val = NULL;

	ret = mds_compound_send(&mc, ms);
	mds_compound_fini(&mc);
	return ret;
}

/* ------------------------------------------------------------------ */
/* LAYOUTERROR                                                         */
/* ------------------------------------------------------------------ */

int mds_layout_error(struct mds_session *ms, struct mds_file *mf,
		     struct ec_layout *layout, uint32_t mirror_idx,
		     nfsstat4 nfs4_status, nfs_opnum4 opnum)
{
	struct mds_compound mc;
	nfs_argop4 *slot;
	int ret;

	if (mirror_idx >= layout->el_nmirrors)
		return -EINVAL;

	/* SEQUENCE + PUTFH + LAYOUTERROR = 3 ops */
	ret = mds_compound_init(&mc, 3, "layouterror");
	if (ret)
		return ret;

	ret = mds_compound_add_sequence(&mc, ms);
	if (ret) {
		mds_compound_fini(&mc);
		return ret;
	}

	slot = mds_compound_add_op(&mc, OP_PUTFH);
	if (!slot) {
		mds_compound_fini(&mc);
		return -ENOSPC;
	}
	slot->nfs_argop4_u.opputfh.object = mf->mf_fh;

	slot = mds_compound_add_op(&mc, OP_LAYOUTERROR);
	if (!slot) {
		mds_compound_fini(&mc);
		return -ENOSPC;
	}

	LAYOUTERROR4args *le_args = &slot->nfs_argop4_u.oplayouterror;

	le_args->lea_offset = 0;
	le_args->lea_length = 0xFFFFFFFFFFFFFFFFULL;
	memcpy(&le_args->lea_stateid, &layout->el_stateid, sizeof(stateid4));

	le_args->lea_errors.lea_errors_len = 1;
	le_args->lea_errors.lea_errors_val = calloc(1, sizeof(device_error4));
	if (!le_args->lea_errors.lea_errors_val) {
		mds_compound_fini(&mc);
		return -ENOMEM;
	}

	struct ec_mirror *em = &layout->el_mirrors[mirror_idx];
	device_error4 *de = &le_args->lea_errors.lea_errors_val[0];

	memcpy(de->de_deviceid, em->em_deviceid, sizeof(deviceid4));
	de->de_status = nfs4_status;
	de->de_opnum = opnum;

	ret = mds_compound_send(&mc, ms);
	mds_compound_fini(&mc);
	return ret;
}

/* ------------------------------------------------------------------ */
/* Cleanup                                                             */
/* ------------------------------------------------------------------ */

void ec_layout_free(struct ec_layout *layout)
{
	free(layout->el_mirrors);
	memset(layout, 0, sizeof(*layout));
}
