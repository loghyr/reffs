/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_TIME_H
#define _REFFS_TIME_H

#include <stdint.h>
#include <time.h>
#include <stdbool.h>
#include "nfsv3_xdr.h"
#include "nfsv42_xdr.h"

static inline int64_t timespec_to_ns(const struct timespec *ts)
{
	return (int64_t)ts->tv_sec * (int64_t)1000000000 + ts->tv_nsec;
}

static inline void timespec_to_nfstime3(struct timespec *ts, nfstime3 *nt)
{
	if (ts->tv_sec > UINT32_MAX)
		nt->seconds = UINT32_MAX;
	else
		nt->seconds = (uint32_t)ts->tv_sec;

	nt->nseconds = (uint32_t)ts->tv_nsec;
}

static inline void nfstime3_to_timespec(nfstime3 *nt, struct timespec *ts)
{
	ts->tv_sec = (time_t)nt->seconds;
	ts->tv_nsec = (long)nt->nseconds;
}

static inline bool nfstime3_is_timespec(nfstime3 *nt, struct timespec *ts)
{
	return ts->tv_sec == (time_t)nt->seconds &&
	       ts->tv_nsec == (long)nt->nseconds;
}

/* ── Validation ─────────────────────────────────────────────────────────── */

/*
 * RFC 5661 §2.2: nseconds must be in [0, 999999999].
 * nfstime4 seconds is int64_t, so negative epochs are legal (pre-1970).
 */
static inline bool nfstime4_valid(const nfstime4 *t)
{
	return t->nseconds <= 999999999U;
}

/* ── Conversion ─────────────────────────────────────────────────────────── */

static inline void nfstime4_to_timespec(const nfstime4 *nfs,
					struct timespec *ts)
{
	ts->tv_sec = (time_t)nfs->seconds;
	ts->tv_nsec = (long)nfs->nseconds;
}

static inline void timespec_to_nfstime4(const struct timespec *ts,
					nfstime4 *nfs)
{
	/*
         * struct timespec tv_nsec must be in [0, 999999999] when the
         * timespec is normalised, but be defensive: a non-normalised
         * timespec (e.g. from arithmetic) could carry a negative or
         * oversized nsec.  Normalise before storing.
         */
	int64_t sec = (int64_t)ts->tv_sec;
	long nsec = ts->tv_nsec;

	if (nsec < 0) {
		sec -= 1;
		nsec += 1000000000L;
	} else if (nsec >= 1000000000L) {
		sec += nsec / 1000000000L;
		nsec = nsec % 1000000000L;
	}

	nfs->seconds = sec;
	nfs->nseconds = (uint32_t)nsec;
}

/* ── Comparison ─────────────────────────────────────────────────────────── */

/*
 * Returns:  -1  if a < b
 *            0  if a == b
 *           +1  if a > b
 */
static inline int nfstime4_cmp(const nfstime4 *a, const nfstime4 *b)
{
	if (a->seconds != b->seconds)
		return a->seconds < b->seconds ? -1 : 1;
	if (a->nseconds != b->nseconds)
		return a->nseconds < b->nseconds ? -1 : 1;
	return 0;
}

static inline bool nfstime4_eq(const nfstime4 *a, const nfstime4 *b)
{
	return nfstime4_cmp(a, b) == 0;
}
static inline bool nfstime4_lt(const nfstime4 *a, const nfstime4 *b)
{
	return nfstime4_cmp(a, b) < 0;
}
static inline bool nfstime4_le(const nfstime4 *a, const nfstime4 *b)
{
	return nfstime4_cmp(a, b) <= 0;
}
static inline bool nfstime4_gt(const nfstime4 *a, const nfstime4 *b)
{
	return nfstime4_cmp(a, b) > 0;
}
static inline bool nfstime4_ge(const nfstime4 *a, const nfstime4 *b)
{
	return nfstime4_cmp(a, b) >= 0;
}

/* Compare an nfstime4 directly against a struct timespec. */
static inline int nfstime4_cmp_timespec(const nfstime4 *a,
					const struct timespec *b)
{
	nfstime4 tmp;
	timespec_to_nfstime4(b, &tmp);
	return nfstime4_cmp(a, &tmp);
}

/* ── Arithmetic ─────────────────────────────────────────────────────────── */

/*
 * Compute delta = a - b as a struct timespec (signed, normalised).
 * Useful for logging elapsed time, checking change windows, etc.
 */
