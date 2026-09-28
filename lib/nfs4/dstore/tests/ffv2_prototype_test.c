/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <check.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <openssl/sha.h>
#include <rpc/xdr.h>

#include "nfsv42_xdr.h"
#include "reffs/dstore.h"
#include "reffs/ffv2_prototype.h"
#include "reffs/fixed_inventory.h"
#include "reffs/fixed_layout.h"
#include "reffs/inode.h"
#include "reffs/layout_segment.h"
#include "reffs/super_block.h"
#include "libreffs_test.h"
#include "nfs4/client.h"
#include "nfs4/compound.h"
#include "nfs4/ops.h"

#include "ffv2_prototype_internal.h"

nfsstat4 layoutget_build_v2(struct layout_segment *seg,
			    uint32_t ffv2m_coding_type, uint32_t writer_id,
			    uint64_t pnfs_clientid,
			    const stateid4 *layout_stateid, char **out_body,
			    u_long *out_size);

enum {
	A_NONCE = 1,
	A_EXPIRES,
	A_SOURCE_UUID = 8,
	A_SERVICE_UUID,
	A_REPLAY_UUID,
	A_GENERATION,
	A_OBJECT_STATEID = 4,
	A_OBJECTS = 19,
	A_OBJECT_MAPPED_HANDLE = 3,
};

struct reply_builder {
	uint8_t bytes[2048];
	size_t len;
};

struct netlink_reply {
	uint8_t bytes[512];
	size_t len;
};

struct netlink_datagram {
	struct sockaddr_nl peer;
	socklen_t peer_len;
	const uint8_t *data;
	size_t data_len;
};

struct netlink_test_context {
	const struct netlink_datagram *datagrams;
	size_t datagram_count;
	size_t next_datagram;
};

static int netlink_test_open(void *context __attribute__((unused)))
{
	return 1;
}

static int netlink_test_bind(void *context __attribute__((unused)),
			     int fd __attribute__((unused)),
			     const struct sockaddr *address
			     __attribute__((unused)),
			     socklen_t address_len __attribute__((unused)))
{
	return 0;
}

static ssize_t netlink_test_send(void *context __attribute__((unused)),
				 int fd __attribute__((unused)),
				 const void *buffer __attribute__((unused)),
				 size_t len,
				 const struct sockaddr *address
				 __attribute__((unused)),
				 socklen_t address_len __attribute__((unused)))
{
	return (ssize_t)len;
}

static ssize_t netlink_test_receive(void *opaque,
				    int fd __attribute__((unused)),
				    void *buffer, size_t len,
				    struct sockaddr *address,
				    socklen_t *address_len)
{
	struct netlink_test_context *context = opaque;
	const struct netlink_datagram *datagram;

	if (context->next_datagram == context->datagram_count) {
		errno = ETIMEDOUT;
		return -1;
	}
	datagram = &context->datagrams[context->next_datagram++];
	if (datagram->data_len > len || datagram->peer_len > *address_len) {
		errno = EMSGSIZE;
		return -1;
	}
	memcpy(buffer, datagram->data, datagram->data_len);
	memcpy(address, &datagram->peer, datagram->peer_len);
	*address_len = datagram->peer_len;
	return (ssize_t)datagram->data_len;
}

static int netlink_test_close(void *context __attribute__((unused)),
			      int fd __attribute__((unused)))
{
	return 0;
}

static int netlink_open(const struct netlink_datagram *datagrams,
			size_t datagram_count, uint16_t *family)
{
	struct netlink_test_context context = {
		.datagrams = datagrams,
		.datagram_count = datagram_count,
	};
	const struct ffv2_prototype_nl_io io = {
		.context = &context,
		.open = netlink_test_open,
		.bind = netlink_test_bind,
		.send = netlink_test_send,
		.receive = netlink_test_receive,
		.close = netlink_test_close,
	};

	return ffv2_prototype_nl_open_test(&io, family);
}

static void build_family_reply(struct netlink_reply *reply, uint32_t seq,
			       uint32_t portid, uint16_t type, uint16_t family)
{
	struct nlmsghdr *message;
	struct genlmsghdr *generic;
	struct nlattr *attribute;
	struct nlmsgerr *ack;
	size_t offset;

	memset(reply, 0, sizeof(*reply));
	message = (struct nlmsghdr *)reply->bytes;
	message->nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
	message->nlmsg_type = type;
	message->nlmsg_seq = seq;
	message->nlmsg_pid = portid;
	generic = NLMSG_DATA(message);
	generic->cmd = CTRL_CMD_NEWFAMILY;
	offset = NLMSG_ALIGN(message->nlmsg_len);
	attribute = (struct nlattr *)(reply->bytes + offset);
	attribute->nla_type = CTRL_ATTR_FAMILY_ID;
	attribute->nla_len = NLA_HDRLEN + sizeof(family);
	memcpy((uint8_t *)attribute + NLA_HDRLEN, &family, sizeof(family));
	message->nlmsg_len = offset + NLA_ALIGN(attribute->nla_len);

	offset = NLMSG_ALIGN(message->nlmsg_len);
	message = (struct nlmsghdr *)(reply->bytes + offset);
	message->nlmsg_len = NLMSG_LENGTH(sizeof(*ack));
	message->nlmsg_type = NLMSG_ERROR;
	message->nlmsg_seq = seq;
	message->nlmsg_pid = portid;
	ack = NLMSG_DATA(message);
	ack->error = 0;
	reply->len = offset + NLMSG_ALIGN(message->nlmsg_len);
}

static struct netlink_datagram
family_datagram(const struct netlink_reply *reply, uint32_t peer_portid)
{
	return (struct netlink_datagram){
		.peer = {
			.nl_family = AF_NETLINK,
			.nl_pid = peer_portid,
		},
		.peer_len = sizeof(struct sockaddr_nl),
		.data = reply->bytes,
		.data_len = reply->len,
	};
}

START_TEST(test_nl_open_accepts_kernel_reply_with_requester_header_portid)
{
	struct netlink_reply reply;
	struct netlink_datagram datagram;
	uint16_t family = 0;

	build_family_reply(&reply, 1, 164, GENL_ID_CTRL, 55);
	datagram = family_datagram(&reply, 0);
	ck_assert_int_eq(netlink_open(&datagram, 1, &family), 0);
	ck_assert_uint_eq(family, 55);
}
END_TEST

START_TEST(test_nl_open_rejects_non_kernel_peer_and_wrong_message_type)
{
	struct netlink_reply reply;
	struct netlink_datagram datagram;
	uint16_t family = 0;

	build_family_reply(&reply, 1, 164, GENL_ID_CTRL, 55);
	datagram = family_datagram(&reply, 7);
	ck_assert_int_eq(netlink_open(&datagram, 1, &family), -EBADMSG);

	build_family_reply(&reply, 1, 164, GENL_ID_CTRL + 1, 55);
	datagram = family_datagram(&reply, 0);
	ck_assert_int_eq(netlink_open(&datagram, 1, &family), -EBADMSG);
}
END_TEST

