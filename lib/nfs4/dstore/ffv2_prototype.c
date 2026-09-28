/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <openssl/crypto.h>
#include <openssl/sha.h>
#include <sys/socket.h>

#include "reffs/dstore.h"
#include "reffs/ffv2_prototype.h"
#include "reffs/layout_segment.h"
#include "reffs/log.h"

#include "ffv2_prototype_internal.h"

#define FFV2_NL_BUFFER_SIZE 8192
#define FFV2_NFSD_FAMILY_NAME "nfsd"
#define FFV2_NFSD_FAMILY_VERSION 1

/* Pinned to the accepted nfsd UAPI at bf7385226fd5. */
enum ffv2_nfsd_command {
	FFV2_NFSD_CMD_PROTOTYPE_ENABLE = 21,
	FFV2_NFSD_CMD_PROTOTYPE_DISABLE,
	FFV2_NFSD_CMD_PROTOTYPE_CHALLENGE,
	FFV2_NFSD_CMD_PROTOTYPE_REGISTER,
};

enum ffv2_nfsd_attribute {
	FFV2_NFSD_A_NONCE = 1,
	FFV2_NFSD_A_EXPIRES,
	FFV2_NFSD_A_AUTH_DOMAIN,
	FFV2_NFSD_A_HANDLE,
	FFV2_NFSD_A_CHUNK_SIZE,
	FFV2_NFSD_A_DATA_COUNT,
	FFV2_NFSD_A_PARITY_COUNT,
	FFV2_NFSD_A_SOURCE_UUID,
	FFV2_NFSD_A_SERVICE_UUID,
	FFV2_NFSD_A_REPLAY_UUID,
	FFV2_NFSD_A_GENERATION,
	FFV2_NFSD_A_STATEID,
	FFV2_NFSD_A_MAPPED_HANDLE,
	FFV2_NFSD_A_STORE_UUID,
	FFV2_NFSD_A_BINDING_TOKEN,
	FFV2_NFSD_A_PERSISTED_HANDLE,
	FFV2_NFSD_A_WRITER_ID,
	FFV2_NFSD_A_PNFS_CLIENTID,
	FFV2_NFSD_A_OBJECTS,
};

enum ffv2_nfsd_object_attribute {
	FFV2_NFSD_A_OBJECT_ORDINARY_HANDLE = 1,
	FFV2_NFSD_A_OBJECT_PERSISTED_HANDLE,
	FFV2_NFSD_A_OBJECT_MAPPED_HANDLE,
	FFV2_NFSD_A_OBJECT_STATEID,
};

struct ffv2_nl {
	int fd;
	uint16_t family;
	uint32_t seq;
	uint8_t buffer[FFV2_NL_BUFFER_SIZE];
	const struct ffv2_prototype_nl_io *io;
	void *io_context;
};

static int nl_system_open(void *context __attribute__((unused)))
{
	return socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
}

static int nl_system_bind(void *context __attribute__((unused)), int fd,
			  const struct sockaddr *address, socklen_t address_len)
{
	return bind(fd, address, address_len);
}

static ssize_t nl_system_send(void *context __attribute__((unused)), int fd,
			      const void *buffer, size_t len,
			      const struct sockaddr *address,
			      socklen_t address_len)
{
	return sendto(fd, buffer, len, 0, address, address_len);
}

static ssize_t nl_system_receive(void *context __attribute__((unused)), int fd,
				 void *buffer, size_t len,
				 struct sockaddr *address,
				 socklen_t *address_len)
{
	return recvfrom(fd, buffer, len, 0, address, address_len);
}

static int nl_system_close(void *context __attribute__((unused)), int fd)
{
	return close(fd);
}

static const struct ffv2_prototype_nl_io nl_system_io = {
	.open = nl_system_open,
	.bind = nl_system_bind,
	.send = nl_system_send,
	.receive = nl_system_receive,
	.close = nl_system_close,
};

static bool
prototype_config_valid(const struct reffs_prototype_registration_config *config)
{
	uint64_t total;
	size_t domain_len;

	if (!config || !config->enabled)
		return false;
	domain_len = strnlen(config->auth_domain, sizeof(config->auth_domain));
	total = (uint64_t)config->data_count + config->parity_count;
	if (!domain_len || domain_len >= sizeof(config->auth_domain) ||
	    !config->chunk_size ||
	    config->chunk_size > REFFS_CONFIG_PROTOTYPE_MAX_CHUNK_SIZE ||
	    !config->data_count || !config->writer_id ||
	    config->writer_id == UINT32_MAX || !config->pnfs_clientid ||
	    !config->object_count ||
	    config->object_count > REFFS_CONFIG_MAX_PROTOTYPE_OBJECTS ||
	    total != config->object_count ||
	    (!config->parity_count && config->object_count != 1))
		return false;
	for (uint32_t i = 0; i < config->object_count; i++) {
		const struct reffs_prototype_object_config *object =
			&config->objects[i];

		if (!object->ordinary_handle_len ||
		    object->ordinary_handle_len >
			    sizeof(object->ordinary_handle))
			return false;
		for (uint32_t j = 0; j < i; j++) {
			const struct reffs_prototype_object_config *prior =
				&config->objects[j];

			if ((object->ordinary_handle_len ==
				     prior->ordinary_handle_len &&
			     !memcmp(object->ordinary_handle,
				     prior->ordinary_handle,
				     object->ordinary_handle_len)) ||
			    !memcmp(object->persisted_handle,
				    prior->persisted_handle,
				    sizeof(object->persisted_handle)))
				return false;
		}
	}
	return true;
}

