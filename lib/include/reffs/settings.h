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
#define REFFS_CONFIG_MAX_DATA_SERVERS 64
#define REFFS_CONFIG_MAX_DSTORES 16
#define REFFS_CONFIG_MAX_HOST 256
#define REFFS_CONFIG_MAX_PROXY_MDS 8
#define REFFS_CONFIG_MAX_ALLOWED_PS 8
#define REFFS_CONFIG_MAX_PRINCIPAL 256
/*
 * Room for a SHA-256 fingerprint formatted as colon-separated hex
 * (32 bytes * 2 + 31 colons = 95 chars, +NUL).  128 leaves headroom
 * for SHA-384/512 if a future slice extends the hash agility.
 */
#define REFFS_CONFIG_MAX_TLS_FINGERPRINT 128
#define REFFS_FENCE_UID_MIN_DEFAULT 1024
#define REFFS_FENCE_UID_MAX_DEFAULT 2048
#define REFFS_LAYOUT_WIDTH_DEFAULT 6 /* RS(4,2): 4 data + 2 parity */

/* Maximum number of IO worker threads (also in io.h MAX_WORKER_THREADS). */
#define REFFS_MAX_WORKER_THREADS 64

/*
 * Server role -- maps to EXCHGID4_FLAG_USE_* bits advertised in EXCHANGE_ID.
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
 * RPC auth flavor values (RFC 5531 S7.2, RFC 2203 S5).
 * "sys" is AUTH_SYS (formerly AUTH_UNIX); krb5 variants are RPCSEC_GSS.
 *
 * REFFS_AUTH_TLS is a pseudo-flavor: AUTH_SYS over a TLS-protected
 * transport.  Not an RPC wire value -- only used in export config to
 * require TLS.
 */
enum reffs_auth_flavor {
	REFFS_AUTH_SYS = 1,
	REFFS_AUTH_KRB5 = 390003,
	REFFS_AUTH_KRB5I = 390004,
	REFFS_AUTH_KRB5P = 390005,
	REFFS_AUTH_TLS = 0x40000001, /* pseudo-flavor: AUTH_SYS + TLS */
};

/*
 * Per-client rule in [[export.clients]].
 * SB_CLIENT_MATCH_MAX / SB_MAX_CLIENT_RULES are also used by
 * struct sb_client_rule in super_block.h (which includes this header).
 */
#define SB_CLIENT_MATCH_MAX 128
#define SB_MAX_CLIENT_RULES 32

struct reffs_client_rule_config {
	char match[SB_CLIENT_MATCH_MAX];
	bool rw; /* false = read-only */
	bool root_squash;
	bool all_squash;
	enum reffs_auth_flavor flavors[REFFS_CONFIG_MAX_FLAVORS];
	unsigned int nflavors;
};

struct reffs_export_config {
	char path[REFFS_CONFIG_MAX_PATH];
	struct reffs_client_rule_config rules[SB_MAX_CLIENT_RULES];
	unsigned int nrules;
	/* pNFS layout types to enable on this export (SB_LAYOUT_* bitmask). */
	uint32_t layout_types;
	/* Dstore IDs to bind to this export (0 = use global pool). */
	uint32_t dstores[REFFS_CONFIG_MAX_DSTORES];
	unsigned int ndstores;
};

/*
 * [[data_server]] -- MDS-only configuration.
 *
 * Each entry names a data server, its export path, and the
 * protocol the MDS uses for the control plane and InBand I/O.
 */
enum reffs_ds_protocol {
	REFFS_DS_PROTO_NFSV3 = 0, /* default -- flex files */
	REFFS_DS_PROTO_NFSV4 = 1, /* file layouts */
};

struct reffs_data_server_config {
	uint32_t id; /* unique dstore ID */
	char address[REFFS_CONFIG_MAX_HOST]; /* IPv4 or IPv6 address */
	uint16_t port; /* explicit NFS port; 0 = portmap (default) */
	char path[REFFS_CONFIG_MAX_PATH]; /* export path on the DS */
	enum reffs_ds_protocol protocol; /* default: nfsv3 */
	/*
	 * Trust-stateid slice 1.5: opt-in tight-coupling override
	 * for NFSv3 dstores.  The NFSv3 vtable defaults
	 * ds_tight_coupled = false because a generic NFSv3 server
	 * cannot enforce a trust table.  When the operator KNOWS
	 * the DS is reffsd (e.g., the bench docker stack), setting
	 * `tight_coupling = true` forces the dstore-alloc path to
	 * advertise tight-coupling -- ec_demo then uses the real
	 * layout stateid in CHUNK_WRITE / READ, and the DS-side
	 * trust table check at chunk.c:136 actually fires.
	 *
	 * Default: false (preserves the existing behaviour).
	 */
	bool tight_coupling;
};

