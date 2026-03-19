/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * nfs4_client_persist_1.c — unit tests for:
 *
 *   - client_identity_append / client_identity_load round-trip
 *   - client_incarnation_add / client_incarnation_remove / symlink-swap
 *   - nfs4_client_find_by_owner: found / not-found / expired
 *   - nfs4_client_alloc_or_find: all five decision-tree cases
 *   - nfs4_client_expire: incarnations file updated before client_put
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <netinet/in.h>

#include <check.h>

#include "nfsv42_xdr.h"
#include "reffs/rcu.h"
#include "reffs/client.h"
#include "reffs/client_persist.h"
#include "reffs/server.h"
#include "nfs4/client.h"
#include "nfs4/client_persist.h"
#include "nfs4_test_harness.h"

/* ------------------------------------------------------------------ */
/* Test fixture                                                        */
/* ------------------------------------------------------------------ */

static struct server_state *g_ss;

static void setup(void)
{
	nfs4_test_setup();
	g_ss = server_state_find();
	ck_assert_ptr_nonnull(g_ss);
}

static void teardown(void)
{
	server_state_put(g_ss);
	g_ss = NULL;
	nfs4_test_teardown();
}

/* ------------------------------------------------------------------ */
/* Wire-parameter helpers                                              */
/* ------------------------------------------------------------------ */

static void make_owner(client_owner4 *owner, const char *id_str, char *buf,
		       size_t bufsz)
{
	size_t len = strlen(id_str);
	if (len >= bufsz)
		len = bufsz - 1;
	memcpy(buf, id_str, len);
	owner->co_ownerid.co_ownerid_val = buf;
	owner->co_ownerid.co_ownerid_len = (u_int)len;
}

static void make_verifier(verifier4 *v, uint8_t byte)
{
	memset(v, byte, NFS4_VERIFIER_SIZE);
}

static void make_sin(struct sockaddr_in *sin, uint32_t ip, uint16_t port)
{
	memset(sin, 0, sizeof(*sin));
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = htonl(ip);
	sin->sin_port = htons(port);
}

static struct nfs_impl_id4 make_impl_id(void)
{
	static char domain[] = "example.com";
	static char name[] = "test-client";
	struct nfs_impl_id4 id = {
		.nii_domain = { .utf8string_val = domain,
				.utf8string_len = sizeof(domain) - 1 },
		.nii_name = { .utf8string_val = name,
			      .utf8string_len = sizeof(name) - 1 },
	};
	return id;
}

/* ------------------------------------------------------------------ */
/* client_identity_load helpers                                        */
/*                                                                     */
/* client_identity_load() is callback-based.  collect_ids_cb() builds  */
/* a heap array so tests can make indexed assertions.                  */
/* ------------------------------------------------------------------ */

struct collect_ids_arg {
	struct client_identity_record *recs;
	uint32_t count;
	uint32_t cap;
};

static int collect_ids_cb(const struct client_identity_record *cir, void *arg)
{
	struct collect_ids_arg *a = arg;

	if (a->count == a->cap) {
		uint32_t newcap = a->cap ? a->cap * 2 : 8;
		struct client_identity_record *tmp =
			realloc(a->recs, newcap * sizeof(*tmp));
		if (!tmp)
			return -1;
		a->recs = tmp;
		a->cap = newcap;
	}
	a->recs[a->count++] = *cir;
	return 0;
}

/*
 * load_all_ids - collect every identity record into a heap array.
 * Returns 0 on success, -ENOENT if no clients file exists yet.
 * Caller must free(*out_recs).
 */
static int load_all_ids(const char *state_dir,
			struct client_identity_record **out_recs,
			uint32_t *out_count)
{
	struct collect_ids_arg a = { NULL, 0, 0 };
	int ret;

	ret = client_identity_load(state_dir, collect_ids_cb, &a);
	if (ret < 0) {
		free(a.recs);
		*out_recs = NULL;
		*out_count = 0;
		return ret;
	}
	*out_recs = a.recs;
	*out_count = a.count;
	return 0;
}

