/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <check.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "reffs/settings.h"

#include "ps_state.h"

static void setup(void)
{
	ps_state_init();
}

static void teardown(void)
{
	ps_state_fini();
}

static struct reffs_proxy_mds_config make_cfg(uint32_t id, const char *addr)
{
	struct reffs_proxy_mds_config cfg;

	memset(&cfg, 0, sizeof(cfg));
	cfg.id = id;
	cfg.port = 4098;
	cfg.mds_port = 2049;
	cfg.mds_probe = 20490;
	if (addr)
		strncpy(cfg.address, addr, sizeof(cfg.address) - 1);
	return cfg;
}

/*
 * Empty registry returns NULL for every lookup, including id 0
 * (native listener, never present here) and unregistered ids.
 */
START_TEST(test_find_empty_returns_null)
{
	ck_assert_ptr_null(ps_state_find(0));
	ck_assert_ptr_null(ps_state_find(1));
	ck_assert_ptr_null(ps_state_find(999));
}
END_TEST

/*
 * Register one entry, verify every field round-trips and the
 * returned pointer addresses the registry slot (stable for the
 * registry's lifetime).
 */
START_TEST(test_register_and_find)
{
	struct reffs_proxy_mds_config c = make_cfg(1, "10.0.0.5");

	ck_assert_int_eq(ps_state_register(&c), 0);

	const struct ps_listener_state *pls = ps_state_find(1);

	ck_assert_ptr_nonnull(pls);
	ck_assert_uint_eq(pls->pls_listener_id, 1);
	ck_assert_str_eq(pls->pls_upstream, "10.0.0.5");
	ck_assert_uint_eq(pls->pls_upstream_port, 2049);
	ck_assert_uint_eq(pls->pls_upstream_probe, 20490);

	/* Second call returns the same pointer. */
	ck_assert_ptr_eq(pls, ps_state_find(1));
}
END_TEST

/*
 * Empty address still registers.  A proxy_mds entry without an
 * upstream address is legal config -- the listener is up, no
 * discovery / forwarding is attempted.  Distinguished via
 * pls_upstream[0] == '\0'.
 */
START_TEST(test_register_empty_address)
{
	struct reffs_proxy_mds_config c = make_cfg(2, "");

	ck_assert_int_eq(ps_state_register(&c), 0);

	const struct ps_listener_state *pls = ps_state_find(2);

	ck_assert_ptr_nonnull(pls);
	ck_assert_str_eq(pls->pls_upstream, "");
	ck_assert_uint_eq(pls->pls_upstream_port, 2049);
}
END_TEST

/*
 * listener_id 0 is reserved for the native listener.  A proxy_mds
 * entry with id 0 is a config error that reffsd.c already logs and
 * skips; the registry rejects it as a safety net so callers can't
 * accidentally make the native listener findable here.
 */
START_TEST(test_register_id_zero_rejected)
{
	struct reffs_proxy_mds_config c = make_cfg(0, "10.0.0.5");

	ck_assert_int_eq(ps_state_register(&c), -EINVAL);
	ck_assert_ptr_null(ps_state_find(0));
}
END_TEST

/*
 * A second register for an id already in the table returns -EEXIST
 * and leaves the first entry intact.  reffsd.c can therefore call
 * register() unconditionally per proxy_mds entry and rely on this
 * check to catch a duplicate id in the config.
 */
START_TEST(test_register_duplicate_rejected)
{
	struct reffs_proxy_mds_config c1 = make_cfg(1, "10.0.0.5");
	struct reffs_proxy_mds_config c2 = make_cfg(1, "10.0.0.6");

	ck_assert_int_eq(ps_state_register(&c1), 0);
	ck_assert_int_eq(ps_state_register(&c2), -EEXIST);

	const struct ps_listener_state *pls = ps_state_find(1);

	ck_assert_str_eq(pls->pls_upstream, "10.0.0.5");
}
END_TEST

