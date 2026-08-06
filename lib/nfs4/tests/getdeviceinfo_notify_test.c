/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * getdeviceinfo_notify_test -- per-client device-notification
 * subscription table (RFC 8881 sec 18.40).
 *
 * Under test: nfs4_client_dev_notify_set / nfs4_client_dev_notify_mask
 * (nfs4/client.h).  GETDEVICEINFO records which notify_deviceid_type4
 * events a client wants for a deviceID; the CB_NOTIFY_DEVICEID sender
 * reads the table to address callbacks.
 *
 * Tests:
 *   A. no subscription -> mask 0
 *   B. register 0x6 -> lookup 0x6
 *   C. re-register 0x2 -> last bitmap wins, lookup 0x2
 *   D. register then clear with 0 -> removed, lookup 0
 *   E. clear a never-registered id -> no-op, no crash
 *   F. two devices per client -> independent masks
 *   F2. same device under two layout types -> independent rows
 *   G. two clients, same device -> independent rows; freeing one
 *      client leaves the other's row intact (UAF/leak under ASAN)
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <arpa/inet.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <check.h>
#include <urcu.h>

#include "nfsv42_xdr.h"
#include "nfs4/client.h"

#include "nfs4_test_harness.h"

#define CHANGE_BIT (1u << NOTIFY_DEVICEID4_CHANGE)
#define DELETE_BIT (1u << NOTIFY_DEVICEID4_DELETE)

static struct nfs4_client *alloc_client(uint32_t ip, clientid4 clid)
{
	verifier4 v;
	struct sockaddr_in sin;

	memset(&v, 0x5a, sizeof(v));
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(ip);
	sin.sin_port = htons(2049);

	return nfs4_client_alloc(&v, &sin, 1, clid, 0);
}

static struct nfs4_client *test_nc;

static void notify_setup(void)
{
	nfs4_test_setup();
	test_nc = alloc_client(0x7f000005, 0xC0DE0005);
	ck_assert_ptr_nonnull(test_nc);
}

static void notify_teardown(void)
{
	nfs4_client_put(test_nc);
	test_nc = NULL;
	nfs4_test_teardown();
}

START_TEST(test_no_subscription)
{
	ck_assert_uint_eq(
		nfs4_client_dev_notify_mask(test_nc, LAYOUT4_FLEX_FILES, 7), 0);
}
END_TEST

START_TEST(test_register_lookup)
{
	ck_assert_int_eq(nfs4_client_dev_notify_set(test_nc, LAYOUT4_FLEX_FILES,
						    7, CHANGE_BIT | DELETE_BIT),
			 0);
	ck_assert_uint_eq(nfs4_client_dev_notify_mask(test_nc,
						      LAYOUT4_FLEX_FILES, 7),
			  CHANGE_BIT | DELETE_BIT);
}
END_TEST

START_TEST(test_last_bitmap_wins)
{
	ck_assert_int_eq(nfs4_client_dev_notify_set(test_nc, LAYOUT4_FLEX_FILES,
						    7, CHANGE_BIT | DELETE_BIT),
			 0);
	ck_assert_int_eq(nfs4_client_dev_notify_set(test_nc, LAYOUT4_FLEX_FILES,
						    7, CHANGE_BIT),
			 0);
	ck_assert_uint_eq(nfs4_client_dev_notify_mask(test_nc,
						      LAYOUT4_FLEX_FILES, 7),
			  CHANGE_BIT);
}
END_TEST

START_TEST(test_empty_turns_off)
{
	ck_assert_int_eq(nfs4_client_dev_notify_set(test_nc, LAYOUT4_FLEX_FILES,
						    7, CHANGE_BIT | DELETE_BIT),
			 0);
	ck_assert_int_eq(nfs4_client_dev_notify_set(test_nc, LAYOUT4_FLEX_FILES,
						    7, 0),
			 0);
	ck_assert_uint_eq(
		nfs4_client_dev_notify_mask(test_nc, LAYOUT4_FLEX_FILES, 7), 0);
}
END_TEST

START_TEST(test_clear_unregistered)
{
	ck_assert_int_eq(nfs4_client_dev_notify_set(test_nc, LAYOUT4_FLEX_FILES,
						    9, 0),
			 0);
	ck_assert_uint_eq(
		nfs4_client_dev_notify_mask(test_nc, LAYOUT4_FLEX_FILES, 9), 0);
}
END_TEST

