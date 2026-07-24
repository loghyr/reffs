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

void gf_mul_tbl_init(uint8_t c, uint8_t tbl[256])
{
	/*
	 * Fill tbl[b] = gf_mul(c, b) for b in [0, 255].  Zero out
	 * b == 0 explicitly; other entries come from the log/antilog
	 * fast path with c's log value hoisted out of the loop.
	 */
	tbl[0] = 0;
	if (c == 0) {
		for (int b = 1; b < 256; b++)
			tbl[b] = 0;
		return;
	}
	int log_c = gf_log[c];

	for (int b = 1; b < 256; b++)
		tbl[b] = gf_exp[log_c + gf_log[b]];
}

void gf_mul_split_tbl_init(uint8_t c, uint8_t tbl[32])
{
	/*
	 * Low-nibble half: tbl[i] = gf_mul(c, i) for i in [0, 16).
	 * High-nibble half: tbl[16 + i] = gf_mul(c, i << 4).
	 *
	 * Both halves start at gf_mul(c, 0) = 0.  c == 0 short-
	 * circuits to all zeros.  Otherwise use the log-hoisted
	 * fast path.
	 */
	tbl[0] = 0;
	tbl[16] = 0;
	if (c == 0) {
		for (int i = 1; i < 16; i++) {
			tbl[i] = 0;
			tbl[16 + i] = 0;
		}
		return;
	}
	int log_c = gf_log[c];

	for (int i = 1; i < 16; i++)
		tbl[i] = gf_exp[log_c + gf_log[i]];
	for (int i = 1; i < 16; i++)
		tbl[16 + i] = gf_exp[log_c + gf_log[i << 4]];
}