static bool bytes_zero(const uint8_t *value, size_t len)
{
	uint8_t any = 0;

	for (size_t i = 0; i < len; i++)
		any |= value[i];
	return any == 0;
}

static struct nlmsghdr *nl_start(struct ffv2_nl *nl, uint16_t type,
				 uint8_t command)
{
	struct nlmsghdr *header = (struct nlmsghdr *)nl->buffer;
	struct genlmsghdr *generic;

	memset(nl->buffer, 0, sizeof(nl->buffer));
	header->nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
	header->nlmsg_type = type;
	header->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
	header->nlmsg_seq = ++nl->seq;
	generic = NLMSG_DATA(header);
	generic->cmd = command;
	generic->version = FFV2_NFSD_FAMILY_VERSION;
	return header;
}

static int nl_put(struct ffv2_nl *nl, uint16_t type, const void *data,
		  uint16_t len)
{
	struct nlmsghdr *header = (struct nlmsghdr *)nl->buffer;
	struct nlattr *attribute;
	size_t offset = NLMSG_ALIGN(header->nlmsg_len);

	if (offset + NLA_HDRLEN + NLA_ALIGN(len) > sizeof(nl->buffer))
		return -EMSGSIZE;
	attribute = (struct nlattr *)(nl->buffer + offset);
	attribute->nla_type = type;
	attribute->nla_len = NLA_HDRLEN + len;
	memcpy((uint8_t *)attribute + NLA_HDRLEN, data, len);
	header->nlmsg_len = offset + NLA_ALIGN(attribute->nla_len);
	return 0;
}

static int nl_put_u32(struct ffv2_nl *nl, uint16_t type, uint32_t value)
{
	return nl_put(nl, type, &value, sizeof(value));
}

static int nl_put_u64(struct ffv2_nl *nl, uint16_t type, uint64_t value)
{
	return nl_put(nl, type, &value, sizeof(value));
}

static int nl_put_string(struct ffv2_nl *nl, uint16_t type, const char *value)
{
	size_t len = strlen(value) + 1;

	if (len > UINT16_MAX)
		return -EMSGSIZE;
	return nl_put(nl, type, value, (uint16_t)len);
}

static struct nlattr *nl_nest_start(struct ffv2_nl *nl, uint16_t type)
{
	struct nlmsghdr *header = (struct nlmsghdr *)nl->buffer;
	struct nlattr *attribute;
	size_t offset = NLMSG_ALIGN(header->nlmsg_len);

	if (offset + NLA_HDRLEN > sizeof(nl->buffer))
		return NULL;
	attribute = (struct nlattr *)(nl->buffer + offset);
	attribute->nla_type = type | NLA_F_NESTED;
	attribute->nla_len = NLA_HDRLEN;
	header->nlmsg_len = offset + NLA_ALIGN(NLA_HDRLEN);
	return attribute;
}

static void nl_nest_end(struct ffv2_nl *nl, struct nlattr *attribute)
{
	struct nlmsghdr *header = (struct nlmsghdr *)nl->buffer;

	attribute->nla_len = (uint8_t *)nl->buffer + header->nlmsg_len -
			     (uint8_t *)attribute;
}

static const void *nl_data(const struct nlattr *attribute, size_t *len)
{
	*len = attribute->nla_len - NLA_HDRLEN;
	return (const uint8_t *)attribute + NLA_HDRLEN;
}

static const struct nlattr *nl_next(const uint8_t *attributes, size_t len,
				    size_t *offset)
{
	const struct nlattr *attribute;
	size_t next;

	if (*offset + NLA_HDRLEN > len)
		return NULL;
	attribute = (const struct nlattr *)(attributes + *offset);
	if (attribute->nla_len < NLA_HDRLEN ||
	    attribute->nla_len > len - *offset)
		return NULL;
	next = *offset + NLA_ALIGN(attribute->nla_len);
	if (next > len)
		return NULL;
	*offset = next;
	return attribute;
}

