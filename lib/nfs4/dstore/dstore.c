/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * Data store lifecycle.
 *
 * Dstores live in a global RCU-protected cds_lfht keyed by ds_id.
 * Each dstore is refcounted; the hash table holds one ref, and each
 * caller that obtains a pointer via dstore_find() or dstore_alloc()
 * holds another.
 *
 * At startup the MDS connects to each configured data server via
 * MOUNT to obtain its root filehandle and stores the libtirpc CLIENT
 * handle for subsequent NFSv3 control-plane operations.
 */

#include <assert.h>
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <rpc/rpc.h>
#include <xxhash.h>

#include "mntv3_xdr.h"
#include "nfsv3_xdr.h"
#include "reffs/dstore.h"
#include "reffs/dstore_ops.h"
#include "reffs/filehandle.h"
#include "reffs/inode.h"
#include "reffs/super_block.h"
#include "reffs/log.h"
#include "reffs/runway.h"
#include "reffs/trace/dstore.h"

/* ------------------------------------------------------------------ */
/* Global hash table                                                   */
/* ------------------------------------------------------------------ */

static struct cds_lfht *g_dstore_ht;

/* ------------------------------------------------------------------ */
/* Hash helpers                                                        */
/* ------------------------------------------------------------------ */

static unsigned long dstore_hash(uint32_t id)
{
	return XXH3_64bits(&id, sizeof(id));
}

static int dstore_match(struct cds_lfht_node *ht_node, const void *vkey)
{
	struct dstore *ds = caa_container_of(ht_node, struct dstore, ds_node);
	const uint32_t *key = vkey;

	return *key == ds->ds_id;
}

/* ------------------------------------------------------------------ */
/* RCU / refcount                                                      */
/* ------------------------------------------------------------------ */

static void dstore_free_rcu(struct rcu_head *rcu)
{
	struct dstore *ds = caa_container_of(rcu, struct dstore, ds_rcu);

	trace_dstore(ds, __func__, __LINE__);
	if (ds->ds_runway)
		runway_destroy(ds->ds_runway);
	if (ds->ds_clnt)
		clnt_destroy(ds->ds_clnt);
	pthread_mutex_destroy(&ds->ds_clnt_mutex);
	free(ds);
}

static void dstore_release(struct urcu_ref *ref)
{
	struct dstore *ds = caa_container_of(ref, struct dstore, ds_ref);

	trace_dstore(ds, __func__, __LINE__);
	dstore_unhash(ds);
	call_rcu(&ds->ds_rcu, dstore_free_rcu);
}

struct dstore *dstore_get(struct dstore *ds)
{
	if (!ds)
		return NULL;
	if (!urcu_ref_get_unless_zero(&ds->ds_ref))
		return NULL;
	trace_dstore(ds, __func__, __LINE__);
	return ds;
}

void dstore_put(struct dstore *ds)
{
	if (!ds)
		return;
	trace_dstore(ds, __func__, __LINE__);
	urcu_ref_put(&ds->ds_ref, dstore_release);
}

bool dstore_unhash(struct dstore *ds)
{
	uint64_t state;
	int __attribute__((unused)) ret;

	state = __atomic_fetch_and(&ds->ds_state, ~DSTORE_IS_HASHED,
				   __ATOMIC_ACQUIRE);
	if (!(state & DSTORE_IS_HASHED))
		return false;

	if (!g_dstore_ht)
		return false;

	trace_dstore(ds, __func__, __LINE__);
	ret = cds_lfht_del(g_dstore_ht, &ds->ds_node);
	assert(!ret);
	return true;
}

/* ------------------------------------------------------------------ */
/* Module lifecycle                                                    */
/* ------------------------------------------------------------------ */

int dstore_init(void)
{
	g_dstore_ht = cds_lfht_new(16, 16, 0, CDS_LFHT_AUTO_RESIZE, NULL);
	if (!g_dstore_ht)
		return -ENOMEM;
	return 0;
}