/*
 * Register up to the configured max; ensure each is separately
 * findable and an unregistered id returns NULL.
 */
START_TEST(test_register_multiple)
{
	struct reffs_proxy_mds_config c1 = make_cfg(1, "10.0.0.5");
	struct reffs_proxy_mds_config c2 = make_cfg(2, "10.0.0.6");
	struct reffs_proxy_mds_config c3 = make_cfg(3, "");

	ck_assert_int_eq(ps_state_register(&c1), 0);
	ck_assert_int_eq(ps_state_register(&c2), 0);
	ck_assert_int_eq(ps_state_register(&c3), 0);

	ck_assert_uint_eq(ps_state_find(1)->pls_listener_id, 1);
	ck_assert_str_eq(ps_state_find(1)->pls_upstream, "10.0.0.5");
	ck_assert_uint_eq(ps_state_find(2)->pls_listener_id, 2);
	ck_assert_str_eq(ps_state_find(2)->pls_upstream, "10.0.0.6");
	ck_assert_uint_eq(ps_state_find(3)->pls_listener_id, 3);
	ck_assert_str_eq(ps_state_find(3)->pls_upstream, "");

	ck_assert_ptr_null(ps_state_find(4));
}
END_TEST

/*
 * fini clears the registry; a subsequent init restores a clean
 * state and prior registrations are gone.  Important so the
 * single-process test runner can't leak state between tests (the
 * fixture does this, but the invariant is worth asserting).
 */
START_TEST(test_fini_clears_registry)
{
	struct reffs_proxy_mds_config c = make_cfg(1, "10.0.0.5");

	ck_assert_int_eq(ps_state_register(&c), 0);
	ps_state_fini();
	ps_state_init();

	ck_assert_ptr_null(ps_state_find(1));
	/* Can re-register after fini/init with no false-duplicate error. */
	ck_assert_int_eq(ps_state_register(&c), 0);
}
END_TEST

/*
 * A NULL config is rejected cleanly, not crashed.
 */
START_TEST(test_register_null_rejected)
{
	ck_assert_int_eq(ps_state_register(NULL), -EINVAL);
}
END_TEST

/*
 * A freshly-registered listener has pls_session == NULL.  The
 * session is attached later by reffsd.c after mds_session_create()
 * succeeds; until then, op handlers that read pls_session see NULL
 * and must fail gracefully.
 */
START_TEST(test_session_defaults_null)
{
	struct reffs_proxy_mds_config c = make_cfg(1, "10.0.0.5");

	ck_assert_int_eq(ps_state_register(&c), 0);
	ck_assert_ptr_null(ps_state_find(1)->pls_session);
}
END_TEST

/*
 * ps_state_set_session stores the pointer verbatim.  Registry does
 * not dereference or own-destroy -- the caller is the owner.  A
 * sentinel non-NULL pointer (cast from an integer) is enough to
 * prove round-trip storage; no real mds_session is needed for this
 * contract.
 */
START_TEST(test_set_session_stores_pointer)
{
	struct reffs_proxy_mds_config c = make_cfg(1, "10.0.0.5");
	struct mds_session *sentinel =
		(struct mds_session *)(uintptr_t)0xDEADBEEF;

	ck_assert_int_eq(ps_state_register(&c), 0);
	ck_assert_int_eq(ps_state_set_session(1, sentinel), 0);
	ck_assert_ptr_eq(ps_state_find(1)->pls_session, sentinel);

	/* NULL clears the stored pointer (used at shutdown). */
	ck_assert_int_eq(ps_state_set_session(1, NULL), 0);
	ck_assert_ptr_null(ps_state_find(1)->pls_session);
}
END_TEST

/*
 * Setting the session on an unregistered listener returns -ENOENT,
 * not a silent no-op -- callers need to know the registry is in an
 * unexpected state.
 */
