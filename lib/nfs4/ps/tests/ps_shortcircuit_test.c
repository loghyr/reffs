/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * Unit coverage for ps_shortcircuit_read / ps_shortcircuit_write.
 *
 * These are the helpers the Phase 5 dispatch hook routes to when a
 * mirror's deviceinfo resolves to one of the PS's own bound
 * addresses (em_local == true).  The helper decodes the upstream
 * filehandle (wire format struct network_file_handle), resolves the
 * local DS sb + inode, and drives the sb's data backend directly --
 * no loopback RPC.
 *
 * The tests live in lib/nfs4/ps/tests/ because the helper depends on
 * super_block_find / inode_find / db_read / db_write (the full
 * lib/fs dep graph).  This matches the ps_sb_alloc_test pattern.
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <check.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>

#include "reffs/errno.h"
#include "reffs/filehandle.h"
#include "reffs/fs.h"
#include "reffs/super_block.h"

#include "nfs4/trust_stateid.h"
#include "nfsv42_xdr.h"

#include "ps_shortcircuit.h"
#include "ps_state.h"

#include "fs_test_harness.h"

/*
 * Synthetic uid/gid the tests chown the runway-equivalent file to,
 * matching the MDS fence range default (1024-2048).  Keeping the
 * value distinct from both 0 (root) and 65534 (nobody) lets the
 * slice-5.3 cred-check cases exercise their intended paths without
 * the fixture aliasing onto either edge.
 */
#define SC_TEST_UID 1024
#define SC_TEST_GID 1024

static void sc_setup(void)
{
	fs_test_setup();
	ck_assert_int_eq(ps_state_init(), 0);
	/*
	 * Slice 5.4: the cred / stateid check helpers in
	 * ps_shortcircuit.c call trust_stateid_find() when the dispatch
	 * passes a non-NULL stateid.  The table must exist or
	 * trust_stateid_find returns NULL on every lookup; the empty-
	 * table path is correct (all non-anonymous stateids reject) but
	 * the trust-table tests need to register an entry, which
	 * requires the table to be initialized.
	 */
	ck_assert_int_eq(trust_stateid_init(), 0);
	/*
	 * The test exercises the helpers directly (no dispatch hook
	 * indirection), so ps_shortcircuit_install on a registered
	 * pls is not strictly necessary here.  We do not register a
	 * listener in this fixture because all tests operate against
	 * the root sb -- the helper only needs sb_id + ino, not a
	 * listener context.
	 */
}

static void sc_teardown(void)
{
	trust_stateid_fini();
	ps_state_fini();
	fs_test_teardown();
}

/*
 * Build a stateid4 with a deterministic non-zero `other` so the
 * trust-table lookup has something stable to hash on.  The seqid
 * is fixed -- trust_stateid_find keys only on other[], so seqid
 * variation here is irrelevant.
 */
static stateid4 make_test_stateid(uint8_t tag)
{
	stateid4 s = { 0 };

	s.seqid = 1;
	for (int i = 0; i < NFS4_OTHER_SIZE; i++)
		s.other[i] = (char)(tag + i);
	return s;
}

/*
 * Build a wire-format FH pointing at the given (sb_id, ino).  The
 * caller passes the buffer; the helper writes the 24-byte packed
 * struct.  Returns the buffer length for ergonomics.
 */
static uint32_t build_fh(uint8_t *out, uint64_t sb_id, uint64_t ino)
{
	struct network_file_handle nfh;

	memset(&nfh, 0, sizeof(nfh));
	nfh.nfh_vers = FILEHANDLE_VERSION_CURR;
	nfh.nfh_sb = sb_id;
	nfh.nfh_ino = ino;
	memcpy(out, &nfh, sizeof(nfh));
	return (uint32_t)sizeof(nfh);
}

/*
 * Create a regular file at `path` inside the root sb, chown it to
 * the slice-5.3 synthetic uid/gid (so the cred-check path has a
 * deterministic target), and return its inode number.  Recurring
 * through the public reffs_fs_* surface keeps the fixture aligned
 * with the path the NFS handlers actually exercise; the chown is
 * the test stand-in for the MDS runway-pop fence that would set
 * the synthetic owner on a real DS file.
 */