static const struct nlattr *nl_find(const uint8_t *attributes, size_t len,
				    uint16_t type)
{
	size_t offset = 0;
	const struct nlattr *attribute;

	while ((attribute = nl_next(attributes, len, &offset)) != NULL)
		if ((attribute->nla_type & NLA_TYPE_MASK) == type)
			return attribute;
	return NULL;
}

static int nl_talk(struct ffv2_nl *nl, uint8_t *reply, size_t reply_size,
		   size_t *reply_len, bool expect_reply)
{
	struct nlmsghdr *request = (struct nlmsghdr *)nl->buffer;
	struct sockaddr_nl kernel = { .nl_family = AF_NETLINK };
	uint8_t input[FFV2_NL_BUFFER_SIZE];
	bool have_reply = false;

	*reply_len = 0;
	if (nl->io->send(nl->io_context, nl->fd, nl->buffer, request->nlmsg_len,
			 (struct sockaddr *)&kernel, sizeof(kernel)) < 0)
		return -errno;
	for (;;) {
		struct sockaddr_nl peer = { 0 };
		socklen_t peer_len = sizeof(peer);
		ssize_t got = nl->io->receive(nl->io_context, nl->fd, input,
					      sizeof(input),
					      (struct sockaddr *)&peer,
					      &peer_len);
		ssize_t remaining = got;
		struct nlmsghdr *message;

		if (got < 0)
			return -errno;
		if (peer_len != sizeof(peer) || peer.nl_family != AF_NETLINK ||
		    peer.nl_pid != 0)
			return -EBADMSG;
		for (message = (struct nlmsghdr *)input;
		     NLMSG_OK(message, remaining);
		     message = NLMSG_NEXT(message, remaining)) {
			size_t payload;

			if (message->nlmsg_seq != request->nlmsg_seq)
				continue;
			if (message->nlmsg_type == NLMSG_ERROR) {
				const struct nlmsgerr *error =
					NLMSG_DATA(message);

				if (error->error)
					return error->error;
				if (!expect_reply || have_reply)
					return 0;
				continue;
			}
			if (message->nlmsg_type == NLMSG_DONE)
				return !expect_reply || have_reply ? 0 :
								     -EBADMSG;
			if (message->nlmsg_type != request->nlmsg_type)
				return -EBADMSG;
			if (message->nlmsg_len < NLMSG_HDRLEN + GENL_HDRLEN)
				return -EBADMSG;
			payload =
				message->nlmsg_len - NLMSG_HDRLEN - GENL_HDRLEN;
			if (have_reply || payload > reply_size)
				return -EMSGSIZE;
			memcpy(reply,
			       (uint8_t *)NLMSG_DATA(message) + GENL_HDRLEN,
			       payload);
			*reply_len = payload;
			have_reply = true;
		}
	}
}

static int nl_open_with_io(struct ffv2_nl *nl,
			   const struct ffv2_prototype_nl_io *io)
{
	struct sockaddr_nl address = { .nl_family = AF_NETLINK };
	uint8_t reply[FFV2_NL_BUFFER_SIZE];
	const struct nlattr *attribute;
	size_t len, value_len;
	int error;

	memset(nl, 0, sizeof(*nl));
	nl->io = io;
	nl->io_context = io->context;
	nl->fd = io->open(io->context);
	if (nl->fd < 0)
		return -errno;
	if (io->bind(io->context, nl->fd, (struct sockaddr *)&address,
		     sizeof(address)) < 0) {
		error = -errno;
		goto out_close;
	}
	nl_start(nl, GENL_ID_CTRL, CTRL_CMD_GETFAMILY);
	error = nl_put_string(nl, CTRL_ATTR_FAMILY_NAME, FFV2_NFSD_FAMILY_NAME);
	if (error)
		goto out_close;
	error = nl_talk(nl, reply, sizeof(reply), &len, true);
	if (error)
		goto out_close;
	attribute = nl_find(reply, len, CTRL_ATTR_FAMILY_ID);
	if (!attribute) {
		error = -ENOENT;
		goto out_close;
	}
	const void *value = nl_data(attribute, &value_len);

	if (value_len != sizeof(nl->family)) {
		error = -EBADMSG;
		goto out_close;
	}
	memcpy(&nl->family, value, sizeof(nl->family));
	return 0;

out_close:
	io->close(io->context, nl->fd);
	nl->fd = -1;
	return error;
}

static int nl_open(struct ffv2_nl *nl)
{
	return nl_open_with_io(nl, &nl_system_io);
}

int ffv2_prototype_nl_open_test(const struct ffv2_prototype_nl_io *io,
				uint16_t *family)
{
	struct ffv2_nl nl;
	int error;

	if (!io || !io->open || !io->bind || !io->send || !io->receive ||
	    !io->close || !family)
		return -EINVAL;
	error = nl_open_with_io(&nl, io);
	if (error)
		return error;
	*family = nl.family;
	io->close(io->context, nl.fd);
	return 0;
}