START_TEST(test_set_session_unknown_id_fails)
{
	struct mds_session *sentinel =
		(struct mds_session *)(uintptr_t)0xDEADBEEF;

	/* Registry is empty (setup calls ps_state_init). */
	ck_assert_int_eq(ps_state_set_session(42, sentinel), -ENOENT);
	ck_assert_int_eq(ps_state_set_session(0, sentinel), -ENOENT);
}
END_TEST

/*
 * A freshly-registered listener has no MDS root FH stored
 * (pls_mds_root_fh_len == 0).  Discovery populates this after
 * mds_session_create succeeds.
 */
START_TEST(test_mds_root_fh_defaults_empty)
{
	struct reffs_proxy_mds_config c = make_cfg(1, "10.0.0.5");

	ck_assert_int_eq(ps_state_register(&c), 0);
	ck_assert_uint_eq(ps_state_find(1)->pls_mds_root_fh_len, 0);
}
END_TEST

/*
 * set_mds_root_fh copies the bytes verbatim and records the length.
 * Caller-owned buffer -- registry does not keep a reference to it.
 */
START_TEST(test_set_mds_root_fh_stores_bytes)
{
	struct reffs_proxy_mds_config c = make_cfg(1, "10.0.0.5");
	uint8_t fh[] = { 0x01, 0x02, 0x03, 0x04, 0x05 };

	ck_assert_int_eq(ps_state_register(&c), 0);
	ck_assert_int_eq(ps_state_set_mds_root_fh(1, fh, sizeof(fh)), 0);

	const struct ps_listener_state *pls = ps_state_find(1);

	ck_assert_uint_eq(pls->pls_mds_root_fh_len, sizeof(fh));
	ck_assert_mem_eq(pls->pls_mds_root_fh, fh, sizeof(fh));

	/* Mutating the caller's buffer does not disturb the stored copy. */
	fh[0] = 0xFF;
	ck_assert_uint_eq(pls->pls_mds_root_fh[0], 0x01);
}
END_TEST

/*
 * Clearing (fh_len=0) is legal and does NOT require a non-NULL
 * buffer -- it's the "forget what we learned" path.
 */
START_TEST(test_set_mds_root_fh_clear)
{
	struct reffs_proxy_mds_config c = make_cfg(1, "10.0.0.5");
	uint8_t fh[] = { 0x01, 0x02, 0x03 };

	ck_assert_int_eq(ps_state_register(&c), 0);
	ck_assert_int_eq(ps_state_set_mds_root_fh(1, fh, sizeof(fh)), 0);
	ck_assert_int_eq(ps_state_set_mds_root_fh(1, NULL, 0), 0);
	ck_assert_uint_eq(ps_state_find(1)->pls_mds_root_fh_len, 0);
}
END_TEST

/*
 * NFSv4 FHs are <= 128 bytes (RFC 8881).  An over-size buffer
 * returns -E2BIG so the caller can't silently truncate.
 */
START_TEST(test_set_mds_root_fh_too_big)
{
	struct reffs_proxy_mds_config c = make_cfg(1, "10.0.0.5");
	uint8_t fh[PS_MAX_FH_SIZE + 1];

	memset(fh, 0xAB, sizeof(fh));

	ck_assert_int_eq(ps_state_register(&c), 0);
	ck_assert_int_eq(ps_state_set_mds_root_fh(1, fh, sizeof(fh)), -E2BIG);
	/* Previous (empty) state preserved on failure. */
	ck_assert_uint_eq(ps_state_find(1)->pls_mds_root_fh_len, 0);
}
END_TEST

/*
 * Setting on an unregistered listener returns -ENOENT, same
 * contract as set_session.
 */
START_TEST(test_set_mds_root_fh_unknown_id)
{
	uint8_t fh[] = { 0x01 };

	ck_assert_int_eq(ps_state_set_mds_root_fh(42, fh, 1), -ENOENT);
}
END_TEST

