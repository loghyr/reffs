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

#include "reffs/filehandle.h"
#include "reffs/fs.h"
#include "reffs/super_block.h"

#include "ps_shortcircuit.h"
#include "ps_state.h"

#include "fs_test_harness.h"

static void sc_setup(void)
{
	fs_test_setup();
	ck_assert_int_eq(ps_state_init(), 0);
	/*
	 * The test exercises the helpers directly (no dispatch hook
	 * indirection), so ps_shortcircuit_install on a registered
	 * pls is not strictly necessary here.  We do not register a
	 * listener in this fixture because all five tests operate
	 * against the root sb -- the helper only needs sb_id + ino,
	 * not a listener context.
	 */
}

static void sc_teardown(void)
{
	ps_state_fini();
	fs_test_teardown();
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
 * Create a regular file at `path` inside the root sb and return its
 * inode number via the standard stat(2) interface.  reffs_fs_create
 * lives at the public API surface; recurring through the same path
 * the NFS handlers use is the most representative test setup.
 */
static uint64_t make_test_file(const char *path)
{
	struct stat st;

	ck_assert_int_eq(reffs_fs_create(path, 0644), 0);
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
					       (const uint8_t *)payload, plen),
			 0);
	ck_assert_int_eq(ps_shortcircuit_read(fh, fh_len, 0, sizeof(read_buf),
					      read_buf, &nread),
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
					       sizeof(writeA)),
			 0);
	ck_assert_int_eq(ps_shortcircuit_write(fh, fh_len, 4096, writeB,
					       sizeof(writeB)),
			 0);

	uint8_t readA[16] = { 0 };
	uint8_t readB[16] = { 0 };
	uint32_t na = 0, nb = 0;

	ck_assert_int_eq(ps_shortcircuit_read(fh, fh_len, 0, sizeof(readA),
					      readA, &na),
			 0);
	ck_assert_uint_eq(na, sizeof(writeA));
	ck_assert_mem_eq(readA, writeA, sizeof(writeA));

	ck_assert_int_eq(ps_shortcircuit_read(fh, fh_len, 4096, sizeof(readB),
					      readB, &nb),
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

	ck_assert_int_eq(ps_shortcircuit_write(fh, fh_len, 0, buf, sizeof(buf)),
			 -ESTALE);
	ck_assert_int_eq(ps_shortcircuit_read(fh, fh_len, 0, sizeof(buf), buf,
					      &nread),
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

	ck_assert_int_eq(ps_shortcircuit_write(fh, fh_len, 0, buf, sizeof(buf)),
			 -ESTALE);
	ck_assert_int_eq(ps_shortcircuit_read(fh, fh_len, 0, sizeof(buf), buf,
					      &nread),
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
					       sizeof(buf)),
			 -EINVAL);

	/* fh_len too small to contain the wire struct */
	ck_assert_int_eq(ps_shortcircuit_write(fh, 4, 0, buf, sizeof(buf)),
			 -EINVAL);
	ck_assert_int_eq(ps_shortcircuit_read(fh, 4, 0, sizeof(buf), buf,
					      &nread),
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
					       0, buf, sizeof(buf)),
			 -EINVAL);
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
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	return fs_test_run(ps_shortcircuit_suite());
}