/* ------------------------------------------------------------------ */
/* client_incarnation_load helper                                      */
/*                                                                     */
/* Takes a caller-supplied fixed buffer.  Tests never create more than  */
/* TEST_MAX_INCS active clients, so a stack array is fine.             */
/* ------------------------------------------------------------------ */

#define TEST_MAX_INCS 64

/* ------------------------------------------------------------------ */
/* client_identity round-trip                                          */
/* ------------------------------------------------------------------ */

START_TEST(test_identity_append_load_roundtrip)
{
	struct client_identity_record written;
	struct client_identity_record *loaded;
	uint32_t count;
	char ownerid_buf[] = "client-identity-test";

	memset(&written, 0, sizeof(written));
	written.cir_magic = CLIENT_IDENTITY_MAGIC;
	written.cir_slot = 7;
	written.cir_ownerid_len = (uint16_t)strlen(ownerid_buf);
	memcpy(written.cir_ownerid, ownerid_buf, written.cir_ownerid_len);
	strncpy(written.cir_domain, "example.com",
		sizeof(written.cir_domain) - 1);
	strncpy(written.cir_name, "roundtrip", sizeof(written.cir_name) - 1);

	ck_assert_int_eq(client_identity_append(g_ss->ss_state_dir, &written),
			 0);

	loaded = NULL;
	count = 0;
	ck_assert_int_eq(load_all_ids(g_ss->ss_state_dir, &loaded, &count), 0);
	ck_assert_ptr_nonnull(loaded);
	ck_assert_uint_eq(count, 1);

	ck_assert_uint_eq(loaded[0].cir_magic, CLIENT_IDENTITY_MAGIC);
	ck_assert_uint_eq(loaded[0].cir_slot, 7);
	ck_assert_uint_eq(loaded[0].cir_ownerid_len, written.cir_ownerid_len);
	ck_assert_mem_eq(loaded[0].cir_ownerid, written.cir_ownerid,
			 written.cir_ownerid_len);
	ck_assert_str_eq(loaded[0].cir_domain, "example.com");
	ck_assert_str_eq(loaded[0].cir_name, "roundtrip");

	free(loaded);
}
END_TEST

START_TEST(test_identity_append_multiple)
{
	struct client_identity_record rec;
	struct client_identity_record *loaded;
	uint32_t count;
	char buf[32];

	for (uint32_t i = 0; i < 5; i++) {
		memset(&rec, 0, sizeof(rec));
		rec.cir_magic = CLIENT_IDENTITY_MAGIC;
		rec.cir_slot = i;
		snprintf(buf, sizeof(buf), "owner-%u", i);
		rec.cir_ownerid_len = (uint16_t)strlen(buf);
		memcpy(rec.cir_ownerid, buf, rec.cir_ownerid_len);
		ck_assert_int_eq(
			client_identity_append(g_ss->ss_state_dir, &rec), 0);
	}

	loaded = NULL;
	count = 0;
	ck_assert_int_eq(load_all_ids(g_ss->ss_state_dir, &loaded, &count), 0);
	ck_assert_uint_eq(count, 5);
	for (uint32_t i = 0; i < 5; i++)
		ck_assert_uint_eq(loaded[i].cir_slot, i);

	free(loaded);
}
END_TEST

/*
 * Domain and name must survive the round-trip on disk and must NOT
 * appear anywhere on the in-memory nfs4_client struct.
 */