/*
 * Bad-arg rejection for ps_state_add_export.  NULL / empty path,
 * NULL FH, fh_len=0, oversize FH, oversize path, and unknown
 * listener_id all short-circuit before any state mutation.  Zero-
 * length FH is rejected because ple_fh_len == 0 is the empty-slot
 * sentinel -- accepting a zero-length FH would be indistinguishable
 * from "slot not populated" at read time.
 */
START_TEST(test_add_export_rejects_bad_args)
{
	struct reffs_proxy_mds_config c = make_cfg(1, "10.0.0.5");
	uint8_t fh[] = { 0x01, 0x02, 0x03 };
	uint8_t big_fh[PS_MAX_FH_SIZE + 1];
	char big_path[2000];

	ck_assert_int_eq(ps_state_register(&c), 0);

	/* Path validation. */
	ck_assert_int_eq(ps_state_add_export(1, NULL, fh, sizeof(fh)), -EINVAL);
	ck_assert_int_eq(ps_state_add_export(1, "", fh, sizeof(fh)), -EINVAL);

	/* FH validation. */
	ck_assert_int_eq(ps_state_add_export(1, "/x", NULL, sizeof(fh)),
			 -EINVAL);
	ck_assert_int_eq(ps_state_add_export(1, "/x", fh, 0), -EINVAL);

	/* Size caps. */
	memset(big_fh, 0xAB, sizeof(big_fh));
	ck_assert_int_eq(ps_state_add_export(1, "/x", big_fh, sizeof(big_fh)),
			 -E2BIG);

	memset(big_path, 'a', sizeof(big_path) - 1);
	big_path[0] = '/';
	big_path[sizeof(big_path) - 1] = '\0';
	ck_assert_int_eq(ps_state_add_export(1, big_path, fh, sizeof(fh)),
			 -E2BIG);

	/* Unknown listener. */
	ck_assert_int_eq(ps_state_add_export(999, "/x", fh, sizeof(fh)),
			 -ENOENT);
}
END_TEST

/*
 * Happy-path round-trip: add two exports on one listener, each is
 * findable by path, the returned slot carries verbatim FH bytes,
 * and find on an unknown path returns NULL.
 */
START_TEST(test_add_and_find_export)
{
	struct reffs_proxy_mds_config c = make_cfg(1, "10.0.0.5");
	uint8_t fh_a[] = { 0x0a, 0x0b, 0x0c };
	uint8_t fh_b[] = { 0x01, 0x02, 0x03, 0x04 };

	ck_assert_int_eq(ps_state_register(&c), 0);
	ck_assert_int_eq(
		ps_state_add_export(1, "/export/a", fh_a, sizeof(fh_a)), 0);
	ck_assert_int_eq(
		ps_state_add_export(1, "/export/b", fh_b, sizeof(fh_b)), 0);

	const struct ps_export *a = ps_state_find_export(1, "/export/a");

	ck_assert_ptr_nonnull(a);
	ck_assert_str_eq(a->ple_path, "/export/a");
	ck_assert_uint_eq(a->ple_fh_len, sizeof(fh_a));
	ck_assert_mem_eq(a->ple_fh, fh_a, sizeof(fh_a));

	const struct ps_export *b = ps_state_find_export(1, "/export/b");

	ck_assert_ptr_nonnull(b);
	ck_assert_uint_eq(b->ple_fh_len, sizeof(fh_b));
	ck_assert_mem_eq(b->ple_fh, fh_b, sizeof(fh_b));

	/* Unknown path / unknown listener / bad path -- all NULL. */
	ck_assert_ptr_null(ps_state_find_export(1, "/nope"));
	ck_assert_ptr_null(ps_state_find_export(999, "/export/a"));
	ck_assert_ptr_null(ps_state_find_export(1, NULL));
	ck_assert_ptr_null(ps_state_find_export(1, ""));

	/*
	 * Prefix-match guard: /export/a MUST NOT satisfy a lookup for
	 * /export/ab (or vice versa).  strcmp does a full-string
	 * comparison, but adding the two cases explicitly documents
	 * the invariant so a future "starts with" optimisation can't
	 * silently regress it.
	 */
	ck_assert_ptr_null(ps_state_find_export(1, "/export/ab"));
	ck_assert_ptr_null(ps_state_find_export(1, "/export"));
}
END_TEST