/*
 * [[proxy_mds]] -- proxy-server listener plus upstream MDS binding.
 *
 * Each entry defines a second NFS listener whose superblocks live
 * in a separate, listener-scoped namespace.  The native listener on
 * cfg->port is always listener_id 0; entries here are 1..N.  The
 * (sb_id, listener_id) pair is the identity the NFS compound
 * dispatch matches on -- an FH minted on one listener that arrives
 * on another misses the scoped lookup and the client sees
 * NFS4ERR_STALE.
 *
 * The `id` field is the per-listener identifier (1..N, unique, must
 * not be 0 which is reserved for the native listener).
 *
 * `address`, `mds_port`, `mds_probe` describe the upstream MDS the
 * proxy-server forwards to.  They are parsed here but not yet
 * consulted at runtime -- the MDS-client session opens in a later
 * Phase 2 slice.  `address == ""` marks the upstream as unconfigured;
 * reffsd currently tolerates that and still opens the listener.
 */
/*
 * tls_mode controls how the PS-MDS session brings TLS up
 * (RFC 9289 / data-mover sec-security).  Default OFF preserves
 * the pre-TLS plain-TCP behaviour for tests and dev configs that
 * have no certs to wire.  STARTTLS issues an AUTH_TLS NULL probe
 * on a fresh TCP conn before SSL_connect.  DIRECT runs SSL_connect
 * immediately -- useful when fronting the MDS with a TLS-only
 * proxy that would reject the cleartext STARTTLS preamble.
 */
enum reffs_proxy_tls_mode {
	REFFS_PROXY_TLS_OFF = 0,
	REFFS_PROXY_TLS_STARTTLS = 1,
	REFFS_PROXY_TLS_DIRECT = 2,
};

struct reffs_proxy_mds_config {
	uint32_t id; /* listener id: 1..N (0 reserved for native) */
	uint16_t port; /* bind port, e.g. 4098 */
	char bind[REFFS_CONFIG_MAX_BIND]; /* "*" = all IPv4 + IPv6 */
	char address[REFFS_CONFIG_MAX_HOST]; /* upstream MDS IPv4/IPv6 */
	uint16_t mds_port; /* upstream MDS NFS port, default 2049 */
	uint16_t mds_probe; /* upstream MDS probe port, default 20490 */

	/*
	 * Mutually-authenticated TLS for the PS-MDS session.  When
	 * any of tls_cert / tls_key is set, the session opens via
	 * tls_starttls (or tls_direct_connect) and PROXY_REGISTRATION
	 * carries the client cert SHA-256 fingerprint into the MDS
	 * allowlist check (see lib/nfs4/server/compound.c +
	 * lib/nfs4/server/proxy_registration.c).  All three paths are
	 * required for full mTLS: client cert + key (so the PS proves
	 * its identity to the MDS) and a CA bundle (so the PS verifies
	 * the MDS server cert).  An empty path string skips that piece
	 * -- e.g., tls_ca = "" with no_verify-equivalent semantics for
	 * dev / smoke topologies that use a self-signed MDS cert.
	 */
	char tls_cert[REFFS_CONFIG_MAX_PATH];
	char tls_key[REFFS_CONFIG_MAX_PATH];
	char tls_ca[REFFS_CONFIG_MAX_PATH];
	enum reffs_proxy_tls_mode tls_mode;
	/*
	 * Explicit opt-out of MDS server-cert verification.  Required
	 * when tls_cert/tls_key are set but tls_ca is empty (the
	 * smoke / self-signed-MDS topology used by slice plan-1-tls.c).
	 * Default false: cert-without-CA without this flag is rejected
	 * at parse time so a missing tls_ca line in a production config
	 * cannot silently downgrade to "TLS without identity check".
	 */
	bool tls_insecure_no_verify;
};

/*
 * MDS-only.  Each [[allowed_ps]] block names a single Proxy Server
 * identity permitted to send PROXY_REGISTRATION.  An entry sets
 * EXACTLY one of `principal` (RPCSEC_GSS path, slice 6b-i) or
 * `tls_cert_fingerprint` (mTLS path, slice 6b-iv) -- empty string
 * means "not set".  The default-deny model means an empty allowlist
 * rejects every registration -- see proxy-server-phase6b.md.
 */
