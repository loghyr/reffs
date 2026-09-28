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
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
#include "reffs/ffv2_prototype.h"
#include "reffs/fixed_inventory.h"
#include "reffs/inode.h"
#include "reffs/layout_segment.h"
#include "reffs/super_block.h"
#include "reffs/log.h"
#include "reffs/posix_shims.h"
#include "reffs/runway.h"
#include "reffs/trace/dstore.h"

/* ------------------------------------------------------------------ */
/* Global hash table                                                   */
/* ------------------------------------------------------------------ */

static struct cds_lfht *g_dstore_ht;
static pthread_mutex_t fixed_status_mutex = PTHREAD_MUTEX_INITIALIZER;

int dstore_fixed_status_write(struct dstore *ds)
{
	static _Atomic uint64_t sequence;
	char path[PATH_MAX] = { 0 }, temporary[PATH_MAX] = { 0 }, data[4096];
	size_t len = 0, written = 0;
	uint64_t admitted[DSTORE_ORDINARY_OP_COUNT];
	uint64_t refused[DSTORE_ORDINARY_OP_COUNT];
	uint64_t inventory_generation = 0, metadata_epoch = 0;
	uint64_t provider_generation = 0, in_flight;
	uint32_t inventory_state = 0, metadata_state = 0, ordinary_state;
	int fd = -1, dirfd = -1, ret = 0;

	if (!ds || !ds->ds_prototype_config.fixed_inventory ||
	    !ds->ds_state_dir[0])
		return 0;
	pthread_mutex_lock(&fixed_status_mutex);
	pthread_mutex_lock(&ds->ds_ordinary_gate.mutex);
	ordinary_state = ds->ds_ordinary_gate.state;
	in_flight = ds->ds_ordinary_gate.in_flight;
	pthread_mutex_unlock(&ds->ds_ordinary_gate.mutex);
	for (uint32_t i = 0; i < DSTORE_ORDINARY_OP_COUNT; i++) {
		admitted[i] =
			atomic_load_explicit(&ds->ds_ordinary_gate.admitted[i],
					     memory_order_relaxed);
		refused[i] = atomic_load_explicit(
			&ds->ds_ordinary_gate.refused[i], memory_order_relaxed);
	}
	ffv2_fixed_inventory_lock();
	if (ds->ds_fixed_inventory) {
		inventory_state = ds->ds_fixed_inventory->state;
		metadata_state = ds->ds_fixed_inventory->metadata_state;
		inventory_generation = ds->ds_fixed_inventory->generation;
		metadata_epoch = ds->ds_fixed_inventory->metadata_epoch;
	}
	ffv2_fixed_inventory_unlock();
	const struct ffv2_prototype_snapshot *snapshot =
		ffv2_prototype_snapshot_borrow(ds);

	if (snapshot)
		provider_generation = snapshot->generation;
	ffv2_prototype_snapshot_release(ds);
#define APPEND(...)                                                            \
	do {                                                                   \
		int amount =                                                   \
			snprintf(data + len, sizeof(data) - len, __VA_ARGS__); \
		if (amount < 0 || (size_t)amount >= sizeof(data) - len) {      \
			ret = -EOVERFLOW;                                      \
			goto out;                                              \
		}                                                              \
		len += (size_t)amount;                                         \
	} while (0)
	APPEND("{\"version\":1,\"dstore_id\":%u,\"inventory_state\":%u,"
	       "\"metadata_state\":%u,\"inventory_generation\":%" PRIu64
	       ",\"metadata_epoch\":%" PRIu64
	       ",\"provider_generation\":%" PRIu64
	       ",\"ordinary_state\":%u,\"in_flight\":%" PRIu64
	       ",\"admitted\":[",
	       ds->ds_id, inventory_state, metadata_state, inventory_generation,
	       metadata_epoch, provider_generation, ordinary_state, in_flight);
	for (uint32_t i = 0; i < DSTORE_ORDINARY_OP_COUNT; i++)
		APPEND("%s%" PRIu64, i ? "," : "", admitted[i]);
	APPEND("],\"refused\":[");
	for (uint32_t i = 0; i < DSTORE_ORDINARY_OP_COUNT; i++)
		APPEND("%s%" PRIu64, i ? "," : "", refused[i]);
	APPEND("]}\n");
#undef APPEND
	if (snprintf(path, sizeof(path), "%s/ffv2-fixed-status-%u.json",
		     ds->ds_state_dir, ds->ds_id) >= (int)sizeof(path) ||
	    snprintf(temporary, sizeof(temporary), "%s.tmp.%u.%" PRIu64, path,
		     (unsigned)getpid(),
		     atomic_fetch_add_explicit(&sequence, 1,
					       memory_order_relaxed)) >=
		    (int)sizeof(temporary)) {
		ret = -ENAMETOOLONG;
		goto out;
	}
	fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (fd < 0) {
		ret = -errno;
		goto out;
	}
	while (written < len) {
		ssize_t amount = write(fd, data + written, len - written);

		if (amount <= 0) {
			ret = amount < 0 ? -errno : -EIO;
			goto out;
		}
		written += (size_t)amount;
	}
	if (reffs_fdatasync(fd)) {
		ret = -errno;
		goto out;
	}
	if (close(fd)) {
		fd = -1;
		ret = -errno;
		goto out;
	}
	fd = -1;
	if (rename(temporary, path)) {
		ret = -errno;
		goto out;
	}
	dirfd = open(ds->ds_state_dir, O_RDONLY | O_DIRECTORY);
	if (dirfd < 0 || fsync(dirfd))
		ret = -errno;
out:
	if (dirfd >= 0)
		close(dirfd);
	if (fd >= 0)
		close(fd);
	if (ret && temporary[0])
		unlink(temporary);
	pthread_mutex_unlock(&fixed_status_mutex);
	return ret;
}

