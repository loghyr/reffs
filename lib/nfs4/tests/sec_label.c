/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * sec_label.c — security label (RFC 7861) round-trip tests.
 *
 * Verifies:
 *  - New inodes have no security label (no server-generated default)
 *  - A client-provided label is stored and retrieved intact
 *  - XDR encode/decode of sec_label4 round-trips correctly
 *  - Zero-length label (empty) encodes without error
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <check.h>
#include <rpc/xdr.h>

#include "reffs/super_block.h"
#include "reffs/inode.h"
#include "nfs4/attr.h"
#include "nfs4_test_harness.h"

static struct super_block *test_sb;
static struct inode *test_inode;

static void sec_label_setup(void)
{
	nfs4_test_setup();

	test_sb = super_block_find(SUPER_BLOCK_ROOT_ID);
	ck_assert_ptr_nonnull(test_sb);

	test_inode = inode_alloc(test_sb, 0);
	ck_assert_ptr_nonnull(test_inode);
	test_inode->i_mode = S_IFREG | 0644;
}

static void sec_label_teardown(void)
{
	if (test_inode) {
		inode_active_put(test_inode);
		test_inode = NULL;
	}
	if (test_sb) {
		super_block_put(test_sb);
		test_sb = NULL;
	}

	nfs4_test_teardown();
}

/* ------------------------------------------------------------------ */

/*
 * A newly created inode must have no security label.  The server must
 * not generate a default — the label is policy-dependent and only the
 * client (with its local SELinux policy) knows what it should be.
 */
START_TEST(test_new_inode_has_no_label)
{
	ck_assert_uint_eq(test_inode->i_sec_label_len, 0);
	ck_assert_uint_eq(test_inode->i_sec_label_lfs, 0);
	ck_assert_uint_eq(test_inode->i_sec_label_pi, 0);
}
END_TEST

/*
 * Store a label on an inode and read it back.  This is the core
 * contract: whatever the client sends via SETATTR, the server stores
 * and returns unchanged.
 */
START_TEST(test_set_and_get_label)
{
	/* Linux uses lfs=0 (reserved/policy-mux) for SELinux */
	const uint32_t lfs = 0;
	const uint32_t pi = 0;
	const char *label = "system_u:object_r:nfs_t:s0";
	uint16_t len = (uint16_t)strlen(label);

	test_inode->i_sec_label_lfs = lfs;
	test_inode->i_sec_label_pi = pi;
	test_inode->i_sec_label_len = len;
	memcpy(test_inode->i_sec_label, label, len);

	ck_assert_uint_eq(test_inode->i_sec_label_lfs, lfs);
	ck_assert_uint_eq(test_inode->i_sec_label_pi, pi);
	ck_assert_uint_eq(test_inode->i_sec_label_len, len);
	ck_assert_int_eq(memcmp(test_inode->i_sec_label, label, len), 0);
}
END_TEST

/*
 * XDR round-trip: encode a sec_label4 with a real label, decode it,
 * and verify all fields match.  This catches any XDR mis-encoding
 * that would cause the Linux client to fail with EIO.
 */
START_TEST(test_xdr_roundtrip_with_label)
{
	const char *label = "system_u:object_r:nfs_t:s0";
	size_t label_len = strlen(label);
	char buf[512];
	XDR xenc, xdec;
	fattr4_sec_label original, decoded;

	memset(&original, 0, sizeof(original));
	memset(&decoded, 0, sizeof(decoded));

	/* Linux uses lfs=0 for SELinux */
	original.slai_lfs.lfs_lfs = 0;
	original.slai_lfs.lfs_pi = 0;
	original.slai_data.slai_data_len = label_len;
	original.slai_data.slai_data_val = (char *)label;

	/* Encode */
	xdrmem_create(&xenc, buf, sizeof(buf), XDR_ENCODE);
	ck_assert(xdr_fattr4_sec_label(&xenc, &original));
	u_int encoded_len = xdr_getpos(&xenc);
	xdr_destroy(&xenc);

	ck_assert_uint_gt(encoded_len, 0);

	/* Decode */
	xdrmem_create(&xdec, buf, encoded_len, XDR_DECODE);
	ck_assert(xdr_fattr4_sec_label(&xdec, &decoded));
	xdr_destroy(&xdec);

	/* Verify all fields match */
	ck_assert_uint_eq(decoded.slai_lfs.lfs_lfs, 0);
	ck_assert_uint_eq(decoded.slai_lfs.lfs_pi, 0);
	ck_assert_uint_eq(decoded.slai_data.slai_data_len, label_len);
	ck_assert_ptr_nonnull(decoded.slai_data.slai_data_val);
	ck_assert_int_eq(
		memcmp(decoded.slai_data.slai_data_val, label, label_len), 0);

	/* Clean up XDR-allocated memory */
	xdrmem_create(&xdec, buf, 0, XDR_FREE);
	xdr_fattr4_sec_label(&xdec, &decoded);
	xdr_destroy(&xdec);
}
END_TEST

/*
 * XDR round-trip for a zero-length label (no label set).  The server
 * must be able to encode and the client to decode an empty sec_label4
 * without error.  This is what every fresh inode returns.
 */
START_TEST(test_xdr_roundtrip_empty_label)
{
	char buf[128];
	XDR xenc, xdec;
	fattr4_sec_label original, decoded;

	memset(&original, 0, sizeof(original));
	memset(&decoded, 0, sizeof(decoded));

	/* All zeros: lfs=0, pi=0, data_len=0, data_val=NULL */

	/* Encode */
	xdrmem_create(&xenc, buf, sizeof(buf), XDR_ENCODE);
	ck_assert(xdr_fattr4_sec_label(&xenc, &original));
	u_int encoded_len = xdr_getpos(&xenc);
	xdr_destroy(&xenc);

	ck_assert_uint_gt(encoded_len, 0);

	/* Decode */
	xdrmem_create(&xdec, buf, encoded_len, XDR_DECODE);
	ck_assert(xdr_fattr4_sec_label(&xdec, &decoded));
	xdr_destroy(&xdec);

	ck_assert_uint_eq(decoded.slai_lfs.lfs_lfs, 0);
	ck_assert_uint_eq(decoded.slai_lfs.lfs_pi, 0);
	ck_assert_uint_eq(decoded.slai_data.slai_data_len, 0);

	xdrmem_create(&xdec, buf, 0, XDR_FREE);
	xdr_fattr4_sec_label(&xdec, &decoded);
	xdr_destroy(&xdec);
}
END_TEST

/* ------------------------------------------------------------------ */

Suite *sec_label_suite(void)
{
	Suite *s = suite_create("nfs4: sec_label");
	TCase *tc = tcase_create("Core");

	tcase_add_checked_fixture(tc, sec_label_setup, sec_label_teardown);
	tcase_add_test(tc, test_new_inode_has_no_label);
	tcase_add_test(tc, test_set_and_get_label);
	tcase_add_test(tc, test_xdr_roundtrip_with_label);
	tcase_add_test(tc, test_xdr_roundtrip_empty_label);
	suite_add_tcase(s, tc);

	return s;
}

int main(void)
{
	return nfs4_test_run(sec_label_suite());
}
