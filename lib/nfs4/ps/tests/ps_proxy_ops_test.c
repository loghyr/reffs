/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <check.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ps_proxy_ops.h"
#include "ps_state.h" /* PS_MAX_FH_SIZE */

/*
 * The forwarder requires a live mds_session for a happy-path run
 * (CI integration covers that).  These unit tests cover only the
 * argument-validation shortcuts that fire before any compound is
 * built -- so a NULL / (void *)1 sentinel for the session pointer
 * is harmless because the function never dereferences it on a bad
 * argument path.
 */

START_TEST(test_forward_getattr_null_args)
{
	uint8_t fh[] = { 0x01, 0x02, 0x03 };
	uint32_t mask[] = { 0x00000001 };
	struct ps_proxy_getattr_reply reply;

	memset(&reply, 0, sizeof(reply));

	ck_assert_int_eq(ps_proxy_forward_getattr(NULL, fh, sizeof(fh), mask, 1,
						  NULL, &reply),
			 -EINVAL);
	ck_assert_int_eq(ps_proxy_forward_getattr((void *)1, NULL, sizeof(fh),
						  mask, 1, NULL, &reply),
			 -EINVAL);
	ck_assert_int_eq(ps_proxy_forward_getattr((void *)1, fh, sizeof(fh),
						  NULL, 1, NULL, &reply),
			 -EINVAL);
	ck_assert_int_eq(ps_proxy_forward_getattr((void *)1, fh, sizeof(fh),
						  mask, 1, NULL, NULL),
			 -EINVAL);
}
END_TEST

/*
 * Zero-length FH and zero-length mask are both programmer errors
 * -- the MDS would reject the compound but the forward call should
 * refuse to send it.
 */
START_TEST(test_forward_getattr_zero_lengths)
{
	uint8_t fh[] = { 0x01 };
	uint32_t mask[] = { 0x00000001 };
	struct ps_proxy_getattr_reply reply;

	memset(&reply, 0, sizeof(reply));

	ck_assert_int_eq(ps_proxy_forward_getattr((void *)1, fh, 0, mask, 1,
						  NULL, &reply),
			 -EINVAL);
	ck_assert_int_eq(ps_proxy_forward_getattr((void *)1, fh, sizeof(fh),
						  mask, 0, NULL, &reply),
			 -EINVAL);
}
END_TEST

/*
 * FH lengths above NFS4_FHSIZE (128 bytes, per RFC 8881) are
 * protocol-invalid and short-circuit before the compound is
 * built.  Matches the other PS primitives (ps_sb_binding_alloc,
 * ps_state_set_mds_root_fh) that use the same cap.
 */
START_TEST(test_forward_getattr_fh_too_big)
{
	uint8_t big_fh[PS_MAX_FH_SIZE + 1];
	uint32_t mask[] = { 0x00000001 };
	struct ps_proxy_getattr_reply reply;

	memset(big_fh, 0xAB, sizeof(big_fh));
	memset(&reply, 0, sizeof(reply));

	ck_assert_int_eq(ps_proxy_forward_getattr((void *)1, big_fh,
						  sizeof(big_fh), mask, 1, NULL,
						  &reply),
			 -E2BIG);
}
END_TEST

/*
 * Freeing a never-populated reply (all-zero struct from memset)
 * and a NULL reply are both safe.  Matches the project convention
 * for NULL-tolerant release helpers.
 */
START_TEST(test_reply_free_null_safe)
{
	struct ps_proxy_getattr_reply reply = { 0 };

	ps_proxy_getattr_reply_free(NULL);
	ps_proxy_getattr_reply_free(&reply);

	/*
	 * Verifiable post-condition: the struct stays zero and a
	 * second free on the same struct is still safe (idempotent).
	 */
	ck_assert_ptr_null(reply.attrmask);
	ck_assert_uint_eq(reply.attrmask_len, 0);
	ck_assert_ptr_null(reply.attr_vals);
	ck_assert_uint_eq(reply.attr_vals_len, 0);
	ps_proxy_getattr_reply_free(&reply);
}
END_TEST

