/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" /* IWYU pragma: keep */
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/sha.h>
#include <zlib.h>

#include "reffs/fixed_inventory.h"
#include "reffs/posix_shims.h"

#define FFV2_INVENTORY_MAGIC 0x52464956U /* RFIV */
#define FFV2_INVENTORY_MAX_ENCODED 8192

static pthread_mutex_t inventory_mutex = PTHREAD_MUTEX_INITIALIZER;

void ffv2_fixed_inventory_lock(void)
{
	pthread_mutex_lock(&inventory_mutex);
}

void ffv2_fixed_inventory_unlock(void)
{
	pthread_mutex_unlock(&inventory_mutex);
}

struct encoder {
	uint8_t *data;
	size_t len;
	size_t capacity;
};

struct decoder {
	const uint8_t *data;
	size_t len;
	size_t offset;
};

static uint64_t cpu_to_be64_value(uint64_t value)
{
	return ((uint64_t)htonl((uint32_t)(value >> 32))) |
	       ((uint64_t)htonl((uint32_t)value) << 32);
}

static uint64_t be64_to_cpu_value(uint64_t value)
{
	return ((uint64_t)ntohl((uint32_t)(value >> 32))) |
	       ((uint64_t)ntohl((uint32_t)value) << 32);
}

static int put_bytes(struct encoder *encoder, const void *data, size_t len)
{
	if (len > encoder->capacity - encoder->len)
		return -EOVERFLOW;
	memcpy(encoder->data + encoder->len, data, len);
	encoder->len += len;
	return 0;
}

static int put_u32(struct encoder *encoder, uint32_t value)
{
	value = htonl(value);
	return put_bytes(encoder, &value, sizeof(value));
}

static int put_u64(struct encoder *encoder, uint64_t value)
{
	value = cpu_to_be64_value(value);
	return put_bytes(encoder, &value, sizeof(value));
}

static int put_string(struct encoder *encoder, const char *value, size_t limit)
{
	size_t len = strnlen(value, limit);
	int ret;

	if (!len || len == limit || len > UINT32_MAX)
		return -EINVAL;
	ret = put_u32(encoder, (uint32_t)len);
	return ret ? ret : put_bytes(encoder, value, len);
}

static int get_bytes(struct decoder *decoder, void *data, size_t len)
{
	if (len > decoder->len - decoder->offset)
		return -EBADMSG;
	memcpy(data, decoder->data + decoder->offset, len);
	decoder->offset += len;
	return 0;
}

static int get_u32(struct decoder *decoder, uint32_t *value)
{
	uint32_t encoded;
	int ret = get_bytes(decoder, &encoded, sizeof(encoded));

	if (!ret)
		*value = ntohl(encoded);
	return ret;
}

static int get_u64(struct decoder *decoder, uint64_t *value)
{
	uint64_t encoded;
	int ret = get_bytes(decoder, &encoded, sizeof(encoded));

	if (!ret)
		*value = be64_to_cpu_value(encoded);
	return ret;
}

static int get_string(struct decoder *decoder, char *value, size_t capacity)
{
	uint32_t len;
	int ret = get_u32(decoder, &len);

	if (ret)
		return ret;
	if (!len || len >= capacity)
		return -EBADMSG;
	ret = get_bytes(decoder, value, len);
	if (!ret)
		value[len] = '\0';
	return ret;
}

static int encode_identity(struct encoder *encoder,
			   const struct ffv2_fixed_inventory_identity *identity,
			   bool include_digest)
{
	int ret;

#define PUT(_expr)                  \
	do {                        \
		ret = (_expr);      \
		if (ret)            \
			return ret; \
	} while (0)
	PUT(put_u32(encoder, identity->dstore_id));
	PUT(put_string(encoder, identity->address, sizeof(identity->address)));
	PUT(put_string(encoder, identity->export_path,
		       sizeof(identity->export_path)));
	PUT(put_string(encoder, identity->auth_domain,
		       sizeof(identity->auth_domain)));
	PUT(put_bytes(encoder, identity->store_uuid,
		      sizeof(identity->store_uuid)));
	PUT(put_bytes(encoder, identity->token_digest,
		      sizeof(identity->token_digest)));
	PUT(put_u32(encoder, identity->chunk_size));
	PUT(put_u32(encoder, identity->data_count));
	PUT(put_u32(encoder, identity->parity_count));
	PUT(put_u32(encoder, identity->writer_id));
	PUT(put_u64(encoder, identity->pnfs_clientid));
	PUT(put_u32(encoder, identity->object_count));
	for (uint32_t i = 0; i < identity->object_count; i++) {
		const struct reffs_prototype_object_config *object =
			&identity->objects[i];

		PUT(put_string(encoder, object->name, sizeof(object->name)));
		PUT(put_u32(encoder, object->ordinary_handle_len));
		PUT(put_bytes(encoder, object->ordinary_handle,
			      object->ordinary_handle_len));
		PUT(put_bytes(encoder, object->persisted_handle,
			      sizeof(object->persisted_handle)));
	}
	if (include_digest)
		PUT(put_bytes(encoder, identity->digest,
			      sizeof(identity->digest)));
#undef PUT
	return 0;
}

