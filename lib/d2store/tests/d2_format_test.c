/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "d1_digest.h"
#include "d2_format.h"

#define D2_ENTRY_PREDECESSOR_PRESENT_OFFSET 360u

static int failures;

static void check(bool ok, const char *what)
{
	if (!ok) {
		fprintf(stderr, "FAIL: %s\n", what);
		failures++;
	}
}

static void fill(uint8_t *p, size_t len, uint8_t first)
{
	size_t i;

	for (i = 0; i < len; i++)
		p[i] = (uint8_t)(first + i);
}

static void put_u32(uint8_t out[4], uint32_t value)
{
	out[0] = (uint8_t)(value >> 24);
	out[1] = (uint8_t)(value >> 16);
	out[2] = (uint8_t)(value >> 8);
	out[3] = (uint8_t)value;
}

static uint32_t get_u32(const uint8_t in[4])
{
	return ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) |
	       ((uint32_t)in[2] << 8) | in[3];
}

static void key_init(struct d2_key_block *k, uint8_t first)
{
	fill(k->session, sizeof(k->session), first);
	k->slot = 3;
	k->sequence = 7;
	k->compound_ordinal = 2;
	d2_operation_key(k->session, k->slot, k->sequence, k->compound_ordinal,
			 k->operation_key);
	fill(k->request_digest, sizeof(k->request_digest), first + 20);
}

static void admission_init(struct d2_admission_block *a, uint8_t first)
{
	fill(a->session, sizeof(a->session), first);
	fill(a->principal, sizeof(a->principal), first + 1);
	fill(a->issuer, sizeof(a->issuer), first + 2);
	a->authority_epoch = 9;
	a->fence_sequence = 10;
	a->lease_epoch = 11;
	a->client_id = 12;
	a->stateid_seqid = 13;
	fill(a->stateid_other, sizeof(a->stateid_other), first + 3);
	a->writer = 14;
	a->rights = 15;
}

static void wal_init(struct d2_wal_header *h, uint16_t family, uint32_t total)
{
	memset(h, 0, sizeof(*h));
	h->family = family;
	h->total_bytes = total;
	fill(h->store_uuid, sizeof(h->store_uuid), 0x10);
	fill(h->wal_uuid, sizeof(h->wal_uuid), 0x30);
	h->lsn = 1;
	h->ds_incarnation = 1;
}

static void test_super(void)
{
	struct d2_superblock a = { 0 }, b;
	uint8_t bytes[D2_SB_SLOT_BYTES];

	fill(a.store_uuid, 16, 0x10);
	fill(a.fs_uuid, 16, 0x20);
	fill(a.export_uuid, 16, 0x30);
	fill(a.wal_uuid, 16, 0x40);
	fill(a.payload_uuid, 16, 0x50);
	fill(a.binding_token_digest, 32, 0x60);
	a.generation = 2;
	a.root_handle_type = 0;
	a.root_handle_len = 0;
	a.ds_incarnation = 1;
	a.capacity_wal_bytes = D2_MIN_WAL_BYTES;
	a.capacity_payload_bytes = 1u << 20;
	a.payload_durable_bytes = D2_PAYLOAD_ALIGN;
	a.state = D2_SB_CLEAN;
	a.format_floor = D2_FORMAT_VERSION;
	check(d2_super_encode(&a, bytes), "superblock encodes");
	check(get_u32(bytes + 356) == 0xb8821966u,
	      "kernel super vector CRC is pinned");
	check(d2_super_decode(bytes, a.store_uuid, &b), "superblock decodes");
	check(b.generation == a.generation && b.state == a.state,
	      "superblock values round trip");
	bytes[100] ^= 1;
	check(!d2_super_decode(bytes, a.store_uuid, &b),
	      "superblock CRC rejects mutation");
}