START_TEST(test_identity_domain_name_on_disk_not_in_memory)
{
	char owner_buf[32];
	client_owner4 owner;
	verifier4 v;
	struct sockaddr_in sin;
	struct nfs_impl_id4 impl = make_impl_id();

	make_owner(&owner, "domain-name-test", owner_buf, sizeof(owner_buf));
	make_verifier(&v, 0x42);
	make_sin(&sin, 0x7f000001, 2049);

	struct nfs4_client *nc =
		nfs4_client_alloc_or_find(g_ss, &owner, &impl, &v, &sin);
	ck_assert_ptr_nonnull(nc);

	struct client_identity_record *ids;
	uint32_t nids;
	ck_assert_int_eq(load_all_ids(g_ss->ss_state_dir, &ids, &nids), 0);
	ck_assert_uint_ge(nids, 1);
	ck_assert_str_eq(ids[0].cir_domain, "example.com");
	ck_assert_str_eq(ids[0].cir_name, "test-client");
	free(ids);

	nfs4_client_expire(g_ss, nc);
}
END_TEST

/* ------------------------------------------------------------------ */
/* client_incarnation add / remove / symlink-swap                     */
/* ------------------------------------------------------------------ */

START_TEST(test_incarnation_add_remove)
{
	struct client_incarnation_record crc;
	struct client_incarnation_record active[TEST_MAX_INCS];
	size_t nactive;
	verifier4 v;
	struct sockaddr_in sin;
	bool found;

	make_verifier(&v, 0xAB);
	make_sin(&sin, 0x7f000001, 2049);

	memset(&crc, 0, sizeof(crc));
	crc.crc_magic = CLIENT_INCARNATION_MAGIC;
	crc.crc_slot = 3;
	crc.crc_boot_seq = 1;
	crc.crc_incarnation = 0;
	memcpy(crc.crc_verifier, &v, NFS4_VERIFIER_SIZE);
	sockaddr_in_to_full_str(&sin, crc.crc_addr, sizeof(crc.crc_addr));

	ck_assert_int_eq(client_incarnation_add(g_ss->ss_state_dir, &crc), 0);

	nactive = 0;
	ck_assert_int_eq(client_incarnation_load(g_ss->ss_state_dir, active,
						 TEST_MAX_INCS, &nactive),
			 0);
	ck_assert_uint_ge(nactive, 1);

	found = false;
	for (size_t i = 0; i < nactive; i++) {
		if (active[i].crc_slot == 3) {
			found = true;
			ck_assert_uint_eq(active[i].crc_incarnation, 0);
			ck_assert_mem_eq(active[i].crc_verifier, &v,
					 NFS4_VERIFIER_SIZE);
			break;
		}
	}
	ck_assert(found);

	ck_assert_int_eq(client_incarnation_remove(g_ss->ss_state_dir, 3), 0);

	nactive = 0;
	ck_assert_int_eq(client_incarnation_load(g_ss->ss_state_dir, active,
						 TEST_MAX_INCS, &nactive),
			 0);
	found = false;
	for (size_t i = 0; i < nactive; i++) {
		if (active[i].crc_slot == 3)
			found = true;
	}
	ck_assert(!found);
}
END_TEST

START_TEST(test_incarnation_symlink_swap_atomicity)
{
	struct client_incarnation_record crc;
	struct client_incarnation_record active[TEST_MAX_INCS];
	size_t nactive;
	verifier4 v;
	struct sockaddr_in sin;

	make_verifier(&v, 0x01);
	make_sin(&sin, 0x0a000001, 2049);

	for (uint32_t slot = 10; slot <= 11; slot++) {
		memset(&crc, 0, sizeof(crc));
		crc.crc_magic = CLIENT_INCARNATION_MAGIC;
		crc.crc_slot = slot;
		crc.crc_boot_seq = 1;
		crc.crc_incarnation = 0;
		memcpy(crc.crc_verifier, &v, NFS4_VERIFIER_SIZE);
		sockaddr_in_to_full_str(&sin, crc.crc_addr,
					sizeof(crc.crc_addr));
		ck_assert_int_eq(
			client_incarnation_add(g_ss->ss_state_dir, &crc), 0);
	}

	ck_assert_int_eq(client_incarnation_remove(g_ss->ss_state_dir, 10), 0);

	nactive = 0;
	ck_assert_int_eq(client_incarnation_load(g_ss->ss_state_dir, active,
						 TEST_MAX_INCS, &nactive),
			 0);

	bool slot10 = false, slot11 = false;
	for (size_t i = 0; i < nactive; i++) {
		if (active[i].crc_slot == 10)
			slot10 = true;
		if (active[i].crc_slot == 11)
			slot11 = true;
	}
	ck_assert(!slot10);
	ck_assert(slot11);
}
END_TEST