static int decode_identity(struct decoder *decoder,
			   struct ffv2_fixed_inventory_identity *identity)
{
	int ret;

#define GET(_expr)                  \
	do {                        \
		ret = (_expr);      \
		if (ret)            \
			return ret; \
	} while (0)
	GET(get_u32(decoder, &identity->dstore_id));
	GET(get_string(decoder, identity->address, sizeof(identity->address)));
	GET(get_string(decoder, identity->export_path,
		       sizeof(identity->export_path)));
	GET(get_string(decoder, identity->auth_domain,
		       sizeof(identity->auth_domain)));
	GET(get_bytes(decoder, identity->store_uuid,
		      sizeof(identity->store_uuid)));
	GET(get_bytes(decoder, identity->token_digest,
		      sizeof(identity->token_digest)));
	GET(get_u32(decoder, &identity->chunk_size));
	GET(get_u32(decoder, &identity->data_count));
	GET(get_u32(decoder, &identity->parity_count));
	GET(get_u32(decoder, &identity->writer_id));
	GET(get_u64(decoder, &identity->pnfs_clientid));
	GET(get_u32(decoder, &identity->object_count));
	if (identity->object_count != 2 || identity->data_count != 1 ||
	    identity->parity_count != 1)
		return -EBADMSG;
	for (uint32_t i = 0; i < identity->object_count; i++) {
		struct reffs_prototype_object_config *object =
			&identity->objects[i];

		GET(get_string(decoder, object->name, sizeof(object->name)));
		GET(get_u32(decoder, &object->ordinary_handle_len));
		if (!object->ordinary_handle_len ||
		    object->ordinary_handle_len >
			    sizeof(object->ordinary_handle))
			return -EBADMSG;
		GET(get_bytes(decoder, object->ordinary_handle,
			      object->ordinary_handle_len));
		GET(get_bytes(decoder, object->persisted_handle,
			      sizeof(object->persisted_handle)));
	}
	GET(get_bytes(decoder, identity->digest, sizeof(identity->digest)));
#undef GET
	return 0;
}

static int identity_digest(struct ffv2_fixed_inventory_identity *identity)
{
	uint8_t data[FFV2_INVENTORY_MAX_ENCODED];
	struct encoder encoder = {
		.data = data,
		.capacity = sizeof(data),
	};
	int ret = encode_identity(&encoder, identity, false);

	if (ret)
		return ret;
	if (!SHA256(data, encoder.len, identity->digest))
		return -EIO;
	return 0;
}

static int identity_validate(struct ffv2_fixed_inventory_identity *identity)
{
	uint8_t expected[FFV2_FIXED_INVENTORY_DIGEST_SIZE];

	memcpy(expected, identity->digest, sizeof(expected));
	if (identity_digest(identity))
		return -EBADMSG;
	if (memcmp(expected, identity->digest, sizeof(expected)))
		return -EBADMSG;
	for (uint32_t i = 0; i < identity->object_count; i++) {
		for (uint32_t j = 0; j < i; j++) {
			if (!strcmp(identity->objects[i].name,
				    identity->objects[j].name) ||
			    (identity->objects[i].ordinary_handle_len ==
				     identity->objects[j].ordinary_handle_len &&
			     !memcmp(identity->objects[i].ordinary_handle,
				     identity->objects[j].ordinary_handle,
				     identity->objects[i].ordinary_handle_len)) ||
			    !memcmp(identity->objects[i].persisted_handle,
				    identity->objects[j].persisted_handle,
				    sizeof(identity->objects[i]
						   .persisted_handle)))
				return -EBADMSG;
		}
	}
	memcpy(identity->digest, expected, sizeof(expected));
	return 0;
}