START_TEST(test_nl_open_ignores_unmatched_sequence_with_bounded_input)
{
	struct netlink_reply replies[2];
	struct netlink_datagram datagrams[2];
	uint16_t family = 0;

	build_family_reply(&replies[0], 99, 164, GENL_ID_CTRL, 44);
	build_family_reply(&replies[1], 1, 164, GENL_ID_CTRL, 55);
	datagrams[0] = family_datagram(&replies[0], 0);
	datagrams[1] = family_datagram(&replies[1], 0);
	ck_assert_int_eq(netlink_open(datagrams, 2, &family), 0);
	ck_assert_uint_eq(family, 55);
}
END_TEST

static struct nlattr *put_attr(struct reply_builder *builder, uint16_t type,
			       const void *value, size_t len)
{
	struct nlattr *attribute =
		(struct nlattr *)(builder->bytes + NLA_ALIGN(builder->len));

	attribute->nla_type = type;
	attribute->nla_len = NLA_HDRLEN + len;
	memcpy((uint8_t *)attribute + NLA_HDRLEN, value, len);
	builder->len = NLA_ALIGN(builder->len) + NLA_ALIGN(attribute->nla_len);
	return attribute;
}

static struct nlattr *nest_start(struct reply_builder *builder, uint16_t type)
{
	return put_attr(builder, type | NLA_F_NESTED, "", 0);
}

static void nest_end(struct reply_builder *builder, struct nlattr *attribute)
{
	attribute->nla_len =
		builder->bytes + builder->len - (uint8_t *)attribute;
}

static void fill_config(struct reffs_prototype_registration_config *config)
{
	memset(config, 0, sizeof(*config));
	config->enabled = true;
	strcpy(config->auth_domain, "client.example");
	for (size_t i = 0; i < sizeof(config->store_uuid); i++)
		config->store_uuid[i] = (uint8_t)i;
	for (size_t i = 0; i < sizeof(config->binding_token); i++)
		config->binding_token[i] = (uint8_t)(0x80 + i);
	config->chunk_size = 4096;
	config->data_count = 1;
	config->parity_count = 1;
	config->writer_id = 17;
	config->pnfs_clientid = 23;
	config->object_count = 2;
	for (uint32_t i = 0; i < 2; i++) {
		config->objects[i].ordinary_handle_len = 4;
		memset(config->objects[i].ordinary_handle, 0x10 + i, 4);
		memset(config->objects[i].persisted_handle, 0x30 + i, 32);
	}
}

START_TEST(test_challenge_requires_nonce_and_expiry)
{
	struct reply_builder reply = { 0 };
	uint8_t expected[32], actual[32];
	uint64_t expires = 10;

	memset(expected, 0x5a, sizeof(expected));
	put_attr(&reply, A_NONCE, expected, sizeof(expected));
	put_attr(&reply, A_EXPIRES, &expires, sizeof(expires));
	ck_assert_int_eq(ffv2_prototype_challenge_parse(reply.bytes, reply.len,
							actual),
			 0);
	ck_assert_int_eq(memcmp(expected, actual, sizeof(expected)), 0);
	reply.len = NLA_ALIGN(NLA_HDRLEN + sizeof(expected));
	ck_assert_int_eq(ffv2_prototype_challenge_parse(reply.bytes, reply.len,
							actual),
			 -EBADMSG);
}
END_TEST

static void
source_uuid(const struct reffs_prototype_registration_config *config,
	    uint8_t source[16])
{
	uint8_t input[104] = { 0 };
	uint8_t digest[SHA256_DIGEST_LENGTH];

	memcpy(input, "NFSD-FFV2-SET-V1", 16);
	memcpy(input + 16, config->store_uuid, 16);
	input[35] = 1;
	input[39] = 1;
	memcpy(input + 40, config->objects[0].persisted_handle, 32);
	memcpy(input + 72, config->objects[1].persisted_handle, 32);
	SHA256(input, sizeof(input), digest);
	memcpy(source, digest, 16);
}

static void
build_reply(const struct reffs_prototype_registration_config *config,
	    struct reply_builder *builder, bool partial, bool duplicate,
	    bool reorder)
{
	uint8_t source[16], service[16], replay[16];
	uint64_t generation = 9;
	struct nlattr *objects;

	memset(builder, 0, sizeof(*builder));
	source_uuid(config, source);
	memset(service, 0x61, sizeof(service));
	memset(replay, 0x72, sizeof(replay));
	put_attr(builder, A_SOURCE_UUID, source, sizeof(source));
	put_attr(builder, A_SERVICE_UUID, service, sizeof(service));
	put_attr(builder, A_REPLAY_UUID, replay, sizeof(replay));
	put_attr(builder, A_GENERATION, &generation, sizeof(generation));
	objects = nest_start(builder, A_OBJECTS);
	for (uint32_t i = 0; i < (partial ? 1U : 2U); i++) {
		uint32_t index = reorder ? 1 - i : i;
		uint8_t mapped[8], stateid[16];
		struct nlattr *child = nest_start(builder, (uint16_t)index);

		memset(mapped, duplicate ? 0x40 : 0x40 + i, sizeof(mapped));
		memset(stateid, duplicate ? 0x50 : 0x50 + i, sizeof(stateid));
		put_attr(builder, A_OBJECT_MAPPED_HANDLE, mapped,
			 sizeof(mapped));
		put_attr(builder, A_OBJECT_STATEID, stateid, sizeof(stateid));
		nest_end(builder, child);
	}
	nest_end(builder, objects);
}

START_TEST(test_request_preserves_order_and_secret_is_not_snapshot_state)
{
	struct reffs_prototype_registration_config config;
	uint8_t message[2048], nonce[32] = { 1 };
	size_t len;

	fill_config(&config);
	ck_assert_int_eq(ffv2_prototype_request_build(&config, nonce, 55, 7,
						      message, sizeof(message),
						      &len),
			 0);
	ck_assert(len > 0);
	ck_assert_ptr_nonnull(memmem(message, len, config.binding_token,
				     sizeof(config.binding_token)));
	ck_assert(memmem(message, len, config.objects[0].ordinary_handle, 4) <
		  memmem(message, len, config.objects[1].ordinary_handle, 4));
	ck_assert_int_eq(sizeof(((struct ffv2_prototype_snapshot *)0)->members),
			 REFFS_CONFIG_MAX_PROTOTYPE_OBJECTS *
				 sizeof(struct ffv2_prototype_member));
}
END_TEST

START_TEST(test_request_rejects_incomplete_identity_before_encoding)
{
	struct reffs_prototype_registration_config config;
	uint8_t message[2048], nonce[32] = { 1 };
	size_t len = 0;

	fill_config(&config);
	config.data_count = 0;
	ck_assert_int_eq(ffv2_prototype_request_build(&config, nonce, 55, 7,
						      message, sizeof(message),
						      &len),
			 -EINVAL);
	fill_config(&config);
	config.object_count = 1;
	ck_assert_int_eq(ffv2_prototype_request_build(&config, nonce, 55, 7,
						      message, sizeof(message),
						      &len),
			 -EINVAL);
	fill_config(&config);
	memcpy(config.objects[1].ordinary_handle,
	       config.objects[0].ordinary_handle,
	       config.objects[0].ordinary_handle_len);
	ck_assert_int_eq(ffv2_prototype_request_build(&config, nonce, 55, 7,
						      message, sizeof(message),
						      &len),
			 -EINVAL);
	fill_config(&config);
	memset(message, 0x5c, sizeof(message));
	ck_assert_int_eq(ffv2_prototype_request_build(&config, nonce, 55, 7,
						      message, 1, &len),
			 -EMSGSIZE);
	for (size_t i = 0; i < sizeof(message); i++)
		ck_assert_uint_eq(message[i], 0x5c);
}
END_TEST