/* ------------------------------------------------------------------ */
/* nfs4_client_find_by_owner                                           */
/* ------------------------------------------------------------------ */

START_TEST(test_find_by_owner_not_found)
{
	char owner_buf[32];
	client_owner4 owner;

	make_owner(&owner, "no-such-client", owner_buf, sizeof(owner_buf));

	struct nfs4_client *nc = nfs4_client_find_by_owner(
		g_ss->ss_state_dir, server_boot_seq(g_ss), &owner, NULL);
	ck_assert_ptr_null(nc);
}
END_TEST

START_TEST(test_find_by_owner_found)
{
	char owner_buf[32];
	client_owner4 owner;
	verifier4 v;
	struct sockaddr_in sin;
	struct nfs_impl_id4 impl = make_impl_id();

	make_owner(&owner, "find-by-owner-test", owner_buf, sizeof(owner_buf));
	make_verifier(&v, 0xF0);
	make_sin(&sin, 0x7f000001, 2049);

	struct nfs4_client *nc1 =
		nfs4_client_alloc_or_find(g_ss, &owner, &impl, &v, &sin);
	ck_assert_ptr_nonnull(nc1);

	struct nfs4_client *nc2 = nfs4_client_find_by_owner(
		g_ss->ss_state_dir, server_boot_seq(g_ss), &owner, NULL);
	ck_assert_ptr_nonnull(nc2);
	ck_assert_ptr_eq(nc1, nc2);

	nfs4_client_put(nc1);
	nfs4_client_expire(g_ss, nc2);
}
END_TEST

/*
 * After expire the client is gone from the in-memory table.
 * find_by_owner must return NULL even though the identity record
 * is still in the clients file (it's append-only).
 */
START_TEST(test_find_by_owner_after_expire)
{
	char owner_buf[32];
	client_owner4 owner;
	verifier4 v;
	struct sockaddr_in sin;
	struct nfs_impl_id4 impl = make_impl_id();

	make_owner(&owner, "expired-client", owner_buf, sizeof(owner_buf));
	make_verifier(&v, 0xD0);
	make_sin(&sin, 0x7f000001, 2049);

	struct nfs4_client *nc =
		nfs4_client_alloc_or_find(g_ss, &owner, &impl, &v, &sin);
	ck_assert_ptr_nonnull(nc);

	nfs4_client_expire(g_ss, nc);

	struct nfs4_client *ghost = nfs4_client_find_by_owner(
		g_ss->ss_state_dir, server_boot_seq(g_ss), &owner, NULL);
	ck_assert_ptr_null(ghost);
}
END_TEST

/* ------------------------------------------------------------------ */
/* nfs4_client_alloc_or_find: all five decision-tree cases            */
/* ------------------------------------------------------------------ */