static uint64_t make_test_file(const char *path)
{
	struct stat st;

	ck_assert_int_eq(reffs_fs_create(path, 0644), 0);
	ck_assert_int_eq(reffs_fs_chown(path, SC_TEST_UID, SC_TEST_GID), 0);
	ck_assert_int_eq(reffs_fs_getattr(path, &st), 0);
	return (uint64_t)st.st_ino;
}

/*
 * Write-then-read roundtrip through the short-circuit helpers
 * against a real local DS sb inode.  This is the load-bearing
 * dispatch test: when slice 5.2's em_local plumbing routes to the
 * helper, the bytes must land in the local inode's data block and
 * come back unchanged on the next call.  No RPC anywhere.
 */
START_TEST(test_shortcircuit_roundtrip)
{
	uint64_t ino = make_test_file("/sc_roundtrip");
	uint8_t fh[64];
	uint32_t fh_len = build_fh(fh, SUPER_BLOCK_ROOT_ID, ino);
	const char *payload = "shortcircuit-payload-0123456789";
	size_t plen = strlen(payload);
	uint8_t read_buf[64] = { 0 };
	uint32_t nread = 0;

	ck_assert_int_eq(ps_shortcircuit_write(fh, fh_len, 0,
					       (const uint8_t *)payload, plen,
					       SC_TEST_UID, SC_TEST_GID, NULL),
			 0);
	ck_assert_int_eq(ps_shortcircuit_read(fh, fh_len, 0, sizeof(read_buf),
					      read_buf, &nread, SC_TEST_UID,
					      SC_TEST_GID, NULL),
			 0);
	ck_assert_uint_eq(nread, plen);
	ck_assert_mem_eq(read_buf, payload, plen);
}
END_TEST

/*
 * Two distinct writes at non-zero offsets land independently and
 * read back in place.  The dispatch hook is per-mirror per-call;
 * within one mirror the helpers must respect block_offset exactly
 * like the RPC path would.
 */
START_TEST(test_shortcircuit_offset_addressing)
{
	uint64_t ino = make_test_file("/sc_offset");
	uint8_t fh[64];
	uint32_t fh_len = build_fh(fh, SUPER_BLOCK_ROOT_ID, ino);
	uint8_t writeA[16];
	uint8_t writeB[16];

	memset(writeA, 0xAA, sizeof(writeA));
	memset(writeB, 0xBB, sizeof(writeB));

	ck_assert_int_eq(ps_shortcircuit_write(fh, fh_len, 0, writeA,
					       sizeof(writeA), SC_TEST_UID,
					       SC_TEST_GID, NULL),
			 0);
	ck_assert_int_eq(ps_shortcircuit_write(fh, fh_len, 4096, writeB,
					       sizeof(writeB), SC_TEST_UID,
					       SC_TEST_GID, NULL),
			 0);

	uint8_t readA[16] = { 0 };
	uint8_t readB[16] = { 0 };
	uint32_t na = 0, nb = 0;

	ck_assert_int_eq(ps_shortcircuit_read(fh, fh_len, 0, sizeof(readA),
					      readA, &na, SC_TEST_UID,
					      SC_TEST_GID, NULL),
			 0);
	ck_assert_uint_eq(na, sizeof(writeA));
	ck_assert_mem_eq(readA, writeA, sizeof(writeA));

	ck_assert_int_eq(ps_shortcircuit_read(fh, fh_len, 4096, sizeof(readB),
					      readB, &nb, SC_TEST_UID,
					      SC_TEST_GID, NULL),
			 0);
	ck_assert_uint_eq(nb, sizeof(writeB));
	ck_assert_mem_eq(readB, writeB, sizeof(writeB));
}
END_TEST

/*
 * An FH whose nfh_sb names a sb that does not exist must fail
 * cleanly -- the helper is on the per-mirror fanout hot path, so a
 * stale FH (e.g. the layout points at a sb that was destroyed
 * between LAYOUTGET and the I/O) MUST surface as -ESTALE rather
 * than NULL-deref or generic -EIO.  -ESTALE matches what the RPC
 * path would propagate from NFS4ERR_STALE.
 */
