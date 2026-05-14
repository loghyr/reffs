/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <arpa/inet.h>
#include <check.h>
#include <errno.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

#include "reffs/settings.h"

#include "ps_local_addr.h"
#include "ps_state.h"

/*
 * Direct manipulation tests do not need the global ps_state registry;
 * they exercise the pls_local_addrs / match primitive against a
 * stack-local ps_listener_state.  Registry-backed tests (the
 * seed-at-register path) re-use the ps_state_init/fini fixture so the
 * register call publishes through the normal acquire/release dance.
 */
static void setup(void)
{
	ps_state_init();
}

static void teardown(void)
{
	ps_state_fini();
}

/*
 * After ps_state_register, pls_local_addrs must include at least
 * 127.0.0.1.  Every Unix host has a loopback interface; if seeding
 * silently failed, this test surfaces it before any later phase-5
 * slice depends on the table being populated.  We assert via the
 * match primitive rather than peeking at the array directly so the
 * test stays decoupled from the on-disk layout of struct ps_local_addr.
 */
START_TEST(test_local_addr_seed_from_getifaddrs)
{
	struct reffs_proxy_mds_config c;

	memset(&c, 0, sizeof(c));
	c.id = 1;
	c.port = 4098;
	c.mds_port = 2049;
	c.mds_probe = 20490;
	strncpy(c.address, "10.0.0.5", sizeof(c.address) - 1);

	ck_assert_int_eq(ps_state_register(&c), 0);

	const struct ps_listener_state *pls = ps_state_find(1);

	ck_assert_ptr_nonnull(pls);
	ck_assert_uint_gt(pls->pls_nlocal_addrs, 0);
	ck_assert(ps_local_addr_match(pls, "127.0.0.1"));
}
END_TEST

/*
 * Numeric loopback IPv4 match: 127.0.0.1 is in the table, 127.0.0.2
 * is not (no host listens on a second loopback address by default).
 * Exercises the per-bit comparison, not just "starts with 127."
 */
START_TEST(test_local_addr_match_loopback)
{
	struct reffs_proxy_mds_config c;

	memset(&c, 0, sizeof(c));
	c.id = 1;
	c.port = 4098;
	c.mds_port = 2049;
	c.mds_probe = 20490;

	ck_assert_int_eq(ps_state_register(&c), 0);
	const struct ps_listener_state *pls = ps_state_find(1);

	ck_assert(ps_local_addr_match(pls, "127.0.0.1"));
	ck_assert(!ps_local_addr_match(pls, "127.0.0.2"));
}
END_TEST

/*
 * IPv6 loopback: ::1 is always present.  Some CI containers run
 * without IPv6 (sysctl net.ipv6.conf.all.disable_ipv6=1); when that's
 * the case getifaddrs returns no AF_INET6 entries and the test is
 * skipped via early return rather than failing.  The skip path is
 * detected by polling getifaddrs first.
 */
static bool ipv6_loopback_present(void)
{
	struct ifaddrs *ifa, *cur;

	if (getifaddrs(&ifa) != 0)
		return false;
	bool found = false;

	for (cur = ifa; cur && !found; cur = cur->ifa_next) {
		if (!cur->ifa_addr || cur->ifa_addr->sa_family != AF_INET6)
			continue;
		const struct sockaddr_in6 *s6 =
			(const struct sockaddr_in6 *)cur->ifa_addr;
		if (IN6_IS_ADDR_LOOPBACK(&s6->sin6_addr))
			found = true;
	}
	freeifaddrs(ifa);
	return found;
}

START_TEST(test_local_addr_match_ipv6_loopback)
{
	if (!ipv6_loopback_present())
		return;

	struct reffs_proxy_mds_config c;

	memset(&c, 0, sizeof(c));
	c.id = 1;
	c.port = 4098;
	c.mds_port = 2049;
	c.mds_probe = 20490;

	ck_assert_int_eq(ps_state_register(&c), 0);
	const struct ps_listener_state *pls = ps_state_find(1);

	ck_assert(ps_local_addr_match(pls, "::1"));
}
END_TEST

/*
 * Cross-check against a real interface address read independently
 * from getifaddrs.  Picks the first non-loopback AF_INET address
 * (typically the host's primary LAN address) and asserts the match
 * primitive recognises it as local.  If no non-loopback IPv4 exists
 * (a stripped-down sandbox), the test is skipped rather than failed.
 */
START_TEST(test_local_addr_match_external_ipv4)
{
	struct ifaddrs *ifa, *cur;
	char buf[INET_ADDRSTRLEN];
	bool found = false;

	if (getifaddrs(&ifa) != 0)
		return;
	for (cur = ifa; cur && !found; cur = cur->ifa_next) {
		if (!cur->ifa_addr || cur->ifa_addr->sa_family != AF_INET)
			continue;
		const struct sockaddr_in *s4 =
			(const struct sockaddr_in *)cur->ifa_addr;
		if (s4->sin_addr.s_addr == htonl(INADDR_LOOPBACK))
			continue;
		if (!inet_ntop(AF_INET, &s4->sin_addr, buf, sizeof(buf)))
			continue;
		found = true;
	}
	freeifaddrs(ifa);
	if (!found)
		return;

	struct reffs_proxy_mds_config c;

	memset(&c, 0, sizeof(c));
	c.id = 1;
	c.port = 4098;
	c.mds_port = 2049;
	c.mds_probe = 20490;

	ck_assert_int_eq(ps_state_register(&c), 0);
	const struct ps_listener_state *pls = ps_state_find(1);

	ck_assert_msg(ps_local_addr_match(pls, buf),
		      "expected %s in pls_local_addrs", buf);
}
END_TEST