static void test_payload(void)
{
	struct d2_payload_header a = { 0 }, b;
	struct d2_payload_object object = { 0 }, decoded;
	uint8_t header[D2_PAYLOAD_ALIGN];
	uint8_t content[4096], *bytes;
	bool content_ok;
	size_t written;

	fill(a.store_uuid, 16, 0x10);
	fill(a.payload_uuid, 16, 0x20);
	fill(a.wal_uuid, 16, 0x30);
	check(d2_payload_header_encode(&a, header), "payload header encodes");
	check(get_u32(header + 80) == 0xcb7eada9u,
	      "kernel payload-header vector CRC is pinned");
	check(d2_payload_header_decode(header, a.store_uuid, &b),
	      "payload header decodes");
	fill(content, sizeof(content), 0x40);
	memcpy(object.store_uuid, a.store_uuid, 16);
	object.payload_object_id = ((uint64_t)1 << 40) | 7;
	object.content_alg = 2;
	object.content_ck_len = 4;
	{
		uint32_t crc = d1_crc32c(content, sizeof(content));

		object.content_ck[0] = (uint8_t)(crc >> 24);
		object.content_ck[1] = (uint8_t)(crc >> 16);
		object.content_ck[2] = (uint8_t)(crc >> 8);
		object.content_ck[3] = (uint8_t)crc;
	}
	object.content_len = sizeof(content);
	object.content = content;
	bytes = calloc(1, d2_payload_object_bytes(object.content_len));
	check(bytes != NULL, "payload allocation succeeds");
	if (!bytes)
		return;
	check(d2_payload_encode(&object, bytes,
				d2_payload_object_bytes(object.content_len),
				&written),
	      "payload object encodes");
	check(written == 8192, "4096-byte content occupies 8192 bytes");
	check(d2_payload_decode(bytes, written, a.store_uuid, &decoded),
	      "payload object decodes");
	object.content_ck[3] ^= 1;
	check(!d2_payload_encode(&object, bytes,
				 d2_payload_object_bytes(object.content_len),
				 &written),
	      "payload rejects wire checksum mismatch");
	object.content_ck[3] ^= 1;
	object.content_alg = 99;
	check(!d2_payload_encode(&object, bytes,
				 d2_payload_object_bytes(object.content_len),
				 &written),
	      "payload rejects unsupported checksum tag");
	check(decoded.payload_object_id == object.payload_object_id &&
		      decoded.content_len == sizeof(content),
	      "payload values round trip");
	bytes[D2_PAYLOAD_HEADER_BYTES + 9] ^= 1;
	check(!d2_payload_decode(bytes, written, a.store_uuid, &decoded),
	      "payload content CRC rejects mutation");
	check(d2_payload_decode_content(bytes, written, a.store_uuid, &decoded,
					&content_ok) &&
		      !content_ok &&
		      decoded.payload_object_id == object.payload_object_id,
	      "payload framing identifies isolated content damage");
	bytes[36] ^= 1;
	check(!d2_payload_decode_content(bytes, written, a.store_uuid, &decoded,
					 &content_ok),
	      "payload header damage remains structural");
	free(bytes);
}

static void test_kernel_payload_vectors(void)
{
	static const struct {
		uint32_t algorithm;
		uint32_t len;
		uint8_t checksum[32];
	} vectors[] = {
		{ 1, 4, { 0xe6, 0x92, 0x5d, 0x7b } },
		{ 3,
		  32,
		  {
			  0, 0, 0, 0, 0x43, 0x42, 0x41, 0x40,
			  0, 0, 0, 0, 0x43, 0x42, 0x41, 0x40,
			  0, 0, 0, 0, 0x43, 0x42, 0x41, 0x40,
			  0, 0, 0, 0, 0x43, 0x42, 0x41, 0x40,
		  } },
		{ 4,
		  32,
		  {
			  0xae, 0xd5, 0xd0, 0xd7, 0xbf, 0x85, 0xa4, 0x04,
			  0x2c, 0x67, 0xfc, 0xc7, 0x3f, 0xba, 0xd1, 0x8a,
			  0x1c, 0xc4, 0x04, 0xc4, 0x7f, 0x41, 0x7b, 0x40,
			  0x8d, 0xc4, 0x75, 0x13, 0x86, 0x22, 0xe3, 0x90,
		  } },
	};
	struct d2_payload_object object = { 0 };
	uint8_t content[] = { 0x40, 0x41, 0x42, 0x43 };
	uint8_t bytes[D2_PAYLOAD_ALIGN];
	size_t written;
	unsigned int i;

	fill(object.store_uuid, 16, 0x10);
	object.payload_object_id = ((uint64_t)1 << 40) | 7;
	object.content_len = sizeof(content);
	object.content = content;
	for (i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
		object.content_alg = vectors[i].algorithm;
		object.content_ck_len = vectors[i].len;
		memset(object.content_ck, 0, sizeof(object.content_ck));
		memcpy(object.content_ck, vectors[i].checksum, vectors[i].len);
		check(d2_payload_encode(&object, bytes, sizeof(bytes),
					&written),
		      "kernel payload checksum vector encodes");
	}
	object.content_alg = 2;
	object.content_ck_len = 4;
	memset(object.content_ck, 0, sizeof(object.content_ck));
	put_u32(object.content_ck, 0x0b9c1e25u);
	check(d2_payload_encode(&object, bytes, sizeof(bytes), &written),
	      "kernel CRC32C payload vector encodes");
	check(get_u32(bytes + 44) == 0x0b9c1e25u &&
		      get_u32(bytes + 140) == 0x2bd696d8u,
	      "kernel payload vector CRCs are pinned");
}