/*
 * Freeing a populated reply releases both buffers and zeroes the
 * struct -- the common path after a successful forward.  Heap
 * buffers simulate what ps_proxy_forward_getattr would have
 * allocated; LSan backstop catches a missing free if the helper
 * ever stops releasing one of the two fields.
 */
START_TEST(test_reply_free_populated)
{
	struct ps_proxy_getattr_reply reply;

	reply.attrmask = calloc(2, sizeof(*reply.attrmask));
	ck_assert_ptr_nonnull(reply.attrmask);
	reply.attrmask[0] = 0xDEADBEEF;
	reply.attrmask[1] = 0xCAFEBABE;
	reply.attrmask_len = 2;

	reply.attr_vals = calloc(8, 1);
	ck_assert_ptr_nonnull(reply.attr_vals);
	memset(reply.attr_vals, 0x55, 8);
	reply.attr_vals_len = 8;

	ps_proxy_getattr_reply_free(&reply);

	ck_assert_ptr_null(reply.attrmask);
	ck_assert_uint_eq(reply.attrmask_len, 0);
	ck_assert_ptr_null(reply.attr_vals);
	ck_assert_uint_eq(reply.attr_vals_len, 0);
}
END_TEST

/*
 * LOOKUP-forwarding arg validation.  Live-MDS coverage is deferred
 * to CI integration + slice 2e-iv-e (the op-handler hook).
 */
START_TEST(test_forward_lookup_null_args)
{
	uint8_t parent[] = { 0x11, 0x22, 0x33 };
	uint8_t child[PS_MAX_FH_SIZE];
	uint32_t child_len = 0;
	const char *name = "file.txt";
	uint32_t name_len = 8;

	ck_assert_int_eq(ps_proxy_forward_lookup(NULL, parent, sizeof(parent),
						 name, name_len, child,
						 sizeof(child), &child_len,
						 NULL, 0, NULL, NULL),
			 -EINVAL);
	ck_assert_int_eq(
		ps_proxy_forward_lookup((void *)1, NULL, sizeof(parent), name,
					name_len, child, sizeof(child),
					&child_len, NULL, 0, NULL, NULL),
		-EINVAL);
	ck_assert_int_eq(
		ps_proxy_forward_lookup((void *)1, parent, sizeof(parent), NULL,
					name_len, child, sizeof(child),
					&child_len, NULL, 0, NULL, NULL),
		-EINVAL);
	ck_assert_int_eq(
		ps_proxy_forward_lookup((void *)1, parent, sizeof(parent), name,
					name_len, NULL, sizeof(child),
					&child_len, NULL, 0, NULL, NULL),
		-EINVAL);
	ck_assert_int_eq(ps_proxy_forward_lookup((void *)1, parent,
						 sizeof(parent), name, name_len,
						 child, sizeof(child), NULL,
						 NULL, 0, NULL, NULL),
			 -EINVAL);
}
END_TEST

/*
 * Attr-request / attrs_out consistency: a caller that asks for
 * attrs must provide both the mask and the sink; either alone is a
 * programmer bug the primitive rejects before hitting the compound.
 */
START_TEST(test_forward_lookup_attr_mismatch)
{
	uint8_t parent[] = { 0x01 };
	uint8_t child[PS_MAX_FH_SIZE];
	uint32_t child_len = 0;
	uint32_t mask[] = { 0x2 };
	struct ps_proxy_attrs_min attrs = { 0 };

	/* mask provided, sink missing -> -EINVAL. */
	ck_assert_int_eq(ps_proxy_forward_lookup((void *)1, parent,
						 sizeof(parent), "x", 1, child,
						 sizeof(child), &child_len,
						 mask, 1, NULL, NULL),
			 -EINVAL);
	/* sink provided, mask missing -> -EINVAL. */
	ck_assert_int_eq(ps_proxy_forward_lookup((void *)1, parent,
						 sizeof(parent), "x", 1, child,
						 sizeof(child), &child_len,
						 NULL, 0, NULL, &attrs),
			 -EINVAL);
	/* mask+sink with zero mask length -> -EINVAL. */
	ck_assert_int_eq(ps_proxy_forward_lookup((void *)1, parent,
						 sizeof(parent), "x", 1, child,
						 sizeof(child), &child_len,
						 mask, 0, NULL, &attrs),
			 -EINVAL);
}
END_TEST