START_TEST(test_two_devices_independent)
{
	ck_assert_int_eq(nfs4_client_dev_notify_set(test_nc, LAYOUT4_FLEX_FILES,
						    1, CHANGE_BIT),
			 0);
	ck_assert_int_eq(nfs4_client_dev_notify_set(test_nc, LAYOUT4_FLEX_FILES,
						    2, DELETE_BIT),
			 0);
	ck_assert_uint_eq(nfs4_client_dev_notify_mask(test_nc,
						      LAYOUT4_FLEX_FILES, 1),
			  CHANGE_BIT);
	ck_assert_uint_eq(nfs4_client_dev_notify_mask(test_nc,
						      LAYOUT4_FLEX_FILES, 2),
			  DELETE_BIT);
	ck_assert_int_eq(nfs4_client_dev_notify_set(test_nc, LAYOUT4_FLEX_FILES,
						    1, 0),
			 0);
	ck_assert_uint_eq(
		nfs4_client_dev_notify_mask(test_nc, LAYOUT4_FLEX_FILES, 1), 0);
	ck_assert_uint_eq(nfs4_client_dev_notify_mask(test_nc,
						      LAYOUT4_FLEX_FILES, 2),
			  DELETE_BIT);
}
END_TEST

START_TEST(test_two_layout_types_independent)
{
	ck_assert_int_eq(nfs4_client_dev_notify_set(test_nc, LAYOUT4_FLEX_FILES,
						    5, CHANGE_BIT),
			 0);
	ck_assert_int_eq(nfs4_client_dev_notify_set(
				 test_nc, LAYOUT4_FLEX_FILES_V2, 5, DELETE_BIT),
			 0);
	ck_assert_uint_eq(nfs4_client_dev_notify_mask(test_nc,
						      LAYOUT4_FLEX_FILES, 5),
			  CHANGE_BIT);
	ck_assert_uint_eq(nfs4_client_dev_notify_mask(test_nc,
						      LAYOUT4_FLEX_FILES_V2, 5),
			  DELETE_BIT);
	ck_assert_int_eq(nfs4_client_dev_notify_set(test_nc, LAYOUT4_FLEX_FILES,
						    5, 0),
			 0);
	ck_assert_uint_eq(
		nfs4_client_dev_notify_mask(test_nc, LAYOUT4_FLEX_FILES, 5), 0);
	ck_assert_uint_eq(nfs4_client_dev_notify_mask(test_nc,
						      LAYOUT4_FLEX_FILES_V2, 5),
			  DELETE_BIT);
}
END_TEST

START_TEST(test_two_clients_independent)
{
	struct nfs4_client *other = alloc_client(0x7f000006, 0xC0DE0006);

	ck_assert_ptr_nonnull(other);
	ck_assert_int_eq(nfs4_client_dev_notify_set(test_nc, LAYOUT4_FLEX_FILES,
						    3, CHANGE_BIT | DELETE_BIT),
			 0);
	ck_assert_int_eq(nfs4_client_dev_notify_set(other, LAYOUT4_FLEX_FILES,
						    3, DELETE_BIT),
			 0);
	ck_assert_uint_eq(nfs4_client_dev_notify_mask(test_nc,
						      LAYOUT4_FLEX_FILES, 3),
			  CHANGE_BIT | DELETE_BIT);
	ck_assert_uint_eq(nfs4_client_dev_notify_mask(other, LAYOUT4_FLEX_FILES,
						      3),
			  DELETE_BIT);

	/* free `other` with rows still registered: its drain must not
	 * touch test_nc's row (ASAN flags a UAF/leak here if it does) */
	nfs4_client_put(other);
	rcu_barrier();
	ck_assert_uint_eq(nfs4_client_dev_notify_mask(test_nc,
						      LAYOUT4_FLEX_FILES, 3),
			  CHANGE_BIT | DELETE_BIT);
}
END_TEST

/* ------------------------------------------------------------------ */
/* ffv2_device_addr4 wire shape                                         */
/* ------------------------------------------------------------------ */

/*
 * A v2 layout's device address carries ffv2_device_addr4, whose
 * per-version entry replaces v1's bool ffdv_tightly_coupled with the
 * ffv2dv_coupling bitmask.  The metadata server sets
 * FFV2_COUPLING_TRUSTED_STATEID on a successful TRUST_STATEID probe,
 * and the client keys tight coupling off that bit, so the bit has to
 * survive the round trip.
 */