/*
 * Re-adding the same path updates the FH in place without growing
 * the table.  This is the on-demand re-discovery path: after an
 * upstream restart the MDS may re-issue new FHs for the same paths,
 * and the PS refreshes its cache without accumulating stale entries.
 */
START_TEST(test_add_export_duplicate_path_updates)
{
	struct reffs_proxy_mds_config c = make_cfg(1, "10.0.0.5");
	uint8_t fh_old[] = { 0x11, 0x22 };
	uint8_t fh_new[] = { 0xaa, 0xbb, 0xcc, 0xdd, 0xee };

	ck_assert_int_eq(ps_state_register(&c), 0);
	ck_assert_int_eq(
		ps_state_add_export(1, "/data", fh_old, sizeof(fh_old)), 0);
	ck_assert_int_eq(
		ps_state_add_export(1, "/data", fh_new, sizeof(fh_new)), 0);

	const struct ps_export *ex = ps_state_find_export(1, "/data");

	ck_assert_ptr_nonnull(ex);
	ck_assert_uint_eq(ex->ple_fh_len, sizeof(fh_new));
	ck_assert_mem_eq(ex->ple_fh, fh_new, sizeof(fh_new));
}
END_TEST

/*
 * Saturate the per-listener exports array: PS_MAX_EXPORTS_PER_LISTENER
 * unique paths register, the (N+1)th returns -ENOSPC.  Re-adding an
 * existing path still succeeds (update-in-place does not consume a
 * slot), so the cap is only on distinct paths.
 */
START_TEST(test_add_export_full_table)
{
	struct reffs_proxy_mds_config c = make_cfg(1, "10.0.0.5");
	uint8_t fh[] = { 0x01 };
	char path[64];

	ck_assert_int_eq(ps_state_register(&c), 0);

	for (uint32_t i = 0; i < PS_MAX_EXPORTS_PER_LISTENER; i++) {
		snprintf(path, sizeof(path), "/p%u", i);
		ck_assert_int_eq(ps_state_add_export(1, path, fh, sizeof(fh)),
				 0);
	}
	ck_assert_int_eq(ps_state_add_export(1, "/overflow", fh, sizeof(fh)),
			 -ENOSPC);

	/* Re-adding an existing path still works -- update-in-place. */
	ck_assert_int_eq(ps_state_add_export(1, "/p0", fh, sizeof(fh)), 0);
}
END_TEST

/*
 * Discovery mutex is initialised at register time and destroyable
 * via fini.  A lock-unlock round-trip on a registered listener must
 * succeed; a lock on an unknown id must return -ENOENT rather than
 * blocking or dereferencing a bogus slot.
 */
START_TEST(test_discovery_lock_unlock)
{
	struct reffs_proxy_mds_config c = make_cfg(1, "10.0.0.5");

	ck_assert_int_eq(ps_state_register(&c), 0);

	ck_assert_int_eq(ps_state_discovery_lock(1), 0);
	ck_assert_int_eq(ps_state_discovery_unlock(1), 0);

	/* Same listener -- a second round-trip must work identically. */
	ck_assert_int_eq(ps_state_discovery_lock(1), 0);
	ck_assert_int_eq(ps_state_discovery_unlock(1), 0);
}
END_TEST

/*
 * Lock/unlock on an unregistered id returns -ENOENT.  Matches
 * ps_state_set_session / ps_state_set_mds_root_fh conventions so
 * callers can tell "no such listener" from "actual lock failure."
 */
