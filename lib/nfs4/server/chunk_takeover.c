/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" /* IWYU pragma: keep */
#endif

#include <errno.h>
#include <limits.h>
#include <openssl/evp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "reffs/server.h"
#include "reffs/time.h"
#include "nfs4/client.h"
#include "nfs4/chunk_takeover.h"
#include "nfs4/chunk_takeover_transition.h"

#define COSE_HEADER_ALG 1
#define COSE_HEADER_KID 4
#define COSE_ALG_ED25519 8 /* CBOR negative integer -1-n, n=7 below. */
#define COSE_LABEL_MAX 64

struct cbor_cursor {
	const uint8_t *p;
	const uint8_t *end;
};

static int cbor_head(struct cbor_cursor *c, uint8_t *major, uint64_t *arg)
{
	uint8_t first;
	unsigned int ai;
	unsigned int bytes;
	uint64_t value = 0;

	if (c->p >= c->end)
		return -EINVAL;
	first = *c->p++;
	*major = first >> 5;
	ai = first & 0x1f;
	if (ai < 24) {
		*arg = ai;
		return 0;
	}
	if (ai == 31 || ai > 27)
		return -EPROTO;
	bytes = 1U << (ai - 24);
	if ((size_t)(c->end - c->p) < bytes)
		return -EINVAL;
	for (unsigned int i = 0; i < bytes; i++)
		value = (value << 8) | c->p[i];
	c->p += bytes;
	if ((bytes == 1 && value < 24) || (bytes == 2 && value <= UINT8_MAX) ||
	    (bytes == 4 && value <= UINT16_MAX) ||
	    (bytes == 8 && value <= UINT32_MAX))
		return -EPROTO;
	*arg = value;
	return 0;
}

static int cbor_uint(struct cbor_cursor *c, uint64_t *value)
{
	uint8_t major;
	int ret = cbor_head(c, &major, value);

	return ret ? ret : (major == 0 ? 0 : -EPROTO);
}

static int cbor_bytes(struct cbor_cursor *c, const uint8_t **value, size_t *len)
{
	uint8_t major;
	uint64_t length;
	int ret = cbor_head(c, &major, &length);

	if (ret)
		return ret;
	if (major != 2 || length > SIZE_MAX ||
	    (size_t)(c->end - c->p) < (size_t)length)
		return -EPROTO;
	*value = c->p;
	*len = (size_t)length;
	c->p += length;
	return 0;
}

static int cbor_text(struct cbor_cursor *c, const uint8_t **value, size_t *len)
{
	uint8_t major;
	uint64_t length;
	int ret = cbor_head(c, &major, &length);

	if (ret)
		return ret;
	if (major != 3 || length == 0 || length > COSE_LABEL_MAX ||
	    (size_t)(c->end - c->p) < (size_t)length)
		return -EPROTO;
	*value = c->p;
	*len = (size_t)length;
	c->p += length;
	return 0;
}

