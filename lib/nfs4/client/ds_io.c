/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * NFSv3 data server I/O for the EC demo client.
 *
 * Connects to a data server with AUTH_SYS credentials (synthetic
 * uid/gid from the Flex Files layout) and does READ/WRITE using
 * the filehandle from LAYOUTGET.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <rpc/rpc.h>
#include <rpc/auth.h>

#include "nfsv3_xdr.h"
#include "ec_client.h"

#define DS_RPC_TIMEOUT_SEC 30

/* ------------------------------------------------------------------ */
/* Connect / disconnect                                                */
/* ------------------------------------------------------------------ */

int ds_connect(struct ds_conn *dc, const struct ec_device *dev, uint32_t uid,
	       uint32_t gid)
{
	memset(dc, 0, sizeof(*dc));

	dc->dc_clnt = clnt_create(dev->ed_host, NFS3_PROGRAM, NFS_V3, "tcp");
	if (!dc->dc_clnt)
		return -ECONNREFUSED;

	/* Set AUTH_SYS with the synthetic uid/gid from the layout. */
	AUTH *auth = authunix_create("ec_demo", uid, gid, 0, NULL);

	if (auth) {
		auth_destroy(dc->dc_clnt->cl_auth);
		dc->dc_clnt->cl_auth = auth;
	}

	return 0;
}

void ds_disconnect(struct ds_conn *dc)
{
	if (dc->dc_clnt) {
		clnt_destroy(dc->dc_clnt);
		dc->dc_clnt = NULL;
	}
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static nfs_fh3 make_fh3(const uint8_t *fh, uint32_t len)
{
	nfs_fh3 f;

	f.data.data_val = (char *)fh;
	f.data.data_len = len;
	return f;
}

/* ------------------------------------------------------------------ */
/* WRITE                                                               */
/* ------------------------------------------------------------------ */

int ds_write(struct ds_conn *dc, const uint8_t *fh, uint32_t fh_len,
	     uint64_t offset, const uint8_t *data, uint32_t len)
{
	WRITE3args args;
	WRITE3res res;
	struct timeval tv = { .tv_sec = DS_RPC_TIMEOUT_SEC, .tv_usec = 0 };
	enum clnt_stat rpc_stat;

	memset(&args, 0, sizeof(args));
	memset(&res, 0, sizeof(res));

	args.file = make_fh3(fh, fh_len);
	args.offset = offset;
	args.count = len;
	args.stable = FILE_SYNC;
	args.data.data_val = (char *)data;
	args.data.data_len = len;

	rpc_stat = clnt_call(dc->dc_clnt, NFSPROC3_WRITE,
			     (xdrproc_t)xdr_WRITE3args, (caddr_t)&args,
			     (xdrproc_t)xdr_WRITE3res, (caddr_t)&res, tv);

	if (rpc_stat != RPC_SUCCESS) {
		xdr_free((xdrproc_t)xdr_WRITE3res, (caddr_t)&res);
		return -EIO;
	}

	if (res.status != NFS3_OK) {
		xdr_free((xdrproc_t)xdr_WRITE3res, (caddr_t)&res);
		return -EIO;
	}

	xdr_free((xdrproc_t)xdr_WRITE3res, (caddr_t)&res);
	return 0;
}

/* ------------------------------------------------------------------ */
/* READ                                                                */
/* ------------------------------------------------------------------ */

int ds_read(struct ds_conn *dc, const uint8_t *fh, uint32_t fh_len,
	    uint64_t offset, uint8_t *data, uint32_t len, uint32_t *nread)
{
	READ3args args;
	READ3res res;
	struct timeval tv = { .tv_sec = DS_RPC_TIMEOUT_SEC, .tv_usec = 0 };
	enum clnt_stat rpc_stat;

	memset(&args, 0, sizeof(args));
	memset(&res, 0, sizeof(res));

	args.file = make_fh3(fh, fh_len);
	args.offset = offset;
	args.count = len;

	rpc_stat = clnt_call(dc->dc_clnt, NFSPROC3_READ,
			     (xdrproc_t)xdr_READ3args, (caddr_t)&args,
			     (xdrproc_t)xdr_READ3res, (caddr_t)&res, tv);

	if (rpc_stat != RPC_SUCCESS) {
		xdr_free((xdrproc_t)xdr_READ3res, (caddr_t)&res);
		return -EIO;
	}

	if (res.status != NFS3_OK) {
		xdr_free((xdrproc_t)xdr_READ3res, (caddr_t)&res);
		return -EIO;
	}

	READ3resok *resok = &res.READ3res_u.resok;
	uint32_t got = resok->data.data_len;

	if (got > len)
		got = len;
	memcpy(data, resok->data.data_val, got);
	*nread = got;

	xdr_free((xdrproc_t)xdr_READ3res, (caddr_t)&res);
	return 0;
}