/* Case 1: New ownerid → new client, incarnation 0. */
START_TEST(test_alloc_new_client)
{
	char owner_buf[32];
	client_owner4 owner;
	verifier4 v;
	struct sockaddr_in sin;
	struct nfs_impl_id4 impl = make_impl_id();

	make_owner(&owner, "new-client-owner", owner_buf, sizeof(owner_buf));
	make_verifier(&v, 0x11);
	make_sin(&sin, 0x7f000001, 2049);

	struct nfs4_client *nc =
		nfs4_client_alloc_or_find(g_ss, &owner, &impl, &v, &sin);
	ck_assert_ptr_nonnull(nc);
	ck_assert_uint_eq(clientid_incarnation(
				  (clientid4)nfs4_client_to_client(nc)->c_id),
			  0);

	/* Identity record written. */
	struct client_identity_record *ids;
	uint32_t nids;
	ck_assert_int_eq(load_all_ids(g_ss->ss_state_dir, &ids, &nids), 0);
	ck_assert_uint_ge(nids, 1);
	free(ids);

	/* Incarnation record written. */
	struct client_incarnation_record incs[TEST_MAX_INCS];
	size_t nincs = 0;
	ck_assert_int_eq(client_incarnation_load(g_ss->ss_state_dir, incs,
						 TEST_MAX_INCS, &nincs),
			 0);
	ck_assert_uint_ge(nincs, 1);

	nfs4_client_expire(g_ss, nc);
}
END_TEST

/* Case 2: Same ownerid, same verifier, same addr → idempotent retry. */
START_TEST(test_alloc_idempotent_retry)
{
	char owner_buf[32];
	client_owner4 owner;
	verifier4 v;
	struct sockaddr_in sin;
	struct nfs_impl_id4 impl = make_impl_id();

	make_owner(&owner, "idempotent-owner", owner_buf, sizeof(owner_buf));
	make_verifier(&v, 0x22);
	make_sin(&sin, 0x7f000001, 2049);

	struct nfs4_client *nc1 =
		nfs4_client_alloc_or_find(g_ss, &owner, &impl, &v, &sin);
	ck_assert_ptr_nonnull(nc1);

	struct nfs4_client *nc2 =
		nfs4_client_alloc_or_find(g_ss, &owner, &impl, &v, &sin);
	ck_assert_ptr_nonnull(nc2);
	ck_assert_ptr_eq(nc1, nc2);

	/* Only one identity record written (clients file is append-only). */
	struct client_identity_record *ids;
	uint32_t nids;
	ck_assert_int_eq(load_all_ids(g_ss->ss_state_dir, &ids, &nids), 0);
	ck_assert_uint_eq(nids, 1);
	free(ids);

	nfs4_client_put(nc2);
	nfs4_client_expire(g_ss, nc1);
}
END_TEST

/* Case 3: Same ownerid, same verifier, different addr → multi-homed. */
START_TEST(test_alloc_multihomed)
{
	char owner_buf[32];
	client_owner4 owner;
	verifier4 v;
	struct sockaddr_in sin1, sin2;
	struct nfs_impl_id4 impl = make_impl_id();

	make_owner(&owner, "multihomed-owner", owner_buf, sizeof(owner_buf));
	make_verifier(&v, 0x33);
	make_sin(&sin1, 0xc0a80001, 2049);
	make_sin(&sin2, 0xc0a80002, 2049);

	struct nfs4_client *nc1 =
		nfs4_client_alloc_or_find(g_ss, &owner, &impl, &v, &sin1);
	ck_assert_ptr_nonnull(nc1);

	struct nfs4_client *nc2 =
		nfs4_client_alloc_or_find(g_ss, &owner, &impl, &v, &sin2);
	ck_assert_ptr_nonnull(nc2);
	ck_assert_ptr_eq(nc1, nc2);

	nfs4_client_put(nc2);
	nfs4_client_expire(g_ss, nc1);
}
END_TEST

/*
 * Case 4: Same ownerid, different verifier, same addr → client restarted.
 * Incarnation must increment; slot is stable; only one slot in incarnations.
 */