static int nl_simple(struct ffv2_nl *nl, uint8_t command)
{
	uint8_t reply[FFV2_NL_BUFFER_SIZE];
	size_t len;

	nl_start(nl, nl->family, command);
	return nl_talk(nl, reply, sizeof(reply), &len, false);
}

int ffv2_prototype_request_build(
	const struct reffs_prototype_registration_config *config,
	const uint8_t nonce[32], uint16_t family, uint32_t seq, uint8_t *buffer,
	size_t buffer_size, size_t *message_size)
{
	struct ffv2_nl nl = { .family = family, .seq = seq - 1 };
	struct nlattr *objects;
	int error = 0;

	if (!prototype_config_valid(config) || !nonce || !buffer ||
	    !message_size)
		return -EINVAL;
	nl_start(&nl, family, FFV2_NFSD_CMD_PROTOTYPE_REGISTER);
#define PUT(_call)                \
	do {                      \
		error = (_call);  \
		if (error)        \
			goto out; \
	} while (0)
	PUT(nl_put(&nl, FFV2_NFSD_A_NONCE, nonce, 32));
	PUT(nl_put_string(&nl, FFV2_NFSD_A_AUTH_DOMAIN, config->auth_domain));
	PUT(nl_put_u32(&nl, FFV2_NFSD_A_CHUNK_SIZE, config->chunk_size));
	PUT(nl_put_u32(&nl, FFV2_NFSD_A_DATA_COUNT, config->data_count));
	PUT(nl_put_u32(&nl, FFV2_NFSD_A_PARITY_COUNT, config->parity_count));
	PUT(nl_put(&nl, FFV2_NFSD_A_STORE_UUID, config->store_uuid,
		   sizeof(config->store_uuid)));
	PUT(nl_put(&nl, FFV2_NFSD_A_BINDING_TOKEN, config->binding_token,
		   sizeof(config->binding_token)));
	PUT(nl_put_u32(&nl, FFV2_NFSD_A_WRITER_ID, config->writer_id));
	PUT(nl_put_u64(&nl, FFV2_NFSD_A_PNFS_CLIENTID, config->pnfs_clientid));
	objects = nl_nest_start(&nl, FFV2_NFSD_A_OBJECTS);
	if (!objects) {
		error = -EMSGSIZE;
		goto out;
	}
	for (uint32_t i = 0; i < config->object_count; i++) {
		const struct reffs_prototype_object_config *object =
			&config->objects[i];
		struct nlattr *child = nl_nest_start(&nl, (uint16_t)i);

		if (!child) {
			error = -EMSGSIZE;
			goto out;
		}
		PUT(nl_put(&nl, FFV2_NFSD_A_OBJECT_ORDINARY_HANDLE,
			   object->ordinary_handle,
			   (uint16_t)object->ordinary_handle_len));
		PUT(nl_put(&nl, FFV2_NFSD_A_OBJECT_PERSISTED_HANDLE,
			   object->persisted_handle,
			   sizeof(object->persisted_handle)));
		nl_nest_end(&nl, child);
	}
	nl_nest_end(&nl, objects);
#undef PUT
	struct nlmsghdr *header = (struct nlmsghdr *)nl.buffer;

	if (header->nlmsg_len > buffer_size) {
		error = -EMSGSIZE;
		goto out;
	}
	memcpy(buffer, nl.buffer, header->nlmsg_len);
	*message_size = header->nlmsg_len;
out:
	OPENSSL_cleanse(&nl, sizeof(nl));
	return error;
}

static void
expected_source_uuid(const struct reffs_prototype_registration_config *config,
		     uint8_t output[16])
{
	uint8_t digest[SHA256_DIGEST_LENGTH];
	uint8_t input[40 +
		      REFFS_CONFIG_MAX_PROTOTYPE_OBJECTS *
			      REFFS_CONFIG_PROTOTYPE_PERSISTED_HANDLE_SIZE] = {
		0
	};
	size_t len = 40 + config->object_count *
				  REFFS_CONFIG_PROTOTYPE_PERSISTED_HANDLE_SIZE;

	memcpy(input, "NFSD-FFV2-SET-V1", 16);
	memcpy(input + 16, config->store_uuid, sizeof(config->store_uuid));
	input[32] = (uint8_t)(config->data_count >> 24);
	input[33] = (uint8_t)(config->data_count >> 16);
	input[34] = (uint8_t)(config->data_count >> 8);
	input[35] = (uint8_t)config->data_count;
	input[36] = (uint8_t)(config->parity_count >> 24);
	input[37] = (uint8_t)(config->parity_count >> 16);
	input[38] = (uint8_t)(config->parity_count >> 8);
	input[39] = (uint8_t)config->parity_count;
	for (uint32_t i = 0; i < config->object_count; i++)
		memcpy(input + 40 +
			       i * REFFS_CONFIG_PROTOTYPE_PERSISTED_HANDLE_SIZE,
		       config->objects[i].persisted_handle,
		       REFFS_CONFIG_PROTOTYPE_PERSISTED_HANDLE_SIZE);
	SHA256(input, len, digest);
	memcpy(output, digest, 16);
	OPENSSL_cleanse(input, sizeof(input));
	OPENSSL_cleanse(digest, sizeof(digest));
}