START_TEST(test_reply_requires_complete_ordered_distinct_vector)
{
	struct reffs_prototype_registration_config config;
	struct ffv2_prototype_snapshot *snapshot;
	struct reply_builder reply;

	fill_config(&config);
	build_reply(&config, &reply, false, false, false);
	ck_assert_int_eq(ffv2_prototype_reply_parse(&config, 7, reply.bytes,
						    reply.len, &snapshot),
			 0);
	ck_assert_uint_eq(snapshot->object_count, 2);
	ck_assert_uint_eq(snapshot->members[0].mapped_handle[0], 0x40);
	ck_assert_uint_eq(snapshot->members[1].mapped_handle[0], 0x41);
	ck_assert_ptr_null(memmem(snapshot, sizeof(*snapshot),
				  config.binding_token,
				  sizeof(config.binding_token)));
	free(snapshot);

	build_reply(&config, &reply, true, false, false);
	ck_assert_int_eq(ffv2_prototype_reply_parse(&config, 7, reply.bytes,
						    reply.len, &snapshot),
			 -EBADMSG);
	build_reply(&config, &reply, false, true, false);
	ck_assert_int_eq(ffv2_prototype_reply_parse(&config, 7, reply.bytes,
						    reply.len, &snapshot),
			 -EBADMSG);
	build_reply(&config, &reply, false, false, true);
	ck_assert_int_eq(ffv2_prototype_reply_parse(&config, 7, reply.bytes,
						    reply.len, &snapshot),
			 -EBADMSG);
}
END_TEST

START_TEST(test_reply_rejects_bad_common_identity_and_extra_state)
{
	struct reffs_prototype_registration_config config;
	struct ffv2_prototype_snapshot *snapshot;
	struct reply_builder reply;
	uint8_t extra = 1;

	fill_config(&config);
	build_reply(&config, &reply, false, false, false);
	reply.bytes[NLA_HDRLEN] ^= 1;
	ck_assert_int_eq(ffv2_prototype_reply_parse(&config, 7, reply.bytes,
						    reply.len, &snapshot),
			 -EBADMSG);

	build_reply(&config, &reply, false, false, false);
	put_attr(&reply, 31, &extra, sizeof(extra));
	ck_assert_int_eq(ffv2_prototype_reply_parse(&config, 7, reply.bytes,
						    reply.len, &snapshot),
			 -EBADMSG);

	build_reply(&config, &reply, false, false, false);
	uint64_t *generation =
		(uint64_t *)(reply.bytes + NLA_ALIGN(3 * (NLA_HDRLEN + 16)) +
			     NLA_HDRLEN);

	*generation = 0;
	ck_assert_int_eq(ffv2_prototype_reply_parse(&config, 7, reply.bytes,
						    reply.len, &snapshot),
			 -EBADMSG);

	build_reply(&config, &reply, false, false, false);
	uint8_t stateid_pattern[16];

	memset(stateid_pattern, 0x50, sizeof(stateid_pattern));
	void *stateid = memmem(reply.bytes, reply.len, stateid_pattern,
			       sizeof(stateid_pattern));

	ck_assert_ptr_nonnull(stateid);
	memset(stateid, 0, sizeof(stateid_pattern));
	ck_assert_int_eq(ffv2_prototype_reply_parse(&config, 7, reply.bytes,
						    reply.len, &snapshot),
			 -EBADMSG);
}
END_TEST

static struct dstore *make_dstore(void)
{
	struct dstore *ds;

	ck_assert_int_eq(dstore_init(), 0);
	ds = dstore_alloc(7, "192.0.2.7", 2049, 0, "/kernel-ds",
			  REFFS_DS_PROTO_NFSV3, false, true);
	ck_assert_ptr_nonnull(ds);
	fill_config(&ds->ds_prototype_config);
	return ds;
}

static void destroy_dstore(struct dstore *ds);

static struct ffv2_prototype_snapshot *make_snapshot(uint64_t generation,
						     uint8_t base)
{
	struct reffs_prototype_registration_config config;
	struct ffv2_prototype_snapshot *snapshot;
	struct reply_builder reply;

	fill_config(&config);
	build_reply(&config, &reply, false, false, false);
	ck_assert_int_eq(ffv2_prototype_reply_parse(&config, 7, reply.bytes,
						    reply.len, &snapshot),
			 0);
	snapshot->generation = generation;
	for (uint32_t i = 0; i < snapshot->object_count; i++) {
		memset(snapshot->members[i].mapped_handle, base + i, 8);
		memset(snapshot->members[i].stateid, base + 0x10 + i, 16);
	}
	return snapshot;
}

enum fake_failure_stage {
	FAKE_FAIL_NONE,
	FAKE_FAIL_OPEN,
	FAKE_FAIL_DISABLE,
	FAKE_FAIL_ENABLE,
	FAKE_FAIL_CHALLENGE,
	FAKE_FAIL_REGISTER,
	FAKE_FAIL_ALLOCATE,
	FAKE_FAIL_VALIDATE,
};

struct fake_transport_context {
	enum fake_failure_stage failure;
	uint32_t disable_calls;
	uint32_t close_calls;
	struct dstore *retiring_ds;
	bool saw_retiring_gate;
	bool saw_absent_snapshot;
};

static int fake_open(void *opaque)
{
	struct fake_transport_context *ctx = opaque;

	return ctx->failure == FAKE_FAIL_OPEN ? -EIO : 0;
}

static int fake_disable(void *opaque)
{
	struct fake_transport_context *ctx = opaque;

	ctx->disable_calls++;
	if (ctx->retiring_ds) {
		const struct ffv2_prototype_snapshot *snapshot =
			ffv2_prototype_snapshot_borrow(ctx->retiring_ds);

		ctx->saw_absent_snapshot = !snapshot;
		ffv2_prototype_snapshot_release(ctx->retiring_ds);
		pthread_mutex_lock(&ctx->retiring_ds->ds_ordinary_gate.mutex);
		ctx->saw_retiring_gate =
			ctx->retiring_ds->ds_ordinary_gate.state ==
			DSTORE_ORDINARY_RETIRING;
		pthread_mutex_unlock(&ctx->retiring_ds->ds_ordinary_gate.mutex);
	}
	return ctx->failure == FAKE_FAIL_DISABLE && ctx->disable_calls == 1 ?
		       -EIO :
		       0;
}

static int fake_enable(void *opaque)
{
	struct fake_transport_context *ctx = opaque;

	return ctx->failure == FAKE_FAIL_ENABLE ? -EIO : 0;
}

static int fake_challenge(void *opaque, uint8_t nonce[32])
{
	struct fake_transport_context *ctx = opaque;

	if (ctx->failure == FAKE_FAIL_CHALLENGE)
		return -EIO;
	memset(nonce, 0x5a, 32);
	return 0;
}

static int
fake_register_vector(void *opaque,
		     const struct reffs_prototype_registration_config *config,
		     uint32_t dstore_id, const uint8_t nonce[32],
		     struct ffv2_prototype_snapshot **snapshot_out)
{
	struct fake_transport_context *ctx = opaque;

