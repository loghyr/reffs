/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" /* IWYU pragma: keep */
#endif

/*
 * Unit tests for mds_parse_host_port -- the host[:port] / [host]:port
 * parser used by mds_session_clnt_open.  The previous strrchr-based
 * parser silently mis-parsed both bracketed and unbracketed IPv6;
 * these tests pin the documented forms and reject the ambiguous
 * cases.
 */

#include <check.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "host_port.h"

/* --- happy paths ------------------------------------------------- */

START_TEST(test_bare_host_no_port)
{
	char host[64];
	int port = -1;

	ck_assert_int_eq(
		mds_parse_host_port("reffs-mds", host, sizeof(host), &port), 0);
	ck_assert_str_eq(host, "reffs-mds");
	ck_assert_int_eq(port, 0);
}
END_TEST

START_TEST(test_host_with_port)
{
	char host[64];
	int port = -1;

	ck_assert_int_eq(mds_parse_host_port("reffs-ps-a:2049", host,
					     sizeof(host), &port),
			 0);
	ck_assert_str_eq(host, "reffs-ps-a");
	ck_assert_int_eq(port, 2049);
}
END_TEST

START_TEST(test_ipv4_with_port)
{
	char host[64];
	int port = -1;

	ck_assert_int_eq(mds_parse_host_port("127.0.0.1:4098", host,
					     sizeof(host), &port),
			 0);
	ck_assert_str_eq(host, "127.0.0.1");
	ck_assert_int_eq(port, 4098);
}
END_TEST

START_TEST(test_bracketed_ipv6_with_port)
{
	char host[64];
	int port = -1;

	ck_assert_int_eq(mds_parse_host_port("[2001:db8::1]:2049", host,
					     sizeof(host), &port),
			 0);
	ck_assert_str_eq(host, "2001:db8::1");
	ck_assert_int_eq(port, 2049);
}
END_TEST

START_TEST(test_bracketed_ipv6_no_port)
{
	char host[64];
	int port = -1;

	ck_assert_int_eq(
		mds_parse_host_port("[::1]", host, sizeof(host), &port), 0);
	ck_assert_str_eq(host, "::1");
	ck_assert_int_eq(port, 0);
}
END_TEST

START_TEST(test_unbracketed_ipv6_no_port)
{
	char host[64];
	int port = -1;

	/*
	 * Bare ">1 colons" form is treated as a literal -- caller's
	 * getaddrinfo handles it.  No port can be attached without
	 * brackets; this is the explicit policy.
	 */
	ck_assert_int_eq(mds_parse_host_port("2001:db8::1", host, sizeof(host),
					     &port),
			 0);
	ck_assert_str_eq(host, "2001:db8::1");
	ck_assert_int_eq(port, 0);
}
END_TEST

START_TEST(test_loopback_ipv6_short)
{
	char host[64];
	int port = -1;

	ck_assert_int_eq(mds_parse_host_port("::1", host, sizeof(host), &port),
			 0);
	ck_assert_str_eq(host, "::1");
	ck_assert_int_eq(port, 0);
}
END_TEST

/* --- failure paths ----------------------------------------------- */

START_TEST(test_null_input)
{
	char host[64];
	int port = -1;

	ck_assert_int_eq(mds_parse_host_port(NULL, host, sizeof(host), &port),
			 -EINVAL);
}
END_TEST

START_TEST(test_null_host_buf)
{
	int port = -1;

	ck_assert_int_eq(mds_parse_host_port("foo", NULL, 16, &port), -EINVAL);
}
END_TEST

START_TEST(test_zero_buf_size)
{
	char host[1];
	int port = -1;

	ck_assert_int_eq(mds_parse_host_port("foo", host, 0, &port), -EINVAL);
}
END_TEST

START_TEST(test_null_port)
{
	char host[64];

	ck_assert_int_eq(mds_parse_host_port("foo", host, sizeof(host), NULL),
			 -EINVAL);
}
END_TEST

START_TEST(test_empty_input)
{
	char host[64];
	int port = -1;

	ck_assert_int_eq(mds_parse_host_port("", host, sizeof(host), &port),
			 -EINVAL);
}
END_TEST

START_TEST(test_empty_host)
{
	char host[64];
	int port = -1;

	ck_assert_int_eq(mds_parse_host_port(":2049", host, sizeof(host),
					     &port),
			 -EINVAL);
}
END_TEST

