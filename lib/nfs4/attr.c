/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include "nfsv42_xdr.h"
#include "reffs/inode.h"
#include "reffs/super_block.h"
#include "reffs/filehandle.h"
#include "reffs/utf8string.h"
#include "nfs4/attr.h"
#include "nfsv42_names.h"
#include "reffs/time.h"
#include "reffs/log.h"
#include "reffs/rpc.h"
#include "nfs4/compound.h"
#include "nfs4/ops.h"
#include "nfs4/errors.h"

struct nfsv42_attr {
	fattr4_supported_attrs supported_attrs;
	fattr4_type type;
	fattr4_fh_expire_type fh_expire_type;
	fattr4_change change;
	fattr4_size size;
	fattr4_link_support link_support;
	fattr4_symlink_support symlink_support;
	fattr4_named_attr named_attr;
	fattr4_fsid fsid;
	fattr4_unique_handles unique_handles;
	fattr4_lease_time lease_time;
	fattr4_rdattr_error rdattr_error;
	fattr4_acl acl;
	fattr4_aclsupport aclsupport;
	fattr4_archive archive;
	fattr4_cansettime cansettime;
	fattr4_case_insensitive case_insensitive;
	fattr4_case_preserving case_preserving;
	fattr4_chown_restricted chown_restricted;
	fattr4_filehandle filehandle;
	fattr4_fileid fileid;
	fattr4_files_avail files_avail;
	fattr4_files_free files_free;
	fattr4_files_total files_total;
	fattr4_fs_locations fs_locations;
	fattr4_hidden hidden;
	fattr4_homogeneous homogeneous;
	fattr4_maxfilesize maxfilesize;
	fattr4_maxlink maxlink;
	fattr4_maxname maxname;
	fattr4_maxread maxread;
	fattr4_maxwrite maxwrite;
	fattr4_mimetype mimetype;
	fattr4_mode mode;
	fattr4_no_trunc no_trunc;
	fattr4_numlinks numlinks;
	fattr4_owner owner;
	fattr4_owner_group owner_group;
	fattr4_quota_avail_hard quota_avail_hard;
	fattr4_quota_avail_soft quota_avail_soft;
	fattr4_quota_used quota_used;
	fattr4_rawdev rawdev;
	fattr4_space_avail space_avail;
	fattr4_space_free space_free;
	fattr4_space_total space_total;
	fattr4_space_used space_used;
	fattr4_system system;
	fattr4_time_access time_access;
	fattr4_time_access_set time_access_set;
	fattr4_time_backup time_backup;
	fattr4_time_create time_create;
	fattr4_time_delta time_delta;
	fattr4_time_metadata time_metadata;
	fattr4_time_modify time_modify;
	fattr4_time_modify_set time_modify_set;
	fattr4_mounted_on_fileid mounted_on_fileid;
	fattr4_dir_notif_delay dir_notif_delay;
	fattr4_dirent_notif_delay dirent_notif_delay;
	fattr4_dacl dacl;
	fattr4_sacl sacl;
	fattr4_change_policy change_policy;
	fattr4_fs_status fs_status;
	fattr4_fs_layout_types fs_layout_types;
	fattr4_layout_hint layout_hint;
	fattr4_layout_types layout_types;
	fattr4_layout_blksize layout_blksize;
	fattr4_layout_alignment layout_alignment;
	fattr4_fs_locations_info fs_locations_info;
	fattr4_mdsthreshold mdsthreshold;
	fattr4_retention_get retention_get;
	fattr4_retention_set retention_set;
	fattr4_retentevt_get retentevt_get;
	fattr4_retentevt_set retentevt_set;
	fattr4_retention_hold retention_hold;
	fattr4_mode_set_masked mode_set_masked;
	fattr4_suppattr_exclcreat suppattr_exclcreat;
	fattr4_fs_charset_cap fs_charset_cap;
	fattr4_clone_blksize clone_blksize;
	fattr4_space_freed space_freed;
	fattr4_change_attr_type change_attr_type;
	fattr4_sec_label sec_label;
	fattr4_mode_umask mode_umask;
	fattr4_xattr_support xattr_support;
	fattr4_offline offline;
	fattr4_time_deleg_access time_deleg_access;
	fattr4_time_deleg_modify time_deleg_modify;
	fattr4_open_arguments open_arguments;
	fattr4_uncacheable_file_data uncacheable_file_data;
	fattr4_uncacheable_dirent_metadata uncacheable_dirent_metadata;
};

static struct nfsv42_attr system_attrs = {
	.fh_expire_type = FH4_PERSISTENT,
	.link_support = true,
	.symlink_support = true,
	.named_attr = false,
	.chown_restricted = false,
	.unique_handles = true,
	.lease_time = 45,
	.aclsupport = false,
	.cansettime = true,
	.case_insensitive = false,
	.case_preserving = true,
	.homogeneous = true,
	.maxfilesize = 123456,
	.maxlink = -1,
	.maxname = 255,
	.maxread = 1024 * 1024,
	.maxwrite = 1024 * 1024,
	.no_trunc = true,
	.numlinks = 255,
	.fs_charset_cap = FSCHARSET_CAP4_ALLOWS_ONLY_UTF8,
	.change_attr_type = NFS4_CHANGE_TYPE_IS_TIME_METADATA,
	.clone_blksize = 4096,
	.xattr_support = false,
	.time_delta = { .seconds = 0, .nseconds = 1 },
};
bitmap4 *supported_attributes = &system_attrs.supported_attrs;

struct nfsv42_attr_ops {
	uint32_t nao_attr;
	count4 (*nao_count)(struct nfsv42_attr *nattr);
	nfsstat4 (*nao_xdr)(XDR *xdrs, struct nfsv42_attr *nattr);
	bool (*nao_equal)(struct nfsv42_attr *a, struct nfsv42_attr *b);
};

static count4 supported_attrs_count(struct nfsv42_attr __attribute__((unused)) *
				    nattr)
{
	bitmap4 bm;

	u_int words = BITMAP4_WORDS_FOR_MAX(FATTR4_ATTRIBUTE_MAX);

	return sizeof(bm) + (words * sizeof(bm.bitmap4_val));
}

static nfsstat4 supported_attrs_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_supported_attrs(xdrs, &nattr->supported_attrs))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool supported_attrs_equal(struct nfsv42_attr __attribute__((unused)) *
					  a,
				  struct nfsv42_attr *b)
{
	return bitmap4_equal(supported_attributes, &b->supported_attrs);
}

static count4 type_count(struct nfsv42_attr __attribute__((unused)) * nattr)
{
	return sizeof(fattr4_type);
}

static nfsstat4 type_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_type(xdrs, &nattr->type))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool type_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->type == b->type;
}

static count4 fh_expire_type_count(struct nfsv42_attr __attribute__((unused)) *
				   nattr)
{
	return sizeof(fattr4_fh_expire_type);
}

static nfsstat4 fh_expire_type_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_fh_expire_type(xdrs, &nattr->fh_expire_type))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool fh_expire_type_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->fh_expire_type == b->fh_expire_type;
}

static count4 change_count(struct nfsv42_attr __attribute__((unused)) * nattr)
{
	return sizeof(fattr4_change);
}

static nfsstat4 change_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_change(xdrs, &nattr->change))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool change_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->change == b->change;
}

static count4 size_count(struct nfsv42_attr __attribute__((unused)) * nattr)
{
	return sizeof(fattr4_size);
}

static nfsstat4 size_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_size(xdrs, &nattr->size))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool size_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->size == b->size;
}

static count4 link_support_count(struct nfsv42_attr __attribute__((unused)) *
				 nattr)
{
	return sizeof(fattr4_link_support);
}

static nfsstat4 link_support_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_link_support(xdrs, &nattr->link_support))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool link_support_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->link_support == b->link_support;
}

static count4 symlink_support_count(struct nfsv42_attr __attribute__((unused)) *
				    nattr)
{
	return sizeof(fattr4_symlink_support);
}

static nfsstat4 symlink_support_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_symlink_support(xdrs, &nattr->symlink_support))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool symlink_support_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->symlink_support == b->symlink_support;
}

static count4 named_attr_count(struct nfsv42_attr __attribute__((unused)) *
			       nattr)
{
	return sizeof(fattr4_named_attr);
}

static nfsstat4 named_attr_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_named_attr(xdrs, &nattr->named_attr))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool named_attr_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->named_attr == b->named_attr;
}

static count4 fsid_count(struct nfsv42_attr __attribute__((unused)) * nattr)
{
	return sizeof(fattr4_fsid);
}

static nfsstat4 fsid_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_fsid(xdrs, &nattr->fsid))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool fsid_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->fsid.major == b->fsid.major && a->fsid.minor == b->fsid.minor;
}

static count4 unique_handles_count(struct nfsv42_attr __attribute__((unused)) *
				   nattr)
{
	return sizeof(fattr4_unique_handles);
}

static nfsstat4 unique_handles_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_unique_handles(xdrs, &nattr->unique_handles))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool unique_handles_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->unique_handles == b->unique_handles;
}

static count4 lease_time_count(struct nfsv42_attr __attribute__((unused)) *
			       nattr)
{
	return sizeof(fattr4_lease_time);
}

static nfsstat4 lease_time_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_lease_time(xdrs, &nattr->lease_time))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool lease_time_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->lease_time == b->lease_time;
}

static count4 rdattr_error_count(struct nfsv42_attr __attribute__((unused)) *
				 nattr)
{
	return sizeof(fattr4_rdattr_error);
}

static nfsstat4 rdattr_error_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_rdattr_error(xdrs, &nattr->rdattr_error))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool rdattr_error_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->rdattr_error == b->rdattr_error;
}

static count4 acl_count(struct nfsv42_attr *nattr)
{
	count4 c = sizeof(fattr4_acl);
	c += nattr->acl.fattr4_acl_len * sizeof(*nattr->acl.fattr4_acl_val);
	for (u_int i = 0; i < nattr->acl.fattr4_acl_len; i++) {
		c += nattr->acl.fattr4_acl_val[i].who.utf8string_len;
	}
	return c;
}