START_TEST(test_discovery_lock_unknown_id)
{
	ck_assert_int_eq(ps_state_discovery_lock(42), -ENOENT);
	ck_assert_int_eq(ps_state_discovery_unlock(42), -ENOENT);
	ck_assert_int_eq(ps_state_discovery_lock(0), -ENOENT);
}
END_TEST

/*
 * fini after register + lock/unlock sequence runs cleanly -- the
 * mutex is destroyable only when not held, so the lock/unlock
 * pairing above is a prerequisite.  Implicitly verifies no slot
 * escapes destruction when the registry is wiped.
 */
START_TEST(test_discovery_mutex_fini_clean)
{
	struct reffs_proxy_mds_config c1 = make_cfg(1, "10.0.0.5");
	struct reffs_proxy_mds_config c2 = make_cfg(2, "10.0.0.6");

	ck_assert_int_eq(ps_state_register(&c1), 0);
	ck_assert_int_eq(ps_state_register(&c2), 0);

	ck_assert_int_eq(ps_state_discovery_lock(1), 0);
	ck_assert_int_eq(ps_state_discovery_unlock(1), 0);
	ck_assert_int_eq(ps_state_discovery_lock(2), 0);
	ck_assert_int_eq(ps_state_discovery_unlock(2), 0);

	/* teardown fixture calls ps_state_fini -- must not assert. */
}
END_TEST

/*
 * Exports iterator test support: the callback pushes each slot it
 * receives into a small capture buffer so the test can verify path,
 * length, and FH bytes without colliding with the function-body
 * stack of the test itself.
 */
struct export_capture_entry {
	char path[64];
	uint8_t fh[8];
	uint32_t fh_len;
};

struct export_capture {
	struct export_capture_entry entries[8];
	unsigned int count;
};

static void capture_cb(const struct ps_export *ex, void *ctx)
{
	struct export_capture *cap = ctx;

	if (cap->count >= 8)
		return;

	struct export_capture_entry *e = &cap->entries[cap->count++];

	strncpy(e->path, ex->ple_path, sizeof(e->path) - 1);
	e->fh_len = ex->ple_fh_len;
	if (ex->ple_fh_len <= sizeof(e->fh))
		memcpy(e->fh, ex->ple_fh, ex->ple_fh_len);
}

/*
 * Empty listener: iterator returns 0 and does not invoke cb.  Proves
 * the "no exports yet" case (common pre-discovery state) does not
 * trip any stale entry by accident.
 */
START_TEST(test_exports_for_each_empty)
{
	struct reffs_proxy_mds_config c = make_cfg(1, "10.0.0.5");
	struct export_capture cap = { .count = 0 };

	ck_assert_int_eq(ps_state_register(&c), 0);
	ck_assert_int_eq(ps_state_exports_for_each(1, capture_cb, &cap), 0);
	ck_assert_uint_eq(cap.count, 0);
}
END_TEST

/*
 * Three exports: iterator visits each in registration order, every
 * field round-trips.  Confirms the iterator sees the same slot
 * addresses a ps_state_find_export would.
 */
