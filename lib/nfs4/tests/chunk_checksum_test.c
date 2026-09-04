/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <check.h>

#include "nfs4/chunk_checksum.h"

struct checksum_vector {
	uint32_t algorithm;
	const uint8_t *input;
	size_t input_len;
	const uint8_t *expected;
	uint32_t expected_len;
};

static const uint8_t digits[] = "123456789";
static const uint8_t abc[] = "abc";
static const uint8_t crc32_result[] = { 0xcb, 0xf4, 0x39, 0x26 };
static const uint8_t crc32c_result[] = { 0xe3, 0x06, 0x92, 0x83 };
static const uint8_t sha256_result[] = {
	0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
	0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
	0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
};
static const uint8_t sha512_result[] = {
	0xdd, 0xaf, 0x35, 0xa1, 0x93, 0x61, 0x7a, 0xba, 0xcc, 0x41, 0x73,
	0x49, 0xae, 0x20, 0x41, 0x31, 0x12, 0xe6, 0xfa, 0x4e, 0x89, 0xa9,
	0x7e, 0xa2, 0x0a, 0x9e, 0xee, 0xe6, 0x4b, 0x55, 0xd3, 0x9a, 0x21,
	0x92, 0x99, 0x2a, 0x27, 0x4f, 0xc1, 0xa8, 0x36, 0xba, 0x3c, 0x23,
	0xa3, 0xfe, 0xeb, 0xbd, 0x45, 0x4d, 0x44, 0x23, 0x64, 0x3c, 0xe8,
	0x0e, 0x2a, 0x9a, 0xc9, 0x4f, 0xa5, 0x4c, 0xa4, 0x9f,
};
static const uint8_t blake3_result[] = {
	0x64, 0x37, 0xb3, 0xac, 0x38, 0x46, 0x51, 0x33, 0xff, 0xb6, 0x3b,
	0x75, 0x27, 0x3a, 0x8d, 0xb5, 0x48, 0xc5, 0x58, 0x46, 0x5d, 0x79,
	0xdb, 0x03, 0xfd, 0x35, 0x9c, 0x6c, 0xd5, 0xbd, 0x9d, 0x85,
};

static void check_vector(const struct checksum_vector *vector)
{
	uint8_t actual[64] = { 0 };
	uint32_t actual_len = 0;

	ck_assert_int_eq(
		chunk_checksum_compute(vector->algorithm, vector->input,
				       vector->input_len, actual, &actual_len),
		0);
	ck_assert_uint_eq(actual_len, vector->expected_len);
	ck_assert_int_eq(memcmp(actual, vector->expected, actual_len), 0);
}

START_TEST(test_registered_known_answers)
{
	static const struct checksum_vector vectors[] = {
		{ CHECKSUM_ALG_CRC32, digits, sizeof(digits) - 1, crc32_result,
		  sizeof(crc32_result) },
		{ CHECKSUM_ALG_CRC32C, digits, sizeof(digits) - 1,
		  crc32c_result, sizeof(crc32c_result) },
		{ CHECKSUM_ALG_SHA256, abc, sizeof(abc) - 1, sha256_result,
		  sizeof(sha256_result) },
		{ CHECKSUM_ALG_SHA512, abc, sizeof(abc) - 1, sha512_result,
		  sizeof(sha512_result) },
		{ CHECKSUM_ALG_BLAKE3, abc, sizeof(abc) - 1, blake3_result,
		  sizeof(blake3_result) },
	};

	for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++)
		check_vector(&vectors[i]);
}
END_TEST

START_TEST(test_fletcher4_wire_value)
{
	static const uint8_t words[] = {
		1, 0, 0, 0, 2, 0, 0, 0, 3, 0, 0, 0, 4, 0, 0, 0,
	};
	static const uint8_t expected[32] = {
		0, 0, 0, 0, 0, 0, 0, 10, 0, 0, 0, 0, 0, 0, 0, 20,
		0, 0, 0, 0, 0, 0, 0, 35, 0, 0, 0, 0, 0, 0, 0, 56,
	};
	const struct checksum_vector vector = { CHECKSUM_ALG_FLETCHER4, words,
						sizeof(words), expected,
						sizeof(expected) };

	check_vector(&vector);
}
END_TEST

START_TEST(test_none_and_invalid_inputs)
{
	uint8_t value[64] = { 0 };
	uint32_t len = 99;

	ck_assert(chunk_checksum_supported(CHECKSUM_ALG_NONE));
	ck_assert_int_eq(chunk_checksum_compute(CHECKSUM_ALG_NONE, NULL, 0,
						value, &len),
			 0);
	ck_assert_uint_eq(len, 0);
	ck_assert_int_eq(chunk_checksum_compute(99, abc, sizeof(abc) - 1, value,
						&len),
			 -EOPNOTSUPP);
	ck_assert_int_eq(chunk_checksum_compute(CHECKSUM_ALG_CRC32, NULL, 1,
						value, &len),
			 -EINVAL);
	ck_assert_int_eq(chunk_checksum_compute(CHECKSUM_ALG_FLETCHER4, abc,
						sizeof(abc) - 1, value, &len),
			 -EINVAL);
}
END_TEST

START_TEST(test_pack_and_verify)
{
	checksum4 checksum = { 0 };

	ck_assert_int_eq(chunk_checksum_pack_data(&checksum,
						  CHECKSUM_ALG_SHA256, abc,
						  sizeof(abc) - 1),
			 0);
	ck_assert_uint_eq(checksum.cs_algorithm, CHECKSUM_ALG_SHA256);
	ck_assert_uint_eq(checksum.cs_value.cs_value_len, 32);
	ck_assert_int_eq(chunk_checksum_verify(&checksum, abc, sizeof(abc) - 1),
			 0);
	checksum.cs_value.cs_value_val[0] ^= 1;
	ck_assert_int_eq(chunk_checksum_verify(&checksum, abc, sizeof(abc) - 1),
			 -EBADMSG);
	checksum.cs_value.cs_value_len--;
	ck_assert_int_eq(chunk_checksum_verify(&checksum, abc, sizeof(abc) - 1),
			 -EINVAL);
	free(checksum.cs_value.cs_value_val);
}
END_TEST

static Suite *chunk_checksum_suite(void)
{
	Suite *suite = suite_create("chunk_checksum");
	TCase *test_case = tcase_create("core");

	tcase_add_test(test_case, test_registered_known_answers);
	tcase_add_test(test_case, test_fletcher4_wire_value);
	tcase_add_test(test_case, test_none_and_invalid_inputs);
	tcase_add_test(test_case, test_pack_and_verify);
	suite_add_tcase(suite, test_case);
	return suite;
}

int main(void)
{
	Suite *suite = chunk_checksum_suite();
	SRunner *runner = srunner_create(suite);

	srunner_run_all(runner, CK_NORMAL);
	int failed = srunner_ntests_failed(runner);

	srunner_free(runner);
	return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
