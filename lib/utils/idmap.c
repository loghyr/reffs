/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * Identity mapping cache -- bidirectional name <--> uid/gid.
 *
 * Two pairs of lock-free hash tables:
 *   g_uid_ht / g_uid_rev_ht   (uid-->name and name-->uid)
 *   g_gid_ht / g_gid_rev_ht   (gid-->name and name-->gid)
 *
 * Entries are inserted via idmap_cache_uid/gid (from GSS auth)
 * or lazily on cache miss via libnfsidmap / nsswitch fallback.
 *
 * NOT_NOW_BROWN_COW: persistence across restarts, TTL eviction.
 */

#include <ctype.h>
#include <errno.h>
#include <pwd.h>
#include <grp.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include <urcu.h>
#include <urcu/rculfhash.h>
#include <urcu/ref.h>
#include <xxhash.h>

#ifdef HAVE_LIBNFSIDMAP
#include <nfsidmap.h>
#endif

#include "reffs/idmap.h"
#include "reffs/log.h"
#include "reffs/utf8string.h"

/* ------------------------------------------------------------------ */
/* Module state                                                        */
/* ------------------------------------------------------------------ */

static char g_domain[256];
static struct cds_lfht *g_uid_ht; /* uid --> name */
static struct cds_lfht *g_uid_rev_ht; /* name --> uid */
static struct cds_lfht *g_gid_ht; /* gid --> name */
static struct cds_lfht *g_gid_rev_ht; /* name --> gid */

#ifdef HAVE_LIBNFSIDMAP
/*
 * nfs4_init_name_mapping is idempotent (guards internally), so a
 * separate pthread_once from gss_context.c is harmless.
 */
static pthread_once_t idmap_nfsidmap_once = PTHREAD_ONCE_INIT;

static void idmap_nfsidmap_init(void)
{
	nfs4_init_name_mapping(NULL);
}
#endif

/* ------------------------------------------------------------------ */
/* Entry structure                                                     */
/* ------------------------------------------------------------------ */

struct idmap_entry {
	struct cds_lfht_node ie_node;
	struct rcu_head ie_rcu;
	uint32_t ie_id; /* uid or gid */
	uint32_t ie_name_len; /* strlen of ie_name */
	char ie_name[]; /* "user@DOMAIN\0" */
};

/* ------------------------------------------------------------------ */
/* Hash + match helpers                                                */
/* ------------------------------------------------------------------ */

static unsigned long hash_id(uint32_t id)
{
	return XXH3_64bits(&id, sizeof(id));
}

static unsigned long hash_name(const char *name, size_t len)
{
	/*
	 * Case-insensitive hash: lowercase a temp copy.
	 * Max reasonable name length is bounded by the entry struct.
	 */
	char buf[512];
	size_t n = len < sizeof(buf) ? len : sizeof(buf);

	for (size_t i = 0; i < n; i++)
		buf[i] = (char)tolower((unsigned char)name[i]);
	return XXH3_64bits(buf, n);
}

static int match_id(struct cds_lfht_node *node, const void *key)
{
	struct idmap_entry *e =
		caa_container_of(node, struct idmap_entry, ie_node);
	const uint32_t *id = key;

	return e->ie_id == *id;
}

static int match_name(struct cds_lfht_node *node, const void *key)
{
	struct idmap_entry *e =
		caa_container_of(node, struct idmap_entry, ie_node);
	const char *name = key;

	return strcasecmp(e->ie_name, name) == 0;
}

/* ------------------------------------------------------------------ */
/* Entry lifecycle                                                     */
/* ------------------------------------------------------------------ */

static struct idmap_entry *idmap_entry_alloc(uint32_t id, const char *name)
{
	size_t nlen = strlen(name);
	struct idmap_entry *e = calloc(1, sizeof(*e) + nlen + 1);

	if (!e)
		return NULL;

	cds_lfht_node_init(&e->ie_node);
	e->ie_id = id;
	e->ie_name_len = (uint32_t)nlen;
	memcpy(e->ie_name, name, nlen + 1);
	return e;
}