int ffv2_fixed_inventory_identity_init(
	struct ffv2_fixed_inventory_identity *identity, uint32_t dstore_id,
	const char *address, const char *export_path,
	const struct reffs_prototype_registration_config *config)
{
	if (!identity || !dstore_id || !address || !address[0] ||
	    !export_path || !export_path[0] || !config || !config->enabled ||
	    !config->fixed_inventory || config->data_count != 1 ||
	    config->parity_count != 1 || config->object_count != 2 ||
	    !config->pnfs_clientid)
		return -EINVAL;
	memset(identity, 0, sizeof(*identity));
	identity->dstore_id = dstore_id;
	if (snprintf(identity->address, sizeof(identity->address), "%s",
		     address) >= (int)sizeof(identity->address) ||
	    snprintf(identity->export_path, sizeof(identity->export_path), "%s",
		     export_path) >= (int)sizeof(identity->export_path))
		return -ENAMETOOLONG;
	memcpy(identity->auth_domain, config->auth_domain,
	       sizeof(identity->auth_domain));
	memcpy(identity->store_uuid, config->store_uuid,
	       sizeof(identity->store_uuid));
	if (!SHA256(config->binding_token, sizeof(config->binding_token),
		    identity->token_digest))
		return -EIO;
	identity->chunk_size = config->chunk_size;
	identity->data_count = config->data_count;
	identity->parity_count = config->parity_count;
	identity->writer_id = config->writer_id;
	identity->pnfs_clientid = config->pnfs_clientid;
	identity->object_count = config->object_count;
	memcpy(identity->objects, config->objects, sizeof(identity->objects));
	return identity_digest(identity);
}

bool ffv2_fixed_inventory_identity_equal(
	const struct ffv2_fixed_inventory_identity *left,
	const struct ffv2_fixed_inventory_identity *right)
{
	return left && right &&
	       !memcmp(left->digest, right->digest, sizeof(left->digest));
}

int ffv2_fixed_inventory_restart_compatible(
	const struct ffv2_fixed_inventory_identity *stored,
	const struct ffv2_fixed_inventory_identity *current, bool *compatible)
{
	struct ffv2_fixed_inventory_identity stored_copy, current_copy;
	int ret;

	if (!stored || !current || !compatible)
		return -EINVAL;
	*compatible = false;
	stored_copy = *stored;
	current_copy = *current;
	if (identity_validate(&stored_copy) || identity_validate(&current_copy))
		return -EBADMSG;
	current_copy.pnfs_clientid = stored_copy.pnfs_clientid;
	ret = identity_digest(&current_copy);
	if (ret)
		return ret;
	*compatible = ffv2_fixed_inventory_identity_equal(&stored_copy,
							  &current_copy);
	return 0;
}

int ffv2_fixed_inventory_transition(struct ffv2_fixed_inventory_record *record,
				    enum ffv2_fixed_inventory_state next,
				    const uuid_t owner_sb_uuid,
				    uint64_t owner_ino)
{
	bool allowed = false;

	if (!record || next > FFV2_INVENTORY_FENCED)
		return -EINVAL;
	switch (record->state) {
	case FFV2_INVENTORY_FREE:
		allowed = next == FFV2_INVENTORY_CLAIMED;
		break;
	case FFV2_INVENTORY_CLAIMED:
		allowed = next == FFV2_INVENTORY_ASSIGNED ||
			  next == FFV2_INVENTORY_RETIRED ||
			  next == FFV2_INVENTORY_FENCED;
		break;
	case FFV2_INVENTORY_ASSIGNED:
		allowed = next == FFV2_INVENTORY_RETIRED ||
			  next == FFV2_INVENTORY_FENCED;
		break;
	case FFV2_INVENTORY_RETIRED:
	case FFV2_INVENTORY_FENCED:
		break;
	}
	if (!allowed)
		return -EINVAL;
	if (next != FFV2_INVENTORY_FREE && (!owner_sb_uuid || !owner_ino))
		return -EINVAL;
	if (record->state != FFV2_INVENTORY_FREE &&
	    (record->owner_ino != owner_ino ||
	     uuid_compare(record->owner_sb_uuid, owner_sb_uuid)))
		return -EPERM;
	record->state = next;
	if (next == FFV2_INVENTORY_RETIRED || next == FFV2_INVENTORY_FENCED)
		record->metadata_state = FFV2_METADATA_CLEAN;
	uuid_copy(record->owner_sb_uuid, owner_sb_uuid);
	record->owner_ino = owner_ino;
	record->generation++;
	return 0;
}

int ffv2_fixed_metadata_transition(struct ffv2_fixed_inventory_record *record,
				   enum ffv2_fixed_metadata_state next)
{
	bool allowed = false;