int ffv2_prototype_reply_parse(
	const struct reffs_prototype_registration_config *config,
	uint32_t dstore_id, const uint8_t *attributes, size_t attributes_len,
	struct ffv2_prototype_snapshot **snapshot_out)
{
	static const uint16_t top_types[] = {
		FFV2_NFSD_A_SOURCE_UUID, FFV2_NFSD_A_SERVICE_UUID,
		FFV2_NFSD_A_REPLAY_UUID, FFV2_NFSD_A_GENERATION,
		FFV2_NFSD_A_OBJECTS,
	};
	const struct nlattr *top[5];
	struct ffv2_prototype_snapshot *snapshot;
	uint8_t expected_source[16];
	size_t offset = 0, len;
	const void *value;

	if (!prototype_config_valid(config) || !attributes || !snapshot_out)
		return -EINVAL;
	*snapshot_out = NULL;
	for (size_t i = 0; i < 5; i++) {
		top[i] = nl_next(attributes, attributes_len, &offset);
		if (!top[i] ||
		    (top[i]->nla_type & NLA_TYPE_MASK) != top_types[i] ||
		    (i == 4 && !(top[i]->nla_type & NLA_F_NESTED)))
			return -EBADMSG;
	}
	if (offset != attributes_len)
		return -EBADMSG;
	snapshot = calloc(1, sizeof(*snapshot));
	if (!snapshot)
		return -ENOMEM;
	snapshot->dstore_id = dstore_id;
	snapshot->chunk_size = config->chunk_size;
	snapshot->data_count = config->data_count;
	snapshot->parity_count = config->parity_count;
	snapshot->writer_id = config->writer_id;
	snapshot->pnfs_clientid = config->pnfs_clientid;
	snapshot->object_count = config->object_count;

	uint8_t *uuids[] = { snapshot->source_uuid, snapshot->service_uuid,
			     snapshot->replay_uuid };
	for (size_t i = 0; i < 3; i++) {
		value = nl_data(top[i], &len);
		if (len != 16 || bytes_zero(value, len))
			goto malformed;
		memcpy(uuids[i], value, len);
	}
	expected_source_uuid(config, expected_source);
	if (memcmp(snapshot->source_uuid, expected_source,
		   sizeof(expected_source)) ||
	    !memcmp(snapshot->source_uuid, snapshot->service_uuid, 16) ||
	    !memcmp(snapshot->source_uuid, snapshot->replay_uuid, 16) ||
	    !memcmp(snapshot->service_uuid, snapshot->replay_uuid, 16))
		goto malformed;
	value = nl_data(top[3], &len);
	if (len != sizeof(snapshot->generation))
		goto malformed;
	memcpy(&snapshot->generation, value, len);
	if (!snapshot->generation)
		goto malformed;

	const uint8_t *object_data = nl_data(top[4], &len);
	size_t object_offset = 0;
	for (uint32_t i = 0; i < config->object_count; i++) {
		const struct nlattr *child =
			nl_next(object_data, len, &object_offset);
		size_t child_len, child_offset = 0, value_len;
		const uint8_t *child_data;
		const struct nlattr *mapped, *stateid;
		struct ffv2_prototype_member *member = &snapshot->members[i];

		if (!child || !(child->nla_type & NLA_F_NESTED) ||
		    (child->nla_type & NLA_TYPE_MASK) != i)
			goto malformed;
		child_data = nl_data(child, &child_len);
		mapped = nl_next(child_data, child_len, &child_offset);
		stateid = nl_next(child_data, child_len, &child_offset);
		if (!mapped || !stateid || child_offset != child_len ||
		    (mapped->nla_type & NLA_TYPE_MASK) !=
			    FFV2_NFSD_A_OBJECT_MAPPED_HANDLE ||
		    (stateid->nla_type & NLA_TYPE_MASK) !=
			    FFV2_NFSD_A_OBJECT_STATEID)
			goto malformed;
		value = nl_data(mapped, &value_len);
		if (!value_len || value_len > sizeof(member->mapped_handle))
			goto malformed;
		member->mapped_handle_len = (uint32_t)value_len;
		memcpy(member->mapped_handle, value, value_len);
		value = nl_data(stateid, &value_len);
		if (value_len != sizeof(member->stateid) ||
		    bytes_zero(value, value_len))
			goto malformed;
		memcpy(member->stateid, value, value_len);
		member->ordinary_handle_len =
			config->objects[i].ordinary_handle_len;
		memcpy(member->ordinary_handle,
		       config->objects[i].ordinary_handle,
		       member->ordinary_handle_len);
		for (uint32_t j = 0; j < i; j++)
			if ((member->mapped_handle_len ==
				     snapshot->members[j].mapped_handle_len &&
			     !memcmp(member->mapped_handle,
				     snapshot->members[j].mapped_handle,
				     member->mapped_handle_len)) ||
			    !memcmp(member->stateid,
				    snapshot->members[j].stateid,
				    sizeof(member->stateid)))
				goto malformed;
	}
	if (object_offset != len)
		goto malformed;
	*snapshot_out = snapshot;
	return 0;

malformed:
	free(snapshot);
	return -EBADMSG;
}