static void idmap_entry_free_rcu(struct rcu_head *rcu)
{
	struct idmap_entry *e =
		caa_container_of(rcu, struct idmap_entry, ie_rcu);

	free(e);
}

/* ------------------------------------------------------------------ */
/* Internal insert (idempotent -- duplicate is a no-op)                 */
/* ------------------------------------------------------------------ */

static void idmap_insert(struct cds_lfht *id_ht, struct cds_lfht *name_ht,
			 uint32_t id, const char *name)
{
	struct idmap_entry *fwd, *rev;
	struct cds_lfht_node *node;
	unsigned long h;

	fwd = idmap_entry_alloc(id, name);
	rev = idmap_entry_alloc(id, name);
	if (!fwd || !rev) {
		free(fwd);
		free(rev);
		return;
	}

	/* Forward: id --> name */
	h = hash_id(id);
	rcu_read_lock();
	node = cds_lfht_add_unique(id_ht, h, match_id, &id, &fwd->ie_node);
	rcu_read_unlock();
	if (node != &fwd->ie_node)
		free(fwd); /* duplicate -- discard */

	/* Reverse: name --> id */
	h = hash_name(name, strlen(name));
	rcu_read_lock();
	node = cds_lfht_add_unique(name_ht, h, match_name, name, &rev->ie_node);
	rcu_read_unlock();
	if (node != &rev->ie_node)
		free(rev); /* duplicate -- discard */
}

/* ------------------------------------------------------------------ */
/* Internal lookup                                                     */
/* ------------------------------------------------------------------ */

static int idmap_lookup_by_id(struct cds_lfht *ht, uint32_t id, utf8string *dst)
{
	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;
	char name_buf[512];
	bool found = false;

	if (!ht)
		return -ENOENT;

	rcu_read_lock();
	cds_lfht_lookup(ht, hash_id(id), match_id, &id, &iter);
	node = cds_lfht_iter_get_node(&iter);
	if (node) {
		struct idmap_entry *e =
			caa_container_of(node, struct idmap_entry, ie_node);

		/* Copy name to stack buffer under RCU; allocate after. */
		if (e->ie_name_len < sizeof(name_buf)) {
			memcpy(name_buf, e->ie_name, e->ie_name_len + 1);
			found = true;
		}
	}
	rcu_read_unlock();

	if (found)
		return cstr_to_utf8string(dst, name_buf);
	return -ENOENT;
}

static int idmap_lookup_by_name(struct cds_lfht *ht, const char *name,
				uint32_t *id)
{
	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;
	int ret = -ENOENT;

	if (!ht)
		return -ENOENT;

	rcu_read_lock();
	cds_lfht_lookup(ht, hash_name(name, strlen(name)), match_name, name,
			&iter);
	node = cds_lfht_iter_get_node(&iter);
	if (node) {
		struct idmap_entry *e =
			caa_container_of(node, struct idmap_entry, ie_node);

		*id = e->ie_id;
		ret = 0;
	}
	rcu_read_unlock();
	return ret;
}

/* ------------------------------------------------------------------ */
/* External source fallback                                            */
/* ------------------------------------------------------------------ */

/*
 * Try libnfsidmap, then getpwuid_r / getgrgid_r to resolve uid-->name.
 * On success, inserts into cache and fills dst.
 */
static int idmap_resolve_uid(uint32_t uid, utf8string *dst)
{
	char name[256];

#ifdef HAVE_LIBNFSIDMAP
	pthread_once(&idmap_nfsidmap_once, idmap_nfsidmap_init);
	if (nfs4_uid_to_name(uid, g_domain, name, sizeof(name)) == 0 &&
	    name[0] != '\0') {
		idmap_insert(g_uid_ht, g_uid_rev_ht, uid, name);
		return cstr_to_utf8string(dst, name);
	}
#endif

	/* getpwuid_r fallback */
	struct passwd pw, *result = NULL;
	char buf[1024];

	if (getpwuid_r(uid, &pw, buf, sizeof(buf), &result) == 0 && result) {
		snprintf(name, sizeof(name), "%s@%s", pw.pw_name, g_domain);
		idmap_insert(g_uid_ht, g_uid_rev_ht, uid, name);
		return cstr_to_utf8string(dst, name);
	}

	return -ENOENT;
}