	if (!record || record->state != FFV2_INVENTORY_ASSIGNED ||
	    next > FFV2_METADATA_COMMITTED)
		return -EINVAL;
	switch (record->metadata_state) {
	case FFV2_METADATA_CLEAN:
		allowed = next == FFV2_METADATA_DIRTY;
		break;
	case FFV2_METADATA_DIRTY:
		allowed = next == FFV2_METADATA_COMMITTED;
		break;
	case FFV2_METADATA_COMMITTED:
		allowed = next == FFV2_METADATA_CLEAN;
		break;
	}
	if (!allowed)
		return -EINVAL;
	if (next == FFV2_METADATA_DIRTY)
		record->metadata_epoch++;
	record->metadata_state = next;
	return 0;
}

int ffv2_fixed_inventory_encode(const struct ffv2_fixed_inventory_record *record,
				uint8_t **data_out, size_t *len_out)
{
	struct encoder encoder;
	uint8_t *data;
	uint32_t checksum;
	int ret;

	if (!record || !data_out || !len_out ||
	    record->state > FFV2_INVENTORY_FENCED)
		return -EINVAL;
	data = calloc(1, FFV2_INVENTORY_MAX_ENCODED);
	if (!data)
		return -ENOMEM;
	encoder = (struct encoder){
		.data = data,
		.capacity = FFV2_INVENTORY_MAX_ENCODED,
	};
#define PUT(_expr)                \
	do {                      \
		ret = (_expr);    \
		if (ret)          \
			goto out; \
	} while (0)
	PUT(put_u32(&encoder, FFV2_INVENTORY_MAGIC));
	PUT(put_u32(&encoder, FFV2_FIXED_INVENTORY_VERSION));
	PUT(encode_identity(&encoder, &record->identity, true));
	PUT(put_u32(&encoder, record->state));
	PUT(put_bytes(&encoder, record->owner_sb_uuid,
		      sizeof(record->owner_sb_uuid)));
	PUT(put_u64(&encoder, record->owner_ino));
	PUT(put_u64(&encoder, record->segment_offset));
	PUT(put_u64(&encoder, record->segment_length));
	PUT(put_u32(&encoder, record->segment_stripe_unit));
	PUT(put_u32(&encoder, record->segment_layout_type));
	PUT(put_u32(&encoder, record->segment_checksum_algorithm));
	PUT(put_u32(&encoder, record->metadata_state));
	PUT(put_u64(&encoder, record->metadata_epoch));
	PUT(put_u64(&encoder, record->generation));
	checksum = (uint32_t)crc32(0, data, encoder.len);
	PUT(put_u32(&encoder, checksum));
#undef PUT
	*data_out = data;
	*len_out = encoder.len;
	return 0;
out:
	free(data);
	return ret;
}

int ffv2_fixed_inventory_decode(const uint8_t *data, size_t len,
				struct ffv2_fixed_inventory_record *record)
{
	struct decoder decoder;
	uint32_t magic, version, state, metadata_state, stored_crc;
	uint32_t computed_crc;
	int ret;

	if (!data || !record || len < 12 || len > FFV2_INVENTORY_MAX_ENCODED)
		return -EINVAL;
	memset(record, 0, sizeof(*record));
	memcpy(&stored_crc, data + len - sizeof(stored_crc),
	       sizeof(stored_crc));
	stored_crc = ntohl(stored_crc);
	computed_crc = (uint32_t)crc32(0, data, len - sizeof(stored_crc));
	if (stored_crc != computed_crc)
		return -EBADMSG;
	decoder = (struct decoder){
		.data = data,
		.len = len - sizeof(stored_crc),
	};
#define GET(_expr)                  \
	do {                        \
		ret = (_expr);      \
		if (ret)            \
			return ret; \
	} while (0)
	GET(get_u32(&decoder, &magic));
	GET(get_u32(&decoder, &version));
	if (magic != FFV2_INVENTORY_MAGIC ||
	    version != FFV2_FIXED_INVENTORY_VERSION)
		return -EBADMSG;
	GET(decode_identity(&decoder, &record->identity));
	GET(get_u32(&decoder, &state));
	if (state > FFV2_INVENTORY_FENCED)
		return -EBADMSG;
	record->state = (enum ffv2_fixed_inventory_state)state;
	GET(get_bytes(&decoder, record->owner_sb_uuid,
		      sizeof(record->owner_sb_uuid)));
	GET(get_u64(&decoder, &record->owner_ino));
	GET(get_u64(&decoder, &record->segment_offset));
	GET(get_u64(&decoder, &record->segment_length));
	GET(get_u32(&decoder, &record->segment_stripe_unit));
	GET(get_u32(&decoder, &record->segment_layout_type));
	GET(get_u32(&decoder, &record->segment_checksum_algorithm));
	GET(get_u32(&decoder, &metadata_state));
	if (metadata_state > FFV2_METADATA_COMMITTED)
		return -EBADMSG;
	record->metadata_state = (enum ffv2_fixed_metadata_state)metadata_state;
	GET(get_u64(&decoder, &record->metadata_epoch));
	GET(get_u64(&decoder, &record->generation));
#undef GET
	if (decoder.offset != decoder.len ||
	    identity_validate(&record->identity))
		return -EBADMSG;
	if (record->state == FFV2_INVENTORY_FREE) {
		if (record->owner_ino || !uuid_is_null(record->owner_sb_uuid) ||
		    record->metadata_state != FFV2_METADATA_CLEAN ||
		    record->metadata_epoch)
			return -EBADMSG;
	} else if (!record->owner_ino || uuid_is_null(record->owner_sb_uuid)) {
		return -EBADMSG;
	} else if (record->state != FFV2_INVENTORY_ASSIGNED &&
		   record->metadata_state != FFV2_METADATA_CLEAN) {
		return -EBADMSG;
	}
	return 0;
}