static void test_start(void)
{
	struct d2_wal_header h, got_h;
	struct d2_start a = { 0 }, b;
	uint8_t bytes[D2_START_RECORD_BYTES];

	wal_init(&h, D2_REC_START, sizeof(bytes));
	a.ds_incarnation = 1;
	a.prev_incarnation = 0;
	a.recovery_decision = D2_RD_FIRST_PROVISION;
	a.wal_append_cursor = D2_START_RECORD_BYTES;
	a.payload_append_cursor = D2_PAYLOAD_ALIGN;
	a.capacity_wal_bytes = D2_MIN_WAL_BYTES;
	a.capacity_payload_bytes = 1u << 20;
	fill(a.export_uuid, 16, 0x70);
	check(d2_start_encode(&h, &a, bytes), "START encodes");
	check(get_u32(bytes + 193) == 0x31544e4eu,
	      "kernel START vector CRC is pinned");
	check(d2_wal_header_decode(bytes, sizeof(bytes), h.store_uuid,
				   h.wal_uuid, &got_h),
	      "START WAL header decodes");
	check(d2_start_decode(bytes, sizeof(bytes), &got_h, &b),
	      "START body decodes");
	check(b.wal_append_cursor == sizeof(bytes), "START cursor round trips");
	bytes[68] = 1;
	check(!d2_wal_header_decode(bytes, sizeof(bytes), h.store_uuid,
				    h.wal_uuid, &got_h),
	      "WAL reserved field rejects mutation");
}