/*
 * Zero-length parent FH / name are protocol violations -- the MDS
 * would reject either; the primitive refuses to send.
 */
START_TEST(test_forward_lookup_zero_lengths)
{
	uint8_t parent[] = { 0x01 };
	uint8_t child[PS_MAX_FH_SIZE];
	uint32_t child_len = 0;
	const char *name = "x";

	ck_assert_int_eq(ps_proxy_forward_lookup((void *)1, parent, 0, name, 1,
						 child, sizeof(child),
						 &child_len, NULL, 0, NULL,
						 NULL),
			 -EINVAL);
	ck_assert_int_eq(ps_proxy_forward_lookup((void *)1, parent,
						 sizeof(parent), name, 0, child,
						 sizeof(child), &child_len,
						 NULL, 0, NULL, NULL),
			 -EINVAL);
}
END_TEST

/*
 * Parent FH larger than NFS4's 128-byte cap short-circuits before
 * the compound is built.  Matches the other PS primitives
 * (ps_sb_binding_alloc, ps_state_set_mds_root_fh,
 * ps_proxy_forward_getattr).
 */
START_TEST(test_forward_lookup_parent_fh_too_big)
{
	uint8_t big_parent[PS_MAX_FH_SIZE + 1];
	uint8_t child[PS_MAX_FH_SIZE];
	uint32_t child_len = 0;
	const char *name = "x";

	memset(big_parent, 0xAB, sizeof(big_parent));

	ck_assert_int_eq(ps_proxy_forward_lookup(
				 (void *)1, big_parent, sizeof(big_parent),
				 name, 1, child, sizeof(child), &child_len,
				 NULL, 0, NULL, NULL),
			 -E2BIG);
}
END_TEST

/*
 * Parser: empty reply -- a server that supports none of the requested
 * attrs returns attrmask_len=0 and attr_vals_len=0.  have_* must stay
 * false so the caller falls back to placeholders.
 */
START_TEST(test_parse_attrs_empty)
{
	struct ps_proxy_attrs_min out;

	ck_assert_int_eq(ps_proxy_parse_attrs_min(NULL, 0, NULL, 0, &out), 0);
	ck_assert(!out.have_type);
	ck_assert(!out.have_mode);
}
END_TEST

/*
 * Parser: FATTR4_TYPE alone (bit 1 -- word 0, bit mask 0x2).
 * attr_vals carries a single nfs_ftype4 big-endian (4 bytes).
 * NF4DIR = 2 per RFC 8881 S3.2.
 */
START_TEST(test_parse_attrs_type_only)
{
	uint32_t mask[] = { 0x00000002 };
	uint8_t vals[] = { 0x00, 0x00, 0x00, 0x02 };
	struct ps_proxy_attrs_min out;

	ck_assert_int_eq(
		ps_proxy_parse_attrs_min(mask, 1, vals, sizeof(vals), &out), 0);
	ck_assert(out.have_type);
	ck_assert_uint_eq(out.type, 2u);
	ck_assert(!out.have_mode);
}
END_TEST

/*
 * Parser: FATTR4_MODE alone (bit 33 -- word 1, bit mask 0x2).
 * attr_vals carries mode4 big-endian (4 bytes).  0o644 = 0x1A4.
 */
START_TEST(test_parse_attrs_mode_only)
{
	uint32_t mask[] = { 0x00000000, 0x00000002 };
	uint8_t vals[] = { 0x00, 0x00, 0x01, 0xA4 };
	struct ps_proxy_attrs_min out;

	ck_assert_int_eq(
		ps_proxy_parse_attrs_min(mask, 2, vals, sizeof(vals), &out), 0);
	ck_assert(!out.have_type);
	ck_assert(out.have_mode);
	ck_assert_uint_eq(out.mode, 0x1A4u);
}
END_TEST

/*
 * Parser: both TYPE and MODE, in ascending bit order -- type first
 * (bit 1) then mode (bit 33).  This is the request shape slice
 * 2e-iv-h-ii will use on every LOOKUP forward.
 */