	ck_assert_ptr_nonnull(config);
	ck_assert_uint_eq(dstore_id, 7);
	ck_assert_uint_eq(nonce[0], 0x5a);
	if (ctx->failure == FAKE_FAIL_REGISTER) {
		*snapshot_out = make_snapshot(20, 0x80);
		return -EIO;
	}
	if (ctx->failure == FAKE_FAIL_ALLOCATE)
		return -ENOMEM;
	*snapshot_out = make_snapshot(20, 0x80);
	if (ctx->failure == FAKE_FAIL_VALIDATE)
		(*snapshot_out)->members[1].ordinary_handle[0] ^= 1;
	return 0;
}

static void fake_close(void *opaque)
{
	struct fake_transport_context *ctx = opaque;

	ctx->close_calls++;
}

START_TEST(test_registration_failures_publish_no_partial_vector)
{
	for (enum fake_failure_stage failure = FAKE_FAIL_OPEN;
	     failure <= FAKE_FAIL_VALIDATE; failure++) {
		struct dstore *ds = make_dstore();
		struct fake_transport_context context = { .failure = failure };
		const struct ffv2_prototype_transport transport = {
			.context = &context,
			.open = fake_open,
			.disable = fake_disable,
			.enable = fake_enable,
			.challenge = fake_challenge,
			.register_vector = fake_register_vector,
			.close = fake_close,
		};

		ck_assert_int_eq(ffv2_prototype_snapshot_replace(
					 ds, make_snapshot(10, 0x60)),
				 0);
		ck_assert_int_lt(
			ffv2_prototype_register_transport(ds, &transport), 0);
		const struct ffv2_prototype_snapshot *snapshot =
			ffv2_prototype_snapshot_borrow(ds);

		ck_assert_ptr_null(snapshot);
		ffv2_prototype_snapshot_release(ds);
		ck_assert_uint_eq(context.close_calls,
				  failure == FAKE_FAIL_OPEN ? 0 : 1);
		destroy_dstore(ds);
	}
}
END_TEST

START_TEST(test_retirement_unpublishes_before_disable_and_fails_closed)
{
	for (enum fake_failure_stage failure = FAKE_FAIL_NONE;
	     failure <= FAKE_FAIL_DISABLE; failure++) {
		struct dstore *ds = make_dstore();
		struct fake_transport_context context = {
			.failure = failure,
			.retiring_ds = ds,
		};
		const struct ffv2_prototype_transport transport = {
			.context = &context,
			.open = fake_open,
			.disable = fake_disable,
			.close = fake_close,
		};

		ck_assert_int_eq(ffv2_prototype_snapshot_replace(
					 ds, make_snapshot(10, 0x60)),
				 0);
		ck_assert_int_eq(dstore_ordinary_close(ds), 0);
		dstore_ordinary_activate(ds);
		dstore_ordinary_retire(ds);
		int ret = ffv2_prototype_retire_transport(ds, &transport);

		ck_assert_int_eq(ret, failure == FAKE_FAIL_NONE ? 0 : -EIO);
		if (failure != FAKE_FAIL_OPEN) {
			ck_assert(context.saw_retiring_gate);
			ck_assert(context.saw_absent_snapshot);
		}
		ck_assert_ptr_null(ds->ds_prototype_snapshot);
		ck_assert_uint_eq(context.disable_calls,
				  failure == FAKE_FAIL_OPEN ? 0 : 1);
		ck_assert_uint_eq(context.close_calls,
				  failure == FAKE_FAIL_OPEN ? 0 : 1);
		for (size_t i = 0;
		     i < sizeof(ds->ds_prototype_config.binding_token); i++)
			ck_assert_uint_eq(
				ds->ds_prototype_config.binding_token[i], 0);
		destroy_dstore(ds);
	}
}
END_TEST

START_TEST(test_registration_success_replaces_whole_vector)
{
	struct dstore *ds = make_dstore();
	struct fake_transport_context context = { 0 };
	const struct ffv2_prototype_transport transport = {
		.context = &context,
		.open = fake_open,
		.disable = fake_disable,
		.enable = fake_enable,
		.challenge = fake_challenge,
		.register_vector = fake_register_vector,
		.close = fake_close,
	};

	ck_assert_int_eq(
		ffv2_prototype_snapshot_replace(ds, make_snapshot(10, 0x60)),
		0);
	ck_assert_int_eq(ffv2_prototype_register_transport(ds, &transport), 0);
	const struct ffv2_prototype_snapshot *snapshot =
		ffv2_prototype_snapshot_borrow(ds);

	ck_assert_ptr_nonnull(snapshot);
	ck_assert_uint_eq(snapshot->generation, 20);
	ck_assert_uint_eq(snapshot->members[0].mapped_handle[0], 0x80);
	ck_assert_uint_eq(snapshot->members[1].mapped_handle[0], 0x81);
	ffv2_prototype_snapshot_release(ds);
	ck_assert_uint_eq(context.disable_calls, 1);
	ck_assert_uint_eq(context.close_calls, 1);
	destroy_dstore(ds);
}
END_TEST

static void destroy_dstore(struct dstore *ds)
{
	ds->ds_prototype_config.enabled = false;
	ffv2_prototype_unregister_dstore(ds);
	dstore_put(ds);
	dstore_fini();
}

static void fixed_inode_init(struct inode *inode, struct super_block *sb,
			     uint64_t ino)
{
	memset(inode, 0, sizeof(*inode));
	inode->i_sb = sb;
	inode->i_ino = ino;
	ck_assert_int_eq(pthread_mutex_init(&inode->i_layout_sync_mutex, NULL),
			 0);
	ck_assert_int_eq(pthread_mutex_init(&inode->i_attr_mutex, NULL), 0);
}

static void fixed_inode_destroy(struct inode *inode)
{
	layout_segments_free(inode->i_layout_segments);
	pthread_mutex_destroy(&inode->i_attr_mutex);
	pthread_mutex_destroy(&inode->i_layout_sync_mutex);
}

static nfsstat4 fixed_layoutcommit(struct inode *inode, clientid4 clientid,
				   const stateid4 *stateid, uint64_t offset)
{
	nfs_argop4 arg = { .argop = OP_LAYOUTCOMMIT };
	nfs_resop4 result = { 0 };
	COMPOUND4args args = {
		.argarray = { .argarray_len = 1, .argarray_val = &arg },
	};
	COMPOUND4res res = {
		.resarray = { .resarray_len = 1, .resarray_val = &result },
	};
	struct nfs4_client client = { 0 };
	struct compound compound = {
		.c_curr_op = 0,
		.c_inode = inode,
		.c_nfs4_client = &client,
		.c_args = &args,
		.c_res = &res,
	};

	client.nc_client.c_id = clientid;
	compound.c_curr_nfh.nfh_sb = inode->i_sb->sb_id;
	compound.c_curr_nfh.nfh_ino = inode->i_ino;
	arg.nfs_argop4_u.oplayoutcommit.loca_stateid = *stateid;
	arg.nfs_argop4_u.oplayoutcommit.loca_last_write_offset.no_newoffset =
		true;
	arg.nfs_argop4_u.oplayoutcommit.loca_last_write_offset.newoffset4_u
		.no_offset = offset;
	ck_assert_uint_eq(nfs4_op_layoutcommit(&compound), 0);
	return result.nfs_resop4_u.oplayoutcommit.locr_status;
}

