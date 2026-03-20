/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * reffs server configuration.
 *
 * All settings have sane defaults (see reffs_config_defaults()).  An absent
 * or empty config file is valid and yields a fully operational standalone
 * server on port 2049 with a RAM backend.
 */

#ifndef _REFFS_SETTINGS_H
#define _REFFS_SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

#define REFFS_CONFIG_MAX_PATH 4096
#define REFFS_CONFIG_MAX_BIND 64
#define REFFS_CONFIG_MAX_EXPORTS 64
#define REFFS_CONFIG_MAX_FLAVORS 8

/*
 * Server role — maps to EXCHGID4_FLAG_USE_* bits advertised in EXCHANGE_ID.
 *
 *   standalone  EXCHGID4_FLAG_USE_NON_PNFS
 *   mds         EXCHGID4_FLAG_USE_PNFS_MDS
 *   ds          EXCHGID4_FLAG_USE_PNFS_DS
 *   combined    EXCHGID4_FLAG_USE_PNFS_MDS | EXCHGID4_FLAG_USE_PNFS_DS
 *   ds_erasure  EXCHGID4_FLAG_USE_ERASURE_DS  (RFC 9754 / CHUNK ops)
 */
enum reffs_role {
	REFFS_ROLE_STANDALONE = 0,
	REFFS_ROLE_MDS,
	REFFS_ROLE_DS,
	REFFS_ROLE_COMBINED,
	REFFS_ROLE_DS_ERASURE,
};

enum reffs_log_level {
	REFFS_LOG_TRACE = 0,
	REFFS_LOG_DEBUG,
	REFFS_LOG_INFO,
	REFFS_LOG_WARN,
	REFFS_LOG_ERROR,
};

enum reffs_backend_type {
	REFFS_BACKEND_RAM = 0,
	REFFS_BACKEND_POSIX,
	REFFS_BACKEND_ROCKSDB,
};

/*
 * RPC auth flavor values (RFC 5531 §7.2, RFC 2203 §5).
 * "sys" is AUTH_SYS (formerly AUTH_UNIX); krb5 variants are RPCSEC_GSS.
 */
enum reffs_auth_flavor {
	REFFS_AUTH_SYS = 1,
	REFFS_AUTH_KRB5 = 390003,
	REFFS_AUTH_KRB5I = 390004,
	REFFS_AUTH_KRB5P = 390005,
};

struct reffs_export_config {
	char path[REFFS_CONFIG_MAX_PATH];
	/*
	 * clients: "*" means all IPv4 and IPv6 connections.
	 * Otherwise an IP address, CIDR block, or hostname.
	 */
	char clients[REFFS_CONFIG_MAX_BIND];
	bool read_only;
	bool root_squash;
	enum reffs_auth_flavor flavors[REFFS_CONFIG_MAX_FLAVORS];
	unsigned int nflavors;
};

struct reffs_config {
	/* [server] */
	uint16_t port;
	char bind[REFFS_CONFIG_MAX_BIND]; /* "*" = all IPv4 + IPv6 */
	enum reffs_role role;
	int minor_versions[2]; /* NFSv4 minor versions to advertise */
	unsigned int n_minor_versions;
	unsigned int grace_period; /* seconds */
	bool tls;
	char tls_cert[REFFS_CONFIG_MAX_PATH];
	char tls_key[REFFS_CONFIG_MAX_PATH];
	unsigned int workers;
	unsigned int max_session_slots;
	char log_file[REFFS_CONFIG_MAX_PATH]; /* "" = stderr */
	enum reffs_log_level log_level;

	/* [backend] */
	enum reffs_backend_type backend_type;
	char backend_path[REFFS_CONFIG_MAX_PATH];
	char state_file[REFFS_CONFIG_MAX_PATH];

	/* [iouring] */
	unsigned int network_sq_size;
	unsigned int network_cq_size;
	unsigned int backend_sq_size;
	unsigned int backend_cq_size;

	/* [[export]] */
	struct reffs_export_config exports[REFFS_CONFIG_MAX_EXPORTS];
	unsigned int nexports;
};

/*
 * Fill *cfg with compile-time defaults.  Call this before reffs_config_load()
 * so that any keys absent from the file retain their default values.
 */
void reffs_config_defaults(struct reffs_config *cfg);

/*
 * Parse the TOML config file at path into *cfg (which must already hold
 * defaults from reffs_config_defaults()).  Only keys present in the file
 * override the defaults.
 *
 * Returns 0 on success, -1 on parse/IO error (error logged via LOG()).
 */
int reffs_config_load(struct reffs_config *cfg, const char *path);

/* Human-readable role name, e.g. "ds_erasure". */
const char *reffs_role_str(enum reffs_role role);

/* EXCHGID4_FLAG_USE_* bitmask for the given role. */
uint32_t reffs_role_exchgid_flags(enum reffs_role role);

#endif /* _REFFS_SETTINGS_H */