static nfsstat4 acl_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_acl(xdrs, &nattr->acl))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool acl_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	if (a->acl.fattr4_acl_len != b->acl.fattr4_acl_len)
		return false;

	for (u_int i = 0; i < a->acl.fattr4_acl_len; i++) {
		if (a->acl.fattr4_acl_val[i].type !=
		    b->acl.fattr4_acl_val[i].type)
			return false;

		if (a->acl.fattr4_acl_val[i].flag !=
		    b->acl.fattr4_acl_val[i].flag)
			return false;

		if (a->acl.fattr4_acl_val[i].access_mask !=
		    b->acl.fattr4_acl_val[i].access_mask)
			return false;

		if (utf8string_cmp(&a->acl.fattr4_acl_val[i].who,
				   &b->acl.fattr4_acl_val[i].who))
			return false;
	}

	return true;
}

static count4 aclsupport_count(struct nfsv42_attr __attribute__((unused)) *
			       nattr)
{
	return sizeof(fattr4_aclsupport);
}

static nfsstat4 aclsupport_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_aclsupport(xdrs, &nattr->aclsupport))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool aclsupport_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->aclsupport == b->aclsupport;
}

static count4 archive_count(struct nfsv42_attr __attribute__((unused)) * nattr)
{
	return sizeof(fattr4_archive);
}

static nfsstat4 archive_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_archive(xdrs, &nattr->archive))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool archive_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->archive == b->archive;
}

static count4 cansettime_count(struct nfsv42_attr __attribute__((unused)) *
			       nattr)
{
	return sizeof(fattr4_cansettime);
}

static nfsstat4 cansettime_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_cansettime(xdrs, &nattr->cansettime))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool cansettime_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->cansettime == b->cansettime;
}

static count4 case_insensitive_count(struct nfsv42_attr
				     __attribute__((unused)) *
				     nattr)
{
	return sizeof(fattr4_case_insensitive);
}

static nfsstat4 case_insensitive_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_case_insensitive(xdrs, &nattr->case_insensitive))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool case_insensitive_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->case_insensitive == b->case_insensitive;
}

static count4 case_preserving_count(struct nfsv42_attr __attribute__((unused)) *
				    nattr)
{
	return sizeof(fattr4_case_preserving);
}

static nfsstat4 case_preserving_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_case_preserving(xdrs, &nattr->case_preserving))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool case_preserving_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->case_preserving == b->case_preserving;
}

static count4 chown_restricted_count(struct nfsv42_attr
				     __attribute__((unused)) *
				     nattr)
{
	return sizeof(fattr4_chown_restricted);
}

static nfsstat4 chown_restricted_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_chown_restricted(xdrs, &nattr->chown_restricted))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool chown_restricted_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->chown_restricted == b->chown_restricted;
}

static count4 filehandle_count(struct nfsv42_attr __attribute__((unused)) *
			       nattr)
{
	return sizeof(fattr4_filehandle);
}

static nfsstat4 filehandle_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_filehandle(xdrs, &nattr->filehandle))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool filehandle_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	if (b->filehandle.nfs_fh4_len != a->filehandle.nfs_fh4_len)
		return false;

	return network_file_handles_equal(
		(struct network_file_handle *)a->filehandle.nfs_fh4_val,
		(struct network_file_handle *)b->filehandle.nfs_fh4_val);
}

static count4 fileid_count(struct nfsv42_attr __attribute__((unused)) * nattr)
{
	return sizeof(fattr4_fileid);
}

static nfsstat4 fileid_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_fileid(xdrs, &nattr->fileid))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool fileid_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->fileid == b->fileid;
}

static count4 files_avail_count(struct nfsv42_attr __attribute__((unused)) *
				nattr)
{
	return sizeof(fattr4_files_avail);
}

static nfsstat4 files_avail_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_files_avail(xdrs, &nattr->files_avail))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool files_avail_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->files_avail == b->files_avail;
}

static count4 files_free_count(struct nfsv42_attr __attribute__((unused)) *
			       nattr)
{
	return sizeof(fattr4_files_free);
}

static nfsstat4 files_free_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_files_free(xdrs, &nattr->files_free))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool files_free_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->files_free == b->files_free;
}

static count4 files_total_count(struct nfsv42_attr __attribute__((unused)) *
				nattr)
{
	return sizeof(fattr4_files_total);
}

static nfsstat4 files_total_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_files_total(xdrs, &nattr->files_total))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool files_total_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->files_total == b->files_total;
}

static inline count4 fs_location_count(fs_location4 *loc)
{
	count4 c = sizeof(fs_location4);

	for (u_int i = 0; i < loc->rootpath.pathname4_len; i++) {
		c += sizeof(component4);
		c += loc->rootpath.pathname4_val[i].utf8string_len;
	}

	for (u_int i = 0; i < loc->server.server_len; i++) {
		c += sizeof(*loc->server.server_val);
		c += loc->server.server_val[i].utf8string_len;
	}

	return c;
}

static count4 fs_locations_count(struct nfsv42_attr *nattr)
{
	count4 c = sizeof(fattr4_fs_locations);
	fs_locations4 *locs = &nattr->fs_locations;

	for (u_int i = 0; i < locs->fs_root.pathname4_len; i++) {
		c += sizeof(component4);
		c += locs->fs_root.pathname4_val[i].utf8string_len;
	}

	for (u_int i = 0; i < locs->locations.locations_len; i++) {
		c += fs_location_count(&locs->locations.locations_val[i]);
	}

	return c;
}

static nfsstat4 fs_locations_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_fs_locations(xdrs, &nattr->fs_locations))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static inline bool fs_location_equal(fs_location4 *a, fs_location4 *b)
{
	if (a->rootpath.pathname4_len != b->rootpath.pathname4_len)
		return false;

	if (a->server.server_len != b->server.server_len)
		return false;

	for (u_int i = 0; i < a->rootpath.pathname4_len; i++) {
		if (utf8string_cmp(&a->rootpath.pathname4_val[i],
				   &b->rootpath.pathname4_val[i]))
			return false;
	}

	for (u_int i = 0; i < a->server.server_len; i++) {
		if (utf8string_cmp(&a->server.server_val[i],
				   &b->server.server_val[i]))
			return false;
	}

	return true;
}

static bool fs_locations_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	if (a->fs_locations.fs_root.pathname4_len !=
	    b->fs_locations.fs_root.pathname4_len)
		return false;

	for (u_int i = 0; i < a->fs_locations.fs_root.pathname4_len; i++) {
		if (utf8string_cmp(&a->fs_locations.fs_root.pathname4_val[i],
				   &b->fs_locations.fs_root.pathname4_val[i]))
			return false;
	}

	for (u_int i = 0; i < a->fs_locations.locations.locations_len; i++) {
		if (!fs_location_equal(
			    &a->fs_locations.locations.locations_val[i],
			    &b->fs_locations.locations.locations_val[i]))
			return false;
	}

	return true;
}

static count4 hidden_count(struct nfsv42_attr __attribute__((unused)) * nattr)
{
	return sizeof(fattr4_hidden);
}

static nfsstat4 hidden_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_hidden(xdrs, &nattr->hidden))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool hidden_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->hidden == b->hidden;
}

static count4 homogeneous_count(struct nfsv42_attr __attribute__((unused)) *
				nattr)
{
	return sizeof(fattr4_homogeneous);
}

static nfsstat4 homogeneous_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_homogeneous(xdrs, &nattr->homogeneous))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool homogeneous_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->homogeneous == b->homogeneous;
}

static count4 maxfilesize_count(struct nfsv42_attr __attribute__((unused)) *
				nattr)
{
	return sizeof(fattr4_maxfilesize);
}

static nfsstat4 maxfilesize_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_maxfilesize(xdrs, &nattr->maxfilesize))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool maxfilesize_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->maxfilesize == b->maxfilesize;
}

static count4 maxlink_count(struct nfsv42_attr __attribute__((unused)) * nattr)
{
	return sizeof(fattr4_maxlink);
}

static nfsstat4 maxlink_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_maxlink(xdrs, &nattr->maxlink))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool maxlink_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->maxlink == b->maxlink;
}

static count4 maxname_count(struct nfsv42_attr __attribute__((unused)) * nattr)
{
	return sizeof(fattr4_maxname);
}

static nfsstat4 maxname_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_maxname(xdrs, &nattr->maxname))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool maxname_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->maxname == b->maxname;
}

static count4 maxread_count(struct nfsv42_attr __attribute__((unused)) * nattr)
{
	return sizeof(fattr4_maxread);
}

static nfsstat4 maxread_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_maxread(xdrs, &nattr->maxread))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool maxread_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->maxread == b->maxread;
}

static count4 maxwrite_count(struct nfsv42_attr __attribute__((unused)) * nattr)
{
	return sizeof(fattr4_maxwrite);
}

static nfsstat4 maxwrite_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_maxwrite(xdrs, &nattr->maxwrite))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool maxwrite_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->maxwrite == b->maxwrite;
}

static count4 mimetype_count(struct nfsv42_attr *nattr)
{
	count4 c = sizeof(fattr4_mimetype);
	c += nattr->mimetype.utf8string_len;
	return c;
}

static nfsstat4 mimetype_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_mimetype(xdrs, &nattr->mimetype))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool mimetype_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	if (utf8string_cmp(&a->mimetype, &b->mimetype))
		return false;

	return true;
}

static count4 mode_count(struct nfsv42_attr __attribute__((unused)) * nattr)
{
	return sizeof(fattr4_mode);
}

static nfsstat4 mode_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_mode(xdrs, &nattr->mode))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool mode_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->mode == b->mode;
}

static count4 no_trunc_count(struct nfsv42_attr __attribute__((unused)) * nattr)
{
	return sizeof(fattr4_no_trunc);
}

static nfsstat4 no_trunc_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_no_trunc(xdrs, &nattr->no_trunc))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool no_trunc_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->no_trunc == b->no_trunc;
}