int dstore_ordinary_get(struct dstore *ds, enum dstore_ordinary_op op)
{
	struct ds_ordinary_gate *gate;
	int ret = 0;

	if (!ds || op >= DSTORE_ORDINARY_OP_COUNT)
		return -EINVAL;
	gate = &ds->ds_ordinary_gate;
	pthread_mutex_lock(&gate->mutex);
	if (gate->state != DSTORE_ORDINARY_PREFLIGHT_OPEN) {
		atomic_fetch_add_explicit(&gate->refused[op], 1,
					  memory_order_relaxed);
		ret = -ESHUTDOWN;
	} else {
		gate->in_flight++;
		atomic_fetch_add_explicit(&gate->admitted[op], 1,
					  memory_order_relaxed);
	}
	pthread_mutex_unlock(&gate->mutex);
	return ret;
}

void dstore_ordinary_put(struct dstore *ds)
{
	struct ds_ordinary_gate *gate = &ds->ds_ordinary_gate;

	pthread_mutex_lock(&gate->mutex);
	assert(gate->in_flight > 0);
	gate->in_flight--;
	if (!gate->in_flight)
		pthread_cond_broadcast(&gate->condition);
	pthread_mutex_unlock(&gate->mutex);
}

int dstore_ordinary_close(struct dstore *ds)
{
	struct ds_ordinary_gate *gate;

	if (!ds)
		return -EINVAL;
	gate = &ds->ds_ordinary_gate;
	pthread_mutex_lock(&gate->mutex);
	if (gate->state != DSTORE_ORDINARY_PREFLIGHT_OPEN) {
		pthread_mutex_unlock(&gate->mutex);
		return -EINVAL;
	}
	gate->state = DSTORE_ORDINARY_REGISTRATION_PENDING;
	while (gate->in_flight)
		pthread_cond_wait(&gate->condition, &gate->mutex);
	pthread_mutex_unlock(&gate->mutex);
	(void)dstore_fixed_status_write(ds);
	return 0;
}

void dstore_ordinary_activate(struct dstore *ds)
{
	pthread_mutex_lock(&ds->ds_ordinary_gate.mutex);
	assert(ds->ds_ordinary_gate.state ==
	       DSTORE_ORDINARY_REGISTRATION_PENDING);
	assert(!ds->ds_ordinary_gate.in_flight);
	ds->ds_ordinary_gate.state = DSTORE_ORDINARY_SERVICE_ACTIVE;
	pthread_mutex_unlock(&ds->ds_ordinary_gate.mutex);
	(void)dstore_fixed_status_write(ds);
}