START_TEST(test_exports_for_each_visits_all)
{
	struct reffs_proxy_mds_config c = make_cfg(1, "10.0.0.5");
	uint8_t fh_a[] = { 0x0a };
	uint8_t fh_b[] = { 0x0b, 0x0c };
	uint8_t fh_c[] = { 0x01, 0x02, 0x03 };
	struct export_capture cap = { .count = 0 };

	ck_assert_int_eq(ps_state_register(&c), 0);
	ck_assert_int_eq(ps_state_add_export(1, "/alpha", fh_a, sizeof(fh_a)),
			 0);
	ck_assert_int_eq(ps_state_add_export(1, "/beta", fh_b, sizeof(fh_b)),
			 0);
	ck_assert_int_eq(ps_state_add_export(1, "/gamma", fh_c, sizeof(fh_c)),
			 0);

	ck_assert_int_eq(ps_state_exports_for_each(1, capture_cb, &cap), 3);
	ck_assert_uint_eq(cap.count, 3);

	ck_assert_str_eq(cap.entries[0].path, "/alpha");
	ck_assert_uint_eq(cap.entries[0].fh_len, sizeof(fh_a));
	ck_assert_mem_eq(cap.entries[0].fh, fh_a, sizeof(fh_a));

	ck_assert_str_eq(cap.entries[1].path, "/beta");
	ck_assert_uint_eq(cap.entries[1].fh_len, sizeof(fh_b));
	ck_assert_mem_eq(cap.entries[1].fh, fh_b, sizeof(fh_b));

	ck_assert_str_eq(cap.entries[2].path, "/gamma");
	ck_assert_uint_eq(cap.entries[2].fh_len, sizeof(fh_c));
	ck_assert_mem_eq(cap.entries[2].fh, fh_c, sizeof(fh_c));
}
END_TEST

/*
 * Unknown listener: -ENOENT (matches set_session / set_mds_root_fh /
 * discovery_lock conventions).  NULL cb: -EINVAL.
 */
START_TEST(test_exports_for_each_bad_args)
{
	struct export_capture cap = { .count = 0 };

	ck_assert_int_eq(ps_state_exports_for_each(42, capture_cb, &cap),
			 -ENOENT);
	/*
	 * Registry has id 0 reserved; listener_by_id(0) returns NULL so
	 * the same -ENOENT fires rather than walking a bogus slot.
	 */
	ck_assert_int_eq(ps_state_exports_for_each(0, capture_cb, &cap),
			 -ENOENT);

	struct reffs_proxy_mds_config c = make_cfg(1, "10.0.0.5");

	ck_assert_int_eq(ps_state_register(&c), 0);
	ck_assert_int_eq(ps_state_exports_for_each(1, NULL, &cap), -EINVAL);
}
END_TEST

static Suite *ps_state_suite(void)
{
	Suite *s = suite_create("ps_state");
	TCase *tc = tcase_create("core");

	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_find_empty_returns_null);
	tcase_add_test(tc, test_register_and_find);
	tcase_add_test(tc, test_register_empty_address);
	tcase_add_test(tc, test_register_id_zero_rejected);
	tcase_add_test(tc, test_register_duplicate_rejected);
	tcase_add_test(tc, test_register_multiple);
	tcase_add_test(tc, test_fini_clears_registry);
	tcase_add_test(tc, test_register_null_rejected);
	tcase_add_test(tc, test_session_defaults_null);
	tcase_add_test(tc, test_set_session_stores_pointer);
	tcase_add_test(tc, test_set_session_unknown_id_fails);
	tcase_add_test(tc, test_mds_root_fh_defaults_empty);
	tcase_add_test(tc, test_set_mds_root_fh_stores_bytes);
	tcase_add_test(tc, test_set_mds_root_fh_clear);
	tcase_add_test(tc, test_set_mds_root_fh_too_big);
	tcase_add_test(tc, test_set_mds_root_fh_unknown_id);
	tcase_add_test(tc, test_add_export_rejects_bad_args);
	tcase_add_test(tc, test_add_and_find_export);
	tcase_add_test(tc, test_add_export_duplicate_path_updates);
	tcase_add_test(tc, test_add_export_full_table);
	tcase_add_test(tc, test_discovery_lock_unlock);
	tcase_add_test(tc, test_discovery_lock_unknown_id);
	tcase_add_test(tc, test_discovery_mutex_fini_clean);
	tcase_add_test(tc, test_exports_for_each_empty);
	tcase_add_test(tc, test_exports_for_each_visits_all);
	tcase_add_test(tc, test_exports_for_each_bad_args);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int failed;
	SRunner *sr = srunner_create(ps_state_suite());

	srunner_run_all(sr, CK_NORMAL);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return failed == 0 ? 0 : 1;
}
