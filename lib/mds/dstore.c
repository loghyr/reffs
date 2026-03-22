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
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <rpc/rpc.h>
#include <xxhash.h>

#include "mntv3_xdr.h"
#include "reffs/dstore.h"
#include "reffs/log.h"
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
	int ret;

	state = __atomic_fetch_and(&ds->ds_state, ~DSTORE_IS_HASHED,
				   __ATOMIC_ACQUIRE);
	if (!(state & DSTORE_IS_HASHED))
		return false;

	if (!g_dstore_ht)
		return false;

	trace_dstore(ds, __func__, __LINE__);
	ret = cds_lfht_del(g_dstore_ht, &ds->ds_node);
	assert(!ret);
	(void)ret;
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
	LOG("dstore_fini: draining");
	dstore_unload_all();
	rcu_barrier();
	LOG("dstore_fini: rcu_barrier complete");
	if (g_dstore_ht) {
		cds_lfht_destroy(g_dstore_ht, NULL);
		g_dstore_ht = NULL;
	}
}

/* ------------------------------------------------------------------ */
/* MOUNT client                                                        */
/* ------------------------------------------------------------------ */

static int mount_get_root_fh(struct dstore *ds)
{
	mountres3 res;
	struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
	dirpath path;
	enum clnt_stat rpc_stat;
	int ret = 0;

	ds->ds_clnt =
		clnt_create(ds->ds_address, MOUNT_PROGRAM, MOUNT_V3, "tcp");
	if (!ds->ds_clnt) {
		LOG("dstore[%u]: clnt_create(%s) failed: %s", ds->ds_id,
		    ds->ds_address, clnt_spcreateerror(""));
		return -ECONNREFUSED;
	}

	path = ds->ds_path;
	memset(&res, 0, sizeof(res));

	rpc_stat = clnt_call(ds->ds_clnt, MOUNTPROC3_MNT,
			     (xdrproc_t)xdr_dirpath, (caddr_t)&path,
			     (xdrproc_t)xdr_mountres3, (caddr_t)&res, tv);
	if (rpc_stat != RPC_SUCCESS) {
		LOG("dstore[%u]: MOUNT %s:%s RPC failed: %s", ds->ds_id,
		    ds->ds_address, ds->ds_path,
		    clnt_sperror(ds->ds_clnt, ""));
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
	__atomic_or_fetch(&ds->ds_state, DSTORE_IS_MOUNTED,
			  __ATOMIC_RELEASE);

	LOG("dstore[%u]: mounted %s:%s (FH %u bytes)", ds->ds_id,
	    ds->ds_address, ds->ds_path, ds->ds_root_fh_len);

out_free:
	xdr_free((xdrproc_t)xdr_mountres3, (caddr_t)&res);
out:
	if (ret < 0 && ds->ds_clnt) {
		clnt_destroy(ds->ds_clnt);
		ds->ds_clnt = NULL;
	}
	return ret;
}

/* ------------------------------------------------------------------ */
/* Alloc / find                                                        */
/* ------------------------------------------------------------------ */

struct dstore *dstore_alloc(uint32_t id, const char *address, const char *path,
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
	strncpy(ds->ds_address, address, sizeof(ds->ds_address) - 1);
	strncpy(ds->ds_path, path, sizeof(ds->ds_path) - 1);
	pthread_mutex_init(&ds->ds_clnt_mutex, NULL);

	cds_lfht_node_init(&ds->ds_node);
	urcu_ref_init(&ds->ds_ref); /* ref 1: hash table */

	/* Connect and mount (skipped for unit tests / deferred mount). */
	if (do_mount && mount_get_root_fh(ds) < 0) {
		LOG("dstore[%u]: mount failed for %s:%s (continuing)", id,
		    address, path);
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
	 * the lock.  If the dstore is already available, we're done.
	 */
	if (dstore_is_available(ds)) {
		pthread_mutex_unlock(&ds->ds_clnt_mutex);
		return 0;
	}

	__atomic_or_fetch(&ds->ds_state, DSTORE_IS_RECONNECTING,
			  __ATOMIC_RELEASE);

	/* Tear down the old handle. */
	__atomic_and_fetch(&ds->ds_state, ~DSTORE_IS_MOUNTED,
			   __ATOMIC_RELEASE);
	if (ds->ds_clnt) {
		clnt_destroy(ds->ds_clnt);
		ds->ds_clnt = NULL;
	}
	ds->ds_root_fh_len = 0;

	LOG("dstore[%u]: reconnecting to %s:%s", ds->ds_id, ds->ds_address,
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
		struct dstore *ds =
			dstore_alloc(dsc->id, dsc->address, dsc->path, true);

		if (!ds) {
			LOG("dstore[%u]: alloc failed for %s:%s", i,
			    dsc->address, dsc->path);
			continue;
		}
		/* Drop the caller ref — hash table holds the dstore alive. */
		dstore_put(ds);
	}

	LOG("dstore: loaded %u data server(s)", n);
	return 0;
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