static int idmap_resolve_gid(uint32_t gid, utf8string *dst)
{
	char name[256];

#ifdef HAVE_LIBNFSIDMAP
	pthread_once(&idmap_nfsidmap_once, idmap_nfsidmap_init);
	if (nfs4_gid_to_name(gid, g_domain, name, sizeof(name)) == 0 &&
	    name[0] != '\0') {
		idmap_insert(g_gid_ht, g_gid_rev_ht, gid, name);
		return cstr_to_utf8string(dst, name);
	}
#endif

	struct group gr, *result = NULL;
	char buf[1024];

	if (getgrgid_r(gid, &gr, buf, sizeof(buf), &result) == 0 && result) {
		snprintf(name, sizeof(name), "%s@%s", gr.gr_name, g_domain);
		idmap_insert(g_gid_ht, g_gid_rev_ht, gid, name);
		return cstr_to_utf8string(dst, name);
	}

	return -ENOENT;
}

/*
 * Try libnfsidmap, then getpwnam_r to resolve name-->uid.
 * On success, inserts into cache and fills *uid.
 */
static int idmap_resolve_name_to_uid(const char *name, uint32_t *uid)
{
#ifdef HAVE_LIBNFSIDMAP
	uid_t u;

	pthread_once(&idmap_nfsidmap_once, idmap_nfsidmap_init);
	if (nfs4_owner_to_uid((char *)name, &u) == 0) {
		*uid = (uint32_t)u;
		idmap_insert(g_uid_ht, g_uid_rev_ht, *uid, name);
		return 0;
	}
#endif

	/* Strip @domain and try getpwnam_r */
	char local[256];
	const char *at = strchr(name, '@');

	if (at) {
		size_t len = (size_t)(at - name);

		if (len >= sizeof(local))
			return -ENOENT;
		memcpy(local, name, len);
		local[len] = '\0';
	} else {
		strncpy(local, name, sizeof(local) - 1);
		local[sizeof(local) - 1] = '\0';
	}

	struct passwd pw, *result = NULL;
	char buf[1024];

	if (getpwnam_r(local, &pw, buf, sizeof(buf), &result) == 0 && result) {
		*uid = (uint32_t)pw.pw_uid;
		idmap_insert(g_uid_ht, g_uid_rev_ht, *uid, name);
		return 0;
	}

	return -ENOENT;
}

static int idmap_resolve_name_to_gid(const char *name, uint32_t *gid)
{
#ifdef HAVE_LIBNFSIDMAP
	gid_t g;

	pthread_once(&idmap_nfsidmap_once, idmap_nfsidmap_init);
	if (nfs4_group_owner_to_gid((char *)name, &g) == 0) {
		*gid = (uint32_t)g;
		idmap_insert(g_gid_ht, g_gid_rev_ht, *gid, name);
		return 0;
	}
#endif

	char local[256];
	const char *at = strchr(name, '@');

	if (at) {
		size_t len = (size_t)(at - name);

		if (len >= sizeof(local))
			return -ENOENT;
		memcpy(local, name, len);
		local[len] = '\0';
	} else {
		strncpy(local, name, sizeof(local) - 1);
		local[sizeof(local) - 1] = '\0';
	}

	struct group gr, *result = NULL;
	char buf[1024];

	if (getgrnam_r(local, &gr, buf, sizeof(buf), &result) == 0 && result) {
		*gid = (uint32_t)gr.gr_gid;
		idmap_insert(g_gid_ht, g_gid_rev_ht, *gid, name);
		return 0;
	}

	return -ENOENT;
}

/* ------------------------------------------------------------------ */
/* Drain helper                                                        */
/* ------------------------------------------------------------------ */

