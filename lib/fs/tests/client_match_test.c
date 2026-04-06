/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * Unit tests for client_rule_match() -- exports(5) style client matching.
 *
 * These tests exercise:
 *   - Anonymous "*" matches any peer
 *   - Exact IPv4/IPv6 host matching
 *   - CIDR prefix matching (IPv4 and IPv6)
 *   - Hostname wildcard matching
 *   - Priority: host (1) > CIDR (2) > hostname wildcard (3) > * (4)
 *   - First-listed wins within same priority
 *   - NULL return when nothing matches
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <arpa/inet.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <check.h>

#include "reffs/client_match.h"
#include "reffs/settings.h"
#include "reffs/super_block.h"

/* ------------------------------------------------------------------ */
/* Helper: build a sockaddr_storage for an IPv4 address string         */
/* ------------------------------------------------------------------ */

static struct sockaddr_storage make_ipv4(const char *addr_str)
{
	struct sockaddr_storage ss;
	struct sockaddr_in *sin = (struct sockaddr_in *)&ss;

	memset(&ss, 0, sizeof(ss));
	sin->sin_family = AF_INET;
	inet_pton(AF_INET, addr_str, &sin->sin_addr);
	return ss;
}

/* Helper: build a sockaddr_storage for an IPv6 address string */
static struct sockaddr_storage make_ipv6(const char *addr_str)
{
	struct sockaddr_storage ss;
	struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&ss;

	memset(&ss, 0, sizeof(ss));
	sin6->sin6_family = AF_INET6;
	inet_pton(AF_INET6, addr_str, &sin6->sin6_addr);
	return ss;
}

/* Helper: build a single-rule array with the given match string */
static void make_rule(struct sb_client_rule *rule, const char *match)
{
	memset(rule, 0, sizeof(*rule));
	strncpy(rule->scr_match, match, SB_CLIENT_MATCH_MAX - 1);
	rule->scr_rw = true;
	rule->scr_root_squash = true;
	rule->scr_flavors[0] = REFFS_AUTH_SYS;
	rule->scr_nflavors = 1;
}

/* ------------------------------------------------------------------ */
/* Anonymous wildcard "*"                                               */
/* ------------------------------------------------------------------ */

/*
 * Intent: "*" matches any IPv4 address.
 */
START_TEST(test_match_star_all)
{
	struct sb_client_rule rules[1];
	struct sockaddr_storage peer = make_ipv4("10.1.2.3");

	make_rule(&rules[0], "*");

	const struct sb_client_rule *m = client_rule_match(rules, 1, &peer);

	ck_assert_ptr_nonnull(m);
	ck_assert_str_eq(m->scr_match, "*");
}
END_TEST

/*
 * Intent: "*" also matches an IPv6 address.
 */