static count4 numlinks_count(struct nfsv42_attr __attribute__((unused)) * nattr)
{
	return sizeof(fattr4_numlinks);
}

static nfsstat4 numlinks_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_numlinks(xdrs, &nattr->numlinks))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool numlinks_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->numlinks == b->numlinks;
}

static count4 owner_count(struct nfsv42_attr *nattr)
{
	count4 c = sizeof(fattr4_owner);
	c += nattr->owner.utf8string_len;
	return c;
}

static nfsstat4 owner_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_owner(xdrs, &nattr->owner))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool owner_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return utf8string_cmp(&a->owner, &b->owner);
}

static count4 owner_group_count(struct nfsv42_attr *nattr)
{
	count4 c = sizeof(fattr4_owner);
	c += nattr->owner_group.utf8string_len;
	return c;
}

static nfsstat4 owner_group_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_owner_group(xdrs, &nattr->owner_group))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool owner_group_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return utf8string_cmp(&a->owner_group, &b->owner_group);
}

static count4 quota_avail_hard_count(struct nfsv42_attr
				     __attribute__((unused)) *
				     nattr)
{
	return sizeof(fattr4_quota_avail_hard);
}

static nfsstat4 quota_avail_hard_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_quota_avail_hard(xdrs, &nattr->quota_avail_hard))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool quota_avail_hard_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->quota_avail_hard == b->quota_avail_hard;
}

static count4 quota_avail_soft_count(struct nfsv42_attr
				     __attribute__((unused)) *
				     nattr)
{
	return sizeof(fattr4_quota_avail_soft);
}

static nfsstat4 quota_avail_soft_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_quota_avail_soft(xdrs, &nattr->quota_avail_soft))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool quota_avail_soft_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->quota_avail_soft == b->quota_avail_soft;
}

static count4 quota_used_count(struct nfsv42_attr __attribute__((unused)) *
			       nattr)
{
	return sizeof(fattr4_quota_used);
}

static nfsstat4 quota_used_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_quota_used(xdrs, &nattr->quota_used))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool quota_used_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->quota_used == b->quota_used;
}

static count4 rawdev_count(struct nfsv42_attr __attribute__((unused)) * nattr)
{
	return sizeof(fattr4_rawdev);
}

static nfsstat4 rawdev_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_rawdev(xdrs, &nattr->rawdev))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool rawdev_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->rawdev.specdata1 == b->rawdev.specdata1 &&
	       a->rawdev.specdata2 == b->rawdev.specdata2;
}

static count4 space_avail_count(struct nfsv42_attr __attribute__((unused)) *
				nattr)
{
	return sizeof(fattr4_space_avail);
}

static nfsstat4 space_avail_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_space_avail(xdrs, &nattr->space_avail))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool space_avail_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->space_avail == b->space_avail;
}

static count4 space_free_count(struct nfsv42_attr __attribute__((unused)) *
			       nattr)
{
	return sizeof(fattr4_space_free);
}

static nfsstat4 space_free_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_space_free(xdrs, &nattr->space_free))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool space_free_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->space_free == b->space_free;
}

static count4 space_total_count(struct nfsv42_attr __attribute__((unused)) *
				nattr)
{
	return sizeof(fattr4_space_total);
}

static nfsstat4 space_total_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_space_total(xdrs, &nattr->space_total))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool space_total_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->space_total == b->space_total;
}

static count4 space_used_count(struct nfsv42_attr __attribute__((unused)) *
			       nattr)
{
	return sizeof(fattr4_space_used);
}

static nfsstat4 space_used_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_space_used(xdrs, &nattr->space_used))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool space_used_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->space_used == b->space_used;
}

static count4 system_count(struct nfsv42_attr __attribute__((unused)) * nattr)
{
	return sizeof(fattr4_system);
}

static nfsstat4 system_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_system(xdrs, &nattr->system))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool system_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->system == b->system;
}

static count4 time_access_count(struct nfsv42_attr __attribute__((unused)) *
				nattr)
{
	return sizeof(fattr4_time_access);
}

static nfsstat4 time_access_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_time_access(xdrs, &nattr->time_access))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool time_access_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return nfstime4_eq(&a->time_access, &b->time_access);
}

static count4 time_access_set_count(struct nfsv42_attr *nattr)
{
	count4 c = sizeof(time_how4);
	if (nattr->time_access_set.set_it == SET_TO_CLIENT_TIME4)
		c += sizeof(nfstime4);
	return c;
}

static nfsstat4 time_access_set_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_time_access_set(xdrs, &nattr->time_access_set))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool time_access_set_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	if (a->time_access_set.set_it != b->time_access_set.set_it)
		return false;
	if (a->time_access_set.set_it == SET_TO_CLIENT_TIME4)
		return nfstime4_eq(&a->time_access_set.settime4_u.time,
				   &b->time_access_set.settime4_u.time);
	return true;
}

static count4 time_backup_count(struct nfsv42_attr __attribute__((unused)) *
				nattr)
{
	return sizeof(fattr4_time_backup);
}

static nfsstat4 time_backup_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_time_backup(xdrs, &nattr->time_backup))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool time_backup_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return nfstime4_eq(&a->time_backup, &b->time_backup);
}

static count4 time_create_count(struct nfsv42_attr __attribute__((unused)) *
				nattr)
{
	return sizeof(fattr4_time_create);
}

static nfsstat4 time_create_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_time_create(xdrs, &nattr->time_create))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool time_create_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return nfstime4_eq(&a->time_create, &b->time_create);
}

static count4 time_delta_count(struct nfsv42_attr __attribute__((unused)) *
			       nattr)
{
	return sizeof(fattr4_time_delta);
}

static nfsstat4 time_delta_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_time_delta(xdrs, &nattr->time_delta))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool time_delta_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return nfstime4_eq(&a->time_delta, &b->time_delta);
}

static count4 time_metadata_count(struct nfsv42_attr __attribute__((unused)) *
				  nattr)
{
	return sizeof(fattr4_time_metadata);
}

static nfsstat4 time_metadata_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_time_metadata(xdrs, &nattr->time_metadata))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool time_metadata_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return nfstime4_eq(&a->time_metadata, &b->time_metadata);
}

static count4 time_modify_count(struct nfsv42_attr __attribute__((unused)) *
				nattr)
{
	return sizeof(fattr4_time_modify);
}

static nfsstat4 time_modify_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_time_modify(xdrs, &nattr->time_modify))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool time_modify_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return nfstime4_eq(&a->time_modify, &b->time_modify);
}

static count4 time_modify_set_count(struct nfsv42_attr *nattr)
{
	count4 c = sizeof(time_how4);
	if (nattr->time_modify_set.set_it == SET_TO_CLIENT_TIME4)
		c += sizeof(nfstime4);
	return c;
}

static nfsstat4 time_modify_set_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_time_modify_set(xdrs, &nattr->time_modify_set))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool time_modify_set_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	if (a->time_modify_set.set_it != b->time_modify_set.set_it)
		return false;
	if (a->time_modify_set.set_it == SET_TO_CLIENT_TIME4)
		return nfstime4_eq(&a->time_modify_set.settime4_u.time,
				   &b->time_modify_set.settime4_u.time);
	return true;
}

static count4 mounted_on_fileid_count(struct nfsv42_attr
				      __attribute__((unused)) *
				      nattr)
{
	return sizeof(fattr4_mounted_on_fileid);
}

static nfsstat4 mounted_on_fileid_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_mounted_on_fileid(xdrs, &nattr->mounted_on_fileid))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool mounted_on_fileid_equal(struct nfsv42_attr *a,
				    struct nfsv42_attr *b)
{
	return a->mounted_on_fileid == b->mounted_on_fileid;
}

static count4 dir_notif_delay_count(struct nfsv42_attr __attribute__((unused)) *
				    nattr)
{
	return sizeof(fattr4_dir_notif_delay);
}

static nfsstat4 dir_notif_delay_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_dir_notif_delay(xdrs, &nattr->dir_notif_delay))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool dir_notif_delay_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return nfstime4_eq(&a->dir_notif_delay, &b->dir_notif_delay);
}

static count4 dirent_notif_delay_count(struct nfsv42_attr
				       __attribute__((unused)) *
				       nattr)
{
	return sizeof(fattr4_dirent_notif_delay);
}

static nfsstat4 dirent_notif_delay_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_dirent_notif_delay(xdrs, &nattr->dirent_notif_delay))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool dirent_notif_delay_equal(struct nfsv42_attr *a,
				     struct nfsv42_attr *b)
{
	return nfstime4_eq(&a->dirent_notif_delay, &b->dirent_notif_delay);
}

static count4 dacl_count(struct nfsv42_attr *nattr)
{
	count4 c = sizeof(fattr4_dacl);
	c += nattr->dacl.na41_aces.na41_aces_len *
	     sizeof(*nattr->dacl.na41_aces.na41_aces_val);

	return c;
}

static nfsstat4 dacl_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_dacl(xdrs, &nattr->dacl))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool nfsace4_equal(nfsace4 *a, nfsace4 *b)
{
	if (a->type != b->type)
		return false;

	if (a->flag != b->flag)
		return false;

	if (a->access_mask != b->access_mask)
		return false;

	if (utf8string_cmp(&a->who, &b->who))
		return false;

	return true;
}

static bool dacl_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	if (a->dacl.na41_flag != b->dacl.na41_flag)
		return false;

	if (a->dacl.na41_aces.na41_aces_len != b->dacl.na41_aces.na41_aces_len)
		return false;

	for (u_int i = 0; i < a->dacl.na41_aces.na41_aces_len; i++) {
		if (!nfsace4_equal(&a->dacl.na41_aces.na41_aces_val[i],
				   &b->dacl.na41_aces.na41_aces_val[i]))
			return false;
	}

	return true;
}

static count4 sacl_count(struct nfsv42_attr *nattr)
{
	count4 c = sizeof(fattr4_sacl);
	c += nattr->sacl.na41_aces.na41_aces_len *
	     sizeof(*nattr->sacl.na41_aces.na41_aces_val);

	return c;
}