static inline struct timespec nfstime4_diff(const nfstime4 *a,
					    const nfstime4 *b)
{
	struct timespec delta;
	int64_t sec = a->seconds - b->seconds;
	int32_t nsec = (int32_t)a->nseconds - (int32_t)b->nseconds;

	if (nsec < 0) {
		sec -= 1;
		nsec += 1000000000L;
	}

	delta.tv_sec = (time_t)sec;
	delta.tv_nsec = (long)nsec;
	return delta;
}

/* ── Convenience: current wall-clock time ───────────────────────────────── */

static inline int nfstime4_now(nfstime4 *nfs)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
		return -1; /* errno set */
	timespec_to_nfstime4(&ts, nfs);
	return 0;
}

/* ── Zero / epoch helpers ───────────────────────────────────────────────── */

static inline void nfstime4_zero(nfstime4 *t)
{
	t->seconds = 0;
	t->nseconds = 0;
}

static inline bool nfstime4_is_zero(const nfstime4 *t)
{
	return t->seconds == 0 && t->nseconds == 0;
}

/* ── Arithmetic: add/subtract nanoseconds ───────────────────────────────── */

/*
 * Add a nanosecond delta (positive or negative) to an nfstime4.
 * Handles all carry/borrow across the seconds boundary correctly.
 *
 * Returns true on success, false on overflow of int64_t seconds.
 */
static inline bool nfstime4_add_nsec(nfstime4 *t, int64_t delta_nsec)
{
	int64_t total_nsec = (int64_t)t->nseconds + delta_nsec;

	/*
         * total_nsec can be anywhere from INT64_MIN+999999999 to
         * INT64_MAX.  Fold it into a seconds carry and a [0,999999999]
         * residual using floored division so the residual is always
         * non-negative, regardless of the sign of total_nsec.
         */
	int64_t carry = total_nsec / (int64_t)1000000000;
	int64_t nsec = total_nsec % (int64_t)1000000000;

	if (nsec < 0) {
		carry -= 1;
		nsec += 1000000000;
	}

	/* Check for seconds overflow before committing. */
	if (carry > 0 && t->seconds > INT64_MAX - carry)
		return false;
	if (carry < 0 && t->seconds < INT64_MIN - carry)
		return false;

	t->seconds += carry;
	t->nseconds = (uint32_t)nsec;
	return true;
}

/*
 * Subtract a nanosecond delta (positive or negative) from an nfstime4.
 * Equivalent to nfstime4_add_nsec(t, -delta_nsec) but avoids negating
 * INT64_MIN.
 */
static inline bool nfstime4_sub_nsec(nfstime4 *t, int64_t delta_nsec)
{
	/*
         * Negating INT64_MIN is UB.  Handle it by splitting: subtract
         * INT64_MAX first (safe), then subtract the remaining 1.
         */
	if (delta_nsec == INT64_MIN) {
		return nfstime4_add_nsec(t, INT64_MAX) &&
		       nfstime4_add_nsec(t, 1);
	}
	return nfstime4_add_nsec(t, -delta_nsec);
}

/* ── Arithmetic: add/subtract struct timespec ───────────────────────────── */

/*
 * Add a (possibly negative, possibly non-normalised) timespec to an nfstime4.
 * The seconds are added first, then the nanoseconds, each with overflow
 * checks, so a large-magnitude timespec does not silently wrap.
 */
static inline bool nfstime4_add_timespec(nfstime4 *t,
					 const struct timespec *delta)
{
	int64_t dsec = (int64_t)delta->tv_sec;
	int64_t dnsec = (int64_t)delta->tv_nsec;

	/* Seconds leg. */
	if (dsec > 0 && t->seconds > INT64_MAX - dsec)
		return false;
	if (dsec < 0 && t->seconds < INT64_MIN - dsec)
		return false;
	t->seconds += dsec;

	/* Nanoseconds leg (handles non-normalised tv_nsec via add_nsec). */
	return nfstime4_add_nsec(t, dnsec);
}

static inline bool nfstime4_sub_timespec(nfstime4 *t,
					 const struct timespec *delta)
{
	struct timespec neg = {
		.tv_sec = -delta->tv_sec,
		.tv_nsec = -delta->tv_nsec,
	};
	return nfstime4_add_timespec(t, &neg);
}

#endif /* _REFFS_TIME_H */