START_TEST(test_shortcircuit_stale_sb)
{
	uint8_t fh[64];
	uint32_t fh_len = build_fh(fh, 0xdeadbeefULL, 0x12345ULL);
	uint8_t buf[8] = { 0 };
	uint32_t nread = 0;

	ck_assert_int_eq(ps_shortcircuit_write(fh, fh_len, 0, buf, sizeof(buf),
					       SC_TEST_UID, SC_TEST_GID, NULL),
			 -ESTALE);
	ck_assert_int_eq(ps_shortcircuit_read(fh, fh_len, 0, sizeof(buf), buf,
					      &nread, SC_TEST_UID, SC_TEST_GID,
					      NULL),
			 -ESTALE);
}
END_TEST

/*
 * An FH whose nfh_sb matches the local DS sb but whose nfh_ino
 * names an inode that never existed must also fail cleanly with
 * -ESTALE.  This catches the case where the MDS runway file was
 * GC'd after the layout was issued but before the short-circuit
 * fires.
 */
START_TEST(test_shortcircuit_stale_ino)
{
	uint8_t fh[64];
	uint32_t fh_len = build_fh(fh, SUPER_BLOCK_ROOT_ID, 0xdeadbeefULL);
	uint8_t buf[8] = { 0 };
	uint32_t nread = 0;

	ck_assert_int_eq(ps_shortcircuit_write(fh, fh_len, 0, buf, sizeof(buf),
					       SC_TEST_UID, SC_TEST_GID, NULL),
			 -ESTALE);
	ck_assert_int_eq(ps_shortcircuit_read(fh, fh_len, 0, sizeof(buf), buf,
					      &nread, SC_TEST_UID, SC_TEST_GID,
					      NULL),
			 -ESTALE);
}
END_TEST

/*
 * Malformed FH lengths and unrecognised versions must be rejected
 * before any sb / inode lookup.  Wire data is untrusted -- the
 * layout body comes from the MDS via LAYOUTGET, but a corrupted
 * upstream session could deliver a short or version-mismatched FH,
 * and the helper must not dereference past the buffer.
 */
START_TEST(test_shortcircuit_bad_fh)
{
	uint8_t fh[64];
	uint32_t fh_len = build_fh(fh, SUPER_BLOCK_ROOT_ID, 1);
	uint8_t buf[8] = { 0 };
	uint32_t nread = 0;

	/* NULL fh */
	ck_assert_int_eq(ps_shortcircuit_write(NULL, fh_len, 0, buf,
					       sizeof(buf), SC_TEST_UID,
					       SC_TEST_GID, NULL),
			 -EINVAL);

	/* fh_len too small to contain the wire struct */
	ck_assert_int_eq(ps_shortcircuit_write(fh, 4, 0, buf, sizeof(buf),
					       SC_TEST_UID, SC_TEST_GID, NULL),
			 -EINVAL);
	ck_assert_int_eq(ps_shortcircuit_read(fh, 4, 0, sizeof(buf), buf,
					      &nread, SC_TEST_UID, SC_TEST_GID,
					      NULL),
			 -EINVAL);

	/*
	 * Mismatched version field.  Wire format may evolve later; for
	 * now any version != FILEHANDLE_VERSION_CURR is rejected as
	 * malformed rather than silently coerced.
	 */
	uint8_t bad_vers_fh[24];

	memcpy(bad_vers_fh, fh, sizeof(bad_vers_fh));
	bad_vers_fh[0] = 0xff; /* clobber nfh_vers low byte */
	bad_vers_fh[1] = 0xff;
	ck_assert_int_eq(ps_shortcircuit_write(bad_vers_fh, sizeof(bad_vers_fh),
					       0, buf, sizeof(buf), SC_TEST_UID,
					       SC_TEST_GID, NULL),
			 -EINVAL);
}
END_TEST

/*
 * Slice 5.3: a forwarded uid + gid that does NOT match the local
 * file's synthetic uid/gid must reject before any data block I/O.
 * The MDS chmod'd the runway file with SC_TEST_UID/SC_TEST_GID; a
 * client whose layout cred carries different values is presenting
 * a credential the RPC path's AUTH_SYS check would refuse, and the
 * short path must refuse too -- otherwise short-circuit becomes a
 * blanket ACL override.  Verify both write and read return -EACCES
 * and that no bytes land in the inode (read after the rejected
 * write returns nread == 0).
 */