START_TEST(test_parse_attrs_type_and_mode)
{
	uint32_t mask[] = { 0x00000002, 0x00000002 };
	uint8_t vals[] = {
		0x00, 0x00, 0x00, 0x02, /* NF4DIR */
		0x00, 0x00, 0x01, 0xED /* 0o755 */
	};
	struct ps_proxy_attrs_min out;

	ck_assert_int_eq(
		ps_proxy_parse_attrs_min(mask, 2, vals, sizeof(vals), &out), 0);
	ck_assert(out.have_type);
	ck_assert_uint_eq(out.type, 2u);
	ck_assert(out.have_mode);
	ck_assert_uint_eq(out.mode, 0x1EDu);
}
END_TEST

/*
 * Parser: unsupported bit -> -ENOTSUP.  FATTR4_CHANGE (bit 3) is a
 * real attribute but would require 8 bytes of decode we do not
 * carry.  Proves the parser refuses rather than mis-advancing the
 * cursor on unknown attrs.
 */
START_TEST(test_parse_attrs_unsupported_bit)
{
	uint32_t mask[] = { 0x00000008 }; /* bit 3 */
	uint8_t vals[] = { 0, 0, 0, 0, 0, 0, 0, 0 };
	struct ps_proxy_attrs_min out;

	ck_assert_int_eq(ps_proxy_parse_attrs_min(mask, 1, vals, sizeof(vals),
						  &out),
			 -ENOTSUP);
}
END_TEST

/*
 * Parser: truncated attr_vals -> -EINVAL.  Mask claims TYPE is
 * present but attr_vals has only 3 bytes (not enough for a 4-byte
 * nfs_ftype4).
 */
START_TEST(test_parse_attrs_short_buffer)
{
	uint32_t mask[] = { 0x00000002 };
	uint8_t vals[] = { 0x00, 0x00, 0x00 };
	struct ps_proxy_attrs_min out;

	ck_assert_int_eq(ps_proxy_parse_attrs_min(mask, 1, vals, sizeof(vals),
						  &out),
			 -EINVAL);
}
END_TEST

/*
 * Parser: trailing bytes beyond the last decoded attr -> -EINVAL.
 * Mask claims only TYPE but attr_vals carries 8 bytes -- framing
 * mismatch, reject rather than silently accept.
 */
START_TEST(test_parse_attrs_trailing_bytes)
{
	uint32_t mask[] = { 0x00000002 };
	uint8_t vals[] = { 0, 0, 0, 2, 0, 0, 0, 0 };
	struct ps_proxy_attrs_min out;

	ck_assert_int_eq(ps_proxy_parse_attrs_min(mask, 1, vals, sizeof(vals),
						  &out),
			 -EINVAL);
}
END_TEST

/*
 * Parser: mask declares an attr is present but attr_vals is empty.
 * Framing error the server never emits but a corrupted transport
 * could -- the short-buffer branch trips on the first requested
 * 4-byte decode and returns -EINVAL rather than reading past the
 * end of attr_vals.
 */
START_TEST(test_parse_attrs_mask_set_no_values)
{
	uint32_t mask[] = { 0x00000002 };
	struct ps_proxy_attrs_min out;

	ck_assert_int_eq(ps_proxy_parse_attrs_min(mask, 1, NULL, 0, &out),
			 -EINVAL);
}
END_TEST

/*
 * Parser: NULL out -> -EINVAL.  NULL attrmask with attrmask_len>0
 * or NULL attr_vals with attr_vals_len>0 are also framing errors
 * the caller should surface as -EINVAL rather than SEGV.
 */
START_TEST(test_parse_attrs_null_args)
{
	uint32_t mask[] = { 0x2 };
	uint8_t vals[] = { 0, 0, 0, 2 };
	struct ps_proxy_attrs_min out;

	ck_assert_int_eq(ps_proxy_parse_attrs_min(mask, 1, vals, sizeof(vals),
						  NULL),
			 -EINVAL);
	ck_assert_int_eq(ps_proxy_parse_attrs_min(NULL, 1, vals, sizeof(vals),
						  &out),
			 -EINVAL);
	ck_assert_int_eq(ps_proxy_parse_attrs_min(mask, 1, NULL, sizeof(vals),
						  &out),
			 -EINVAL);
}
END_TEST