static bool utf8_valid(const uint8_t *s, size_t len)
{
	size_t i = 0;

	while (i < len) {
		uint8_t first = s[i++];
		unsigned int n;
		uint32_t cp;

		if (first < 0x80)
			continue;
		if (first >= 0xc2 && first <= 0xdf) {
			n = 1;
			cp = first & 0x1f;
		} else if (first >= 0xe0 && first <= 0xef) {
			n = 2;
			cp = first & 0x0f;
		} else if (first >= 0xf0 && first <= 0xf4) {
			n = 3;
			cp = first & 0x07;
		} else {
			return false;
		}
		if (len - i < n)
			return false;
		for (unsigned int j = 0; j < n; j++) {
			if ((s[i] & 0xc0) != 0x80)
				return false;
			cp = (cp << 6) | (s[i++] & 0x3f);
		}
		if ((n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000) ||
		    cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
			return false;
	}
	return true;
}

static int cbor_tagged_uint(struct cbor_cursor *c, uint64_t *value)
{
	uint8_t major;
	uint64_t tag;
	int ret = cbor_head(c, &major, &tag);

	if (ret || major != 6 || tag != 1)
		return -EPROTO;
	return cbor_uint(c, value);
}

static int cbor_skip_simple(struct cbor_cursor *c)
{
	uint8_t major;
	uint64_t arg;
	int ret = cbor_head(c, &major, &arg);

	if (ret)
		return ret;
	if (major == 2 || major == 3) {
		if ((size_t)(c->end - c->p) < arg)
			return -EPROTO;
		c->p += arg;
		return 0;
	}
	return major <= 1 || (major == 7 && arg <= 23) ? 0 : -EPROTO;
}

static int cose_protected_alg(const uint8_t *protected, size_t len)
{
	struct cbor_cursor c = { protected, protected + len };
	uint8_t major;
	uint64_t entries, label, arg, last_label = 0;
	const uint8_t *kid;
	size_t kid_len;
	bool found = false;

	if (cbor_head(&c, &major, &entries) || major != 5 || entries > 2)
		return -EPROTO;
	for (uint64_t i = 0; i < entries; i++) {
		if (cbor_uint(&c, &label) || (i && label <= last_label))
			return -EPROTO;
		last_label = label;
		if (label == COSE_HEADER_ALG) {
			if (cbor_head(&c, &major, &arg) || major != 1 ||
			    arg != COSE_ALG_ED25519 - 1)
				return -EOPNOTSUPP;
			found = true;
		} else if (label == COSE_HEADER_KID) {
			if (cbor_bytes(&c, &kid, &kid_len))
				return -EPROTO;
		} else {
			return -EPROTO;
		}
	}
	return found && c.p == c.end ? 0 : -EPROTO;
}

static int cbor_put_head(uint8_t *buf, size_t cap, size_t *off, uint8_t major,
			 uint64_t arg)
{
	unsigned int bytes;
	uint8_t ai;

	if (arg < 24) {
		if (*off >= cap)
			return -E2BIG;
		buf[(*off)++] = (uint8_t)((major << 5) | arg);
		return 0;
	}
	if (arg <= UINT8_MAX) {
		ai = 24;
		bytes = 1;
	} else if (arg <= UINT16_MAX) {
		ai = 25;
		bytes = 2;
	} else if (arg <= UINT32_MAX) {
		ai = 26;
		bytes = 4;
	} else {
		ai = 27;
		bytes = 8;
	}
	if (*off > cap - 1 - bytes)
		return -E2BIG;
	buf[(*off)++] = (uint8_t)((major << 5) | ai);
	for (unsigned int i = 0; i < bytes; i++)
		buf[(*off)++] = (uint8_t)(arg >> (8 * (bytes - i - 1)));
	return 0;
}

static int cose_sig_structure(const uint8_t *protected, size_t protected_len,
			      const uint8_t *payload, size_t payload_len,
			      uint8_t **out, size_t *out_len)
{
	uint8_t *buf = malloc(8 + 9 + protected_len + payload_len + 16);
	size_t off = 0;

	if (!buf)
		return -ENOMEM;
	if (cbor_put_head(buf, 8 + 9 + protected_len + payload_len + 16, &off,
			  4, 4) ||
	    cbor_put_head(buf, 8 + 9 + protected_len + payload_len + 16, &off,
			  3, 9))
		goto too_big;
	memcpy(buf + off, "Signature1", 9);
	off += 9;
	if (cbor_put_head(buf, 8 + 9 + protected_len + payload_len + 16, &off,
			  2, protected_len))
		goto too_big;
	memcpy(buf + off, protected, protected_len);
	off += protected_len;
	if (cbor_put_head(buf, 8 + 9 + protected_len + payload_len + 16, &off,
			  2, 0) ||
	    cbor_put_head(buf, 8 + 9 + protected_len + payload_len + 16, &off,
			  2, payload_len))
		goto too_big;
	memcpy(buf + off, payload, payload_len);
	off += payload_len;
	*out = buf;
	*out_len = off;
	return 0;
too_big:
	free(buf);
	return -E2BIG;
}

static int payload_parse(const uint8_t *payload, size_t payload_len,
			 uint64_t expected_epoch,
			 const struct chunk_takeover_policy *policy,
			 struct chunk_takeover_claim *claim)
{
	struct cbor_cursor c = { payload, payload + payload_len };
	uint8_t major;
	uint64_t entries, key;
	uint64_t last_key = 0;
	bool seen[7] = { false };
	const uint8_t *value;
	size_t value_len;

	if (cbor_head(&c, &major, &entries) || major != 5 || entries != 6)
		return -EPROTO;
	for (uint64_t i = 0; i < entries; i++) {
		if (cbor_uint(&c, &key) || key == 0 || key > 6 || seen[key] ||
		    (i && key <= last_key))
			return -EPROTO;
		last_key = key;
		seen[key] = true;
		switch (key) {
		case 1:
			if (cbor_text(&c, &value, &value_len) ||
			    !utf8_valid(value, value_len) ||
			    strlen(policy->principal) != value_len ||
			    memcmp(policy->principal, value, value_len))
				return -EACCES;
			break;
		case 2:
			if (cbor_uint(&c, &claim->epoch) ||
			    claim->epoch != expected_epoch)
				return -EACCES;
			break;
		case 3:
			if (cbor_text(&c, &value, &value_len) ||
			    !utf8_valid(value, value_len) ||
			    strlen(policy->scope) != value_len ||
			    memcmp(policy->scope, value, value_len))
				return -EACCES;
			break;
		case 4:
			if (cbor_tagged_uint(&c, &claim->issued_at))
				return -EACCES;
			break;
		case 5:
			if (cbor_tagged_uint(&c, &claim->expires_at))
				return -EACCES;
			break;
		case 6:
			if (cbor_bytes(&c, &value, &value_len) ||
			    value_len != CHUNK_TAKEOVER_TOKEN_ID_LEN)
				return -EACCES;
			memcpy(claim->token_id, value, value_len);
			break;
		}
	}
	if (c.p != c.end || claim->issued_at > claim->expires_at ||
	    claim->issued_at > policy->now_sec + policy->skew_sec ||
	    policy->now_sec >= claim->expires_at)
		return -EACCES;
	return 0;
}

int chunk_takeover_verify_proof(const uint8_t *proof, size_t proof_len,
				uint64_t expected_epoch,
				const struct chunk_takeover_policy *policy,
				struct chunk_takeover_claim *claim)
{
	struct cbor_cursor c = { proof, proof + proof_len };
	const uint8_t *protected, *payload, *signature;
	size_t protected_len, payload_len, signature_len;
	uint8_t major;
	uint64_t items, label;
	uint8_t *sig_structure = NULL;
	size_t sig_structure_len;
	EVP_PKEY *key = NULL;
	EVP_MD_CTX *md = NULL;
	int ret;

	if (!proof || !proof_len || proof_len > CHUNK_TAKEOVER_PROOF_MAX ||
	    !policy || !claim || !policy->public_key || !policy->principal ||
	    !policy->scope || policy->now_sec > UINT64_MAX - policy->skew_sec)
		return -EINVAL;
	if (cbor_head(&c, &major, &items) || major != 4 || items != 4 ||
	    cbor_bytes(&c, &protected, &protected_len))
		return -EPROTO;
	if (cbor_head(&c, &major, &items) || major != 5 || items > 2)
		return -EPROTO;
	for (uint64_t i = 0; i < items; i++) {
		if (cbor_uint(&c, &label))
			return -EPROTO;
		if (label == COSE_HEADER_KID) {
			if (cbor_bytes(&c, &payload, &payload_len))
				return -EPROTO;
		} else if (cbor_skip_simple(&c)) {
			return -EPROTO;
		}
	}
	if (cbor_bytes(&c, &payload, &payload_len) ||
	    cbor_bytes(&c, &signature, &signature_len) || c.p != c.end ||
	    signature_len != CHUNK_TAKEOVER_ED25519_SIGNATURE_LEN)
		return -EPROTO;
	if (cose_protected_alg(protected, protected_len))
		return -EOPNOTSUPP;
	ret = payload_parse(payload, payload_len, expected_epoch, policy,
			    claim);
	if (ret)
		return ret;
	ret = cose_sig_structure(protected, protected_len, payload, payload_len,
				 &sig_structure, &sig_structure_len);
	if (ret)
		return ret;
	key = EVP_PKEY_new_raw_public_key(
		EVP_PKEY_ED25519, NULL, policy->public_key,
		CHUNK_TAKEOVER_ED25519_PUBLIC_KEY_LEN);
	md = key ? EVP_MD_CTX_new() : NULL;
	if (!md || EVP_DigestVerifyInit(md, NULL, NULL, NULL, key) != 1 ||
	    EVP_DigestVerify(md, signature, signature_len, sig_structure,
			     sig_structure_len) != 1)
		ret = -EACCES;
	else
		ret = 0;
	EVP_MD_CTX_free(md);
	EVP_PKEY_free(key);
	free(sig_structure);
	return ret;
}

static nfsstat4 chunk_takeover_proof_error(int error)
{
	/* The XDR envelope is valid, but the signed proof is not admissible. */
	return error == -EPROTO || error == -EINVAL ? NFS4ERR_BADXDR :
						      NFS4ERR_ACCESS;
}

nfsstat4 chunk_takeover_execute(const struct server_state *server,
				const struct nfs4_client *client,
				const char *principal,
				const CHUNK_ESCROW_TAKEOVER4args *args)
{
	struct chunk_takeover_policy policy;
	struct chunk_takeover_claim claim;
	struct chunk_takeover_transition transition;
	uint64_t now_sec, expires_at_ns;
	int ret;

	if (!client || !(client->nc_exchgid_flags & EXCHGID4_FLAG_USE_PNFS_MDS))
		return NFS4ERR_PERM;
	if (!principal || !principal[0])
		return NFS4ERR_ACCESS;
	if (!args)
		return NFS4ERR_BADXDR;
	if (args->ceta_proof_profile != PROOF_PROFILE_HA_AUTHORITY_ED25519)
		return NFS4ERR_NOTSUPP;
	if (!server || !server->ss_chunk_takeover_configured)
		return NFS4ERR_NOTSUPP;
	if (strcmp(principal, server->ss_chunk_takeover_principal) != 0)
		return NFS4ERR_ACCESS;
	if (args->ceta_new_epoch < args->ceta_expected_prior_epoch)
		return NFS4ERR_INVAL;

	now_sec = reffs_now_ns() / 1000000000ULL;
	policy = (struct chunk_takeover_policy){
		.public_key = server->ss_chunk_takeover_public_key,
		.principal = principal,
		.scope = server->ss_chunk_takeover_scope,
		.now_sec = now_sec,
		.skew_sec = server->ss_chunk_takeover_skew_sec,
	};
	ret = chunk_takeover_verify_proof(
		(const uint8_t *)args->ceta_proof_data.ceta_proof_data_val,
		args->ceta_proof_data.ceta_proof_data_len, args->ceta_new_epoch,
		&policy, &claim);
	if (ret)
		return chunk_takeover_proof_error(ret);
	if (claim.expires_at > UINT64_MAX / 1000000000ULL)
		return NFS4ERR_ACCESS;
	expires_at_ns = claim.expires_at * 1000000000ULL;
	transition = (struct chunk_takeover_transition){
		.profile = args->ceta_proof_profile,
		.principal = principal,
		.token_id = claim.token_id,
		.token_expires_at = claim.expires_at,
		.expected_prior_epoch = args->ceta_expected_prior_epoch,
		.new_epoch = args->ceta_new_epoch,
		.new_expires_at_ns = expires_at_ns,
		.issuer_clientid = client->nc_client.c_id,
	};
	ret = chunk_takeover_transition_apply(server->ss_state_dir, &transition,
					      now_sec);
	if (!ret)
		return NFS4_OK;
	if (ret == -ESTALE)
		return NFS4ERR_STALE_MDS_EPOCH;
	if (ret == -EINVAL)
		return NFS4ERR_INVAL;
	return NFS4ERR_SERVERFAULT;
}