static nfsstat4 sacl_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_sacl(xdrs, &nattr->sacl))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool sacl_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	if (a->sacl.na41_flag != b->sacl.na41_flag)
		return false;

	if (a->sacl.na41_aces.na41_aces_len != b->sacl.na41_aces.na41_aces_len)
		return false;

	for (u_int i = 0; i < a->sacl.na41_aces.na41_aces_len; i++) {
		if (!nfsace4_equal(&a->sacl.na41_aces.na41_aces_val[i],
				   &b->sacl.na41_aces.na41_aces_val[i]))
			return false;
	}

	return true;
}

static count4 change_policy_count(struct nfsv42_attr __attribute__((unused)) *
				  nattr)
{
	return sizeof(fattr4_change_policy);
}

static nfsstat4 change_policy_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_change_policy(xdrs, &nattr->change_policy))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool change_policy_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->change_policy.cp_major == b->change_policy.cp_major &&
	       a->change_policy.cp_minor == b->change_policy.cp_minor;
}

static count4 fs_status_count(struct nfsv42_attr *nattr)
{
	count4 c = sizeof(fattr4_fs_status);
	c += nattr->fs_status.fss_source.utf8string_len;
	c += nattr->fs_status.fss_current.utf8string_len;

	return c;
}

static nfsstat4 fs_status_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_fs_status(xdrs, &nattr->fs_status))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool fs_status_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	if (a->fs_status.fss_absent != b->fs_status.fss_absent)
		return false;

	if (a->fs_status.fss_type != b->fs_status.fss_type)
		return false;

	if (a->fs_status.fss_age != b->fs_status.fss_age)
		return false;

	if (utf8string_cmp(&a->fs_status.fss_source, &b->fs_status.fss_source))
		return false;

	if (utf8string_cmp(&a->fs_status.fss_current,
			   &b->fs_status.fss_current))
		return false;

	return nfstime4_eq(&a->fs_status.fss_version,
			   &b->fs_status.fss_version);
}

static count4 fs_layout_types_count(struct nfsv42_attr *nattr)
{
	count4 c = sizeof(fattr4_fs_layout_types);
	c += nattr->fs_layout_types.fattr4_fs_layout_types_len *
	     sizeof(*nattr->fs_layout_types.fattr4_fs_layout_types_val);

	return c;
}

static nfsstat4 fs_layout_types_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_fs_layout_types(xdrs, &nattr->fs_layout_types))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool fs_layout_types_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	if (a->fs_layout_types.fattr4_fs_layout_types_len !=
	    b->fs_layout_types.fattr4_fs_layout_types_len)
		return false;

	for (u_int i = 0; i < a->fs_layout_types.fattr4_fs_layout_types_len;
	     i++)
		if (a->fs_layout_types.fattr4_fs_layout_types_val[i] !=
		    b->fs_layout_types.fattr4_fs_layout_types_val[i])
			return false;

	return true;
}

static count4 layout_hint_count(struct nfsv42_attr *nattr)
{
	count4 c = sizeof(layouttype4);
	c += sizeof(u_int); /* loh_body_len */
	c += nattr->layout_hint.loh_body.loh_body_len;
	return c;
}

static nfsstat4 layout_hint_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_layout_hint(xdrs, &nattr->layout_hint))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool layout_hint_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	if (a->layout_hint.loh_type != b->layout_hint.loh_type)
		return false;
	if (a->layout_hint.loh_body.loh_body_len !=
	    b->layout_hint.loh_body.loh_body_len)
		return false;
	return memcmp(a->layout_hint.loh_body.loh_body_val,
		      b->layout_hint.loh_body.loh_body_val,
		      a->layout_hint.loh_body.loh_body_len) == 0;
}

static count4 layout_types_count(struct nfsv42_attr __attribute__((unused)) *
				 nattr)
{
	count4 c = sizeof(fattr4_layout_types);
	c += nattr->layout_types.fattr4_layout_types_len *
	     sizeof(*nattr->layout_types.fattr4_layout_types_val);

	return c;
}

static nfsstat4 layout_types_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_layout_types(xdrs, &nattr->layout_types))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool layout_types_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	if (a->layout_types.fattr4_layout_types_len !=
	    b->layout_types.fattr4_layout_types_len)
		return false;

	for (u_int i = 0; i < a->layout_types.fattr4_layout_types_len; i++)
		if (a->layout_types.fattr4_layout_types_val[i] !=
		    b->layout_types.fattr4_layout_types_val[i])
			return false;

	return true;
}

static count4 layout_blksize_count(struct nfsv42_attr __attribute__((unused)) *
				   nattr)
{
	return sizeof(fattr4_layout_blksize);
}

static nfsstat4 layout_blksize_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_layout_blksize(xdrs, &nattr->layout_blksize))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool layout_blksize_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->layout_blksize == b->layout_blksize;
}

static count4 layout_alignment_count(struct nfsv42_attr
				     __attribute__((unused)) *
				     nattr)
{
	return sizeof(fattr4_layout_alignment);
}

static nfsstat4 layout_alignment_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_layout_alignment(xdrs, &nattr->layout_alignment))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool layout_alignment_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->layout_alignment == b->layout_alignment;
}

static count4 fs_locations_info_count(struct nfsv42_attr *nattr)
{
	count4 c = sizeof(fattr4_fs_locations_info);
	fs_locations_info4 *fli = &nattr->fs_locations_info;

	for (u_int i = 0; i < fli->fli_fs_root.pathname4_len; i++) {
		c += sizeof(component4);
		c += fli->fli_fs_root.pathname4_val[i].utf8string_len;
	}

	for (u_int i = 0; i < fli->fli_items.fli_items_len; i++) {
		fs_locations_item4 *item = &fli->fli_items.fli_items_val[i];
		c += sizeof(fs_locations_item4);
		for (u_int j = 0; j < item->fli_entries.fli_entries_len; j++) {
			fs_locations_server4 *srv =
				&item->fli_entries.fli_entries_val[j];
			c += sizeof(fs_locations_server4);
			c += srv->fls_server.utf8string_len;
		}
		c += item->fli_rootpath.pathname4_len * sizeof(component4);
		for (u_int j = 0; j < item->fli_rootpath.pathname4_len; j++) {
			c += item->fli_rootpath.pathname4_val[j].utf8string_len;
		}
	}

	return c;
}

static nfsstat4 fs_locations_info_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_fs_locations_info(xdrs, &nattr->fs_locations_info))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool fs_locations_info_equal(struct nfsv42_attr *a,
				    struct nfsv42_attr *b)
{
	fs_locations_info4 *ai = &a->fs_locations_info;
	fs_locations_info4 *bi = &b->fs_locations_info;

	if (ai->fli_flags != bi->fli_flags)
		return false;

	if (ai->fli_valid_for != bi->fli_valid_for)
		return false;

	if (ai->fli_fs_root.pathname4_len != bi->fli_fs_root.pathname4_len)
		return false;

	for (u_int i = 0; i < ai->fli_fs_root.pathname4_len; i++) {
		if (utf8string_cmp(&ai->fli_fs_root.pathname4_val[i],
				   &bi->fli_fs_root.pathname4_val[i]))
			return false;
	}

	if (ai->fli_items.fli_items_len != bi->fli_items.fli_items_len)
		return false;

	/* For simplicity, we just check lengths and flag equality here,
	 * as deep comparison of fs_locations_item4 is complex.
	 */

	return true;
}

static count4 mdsthreshold_count(struct nfsv42_attr *nattr)
{
	count4 c = sizeof(fattr4_mdsthreshold);
	c += nattr->mdsthreshold.mth_hints.mth_hints_len *
	     sizeof(threshold_item4);

	return c;
}

static nfsstat4 mdsthreshold_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_mdsthreshold(xdrs, &nattr->mdsthreshold))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool mdsthreshold_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	if (a->mdsthreshold.mth_hints.mth_hints_len !=
	    b->mdsthreshold.mth_hints.mth_hints_len)
		return false;

	for (u_int i = 0; i < a->mdsthreshold.mth_hints.mth_hints_len; i++) {
		threshold_item4 *ai =
			&a->mdsthreshold.mth_hints.mth_hints_val[i];
		threshold_item4 *bi =
			&b->mdsthreshold.mth_hints.mth_hints_val[i];

		if (ai->thi_layout_type != bi->thi_layout_type)
			return false;
		if (!bitmap4_equal(&ai->thi_hintset, &bi->thi_hintset))
			return false;
		/* ... further fields if needed ... */
	}

	return true;
}

static count4 retention_get_count(struct nfsv42_attr *nattr)
{
	count4 c = sizeof(fattr4_retention_get);
	if (nattr->retention_get.rg_begin_time.rg_begin_time_len)
		c += sizeof(nfstime4);

	return c;
}

static nfsstat4 retention_get_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_retention_get(xdrs, &nattr->retention_get))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool retention_get_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	if (a->retention_get.rg_duration != b->retention_get.rg_duration)
		return false;

	if (a->retention_get.rg_begin_time.rg_begin_time_len !=
	    b->retention_get.rg_begin_time.rg_begin_time_len)
		return false;

	if (a->retention_get.rg_begin_time.rg_begin_time_len) {
		return nfstime4_eq(
			a->retention_get.rg_begin_time.rg_begin_time_val,
			b->retention_get.rg_begin_time.rg_begin_time_val);
	}

	return true;
}

static count4 retention_set_count(struct nfsv42_attr *nattr)
{
	count4 c = sizeof(bool_t);
	c += sizeof(u_int); /* rs_duration_len */
	if (nattr->retention_set.rs_duration.rs_duration_len)
		c += sizeof(uint64_t);
	return c;
}

static nfsstat4 retention_set_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_retention_set(xdrs, &nattr->retention_set))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool retention_set_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	if (a->retention_set.rs_enable != b->retention_set.rs_enable)
		return false;
	if (a->retention_set.rs_duration.rs_duration_len !=
	    b->retention_set.rs_duration.rs_duration_len)
		return false;
	if (a->retention_set.rs_duration.rs_duration_len)
		return *a->retention_set.rs_duration.rs_duration_val ==
		       *b->retention_set.rs_duration.rs_duration_val;
	return true;
}