void dstore_fini(void)
{
	TRACE("dstore_fini: draining");
	dstore_unload_all();
	rcu_barrier();
	TRACE("dstore_fini: rcu_barrier complete");
	if (g_dstore_ht) {
		cds_lfht_destroy(g_dstore_ht, NULL);
		g_dstore_ht = NULL;
	}
}

/* ------------------------------------------------------------------ */
/* MOUNT client                                                        */
/* ------------------------------------------------------------------ */

static void resolve_ds_ip(struct dstore *ds)
{
	struct addrinfo hints, *res;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;

	if (getaddrinfo(ds->ds_address, NULL, &hints, &res) != 0) {
		/* Fall back to using the address string as-is. */
		strncpy(ds->ds_ip, ds->ds_address, sizeof(ds->ds_ip) - 1);
		return;
	}

	struct sockaddr_in *sin = (struct sockaddr_in *)res->ai_addr;

	inet_ntop(AF_INET, &sin->sin_addr, ds->ds_ip, sizeof(ds->ds_ip));
	freeaddrinfo(res);
}

static int mount_get_root_fh(struct dstore *ds)
{
	CLIENT *mnt_clnt;
	mountres3 res;
	struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
	dirpath path;
	enum clnt_stat rpc_stat;
	int ret = 0;

	if (ds->ds_port > 0) {
		/*
		 * Explicit port -> bypass portmap.  reffsd-as-DS does not
		 * register MOUNT_V3 with rpcbind on a non-standard port,
		 * so the portmap lookup would fail.  Connect directly to
		 * <address>:<port> using clnttcp_create.
		 */
		struct sockaddr_in sin;
		struct addrinfo hints = {
			.ai_family = AF_INET,
			.ai_socktype = SOCK_STREAM,
		};
		struct addrinfo *res = NULL;

		if (getaddrinfo(ds->ds_address, NULL, &hints, &res) != 0 ||
		    !res) {
			LOG("dstore[%u]: getaddrinfo(%s) failed", ds->ds_id,
			    ds->ds_address);
			return -ECONNREFUSED;
		}
		sin = *(struct sockaddr_in *)res->ai_addr;
		freeaddrinfo(res);
		sin.sin_port = htons(ds->ds_port);

		int fd = RPC_ANYSOCK;

		mnt_clnt = clnttcp_create(&sin, MOUNT_PROGRAM, MOUNT_V3, &fd,
					  0, 0);
		if (!mnt_clnt) {
			LOG("dstore[%u]: clnttcp_create(%s:%u) MOUNT failed",
			    ds->ds_id, ds->ds_address, ds->ds_port);
			return -ECONNREFUSED;
		}
	} else {
		mnt_clnt = clnt_create(ds->ds_address, MOUNT_PROGRAM, MOUNT_V3,
				       "tcp");
		if (!mnt_clnt) {
			LOG("dstore[%u]: clnt_create(%s) MOUNT failed: %s",
			    ds->ds_id, ds->ds_address,
			    clnt_spcreateerror(""));
			return -ECONNREFUSED;
		}
	}

	path = ds->ds_path;
	memset(&res, 0, sizeof(res));

	rpc_stat = clnt_call(mnt_clnt, MOUNTPROC3_MNT, (xdrproc_t)xdr_dirpath,
			     (caddr_t)&path, (xdrproc_t)xdr_mountres3,
			     (caddr_t)&res, tv);
	if (rpc_stat != RPC_SUCCESS) {
		LOG("dstore[%u]: MOUNT %s:%s RPC failed: %s", ds->ds_id,
		    ds->ds_address, ds->ds_path, clnt_sperror(mnt_clnt, ""));
		ret = -EIO;
		goto out;
	}

	if (res.fhs_status != MNT3_OK) {
		LOG("dstore[%u]: MOUNT %s:%s status=%d", ds->ds_id,
		    ds->ds_address, ds->ds_path, res.fhs_status);
		ret = -ENOENT;
		goto out_free;
	}

	mountres3_ok *ok = &res.mountres3_u.mountinfo;

	if (ok->fhandle.fhandle3_len > DSTORE_MAX_FH) {
		LOG("dstore[%u]: FH too large (%u > %d)", ds->ds_id,
		    ok->fhandle.fhandle3_len, DSTORE_MAX_FH);
		ret = -EOVERFLOW;
		goto out_free;
	}

	memcpy(ds->ds_root_fh, ok->fhandle.fhandle3_val,
	       ok->fhandle.fhandle3_len);
	ds->ds_root_fh_len = ok->fhandle.fhandle3_len;

out_free:
	xdr_free((xdrproc_t)xdr_mountres3, (caddr_t)&res);
out:
	clnt_destroy(mnt_clnt);

	if (ret < 0)
		return ret;

	/*
	 * MOUNT gave us the root FH.  Now create the NFS program client
	 * that will be used for all subsequent control-plane RPCs
	 * (CREATE, GETATTR, SETATTR, REMOVE).  Same port-or-portmap
	 * choice as MOUNT above.
	 */
	if (ds->ds_port > 0) {
		struct sockaddr_in sin;
		struct addrinfo hints = {
			.ai_family = AF_INET,
			.ai_socktype = SOCK_STREAM,
		};
		struct addrinfo *res = NULL;

		if (getaddrinfo(ds->ds_address, NULL, &hints, &res) != 0 ||
		    !res) {
			LOG("dstore[%u]: getaddrinfo(%s) failed (NFS)",
			    ds->ds_id, ds->ds_address);
			ds->ds_root_fh_len = 0;
			return -ECONNREFUSED;
		}
		sin = *(struct sockaddr_in *)res->ai_addr;
		freeaddrinfo(res);
		sin.sin_port = htons(ds->ds_port);

		int fd = RPC_ANYSOCK;

		ds->ds_clnt = clnttcp_create(&sin, NFS3_PROGRAM, NFS_V3, &fd,
					     0, 0);
	} else {
		ds->ds_clnt = clnt_create(ds->ds_address, NFS3_PROGRAM,
					  NFS_V3, "tcp");
	}
	if (!ds->ds_clnt) {
		LOG("dstore[%u]: clnt_create(%s:%u) NFS failed: %s", ds->ds_id,
		    ds->ds_address, ds->ds_port, clnt_spcreateerror(""));
		ds->ds_root_fh_len = 0;
		return -ECONNREFUSED;
	}

	/*
	 * Control-plane ops (SETATTR uid/gid for fencing, CREATE, REMOVE)
	 * require root privileges on the DS.  Set AUTH_SYS uid=0/gid=0.
	 */
	ds->ds_clnt->cl_auth = authsys_create("", 0, 0, 0, NULL);

	/*
	 * Resolve hostname to dotted-decimal IP for use in GETDEVICEINFO
	 * uaddrs.  The uaddr format requires a numeric IPv4 address.
	 */
	resolve_ds_ip(ds);

	__atomic_or_fetch(&ds->ds_state, DSTORE_IS_MOUNTED, __ATOMIC_RELEASE);

	TRACE("dstore[%u]: mounted %s:%s (FH %u bytes)", ds->ds_id,
	      ds->ds_address, ds->ds_path, ds->ds_root_fh_len);

	return 0;
}

