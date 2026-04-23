/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <rpc/rpc.h>

#include "mntv3_xdr.h"

#include "ps_mount_client.h"

void ps_mount_free_exports(struct ps_export_entry *arr)
{
	free(arr);
}

/*
 * Walk the exports linked list, counting entries.  Bounded by PS's
 * own registry capacity in practice; we cap here at 1024 to avoid
 * a pathological server pinning the caller's memory.
 */
#define PS_MOUNT_MAX_EXPORTS 1024

static size_t count_exports(const exports head)
{
	size_t n = 0;
	const struct exportnode *p;

	for (p = head; p && n < PS_MOUNT_MAX_EXPORTS; p = p->ex_next)
		n++;
	return n;
}

int ps_mount_fetch_exports(const char *host, struct ps_export_entry **out,
			   size_t *nout)
{
	CLIENT *clnt;
	exports list = NULL;
	enum clnt_stat rpc_stat;
	struct timeval tmo = { .tv_sec = 10, .tv_usec = 0 };
	int ret = 0;

	if (!host || host[0] == '\0' || !out || !nout)
		return -EINVAL;

	*out = NULL;
	*nout = 0;

	clnt = clnt_create(host, MOUNT_PROGRAM, MOUNT_V3, "tcp");
	if (!clnt)
		return -ECONNREFUSED;

	/*
	 * TIRPC declares xdr_void() with the legacy `bool_t (*)(void)`
	 * signature, which mismatches the xdrproc_t prototype.  The
	 * two-step cast through `void *` silences the
	 * -Wcast-function-type-mismatch diagnostic without losing
	 * validation on the real XDR procs.
	 */
	rpc_stat = clnt_call(clnt, MOUNTPROC3_EXPORT,
			     (xdrproc_t)(void *)xdr_void, NULL,
			     (xdrproc_t)xdr_exports, (caddr_t)&list, tmo);
	if (rpc_stat != RPC_SUCCESS) {
		ret = -EIO;
		goto out;
	}

	size_t n = count_exports(list);

	if (n == 0)
		goto out;

	struct ps_export_entry *arr = calloc(n, sizeof(*arr));

	if (!arr) {
		ret = -ENOMEM;
		goto out;
	}

	size_t i = 0;
	const struct exportnode *p;

	for (p = list; p && i < n; p = p->ex_next, i++) {
		if (p->ex_dir)
			strncpy(arr[i].path, p->ex_dir, PS_MOUNT_PATH_MAX - 1);
	}

	*out = arr;
	*nout = i;

out:
	if (list)
		clnt_freeres(clnt, (xdrproc_t)xdr_exports, (caddr_t)&list);
	clnt_destroy(clnt);
	return ret;
}