START_TEST(test_ffv2_device_addr_roundtrip)
{
	char uaddr[] = "192.168.1.1.8.1";
	char netid[] = "tcp";
	netaddr4 na = { .na_r_netid = netid, .na_r_addr = uaddr };
	ffv2_device_versions4 ver = {
		.ffv2dv_version = 4,
		.ffv2dv_minorversion = 2,
		.ffv2dv_rsize = 1048576,
		.ffv2dv_wsize = 1048576,
		.ffv2dv_coupling = FFV2_COUPLING_TRUSTED_STATEID,
	};
	ffv2_device_addr4 out;

	memset(&out, 0, sizeof(out));
	out.ffv2da_netaddrs.multipath_list4_len = 1;
	out.ffv2da_netaddrs.multipath_list4_val = &na;
	out.ffv2da_versions.ffv2da_versions_len = 1;
	out.ffv2da_versions.ffv2da_versions_val = &ver;

	u_long sz = xdr_sizeof((xdrproc_t)xdr_ffv2_device_addr4, &out);
	char *buf = calloc(1, sz);

	ck_assert_ptr_nonnull(buf);

	XDR xdrs;

	xdrmem_create(&xdrs, buf, sz, XDR_ENCODE);
	ck_assert(xdr_ffv2_device_addr4(&xdrs, &out));
	xdr_destroy(&xdrs);

	ffv2_device_addr4 in;

	memset(&in, 0, sizeof(in));
	xdrmem_create(&xdrs, buf, sz, XDR_DECODE);
	ck_assert(xdr_ffv2_device_addr4(&xdrs, &in));
	xdr_destroy(&xdrs);

	ck_assert_uint_eq(in.ffv2da_versions.ffv2da_versions_len, 1u);

	ffv2_device_versions4 *got = &in.ffv2da_versions.ffv2da_versions_val[0];

	ck_assert_uint_eq(got->ffv2dv_version, 4u);
	ck_assert_uint_eq(got->ffv2dv_minorversion, 2u);
	ck_assert(got->ffv2dv_coupling & FFV2_COUPLING_TRUSTED_STATEID);

	ck_assert_uint_eq(in.ffv2da_netaddrs.multipath_list4_len, 1u);
	ck_assert_str_eq(in.ffv2da_netaddrs.multipath_list4_val[0].na_r_addr,
			 uaddr);

	xdr_free((xdrproc_t)xdr_ffv2_device_addr4, (caddr_t)&in);
	free(buf);
}
END_TEST

/*
 * No coupling bits means the synthetic-uid model, which is the value
 * the metadata server sends when the TRUST_STATEID probe came back
 * NFS4ERR_NOTSUPP.  Pinned because FFV2_COUPLING_SYNTHETIC_UIDS is
 * zero: a client that tested the field for equality against
 * TRUSTED_STATEID rather than masking would still work here, and this
 * is the case that would hide that mistake.
 */
START_TEST(test_ffv2_device_addr_no_coupling_bits)
{
	char uaddr[] = "10.0.0.1.8.1";
	char netid[] = "tcp";
	netaddr4 na = { .na_r_netid = netid, .na_r_addr = uaddr };
	ffv2_device_versions4 ver = {
		.ffv2dv_version = 3,
		.ffv2dv_minorversion = 0,
		.ffv2dv_coupling = FFV2_COUPLING_SYNTHETIC_UIDS,
	};
	ffv2_device_addr4 out;

	memset(&out, 0, sizeof(out));
	out.ffv2da_netaddrs.multipath_list4_len = 1;
	out.ffv2da_netaddrs.multipath_list4_val = &na;
	out.ffv2da_versions.ffv2da_versions_len = 1;
	out.ffv2da_versions.ffv2da_versions_val = &ver;

	u_long sz = xdr_sizeof((xdrproc_t)xdr_ffv2_device_addr4, &out);
	char *buf = calloc(1, sz);

	ck_assert_ptr_nonnull(buf);

	XDR xdrs;

	xdrmem_create(&xdrs, buf, sz, XDR_ENCODE);
	ck_assert(xdr_ffv2_device_addr4(&xdrs, &out));
	xdr_destroy(&xdrs);

	ffv2_device_addr4 in;

	memset(&in, 0, sizeof(in));
	xdrmem_create(&xdrs, buf, sz, XDR_DECODE);
	ck_assert(xdr_ffv2_device_addr4(&xdrs, &in));
	xdr_destroy(&xdrs);

	ffv2_device_versions4 *got = &in.ffv2da_versions.ffv2da_versions_val[0];

	ck_assert(!(got->ffv2dv_coupling & FFV2_COUPLING_TRUSTED_STATEID));

	xdr_free((xdrproc_t)xdr_ffv2_device_addr4, (caddr_t)&in);
	free(buf);
}
END_TEST

static Suite *notify_suite(void)
{
	Suite *s = suite_create("getdeviceinfo_notify");
	TCase *tc = tcase_create("table");

	tcase_add_checked_fixture(tc, notify_setup, notify_teardown);
	tcase_add_test(tc, test_no_subscription);
	tcase_add_test(tc, test_register_lookup);
	tcase_add_test(tc, test_last_bitmap_wins);
	tcase_add_test(tc, test_empty_turns_off);
	tcase_add_test(tc, test_clear_unregistered);
	tcase_add_test(tc, test_two_devices_independent);
	tcase_add_test(tc, test_two_layout_types_independent);
	tcase_add_test(tc, test_two_clients_independent);
	suite_add_tcase(s, tc);

	TCase *tc_addr = tcase_create("ffv2_device_addr");
	tcase_add_test(tc_addr, test_ffv2_device_addr_roundtrip);
	tcase_add_test(tc_addr, test_ffv2_device_addr_no_coupling_bits);
	suite_add_tcase(s, tc_addr);
	return s;
}

int main(void)
{
	return nfs4_test_run(notify_suite());
}