/*
 * READ forwarder arg validation.  Live-MDS coverage is deferred to CI
 * integration; these guard the shortcuts that fire before any
 * compound is built.
 */
START_TEST(test_forward_read_null_args)
{
	uint8_t fh[] = { 0x11, 0x22 };
	uint8_t other[PS_STATEID_OTHER_SIZE] = { 0 };
	struct ps_proxy_read_reply reply;

	memset(&reply, 0, sizeof(reply));

	ck_assert_int_eq(ps_proxy_forward_read(NULL, fh, sizeof(fh), 0, other,
					       0, 4096, NULL, &reply),
			 -EINVAL);
	ck_assert_int_eq(ps_proxy_forward_read((void *)1, NULL, sizeof(fh), 0,
					       other, 0, 4096, NULL, &reply),
			 -EINVAL);
	ck_assert_int_eq(ps_proxy_forward_read((void *)1, fh, sizeof(fh), 0,
					       NULL, 0, 4096, NULL, &reply),
			 -EINVAL);
	ck_assert_int_eq(ps_proxy_forward_read((void *)1, fh, sizeof(fh), 0,
					       other, 0, 4096, NULL, NULL),
			 -EINVAL);
}
END_TEST

/*
 * Zero-length FH is a programmer error: the MDS would reject and the
 * primitive short-circuits locally.  Zero count is a no-op success
 * per RFC 8881 S18.22 (READ count=0 returns zero bytes); we
 * short-circuit before the compound is built so the MDS round-trip is
 * skipped.
 */
START_TEST(test_forward_read_zero_lengths)
{
	uint8_t fh[] = { 0x33 };
	uint8_t other[PS_STATEID_OTHER_SIZE] = { 0 };
	struct ps_proxy_read_reply reply;

	memset(&reply, 0, sizeof(reply));

	ck_assert_int_eq(ps_proxy_forward_read((void *)1, fh, 0, 0, other, 0,
					       4096, NULL, &reply),
			 -EINVAL);
	/* count == 0 is a valid no-op (RFC 8881 S18.22). */
	ck_assert_int_eq(ps_proxy_forward_read((void *)1, fh, sizeof(fh), 0,
					       other, 0, 0, NULL, &reply),
			 0);
}
END_TEST

/*
 * FH lengths above PS_MAX_FH_SIZE short-circuit before the compound
 * is built.  Matches the cap used by every other PS primitive.
 */
START_TEST(test_forward_read_fh_too_big)
{
	uint8_t big_fh[PS_MAX_FH_SIZE + 1];
	uint8_t other[PS_STATEID_OTHER_SIZE] = { 0 };
	struct ps_proxy_read_reply reply;

	memset(big_fh, 0xAB, sizeof(big_fh));
	memset(&reply, 0, sizeof(reply));

	ck_assert_int_eq(ps_proxy_forward_read((void *)1, big_fh,
					       sizeof(big_fh), 0, other, 0,
					       4096, NULL, &reply),
			 -E2BIG);
}
END_TEST

/*
 * read_reply_free is NULL-safe and idempotent.  Mirrors the getattr-
 * reply free helper contract.
 */
START_TEST(test_read_reply_free_null_safe)
{
	struct ps_proxy_read_reply reply = { 0 };

	ps_proxy_read_reply_free(NULL);
	ps_proxy_read_reply_free(&reply);

	ck_assert_ptr_null(reply.data);
	ck_assert_uint_eq(reply.data_len, 0);
	ck_assert(!reply.eof);
	ps_proxy_read_reply_free(&reply); /* idempotent */
}
END_TEST

/*
 * read_reply_free on a populated reply releases the data buffer and
 * zeroes the struct.  LSan backstop catches a missing free if the
 * helper ever stops releasing the field.
 */
START_TEST(test_read_reply_free_populated)
{
	struct ps_proxy_read_reply reply;

	reply.data = calloc(64, 1);
	ck_assert_ptr_nonnull(reply.data);
	memset(reply.data, 0xAA, 64);
	reply.data_len = 64;
	reply.eof = true;

	ps_proxy_read_reply_free(&reply);

	ck_assert_ptr_null(reply.data);
	ck_assert_uint_eq(reply.data_len, 0);
	ck_assert(!reply.eof);
}
END_TEST