/*
 * A TEST-NET-3 address (RFC 5737 reserves 203.0.113.0/24 for docs)
 * is guaranteed not to be locally configured on any sane CI host.
 * Confirms the no-match path returns false rather than defaulting
 * to true.
 */
START_TEST(test_local_addr_no_match_remote)
{
	struct reffs_proxy_mds_config c;

	memset(&c, 0, sizeof(c));
	c.id = 1;
	c.port = 4098;
	c.mds_port = 2049;
	c.mds_probe = 20490;

	ck_assert_int_eq(ps_state_register(&c), 0);
	const struct ps_listener_state *pls = ps_state_find(1);

	ck_assert(!ps_local_addr_match(pls, "203.0.113.5"));
}
END_TEST

/*
 * Direct construction of a maxed-out table.  This bypasses
 * getifaddrs to exercise the truncation guard: pls_nlocal_addrs
 * must cap at PS_MAX_LOCAL_ADDRS, and the match primitive must
 * walk all entries without reading past la_len bytes.  We hand-fill
 * the table with synthetic 10.0.0.x addresses, then assert match
 * for the last entry (proves the walk reaches index N-1) and
 * non-match for a sentinel that nobody filled in.
 */
START_TEST(test_local_addr_full_table)
{
	struct ps_listener_state pls;

	memset(&pls, 0, sizeof(pls));
	for (uint32_t i = 0; i < PS_MAX_LOCAL_ADDRS; i++) {
		struct sockaddr_in *sin =
			(struct sockaddr_in *)&pls.pls_local_addrs[i].la_ss;
		sin->sin_family = AF_INET;
		sin->sin_addr.s_addr = htonl(0x0a000000u | i);
		pls.pls_local_addrs[i].la_len = sizeof(*sin);
	}
	pls.pls_nlocal_addrs = PS_MAX_LOCAL_ADDRS;

	char last[INET_ADDRSTRLEN];

	snprintf(last, sizeof(last), "10.0.0.%u", PS_MAX_LOCAL_ADDRS - 1);
	ck_assert(ps_local_addr_match(&pls, "10.0.0.0"));
	ck_assert(ps_local_addr_match(&pls, last));
	ck_assert(!ps_local_addr_match(&pls, "10.0.1.0"));

	/*
	 * Asking the seed routine to populate again on a full struct
	 * is the way callers would refresh; it must overwrite rather
	 * than appending past PS_MAX_LOCAL_ADDRS.  We do not assert
	 * a specific post-seed contents (the host may have any number
	 * of interfaces) -- the contract under test is that
	 * pls_nlocal_addrs <= PS_MAX_LOCAL_ADDRS after the call.
	 */
	ck_assert_int_eq(ps_local_addr_seed(&pls), 0);
	ck_assert_uint_le(pls.pls_nlocal_addrs, PS_MAX_LOCAL_ADDRS);
}
END_TEST

/*
 * Defensive arg checks: NULL pls or NULL/empty host returns false
 * without crashing.  These are the two paths a caller might trip
 * (NULL pls because ps_state_find returned NULL on a bad listener
 * id; empty host because the deviceinfo decode left ed_host blank).
 */
START_TEST(test_local_addr_match_bad_args)
{
	struct reffs_proxy_mds_config c;

	memset(&c, 0, sizeof(c));
	c.id = 1;
	c.port = 4098;
	c.mds_port = 2049;
	c.mds_probe = 20490;

	ck_assert_int_eq(ps_state_register(&c), 0);
	const struct ps_listener_state *pls = ps_state_find(1);

	ck_assert(!ps_local_addr_match(NULL, "127.0.0.1"));
	ck_assert(!ps_local_addr_match(pls, NULL));
	ck_assert(!ps_local_addr_match(pls, ""));

	/*
	 * Hostnames (non-numeric) must be rejected -- per the design,
	 * the match primitive uses AI_NUMERICHOST to keep the fanout
	 * hot path DNS-free.  Names that look "host-y" but are not
	 * resolvable as literal addresses MUST return false.
	 */
	ck_assert(!ps_local_addr_match(pls, "this.is.not.an.ip"));
}
END_TEST

static Suite *ps_local_addr_suite(void)
{
	Suite *s = suite_create("ps_local_addr");
	TCase *tc = tcase_create("local_addr");

	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_local_addr_seed_from_getifaddrs);
	tcase_add_test(tc, test_local_addr_match_loopback);
	tcase_add_test(tc, test_local_addr_match_ipv6_loopback);
	tcase_add_test(tc, test_local_addr_match_external_ipv4);
	tcase_add_test(tc, test_local_addr_no_match_remote);
	tcase_add_test(tc, test_local_addr_full_table);
	tcase_add_test(tc, test_local_addr_match_bad_args);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int failed;
	SRunner *sr = srunner_create(ps_local_addr_suite());

	srunner_run_all(sr, CK_NORMAL);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return failed == 0 ? 0 : 1;
}