START_TEST(test_alloc_client_restart)
{
	char owner_buf[32];
	client_owner4 owner;
	verifier4 v1, v2;
	struct sockaddr_in sin;
	struct nfs_impl_id4 impl = make_impl_id();

	make_owner(&owner, "restart-owner", owner_buf, sizeof(owner_buf));
	make_verifier(&v1, 0xAA);
	make_verifier(&v2, 0xBB);
	make_sin(&sin, 0x7f000001, 2049);

	struct nfs4_client *nc1 =
		nfs4_client_alloc_or_find(g_ss, &owner, &impl, &v1, &sin);
	ck_assert_ptr_nonnull(nc1);

	clientid4 clid1 = (clientid4)nfs4_client_to_client(nc1)->c_id;
	uint32_t slot1 = clientid_slot(clid1);

	nfs4_client_put(nc1);

	struct nfs4_client *nc2 =
		nfs4_client_alloc_or_find(g_ss, &owner, &impl, &v2, &sin);
	ck_assert_ptr_nonnull(nc2);

	clientid4 clid2 = (clientid4)nfs4_client_to_client(nc2)->c_id;

	ck_assert_uint_eq(slot1, clientid_slot(clid2));
	ck_assert_uint_eq(clientid_incarnation(clid2), 1);

	/* Only one entry in the incarnations file after restart. */
	struct client_incarnation_record incs[TEST_MAX_INCS];
	size_t nincs = 0;
	ck_assert_int_eq(client_incarnation_load(g_ss->ss_state_dir, incs,
						 TEST_MAX_INCS, &nincs),
			 0);
	ck_assert_uint_eq(nincs, 1);
	ck_assert_uint_eq(incs[0].crc_incarnation, 1);

	nfs4_client_expire(g_ss, nc2);
}
END_TEST

/*
 * Case 5: Same ownerid, different verifier, different addr →
 * misconfiguration; alloc_or_find returns NULL (NFS4ERR_CLID_INUSE path).
 */
START_TEST(test_alloc_clid_inuse)
{
	char owner_buf[32];
	client_owner4 owner;
	verifier4 v1, v2;
	struct sockaddr_in sin1, sin2;
	struct nfs_impl_id4 impl = make_impl_id();

	make_owner(&owner, "collision-owner", owner_buf, sizeof(owner_buf));
	make_verifier(&v1, 0xAA);
	make_verifier(&v2, 0xCC);
	make_sin(&sin1, 0x0a000001, 2049);
	make_sin(&sin2, 0x0a000002, 2049);

	struct nfs4_client *nc1 =
		nfs4_client_alloc_or_find(g_ss, &owner, &impl, &v1, &sin1);
	ck_assert_ptr_nonnull(nc1);

	/*
	 * The collision path drops the ref returned by
	 * nfs4_client_find_by_owner() and returns NULL.  nc1's own
	 * ref (held since the first alloc_or_find) is unaffected.
	 */
	struct nfs4_client *nc2 =
		nfs4_client_alloc_or_find(g_ss, &owner, &impl, &v2, &sin2);
	ck_assert_ptr_null(nc2);

	/* nc1 must still be findable via disk. */
	struct nfs4_client *found = nfs4_client_find_by_owner(
		g_ss->ss_state_dir, server_boot_seq(g_ss), &owner, NULL);
	ck_assert_ptr_nonnull(found);
	nfs4_client_put(found);

	nfs4_client_expire(g_ss, nc1);
}
END_TEST

/* ------------------------------------------------------------------ */
/* nfs4_client_expire ordering                                         */
/* ------------------------------------------------------------------ */