int ffv2_prototype_challenge_parse(const uint8_t *attributes,
				   size_t attributes_len, uint8_t nonce[32])
{
	const struct nlattr *nonce_attr, *expiry_attr;
	size_t offset = 0, value_len;
	uint64_t expires;
	const void *value;

	if (!attributes || !nonce)
		return -EINVAL;
	nonce_attr = nl_next(attributes, attributes_len, &offset);
	expiry_attr = nl_next(attributes, attributes_len, &offset);
	if (!nonce_attr || !expiry_attr || offset != attributes_len ||
	    (nonce_attr->nla_type & NLA_TYPE_MASK) != FFV2_NFSD_A_NONCE ||
	    (expiry_attr->nla_type & NLA_TYPE_MASK) != FFV2_NFSD_A_EXPIRES)
		return -EBADMSG;
	value = nl_data(nonce_attr, &value_len);
	if (value_len != 32)
		return -EBADMSG;
	memcpy(nonce, value, 32);
	value = nl_data(expiry_attr, &value_len);
	if (value_len != sizeof(expires))
		goto malformed;
	memcpy(&expires, value, sizeof(expires));
	if (!expires)
		goto malformed;
	return 0;

malformed:
	OPENSSL_cleanse(nonce, 32);
	return -EBADMSG;
}

static int prototype_nl_challenge(void *context, uint8_t nonce[32])
{
	struct ffv2_nl *nl = context;
	uint8_t reply[FFV2_NL_BUFFER_SIZE];
	size_t reply_len;
	int error;

	nl_start(nl, nl->family, FFV2_NFSD_CMD_PROTOTYPE_CHALLENGE);
	error = nl_talk(nl, reply, sizeof(reply), &reply_len, true);
	if (error)
		return error;
	error = ffv2_prototype_challenge_parse(reply, reply_len, nonce);
	return error;
}

static int
prototype_nl_register(void *context,
		      const struct reffs_prototype_registration_config *config,
		      uint32_t dstore_id, const uint8_t nonce[32],
		      struct ffv2_prototype_snapshot **snapshot_out)
{
	struct ffv2_nl *nl = context;
	uint8_t reply[FFV2_NL_BUFFER_SIZE];
	size_t reply_len, message_len;
	int error;

	error = ffv2_prototype_request_build(config, nonce, nl->family,
					     nl->seq + 1, nl->buffer,
					     sizeof(nl->buffer), &message_len);
	if (error)
		return error;
	((struct nlmsghdr *)nl->buffer)->nlmsg_len = message_len;
	nl->seq++;
	error = nl_talk(nl, reply, sizeof(reply), &reply_len, true);
	OPENSSL_cleanse(nl->buffer, sizeof(nl->buffer));
	if (error)
		return error;
	return ffv2_prototype_reply_parse(config, dstore_id, reply, reply_len,
					  snapshot_out);
}

static int prototype_nl_open(void *context)
{
	return nl_open(context);
}

static int prototype_nl_disable(void *context)
{
	struct ffv2_nl *nl = context;

	return nl_simple(nl, FFV2_NFSD_CMD_PROTOTYPE_DISABLE);
}

static int prototype_nl_enable(void *context)
{
	struct ffv2_nl *nl = context;

	return nl_simple(nl, FFV2_NFSD_CMD_PROTOTYPE_ENABLE);
}

static void prototype_nl_close(void *context)
{
	struct ffv2_nl *nl = context;

	OPENSSL_cleanse(nl->buffer, sizeof(nl->buffer));
	close(nl->fd);
}

int ffv2_prototype_register_transport(
	struct dstore *ds, const struct ffv2_prototype_transport *transport)
{
	struct ffv2_prototype_snapshot *snapshot = NULL;
	struct ffv2_prototype_snapshot *old;
	uint8_t nonce[32] = { 0 };
	bool opened = false;
	int error;

