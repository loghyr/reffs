/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * RocksDB key encoding utilities.
 *
 * All integer keys are 8-byte big-endian for correct lexicographic
 * ordering in RocksDB (which sorts keys as byte strings).
 *
 * Key formats:
 *   inodes CF:  "ino:" + BE64(ino)           = 12 bytes
 *   dirs CF:    "dir:" + BE64(parent) + BE64(cookie) = 20 bytes
 *   symlinks:   "lnk:" + BE64(ino)           = 12 bytes
 *   layouts:    "lay:" + BE64(ino)           = 12 bytes
 *   chunks:     "chk:" + BE64(ino) + BE64(offset) = 20 bytes
 */

#ifndef _REFFS_ROCKSDB_KEYS_H
#define _REFFS_ROCKSDB_KEYS_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Maximum key sizes (prefix + encoded integers) */
#define ROCKSDB_KEY_INO_SIZE 12
#define ROCKSDB_KEY_DIR_SIZE 20
#define ROCKSDB_KEY_LNK_SIZE 12
#define ROCKSDB_KEY_LAY_SIZE 12
#define ROCKSDB_KEY_CHK_SIZE 20
#define ROCKSDB_KEY_MAX_SIZE 20

static inline void encode_be64(uint8_t *buf, uint64_t val)
{
	buf[0] = (uint8_t)(val >> 56);
	buf[1] = (uint8_t)(val >> 48);
	buf[2] = (uint8_t)(val >> 40);
	buf[3] = (uint8_t)(val >> 32);
	buf[4] = (uint8_t)(val >> 24);
	buf[5] = (uint8_t)(val >> 16);
	buf[6] = (uint8_t)(val >> 8);
	buf[7] = (uint8_t)(val);
}

static inline uint64_t decode_be64(const uint8_t *buf)
{
	return ((uint64_t)buf[0] << 56) | ((uint64_t)buf[1] << 48) |
	       ((uint64_t)buf[2] << 40) | ((uint64_t)buf[3] << 32) |
	       ((uint64_t)buf[4] << 24) | ((uint64_t)buf[5] << 16) |
	       ((uint64_t)buf[6] << 8) | ((uint64_t)buf[7]);
}

/* Build key: "ino:" + BE64(ino) --> 12 bytes */
static inline size_t rocksdb_key_ino(uint8_t *buf, uint64_t ino)
{
	memcpy(buf, "ino:", 4);
	encode_be64(buf + 4, ino);
	return ROCKSDB_KEY_INO_SIZE;
}

/* Build key: "dir:" + BE64(parent_ino) + BE64(cookie) --> 20 bytes */
static inline size_t rocksdb_key_dir(uint8_t *buf, uint64_t parent_ino,
				     uint64_t cookie)
{
	memcpy(buf, "dir:", 4);
	encode_be64(buf + 4, parent_ino);
	encode_be64(buf + 12, cookie);
	return ROCKSDB_KEY_DIR_SIZE;
}

/* Build prefix: "dir:" + BE64(parent_ino) --> 12 bytes (for iteration) */
static inline size_t rocksdb_key_dir_prefix(uint8_t *buf, uint64_t parent_ino)
{
	memcpy(buf, "dir:", 4);
	encode_be64(buf + 4, parent_ino);
	return 12;
}

/* Build key: "lnk:" + BE64(ino) --> 12 bytes */
static inline size_t rocksdb_key_lnk(uint8_t *buf, uint64_t ino)
{
	memcpy(buf, "lnk:", 4);
	encode_be64(buf + 4, ino);
	return ROCKSDB_KEY_LNK_SIZE;
}

/* Build key: "lay:" + BE64(ino) --> 12 bytes */
static inline size_t rocksdb_key_lay(uint8_t *buf, uint64_t ino)
{
	memcpy(buf, "lay:", 4);
	encode_be64(buf + 4, ino);
	return ROCKSDB_KEY_LAY_SIZE;
}

/* Build key: "chk:" + BE64(ino) + BE64(block_offset) --> 20 bytes */
static inline size_t rocksdb_key_chk(uint8_t *buf, uint64_t ino,
				     uint64_t block_offset)
{
	memcpy(buf, "chk:", 4);
	encode_be64(buf + 4, ino);
	encode_be64(buf + 12, block_offset);
	return ROCKSDB_KEY_CHK_SIZE;
}

/* Build prefix: "chk:" + BE64(ino) --> 12 bytes (for iteration) */
static inline size_t rocksdb_key_chk_prefix(uint8_t *buf, uint64_t ino)
{
	memcpy(buf, "chk:", 4);
	encode_be64(buf + 4, ino);
	return 12;
}

#endif /* _REFFS_ROCKSDB_KEYS_H */