START_TEST(test_match_star_ipv6)
{
	struct sb_client_rule rules[1];
	struct sockaddr_storage peer = make_ipv6("2001:db8::1");

	make_rule(&rules[0], "*");

	const struct sb_client_rule *m = client_rule_match(rules, 1, &peer);

	ck_assert_ptr_nonnull(m);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Exact IPv4 host                                                      */
/* ------------------------------------------------------------------ */

/*
 * Intent: a single IPv4 host rule matches that exact address.
 */
START_TEST(test_match_ipv4_exact)
{
	struct sb_client_rule rules[2];
	struct sockaddr_storage peer = make_ipv4("192.168.1.5");

	make_rule(&rules[0], "192.168.1.5");
	make_rule(&rules[1], "*");

	const struct sb_client_rule *m = client_rule_match(rules, 2, &peer);

	ck_assert_ptr_nonnull(m);
	ck_assert_str_eq(m->scr_match, "192.168.1.5");
}
END_TEST

/*
 * Intent: a single IPv4 host rule does NOT match a neighboring address.
 * The "*" rule should be reached instead.
 */
START_TEST(test_match_ipv4_exact_no_neighbor)
{
	struct sb_client_rule rules[2];
	struct sockaddr_storage peer = make_ipv4("192.168.1.6");

	make_rule(&rules[0], "192.168.1.5");
	make_rule(&rules[1], "*");

	const struct sb_client_rule *m = client_rule_match(rules, 2, &peer);

	ck_assert_ptr_nonnull(m);
	ck_assert_str_eq(m->scr_match, "*");
}
END_TEST

/* ------------------------------------------------------------------ */
/* IPv4 CIDR                                                            */
/* ------------------------------------------------------------------ */

/*
 * Intent: a /24 CIDR rule matches hosts in the subnet.
 */
START_TEST(test_match_cidr_v4)
{
	struct sb_client_rule rules[2];
	struct sockaddr_storage peer = make_ipv4("192.168.1.99");

	make_rule(&rules[0], "192.168.1.0/24");
	make_rule(&rules[1], "*");

	const struct sb_client_rule *m = client_rule_match(rules, 2, &peer);

	ck_assert_ptr_nonnull(m);
	ck_assert_str_eq(m->scr_match, "192.168.1.0/24");
}
END_TEST

/*
 * Intent: a /24 CIDR rule does NOT match hosts outside the subnet.
 */
START_TEST(test_match_cidr_v4_outside)
{
	struct sb_client_rule rules[1];
	struct sockaddr_storage peer = make_ipv4("192.168.2.1");

	make_rule(&rules[0], "192.168.1.0/24");

	const struct sb_client_rule *m = client_rule_match(rules, 1, &peer);

	ck_assert_ptr_null(m);
}
END_TEST

/*
 * Intent: an IPv6 /48 CIDR prefix matches addresses within the prefix.
 */
START_TEST(test_match_cidr_v6)
{
	struct sb_client_rule rules[2];
	struct sockaddr_storage peer = make_ipv6("2001:db8:1::1");

	make_rule(&rules[0], "2001:db8::/32");
	make_rule(&rules[1], "*");

	const struct sb_client_rule *m = client_rule_match(rules, 2, &peer);

	ck_assert_ptr_nonnull(m);
	ck_assert_str_eq(m->scr_match, "2001:db8::/32");
}
END_TEST

/* ------------------------------------------------------------------ */
/* Priority: host (1) beats CIDR (2) beats * (4)                       */
/* ------------------------------------------------------------------ */

/*
 * Intent: an exact host rule has higher priority than a CIDR rule that
 * also matches, even when the CIDR rule is listed first.
 */
START_TEST(test_match_priority_host_beats_cidr)
{
	struct sb_client_rule rules[2];
	struct sockaddr_storage peer = make_ipv4("10.0.0.1");

	/* CIDR listed first, then exact host */
	make_rule(&rules[0], "10.0.0.0/24");
	make_rule(&rules[1], "10.0.0.1");

	const struct sb_client_rule *m = client_rule_match(rules, 2, &peer);

	ck_assert_ptr_nonnull(m);
	/* Host rule (priority 1) wins over CIDR (priority 2) */
	ck_assert_str_eq(m->scr_match, "10.0.0.1");
}
END_TEST

/*
 * Intent: a CIDR rule has higher priority than "*".
 */
START_TEST(test_match_priority_cidr_beats_star)
{
	struct sb_client_rule rules[2];
	struct sockaddr_storage peer = make_ipv4("10.0.0.5");

	/* "*" listed first, then CIDR */
	make_rule(&rules[0], "*");
	make_rule(&rules[1], "10.0.0.0/24");

	const struct sb_client_rule *m = client_rule_match(rules, 2, &peer);

	ck_assert_ptr_nonnull(m);
	/* CIDR (priority 2) wins over * (priority 4) */
	ck_assert_str_eq(m->scr_match, "10.0.0.0/24");
}
END_TEST

/* ------------------------------------------------------------------ */
/* No match                                                             */
/* ------------------------------------------------------------------ */

/*
 * Intent: when no rule matches, client_rule_match returns NULL.
 */
START_TEST(test_match_no_match)
{
	struct sb_client_rule rules[1];
	/* Peer is outside the only configured subnet */
	struct sockaddr_storage peer = make_ipv4("172.16.0.1");

	make_rule(&rules[0], "10.0.0.0/8");

	const struct sb_client_rule *m = client_rule_match(rules, 1, &peer);

	ck_assert_ptr_null(m);
}
END_TEST

/*
 * Intent: an empty rule list always returns NULL.
 */
START_TEST(test_match_empty_rules)
{
	struct sockaddr_storage peer = make_ipv4("1.2.3.4");
	const struct sb_client_rule *m = client_rule_match(NULL, 0, &peer);

	ck_assert_ptr_null(m);
}
END_TEST

/* ------------------------------------------------------------------ */
/* First-listed wins within same priority level                         */
/* ------------------------------------------------------------------ */

/*
 * Intent: when two CIDR rules both match, the first one listed wins.
 */
START_TEST(test_match_first_of_same_type)
{
	struct sb_client_rule rules[2];
	/* Both /24 subnets contain 192.168.1.5 -- /24 is priority 2 for both */
	struct sockaddr_storage peer = make_ipv4("192.168.1.5");

	/* /24 and /16 both match; first listed (/24) should win */
	make_rule(&rules[0], "192.168.1.0/24");
	make_rule(&rules[1], "192.168.0.0/16");

	const struct sb_client_rule *m = client_rule_match(rules, 2, &peer);

	ck_assert_ptr_nonnull(m);
	ck_assert_str_eq(m->scr_match, "192.168.1.0/24");
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite                                                                */
/* ------------------------------------------------------------------ */

static Suite *client_match_suite(void)
{
	Suite *s = suite_create("client_match");
	TCase *tc;

	tc = tcase_create("star");
	tcase_add_test(tc, test_match_star_all);
	tcase_add_test(tc, test_match_star_ipv6);
	suite_add_tcase(s, tc);

	tc = tcase_create("ipv4_exact");
	tcase_add_test(tc, test_match_ipv4_exact);
	tcase_add_test(tc, test_match_ipv4_exact_no_neighbor);
	suite_add_tcase(s, tc);

	tc = tcase_create("cidr");
	tcase_add_test(tc, test_match_cidr_v4);
	tcase_add_test(tc, test_match_cidr_v4_outside);
	tcase_add_test(tc, test_match_cidr_v6);
	suite_add_tcase(s, tc);

	tc = tcase_create("priority");
	tcase_add_test(tc, test_match_priority_host_beats_cidr);
	tcase_add_test(tc, test_match_priority_cidr_beats_star);
	suite_add_tcase(s, tc);

	tc = tcase_create("no_match");
	tcase_add_test(tc, test_match_no_match);
	tcase_add_test(tc, test_match_empty_rules);
	suite_add_tcase(s, tc);

	tc = tcase_create("ordering");
	tcase_add_test(tc, test_match_first_of_same_type);
	suite_add_tcase(s, tc);

	return s;
}

int main(void)
{
	int nfailed;
	Suite *s = client_match_suite();
	SRunner *sr = srunner_create(s);

	srunner_run_all(sr, CK_NORMAL);
	nfailed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return nfailed ? 1 : 0;
}