	if (!ds || !prototype_config_valid(&ds->ds_prototype_config) ||
	    !transport || !transport->open || !transport->disable ||
	    !transport->enable || !transport->challenge ||
	    !transport->register_vector || !transport->close)
		return -EINVAL;
	pthread_mutex_lock(&ds->ds_prototype_mutex);
	/* Stop advertising before the kernel tears down the old provider. */
	pthread_rwlock_wrlock(&ds->ds_prototype_lock);
	old = ds->ds_prototype_snapshot;
	ds->ds_prototype_snapshot = NULL;
	pthread_rwlock_unlock(&ds->ds_prototype_lock);
	free(old);

	error = transport->open(transport->context);
	if (error)
		goto out_unlock;
	opened = true;
	error = transport->disable(transport->context);
	if (error && error != -EOPNOTSUPP)
		goto out_close;
	error = transport->enable(transport->context);
	if (error)
		goto out_close;
	error = transport->challenge(transport->context, nonce);
	if (error)
		goto out_disable;
	error = transport->register_vector(transport->context,
					   &ds->ds_prototype_config, ds->ds_id,
					   nonce, &snapshot);
	OPENSSL_cleanse(nonce, sizeof(nonce));
	if (error) {
		free(snapshot);
		goto out_disable;
	}
	error = ffv2_prototype_snapshot_replace(ds, snapshot);
	if (error) {
		free(snapshot);
		goto out_disable;
	}
	TRACE("dstore[%u]: published %u-member FFv2 prototype generation %llu",
	      ds->ds_id, snapshot->object_count,
	      (unsigned long long)snapshot->generation);
	goto out_close;

out_disable:
	OPENSSL_cleanse(nonce, sizeof(nonce));
	transport->disable(transport->context);

out_close:
	if (opened)
		transport->close(transport->context);
out_unlock:
	pthread_mutex_unlock(&ds->ds_prototype_mutex);
	return error;
}

int ffv2_prototype_register_dstore(struct dstore *ds)
{
	struct ffv2_nl nl;
	const struct ffv2_prototype_transport transport = {
		.context = &nl,
		.open = prototype_nl_open,
		.disable = prototype_nl_disable,
		.enable = prototype_nl_enable,
		.challenge = prototype_nl_challenge,
		.register_vector = prototype_nl_register,
		.close = prototype_nl_close,
	};

	return ffv2_prototype_register_transport(ds, &transport);
}

int ffv2_prototype_disable(void)
{
	struct ffv2_nl nl;
	int ret;

	ret = nl_open(&nl);
	if (ret)
		return ret;
	ret = nl_simple(&nl, FFV2_NFSD_CMD_PROTOTYPE_DISABLE);
	OPENSSL_cleanse(nl.buffer, sizeof(nl.buffer));
	close(nl.fd);
	return ret;
}

void ffv2_prototype_unregister_dstore(struct dstore *ds)
{
	struct ffv2_prototype_snapshot *snapshot;
	struct ffv2_nl nl;

	if (!ds)
		return;
	pthread_mutex_lock(&ds->ds_prototype_mutex);
	pthread_rwlock_wrlock(&ds->ds_prototype_lock);
	snapshot = ds->ds_prototype_snapshot;
	ds->ds_prototype_snapshot = NULL;
	pthread_rwlock_unlock(&ds->ds_prototype_lock);
	free(snapshot);
	if (!ds->ds_prototype_config.enabled)
		goto out_unlock;
	if (!nl_open(&nl)) {
		nl_simple(&nl, FFV2_NFSD_CMD_PROTOTYPE_DISABLE);
		OPENSSL_cleanse(nl.buffer, sizeof(nl.buffer));
		close(nl.fd);
	}
	OPENSSL_cleanse(ds->ds_prototype_config.binding_token,
			sizeof(ds->ds_prototype_config.binding_token));
out_unlock:
	pthread_mutex_unlock(&ds->ds_prototype_mutex);
}

const struct ffv2_prototype_snapshot *
ffv2_prototype_snapshot_borrow(struct dstore *ds)
{
	if (!ds)
		return NULL;
	pthread_rwlock_rdlock(&ds->ds_prototype_lock);
	return ds->ds_prototype_snapshot;
}

void ffv2_prototype_snapshot_release(struct dstore *ds)
{
	if (ds)
		pthread_rwlock_unlock(&ds->ds_prototype_lock);
}

int ffv2_prototype_snapshot_replace(struct dstore *ds,
				    struct ffv2_prototype_snapshot *snapshot)
{
	const struct reffs_prototype_registration_config *config;
	struct ffv2_prototype_snapshot *old;
	uint8_t expected_source[16];

