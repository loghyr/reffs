/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include <rpc/rpc.h>

#include "mntv3_xdr.h"

#include "ps_mount_client.h"
#include "ps_state.h" /* PS_MAX_EXPORTS_PER_LISTENER */

void ps_mount_free_exports(struct ps_export_entry *arr)
{
	free(arr);
}

/*
 * Walk the exports linked list, counting entries.  Cap at the PS
 * registry capacity (PS_MAX_EXPORTS_PER_LISTENER) since exports past
 * that are silently dropped by ps_state_add_export.  An MDS that
 * advertises more is logged so the operator can see the truncation
 * rather than wonder why some exports are missing.
 */
#define PS_MOUNT_MAX_EXPORTS PS_MAX_EXPORTS_PER_LISTENER

static size_t count_exports(const exports head)
{
	size_t n = 0;
	const struct exportnode *p;

	for (p = head; p; p = p->ex_next)
		n++;
	if (n > PS_MOUNT_MAX_EXPORTS) {
		fprintf(stderr,
			"ps_mount: upstream advertises %zu exports, "
			"PS will only register the first %d "
			"(PS_MAX_EXPORTS_PER_LISTENER)\n",
			n, PS_MOUNT_MAX_EXPORTS);
		n = PS_MOUNT_MAX_EXPORTS;
	}
	return n;
}

int ps_mount_fetch_exports(const char *host, uint16_t port,
			   struct ps_export_entry **out, size_t *nout)
{
	CLIENT *clnt = NULL;
	exports list = NULL;
	enum clnt_stat rpc_stat;
	struct timeval tmo = { .tv_sec = 10, .tv_usec = 0 };
	int ret = 0;

	if (!host || host[0] == '\0' || !out || !nout)
		return -EINVAL;

	*out = NULL;
	*nout = 0;

	if (port > 0) {
		/*
		 * Explicit-port direct connect: bypass portmap.  Required
		 * for container topologies where the host's rpcbind has no
		 * MOUNT_V3 service registered (e.g. the bench MDS runs in
		 * docker; the host's rpcbind doesn't see it).  Mirrors
		 * ds_io.c's bdde4f6539db pattern and the mds_session
		 * explicit-port path.
		 */
		struct addrinfo hints = { .ai_family = AF_INET,
					  .ai_socktype = SOCK_STREAM };
		struct addrinfo *res = NULL;
		struct sockaddr_in sin;

		if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res)
			return -ECONNREFUSED;

		/*
		 * AF_INET is a hint, not a guarantee; some resolvers can
		 * return AF_INET6 if no v4 record exists.  Casting an
		 * AF_INET6 sockaddr_in6 to sockaddr_in and writing
		 * sin_port aliases the wrong byte offset and produces a
		 * connection to a garbage port (silent failure -> harder
		 * to diagnose).  Reject non-v4 returns explicitly.
		 */
		if (res->ai_addr->sa_family != AF_INET) {
			freeaddrinfo(res);
			return -ECONNREFUSED;
		}

		sin = *(struct sockaddr_in *)res->ai_addr;
		freeaddrinfo(res);
		sin.sin_port = htons(port);

		int fd = RPC_ANYSOCK;

		clnt = clnttcp_create(&sin, MOUNT_PROGRAM, MOUNT_V3, &fd, 0, 0);
	} else {
		/*
		 * Portmap-driven path: works against any upstream whose
		 * host's rpcbind knows about MOUNT_V3.  reffsd
		 * pmap-registers MOUNT3 at startup, so this resolves
		 * cleanly for non-containerised deployments.
		 */
		clnt = clnt_create(host, MOUNT_PROGRAM, MOUNT_V3, "tcp");
	}

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