void dstore_ordinary_retire(struct dstore *ds)
{
	pthread_mutex_lock(&ds->ds_ordinary_gate.mutex);
	if (ds->ds_ordinary_gate.state != DSTORE_ORDINARY_PREFLIGHT_OPEN)
		ds->ds_ordinary_gate.state = DSTORE_ORDINARY_RETIRING;
	pthread_mutex_unlock(&ds->ds_ordinary_gate.mutex);
	(void)dstore_fixed_status_write(ds);
}

int dstore_ordinary_reopen(struct dstore *ds)
{
	int ret = 0;

	pthread_mutex_lock(&ds->ds_ordinary_gate.mutex);
	if (ds->ds_ordinary_gate.state != DSTORE_ORDINARY_RETIRING ||
	    ds->ds_ordinary_gate.in_flight)
		ret = -EBUSY;
	else
		ds->ds_ordinary_gate.state = DSTORE_ORDINARY_PREFLIGHT_OPEN;
	pthread_mutex_unlock(&ds->ds_ordinary_gate.mutex);
	(void)dstore_fixed_status_write(ds);
	return ret;
}

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
	struct runway *rw =
		atomic_load_explicit(&ds->ds_runway, memory_order_acquire);

	if (rw)
		runway_destroy(rw);
	if (ds->ds_clnt)
		clnt_destroy(ds->ds_clnt);
	pthread_mutex_destroy(&ds->ds_clnt_mutex);
	/*
	 * Keep-alive slice: destroy the rwlock that protected
	 * ds_v4_session.  Lifetime guarantee: by the time this RCU
	 * callback runs, refcount has hit zero AND a grace period has
	 * elapsed; all rdlock holders (borrowers) and wrlock holders
	 * (reconnect / shutdown) had to drop their dstore ref before
	 * the refcount could reach zero, so no thread can hold the
	 * rwlock at this point regardless of whether ds_v4_session
	 * itself is still installed.  ds_v4_session is now reliably
	 * NULL'd by dstore_unload_all (calls ds_session_destroy on the
	 * collect_all snapshot before dropping the hash ref), so the
	 * pre-existing shutdown leak is closed; only the rwlock
	 * teardown remains here.
	 */
	pthread_rwlock_destroy(&ds->ds_v4_session_rwlock);
	pthread_mutex_destroy(&ds->ds_prototype_mutex);
	pthread_rwlock_destroy(&ds->ds_prototype_lock);
	pthread_cond_destroy(&ds->ds_ordinary_gate.condition);
	pthread_mutex_destroy(&ds->ds_ordinary_gate.mutex);
	free(ds->ds_fixed_inventory);
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