START_TEST(test_empty_port)
{
	char host[64];
	int port = -1;

	ck_assert_int_eq(mds_parse_host_port("host:", host, sizeof(host),
					     &port),
			 -EINVAL);
}
END_TEST

START_TEST(test_port_zero_rejected)
{
	char host[64];
	int port = -1;

	ck_assert_int_eq(mds_parse_host_port("host:0", host, sizeof(host),
					     &port),
			 -EINVAL);
}
END_TEST

START_TEST(test_port_too_high)
{
	char host[64];
	int port = -1;

	ck_assert_int_eq(mds_parse_host_port("host:65536", host, sizeof(host),
					     &port),
			 -EINVAL);
}
END_TEST

START_TEST(test_port_non_numeric)
{
	char host[64];
	int port = -1;

	ck_assert_int_eq(mds_parse_host_port("host:abc", host, sizeof(host),
					     &port),
			 -EINVAL);
}
END_TEST

START_TEST(test_bracketed_no_close)
{
	char host[64];
	int port = -1;

	ck_assert_int_eq(mds_parse_host_port("[2001:db8::1", host, sizeof(host),
					     &port),
			 -EINVAL);
}
END_TEST

START_TEST(test_bracketed_empty_host)
{
	char host[64];
	int port = -1;

	ck_assert_int_eq(mds_parse_host_port("[]:2049", host, sizeof(host),
					     &port),
			 -EINVAL);
}
END_TEST

START_TEST(test_bracketed_garbage_after)
{
	char host[64];
	int port = -1;

	ck_assert_int_eq(mds_parse_host_port("[host]junk", host, sizeof(host),
					     &port),
			 -EINVAL);
}
END_TEST

START_TEST(test_bracketed_empty_port_after_colon)
{
	char host[64];
	int port = -1;

	ck_assert_int_eq(mds_parse_host_port("[host]:", host, sizeof(host),
					     &port),
			 -EINVAL);
}
END_TEST

START_TEST(test_buf_too_small)
{
	char host[4];
	int port = -1;

	ck_assert_int_eq(mds_parse_host_port("longerhost", host, sizeof(host),
					     &port),
			 -EINVAL);
}
END_TEST

START_TEST(test_buf_exact_fit)
{
	/* 4-byte buf must hold "abc" + NUL exactly. */
	char host[4];
	int port = -1;

	ck_assert_int_eq(mds_parse_host_port("abc", host, sizeof(host), &port),
			 0);
	ck_assert_str_eq(host, "abc");
}
END_TEST

/* --- wiring ------------------------------------------------------ */

static Suite *host_port_suite(void)
{
	Suite *s = suite_create("host_port");
	TCase *tc = tcase_create("parse");

	tcase_add_test(tc, test_bare_host_no_port);
	tcase_add_test(tc, test_host_with_port);
	tcase_add_test(tc, test_ipv4_with_port);
	tcase_add_test(tc, test_bracketed_ipv6_with_port);
	tcase_add_test(tc, test_bracketed_ipv6_no_port);
	tcase_add_test(tc, test_unbracketed_ipv6_no_port);
	tcase_add_test(tc, test_loopback_ipv6_short);
	tcase_add_test(tc, test_null_input);
	tcase_add_test(tc, test_null_host_buf);
	tcase_add_test(tc, test_zero_buf_size);
	tcase_add_test(tc, test_null_port);
	tcase_add_test(tc, test_empty_input);
	tcase_add_test(tc, test_empty_host);
	tcase_add_test(tc, test_empty_port);
	tcase_add_test(tc, test_port_zero_rejected);
	tcase_add_test(tc, test_port_too_high);
	tcase_add_test(tc, test_port_non_numeric);
	tcase_add_test(tc, test_bracketed_no_close);
	tcase_add_test(tc, test_bracketed_empty_host);
	tcase_add_test(tc, test_bracketed_garbage_after);
	tcase_add_test(tc, test_bracketed_empty_port_after_colon);
	tcase_add_test(tc, test_buf_too_small);
	tcase_add_test(tc, test_buf_exact_fit);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int failed;
	SRunner *sr = srunner_create(host_port_suite());

	srunner_run_all(sr, CK_NORMAL);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