/*
 * OPEN forwarder arg validation.  Live-MDS coverage is CI
 * integration; these guard the shortcuts before any compound is
 * built.
 */
START_TEST(test_forward_open_null_args)
{
	uint8_t parent[] = { 0x01, 0x02 };
	struct ps_proxy_open_request req = { 0 };
	struct ps_proxy_open_reply reply;

	memset(&reply, 0, sizeof(reply));

	ck_assert_int_eq(ps_proxy_forward_open(NULL, parent, sizeof(parent),
					       "f", 1, &req, NULL, &reply),
			 -EINVAL);
	ck_assert_int_eq(ps_proxy_forward_open((void *)1, NULL, sizeof(parent),
					       "f", 1, &req, NULL, &reply),
			 -EINVAL);
	ck_assert_int_eq(ps_proxy_forward_open((void *)1, parent,
					       sizeof(parent), NULL, 1, &req,
					       NULL, &reply),
			 -EINVAL);
	ck_assert_int_eq(ps_proxy_forward_open((void *)1, parent,
					       sizeof(parent), "f", 1, NULL,
					       NULL, &reply),
			 -EINVAL);
	ck_assert_int_eq(ps_proxy_forward_open((void *)1, parent,
					       sizeof(parent), "f", 1, &req,
					       NULL, NULL),
			 -EINVAL);
}
END_TEST

/*
 * Zero-length parent FH / name are programmer errors: the MDS would
 * reject either; the primitive refuses to send.
 */
START_TEST(test_forward_open_zero_lengths)
{
	uint8_t parent[] = { 0x01 };
	struct ps_proxy_open_request req = { 0 };
	struct ps_proxy_open_reply reply;

	memset(&reply, 0, sizeof(reply));

	ck_assert_int_eq(ps_proxy_forward_open((void *)1, parent, 0, "f", 1,
					       &req, NULL, &reply),
			 -EINVAL);
	ck_assert_int_eq(ps_proxy_forward_open((void *)1, parent,
					       sizeof(parent), "f", 0, &req,
					       NULL, &reply),
			 -EINVAL);
}
END_TEST

/*
 * Parent FH larger than PS_MAX_FH_SIZE short-circuits before the
 * compound is built.
 */
START_TEST(test_forward_open_parent_fh_too_big)
{
	uint8_t big_parent[PS_MAX_FH_SIZE + 1];
	struct ps_proxy_open_request req = { 0 };
	struct ps_proxy_open_reply reply;

	memset(big_parent, 0xAB, sizeof(big_parent));
	memset(&reply, 0, sizeof(reply));

	ck_assert_int_eq(ps_proxy_forward_open((void *)1, big_parent,
					       sizeof(big_parent), "f", 1, &req,
					       NULL, &reply),
			 -E2BIG);
}
END_TEST

/*
 * Owner_data bounds: NULL+len>0 and oversized both surface as
 * -EINVAL before the compound is built.  Protects the upstream from
 * amplification by a malformed end-client OPEN.
 */
START_TEST(test_forward_open_owner_bounds)
{
	uint8_t parent[] = { 0x01 };
	struct ps_proxy_open_request req = { 0 };
	struct ps_proxy_open_reply reply;

	memset(&reply, 0, sizeof(reply));

	/* owner_data_len > 0 with NULL owner_data. */
	req.owner_data = NULL;
	req.owner_data_len = 16;
	ck_assert_int_eq(ps_proxy_forward_open((void *)1, parent,
					       sizeof(parent), "f", 1, &req,
					       NULL, &reply),
			 -EINVAL);

	/* Oversized owner_data_len (> PS_PROXY_OPEN_OWNER_MAX = 512). */
	uint8_t dummy[1] = { 0 };

	req.owner_data = dummy;
	req.owner_data_len = 4096;
	ck_assert_int_eq(ps_proxy_forward_open((void *)1, parent,
					       sizeof(parent), "f", 1, &req,
					       NULL, &reply),
			 -EINVAL);
}
END_TEST

/*
 * WRITE forwarder arg validation -- symmetric to the READ forwarder.
 */
