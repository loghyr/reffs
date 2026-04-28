/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * Custom libtirpc CLIENT* whose call/recv go through SSL_read /
 * SSL_write (via tls_rpc_send / tls_rpc_recv from
 * lib/include/reffs/tls_client.h).
 *
 * The PS-MDS session (lib/nfs4/client/mds_session.c) uses the
 * libtirpc CLIENT API (clnt_call / cl_auth / clnt_destroy) for
 * every wire op (EXCHANGE_ID, CREATE_SESSION, SEQUENCE,
 * PROXY_REGISTRATION, PROXY_*).  Slice plan-1-tls.b will swap
 * mds_session_clnt_open's plain-TCP path to call
 * mds_tls_xprt_create when the [[proxy_mds]] config carries TLS
 * cert paths; everything above clnt_call stays unchanged.
 *
 * Out of scope for slice plan-1-tls.a:
 *   - mds_session integration (1-tls.b)
 *   - [[proxy_mds]] tls_cert / tls_key / tls_ca config (1-tls.b)
 *   - mini-CA fixture + end-to-end smoke (1-tls.c)
 *
 * The XPRT owns the SSL* and the underlying fd: clnt_destroy
 * runs SSL_shutdown -> SSL_free -> close(fd) in that order.
 *
 * Single-threaded per-CLIENT.  Callers serialise via the
 * mds_session ms_call_mutex (lib/nfs4/client/mds_session.h); the
 * XPRT itself does not lock.
 */

#ifndef _REFFS_NFS4_CLIENT_MDS_TLS_XPRT_H
#define _REFFS_NFS4_CLIENT_MDS_TLS_XPRT_H

#include <openssl/ssl.h>
#include <stdint.h>
#include <rpc/rpc.h>

/*
 * Take ownership of fd + ssl and produce a libtirpc CLIENT*
 * bound to (prog, vers).  On success, caller frees via
 * clnt_destroy (which tears down ssl + fd).  On NULL return,
 * caller still owns fd + ssl and must clean up.
 *
 * prog / vers use uint32_t rather than rpcprog_t / rpcvers_t to
 * keep this header portable across libtirpc and BSD-derived RPC
 * headers (the libtirpc typedefs are not present on every host).
 */
CLIENT *mds_tls_xprt_create(int fd, SSL *ssl, uint32_t prog, uint32_t vers);

#endif /* _REFFS_NFS4_CLIENT_MDS_TLS_XPRT_H */
