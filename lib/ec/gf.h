/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * GF(2^8) finite field arithmetic.
 *
 * Irreducible polynomial: x^8 + x^4 + x^3 + x^2 + 1 (0x11d).
 * Generator: 2 (primitive element of order 255).
 *
 * All arithmetic uses scalar log/antilog table lookup.
 * Reference: Berlekamp, "Algebraic Coding Theory" (1968).
 */

#ifndef _REFFS_EC_GF_H
#define _REFFS_EC_GF_H

#include <stdint.h>

/*
 * gf_init -- build the log and antilog tables.
 *
 * Must be called once before any other gf_* function.
 * Idempotent: safe to call multiple times.
 */
void gf_init(void);

/* Addition in GF(2^8) is XOR. */
static inline uint8_t gf_add(uint8_t a, uint8_t b)
{
	return a ^ b;
}

/* Subtraction in GF(2^8) equals addition (characteristic 2). */
static inline uint8_t gf_sub(uint8_t a, uint8_t b)
{
	return a ^ b;
}

/* Multiplication via log/antilog tables.  Returns 0 if either input is 0. */
uint8_t gf_mul(uint8_t a, uint8_t b);

/* Multiplicative inverse.  Undefined for 0 (returns 0 as sentinel). */
uint8_t gf_inv(uint8_t a);

/* Division: a / b = a * inv(b).  Undefined for b == 0. */
static inline uint8_t gf_div(uint8_t a, uint8_t b)
{
	return gf_mul(a, gf_inv(b));
}

/* Power: a^n in GF(2^8).  a^0 = 1 for all a including 0. */
uint8_t gf_pow(uint8_t a, uint8_t n);

#endif /* _REFFS_EC_GF_H */