/*
 * Check if an address matches any local network interface.
 * Used by combined mode to detect that a DS address is the
 * local machine (use VFS vtable instead of NFSv3 RPC).
 */
static bool dstore_address_is_local(const char *address)
{
	struct ifaddrs *ifa_list, *ifa;
	bool local = false;

	if (getifaddrs(&ifa_list) < 0)
		return false;

	for (ifa = ifa_list; ifa; ifa = ifa->ifa_next) {
		if (!ifa->ifa_addr)
			continue;
		char buf[INET6_ADDRSTRLEN];

		if (ifa->ifa_addr->sa_family == AF_INET) {
			struct sockaddr_in *sin =
				(struct sockaddr_in *)ifa->ifa_addr;
			inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf));
		} else if (ifa->ifa_addr->sa_family == AF_INET6) {
			struct sockaddr_in6 *sin6 =
				(struct sockaddr_in6 *)ifa->ifa_addr;
			inet_ntop(AF_INET6, &sin6->sin6_addr, buf, sizeof(buf));
		} else {
			continue;
		}

		if (!strcmp(address, buf)) {
			local = true;
			break;
		}
	}

	freeifaddrs(ifa_list);
	return local;
}

/* ------------------------------------------------------------------ */
/* Root access probe                                                   */
/* ------------------------------------------------------------------ */