static void test_entry(void)
{
	struct d2_wal_header h, got_h;
	struct d2_entry a = { 0 }, b;
	uint8_t bytes[D2_ENTRY_RECORD_BYTES];

	wal_init(&h, D2_REC_ENTRY, sizeof(bytes));
	a.transition = D2_COMMITTED;
	a.status = 1;
	a.disposition = 1;
	a.stability = 3;
	a.batch_count = 1;
	fill(a.file_key, 32, 0x10);
	a.chunk_index = 1;
	a.txn_id = 3;
	key_init(&a.key, 0x20);
	admission_init(&a.admission, 0x40);
	a.owner_cohort = 7;
	a.owner_client_id = a.admission.writer;
	a.owner_co_id = 12;
	a.generation = 1;
	a.extent_kind = 2;
	a.extent_high_water = 8192;
	a.extent_highest_index = 1;
	a.index_generation = 1;
	a.payload_object_id = ((uint64_t)1 << 40) | 7;
	a.payload_object_offset = D2_PAYLOAD_ALIGN;
	a.payload_content_len = 4096;
	a.result_activated = true;
	a.result_visible_object_id = 1;
	a.result_effective_len = 4096;
	a.writer_payload_id = 0x4242;
	a.result_ck_alg = 2;
	a.result_ck_len = 4;
	fill(a.result_ck, 4, 0x80);
	a.result_guard_generation = 1;
	a.result_guard_writer = a.admission.writer;
	check(d2_entry_encode(&h, &a, bytes), "ENTRY encodes");
	check(d2_wal_header_decode(bytes, sizeof(bytes), h.store_uuid,
				   h.wal_uuid, &got_h) &&
		      d2_entry_decode(bytes, sizeof(bytes), &got_h, &b),
	      "ENTRY decodes");
	check(b.extent_high_water == 8192 && b.result_ck_len == 4 &&
		      b.writer_payload_id == 0x4242,
	      "ENTRY values round trip");
	a.predecessor_object_id = 1;
	check(!d2_entry_encode(&h, &a, bytes),
	      "ENTRY rejects absent predecessor with governed state");
	a.predecessor_object_id = 0;
	a.postcond_id = 1;
	check(!d2_entry_encode(&h, &a, bytes),
	      "ENTRY rejects absent postcondition with governed state");
	a.postcond_id = 0;
	a.predecessor_present = true;
	a.predecessor_object_id = 1;
	a.predecessor_generation = 2;
	check(d2_entry_encode(&h, &a, bytes),
	      "ENTRY with governed predecessor encodes");
	bytes[D2_ENTRY_PREDECESSOR_PRESENT_OFFSET] = 0;
	put_u32(bytes + sizeof(bytes) - 4,
		d2_crc32c_domain("FFV2-D2-WAL-v1", bytes, sizeof(bytes) - 4));
	check(d2_wal_header_decode(bytes, sizeof(bytes), h.store_uuid,
				   h.wal_uuid, &got_h) &&
		      !d2_entry_decode(bytes, sizeof(bytes), &got_h, &b),
	      "ENTRY decoder rejects clear predecessor presence cell");
	a.predecessor_present = false;
	a.predecessor_object_id = 0;
	a.predecessor_generation = 0;
	a.transition = D2_ADMITTED;
	check(!d2_entry_encode(&h, &a, bytes),
	      "ENTRY rejects inadmissible transition and status pair");
	a.transition = D2_COMMITTED;
	bytes[200] ^= 1;
	check(!d2_wal_header_decode(bytes, sizeof(bytes), h.store_uuid,
				    h.wal_uuid, &got_h),
	      "ENTRY CRC rejects mutation");
}

static void test_cohort(void)
{
	struct d2_wal_header h, got_h;
	struct d2_cohort a = { 0 }, b;
	uint8_t bytes[D2_MAX_RECORD_BYTES];
	size_t written;
	unsigned int i;

	wal_init(&h, D2_REC_COHORT, 0);
	a.transition = D2_ADMITTED;
	a.status = 1;
	a.disposition = 1;
	a.cohort_id = 1;
	a.member_count = 2;
	key_init(&a.key, 0x20);
	admission_init(&a.admission, 0x40);
	for (i = 0; i < a.member_count; i++) {
		a.members[i].repair_mode = 2;
		fill(a.members[i].file_key, 32, (uint8_t)(0x60 + i));
		a.members[i].chunk_index = i;
		a.members[i].member_txn_id = i + 1;
		a.members[i].extent_kind = D2_EXTENT_UNCHANGED;
	}
	check(d2_cohort_encode(&h, &a, bytes, sizeof(bytes), &written),
	      "COHORT encodes");
	check(written == 820, "two-member COHORT is 820 bytes");
	check(d2_wal_header_decode(bytes, written, h.store_uuid, h.wal_uuid,
				   &got_h) &&
		      d2_cohort_decode(bytes, written, &got_h, &b),
	      "COHORT decodes");
	check(b.member_count == 2 && b.members[1].chunk_index == 1,
	      "COHORT members round trip");
	a.transition = D2_ABORTED;
	a.status = D1_STALE_AUTH;
	a.flags = 2;
	check(!d2_cohort_encode(&h, &a, bytes, sizeof(bytes), &written),
	      "COHORT rejects inadmissible kept-abort status");
	a.transition = D2_ADMITTED;
	a.status = D1_OK;
	a.flags = 4;
	check(!d2_cohort_encode(&h, &a, bytes, sizeof(bytes), &written),
	      "COHORT rejects reserved flag bits");
}

