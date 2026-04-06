/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * GF(2^8) finite field arithmetic using log/antilog tables.
 *
 * Irreducible polynomial: x^8 + x^4 + x^3 + x^2 + 1 = 0x11d.
 * Generator (primitive element): g = 2, which has order 255.
 *
 * The antilog (exp) table is doubled to 512 entries so that
 * multiplication can use gf_exp[gf_log[a] + gf_log[b]] without
 * modular reduction -- the sum of two log values is at most 508,
 * and gf_exp[i] == gf_exp[i - 255] for i >= 255.
 *
 * Reference: Berlekamp, "Algebraic Coding Theory" (1968), Ch. 6.
 *            Peterson & Weldon, "Error-Correcting Codes" (1972), Ch. 7.
 */

#include "gf.h"

#include <pthread.h>
#include <stdbool.h>

#define GF_POLY 0x11d /* x^8 + x^4 + x^3 + x^2 + 1 */
#define GF_ORDER 255 /* order of the multiplicative group */

static uint8_t gf_exp[512]; /* antilog table (doubled) */
static uint8_t gf_log[256]; /* log table; gf_log[0] unused */
static pthread_once_t gf_once = PTHREAD_ONCE_INIT;

static void gf_init_tables(void)
{
	int x = 1;

	for (int i = 0; i < GF_ORDER; i++) {
		gf_exp[i] = (uint8_t)x;
		gf_log[(uint8_t)x] = (uint8_t)i;
		x <<= 1;
		if (x & 0x100)
			x ^= GF_POLY;
	}

	/* Double the exp table so log[a]+log[b] can index without mod. */
	for (int i = GF_ORDER; i < 512; i++)
		gf_exp[i] = gf_exp[i - GF_ORDER];

	gf_log[0] = 0; /* sentinel -- never used in valid mul */
}

void gf_init(void)
{
	pthread_once(&gf_once, gf_init_tables);
}

uint8_t gf_mul(uint8_t a, uint8_t b)
{
	if (a == 0 || b == 0)
		return 0;
	return gf_exp[gf_log[a] + gf_log[b]];
}

uint8_t gf_inv(uint8_t a)
{
	if (a == 0)
		return 0; /* sentinel */
	return gf_exp[GF_ORDER - gf_log[a]];
}

uint8_t gf_pow(uint8_t a, uint8_t n)
{
	if (n == 0)
		return 1;
	if (a == 0)
		return 0;
	int log_a = gf_log[a];
	int log_result = (log_a * n) % GF_ORDER;
	return gf_exp[log_result];
}