static count4 retentevt_get_count(struct nfsv42_attr *nattr)
{
	count4 c = sizeof(fattr4_retentevt_get);
	if (nattr->retentevt_get.rg_begin_time.rg_begin_time_len)
		c += sizeof(nfstime4);

	return c;
}

static nfsstat4 retentevt_get_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_retentevt_get(xdrs, &nattr->retentevt_get))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool retentevt_get_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	if (a->retentevt_get.rg_duration != b->retentevt_get.rg_duration)
		return false;

	if (a->retentevt_get.rg_begin_time.rg_begin_time_len !=
	    b->retentevt_get.rg_begin_time.rg_begin_time_len)
		return false;

	if (a->retentevt_get.rg_begin_time.rg_begin_time_len) {
		return nfstime4_eq(
			a->retentevt_get.rg_begin_time.rg_begin_time_val,
			b->retentevt_get.rg_begin_time.rg_begin_time_val);
	}

	return true;
}

static count4 retentevt_set_count(struct nfsv42_attr *nattr)
{
	count4 c = sizeof(bool_t);
	c += sizeof(u_int); /* rs_duration_len */
	if (nattr->retentevt_set.rs_duration.rs_duration_len)
		c += sizeof(uint64_t);
	return c;
}

static nfsstat4 retentevt_set_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_retentevt_set(xdrs, &nattr->retentevt_set))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool retentevt_set_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	if (a->retentevt_set.rs_enable != b->retentevt_set.rs_enable)
		return false;
	if (a->retentevt_set.rs_duration.rs_duration_len !=
	    b->retentevt_set.rs_duration.rs_duration_len)
		return false;
	if (a->retentevt_set.rs_duration.rs_duration_len)
		return *a->retentevt_set.rs_duration.rs_duration_val ==
		       *b->retentevt_set.rs_duration.rs_duration_val;
	return true;
}

static count4 retention_hold_count(struct nfsv42_attr __attribute__((unused)) *
				   nattr)
{
	return sizeof(fattr4_retention_hold);
}

static nfsstat4 retention_hold_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_retention_hold(xdrs, &nattr->retention_hold))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool retention_hold_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->retention_hold == b->retention_hold;
}

static count4 mode_set_masked_count(struct nfsv42_attr __attribute__((unused)) *
				    nattr)
{
	return sizeof(mode_masked4);
}

static nfsstat4 mode_set_masked_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_mode_set_masked(xdrs, &nattr->mode_set_masked))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool mode_set_masked_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->mode_set_masked.mm_value_to_set ==
		       b->mode_set_masked.mm_value_to_set &&
	       a->mode_set_masked.mm_mask_bits ==
		       b->mode_set_masked.mm_mask_bits;
}

static count4 suppattr_exclcreat_count(struct nfsv42_attr *nattr)
{
	count4 c = sizeof(fattr4_suppattr_exclcreat);
	c += nattr->suppattr_exclcreat.bitmap4_len * sizeof(uint32_t);

	return c;
}

static nfsstat4 suppattr_exclcreat_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_suppattr_exclcreat(xdrs, &nattr->suppattr_exclcreat))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool suppattr_exclcreat_equal(struct nfsv42_attr *a,
				     struct nfsv42_attr *b)
{
	return bitmap4_equal(&a->suppattr_exclcreat, &b->suppattr_exclcreat);
}

static count4 fs_charset_cap_count(struct nfsv42_attr __attribute__((unused)) *
				   nattr)
{
	return sizeof(fattr4_fs_charset_cap);
}

static nfsstat4 fs_charset_cap_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_fs_charset_cap(xdrs, &nattr->fs_charset_cap))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool fs_charset_cap_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->fs_charset_cap == b->fs_charset_cap;
}

static count4 clone_blksize_count(struct nfsv42_attr __attribute__((unused)) *
				  nattr)
{
	return sizeof(fattr4_clone_blksize);
}

static nfsstat4 clone_blksize_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_clone_blksize(xdrs, &nattr->clone_blksize))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool clone_blksize_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->clone_blksize == b->clone_blksize;
}

static count4 space_freed_count(struct nfsv42_attr __attribute__((unused)) *
				nattr)
{
	return sizeof(fattr4_space_freed);
}

static nfsstat4 space_freed_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_space_freed(xdrs, &nattr->space_freed))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool space_freed_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->space_freed == b->space_freed;
}

static count4 change_attr_type_count(struct nfsv42_attr
				     __attribute__((unused)) *
				     nattr)
{
	return sizeof(fattr4_change_attr_type);
}

static nfsstat4 change_attr_type_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_change_attr_type(xdrs, &nattr->change_attr_type))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool change_attr_type_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->change_attr_type == b->change_attr_type;
}

static count4 sec_label_count(struct nfsv42_attr *nattr)
{
	count4 c = sizeof(fattr4_sec_label);
	c += nattr->sec_label.slai_data.slai_data_len;

	return c;
}

static nfsstat4 sec_label_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_sec_label(xdrs, &nattr->sec_label))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool sec_label_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	if (a->sec_label.slai_lfs.lfs_lfs != b->sec_label.slai_lfs.lfs_lfs ||
	    a->sec_label.slai_lfs.lfs_pi != b->sec_label.slai_lfs.lfs_pi)
		return false;

	if (a->sec_label.slai_data.slai_data_len !=
	    b->sec_label.slai_data.slai_data_len)
		return false;

	return memcmp(a->sec_label.slai_data.slai_data_val,
		      b->sec_label.slai_data.slai_data_val,
		      a->sec_label.slai_data.slai_data_len) == 0;
}

static count4 mode_umask_count(struct nfsv42_attr __attribute__((unused)) *
			       nattr)
{
	return sizeof(fattr4_mode_umask);
}

static nfsstat4 mode_umask_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_mode_umask(xdrs, &nattr->mode_umask))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool mode_umask_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->mode_umask.mu_mode == b->mode_umask.mu_mode &&
	       a->mode_umask.mu_umask == b->mode_umask.mu_umask;
}

static count4 xattr_support_count(struct nfsv42_attr __attribute__((unused)) *
				  nattr)
{
	return sizeof(fattr4_xattr_support);
}

static nfsstat4 xattr_support_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_xattr_support(xdrs, &nattr->xattr_support))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool xattr_support_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->xattr_support == b->xattr_support;
}

static count4 offline_count(struct nfsv42_attr __attribute__((unused)) * nattr)
{
	return sizeof(fattr4_offline);
}

static nfsstat4 offline_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_offline(xdrs, &nattr->offline))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool offline_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	return a->offline == b->offline;
}

static count4 time_deleg_access_count(struct nfsv42_attr
				      __attribute__((unused)) *
				      nattr)
{
	return sizeof(fattr4_time_deleg_access);
}

static nfsstat4 time_deleg_access_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_time_deleg_access(xdrs, &nattr->time_deleg_access))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool time_deleg_access_equal(struct nfsv42_attr *a,
				    struct nfsv42_attr *b)
{
	return nfstime4_eq(&a->time_deleg_access, &b->time_deleg_access);
}

static count4 time_deleg_modify_count(struct nfsv42_attr
				      __attribute__((unused)) *
				      nattr)
{
	return sizeof(fattr4_time_deleg_modify);
}

static nfsstat4 time_deleg_modify_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_time_deleg_modify(xdrs, &nattr->time_deleg_modify))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool time_deleg_modify_equal(struct nfsv42_attr *a,
				    struct nfsv42_attr *b)
{
	return nfstime4_eq(&a->time_deleg_modify, &b->time_deleg_modify);
}

static count4 open_arguments_count(struct nfsv42_attr *nattr)
{
	count4 c = sizeof(fattr4_open_arguments);
	c += nattr->open_arguments.oa_share_access.bitmap4_len *
	     sizeof(uint32_t);
	c += nattr->open_arguments.oa_share_deny.bitmap4_len * sizeof(uint32_t);
	c += nattr->open_arguments.oa_share_access_want.bitmap4_len *
	     sizeof(uint32_t);
	c += nattr->open_arguments.oa_open_claim.bitmap4_len * sizeof(uint32_t);
	c += nattr->open_arguments.oa_createmode.bitmap4_len * sizeof(uint32_t);

	return c;
}

static nfsstat4 open_arguments_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_open_arguments(xdrs, &nattr->open_arguments))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool open_arguments_equal(struct nfsv42_attr *a, struct nfsv42_attr *b)
{
	if (!bitmap4_equal(&a->open_arguments.oa_share_access,
			   &b->open_arguments.oa_share_access))
		return false;

	if (!bitmap4_equal(&a->open_arguments.oa_share_deny,
			   &b->open_arguments.oa_share_deny))
		return false;

	if (!bitmap4_equal(&a->open_arguments.oa_share_access_want,
			   &b->open_arguments.oa_share_access_want))
		return false;

	if (!bitmap4_equal(&a->open_arguments.oa_open_claim,
			   &b->open_arguments.oa_open_claim))
		return false;

	return bitmap4_equal(&a->open_arguments.oa_createmode,
			     &b->open_arguments.oa_createmode);
}

static count4 uncacheable_file_data_count(struct nfsv42_attr
					  __attribute__((unused)) *
					  nattr)
{
	return sizeof(fattr4_uncacheable_file_data);
}

static nfsstat4 uncacheable_file_data_xdr(XDR *xdrs, struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_uncacheable_file_data(xdrs,
					      &nattr->uncacheable_file_data))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool uncacheable_file_data_equal(struct nfsv42_attr *a,
					struct nfsv42_attr *b)
{
	return a->uncacheable_file_data == b->uncacheable_file_data;
}

static count4 uncacheable_dirent_metadata_count(struct nfsv42_attr
						__attribute__((unused)) *
						nattr)
{
	return sizeof(fattr4_uncacheable_dirent_metadata);
}