START_TEST(test_shortcircuit_reject_wrong_uid)
{
	uint64_t ino = make_test_file("/sc_reject");
	uint8_t fh[64];
	uint32_t fh_len = build_fh(fh, SUPER_BLOCK_ROOT_ID, ino);
	const uint8_t payload[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	uint8_t read_buf[16] = { 0 };
	uint32_t nread = 0;

	/* Wrong uid AND wrong gid -- neither matches the stored pair. */
	ck_assert_int_eq(ps_shortcircuit_write(fh, fh_len, 0, payload,
					       sizeof(payload), SC_TEST_UID + 1,
					       SC_TEST_GID + 1, NULL),
			 -EACCES);
	ck_assert_int_eq(ps_shortcircuit_read(fh, fh_len, 0, sizeof(read_buf),
					      read_buf, &nread, SC_TEST_UID + 1,
					      SC_TEST_GID + 1, NULL),
			 -EACCES);

	/*
	 * A subsequent matching-cred read must observe an unwritten
	 * data block -- the rejected write did not modify state.
	 */
	ck_assert_int_eq(ps_shortcircuit_read(fh, fh_len, 0, sizeof(read_buf),
					      read_buf, &nread, SC_TEST_UID,
					      SC_TEST_GID, NULL),
			 0);
	ck_assert_uint_eq(nread, 0);
}
END_TEST

/*
 * Slice 5.3: forwarded uid matches stored synthetic uid -- write
 * proceeds and the bytes land in the inode.  The complementary
 * accept case to test_shortcircuit_reject_wrong_uid; together they
 * prove the cred-check is enforcement, not theatre.
 */
START_TEST(test_shortcircuit_accept_matching_uid)
{
	uint64_t ino = make_test_file("/sc_accept");
	uint8_t fh[64];
	uint32_t fh_len = build_fh(fh, SUPER_BLOCK_ROOT_ID, ino);
	const uint8_t payload[] = "accept-write";
	uint8_t read_buf[16] = { 0 };
	uint32_t nread = 0;

	ck_assert_int_eq(ps_shortcircuit_write(fh, fh_len, 0, payload,
					       sizeof(payload), SC_TEST_UID,
					       SC_TEST_GID, NULL),
			 0);
	ck_assert_int_eq(ps_shortcircuit_read(fh, fh_len, 0, sizeof(read_buf),
					      read_buf, &nread, SC_TEST_UID,
					      SC_TEST_GID, NULL),
			 0);
	ck_assert_uint_eq(nread, sizeof(payload));
	ck_assert_mem_eq(read_buf, payload, sizeof(payload));
}
END_TEST

/*
 * Slice 5.3: root_squash semantics.  When the upstream RPC layer
 * squashes a client uid=0 down to nobody (65534), the squashed
 * credential is what reaches the short-circuit dispatch hook.
 * Squashing happens before this layer, so the helper sees uid=65534
 * vs the file's stored synthetic uid -- which must mismatch and
 * reject.  This test pins the contract that the cred check honours
 * whatever transformation the RPC layer applied; the short path
 * never re-elevates a squashed credential.
 */
START_TEST(test_shortcircuit_root_squash)
{
	uint64_t ino = make_test_file("/sc_squash");
	uint8_t fh[64];
	uint32_t fh_len = build_fh(fh, SUPER_BLOCK_ROOT_ID, ino);
	const uint8_t payload[4] = { 9, 9, 9, 9 };
	const uint32_t nobody_uid = 65534;
	const uint32_t nobody_gid = 65534;

	ck_assert_int_eq(ps_shortcircuit_write(fh, fh_len, 0, payload,
					       sizeof(payload), nobody_uid,
					       nobody_gid, NULL),
			 -EACCES);
}
END_TEST

/*
 * Slice 5.4: NULL layout stateid is the "anonymous stateid"
 * shortcut.  The dispatch hook in ec_pipeline.c passes NULL when
 * em_tight_coupled is false (the mirror's GETDEVICEINFO did not
 * advertise tight coupling), and the helper MUST accept without
 * consulting the trust table.  Today's compat path: the table
 * starts empty; without this bypass, every short-circuit would
 * reject -EBADSTATEID once Phase 1 trust-stateid is wired in.
 */
START_TEST(test_shortcircuit_null_stateid_skips_trust_check)
{
	uint64_t ino = make_test_file("/sc_nullstid");
	uint8_t fh[64];
	uint32_t fh_len = build_fh(fh, SUPER_BLOCK_ROOT_ID, ino);
	const uint8_t payload[8] = { 0xA, 0xB, 0xC, 0xD, 0xE, 0xF, 0, 1 };
	uint8_t read_buf[16] = { 0 };
	uint32_t nread = 0;

	/*
	 * NULL stid even with an EMPTY trust table -- the bypass
	 * fires before any lookup.  No registration in this test.
	 */
	ck_assert_int_eq(ps_shortcircuit_write(fh, fh_len, 0, payload,
					       sizeof(payload), SC_TEST_UID,
					       SC_TEST_GID, NULL),
			 0);
	ck_assert_int_eq(ps_shortcircuit_read(fh, fh_len, 0, sizeof(read_buf),
					      read_buf, &nread, SC_TEST_UID,
					      SC_TEST_GID, NULL),
			 0);
	ck_assert_uint_eq(nread, sizeof(payload));
	ck_assert_mem_eq(read_buf, payload, sizeof(payload));
}
END_TEST

/*
 * Slice 5.4: forwarded layout stateid that IS registered in the
 * trust table (TRUST_ACTIVE, unexpired) accepts.  This pins the
 * load-bearing case once tight coupling is enabled per-mirror --
 * the RPC path's nfs4_op_chunk_write would accept on the same
 * stateid, so the short path must too, otherwise tight-coupled
 * mirrors lose I/O the moment the dispatch hook fires.
 */
START_TEST(test_shortcircuit_trusted_stateid_accepted)
{
	uint64_t ino = make_test_file("/sc_trustedstid");
	uint8_t fh[64];
	uint32_t fh_len = build_fh(fh, SUPER_BLOCK_ROOT_ID, ino);
	stateid4 trusted = make_test_stateid(0x42);
	clientid4 cid = 0x1234567890ABCDEFULL;
	/* Lease deadline well in the future. */
	uint64_t expire_ns = UINT64_C(1) << 62;

	ck_assert_int_eq(trust_stateid_register(&trusted, ino, cid,
						LAYOUTIOMODE4_RW, expire_ns,
						""),
			 0);

	const uint8_t payload[] = "trust!";
	uint8_t read_buf[16] = { 0 };
	uint32_t nread = 0;

	ck_assert_int_eq(ps_shortcircuit_write(fh, fh_len, 0, payload,
					       sizeof(payload), SC_TEST_UID,
					       SC_TEST_GID, &trusted),
			 0);
	ck_assert_int_eq(ps_shortcircuit_read(fh, fh_len, 0, sizeof(read_buf),
					      read_buf, &nread, SC_TEST_UID,
					      SC_TEST_GID, &trusted),
			 0);
	ck_assert_uint_eq(nread, sizeof(payload));
	ck_assert_mem_eq(read_buf, payload, sizeof(payload));
}
END_TEST

/*
 * Slice 5.4: forwarded layout stateid that is NOT in the trust
 * table -- the RPC path rejects with NFS4ERR_BAD_STATEID, so the
 * short path returns -EBADSTATEID.  This is the canonical phase 5
 * test_shortcircuit_rejects_unknown_stateid case from
 * proxy-server.md: a forwarded stateid for a file the MDS never
 * registered (e.g., from a different client's lease, or a
 * revoked layout) must not bypass authorization.  Verify the
 * data block stays unwritten by reading back with NULL stid
 * afterward and observing nread == 0.
 */
START_TEST(test_shortcircuit_rejects_unknown_stateid)
{
	uint64_t ino = make_test_file("/sc_unknownstid");
	uint8_t fh[64];
	uint32_t fh_len = build_fh(fh, SUPER_BLOCK_ROOT_ID, ino);
	stateid4 unknown = make_test_stateid(0x88);
	const uint8_t payload[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
	uint8_t read_buf[16] = { 0 };
	uint32_t nread = 0;

	/* No trust_stateid_register call -- the table does not know `unknown`. */
	ck_assert_int_eq(ps_shortcircuit_write(fh, fh_len, 0, payload,
					       sizeof(payload), SC_TEST_UID,
					       SC_TEST_GID, &unknown),
			 -EBADSTATEID);
	ck_assert_int_eq(ps_shortcircuit_read(fh, fh_len, 0, sizeof(read_buf),
					      read_buf, &nread, SC_TEST_UID,
					      SC_TEST_GID, &unknown),
			 -EBADSTATEID);

	/* Unwritten data block: subsequent NULL-stid read sees nread == 0. */
	ck_assert_int_eq(ps_shortcircuit_read(fh, fh_len, 0, sizeof(read_buf),
					      read_buf, &nread, SC_TEST_UID,
					      SC_TEST_GID, NULL),
			 0);
	ck_assert_uint_eq(nread, 0);
}
END_TEST

/*
 * Slice 5.5: pls_shortcircuit_total bumps when the ec_pipeline
 * dispatch hook routes through the short-circuit path.  The bump
 * lives in `ps_listener_record_shortcircuit()`, an inline helper
 * the dispatch hook calls before invoking pls_sc_write_fn /
 * pls_sc_read_fn; calling the helper directly N times against a
 * freshly-registered pls verifies the counter atomic + the probe
 * surface's load both observe N.  The dispatch-hook call site is
 * exercised by the higher-level integration test
 * (test_shortcircuit_partial_2_mirrors, follow-up slice); this
 * test pins the counter primitive in isolation so a future
 * refactor that swaps the helper for a direct atomic call cannot
 * silently drop the probe-surface plumbing.
 */
#define SC_COUNTER_TEST_LISTENER_ID 31337
START_TEST(test_shortcircuit_counter_increments)
{
	struct reffs_proxy_mds_config cfg;

	memset(&cfg, 0, sizeof(cfg));
	cfg.id = SC_COUNTER_TEST_LISTENER_ID;
	cfg.port = 4098;
	cfg.mds_port = 2049;
	cfg.mds_probe = 20490;
	strncpy(cfg.address, "127.0.0.1", sizeof(cfg.address) - 1);
	ck_assert_int_eq(ps_state_register(&cfg), 0);

	struct ps_listener_state *pls =
		(struct ps_listener_state *)ps_state_find(
			SC_COUNTER_TEST_LISTENER_ID);

	ck_assert_ptr_nonnull(pls);
	/*
	 * Fresh listener: ps_state_register zero-inits the counter
	 * cluster.  The probe handler will surface this as 0.
	 */
	ck_assert_uint_eq(atomic_load_explicit(&pls->pls_shortcircuit_total,
					       memory_order_relaxed),
			  0);

	const unsigned int N = 17;

	for (unsigned int i = 0; i < N; i++)
		ps_listener_record_shortcircuit(pls);

	ck_assert_uint_eq(atomic_load_explicit(&pls->pls_shortcircuit_total,
					       memory_order_relaxed),
			  N);

	/*
	 * NULL pls is a no-op: the dispatch hook guards on pls != NULL
	 * before calling the helper, but the helper is also called
	 * from contexts where pls is reached via a function-pointer
	 * chain that may not have a valid listener (ec_demo, which
	 * never installs a pls).  Test that explicitly so a future
	 * refactor that hoists the helper above the dispatch guard
	 * does not segfault.
	 */
	ps_listener_record_shortcircuit(NULL);
	ck_assert_uint_eq(atomic_load_explicit(&pls->pls_shortcircuit_total,
					       memory_order_relaxed),
			  N);
}
END_TEST

static Suite *ps_shortcircuit_suite(void)
{
	Suite *s = suite_create("ps_shortcircuit");
	TCase *tc = tcase_create("shortcircuit");

	tcase_add_checked_fixture(tc, sc_setup, sc_teardown);
	tcase_add_test(tc, test_shortcircuit_roundtrip);
	tcase_add_test(tc, test_shortcircuit_offset_addressing);
	tcase_add_test(tc, test_shortcircuit_stale_sb);
	tcase_add_test(tc, test_shortcircuit_stale_ino);
	tcase_add_test(tc, test_shortcircuit_bad_fh);
	tcase_add_test(tc, test_shortcircuit_reject_wrong_uid);
	tcase_add_test(tc, test_shortcircuit_accept_matching_uid);
	tcase_add_test(tc, test_shortcircuit_root_squash);
	tcase_add_test(tc, test_shortcircuit_null_stateid_skips_trust_check);
	tcase_add_test(tc, test_shortcircuit_trusted_stateid_accepted);
	tcase_add_test(tc, test_shortcircuit_rejects_unknown_stateid);
	tcase_add_test(tc, test_shortcircuit_counter_increments);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	return fs_test_run(ps_shortcircuit_suite());
}