START_TEST(test_forward_write_null_args)
{
	uint8_t fh[] = { 0x11 };
	uint8_t other[PS_STATEID_OTHER_SIZE] = { 0 };
	uint8_t data[] = { 0xCA, 0xFE };
	struct ps_proxy_write_reply reply;

	memset(&reply, 0, sizeof(reply));

	ck_assert_int_eq(ps_proxy_forward_write(NULL, fh, sizeof(fh), 0, other,
						0, 0, data, sizeof(data), NULL,
						&reply),
			 -EINVAL);
	ck_assert_int_eq(ps_proxy_forward_write((void *)1, NULL, sizeof(fh), 0,
						other, 0, 0, data, sizeof(data),
						NULL, &reply),
			 -EINVAL);
	ck_assert_int_eq(ps_proxy_forward_write((void *)1, fh, sizeof(fh), 0,
						NULL, 0, 0, data, sizeof(data),
						NULL, &reply),
			 -EINVAL);
	ck_assert_int_eq(ps_proxy_forward_write((void *)1, fh, sizeof(fh), 0,
						other, 0, 0, NULL, sizeof(data),
						NULL, &reply),
			 -EINVAL);
	ck_assert_int_eq(ps_proxy_forward_write((void *)1, fh, sizeof(fh), 0,
						other, 0, 0, data, sizeof(data),
						NULL, NULL),
			 -EINVAL);
}
END_TEST

START_TEST(test_forward_write_zero_lengths)
{
	uint8_t fh[] = { 0x33 };
	uint8_t other[PS_STATEID_OTHER_SIZE] = { 0 };
	uint8_t data[] = { 0xAA };
	struct ps_proxy_write_reply reply;

	memset(&reply, 0, sizeof(reply));

	ck_assert_int_eq(ps_proxy_forward_write((void *)1, fh, 0, 0, other, 0,
						0, data, sizeof(data), NULL,
						&reply),
			 -EINVAL);
	ck_assert_int_eq(ps_proxy_forward_write((void *)1, fh, sizeof(fh), 0,
						other, 0, 0, data, 0, NULL,
						&reply),
			 -EINVAL);
}
END_TEST

START_TEST(test_forward_write_fh_too_big)
{
	uint8_t big_fh[PS_MAX_FH_SIZE + 1];
	uint8_t other[PS_STATEID_OTHER_SIZE] = { 0 };
	uint8_t data[] = { 0 };
	struct ps_proxy_write_reply reply;

	memset(big_fh, 0xAB, sizeof(big_fh));
	memset(&reply, 0, sizeof(reply));

	ck_assert_int_eq(ps_proxy_forward_write(
				 (void *)1, big_fh, sizeof(big_fh), 0, other, 0,
				 0, data, sizeof(data), NULL, &reply),
			 -E2BIG);
}
END_TEST

/*
 * stable_how4 values are UNSTABLE4=0, DATA_SYNC4=1, FILE_SYNC4=2.
 * Anything outside 0..2 is programmer error and short-circuits
 * before the compound is built.
 */
START_TEST(test_forward_write_bad_stable)
{
	uint8_t fh[] = { 0x44 };
	uint8_t other[PS_STATEID_OTHER_SIZE] = { 0 };
	uint8_t data[] = { 0 };
	struct ps_proxy_write_reply reply;

	memset(&reply, 0, sizeof(reply));

	ck_assert_int_eq(ps_proxy_forward_write((void *)1, fh, sizeof(fh), 0,
						other, 0, 99, data,
						sizeof(data), NULL, &reply),
			 -EINVAL);
}
END_TEST

/*
 * CLOSE forwarder arg validation -- same shape as READ/WRITE.
 */
START_TEST(test_forward_close_null_args)
{
	uint8_t fh[] = { 0x11 };
	uint8_t other[PS_STATEID_OTHER_SIZE] = { 0 };
	struct ps_proxy_close_reply reply;

	memset(&reply, 0, sizeof(reply));

	ck_assert_int_eq(ps_proxy_forward_close(NULL, fh, sizeof(fh), 0, 0,
						other, NULL, &reply),
			 -EINVAL);
	ck_assert_int_eq(ps_proxy_forward_close((void *)1, NULL, sizeof(fh), 0,
						0, other, NULL, &reply),
			 -EINVAL);
	ck_assert_int_eq(ps_proxy_forward_close((void *)1, fh, sizeof(fh), 0, 0,
						NULL, NULL, &reply),
			 -EINVAL);
	ck_assert_int_eq(ps_proxy_forward_close((void *)1, fh, sizeof(fh), 0, 0,
						other, NULL, NULL),
			 -EINVAL);
}
END_TEST