static void idmap_drain_ht(struct cds_lfht *ht)
{
	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;

	if (!ht)
		return;

	rcu_read_lock();
	cds_lfht_for_each(ht, &iter, node)
	{
		struct idmap_entry *e =
			caa_container_of(node, struct idmap_entry, ie_node);

		cds_lfht_del(ht, node);
		call_rcu(&e->ie_rcu, idmap_entry_free_rcu);
	}
	rcu_read_unlock();
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int idmap_init(const char *domain)
{
	memset(g_domain, 0, sizeof(g_domain));

	if (domain && domain[0] != '\0') {
		strncpy(g_domain, domain, sizeof(g_domain) - 1);
	} else {
		/*
		 * Auto-detect: try system hostname domain part.
		 * E.g., "host.example.com" --> "example.com"
		 */
		char hostname[256];

		if (gethostname(hostname, sizeof(hostname)) == 0) {
			char *dot = strchr(hostname, '.');

			if (dot && dot[1] != '\0')
				strncpy(g_domain, dot + 1,
					sizeof(g_domain) - 1);
		}

		if (g_domain[0] == '\0')
			strncpy(g_domain, "localdomain", sizeof(g_domain) - 1);
	}

	g_uid_ht = cds_lfht_new(16, 16, 0, CDS_LFHT_AUTO_RESIZE, NULL);
	g_uid_rev_ht = cds_lfht_new(16, 16, 0, CDS_LFHT_AUTO_RESIZE, NULL);
	g_gid_ht = cds_lfht_new(16, 16, 0, CDS_LFHT_AUTO_RESIZE, NULL);
	g_gid_rev_ht = cds_lfht_new(16, 16, 0, CDS_LFHT_AUTO_RESIZE, NULL);

	if (!g_uid_ht || !g_uid_rev_ht || !g_gid_ht || !g_gid_rev_ht) {
		idmap_fini();
		return -ENOMEM;
	}

	TRACE("idmap: initialized with domain '%s'", g_domain);
	return 0;
}

void idmap_fini(void)
{
	idmap_drain_ht(g_uid_ht);
	idmap_drain_ht(g_uid_rev_ht);
	idmap_drain_ht(g_gid_ht);
	idmap_drain_ht(g_gid_rev_ht);

	synchronize_rcu();

	if (g_uid_ht) {
		cds_lfht_destroy(g_uid_ht, NULL);
		g_uid_ht = NULL;
	}
	if (g_uid_rev_ht) {
		cds_lfht_destroy(g_uid_rev_ht, NULL);
		g_uid_rev_ht = NULL;
	}
	if (g_gid_ht) {
		cds_lfht_destroy(g_gid_ht, NULL);
		g_gid_ht = NULL;
	}
	if (g_gid_rev_ht) {
		cds_lfht_destroy(g_gid_rev_ht, NULL);
		g_gid_rev_ht = NULL;
	}

	TRACE("idmap: finalized");
}

int idmap_uid_to_name(uid_t uid, utf8string *dst)
{
	int ret;

	/* Fast path: check cache. */
	ret = idmap_lookup_by_id(g_uid_ht, (uint32_t)uid, dst);
	if (ret == 0)
		return 0;

	/* Slow path: external lookup + cache insert. */
	return idmap_resolve_uid((uint32_t)uid, dst);
}

int idmap_gid_to_name(gid_t gid, utf8string *dst)
{
	int ret;

	ret = idmap_lookup_by_id(g_gid_ht, (uint32_t)gid, dst);
	if (ret == 0)
		return 0;

	return idmap_resolve_gid((uint32_t)gid, dst);
}

int idmap_name_to_uid(const utf8string *name, uid_t *uid)
{
	char cname[512];
	uint32_t id;
	int ret;

	if (!name || name->utf8string_len == 0)
		return -EINVAL;

	/* NUL-terminate for C string operations. */
	size_t len = name->utf8string_len;

	if (len >= sizeof(cname))
		return -EINVAL;
	memcpy(cname, name->utf8string_val, len);
	cname[len] = '\0';

	/* Numeric strings bypass the cache entirely. */
	if (utf8string_to_uid(name, uid) == 0)
		return 0;

	/* Cache lookup. */
	ret = idmap_lookup_by_name(g_uid_rev_ht, cname, &id);
	if (ret == 0) {
		*uid = (uid_t)id;
		return 0;
	}

	/* External resolution. */
	ret = idmap_resolve_name_to_uid(cname, &id);
	if (ret == 0) {
		*uid = (uid_t)id;
		return 0;
	}

	return -ENOENT;
}

int idmap_name_to_gid(const utf8string *name, gid_t *gid)
{
	char cname[512];
	uint32_t id;
	int ret;

	if (!name || name->utf8string_len == 0)
		return -EINVAL;

	size_t len = name->utf8string_len;

	if (len >= sizeof(cname))
		return -EINVAL;
	memcpy(cname, name->utf8string_val, len);
	cname[len] = '\0';

	if (utf8string_to_gid(name, gid) == 0)
		return 0;

	ret = idmap_lookup_by_name(g_gid_rev_ht, cname, &id);
	if (ret == 0) {
		*gid = (gid_t)id;
		return 0;
	}

	ret = idmap_resolve_name_to_gid(cname, &id);
	if (ret == 0) {
		*gid = (gid_t)id;
		return 0;
	}

	return -ENOENT;
}

void idmap_cache_uid(uid_t uid, const char *name)
{
	if (!name || !g_uid_ht)
		return;

	idmap_insert(g_uid_ht, g_uid_rev_ht, (uint32_t)uid, name);
	TRACE("idmap: cached uid %u --> %s", (unsigned)uid, name);
}

void idmap_cache_gid(gid_t gid, const char *name)
{
	if (!name || !g_gid_ht)
		return;

	idmap_insert(g_gid_ht, g_gid_rev_ht, (uint32_t)gid, name);
	TRACE("idmap: cached gid %u --> %s", (unsigned)gid, name);
}

/* ------------------------------------------------------------------ */
/* Batch pre-warm for READDIR                                          */
/* ------------------------------------------------------------------ */

#define IDMAP_PREWARM_DEFAULT_MS 3000
#define IDMAP_PREWARM_MAX_THREADS 64

struct prewarm_ctx {
	pthread_mutex_t pw_mutex;
	pthread_cond_t pw_cond;
	_Atomic int pw_pending;
};

struct prewarm_uid_arg {
	uid_t pa_uid;
	struct prewarm_ctx *pa_ctx;
};

struct prewarm_gid_arg {
	gid_t pa_gid;
	struct prewarm_ctx *pa_ctx;
};

static void *prewarm_uid_thread(void *arg)
{
	struct prewarm_uid_arg *pa = arg;
	utf8string dummy = { 0 };

	rcu_register_thread();
	idmap_resolve_uid(pa->pa_uid, &dummy);
	utf8string_free(&dummy);
	rcu_unregister_thread();

	if (atomic_fetch_sub_explicit(&pa->pa_ctx->pw_pending, 1,
				      memory_order_acq_rel) == 1) {
		pthread_mutex_lock(&pa->pa_ctx->pw_mutex);
		pthread_cond_signal(&pa->pa_ctx->pw_cond);
		pthread_mutex_unlock(&pa->pa_ctx->pw_mutex);
	}

	free(pa);
	return NULL;
}

static void *prewarm_gid_thread(void *arg)
{
	struct prewarm_gid_arg *pa = arg;
	utf8string dummy = { 0 };

	rcu_register_thread();
	idmap_resolve_gid(pa->pa_gid, &dummy);
	utf8string_free(&dummy);
	rcu_unregister_thread();

	if (atomic_fetch_sub_explicit(&pa->pa_ctx->pw_pending, 1,
				      memory_order_acq_rel) == 1) {
		pthread_mutex_lock(&pa->pa_ctx->pw_mutex);
		pthread_cond_signal(&pa->pa_ctx->pw_cond);
		pthread_mutex_unlock(&pa->pa_ctx->pw_mutex);
	}

	free(pa);
	return NULL;
}

void idmap_prewarm(const uid_t *uids, int nuids, const gid_t *gids, int ngids,
		   int timeout_ms)
{
	if (timeout_ms <= 0)
		timeout_ms = IDMAP_PREWARM_DEFAULT_MS;

	/*
	 * Filter to only uncached IDs.  The cache lookup is O(1) per ID.
	 */
	uid_t uncached_uids[IDMAP_PREWARM_MAX_THREADS];
	gid_t uncached_gids[IDMAP_PREWARM_MAX_THREADS];
	int nu = 0, ng = 0;

	for (int i = 0; i < nuids && nu < IDMAP_PREWARM_MAX_THREADS; i++) {
		utf8string tmp = { 0 };

		if (idmap_lookup_by_id(g_uid_ht, uids[i], &tmp) != 0)
			uncached_uids[nu++] = uids[i];
		else
			utf8string_free(&tmp);
	}

	for (int i = 0; i < ngids && ng < IDMAP_PREWARM_MAX_THREADS; i++) {
		utf8string tmp = { 0 };

		if (idmap_lookup_by_id(g_gid_ht, gids[i], &tmp) != 0)
			uncached_gids[ng++] = gids[i];
		else
			utf8string_free(&tmp);
	}

	int total = nu + ng;

	if (total == 0)
		return;

	struct prewarm_ctx ctx = {
		.pw_mutex = PTHREAD_MUTEX_INITIALIZER,
		.pw_cond = PTHREAD_COND_INITIALIZER,
	};

	atomic_store_explicit(&ctx.pw_pending, total, memory_order_relaxed);

	/* Spawn resolver threads. */
	for (int i = 0; i < nu; i++) {
		struct prewarm_uid_arg *pa = malloc(sizeof(*pa));

		if (!pa) {
			atomic_fetch_sub_explicit(&ctx.pw_pending, 1,
						  memory_order_relaxed);
			continue;
		}
		pa->pa_uid = uncached_uids[i];
		pa->pa_ctx = &ctx;

		pthread_t t;
		pthread_attr_t attr;

		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
		if (pthread_create(&t, &attr, prewarm_uid_thread, pa)) {
			free(pa);
			atomic_fetch_sub_explicit(&ctx.pw_pending, 1,
						  memory_order_relaxed);
		}
		pthread_attr_destroy(&attr);
	}

	for (int i = 0; i < ng; i++) {
		struct prewarm_gid_arg *pa = malloc(sizeof(*pa));

		if (!pa) {
			atomic_fetch_sub_explicit(&ctx.pw_pending, 1,
						  memory_order_relaxed);
			continue;
		}
		pa->pa_gid = uncached_gids[i];
		pa->pa_ctx = &ctx;

		pthread_t t;
		pthread_attr_t attr;

		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
		if (pthread_create(&t, &attr, prewarm_gid_thread, pa)) {
			free(pa);
			atomic_fetch_sub_explicit(&ctx.pw_pending, 1,
						  memory_order_relaxed);
		}
		pthread_attr_destroy(&attr);
	}

	/* Wait for all threads to complete, with timeout. */
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += timeout_ms / 1000;
	ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
	if (ts.tv_nsec >= 1000000000L) {
		ts.tv_sec++;
		ts.tv_nsec -= 1000000000L;
	}

	pthread_mutex_lock(&ctx.pw_mutex);
	while (atomic_load_explicit(&ctx.pw_pending, memory_order_acquire) > 0)
		if (pthread_cond_timedwait(&ctx.pw_cond, &ctx.pw_mutex, &ts))
			break; /* timeout */
	pthread_mutex_unlock(&ctx.pw_mutex);

	int remaining =
		atomic_load_explicit(&ctx.pw_pending, memory_order_relaxed);

	if (remaining > 0)
		TRACE("idmap_prewarm: %d/%d lookups timed out", remaining,
		      total);

	pthread_mutex_destroy(&ctx.pw_mutex);
	pthread_cond_destroy(&ctx.pw_cond);
}