	if (!ds || !snapshot)
		return -EINVAL;
	config = &ds->ds_prototype_config;
	if (!config->enabled || snapshot->dstore_id != ds->ds_id ||
	    snapshot->chunk_size != config->chunk_size ||
	    snapshot->data_count != config->data_count ||
	    snapshot->parity_count != config->parity_count ||
	    snapshot->writer_id != config->writer_id ||
	    snapshot->pnfs_clientid != config->pnfs_clientid ||
	    snapshot->object_count != config->object_count ||
	    !snapshot->generation)
		return -EINVAL;
	expected_source_uuid(config, expected_source);
	if (memcmp(snapshot->source_uuid, expected_source,
		   sizeof(expected_source)) ||
	    bytes_zero(snapshot->service_uuid,
		       sizeof(snapshot->service_uuid)) ||
	    bytes_zero(snapshot->replay_uuid, sizeof(snapshot->replay_uuid)) ||
	    !memcmp(snapshot->source_uuid, snapshot->service_uuid, 16) ||
	    !memcmp(snapshot->source_uuid, snapshot->replay_uuid, 16) ||
	    !memcmp(snapshot->service_uuid, snapshot->replay_uuid, 16))
		return -EINVAL;
	for (uint32_t i = 0; i < config->object_count; i++) {
		const struct ffv2_prototype_member *member =
			&snapshot->members[i];

		if (snapshot->members[i].ordinary_handle_len !=
			    config->objects[i].ordinary_handle_len ||
		    memcmp(snapshot->members[i].ordinary_handle,
			   config->objects[i].ordinary_handle,
			   config->objects[i].ordinary_handle_len) ||
		    !member->mapped_handle_len ||
		    member->mapped_handle_len > sizeof(member->mapped_handle) ||
		    bytes_zero(member->stateid, sizeof(member->stateid)))
			return -EINVAL;
		for (uint32_t j = 0; j < i; j++) {
			const struct ffv2_prototype_member *prior =
				&snapshot->members[j];

			if ((member->mapped_handle_len ==
				     prior->mapped_handle_len &&
			     !memcmp(member->mapped_handle,
				     prior->mapped_handle,
				     member->mapped_handle_len)) ||
			    !memcmp(member->stateid, prior->stateid,
				    sizeof(member->stateid)))
				return -EINVAL;
		}
	}
	pthread_rwlock_wrlock(&ds->ds_prototype_lock);
	old = ds->ds_prototype_snapshot;
	if (old && snapshot->generation <= old->generation) {
		pthread_rwlock_unlock(&ds->ds_prototype_lock);
		return -ESTALE;
	}
	ds->ds_prototype_snapshot = snapshot;
	pthread_rwlock_unlock(&ds->ds_prototype_lock);
	free(old);
	return 0;
}

int ffv2_prototype_snapshot_select(
	const struct layout_segment *seg, uint32_t writer_id,
	uint64_t pnfs_clientid, struct dstore **ds_out,
	const struct ffv2_prototype_snapshot **snapshot_out)
{
	struct dstore *prototype_ds = NULL;
	const struct ffv2_prototype_snapshot *snapshot;

	*ds_out = NULL;
	*snapshot_out = NULL;
	if (!seg)
		return -EINVAL;
	for (uint32_t i = 0; i < seg->ls_nfiles; i++) {
		struct dstore *ds = dstore_find(seg->ls_files[i].ldf_dstore_id);

		if (!ds)
			continue;
		if (ds->ds_prototype_config.enabled) {
			if (prototype_ds && prototype_ds->ds_id != ds->ds_id) {
				dstore_put(ds);
				dstore_put(prototype_ds);
				return -ESTALE;
			}
			if (!prototype_ds)
				prototype_ds = ds;
			else
				dstore_put(ds);
		} else {
			dstore_put(ds);
		}
	}
	if (!prototype_ds)
		return 0;
	snapshot = ffv2_prototype_snapshot_borrow(prototype_ds);
	if (!snapshot || snapshot->writer_id != writer_id ||
	    snapshot->pnfs_clientid != pnfs_clientid ||
	    snapshot->chunk_size != seg->ls_stripe_unit ||
	    snapshot->data_count != seg->ls_k ||
	    snapshot->parity_count != seg->ls_m ||
	    snapshot->object_count != seg->ls_nfiles)
		goto mismatch;
	for (uint32_t i = 0; i < seg->ls_nfiles; i++) {
		const struct layout_data_file *file = &seg->ls_files[i];
		const struct ffv2_prototype_member *member =
			&snapshot->members[i];

		if (file->ldf_dstore_id != prototype_ds->ds_id ||
		    file->ldf_fh_len != member->ordinary_handle_len ||
		    memcmp(file->ldf_fh, member->ordinary_handle,
			   file->ldf_fh_len))
			goto mismatch;
	}
	*ds_out = prototype_ds;
	*snapshot_out = snapshot;
	return 1;

mismatch:
	ffv2_prototype_snapshot_release(prototype_ds);
	dstore_put(prototype_ds);
	return -ESTALE;
}