START_TEST(test_forward_close_zero_fh)
{
	uint8_t fh[] = { 0x33 };
	uint8_t other[PS_STATEID_OTHER_SIZE] = { 0 };
	struct ps_proxy_close_reply reply;

	memset(&reply, 0, sizeof(reply));

	ck_assert_int_eq(ps_proxy_forward_close((void *)1, fh, 0, 0, 0, other,
						NULL, &reply),
			 -EINVAL);
}
END_TEST

START_TEST(test_forward_close_fh_too_big)
{
	uint8_t big_fh[PS_MAX_FH_SIZE + 1];
	uint8_t other[PS_STATEID_OTHER_SIZE] = { 0 };
	struct ps_proxy_close_reply reply;

	memset(big_fh, 0xAB, sizeof(big_fh));
	memset(&reply, 0, sizeof(reply));

	ck_assert_int_eq(ps_proxy_forward_close((void *)1, big_fh,
						sizeof(big_fh), 0, 0, other,
						NULL, &reply),
			 -E2BIG);
}
END_TEST

static Suite *ps_proxy_ops_suite(void)
{
	Suite *s = suite_create("ps_proxy_ops");
	TCase *tc = tcase_create("core");

	tcase_add_test(tc, test_forward_getattr_null_args);
	tcase_add_test(tc, test_forward_getattr_zero_lengths);
	tcase_add_test(tc, test_forward_getattr_fh_too_big);
	tcase_add_test(tc, test_reply_free_null_safe);
	tcase_add_test(tc, test_reply_free_populated);
	tcase_add_test(tc, test_forward_lookup_null_args);
	tcase_add_test(tc, test_forward_lookup_attr_mismatch);
	tcase_add_test(tc, test_forward_lookup_zero_lengths);
	tcase_add_test(tc, test_forward_lookup_parent_fh_too_big);
	tcase_add_test(tc, test_parse_attrs_empty);
	tcase_add_test(tc, test_parse_attrs_type_only);
	tcase_add_test(tc, test_parse_attrs_mode_only);
	tcase_add_test(tc, test_parse_attrs_type_and_mode);
	tcase_add_test(tc, test_parse_attrs_unsupported_bit);
	tcase_add_test(tc, test_parse_attrs_short_buffer);
	tcase_add_test(tc, test_parse_attrs_trailing_bytes);
	tcase_add_test(tc, test_parse_attrs_mask_set_no_values);
	tcase_add_test(tc, test_parse_attrs_null_args);
	tcase_add_test(tc, test_forward_read_null_args);
	tcase_add_test(tc, test_forward_read_zero_lengths);
	tcase_add_test(tc, test_forward_read_fh_too_big);
	tcase_add_test(tc, test_read_reply_free_null_safe);
	tcase_add_test(tc, test_read_reply_free_populated);
	tcase_add_test(tc, test_forward_open_null_args);
	tcase_add_test(tc, test_forward_open_zero_lengths);
	tcase_add_test(tc, test_forward_open_parent_fh_too_big);
	tcase_add_test(tc, test_forward_open_owner_bounds);
	tcase_add_test(tc, test_forward_write_null_args);
	tcase_add_test(tc, test_forward_write_zero_lengths);
	tcase_add_test(tc, test_forward_write_fh_too_big);
	tcase_add_test(tc, test_forward_write_bad_stable);
	tcase_add_test(tc, test_forward_close_null_args);
	tcase_add_test(tc, test_forward_close_zero_fh);
	tcase_add_test(tc, test_forward_close_fh_too_big);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int failed;
	SRunner *sr = srunner_create(ps_proxy_ops_suite());

	srunner_run_all(sr, CK_NORMAL);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return failed == 0 ? 0 : 1;
}