START_TEST(test_fixed_layout_claim_retry_and_exhaustion)
{
	char directory[] = "/tmp/reffs-fixed-layout-XXXXXX";
	char record_path[256];
	struct dstore *ds = make_dstore();
	struct super_block sb = { 0 };
	struct inode first, second;
	uint64_t ordinary_before = 0;

	ck_assert_ptr_nonnull(mkdtemp(directory));
	ds->ds_prototype_config.fixed_inventory = true;
	strcpy(ds->ds_prototype_config.objects[0].name, "gate-b-data");
	strcpy(ds->ds_prototype_config.objects[1].name, "gate-b-parity");
	ck_assert_int_lt(snprintf(ds->ds_state_dir, sizeof(ds->ds_state_dir),
				  "%s", directory),
			 (int)sizeof(ds->ds_state_dir));
	ds->ds_fixed_inventory = calloc(1, sizeof(*ds->ds_fixed_inventory));
	ck_assert_ptr_nonnull(ds->ds_fixed_inventory);
	ck_assert_int_eq(ffv2_fixed_inventory_identity_init(
				 &ds->ds_fixed_inventory->identity, ds->ds_id,
				 ds->ds_address, ds->ds_path,
				 &ds->ds_prototype_config),
			 0);
	ck_assert_int_eq(ffv2_fixed_inventory_save(ds->ds_state_dir,
						   ds->ds_fixed_inventory),
			 0);
	ck_assert_int_eq(
		ffv2_prototype_snapshot_replace(ds, make_snapshot(10, 0x60)),
		0);

	uuid_generate(sb.sb_uuid);
	sb.sb_dstore_ids[0] = ds->ds_id;
	sb.sb_ndstores = 1;
	sb.sb_id = 11;
	sb.sb_block_size = 4096;
	sb.sb_stripe_unit = 4096;
	sb.sb_checksum_algorithm = LAYOUT_CHECKSUM_ALG_CRC32;
	fixed_inode_init(&first, &sb, 101);
	fixed_inode_init(&second, &sb, 102);
	for (uint32_t i = 0; i < DSTORE_ORDINARY_OP_COUNT; i++)
		ordinary_before +=
			atomic_load_explicit(&ds->ds_ordinary_gate.admitted[i],
					     memory_order_relaxed);

	ck_assert_int_eq(
		ffv2_fixed_layout_assign(&first, LAYOUT4_FLEX_FILES_V2), 1);
	ck_assert_int_eq(ds->ds_fixed_inventory->state,
			 FFV2_INVENTORY_ASSIGNED);
	ck_assert_uint_eq(ds->ds_fixed_inventory->owner_ino, first.i_ino);
	ck_assert_uint_eq(first.i_layout_segments->lss_count, 1);
	ck_assert_uint_eq(first.i_layout_segments->lss_segs[0].ls_nfiles, 2);
	uint64_t assigned_generation = ds->ds_fixed_inventory->generation;

	ck_assert_int_eq(
		ffv2_fixed_layout_assign(&first, LAYOUT4_FLEX_FILES_V2), 1);
	ck_assert_uint_eq(ds->ds_fixed_inventory->generation,
			  assigned_generation);
	stateid4 layout_stateid = { .seqid = 3 };
	sessionid4 sessionid = { 0 };

	memset(layout_stateid.other, 0xa5, sizeof(layout_stateid.other));
	ck_assert_int_eq(nfs4_fixed_layout_barrier_begin(
				 &first, 23, &layout_stateid, sessionid),
			 0);
	ck_assert_int_eq(ds->ds_fixed_inventory->metadata_state,
			 FFV2_METADATA_DIRTY);
	ck_assert_int_eq(fixed_layoutcommit(&first, 23, &layout_stateid, 4095),
			 NFS4_OK);
	ck_assert_int_eq(first.i_size, 4096);
	ck_assert_int_eq(first.i_used, 1);
	ck_assert_uint_eq(atomic_load_explicit(&sb.sb_bytes_used,
					       memory_order_relaxed),
			  4096);
	ck_assert_uint_eq(atomic_load_explicit(&first.i_changeid,
					       memory_order_relaxed),
			  1);
	ck_assert_int_eq(ds->ds_fixed_inventory->metadata_state,
			 FFV2_METADATA_COMMITTED);
	ck_assert_int_eq(ffv2_fixed_metadata_transition(ds->ds_fixed_inventory,
							FFV2_METADATA_CLEAN),
			 0);
	first.i_layout_barrier.uncertain = false;
	first.i_layout_barrier.commit_seen = false;
	first.i_layout_barrier.return_seen = true;
	ck_assert_int_eq(nfs4_fixed_layout_barrier_begin(
				 &first, 23, &layout_stateid, sessionid),
			 0);
	struct timespec mtime_before = first.i_mtime;
	uint64_t change_before =
		atomic_load_explicit(&first.i_changeid, memory_order_relaxed);

	ck_assert_int_eq(fixed_layoutcommit(&first, 23, &layout_stateid, 1023),
			 NFS4_OK);
	ck_assert_int_eq(first.i_size, 4096);
	ck_assert_uint_eq(atomic_load_explicit(&first.i_changeid,
					       memory_order_relaxed),
			  change_before + 1);
	ck_assert(first.i_mtime.tv_sec > mtime_before.tv_sec ||
		  (first.i_mtime.tv_sec == mtime_before.tv_sec &&
		   first.i_mtime.tv_nsec >= mtime_before.tv_nsec));
	ck_assert_int_eq(ffv2_fixed_layout_assign(&second,
						  LAYOUT4_FLEX_FILES_V2),
			 -ENOSPC);
	ck_assert_ptr_null(second.i_layout_segments);
	ds->ds_prototype_config.chunk_size = 8192;
	ck_assert_int_eq(ffv2_fixed_layout_assign(&first,
						  LAYOUT4_FLEX_FILES_V2),
			 -ESTALE);
	ds->ds_prototype_config.chunk_size = 4096;
	struct reffs_prototype_object_config swap =
		ds->ds_prototype_config.objects[0];

	ds->ds_prototype_config.objects[0] = ds->ds_prototype_config.objects[1];
	ds->ds_prototype_config.objects[1] = swap;
	ck_assert_int_eq(ffv2_fixed_layout_assign(&first,
						  LAYOUT4_FLEX_FILES_V2),
			 -ESTALE);
	swap = ds->ds_prototype_config.objects[0];
	ds->ds_prototype_config.objects[0] = ds->ds_prototype_config.objects[1];
	ds->ds_prototype_config.objects[1] = swap;

	uint64_t ordinary_after = 0;

	for (uint32_t i = 0; i < DSTORE_ORDINARY_OP_COUNT; i++)
		ordinary_after +=
			atomic_load_explicit(&ds->ds_ordinary_gate.admitted[i],
					     memory_order_relaxed);
	ck_assert_uint_eq(ordinary_after, ordinary_before);
	struct compound barrier_compound = { .c_inode = &first };
	nfsstat4 barrier_status = NFS4_OK;

	first.i_layout_barrier.deadline_ns = 1;
	ck_assert_uint_eq(nfs4_layout_metadata_barrier(&barrier_compound,
						       &barrier_status,
						       nfs4_op_getattr),
			  0);
	ck_assert_int_eq(barrier_status, NFS4ERR_IO);
	ck_assert_int_eq(ds->ds_fixed_inventory->state, FFV2_INVENTORY_FENCED);
	ck_assert_ptr_null(ds->ds_prototype_snapshot);

	fixed_inode_destroy(&second);
	fixed_inode_destroy(&first);
	snprintf(record_path, sizeof(record_path), "%s/ffv2-fixed-inventory-%u",
		 directory, ds->ds_id);
	unlink(record_path);
	rmdir(directory);
	destroy_dstore(ds);
}
END_TEST