static int inventory_path(char *path, size_t capacity, const char *state_dir,
			  uint32_t dstore_id)
{
	int len;

	if (!state_dir || !state_dir[0])
		return -EINVAL;
	len = snprintf(path, capacity, "%s/ffv2-fixed-inventory-%u", state_dir,
		       dstore_id);
	return len < 0 || (size_t)len >= capacity ? -ENAMETOOLONG : 0;
}

int ffv2_fixed_inventory_save(const char *state_dir,
			      const struct ffv2_fixed_inventory_record *record)
{
	static _Atomic uint64_t sequence;
	char path[PATH_MAX], temporary[PATH_MAX];
	uint8_t *data = NULL;
	size_t len = 0, written = 0;
	uint64_t seq;
	int fd = -1, dirfd = -1, ret;

	ret = inventory_path(path, sizeof(path), state_dir,
			     record->identity.dstore_id);
	if (ret)
		return ret;
	seq = atomic_fetch_add_explicit(&sequence, 1, memory_order_relaxed);
	if (snprintf(temporary, sizeof(temporary), "%s.tmp.%u.%" PRIu64, path,
		     (unsigned)getpid(), seq) >= (int)sizeof(temporary))
		return -ENAMETOOLONG;
	ret = ffv2_fixed_inventory_encode(record, &data, &len);
	if (ret)
		return ret;
	fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (fd < 0) {
		ret = -errno;
		goto out;
	}
	while (written < len) {
		ssize_t amount = write(fd, data + written, len - written);

		if (amount < 0) {
			ret = -errno;
			goto out;
		}
		written += (size_t)amount;
	}
	if (reffs_fdatasync(fd)) {
		ret = -errno;
		goto out;
	}
	if (close(fd)) {
		fd = -1;
		ret = -errno;
		goto out;
	}
	fd = -1;
	if (rename(temporary, path)) {
		ret = -errno;
		goto out;
	}
	dirfd = open(state_dir, O_RDONLY | O_DIRECTORY);
	if (dirfd < 0 || fsync(dirfd)) {
		ret = -errno;
		goto out;
	}
	ret = 0;
out:
	if (dirfd >= 0)
		close(dirfd);
	if (fd >= 0)
		close(fd);
	if (ret)
		unlink(temporary);
	free(data);
	return ret;
}

int ffv2_fixed_inventory_load(const char *state_dir, uint32_t dstore_id,
			      struct ffv2_fixed_inventory_record *record)
{
	char path[PATH_MAX];
	uint8_t data[FFV2_INVENTORY_MAX_ENCODED + 1];
	ssize_t len;
	int fd, ret;

	ret = inventory_path(path, sizeof(path), state_dir, dstore_id);
	if (ret)
		return ret;
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -errno;
	len = read(fd, data, sizeof(data));
	if (len < 0)
		ret = -errno;
	else if ((size_t)len == sizeof(data))
		ret = -EFBIG;
	else
		ret = ffv2_fixed_inventory_decode(data, (size_t)len, record);
	close(fd);
	if (!ret && record->identity.dstore_id != dstore_id)
		ret = -EBADMSG;
	return ret;
}