/*
 * dstore_probe_root_access -- verify that the MDS can create files on
 * the DS export with uid=0.  If the DS has root_squash enabled for the
 * MDS address, NFSv3 CREATE will return NFS3ERR_ACCES or NFS3ERR_PERM.
 *
 * Breadcrumb cleanup: remove any stale .root_probe file left by a prior
 * crash or unclean shutdown before creating the new one.
 *
 * Returns 0 if root access is confirmed, -EACCES if root is squashed
 * (LOG emitted), or another negative errno for unexpected failures.
 */
int dstore_probe_root_access(struct dstore *ds)
{
	uint8_t probe_fh[RUNWAY_MAX_FH];
	uint32_t probe_fh_len = 0;
	int ret;

	/*
	 * Breadcrumb cleanup: silently remove any .root_probe left from a
	 * prior run that did not complete cleanly.  ENOENT is expected and
	 * ignored; other errors are also ignored -- the CREATE below will
	 * surface any real problem.
	 */
	dstore_data_file_remove(ds, ds->ds_root_fh, ds->ds_root_fh_len,
				".root_probe");

	ret = dstore_data_file_create(ds, ds->ds_root_fh, ds->ds_root_fh_len,
				      ".root_probe", probe_fh, &probe_fh_len);
	if (ret == -EACCES || ret == -EPERM) {
		LOG("DS %s:%s denies root access (root_squash likely set) -- "
		    "MDS control-plane will fail; set root_squash=false "
		    "for the MDS address on the DS export",
		    ds->ds_address, ds->ds_path);
		return -EACCES;
	}
	if (ret < 0) {
		TRACE("dstore[%u]: root access probe failed for %s:%s: %s",
		      ds->ds_id, ds->ds_address, ds->ds_path, strerror(-ret));
		return ret;
	}

	/* Probe confirmed -- clean up the file immediately. */
	dstore_data_file_remove(ds, ds->ds_root_fh, ds->ds_root_fh_len,
				".root_probe");
	return 0;
}

/* ------------------------------------------------------------------ */
/* Alloc / find                                                        */
/* ------------------------------------------------------------------ */