START_TEST(test_fixed_layout_restart_rebinds_only_clean_owner)
{
	char directory[] = "/tmp/reffs-fixed-restart-XXXXXX";
	char path[256];
	struct dstore *ds = make_dstore();
	struct super_block *sb;
	struct inode *owner;

	ck_assert_ptr_nonnull(mkdtemp(directory));
	ds->ds_prototype_config.fixed_inventory = true;
	strcpy(ds->ds_prototype_config.objects[0].name, "gate-b-data");
	strcpy(ds->ds_prototype_config.objects[1].name, "gate-b-parity");
	ck_assert_int_lt(snprintf(ds->ds_state_dir, sizeof(ds->ds_state_dir),
				  "%s", directory),
			 (int)sizeof(ds->ds_state_dir));
	ds->ds_fixed_inventory = calloc(1, sizeof(*ds->ds_fixed_inventory));
	ck_assert_ptr_nonnull(ds->ds_fixed_inventory);
	ck_assert_int_eq(ffv2_fixed_inventory_identity_init(
				 &ds->ds_fixed_inventory->identity, ds->ds_id,
				 ds->ds_address, ds->ds_path,
				 &ds->ds_prototype_config),
			 0);
	ck_assert_int_eq(ffv2_fixed_inventory_save(ds->ds_state_dir,
						   ds->ds_fixed_inventory),
			 0);
	ck_assert_int_eq(
		ffv2_prototype_snapshot_replace(ds, make_snapshot(10, 0x60)),
		0);

	sb = super_block_alloc(901, "/fixed-restart", REFFS_STORAGE_RAM, NULL);
	ck_assert_ptr_nonnull(sb);
	uuid_generate(sb->sb_uuid);
	sb->sb_dstore_ids[0] = ds->ds_id;
	sb->sb_ndstores = 1;
	sb->sb_block_size = 4096;
	sb->sb_stripe_unit = 4096;
	sb->sb_checksum_algorithm = LAYOUT_CHECKSUM_ALG_CRC32;
	owner = inode_alloc(sb, 101);
	ck_assert_ptr_nonnull(owner);
	ck_assert_int_eq(ffv2_fixed_layout_assign(owner, LAYOUT4_FLEX_FILES_V2),
			 1);
	ck_assert_int_eq(ds->ds_fixed_inventory->state,
			 FFV2_INVENTORY_ASSIGNED);

	/* Simulate restart by discarding memory and reloading the record. */
	free(ds->ds_fixed_inventory);
	ds->ds_fixed_inventory = calloc(1, sizeof(*ds->ds_fixed_inventory));
	ck_assert_ptr_nonnull(ds->ds_fixed_inventory);
	owner->i_layout_barrier.active = false;
	ck_assert_int_eq(ffv2_fixed_inventory_load(ds->ds_state_dir, ds->ds_id,
						   ds->ds_fixed_inventory),
			 0);
	ck_assert_int_eq(dstore_fixed_inventory_recover(ds), 0);
	ck_assert(owner->i_layout_barrier.active);
	ck_assert_int_eq(ds->ds_fixed_inventory->state,
			 FFV2_INVENTORY_ASSIGNED);

	/* A crash with uncertain metadata fences before re-registration. */
	ck_assert_int_eq(ffv2_fixed_metadata_transition(ds->ds_fixed_inventory,
							FFV2_METADATA_DIRTY),
			 0);
	ck_assert_int_eq(ffv2_fixed_inventory_save(ds->ds_state_dir,
						   ds->ds_fixed_inventory),
			 0);
	owner->i_layout_barrier.active = false;
	ck_assert_int_eq(dstore_fixed_inventory_recover(ds), -ESTALE);
	ck_assert_int_eq(ds->ds_fixed_inventory->state, FFV2_INVENTORY_FENCED);
	ck_assert(!owner->i_layout_barrier.active);

	inode_active_put(owner);
	super_block_drain(sb);
	super_block_put(sb);
	ds->ds_prototype_config.enabled = false;
	destroy_dstore(ds);
	snprintf(path, sizeof(path), "%s/ffv2-fixed-inventory-7", directory);
	unlink(path);
	snprintf(path, sizeof(path), "%s/ffv2-fixed-status-7.json", directory);
	unlink(path);
	rmdir(directory);
}
END_TEST

START_TEST(test_snapshot_replacement_rejects_stale_generation)
{
	struct dstore *ds = make_dstore();
	struct ffv2_prototype_snapshot *first = make_snapshot(10, 0x60);
	struct ffv2_prototype_snapshot *stale = make_snapshot(9, 0x70);
	const struct ffv2_prototype_snapshot *borrowed;

	ck_assert_int_eq(ffv2_prototype_snapshot_replace(ds, first), 0);
	ck_assert_int_eq(ffv2_prototype_snapshot_replace(ds, stale), -ESTALE);
	borrowed = ffv2_prototype_snapshot_borrow(ds);
	ck_assert_uint_eq(borrowed->generation, 10);
	ck_assert_uint_eq(borrowed->members[0].mapped_handle[0], 0x60);
	ffv2_prototype_snapshot_release(ds);
	free(stale);
	destroy_dstore(ds);
}
END_TEST

START_TEST(test_snapshot_replacement_is_complete_and_config_bound)
{
	struct dstore *ds = make_dstore();
	struct ffv2_prototype_snapshot *first = make_snapshot(10, 0x60);
	struct ffv2_prototype_snapshot *second = make_snapshot(11, 0x80);
	struct ffv2_prototype_snapshot *invalid = make_snapshot(12, 0xa0);
	const struct ffv2_prototype_snapshot *borrowed;

	ck_assert_int_eq(ffv2_prototype_snapshot_replace(ds, first), 0);
	invalid->writer_id++;
	ck_assert_int_eq(ffv2_prototype_snapshot_replace(ds, invalid), -EINVAL);
	free(invalid);
	invalid = make_snapshot(12, 0xa0);
	invalid->object_count = 1;
	ck_assert_int_eq(ffv2_prototype_snapshot_replace(ds, invalid), -EINVAL);
	free(invalid);
	invalid = make_snapshot(12, 0xa0);
	invalid->members[1].ordinary_handle[0] ^= 1;
	ck_assert_int_eq(ffv2_prototype_snapshot_replace(ds, invalid), -EINVAL);
	free(invalid);
	invalid = make_snapshot(12, 0xa0);
	invalid->members[1].mapped_handle_len = 0;
	ck_assert_int_eq(ffv2_prototype_snapshot_replace(ds, invalid), -EINVAL);
	free(invalid);
	ck_assert_int_eq(ffv2_prototype_snapshot_replace(ds, second), 0);
	borrowed = ffv2_prototype_snapshot_borrow(ds);
	ck_assert_uint_eq(borrowed->generation, 11);
	ck_assert_uint_eq(borrowed->members[0].mapped_handle[0], 0x80);
	ck_assert_uint_eq(borrowed->members[1].mapped_handle[0], 0x81);
	ck_assert_uint_eq(borrowed->members[0].stateid[0], 0x90);
	ck_assert_uint_eq(borrowed->members[1].stateid[0], 0x91);
	ffv2_prototype_snapshot_release(ds);
	destroy_dstore(ds);
}
END_TEST