int dstore_startup_result(const struct reffs_config *cfg, int load_result)
{
	if (load_result >= 0 || !cfg)
		return load_result;
	for (unsigned int i = 0; i < cfg->ndata_servers; i++) {
		const struct reffs_prototype_registration_config *prototype =
			&cfg->data_servers[i].prototype_registration;

		if (prototype->enabled && prototype->fixed_inventory)
			return load_result;
	}
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

void resolve_ds_ip(struct dstore *ds)
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
		/*
		 * mountd may live on a different port than nfsd (e.g. a
		 * knfsd DS).  Use the explicit mount_port when set; else
		 * fall back to the NFS port (a reffs DS serves both on one
		 * port).
		 */
		uint16_t mport = ds->ds_mount_port > 0 ? ds->ds_mount_port :
							 ds->ds_port;

		sin = *(struct sockaddr_in *)res->ai_addr;
		freeaddrinfo(res);
		sin.sin_port = htons(mport);

		int fd = RPC_ANYSOCK;

		mnt_clnt = clnttcp_create(&sin, MOUNT_PROGRAM, MOUNT_V3, &fd, 0,
					  0);
		if (!mnt_clnt) {
			LOG("dstore[%u]: clnttcp_create(%s:%u) MOUNT failed",
			    ds->ds_id, ds->ds_address, mport);
			return -ECONNREFUSED;
		}
	} else {
		mnt_clnt = clnt_create(ds->ds_address, MOUNT_PROGRAM, MOUNT_V3,
				       "tcp");
		if (!mnt_clnt) {
			LOG("dstore[%u]: clnt_create(%s) MOUNT failed: %s",
			    ds->ds_id, ds->ds_address, clnt_spcreateerror(""));
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

		ds->ds_clnt =
			clnttcp_create(&sin, NFS3_PROGRAM, NFS_V3, &fd, 0, 0);
	} else {
		ds->ds_clnt = clnt_create(ds->ds_address, NFS3_PROGRAM, NFS_V3,
					  "tcp");
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

int dstore_mount_preflight(struct dstore *ds)
{
	int ret;

	if (!ds)
		return -EINVAL;
	if (ds->ds_ops == &dstore_ops_local)
		return 0;
	if (ds->ds_protocol == REFFS_DS_PROTO_NFSV4)
		return ds_session_create(ds);
	ret = mount_get_root_fh(ds);
	if (ret)
		return ret;
	ret = dstore_probe_root_access(ds);
	if (ret)
		__atomic_and_fetch(&ds->ds_state, ~DSTORE_IS_MOUNTED,
				   __ATOMIC_RELEASE);
	return ret;
}

int dstore_fixed_inventory_preflight(struct dstore *ds)
{
	struct ffv2_fixed_inventory_record *record;
	struct ffv2_fixed_inventory_identity identity;
	const struct reffs_prototype_registration_config *config;
	int ret;

	if (!ds)
		return -EINVAL;
	config = &ds->ds_prototype_config;
	if (!config->enabled || !config->fixed_inventory)
		return 0;
	if (!ds->ds_state_dir[0])
		return -EINVAL;
	ret = ffv2_fixed_inventory_identity_init(
		&identity, ds->ds_id, ds->ds_address, ds->ds_path, config);
	if (ret)
		return ret;
	for (uint32_t i = 0; i < config->object_count; i++) {
		const struct reffs_prototype_object_config *object =
			&config->objects[i];
		struct layout_data_file attributes = { 0 };
		uint8_t handle[REFFS_CONFIG_MAX_PROTOTYPE_FH];
		uint32_t handle_len = 0;

		ret = dstore_data_file_create(ds, ds->ds_root_fh,
					      ds->ds_root_fh_len, object->name,
					      handle, &handle_len);
		if (ret)
			return ret;
		if (handle_len != object->ordinary_handle_len ||
		    memcmp(handle, object->ordinary_handle, handle_len))
			return -ESTALE;
		ret = dstore_data_file_getattr(ds, handle, handle_len,
					       &attributes);
		if (ret)
			return ret;
		if (attributes.ldf_size != 0)
			return -EBUSY;
	}
	record = calloc(1, sizeof(*record));
	if (!record)
		return -ENOMEM;
	ret = ffv2_fixed_inventory_load(ds->ds_state_dir, ds->ds_id, record);
	if (ret == -ENOENT) {
		record->identity = identity;
		ret = ffv2_fixed_inventory_save(ds->ds_state_dir, record);
	} else if (!ret && !ffv2_fixed_inventory_identity_equal(
				   &record->identity, &identity)) {
		ret = -ESTALE;
	}
	if (ret) {
		free(record);
		return ret;
	}
	free(ds->ds_fixed_inventory);
	ds->ds_fixed_inventory = record;
	return 0;
}

static bool
fixed_recovery_segment_equal(const struct dstore *ds,
			     const struct layout_segment *segment,
			     const struct ffv2_fixed_inventory_record *record)
{
	const struct ffv2_fixed_inventory_identity *identity =
		&record->identity;

	if (!segment || segment->ls_offset != record->segment_offset ||
	    segment->ls_length != record->segment_length ||
	    segment->ls_stripe_unit != record->segment_stripe_unit ||
	    segment->ls_k != identity->data_count ||
	    segment->ls_m != identity->parity_count ||
	    segment->ls_nfiles != identity->object_count ||
	    segment->ls_layout_type != record->segment_layout_type ||
	    segment->ls_checksum_algorithm !=
		    record->segment_checksum_algorithm)
		return false;
	for (uint32_t i = 0; i < identity->object_count; i++) {
		const struct layout_data_file *file = &segment->ls_files[i];
		const struct reffs_prototype_object_config *object =
			&identity->objects[i];

		if (file->ldf_dstore_id != ds->ds_id ||
		    file->ldf_fh_len != object->ordinary_handle_len ||
		    memcmp(file->ldf_fh, object->ordinary_handle,
			   object->ordinary_handle_len))
			return false;
	}
	return true;
}

static int fixed_recovery_transition(struct dstore *ds,
				     enum ffv2_fixed_inventory_state next)
{
	struct ffv2_fixed_inventory_record *record = ds->ds_fixed_inventory;
	int ret = ffv2_fixed_inventory_transition(
		record, next, record->owner_sb_uuid, record->owner_ino);

	if (!ret)
		ret = ffv2_fixed_inventory_save(ds->ds_state_dir, record);
	return ret;
}

int dstore_fixed_inventory_recover(struct dstore *ds)
{
	struct ffv2_fixed_inventory_record *record;
	struct super_block *owner_sb = NULL;
	struct inode *owner = NULL;
	int ret = 0;

	if (!ds || !ds->ds_prototype_config.fixed_inventory)
		return 0;
	record = ds->ds_fixed_inventory;
	if (!record)
		return -EINVAL;
	ffv2_fixed_inventory_lock();
	if (record->state == FFV2_INVENTORY_FREE)
		goto out;
	if (record->state == FFV2_INVENTORY_RETIRED ||
	    record->state == FFV2_INVENTORY_FENCED) {
		ret = -ESTALE;
		goto out;
	}

	rcu_read_lock();
	struct super_block *candidate;

	cds_list_for_each_entry_rcu(candidate, super_block_list_head(),
				    sb_link) {
		if (!uuid_compare(candidate->sb_uuid, record->owner_sb_uuid)) {
			owner_sb = super_block_get(candidate);
			break;
		}
	}
	rcu_read_unlock();
	if (owner_sb)
		owner = inode_find(owner_sb, record->owner_ino);
	if (!owner) {
		ret = fixed_recovery_transition(ds, FFV2_INVENTORY_RETIRED);
		if (!ret)
			ret = -ENOENT;
		goto out;
	}

	pthread_mutex_lock(&owner->i_layout_sync_mutex);
	pthread_mutex_lock(&owner->i_attr_mutex);
	if (!owner->i_layout_segments || !owner->i_layout_segments->lss_count) {
		if (record->state == FFV2_INVENTORY_ASSIGNED) {
			ret = -ESTALE;
			goto out_inode;
		}
		struct layout_data_file *files;
		struct layout_segment segment = {
			.ls_offset = record->segment_offset,
			.ls_length = record->segment_length,
			.ls_stripe_unit = record->segment_stripe_unit,
			.ls_k = record->identity.data_count,
			.ls_m = record->identity.parity_count,
			.ls_nfiles = record->identity.object_count,
			.ls_layout_type = record->segment_layout_type,
			.ls_checksum_algorithm =
				record->segment_checksum_algorithm,
		};

		files = calloc(segment.ls_nfiles, sizeof(*files));
		if (!files) {
			ret = -ENOMEM;
			goto out_inode;
		}
		for (uint32_t i = 0; i < segment.ls_nfiles; i++) {
			files[i].ldf_dstore_id = ds->ds_id;
			files[i].ldf_fh_len =
				record->identity.objects[i].ordinary_handle_len;
			memcpy(files[i].ldf_fh,
			       record->identity.objects[i].ordinary_handle,
			       files[i].ldf_fh_len);
		}
		segment.ls_files = files;
		if (!owner->i_layout_segments)
			owner->i_layout_segments = layout_segments_alloc();
		if (!owner->i_layout_segments) {
			free(files);
			ret = -ENOMEM;
			goto out_inode;
		}
		ret = layout_segments_add(owner->i_layout_segments, &segment);
		if (ret) {
			free(files);
			goto out_inode;
		}
		inode_sync_to_disk(owner);
	} else if (owner->i_layout_segments->lss_count != 1 ||
		   !fixed_recovery_segment_equal(
			   ds, &owner->i_layout_segments->lss_segs[0],
			   record)) {
		ret = -ESTALE;
	}
	if (!ret && record->state == FFV2_INVENTORY_ASSIGNED) {
		if (record->metadata_state != FFV2_METADATA_CLEAN) {
			ret = -ESTALE;
		} else {
			owner->i_layout_barrier.active = true;
			owner->i_layout_barrier.dirty_epoch =
				record->metadata_epoch;
		}
	}
out_inode:
	pthread_mutex_unlock(&owner->i_attr_mutex);
	pthread_mutex_unlock(&owner->i_layout_sync_mutex);
	if (ret == -ESTALE) {
		int fence_ret =
			fixed_recovery_transition(ds, FFV2_INVENTORY_FENCED);

		if (fence_ret)
			ret = fence_ret;
	} else if (!ret && record->state == FFV2_INVENTORY_CLAIMED) {
		ret = fixed_recovery_transition(ds, FFV2_INVENTORY_ASSIGNED);
	}
out:
	if (owner)
		inode_active_put(owner);
	if (owner_sb)
		super_block_put(owner_sb);
	ffv2_fixed_inventory_unlock();
	(void)dstore_fixed_status_write(ds);
	return ret;
}

/* ------------------------------------------------------------------ */
/* Alloc / find                                                        */
/* ------------------------------------------------------------------ */

struct dstore *dstore_alloc(uint32_t id, const char *address, uint16_t port,
			    uint16_t mount_port, const char *path,
			    enum reffs_ds_protocol protocol, bool do_mount,
			    bool tight_coupling)
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
	ds->ds_mount_port = mount_port;
	strncpy(ds->ds_address, address, sizeof(ds->ds_address) - 1);
	strncpy(ds->ds_path, path, sizeof(ds->ds_path) - 1);
	pthread_mutex_init(&ds->ds_clnt_mutex, NULL);
	/*
	 * Keep-alive slice: rwlock protects ds_v4_session.  Atomic backoff
	 * fields default to 0 from calloc (no current backoff, next attempt
	 * is "now"); explicit init for clarity and future-proofing if calloc
	 * semantics change.  See struct dstore in lib/include/reffs/dstore.h.
	 */
	pthread_rwlock_init(&ds->ds_v4_session_rwlock, NULL);
	pthread_mutex_init(&ds->ds_prototype_mutex, NULL);
	pthread_rwlock_init(&ds->ds_prototype_lock, NULL);
	pthread_mutex_init(&ds->ds_ordinary_gate.mutex, NULL);
	pthread_cond_init(&ds->ds_ordinary_gate.condition, NULL);
	ds->ds_ordinary_gate.state = DSTORE_ORDINARY_PREFLIGHT_OPEN;
	atomic_store_explicit(&ds->ds_reconnect_backoff_sec, 0,
			      memory_order_relaxed);
	atomic_store_explicit(&ds->ds_reconnect_next_attempt_ns, 0,
			      memory_order_relaxed);

	/*
	 * Opt-in tight coupling for NFSv3
	 * dstores known to be reffsd.  Set BEFORE the hash-table
	 * publish so readers never see ds_tight_coupled=false on
	 * a tight-coupled dstore.  Local dstores override below
	 * (combined mode is structurally tight); NFSv4 dstores
	 * rely on probe_tight_coupling at session-setup time.
	 */
	if (tight_coupling)
		ds->ds_tight_coupled = true;

	/*
	 * Select the ops vtable: local if the address is the loopback
	 * or matches our own server.  For remote DSes, select based
	 * on the configured protocol.
	 *
	 * An explicit NFS port (port > 0) means "this is a real wire DS
	 * at this address:port" -- force the remote vtable and skip the
	 * local-address heuristic, so a DS on a link-local or same-host
	 * address (e.g. a knfsd instance) is contacted over the wire
	 * rather than served from the local combined-mode VFS.
	 */
	bool force_remote = port > 0;

	if (!force_remote &&
	    (!strcmp(address, "127.0.0.1") || !strcmp(address, "::1") ||
	     !strcmp(address, "localhost") ||
	     dstore_address_is_local(address))) {
		ds->ds_ops = &dstore_ops_local;
		ds->ds_tight_coupled = true; /* combined mode is always tight */
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
	if (do_mount && dstore_mount_preflight(ds) < 0)
		LOG("dstore[%u]: mount failed for %s:%s (continuing)", id,
		    address, path);

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
		pthread_rwlock_destroy(&ds->ds_v4_session_rwlock);
		pthread_mutex_destroy(&ds->ds_prototype_mutex);
		pthread_rwlock_destroy(&ds->ds_prototype_lock);
		pthread_cond_destroy(&ds->ds_ordinary_gate.condition);
		pthread_mutex_destroy(&ds->ds_ordinary_gate.mutex);
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
	int gate_ret;

	gate_ret = dstore_ordinary_get(ds, DSTORE_ORDINARY_PROBE);
	if (gate_ret)
		return gate_ret;

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
		dstore_ordinary_put(ds);
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
	else if (ds->ds_prototype_config.enabled &&
		 !ds->ds_prototype_config.fixed_inventory &&
		 ffv2_prototype_register_dstore(ds) < 0)
		LOG("dstore[%u]: prototype rebind failed; layouts fail closed",
		    ds->ds_id);

	__atomic_and_fetch(&ds->ds_state, ~DSTORE_IS_RECONNECTING,
			   __ATOMIC_RELEASE);
	pthread_mutex_unlock(&ds->ds_clnt_mutex);
	dstore_ordinary_put(ds);
	return ret;
}

/* ------------------------------------------------------------------ */
/* Bulk operations                                                     */
/* ------------------------------------------------------------------ */

int dstore_load_config(const struct reffs_config *cfg)
{
	unsigned int n = cfg->ndata_servers;
	bool fixed_prototype = false;
	int ret;

	if (!g_dstore_ht)
		return -EINVAL;

	if (n == 0) {
		LOG("dstore: no data servers configured");
		return -EINVAL;
	}
	for (unsigned int i = 0; i < n; i++) {
		if (cfg->data_servers[i].prototype_registration.enabled &&
		    cfg->data_servers[i].prototype_registration.fixed_inventory) {
			fixed_prototype = true;
			break;
		}
	}
	if (fixed_prototype) {
		ret = ffv2_prototype_disable();
		if (ret && ret != -EOPNOTSUPP)
			return ret;
	}

	for (unsigned int i = 0; i < n; i++) {
		const struct reffs_data_server_config *dsc =
			&cfg->data_servers[i];
		struct dstore *ds = dstore_alloc(dsc->id, dsc->address,
						 dsc->port, dsc->mount_port,
						 dsc->path, dsc->protocol,
						 false, dsc->tight_coupling);

		if (!ds) {
			LOG("dstore[%u]: alloc failed for %s:%s", i,
			    dsc->address, dsc->path);
			continue;
		}

		/*
		 * Remember the pool size here so the renewal thread can
		 * build a runway for a data server that was not
		 * reachable at startup; it has no access to the config.
		 */
		ds->ds_runway_size = cfg->runway_size;
		ds->ds_prototype_config = dsc->prototype_registration;
		if (snprintf(ds->ds_state_dir, sizeof(ds->ds_state_dir), "%s",
			     cfg->state_file) >=
		    (int)sizeof(ds->ds_state_dir)) {
			ret = -ENAMETOOLONG;
			goto fixed_failure;
		}
		ret = dstore_mount_preflight(ds);
		if (ret) {
			LOG("dstore[%u]: mount failed for %s:%s", ds->ds_id,
			    dsc->address, dsc->path);
			if (ds->ds_prototype_config.fixed_inventory)
				goto fixed_failure;
		}
		if (ds->ds_prototype_config.fixed_inventory) {
			ret = dstore_fixed_inventory_preflight(ds);
			if (ret)
				goto fixed_failure;
			ret = dstore_fixed_inventory_recover(ds);
			if (ret)
				goto fixed_failure;
			ret = dstore_ordinary_close(ds);
			if (ret)
				goto fixed_failure;
			ret = ffv2_prototype_register_dstore(ds);
			if (ret)
				goto fixed_failure;
			dstore_ordinary_activate(ds);
		} else if (ds->ds_prototype_config.enabled &&
			   ffv2_prototype_register_dstore(ds) < 0) {
			LOG("dstore[%u]: prototype registration failed; layouts fail closed",
			    ds->ds_id);
		}
		/* Drop the caller ref -- hash table holds the dstore alive. */
		dstore_put(ds);
		continue;

fixed_failure:
		LOG("dstore[%u]: fixed prototype startup failed: %s", ds->ds_id,
		    strerror(-ret));
		dstore_put(ds);
		dstore_unload_all();
		return ret;
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
 * the DSTORE_LIST probe op to surface
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
	struct dstore *snapshot[DSTORE_REVOKE_MAX];
	uint32_t n;
	uint32_t i;

	if (!g_dstore_ht)
		return;

	/*
	 * Snapshot via dstore_collect_all so the destroy-side work runs
	 * OUTSIDE rcu_read_lock.  ds_session_destroy issues blocking
	 * RPCs (DESTROY_SESSION + DESTROY_CLIENTID via
	 * mds_session_destroy) and rcu-violations.md rule 1 forbids
	 * blocking inside a read-side critical section.  The snapshot
	 * also lets us tear down sessions before dropping the hash ref
	 * so the v4 session, the rwlock that protects it, and the
	 * dstore itself all die in a single deterministic sequence.
	 * dstore_collect_all bumps each ref; we drop it explicitly
	 * after the unhash.
	 *
	 * DSTORE_REVOKE_MAX bound: this is a compile-time alias for
	 * REFFS_CONFIG_MAX_DSTORES (currently 1024) -- the same bound the
	 * renewal worker (ds_renewal.c) uses for an in-flight scan.
	 * A deployment that hits this bound is at the configured
	 * per-server dstore ceiling and has other things to worry about.
	 */
	n = dstore_collect_all(snapshot, DSTORE_REVOKE_MAX);

	for (i = 0; i < n; i++) {
		struct dstore *ds = snapshot[i];

		trace_dstore(ds, __func__, __LINE__);
		/*
		 * Drop the v4 session BEFORE dropping the hash ref.
		 * ds_session_destroy is idempotent on a NULL session
		 * (dstore_session_replace with old_session=NULL skips
		 * the destroy block) so dstores that never created a
		 * session (NFSv3, local, do_mount=false) take a fast
		 * path here; the v4 session is now reliably torn down at
		 * shutdown.
		 */
		ds_session_destroy(ds);
		if (ds->ds_prototype_config.fixed_inventory) {
			dstore_ordinary_retire(ds);
			int retire_ret = ffv2_prototype_retire_dstore(ds);

			if (retire_ret) {
				LOG("dstore[%u]: prototype retirement failed: %s",
				    ds->ds_id, strerror(-retire_ret));
			} else {
				ffv2_fixed_inventory_lock();
				struct ffv2_fixed_inventory_record *record =
					ds->ds_fixed_inventory;

				if (record &&
				    record->state == FFV2_INVENTORY_ASSIGNED &&
				    record->metadata_state !=
					    FFV2_METADATA_CLEAN &&
				    !ffv2_fixed_inventory_transition(
					    record, FFV2_INVENTORY_FENCED,
					    record->owner_sb_uuid,
					    record->owner_ino) &&
				    ffv2_fixed_inventory_save(ds->ds_state_dir,
							      record))
					LOG("dstore[%u]: failed to persist fenced "
					    "metadata epoch",
					    ds->ds_id);
				ffv2_fixed_inventory_unlock();
			}
			(void)dstore_fixed_status_write(ds);
		} else {
			ffv2_prototype_unregister_dstore(ds);
		}

		if (dstore_unhash(ds))
			dstore_put(ds); /* drop hash ref */
		dstore_put(ds); /* drop collect_all ref */
	}
}