static nfsstat4 uncacheable_dirent_metadata_xdr(XDR *xdrs,
						struct nfsv42_attr *nattr)
{
	if (!xdr_fattr4_uncacheable_dirent_metadata(
		    xdrs, &nattr->uncacheable_dirent_metadata))
		return NFS4ERR_BADXDR;
	return NFS4_OK;
}

static bool uncacheable_dirent_metadata_equal(struct nfsv42_attr *a,
					      struct nfsv42_attr *b)
{
	return a->uncacheable_dirent_metadata == b->uncacheable_dirent_metadata;
}

static struct nfsv42_attr_ops nao[] = {
	{ FATTR4_SUPPORTED_ATTRS, supported_attrs_count, supported_attrs_xdr,
	  supported_attrs_equal },
	{ FATTR4_TYPE, type_count, type_xdr, type_equal },
	{ FATTR4_FH_EXPIRE_TYPE, fh_expire_type_count, fh_expire_type_xdr,
	  fh_expire_type_equal },
	{ FATTR4_CHANGE, change_count, change_xdr, change_equal },
	{ FATTR4_SIZE, size_count, size_xdr, size_equal },
	{ FATTR4_LINK_SUPPORT, link_support_count, link_support_xdr,
	  link_support_equal },
	{ FATTR4_SYMLINK_SUPPORT, symlink_support_count, symlink_support_xdr,
	  symlink_support_equal },
	{ FATTR4_NAMED_ATTR, named_attr_count, named_attr_xdr,
	  named_attr_equal },
	{ FATTR4_FSID, fsid_count, fsid_xdr, fsid_equal },
	{ FATTR4_UNIQUE_HANDLES, unique_handles_count, unique_handles_xdr,
	  unique_handles_equal },
	{ FATTR4_LEASE_TIME, lease_time_count, lease_time_xdr,
	  lease_time_equal },
	{ FATTR4_RDATTR_ERROR, rdattr_error_count, rdattr_error_xdr,
	  rdattr_error_equal },
	{ FATTR4_ACL, acl_count, acl_xdr, acl_equal },
	{ FATTR4_ACLSUPPORT, aclsupport_count, aclsupport_xdr,
	  aclsupport_equal },
	{ FATTR4_ARCHIVE, archive_count, archive_xdr, archive_equal },
	{ FATTR4_CANSETTIME, cansettime_count, cansettime_xdr,
	  cansettime_equal },
	{ FATTR4_CASE_INSENSITIVE, case_insensitive_count, case_insensitive_xdr,
	  case_insensitive_equal },
	{ FATTR4_CASE_PRESERVING, case_preserving_count, case_preserving_xdr,
	  case_preserving_equal },
	{ FATTR4_CHOWN_RESTRICTED, chown_restricted_count, chown_restricted_xdr,
	  chown_restricted_equal },
	{ FATTR4_FILEHANDLE, filehandle_count, filehandle_xdr,
	  filehandle_equal },
	{ FATTR4_FILEID, fileid_count, fileid_xdr, fileid_equal },
	{ FATTR4_FILES_AVAIL, files_avail_count, files_avail_xdr,
	  files_avail_equal },
	{ FATTR4_FILES_FREE, files_free_count, files_free_xdr,
	  files_free_equal },
	{ FATTR4_FILES_TOTAL, files_total_count, files_total_xdr,
	  files_total_equal },
	{ FATTR4_FS_LOCATIONS, fs_locations_count, fs_locations_xdr,
	  fs_locations_equal },
	{ FATTR4_HIDDEN, hidden_count, hidden_xdr, hidden_equal },
	{ FATTR4_HOMOGENEOUS, homogeneous_count, homogeneous_xdr,
	  homogeneous_equal },
	{ FATTR4_MAXFILESIZE, maxfilesize_count, maxfilesize_xdr,
	  maxfilesize_equal },
	{ FATTR4_MAXLINK, maxlink_count, maxlink_xdr, maxlink_equal },
	{ FATTR4_MAXNAME, maxname_count, maxname_xdr, maxname_equal },
	{ FATTR4_MAXREAD, maxread_count, maxread_xdr, maxread_equal },
	{ FATTR4_MAXWRITE, maxwrite_count, maxwrite_xdr, maxwrite_equal },
	{ FATTR4_MIMETYPE, mimetype_count, mimetype_xdr, mimetype_equal },
	{ FATTR4_MODE, mode_count, mode_xdr, mode_equal },
	{ FATTR4_NO_TRUNC, no_trunc_count, no_trunc_xdr, no_trunc_equal },
	{ FATTR4_NUMLINKS, numlinks_count, numlinks_xdr, numlinks_equal },
	{ FATTR4_OWNER, owner_count, owner_xdr, owner_equal },
	{ FATTR4_OWNER_GROUP, owner_group_count, owner_group_xdr,
	  owner_group_equal },
	{ FATTR4_QUOTA_AVAIL_HARD, quota_avail_hard_count, quota_avail_hard_xdr,
	  quota_avail_hard_equal },
	{ FATTR4_QUOTA_AVAIL_SOFT, quota_avail_soft_count, quota_avail_soft_xdr,
	  quota_avail_soft_equal },
	{ FATTR4_QUOTA_USED, quota_used_count, quota_used_xdr,
	  quota_used_equal },
	{ FATTR4_RAWDEV, rawdev_count, rawdev_xdr, rawdev_equal },
	{ FATTR4_SPACE_AVAIL, space_avail_count, space_avail_xdr,
	  space_avail_equal },
	{ FATTR4_SPACE_FREE, space_free_count, space_free_xdr,
	  space_free_equal },
	{ FATTR4_SPACE_TOTAL, space_total_count, space_total_xdr,
	  space_total_equal },
	{ FATTR4_SPACE_USED, space_used_count, space_used_xdr,
	  space_used_equal },
	{ FATTR4_SYSTEM, system_count, system_xdr, system_equal },
	{ FATTR4_TIME_ACCESS, time_access_count, time_access_xdr,
	  time_access_equal },
	{ FATTR4_TIME_ACCESS_SET, time_access_set_count, time_access_set_xdr,
	  time_access_set_equal },
	{ FATTR4_TIME_BACKUP, time_backup_count, time_backup_xdr,
	  time_backup_equal },
	{ FATTR4_TIME_CREATE, time_create_count, time_create_xdr,
	  time_create_equal },
	{ FATTR4_TIME_DELTA, time_delta_count, time_delta_xdr,
	  time_delta_equal },
	{ FATTR4_TIME_METADATA, time_metadata_count, time_metadata_xdr,
	  time_metadata_equal },
	{ FATTR4_TIME_MODIFY, time_modify_count, time_modify_xdr,
	  time_modify_equal },
	{ FATTR4_TIME_MODIFY_SET, time_modify_set_count, time_modify_set_xdr,
	  time_modify_set_equal },
	{ FATTR4_MOUNTED_ON_FILEID, mounted_on_fileid_count,
	  mounted_on_fileid_xdr, mounted_on_fileid_equal },
	{ FATTR4_DIR_NOTIF_DELAY, dir_notif_delay_count, dir_notif_delay_xdr,
	  dir_notif_delay_equal },
	{ FATTR4_DIRENT_NOTIF_DELAY, dirent_notif_delay_count,
	  dirent_notif_delay_xdr, dirent_notif_delay_equal },
	{ FATTR4_DACL, dacl_count, dacl_xdr, dacl_equal },
	{ FATTR4_SACL, sacl_count, sacl_xdr, sacl_equal },
	{ FATTR4_CHANGE_POLICY, change_policy_count, change_policy_xdr,
	  change_policy_equal },
	{ FATTR4_FS_STATUS, fs_status_count, fs_status_xdr, fs_status_equal },
	{ FATTR4_FS_LAYOUT_TYPES, fs_layout_types_count, fs_layout_types_xdr,
	  fs_layout_types_equal },
	{ FATTR4_LAYOUT_HINT, layout_hint_count, layout_hint_xdr,
	  layout_hint_equal },
	{ FATTR4_LAYOUT_TYPES, layout_types_count, layout_types_xdr,
	  layout_types_equal },
	{ FATTR4_LAYOUT_BLKSIZE, layout_blksize_count, layout_blksize_xdr,
	  layout_blksize_equal },
	{ FATTR4_LAYOUT_ALIGNMENT, layout_alignment_count, layout_alignment_xdr,
	  layout_alignment_equal },
	{ FATTR4_FS_LOCATIONS_INFO, fs_locations_info_count,
	  fs_locations_info_xdr, fs_locations_info_equal },
	{ FATTR4_MDSTHRESHOLD, mdsthreshold_count, mdsthreshold_xdr,
	  mdsthreshold_equal },
	{ FATTR4_RETENTION_GET, retention_get_count, retention_get_xdr,
	  retention_get_equal },
	{ FATTR4_RETENTION_SET, retention_set_count, retention_set_xdr,
	  retention_set_equal },
	{ FATTR4_RETENTEVT_GET, retentevt_get_count, retentevt_get_xdr,
	  retentevt_get_equal },
	{ FATTR4_RETENTEVT_SET, retentevt_set_count, retentevt_set_xdr,
	  retentevt_set_equal },
	{ FATTR4_RETENTION_HOLD, retention_hold_count, retention_hold_xdr,
	  retention_hold_equal },
	{ FATTR4_MODE_SET_MASKED, mode_set_masked_count, mode_set_masked_xdr,
	  mode_set_masked_equal },
	{ FATTR4_SUPPATTR_EXCLCREAT, suppattr_exclcreat_count,
	  suppattr_exclcreat_xdr, suppattr_exclcreat_equal },
	{ FATTR4_FS_CHARSET_CAP, fs_charset_cap_count, fs_charset_cap_xdr,
	  fs_charset_cap_equal },
	{ FATTR4_CLONE_BLKSIZE, clone_blksize_count, clone_blksize_xdr,
	  clone_blksize_equal },
	{ FATTR4_SPACE_FREED, space_freed_count, space_freed_xdr,
	  space_freed_equal },
	{ FATTR4_CHANGE_ATTR_TYPE, change_attr_type_count, change_attr_type_xdr,
	  change_attr_type_equal },
	{ FATTR4_SEC_LABEL, sec_label_count, sec_label_xdr, sec_label_equal },
	{ FATTR4_MODE_UMASK, mode_umask_count, mode_umask_xdr,
	  mode_umask_equal },
	{ FATTR4_XATTR_SUPPORT, xattr_support_count, xattr_support_xdr,
	  xattr_support_equal },
	{ FATTR4_OFFLINE, offline_count, offline_xdr, offline_equal },
	{ FATTR4_TIME_DELEG_ACCESS, time_deleg_access_count,
	  time_deleg_access_xdr, time_deleg_access_equal },
	{ FATTR4_TIME_DELEG_MODIFY, time_deleg_modify_count,
	  time_deleg_modify_xdr, time_deleg_modify_equal },
	{ FATTR4_OPEN_ARGUMENTS, open_arguments_count, open_arguments_xdr,
	  open_arguments_equal },
	{ FATTR4_UNCACHEABLE_FILE_DATA, uncacheable_file_data_count,
	  uncacheable_file_data_xdr, uncacheable_file_data_equal },
	{ FATTR4_UNCACHEABLE_DIRENT_METADATA, uncacheable_dirent_metadata_count,
	  uncacheable_dirent_metadata_xdr, uncacheable_dirent_metadata_equal }
};