START_TEST(test_layout_builder_publishes_exact_member_identities)
{
	struct dstore *ds = make_dstore();
	struct ffv2_prototype_snapshot *snapshot = make_snapshot(10, 0x60);
	struct layout_data_file files[2] = { 0 };
	struct layout_segment segment = {
		.ls_stripe_unit = 4096,
		.ls_k = 1,
		.ls_m = 1,
		.ls_nfiles = 2,
		.ls_checksum_algorithm = CHECKSUM_ALG_CRC32,
		.ls_files = files,
	};
	stateid4 layout_stateid = { 0 };
	ffv2_layout4 layout = { 0 };
	char *body = NULL;
	u_long size = 0;

	for (uint32_t i = 0; i < 2; i++) {
		files[i].ldf_dstore_id = 7;
		files[i].ldf_fh_len = 4;
		memset(files[i].ldf_fh, 0x10 + i, 4);
	}
	ck_assert_int_eq(ffv2_prototype_snapshot_replace(ds, snapshot), 0);
	ck_assert_int_eq(layoutget_build_v2(&segment,
					    FFV2_ENCODING_RS_VANDERMONDE, 17,
					    23, &layout_stateid, &body, &size),
			 NFS4_OK);
	XDR xdr;

	xdrmem_create(&xdr, body, size, XDR_DECODE);
	ck_assert(xdr_ffv2_layout4(&xdr, &layout));
	xdr_destroy(&xdr);
	ffv2_mirror4 *mirror = &layout.ffv2l_mirrors.ffv2l_mirrors_val[0];
	ffv2_data_server4 *servers =
		mirror->ffv2m_stripes.ffv2m_stripes_val[0]
			.ffv2s_data_servers.ffv2s_data_servers_val;

	ck_assert(layout.ffv2l_flags & FFV2_FLAGS_NO_IO_THRU_MDS);
	ck_assert(!(layout.ffv2l_flags & FFV2_FLAGS_NO_LAYOUTCOMMIT));
	ck_assert_uint_eq(mirror->ffv2m_client_id, 17);
	for (uint32_t i = 0; i < 2; i++) {
		ffv2_file_info4 *info =
			&servers[i].ffv2ds_file_info.ffv2ds_file_info_val[0];

		ck_assert_uint_eq(info->ffv2fi_fh_vers.nfs_fh4_len, 8);
		ck_assert_uint_eq((uint8_t)info->ffv2fi_fh_vers.nfs_fh4_val[0],
				  0x60 + i);
		ck_assert_uint_eq(((uint8_t *)&info->ffv2fi_stateid)[0],
				  0x70 + i);
	}
	xdr_free((xdrproc_t)xdr_ffv2_layout4, (char *)&layout);
	free(body);
	destroy_dstore(ds);
}
END_TEST

START_TEST(test_opted_in_layout_fails_closed_on_identity_mismatch)
{
	struct dstore *ds = make_dstore();
	struct ffv2_prototype_snapshot *snapshot = make_snapshot(10, 0x60);
	struct layout_data_file files[2] = { 0 };
	struct layout_segment segment = {
		.ls_stripe_unit = 4096,
		.ls_k = 1,
		.ls_m = 1,
		.ls_nfiles = 2,
		.ls_checksum_algorithm = CHECKSUM_ALG_CRC32,
		.ls_files = files,
	};
	stateid4 layout_stateid = { 0 };
	char *body = NULL;
	u_long size = 0;

	for (uint32_t i = 0; i < 2; i++) {
		files[i].ldf_dstore_id = 7;
		files[i].ldf_fh_len = 4;
		memset(files[i].ldf_fh, 0x10 + i, 4);
	}
	ck_assert_int_eq(layoutget_build_v2(&segment,
					    FFV2_ENCODING_RS_VANDERMONDE, 17,
					    23, &layout_stateid, &body, &size),
			 NFS4ERR_LAYOUTUNAVAILABLE);
	ck_assert_int_eq(ffv2_prototype_snapshot_replace(ds, snapshot), 0);
	ck_assert_int_eq(layoutget_build_v2(&segment,
					    FFV2_ENCODING_RS_VANDERMONDE, 18,
					    23, &layout_stateid, &body, &size),
			 NFS4ERR_LAYOUTUNAVAILABLE);
	ck_assert_int_eq(layoutget_build_v2(&segment,
					    FFV2_ENCODING_RS_VANDERMONDE, 17,
					    24, &layout_stateid, &body, &size),
			 NFS4ERR_LAYOUTUNAVAILABLE);
	segment.ls_stripe_unit = 8192;
	ck_assert_int_eq(layoutget_build_v2(&segment,
					    FFV2_ENCODING_RS_VANDERMONDE, 17,
					    23, &layout_stateid, &body, &size),
			 NFS4ERR_LAYOUTUNAVAILABLE);
	segment.ls_stripe_unit = 4096;
	segment.ls_k = 2;
	segment.ls_m = 0;
	ck_assert_int_eq(layoutget_build_v2(&segment,
					    FFV2_ENCODING_RS_VANDERMONDE, 17,
					    23, &layout_stateid, &body, &size),
			 NFS4ERR_LAYOUTUNAVAILABLE);
	segment.ls_k = 1;
	segment.ls_m = 1;
	files[0].ldf_fh[0] ^= 1;
	ck_assert_int_eq(layoutget_build_v2(&segment,
					    FFV2_ENCODING_RS_VANDERMONDE, 17,
					    23, &layout_stateid, &body, &size),
			 NFS4ERR_LAYOUTUNAVAILABLE);
	ck_assert_ptr_null(body);
	destroy_dstore(ds);
}
END_TEST

START_TEST(test_absent_configuration_preserves_ordinary_layout_identity)
{
	struct dstore *ds = make_dstore();
	struct layout_data_file files[2] = { 0 };
	struct layout_segment segment = {
		.ls_stripe_unit = 4096,
		.ls_k = 1,
		.ls_m = 1,
		.ls_nfiles = 2,
		.ls_checksum_algorithm = CHECKSUM_ALG_CRC32,
		.ls_files = files,
	};
	stateid4 layout_stateid;
	ffv2_layout4 layout = { 0 };
	char *body = NULL;
	u_long size = 0;

	memset(&layout_stateid, 0x2a, sizeof(layout_stateid));
	ds->ds_prototype_config.enabled = false;
	for (uint32_t i = 0; i < 2; i++) {
		files[i].ldf_dstore_id = 7;
		files[i].ldf_fh_len = 4;
		memset(files[i].ldf_fh, 0x10 + i, 4);
	}
	ck_assert_int_eq(layoutget_build_v2(&segment,
					    FFV2_ENCODING_RS_VANDERMONDE, 29,
					    31, &layout_stateid, &body, &size),
			 NFS4_OK);
	XDR xdr;

	xdrmem_create(&xdr, body, size, XDR_DECODE);
	ck_assert(xdr_ffv2_layout4(&xdr, &layout));
	xdr_destroy(&xdr);
	ffv2_mirror4 *mirror = &layout.ffv2l_mirrors.ffv2l_mirrors_val[0];
	ffv2_data_server4 *servers =
		mirror->ffv2m_stripes.ffv2m_stripes_val[0]
			.ffv2s_data_servers.ffv2s_data_servers_val;

	ck_assert_uint_eq(mirror->ffv2m_client_id, 29);
	for (uint32_t i = 0; i < 2; i++) {
		ffv2_file_info4 *info =
			&servers[i].ffv2ds_file_info.ffv2ds_file_info_val[0];

		ck_assert_uint_eq(info->ffv2fi_fh_vers.nfs_fh4_len, 4);
		ck_assert_uint_eq((uint8_t)info->ffv2fi_fh_vers.nfs_fh4_val[0],
				  0x10 + i);
		ck_assert_int_eq(memcmp(&info->ffv2fi_stateid, &layout_stateid,
					sizeof(layout_stateid)),
				 0);
	}
	xdr_free((xdrproc_t)xdr_ffv2_layout4, (char *)&layout);
	free(body);
	destroy_dstore(ds);
}
END_TEST

