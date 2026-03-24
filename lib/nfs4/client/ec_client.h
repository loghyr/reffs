/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * EC demo client — minimal NFSv4.2 client for erasure-coding demonstration.
 *
 * Talks NFSv4.2 to the MDS (EXCHANGE_ID, CREATE_SESSION, SEQUENCE,
 * OPEN, LAYOUTGET, GETDEVICEINFO, LAYOUTRETURN, CLOSE) and NFSv3 to
 * the data servers (READ, WRITE).
 */

#ifndef _REFFS_EC_CLIENT_H
#define _REFFS_EC_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#include <rpc/rpc.h>

#include "nfsv42_xdr.h"

/* ------------------------------------------------------------------ */
/* MDS session                                                         */
/* ------------------------------------------------------------------ */

struct mds_session {
	CLIENT *ms_clnt;
	clientid4 ms_clientid;
	sequenceid4 ms_create_seq;
	sessionid4 ms_sessionid;
	uint32_t ms_slot_seqid; /* next seqid for slot 0 */
	char ms_owner[256]; /* client owner string for EXCHANGE_ID */
};

/*
 * mds_session_set_owner -- set the client owner string before create.
 *
 * Builds "hostname:id" where id defaults to the PID if not provided.
 * Must be called before mds_session_create().
 */
void mds_session_set_owner(struct mds_session *ms, const char *id);

int mds_session_create(struct mds_session *ms, const char *host);
void mds_session_destroy(struct mds_session *ms);

/* ------------------------------------------------------------------ */
/* COMPOUND builder                                                    */
/* ------------------------------------------------------------------ */

/*
 * Simple COMPOUND builder for the client side.  Allocates an argarray
 * of max_ops entries, tracks the current count, and sends via clnt_call.
 */
struct mds_compound {
	COMPOUND4args mc_args;
	COMPOUND4res mc_res;
	uint32_t mc_max_ops;
	uint32_t mc_count;
};

int mds_compound_init(struct mds_compound *mc, uint32_t max_ops,
		      const char *tag);
void mds_compound_fini(struct mds_compound *mc);

/* Returns pointer to the next nfs_argop4 slot (caller fills the union). */
nfs_argop4 *mds_compound_add_op(struct mds_compound *mc, nfs_opnum4 op);

/* Add SEQUENCE as the first op, using session state. */
int mds_compound_add_sequence(struct mds_compound *mc, struct mds_session *ms);

/* Send the COMPOUND and receive the response. */
int mds_compound_send(struct mds_compound *mc, struct mds_session *ms);

/* Access result for op at index i. */
static inline nfs_resop4 *mds_compound_result(struct mds_compound *mc,
					      uint32_t i)
{
	if (i >= mc->mc_res.resarray.resarray_len)
		return NULL;
	return &mc->mc_res.resarray.resarray_val[i];
}

/* ------------------------------------------------------------------ */
/* File operations                                                     */
/* ------------------------------------------------------------------ */

struct mds_file {
	stateid4 mf_stateid; /* open stateid */
	nfs_fh4 mf_fh; /* current filehandle */
};

int mds_file_open(struct mds_session *ms, const char *path,
		  struct mds_file *mf);
int mds_file_close(struct mds_session *ms, struct mds_file *mf);

/* ------------------------------------------------------------------ */
/* Layout operations                                                   */
/* ------------------------------------------------------------------ */

/* Parsed mirror from a Flex Files layout. */
struct ec_mirror {
	deviceid4 em_deviceid;
	uint32_t em_efficiency;
	uint8_t em_fh[128]; /* NFSv3/NFSv4 filehandle */
	uint32_t em_fh_len;
	uint32_t em_uid;
	uint32_t em_gid;
	uint32_t em_flags; /* FFV2_DS_FLAGS_* (v2 only, 0 for v1) */
};

/* Parsed layout from LAYOUTGET. */
struct ec_layout {
	stateid4 el_stateid;
	layouttype4 el_layout_type; /* v1 or v2 */
	uint32_t el_stripe_unit; /* v1: ffl_stripe_unit */
	uint32_t el_chunk_size; /* v2: ffm_striping_unit_size */
	uint32_t el_coding_type; /* v2: ffv2_coding_type4 */
	uint32_t el_nmirrors;
	struct ec_mirror *el_mirrors;
};

int mds_layout_get(struct mds_session *ms, struct mds_file *mf,
		   layoutiomode4 iomode, layouttype4 layout_type,
		   struct ec_layout *layout);
int mds_layout_return(struct mds_session *ms, struct mds_file *mf,
		      struct ec_layout *layout);
void ec_layout_free(struct ec_layout *layout);

/* Resolved data server address from GETDEVICEINFO. */
struct ec_device {
	char ed_host[256];
	uint16_t ed_port;
};

int mds_getdeviceinfo(struct mds_session *ms, const deviceid4 devid,
		      layouttype4 layout_type, struct ec_device *dev);

/* ------------------------------------------------------------------ */
/* DS I/O (NFSv3)                                                      */
/* ------------------------------------------------------------------ */

struct ds_conn {
	CLIENT *dc_clnt;
};

int ds_connect(struct ds_conn *dc, const struct ec_device *dev, uint32_t uid,
	       uint32_t gid);
void ds_disconnect(struct ds_conn *dc);

int ds_write(struct ds_conn *dc, const uint8_t *fh, uint32_t fh_len,
	     uint64_t offset, const uint8_t *data, uint32_t len);
int ds_read(struct ds_conn *dc, const uint8_t *fh, uint32_t fh_len,
	    uint64_t offset, uint8_t *data, uint32_t len, uint32_t *nread);

/* ------------------------------------------------------------------ */
/* Plain I/O — write/read through layout, no erasure coding            */
/* ------------------------------------------------------------------ */

int plain_write(struct mds_session *ms, const char *path, const uint8_t *data,
		size_t data_len);
int plain_read(struct mds_session *ms, const char *path, uint8_t *buf,
	       size_t buf_len, size_t *out_len);

/* ------------------------------------------------------------------ */
/* EC I/O — high-level erasure-coded write/read                        */
/* ------------------------------------------------------------------ */

enum ec_codec_type {
	EC_CODEC_RS = 0, /* Reed-Solomon (default) */
	EC_CODEC_MOJETTE_SYS = 1, /* Mojette systematic */
	EC_CODEC_MOJETTE_NONSYS = 2, /* Mojette non-systematic */
};

int ec_write(struct mds_session *ms, const char *path, const uint8_t *data,
	     size_t data_len, int k, int m);
int ec_read(struct mds_session *ms, const char *path, uint8_t *buf,
	    size_t buf_len, size_t *out_len, int k, int m);

int ec_write_codec(struct mds_session *ms, const char *path,
		   const uint8_t *data, size_t data_len, int k, int m,
		   enum ec_codec_type codec_type);
int ec_read_codec(struct mds_session *ms, const char *path, uint8_t *buf,
		  size_t buf_len, size_t *out_len, int k, int m,
		  enum ec_codec_type codec_type);

#endif /* _REFFS_EC_CLIENT_H */