int nfs4_attribute_init(void)
{
	bitmap4 *bm = supported_attributes;
	bitmap4_init(bm, FATTR4_ATTRIBUTE_MAX);

	bitmap4_attribute_set(bm, FATTR4_SUPPORTED_ATTRS);
	bitmap4_attribute_set(bm, FATTR4_TYPE);
	bitmap4_attribute_set(bm, FATTR4_FH_EXPIRE_TYPE);
	bitmap4_attribute_set(bm, FATTR4_CHANGE);
	bitmap4_attribute_set(bm, FATTR4_SIZE);
	bitmap4_attribute_set(bm, FATTR4_LINK_SUPPORT);
	bitmap4_attribute_set(bm, FATTR4_SYMLINK_SUPPORT);
	bitmap4_attribute_set(bm, FATTR4_NAMED_ATTR);
	bitmap4_attribute_set(bm, FATTR4_FSID);
	bitmap4_attribute_set(bm, FATTR4_UNIQUE_HANDLES);
	bitmap4_attribute_set(bm, FATTR4_LEASE_TIME);
	bitmap4_attribute_set(bm, FATTR4_RDATTR_ERROR);
	bitmap4_attribute_set(bm, FATTR4_FILEHANDLE);
	bitmap4_attribute_set(bm, FATTR4_SUPPATTR_EXCLCREAT);
	bitmap4_attribute_clear(bm, FATTR4_ACL);
	bitmap4_attribute_clear(bm, FATTR4_ACLSUPPORT);
	bitmap4_attribute_set(bm, FATTR4_ARCHIVE);
	bitmap4_attribute_set(bm, FATTR4_CANSETTIME);
	bitmap4_attribute_set(bm, FATTR4_CASE_INSENSITIVE);
	bitmap4_attribute_set(bm, FATTR4_CASE_PRESERVING);
	bitmap4_attribute_set(bm, FATTR4_CHOWN_RESTRICTED);
	bitmap4_attribute_set(bm, FATTR4_FILEID);
	bitmap4_attribute_set(bm, FATTR4_FILES_AVAIL);
	bitmap4_attribute_set(bm, FATTR4_FILES_FREE);
	bitmap4_attribute_set(bm, FATTR4_FILES_TOTAL);
	bitmap4_attribute_clear(bm, FATTR4_FS_LOCATIONS);
	bitmap4_attribute_set(bm, FATTR4_HIDDEN);
	bitmap4_attribute_set(bm, FATTR4_HOMOGENEOUS);
	bitmap4_attribute_set(bm, FATTR4_MAXFILESIZE);
	bitmap4_attribute_set(bm, FATTR4_MAXLINK);
	bitmap4_attribute_set(bm, FATTR4_MAXNAME);
	bitmap4_attribute_set(bm, FATTR4_MAXREAD);
	bitmap4_attribute_set(bm, FATTR4_MAXWRITE);
	bitmap4_attribute_clear(bm, FATTR4_MIMETYPE);
	bitmap4_attribute_set(bm, FATTR4_MODE);
	bitmap4_attribute_set(bm, FATTR4_NO_TRUNC);
	bitmap4_attribute_set(bm, FATTR4_NUMLINKS);
	bitmap4_attribute_set(bm, FATTR4_OWNER);
	bitmap4_attribute_set(bm, FATTR4_OWNER_GROUP);
	bitmap4_attribute_clear(bm, FATTR4_QUOTA_AVAIL_HARD);
	bitmap4_attribute_clear(bm, FATTR4_QUOTA_AVAIL_SOFT);
	bitmap4_attribute_clear(bm, FATTR4_QUOTA_USED);
	bitmap4_attribute_set(bm, FATTR4_RAWDEV);
	bitmap4_attribute_set(bm, FATTR4_SPACE_AVAIL);
	bitmap4_attribute_set(bm, FATTR4_SPACE_FREE);
	bitmap4_attribute_set(bm, FATTR4_SPACE_TOTAL);
	bitmap4_attribute_set(bm, FATTR4_SPACE_USED);
	bitmap4_attribute_set(bm, FATTR4_SYSTEM);
	bitmap4_attribute_set(bm, FATTR4_TIME_ACCESS);
	bitmap4_attribute_set(bm, FATTR4_TIME_ACCESS_SET);
	bitmap4_attribute_set(bm, FATTR4_TIME_BACKUP);
	bitmap4_attribute_set(bm, FATTR4_TIME_CREATE);
	bitmap4_attribute_set(bm, FATTR4_TIME_DELTA);
	bitmap4_attribute_set(bm, FATTR4_TIME_METADATA);
	bitmap4_attribute_set(bm, FATTR4_TIME_MODIFY);
	bitmap4_attribute_set(bm, FATTR4_TIME_MODIFY_SET);
	bitmap4_attribute_set(bm, FATTR4_MOUNTED_ON_FILEID);
	bitmap4_attribute_set(bm, FATTR4_DIR_NOTIF_DELAY);
	bitmap4_attribute_set(bm, FATTR4_DIRENT_NOTIF_DELAY);
	bitmap4_attribute_clear(bm, FATTR4_DACL);
	bitmap4_attribute_clear(bm, FATTR4_SACL);
	bitmap4_attribute_set(bm, FATTR4_CHANGE_POLICY);
	bitmap4_attribute_set(bm, FATTR4_FS_STATUS);
	bitmap4_attribute_clear(bm, FATTR4_FS_LAYOUT_TYPES);
	bitmap4_attribute_clear(bm, FATTR4_LAYOUT_HINT);
	bitmap4_attribute_clear(bm, FATTR4_LAYOUT_TYPES);
	bitmap4_attribute_clear(bm, FATTR4_LAYOUT_BLKSIZE);
	bitmap4_attribute_clear(bm, FATTR4_LAYOUT_ALIGNMENT);
	bitmap4_attribute_clear(bm, FATTR4_FS_LOCATIONS_INFO);
	bitmap4_attribute_clear(bm, FATTR4_MDSTHRESHOLD);
	bitmap4_attribute_set(bm, FATTR4_RETENTION_GET);
	bitmap4_attribute_set(bm, FATTR4_RETENTION_SET);
	bitmap4_attribute_set(bm, FATTR4_RETENTEVT_GET);
	bitmap4_attribute_set(bm, FATTR4_RETENTEVT_SET);
	bitmap4_attribute_set(bm, FATTR4_RETENTION_HOLD);
	bitmap4_attribute_set(bm, FATTR4_MODE_SET_MASKED);
	bitmap4_attribute_set(bm, FATTR4_FS_CHARSET_CAP);
	bitmap4_attribute_set(bm, FATTR4_CLONE_BLKSIZE);
	bitmap4_attribute_set(bm, FATTR4_SPACE_FREED);
	bitmap4_attribute_set(bm, FATTR4_CHANGE_ATTR_TYPE);
	bitmap4_attribute_clear(bm, FATTR4_SEC_LABEL);
	bitmap4_attribute_clear(bm, FATTR4_MODE_UMASK);
	bitmap4_attribute_clear(bm, FATTR4_XATTR_SUPPORT);
	bitmap4_attribute_set(bm, FATTR4_OFFLINE);
	bitmap4_attribute_clear(bm, FATTR4_TIME_DELEG_ACCESS);
	bitmap4_attribute_clear(bm, FATTR4_TIME_DELEG_MODIFY);
	bitmap4_attribute_set(bm, FATTR4_OPEN_ARGUMENTS);
	bitmap4_attribute_set(bm, FATTR4_UNCACHEABLE_FILE_DATA);
	bitmap4_attribute_set(bm, FATTR4_UNCACHEABLE_DIRENT_METADATA);

	return 0;
}

int nfs4_attribute_fini(void)
{
	bitmap4_destroy(supported_attributes);

	return 0;
}

#ifdef NOT_NOW_BROWN_COW
static void __attribute__((constructor)) nfs4_attribute_load(void)
{
	if (nfs4_attribute_init() != 0)
		abort();
}

static void __attribute__((destructor)) nfs4_attribute_unload(void)
{
	nfs4_attribute_fini();
}
#endif

static void nattr_release(struct nfsv42_attr *nattr)
{
	free(nattr->supported_attrs.bitmap4_val);
	free(nattr->filehandle.nfs_fh4_val);
	utf8string_free(&nattr->owner);
	utf8string_free(&nattr->owner_group);
}

#ifdef NOT_NOW_BROWN_COW
static nfsstat4 nattr_to_inode(struct nfsv42_attr *nattr, struct inode *inode)
{
	inode->i_mode = nattr->type;
	return NFS4_OK;
}
#endif