enum { CONCURRENT_SNAPSHOTS = 32 };

struct concurrent_publish {
	struct dstore *ds;
	struct ffv2_prototype_snapshot *snapshots[CONCURRENT_SNAPSHOTS];
	int error;
};

static void *publish_snapshots(void *opaque)
{
	struct concurrent_publish *ctx = opaque;

	for (uint32_t i = 0; i < CONCURRENT_SNAPSHOTS; i++) {
		ctx->error = ffv2_prototype_snapshot_replace(ctx->ds,
							     ctx->snapshots[i]);
		if (ctx->error)
			return NULL;
		ctx->snapshots[i] = NULL;
		usleep(100);
	}
	return NULL;
}

START_TEST(test_concurrent_layout_observes_one_complete_vector)
{
	struct dstore *ds = make_dstore();
	struct layout_data_file files[2] = { 0 };
	struct layout_segment segment = {
		.ls_stripe_unit = 4096,
		.ls_k = 1,
		.ls_m = 1,
		.ls_nfiles = 2,
		.ls_checksum_algorithm = CHECKSUM_ALG_CRC32,
		.ls_files = files,
	};
	struct concurrent_publish ctx = { .ds = ds };
	stateid4 layout_stateid = { 0 };
	pthread_t publisher;

	for (uint32_t i = 0; i < 2; i++) {
		files[i].ldf_dstore_id = 7;
		files[i].ldf_fh_len = 4;
		memset(files[i].ldf_fh, 0x10 + i, 4);
	}
	ck_assert_int_eq(
		ffv2_prototype_snapshot_replace(ds, make_snapshot(10, 0x60)),
		0);
	for (uint32_t i = 0; i < CONCURRENT_SNAPSHOTS; i++)
		ctx.snapshots[i] = make_snapshot(11 + i, (i & 1) ? 0x80 : 0xa0);
	ck_assert_int_eq(
		pthread_create(&publisher, NULL, publish_snapshots, &ctx), 0);
	for (uint32_t iteration = 0; iteration < 64; iteration++) {
		ffv2_layout4 layout = { 0 };
		char *body = NULL;
		u_long size = 0;
		XDR xdr;

		ck_assert_int_eq(layoutget_build_v2(
					 &segment, FFV2_ENCODING_RS_VANDERMONDE,
					 17, 23, &layout_stateid, &body, &size),
				 NFS4_OK);
		xdrmem_create(&xdr, body, size, XDR_DECODE);
		ck_assert(xdr_ffv2_layout4(&xdr, &layout));
		xdr_destroy(&xdr);
		ffv2_data_server4 *servers =
			layout.ffv2l_mirrors.ffv2l_mirrors_val[0]
				.ffv2m_stripes.ffv2m_stripes_val[0]
				.ffv2s_data_servers.ffv2s_data_servers_val;
		ffv2_file_info4 *first =
			&servers[0].ffv2ds_file_info.ffv2ds_file_info_val[0];
		ffv2_file_info4 *second =
			&servers[1].ffv2ds_file_info.ffv2ds_file_info_val[0];
		uint8_t first_handle = first->ffv2fi_fh_vers.nfs_fh4_val[0];

		ck_assert_uint_eq(
			(uint8_t)second->ffv2fi_fh_vers.nfs_fh4_val[0],
			(uint8_t)(first_handle + 1));
		ck_assert_uint_eq(((uint8_t *)&first->ffv2fi_stateid)[0],
				  (uint8_t)(first_handle + 0x10));
		ck_assert_uint_eq(((uint8_t *)&second->ffv2fi_stateid)[0],
				  (uint8_t)(first_handle + 0x11));
		xdr_free((xdrproc_t)xdr_ffv2_layout4, (char *)&layout);
		free(body);
	}
	ck_assert_int_eq(pthread_join(publisher, NULL), 0);
	ck_assert_int_eq(ctx.error, 0);
	for (uint32_t i = 0; i < CONCURRENT_SNAPSHOTS; i++)
		free(ctx.snapshots[i]);
	destroy_dstore(ds);
}
END_TEST

static Suite *prototype_suite(void)
{
	Suite *suite = suite_create("ffv2_prototype");
	TCase *test = tcase_create("publication");

	tcase_add_test(
		test,
		test_request_preserves_order_and_secret_is_not_snapshot_state);
	tcase_add_test(
		test,
		test_nl_open_accepts_kernel_reply_with_requester_header_portid);
	tcase_add_test(
		test,
		test_nl_open_rejects_non_kernel_peer_and_wrong_message_type);
	tcase_add_test(
		test,
		test_nl_open_ignores_unmatched_sequence_with_bounded_input);
	tcase_add_test(
		test, test_request_rejects_incomplete_identity_before_encoding);
	tcase_add_test(test, test_challenge_requires_nonce_and_expiry);
	tcase_add_test(test,
		       test_reply_requires_complete_ordered_distinct_vector);
	tcase_add_test(test,
		       test_reply_rejects_bad_common_identity_and_extra_state);
	tcase_add_test(test,
		       test_snapshot_replacement_rejects_stale_generation);
	tcase_add_test(test,
		       test_snapshot_replacement_is_complete_and_config_bound);
	tcase_add_test(test,
		       test_registration_failures_publish_no_partial_vector);
	tcase_add_test(test, test_registration_success_replaces_whole_vector);
	tcase_add_test(
		test,
		test_retirement_unpublishes_before_disable_and_fails_closed);
	tcase_add_test(test,
		       test_layout_builder_publishes_exact_member_identities);
	tcase_add_test(test,
		       test_opted_in_layout_fails_closed_on_identity_mismatch);
	tcase_add_test(
		test,
		test_absent_configuration_preserves_ordinary_layout_identity);
	tcase_add_test(test,
		       test_concurrent_layout_observes_one_complete_vector);
	tcase_add_test(test, test_fixed_layout_claim_retry_and_exhaustion);
	tcase_add_test(test,
		       test_fixed_layout_restart_rebinds_only_clean_owner);
	suite_add_tcase(suite, test);
	return suite;
}

int main(void)
{
	return reffs_test_run_suite(prototype_suite(), NULL, NULL);
}