struct dstore *dstore_alloc(uint32_t id, const char *address, uint16_t port,
			    const char *path, enum reffs_ds_protocol protocol,
			    bool do_mount)
{
	struct dstore *ds;
	struct cds_lfht_node *node;
	unsigned long hash;

	if (!g_dstore_ht)
		return NULL;

	ds = calloc(1, sizeof(*ds));
	if (!ds)
		return NULL;

	ds->ds_id = id;
	ds->ds_protocol = protocol;
	ds->ds_port = port;
	strncpy(ds->ds_address, address, sizeof(ds->ds_address) - 1);
	strncpy(ds->ds_path, path, sizeof(ds->ds_path) - 1);
	pthread_mutex_init(&ds->ds_clnt_mutex, NULL);

	/*
	 * Select the ops vtable: local if the address is the loopback
	 * or matches our own server.  For remote DSes, select based
	 * on the configured protocol.
	 */
	if (!strcmp(address, "127.0.0.1") || !strcmp(address, "::1") ||
	    !strcmp(address, "localhost") || dstore_address_is_local(address)) {
		ds->ds_ops = &dstore_ops_local;
		__atomic_or_fetch(&ds->ds_state, DSTORE_IS_MOUNTED,
				  __ATOMIC_RELEASE);

		/*
		 * Build a local root FH pointing at the DS super_block
		 * (sb_id=2), not the MDS export (sb_id=1).  This keeps
		 * pool files isolated from the client-visible namespace.
		 */
		struct network_file_handle nfh = {
			.nfh_vers = FILEHANDLE_VERSION_CURR,
			.nfh_sb = SUPER_BLOCK_DS_ID,
			.nfh_ino = INODE_ROOT_ID,
		};

		memcpy(ds->ds_root_fh, &nfh, sizeof(nfh));
		ds->ds_root_fh_len = sizeof(nfh);

		strncpy(ds->ds_ip, address, sizeof(ds->ds_ip) - 1);
		TRACE("dstore[%u]: local path %s:%s", id, address, path);
	} else if (protocol == REFFS_DS_PROTO_NFSV4) {
		ds->ds_ops = &dstore_ops_nfsv4;
	} else {
		ds->ds_ops = &dstore_ops_nfsv3;
	}

	cds_lfht_node_init(&ds->ds_node);
	urcu_ref_init(&ds->ds_ref); /* ref 1: hash table */

	/* Connect and mount (skipped for local / unit tests). */
	if (do_mount && protocol == REFFS_DS_PROTO_NFSV4) {
		/* NFSv4 DS: establish session + get root FH */
		if (ds_session_create(ds) < 0)
			LOG("dstore[%u]: NFSv4 session to %s failed "
			    "(continuing)",
			    id, address);
	} else if (do_mount && ds->ds_ops == &dstore_ops_nfsv3) {
		if (mount_get_root_fh(ds) < 0) {
			LOG("dstore[%u]: mount failed for %s:%s (continuing)",
			    id, address, path);
		} else if (dstore_probe_root_access(ds) < 0) {
			/*
			 * Root access denied -- the DS export has root_squash
			 * enabled.  Clear MOUNTED so LAYOUTGET skips this
			 * dstore.  The LOG was already emitted by the probe.
			 */
			__atomic_and_fetch(&ds->ds_state, ~DSTORE_IS_MOUNTED,
					   __ATOMIC_RELEASE);
		}
	}

	/* Insert into hash table. */
	hash = dstore_hash(id);
	ds->ds_state |= DSTORE_IS_HASHED;

	rcu_read_lock();
	node = cds_lfht_add_unique(g_dstore_ht, hash, dstore_match, &id,
				   &ds->ds_node);
	rcu_read_unlock();

	if (caa_unlikely(node != &ds->ds_node)) {
		LOG("dstore[%u]: duplicate id", id);
		ds->ds_state &= ~DSTORE_IS_HASHED;
		if (ds->ds_clnt)
			clnt_destroy(ds->ds_clnt);
		pthread_mutex_destroy(&ds->ds_clnt_mutex);
		free(ds);
		return NULL;
	}

	/* Ref 2: caller. */
	dstore_get(ds);
	trace_dstore(ds, __func__, __LINE__);
	return ds;
}

struct dstore *dstore_find(uint32_t id)
{
	struct dstore *ds = NULL;
	struct dstore *tmp;
	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;
	unsigned long hash = dstore_hash(id);

	if (!g_dstore_ht)
		return NULL;

	rcu_read_lock();
	cds_lfht_lookup(g_dstore_ht, hash, dstore_match, &id, &iter);
	node = cds_lfht_iter_get_node(&iter);
	if (node) {
		tmp = caa_container_of(node, struct dstore, ds_node);
		ds = dstore_get(tmp);
	}
	rcu_read_unlock();

	return ds;
}

/* ------------------------------------------------------------------ */
/* Reconnect                                                           */
/* ------------------------------------------------------------------ */