static nfsstat4 inode_to_nattr(struct inode *inode, struct nfsv42_attr *nattr)
{
	uint16_t type = inode->i_mode & S_IFMT;
	struct super_block *sb = inode->i_sb;
	int ret = 0;

	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);

	switch (type) {
	case S_IFLNK:
		nattr->type = NF4LNK;
		break;
	case S_IFREG:
		nattr->type = NF4REG;
		break;
	case S_IFDIR:
		nattr->type = NF4DIR;
		break;
	case S_IFCHR:
		nattr->type = NF4CHR;
		break;
	case S_IFBLK:
		nattr->type = NF4BLK;
		break;
	case S_IFIFO:
		nattr->type = NF4FIFO;
		break;
	case S_IFSOCK:
		nattr->type = NF4SOCK;
		break;
	}

	ret = bitmap4_copy(supported_attributes, &nattr->supported_attrs);
	if (ret)
		goto out;

	nattr->filehandle.nfs_fh4_val =
		(char *)network_file_handle_construct(sb->sb_id, inode->i_ino);
	if (!nattr->filehandle.nfs_fh4_val)
		goto out;

	nattr->filehandle.nfs_fh4_len = sizeof(struct network_file_handle);

	ret = utf8string_from_uid(&nattr->owner, inode->i_uid);
	if (ret)
		goto out;

	ret = utf8string_from_gid(&nattr->owner_group, inode->i_gid);
	if (ret)
		goto out;

	nattr->fh_expire_type = system_attrs.fh_expire_type;
	nattr->change = timespec_to_ns(&now);
	nattr->size = inode->i_size;
	nattr->link_support = system_attrs.link_support;
	nattr->symlink_support = system_attrs.symlink_support;
	nattr->named_attr = system_attrs.named_attr;
	nattr->fsid.major = inode->i_sb->sb_id;
	nattr->fsid.minor = 0;
	nattr->unique_handles = system_attrs.unique_handles;
	nattr->lease_time = system_attrs.lease_time;
	nattr->rdattr_error = NFS4ERR_DELAY;
	nattr->aclsupport = system_attrs.aclsupport;
	nattr->archive = false;
	nattr->cansettime = system_attrs.cansettime;
	nattr->case_insensitive = system_attrs.case_insensitive;
	nattr->case_preserving = system_attrs.case_preserving;
	nattr->chown_restricted = system_attrs.chown_restricted;

	nattr->fileid = inode->i_ino;
	nattr->files_avail = sb->sb_inodes_max - sb->sb_inodes_used;
	nattr->files_free = sb->sb_inodes_max - sb->sb_inodes_used;
	nattr->files_total = sb->sb_inodes_max;
	nattr->hidden = inode->i_attr_flags & INODE_IS_UNCACHEABLE;
	nattr->homogeneous = system_attrs.homogeneous;
	nattr->maxfilesize = system_attrs.maxfilesize;
	nattr->maxlink = system_attrs.maxlink;
	nattr->maxname = system_attrs.maxname;
	nattr->maxread = system_attrs.maxread;
	nattr->maxwrite = system_attrs.maxwrite;
	nattr->mode = inode->i_mode;
	nattr->no_trunc = system_attrs.no_trunc;
	nattr->numlinks = inode->i_nlink;
	nattr->quota_avail_hard = system_attrs.quota_avail_hard;
	nattr->quota_avail_soft = system_attrs.quota_avail_soft;
	nattr->rawdev.specdata1 = inode->i_dev_major;
	nattr->rawdev.specdata1 = inode->i_dev_minor;
	nattr->space_avail = sb->sb_bytes_max - sb->sb_bytes_used;
	nattr->space_free = sb->sb_bytes_max - sb->sb_bytes_used;
	nattr->space_total = sb->sb_bytes_max;
	nattr->space_used = sb->sb_bytes_used;
	nattr->system = false;

	nattr->time_delta = system_attrs.time_delta;
	timespec_to_nfstime4(&inode->i_ctime, &nattr->time_metadata);
	timespec_to_nfstime4(&inode->i_mtime, &nattr->time_modify);
	timespec_to_nfstime4(&inode->i_atime, &nattr->time_access);
	timespec_to_nfstime4(&inode->i_btime, &nattr->time_create);

	nattr->mounted_on_fileid = 0;
	// nattr->suppattr_exclcreat;
	nattr->fs_charset_cap = system_attrs.fs_charset_cap;
	nattr->clone_blksize = system_attrs.clone_blksize;
	// nattr->space_freed;
	nattr->change_attr_type = system_attrs.change_attr_type;
	// nattr->mode_umask;
	nattr->xattr_support = system_attrs.xattr_support;
	nattr->offline = inode->i_attr_flags & INODE_IS_UNCACHEABLE;
	nattr->uncacheable_file_data = inode->i_attr_flags &
				       INODE_IS_UNCACHEABLE;
	nattr->uncacheable_dirent_metadata = inode->i_attr_flags &
					     INODE_IS_UNCACHEABLE;

out:
	return NFS4_OK;
}

void nfs4_op_getattr(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	GETATTR4args *args = NFS4_OP_ARG_SETUP(c, ph, opgetattr);
	GETATTR4res *res = NFS4_OP_RES_SETUP(c, ph, opgetattr);
	nfsstat4 *status = &res->status;
	GETATTR4resok *resok = NFS4_OP_RESOK_SETUP(res, GETATTR4res_u, resok4);

	bitmap4 *attr_request = &args->attr_request;
	fattr4 *fattr = &resok->obj_attributes;

	u_int i;
	u_int scan_bits = attr_request->bitmap4_len * 32U;

	XDR sptr;
	XDR *xdrs = &sptr;

	int ret = 0;

	struct inode *inode = c->c_inode;
	struct nfsv42_attr nattr = { 0 };

	/* Nothing requested */
	if (attr_request->bitmap4_len == 0)
		goto out;

	ret = bitmap4_init(&fattr->attrmask, FATTR4_ATTRIBUTE_MAX);
	if (ret) {
		*status = errno_to_nfs4(ret, NFS4_OP_NUM(c));
		goto out;
	}

	/* Note: Once we copy it, all bets are off as to the contents! */
	pthread_mutex_lock(&inode->i_attr_mutex);
	ret = inode_to_nattr(inode, &nattr);
	pthread_mutex_unlock(&inode->i_attr_mutex);
	if (ret) {
		*status = errno_to_nfs4(ret, NFS4_OP_NUM(c));
		goto out;
	}

	fattr->attr_vals.attrlist4_len = 0;
	for (i = 0; i < scan_bits; i++) {
		if (bitmap4_attribute_is_set(attr_request, i)) {
			bool b = bitmap4_attribute_is_set(supported_attributes,
							  i);
			if (b) {
				fattr->attr_vals.attrlist4_len +=
					nao[i].nao_count(&nattr);
			}
		}
	}

	fattr->attr_vals.attrlist4_val =
		calloc(fattr->attr_vals.attrlist4_len, sizeof(char));
	if (!fattr->attr_vals.attrlist4_val) {
		*status = NFS4ERR_DELAY;
		fattr->attr_vals.attrlist4_len = 0;
		goto out;
	}

	xdrmem_create(xdrs, fattr->attr_vals.attrlist4_val,
		      fattr->attr_vals.attrlist4_len, XDR_ENCODE);

	for (i = 0; i < scan_bits; i++) {
		if (bitmap4_attribute_is_set(attr_request, i)) {
			bool b = bitmap4_attribute_is_set(supported_attributes,
							  i);
			if (b) {
				*status = nao[i].nao_xdr(xdrs, &nattr);
				if (*status) {
					fattr->attr_vals.attrlist4_len = 0;
					free(fattr->attr_vals.attrlist4_val);
					fattr->attr_vals.attrlist4_val = NULL;
					break;
				}
			}
		}
	}

	xdr_destroy(xdrs);

out:
	nattr_release(&nattr);
	LOG("%s status=%s(%d) args=%p res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)args, (void *)res,
	    (void *)resok);
}

void nfs4_op_readdir(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	READDIR4args *args = NFS4_OP_ARG_SETUP(c, ph, opreaddir);
	READDIR4res *res = NFS4_OP_RES_SETUP(c, ph, opreaddir);
	nfsstat4 *status = &res->status;
	READDIR4resok *resok = NFS4_OP_RESOK_SETUP(res, READDIR4res_u, resok4);

	if (network_file_handle_empty(&c->c_curr_nfh)) {
		*status = NFS4ERR_BADHANDLE;
		goto out;
	}

	*status = NFS4ERR_NOTSUPP;

out:
	LOG("%s status=%s(%d) args=%p res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)args, (void *)res,
	    (void *)resok);
}

void nfs4_op_setattr(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	SETATTR4args *args = NFS4_OP_ARG_SETUP(c, ph, opsetattr);
	SETATTR4res *res = NFS4_OP_RES_SETUP(c, ph, opsetattr);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}

void nfs4_op_verify(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	VERIFY4args *args = NFS4_OP_ARG_SETUP(c, ph, opverify);
	VERIFY4res *res = NFS4_OP_RES_SETUP(c, ph, opverify);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}

void nfs4_op_nverify(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	NVERIFY4args *args = NFS4_OP_ARG_SETUP(c, ph, opnverify);
	NVERIFY4res *res = NFS4_OP_RES_SETUP(c, ph, opnverify);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}

void nfs4_op_access(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	ACCESS4args *args = NFS4_OP_ARG_SETUP(c, ph, opaccess);
	ACCESS4res *res = NFS4_OP_RES_SETUP(c, ph, opaccess);
	nfsstat4 *status = &res->status;
	ACCESS4resok *resok = NFS4_OP_RESOK_SETUP(res, ACCESS4res_u, resok4);

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)args, (void *)res,
	    (void *)resok);
}

void nfs4_op_access_mask(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	ACCESS_MASK4args *args = NFS4_OP_ARG_SETUP(c, ph, opaccess_mask);
	ACCESS_MASK4res *res = NFS4_OP_RES_SETUP(c, ph, opaccess_mask);
	nfsstat4 *status = &res->amr_status;
	ACCESS_MASK4resok *resok =
		NFS4_OP_RESOK_SETUP(res, ACCESS_MASK4res_u, amr_resok4);

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)args, (void *)res,
	    (void *)resok);
}