START_TEST(test_expire_incarnation_before_put)
{
	char owner_buf[32];
	client_owner4 owner;
	verifier4 v;
	struct sockaddr_in sin;
	struct nfs_impl_id4 impl = make_impl_id();

	make_owner(&owner, "expire-order-owner", owner_buf, sizeof(owner_buf));
	make_verifier(&v, 0xEE);
	make_sin(&sin, 0x7f000001, 2049);

	struct nfs4_client *nc =
		nfs4_client_alloc_or_find(g_ss, &owner, &impl, &v, &sin);
	ck_assert_ptr_nonnull(nc);

	uint32_t slot =
		clientid_slot((clientid4)nfs4_client_to_client(nc)->c_id);

	nfs4_client_expire(g_ss, nc);

	/* Slot must be gone from the incarnations file. */
	struct client_incarnation_record incs[TEST_MAX_INCS];
	size_t nincs = 0;
	ck_assert_int_eq(client_incarnation_load(g_ss->ss_state_dir, incs,
						 TEST_MAX_INCS, &nincs),
			 0);
	bool found = false;
	for (size_t i = 0; i < nincs; i++) {
		if (incs[i].crc_slot == slot)
			found = true;
	}
	ck_assert(!found);

	/* find_by_owner returns NULL (slot absent from incarnations). */
	struct nfs4_client *ghost = nfs4_client_find_by_owner(
		g_ss->ss_state_dir, server_boot_seq(g_ss), &owner, NULL);
	ck_assert_ptr_null(ghost);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Two distinct clients coexist                                        */
/* ------------------------------------------------------------------ */

START_TEST(test_two_clients_coexist)
{
	char buf1[32], buf2[32];
	client_owner4 owner1, owner2;
	verifier4 v;
	struct sockaddr_in sin;
	struct nfs_impl_id4 impl = make_impl_id();

	make_owner(&owner1, "client-alpha", buf1, sizeof(buf1));
	make_owner(&owner2, "client-beta", buf2, sizeof(buf2));
	make_verifier(&v, 0x55);
	make_sin(&sin, 0x7f000001, 2049);

	struct nfs4_client *nc1 =
		nfs4_client_alloc_or_find(g_ss, &owner1, &impl, &v, &sin);
	struct nfs4_client *nc2 =
		nfs4_client_alloc_or_find(g_ss, &owner2, &impl, &v, &sin);
	ck_assert_ptr_nonnull(nc1);
	ck_assert_ptr_nonnull(nc2);
	ck_assert_ptr_ne(nc1, nc2);

	struct nfs4_client *f1 = nfs4_client_find_by_owner(
		g_ss->ss_state_dir, server_boot_seq(g_ss), &owner1, NULL);
	struct nfs4_client *f2 = nfs4_client_find_by_owner(
		g_ss->ss_state_dir, server_boot_seq(g_ss), &owner2, NULL);

	ck_assert_ptr_eq(f1, nc1);
	ck_assert_ptr_eq(f2, nc2);

	nfs4_client_put(f1);
	nfs4_client_put(f2);
	nfs4_client_expire(g_ss, nc1);
	nfs4_client_expire(g_ss, nc2);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite assembly                                                      */
/* ------------------------------------------------------------------ */

Suite *nfs4_client_persist_suite(void)
{
	Suite *s = suite_create("nfs4_client_persist");
	TCase *tc;

	tc = tcase_create("identity_persistence");
	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_identity_append_load_roundtrip);
	tcase_add_test(tc, test_identity_append_multiple);
	tcase_add_test(tc, test_identity_domain_name_on_disk_not_in_memory);
	suite_add_tcase(s, tc);

	tc = tcase_create("incarnation_persistence");
	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_incarnation_add_remove);
	tcase_add_test(tc, test_incarnation_symlink_swap_atomicity);
	suite_add_tcase(s, tc);

	tc = tcase_create("find_by_owner");
	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_find_by_owner_not_found);
	tcase_add_test(tc, test_find_by_owner_found);
	tcase_add_test(tc, test_find_by_owner_after_expire);
	suite_add_tcase(s, tc);

	tc = tcase_create("alloc_or_find_decision_tree");
	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_alloc_new_client);
	tcase_add_test(tc, test_alloc_idempotent_retry);
	tcase_add_test(tc, test_alloc_multihomed);
	tcase_add_test(tc, test_alloc_client_restart);
	tcase_add_test(tc, test_alloc_clid_inuse);
	suite_add_tcase(s, tc);

	tc = tcase_create("correctness");
	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_expire_incarnation_before_put);
	tcase_add_test(tc, test_two_clients_coexist);
	suite_add_tcase(s, tc);

	return s;
}

int main(void)
{
	return nfs4_test_run(nfs4_client_persist_suite());
}