int dstore_reconnect(struct dstore *ds)
{
	int ret;

	pthread_mutex_lock(&ds->ds_clnt_mutex);

	/*
	 * Another thread may have reconnected while we waited for
	 * the lock.  If the dstore is already connected, we're done.
	 *
	 * Use is_connected (not is_available): drain only blocks new
	 * placements; an already-mounted drained dstore must not be
	 * torn down by a passing reconnect probe.
	 */
	if (dstore_is_connected(ds)) {
		pthread_mutex_unlock(&ds->ds_clnt_mutex);
		return 0;
	}

	__atomic_or_fetch(&ds->ds_state, DSTORE_IS_RECONNECTING,
			  __ATOMIC_RELEASE);

	/* Tear down the old handle. */
	__atomic_and_fetch(&ds->ds_state, ~DSTORE_IS_MOUNTED, __ATOMIC_RELEASE);
	if (ds->ds_clnt) {
		clnt_destroy(ds->ds_clnt);
		ds->ds_clnt = NULL;
	}
	ds->ds_root_fh_len = 0;

	TRACE("dstore[%u]: reconnecting to %s:%s", ds->ds_id, ds->ds_address,
	      ds->ds_path);

	ret = mount_get_root_fh(ds);
	if (ret < 0)
		LOG("dstore[%u]: reconnect failed: %s", ds->ds_id,
		    strerror(-ret));

	__atomic_and_fetch(&ds->ds_state, ~DSTORE_IS_RECONNECTING,
			   __ATOMIC_RELEASE);
	pthread_mutex_unlock(&ds->ds_clnt_mutex);
	return ret;
}

/* ------------------------------------------------------------------ */
/* Bulk operations                                                     */
/* ------------------------------------------------------------------ */

int dstore_load_config(const struct reffs_config *cfg)
{
	unsigned int n = cfg->ndata_servers;

	if (!g_dstore_ht)
		return -EINVAL;

	if (n == 0) {
		LOG("dstore: no data servers configured");
		return -EINVAL;
	}

	for (unsigned int i = 0; i < n; i++) {
		const struct reffs_data_server_config *dsc =
			&cfg->data_servers[i];
		struct dstore *ds = dstore_alloc(
			dsc->id, dsc->address, dsc->port, dsc->path,
			dsc->protocol, true);

		if (!ds) {
			LOG("dstore[%u]: alloc failed for %s:%s", i,
			    dsc->address, dsc->path);
			continue;
		}
		/* Drop the caller ref -- hash table holds the dstore alive. */
		dstore_put(ds);
	}

	TRACE("dstore: loaded %u data server(s)", n);
	return 0;
}

uint32_t dstore_collect_available(struct dstore **out, uint32_t max)
{
	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;
	uint32_t n = 0;

	if (!g_dstore_ht)
		return 0;

	rcu_read_lock();
	cds_lfht_first(g_dstore_ht, &iter);
	while ((node = cds_lfht_iter_get_node(&iter)) != NULL && n < max) {
		struct dstore *ds =
			caa_container_of(node, struct dstore, ds_node);
		if (dstore_is_available(ds)) {
			struct dstore *ref = dstore_get(ds);

			if (ref)
				out[n++] = ref;
		}
		cds_lfht_next(g_dstore_ht, &iter);
	}
	rcu_read_unlock();
	return n;
}

/*
 * dstore_collect_all -- gather refs to every dstore in the global
 * pool, regardless of mount / drain / reconnecting state.  Used by
 * the DSTORE_LIST probe op (mirror-lifecycle Slice B) to surface
 * the full operator dashboard.  Caller drops each ref via
 * dstore_put().
 */
uint32_t dstore_collect_all(struct dstore **out, uint32_t max)
{
	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;
	uint32_t n = 0;

	if (!g_dstore_ht)
		return 0;

	rcu_read_lock();
	cds_lfht_first(g_dstore_ht, &iter);
	while ((node = cds_lfht_iter_get_node(&iter)) != NULL && n < max) {
		struct dstore *ds =
			caa_container_of(node, struct dstore, ds_node);
		struct dstore *ref = dstore_get(ds);

		if (ref)
			out[n++] = ref;
		cds_lfht_next(g_dstore_ht, &iter);
	}
	rcu_read_unlock();
	return n;
}

void dstore_unload_all(void)
{
	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;
	struct dstore *ds;

	if (!g_dstore_ht)
		return;

	rcu_read_lock();
	cds_lfht_first(g_dstore_ht, &iter);
	while ((node = cds_lfht_iter_get_node(&iter)) != NULL) {
		ds = caa_container_of(node, struct dstore, ds_node);
		trace_dstore(ds, __func__, __LINE__);
		cds_lfht_next(g_dstore_ht, &iter);
		if (dstore_unhash(ds))
			dstore_put(ds);
	}
	rcu_read_unlock();
}