struct reffs_allowed_ps_config {
	char principal[REFFS_CONFIG_MAX_PRINCIPAL];
	char tls_cert_fingerprint[REFFS_CONFIG_MAX_TLS_FINGERPRINT];
};

struct reffs_config {
	/* [server] */
	uint16_t port;
	uint16_t probe_port; /* internal admin/probe listener, default PROBE_PORT */
	char bind[REFFS_CONFIG_MAX_BIND]; /* "*" = all IPv4 + IPv6 */
	enum reffs_role role;
	int minor_versions[2]; /* NFSv4 minor versions to advertise */
	unsigned int n_minor_versions;
	unsigned int grace_period; /* seconds */
	bool tls;
	char tls_cert[REFFS_CONFIG_MAX_PATH];
	char tls_key[REFFS_CONFIG_MAX_PATH];
	/*
	 * CA bundle for verifying client certs.  When set, the
	 * server-side TLS context goes from SSL_VERIFY_NONE to
	 * SSL_VERIFY_PEER | FAIL_IF_NO_PEER_CERT so the per-connection
	 * peer-cert fingerprint becomes available for the MDS
	 * PROXY_REGISTRATION allowlist (slice plan-1-tls.c, #139).
	 * Empty string preserves the historical TLS-server-only
	 * behaviour for clients that don't present a cert.
	 */
	char tls_ca[REFFS_CONFIG_MAX_PATH];
	/*
	 * Register reffsd's NFS programs (NFSv4, NFSv3, MOUNT, NLM,
	 * NSM) with the local rpcbind/portmap daemon at startup.
	 * Required for NFSv3 clients that auto-discover MOUNT/NLM
	 * ports via rpcbind.  Set to false for NFSv4-only deployments
	 * and for soak/CI runs where the ~22 rpcbind round-trips at
	 * startup cause readiness-race flakes.  Default true preserves
	 * upgrade compatibility -- see .claude/design/no-rpcbind.md.
	 */
	bool register_with_rpcbind;
	unsigned int workers;
	unsigned int max_session_slots;
	char log_file[REFFS_CONFIG_MAX_PATH]; /* "" = stderr */
	enum reffs_log_level log_level;
	char nfs4_domain[REFFS_CONFIG_MAX_HOST]; /* NFSv4 owner string domain */
	char trace_file[REFFS_CONFIG_MAX_PATH]; /* "" = default */
	uint32_t trace_categories; /* bitmask of REFFS_TRACE_CAT_* */

	/* [backend] */
	enum reffs_backend_type backend_type;
	char backend_path[REFFS_CONFIG_MAX_PATH];
	char state_file[REFFS_CONFIG_MAX_PATH];

	/* [cache] */
	unsigned int inode_cache_max; /* inode LRU eviction threshold */
	unsigned int dirent_cache_max; /* dirent LRU eviction threshold */

	/* [iouring] */
	unsigned int network_sq_size;
	unsigned int network_cq_size;
	unsigned int backend_sq_size;
	unsigned int backend_cq_size;

	/* [[export]] */
	struct reffs_export_config exports[REFFS_CONFIG_MAX_EXPORTS];
	unsigned int nexports;

	/* DS backend (combined role only) */
	char ds_backend_path[REFFS_CONFIG_MAX_PATH];

	/* Fencing -- synthetic uid/gid range for data file fencing */
	uint32_t fence_uid_min;
	uint32_t fence_uid_max;

	/*
	 * Layout width -- number of data files per layout.
	 * When fewer dstores are available, files are round-robin'd
	 * across the available set.  Default: 6 (RS 4+2).
	 */
	unsigned int layout_width;

	/* [[data_server]] -- only used when role = mds or combined */
	struct reffs_data_server_config
		data_servers[REFFS_CONFIG_MAX_DATA_SERVERS];
	unsigned int ndata_servers;

	/* [[proxy_mds]] -- additional listeners for proxy-server namespaces */
	struct reffs_proxy_mds_config proxy_mds[REFFS_CONFIG_MAX_PROXY_MDS];
	unsigned int nproxy_mds;

	/* [[allowed_ps]] -- MDS-side allowlist for PROXY_REGISTRATION */
	struct reffs_allowed_ps_config allowed_ps[REFFS_CONFIG_MAX_ALLOWED_PS];
	unsigned int nallowed_ps;
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