static void store_key(struct d2_control *c, uint64_t incarnation,
		      uint32_t ordinal)
{
	memset(&c->key, 0, sizeof(c->key));
	c->key.sequence = ordinal;
	c->key.operation_key[0] = (uint8_t)(incarnation >> 56);
	c->key.operation_key[1] = (uint8_t)(incarnation >> 48);
	c->key.operation_key[2] = (uint8_t)(incarnation >> 40);
	c->key.operation_key[3] = (uint8_t)(incarnation >> 32);
	c->key.operation_key[4] = (uint8_t)(incarnation >> 24);
	c->key.operation_key[5] = (uint8_t)(incarnation >> 16);
	c->key.operation_key[6] = (uint8_t)(incarnation >> 8);
	c->key.operation_key[7] = (uint8_t)incarnation;
	c->key.operation_key[12] = (uint8_t)(ordinal >> 24);
	c->key.operation_key[13] = (uint8_t)(ordinal >> 16);
	c->key.operation_key[14] = (uint8_t)(ordinal >> 8);
	c->key.operation_key[15] = (uint8_t)ordinal;
}

static void test_control(void)
{
	struct d2_wal_header h, got_h;
	struct d2_control a = { 0 }, b;
	uint8_t bytes[D2_MAX_RECORD_BYTES];
	size_t written;

	wal_init(&h, D2_REC_CONTROL, 0);
	a.subtype = D2_CTL_FILE_REGISTER;
	a.transition = D2_COMMITTED;
	a.status = 1;
	a.body_len = 184;
	store_key(&a, h.ds_incarnation, 1);
	fill(a.body + 36, 32, 0x70);
	d2_file_key(a.body + 36, 32, a.body);
	a.body[35] = 32;
	a.body[166] = 0x10;
	a.body[175] = 8;
	check(d2_control_encode(&h, &a, bytes, sizeof(bytes), &written),
	      "fixed CONTROL encodes");
	check(written == 396, "FILE_REGISTER is 396 bytes");
	check(d2_wal_header_decode(bytes, written, h.store_uuid, h.wal_uuid,
				   &got_h) &&
		      d2_control_decode(bytes, written, &got_h, &b),
	      "fixed CONTROL decodes");
	a.body[183] = 1;
	check(!d2_control_encode(&h, &a, bytes, sizeof(bytes), &written),
	      "FILE_REGISTER rejects nonzero initial EOF");
	memset(&a, 0, sizeof(a));
	a.subtype = D2_CTL_RECOVERY_ADMIT;
	a.transition = D2_COMMITTED;
	a.status = 1;
	a.body_len = 4 + 2 * 40;
	key_init(&a.key, 0x20);
	a.body[3] = 2;
	a.body[11] = 1;
	a.body[51] = 2;
	check(d2_control_encode(&h, &a, bytes, sizeof(bytes), &written),
	      "counted CONTROL encodes");
	a.body[51] = 1;
	check(!d2_control_encode(&h, &a, bytes, sizeof(bytes), &written),
	      "counted CONTROL rejects duplicate keys");
	memset(&a, 0, sizeof(a));
	a.subtype = D2_CTL_CUSTODY;
	a.transition = D2_COMMITTED;
	a.status = D1_OK;
	a.body_len = 36;
	key_init(&a.key, 0x30);
	a.body[19] = 1;
	fill(a.body + 20, 16, 0x80);
	check(d2_control_encode(&h, &a, bytes, sizeof(bytes), &written),
	      "CUSTODY reads its operation at offset 16");
	a.body[19] = 0;
	check(!d2_control_encode(&h, &a, bytes, sizeof(bytes), &written),
	      "CUSTODY rejects an invalid operation");
}

int main(void)
{
	test_super();
	test_payload();
	test_kernel_payload_vectors();
	test_start();
	test_entry();
	test_cohort();
	test_control();
	if (failures)
		fprintf(stderr, "%d d2 format checks failed\n", failures);
	else
		printf("D2 FORMAT: 39 checks pass\n");
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
