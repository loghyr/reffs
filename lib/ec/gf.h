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

/*
 * gf_mul_tbl_init -- precompute a 256-entry multiplication table
 * for a single coefficient c, so callers can replace
 * `gf_mul(c, b)` on the inner loop with `tbl[b]` (single memory
 * access instead of log/antilog double-lookup).
 *
 * Typical use: RS encode/decode inner loop, once per (row, col)
 * matrix coefficient at create time or decode-invert time.  256
 * bytes per (row, col); (k+m) x k table fits in tens of KiB for
 * realistic geometries.
 */
void gf_mul_tbl_init(uint8_t c, uint8_t tbl[256]);

/*
 * gf_mul_split_tbl_init -- precompute two 16-entry multiplication
 * tables (low nibble and high nibble) for a single coefficient c.
 * Layout: tbl[0..15]  = gf_mul(c, i)        for i in [0, 16)   (low nibble)
 *         tbl[16..31] = gf_mul(c, i << 4)   for i in [0, 16)   (high nibble)
 *
 * Callers reconstruct gf_mul(c, b) as
 *   tbl[b & 0xf] ^ tbl[16 + (b >> 4)]
 * i.e. two 4-bit-indexed lookups.  This factoring maps directly
 * onto SIMD byte-shuffle instructions -- aarch64's vqtbl1q_u8
 * and x86's pshufb both do 16 parallel 4-bit-indexed lookups per
 * instruction on a 16-byte register -- so the hot loop can
 * process 16 shard bytes per shuffle pair instead of one byte
 * per full-table lookup.
 *
 * 32 bytes per (row, col) coefficient.  Compare with
 * gf_mul_tbl_init's 256 bytes.
 */
void gf_mul_split_tbl_init(uint8_t c, uint8_t tbl[32]);

#endif /* _REFFS_EC_GF_H */
