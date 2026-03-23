/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "toml.h"

#include "nfsv42_xdr.h"
#include "reffs/settings.h"
#include "reffs/log.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static enum reffs_role parse_role(const char *s)
{
	if (!strcasecmp(s, "standalone"))
		return REFFS_ROLE_STANDALONE;
	if (!strcasecmp(s, "mds"))
		return REFFS_ROLE_MDS;
	if (!strcasecmp(s, "ds"))
		return REFFS_ROLE_DS;
	if (!strcasecmp(s, "combined"))
		return REFFS_ROLE_COMBINED;
	if (!strcasecmp(s, "ds_erasure"))
		return REFFS_ROLE_DS_ERASURE;

	LOG("config: unknown role '%s', defaulting to standalone", s);
	return REFFS_ROLE_STANDALONE;
}

static enum reffs_log_level parse_log_level(const char *s)
{
	if (!strcasecmp(s, "trace"))
		return REFFS_LOG_TRACE;
	if (!strcasecmp(s, "debug"))
		return REFFS_LOG_DEBUG;
	if (!strcasecmp(s, "info"))
		return REFFS_LOG_INFO;
	if (!strcasecmp(s, "warn"))
		return REFFS_LOG_WARN;
	if (!strcasecmp(s, "error"))
		return REFFS_LOG_ERROR;

	LOG("config: unknown log_level '%s', defaulting to info", s);
	return REFFS_LOG_INFO;
}

static enum reffs_backend_type parse_backend_type(const char *s)
{
	if (!strcasecmp(s, "ram"))
		return REFFS_BACKEND_RAM;
	if (!strcasecmp(s, "posix"))
		return REFFS_BACKEND_POSIX;
	if (!strcasecmp(s, "rocksdb"))
		return REFFS_BACKEND_ROCKSDB;

	LOG("config: unknown backend type '%s', defaulting to ram", s);
	return REFFS_BACKEND_RAM;
}

static enum reffs_auth_flavor parse_flavor(const char *s)
{
	if (!strcasecmp(s, "sys"))
		return REFFS_AUTH_SYS;
	if (!strcasecmp(s, "krb5"))
		return REFFS_AUTH_KRB5;
	if (!strcasecmp(s, "krb5i"))
		return REFFS_AUTH_KRB5I;
	if (!strcasecmp(s, "krb5p"))
		return REFFS_AUTH_KRB5P;

	LOG("config: unknown auth flavor '%s', ignoring", s);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void reffs_config_defaults(struct reffs_config *cfg)
{
	long ncpus;

	memset(cfg, 0, sizeof(*cfg));

	/* [server] */
	cfg->port = 2049;
	strncpy(cfg->bind, "*", sizeof(cfg->bind) - 1);
	cfg->role = REFFS_ROLE_STANDALONE;
	cfg->minor_versions[0] = 1;
	cfg->minor_versions[1] = 2;
	cfg->n_minor_versions = 2;
	cfg->grace_period = 45;
	cfg->tls = false;
	ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	cfg->workers = (ncpus > 0 && ncpus <= 64) ? (unsigned int)ncpus : 8;
	cfg->max_session_slots = 64;
	/* log_file = "" → stderr */
	cfg->log_level = REFFS_LOG_INFO;
	cfg->fence_uid_min = REFFS_FENCE_UID_MIN_DEFAULT;
	cfg->fence_uid_max = REFFS_FENCE_UID_MAX_DEFAULT;

	/* [backend] */
	cfg->backend_type = REFFS_BACKEND_RAM;
	strncpy(cfg->backend_path, "/var/lib/reffs/data",
		sizeof(cfg->backend_path) - 1);
	strncpy(cfg->state_file, "/var/lib/reffs/reffs.state",
		sizeof(cfg->state_file) - 1);

	/* [iouring] */
	cfg->network_sq_size = 2048;
	cfg->network_cq_size = 8192;
	cfg->backend_sq_size = 512;
	cfg->backend_cq_size = 2048;

	/* [[export]] — one permissive default */
	cfg->nexports = 1;
	strncpy(cfg->exports[0].path, "/", sizeof(cfg->exports[0].path) - 1);
	strncpy(cfg->exports[0].clients, "*",
		sizeof(cfg->exports[0].clients) - 1);
	cfg->exports[0].read_only = false;
	cfg->exports[0].root_squash = false;
	cfg->exports[0].flavors[0] = REFFS_AUTH_SYS;
	cfg->exports[0].nflavors = 1;
}

/* Parse [server] table. */
static void parse_server(struct reffs_config *cfg, toml_table_t *srv)
{
	toml_datum_t d;
	toml_array_t *arr;

	d = toml_int_in(srv, "port");
	if (d.ok)
		cfg->port = (uint16_t)d.u.i;

	d = toml_string_in(srv, "bind");
	if (d.ok) {
		strncpy(cfg->bind, d.u.s, sizeof(cfg->bind) - 1);
		free(d.u.s);
	}

	d = toml_string_in(srv, "role");
	if (d.ok) {
		cfg->role = parse_role(d.u.s);
		free(d.u.s);
	}

	arr = toml_array_in(srv, "minor_versions");
	if (arr) {
		int n = toml_array_nelem(arr);
		cfg->n_minor_versions = 0;
		for (int i = 0; i < n && i < 2; i++) {
			d = toml_int_at(arr, i);
			if (d.ok)
				cfg->minor_versions[cfg->n_minor_versions++] =
					(int)d.u.i;
		}
	}

	d = toml_int_in(srv, "grace_period");
	if (d.ok)
		cfg->grace_period = (unsigned int)d.u.i;

	d = toml_bool_in(srv, "tls");
	if (d.ok)
		cfg->tls = (bool)d.u.b;

	d = toml_string_in(srv, "tls_cert");
	if (d.ok) {
		strncpy(cfg->tls_cert, d.u.s, sizeof(cfg->tls_cert) - 1);
		free(d.u.s);
	}

	d = toml_string_in(srv, "tls_key");
	if (d.ok) {
		strncpy(cfg->tls_key, d.u.s, sizeof(cfg->tls_key) - 1);
		free(d.u.s);
	}

	d = toml_int_in(srv, "workers");
	if (d.ok && d.u.i > 0)
		cfg->workers = (unsigned int)d.u.i;

	d = toml_int_in(srv, "max_session_slots");
	if (d.ok && d.u.i > 0)
		cfg->max_session_slots = (unsigned int)d.u.i;

	d = toml_string_in(srv, "log_file");
	if (d.ok) {
		strncpy(cfg->log_file, d.u.s, sizeof(cfg->log_file) - 1);
		free(d.u.s);
	}

	d = toml_string_in(srv, "log_level");
	if (d.ok) {
		cfg->log_level = parse_log_level(d.u.s);
		free(d.u.s);
	}

	d = toml_int_in(srv, "fence_uid_min");
	if (d.ok && d.u.i >= 0)
		cfg->fence_uid_min = (uint32_t)d.u.i;

	d = toml_int_in(srv, "fence_uid_max");
	if (d.ok && d.u.i >= 0)
		cfg->fence_uid_max = (uint32_t)d.u.i;
}

/* Parse [backend] table. */
static void parse_backend(struct reffs_config *cfg, toml_table_t *be)
{
	toml_datum_t d;

	d = toml_string_in(be, "type");
	if (d.ok) {
		cfg->backend_type = parse_backend_type(d.u.s);
		free(d.u.s);
	}

	d = toml_string_in(be, "path");
	if (d.ok) {
		strncpy(cfg->backend_path, d.u.s,
			sizeof(cfg->backend_path) - 1);
		free(d.u.s);
	}

	d = toml_string_in(be, "state_file");
	if (d.ok) {
		strncpy(cfg->state_file, d.u.s, sizeof(cfg->state_file) - 1);
		free(d.u.s);
	}

	d = toml_string_in(be, "ds_path");
	if (d.ok) {
		strncpy(cfg->ds_backend_path, d.u.s,
			sizeof(cfg->ds_backend_path) - 1);
		free(d.u.s);
	}
}

/* Parse [iouring] table. */
static void parse_iouring(struct reffs_config *cfg, toml_table_t *io)
{
	toml_datum_t d;

	d = toml_int_in(io, "network_sq_size");
	if (d.ok && d.u.i > 0)
		cfg->network_sq_size = (unsigned int)d.u.i;

	d = toml_int_in(io, "network_cq_size");
	if (d.ok && d.u.i > 0)
		cfg->network_cq_size = (unsigned int)d.u.i;

	d = toml_int_in(io, "backend_sq_size");
	if (d.ok && d.u.i > 0)
		cfg->backend_sq_size = (unsigned int)d.u.i;

	d = toml_int_in(io, "backend_cq_size");
	if (d.ok && d.u.i > 0)
		cfg->backend_cq_size = (unsigned int)d.u.i;
}

/* Parse one [[export]] table entry. */
static void parse_one_export(struct reffs_export_config *exp, toml_table_t *tbl)
{
	toml_datum_t d;
	toml_array_t *arr;

	d = toml_string_in(tbl, "path");
	if (d.ok) {
		strncpy(exp->path, d.u.s, sizeof(exp->path) - 1);
		free(d.u.s);
	}

	d = toml_string_in(tbl, "clients");
	if (d.ok) {
		strncpy(exp->clients, d.u.s, sizeof(exp->clients) - 1);
		free(d.u.s);
	}

	d = toml_string_in(tbl, "access");
	if (d.ok) {
		exp->read_only = !strcasecmp(d.u.s, "ro");
		free(d.u.s);
	}

	d = toml_bool_in(tbl, "root_squash");
	if (d.ok)
		exp->root_squash = (bool)d.u.b;

	arr = toml_array_in(tbl, "flavors");
	if (arr) {
		int n = toml_array_nelem(arr);
		exp->nflavors = 0;
		for (int i = 0; i < n && i < REFFS_CONFIG_MAX_FLAVORS; i++) {
			d = toml_string_at(arr, i);
			if (d.ok) {
				enum reffs_auth_flavor f = parse_flavor(d.u.s);
				free(d.u.s);
				if (f)
					exp->flavors[exp->nflavors++] = f;
			}
		}
	}
}

int reffs_config_load(struct reffs_config *cfg, const char *path)
{
	FILE *fp;
	char errbuf[256];
	toml_table_t *root;
	toml_table_t *tbl;
	toml_array_t *arr;
	int nexports;

	fp = fopen(path, "r");
	if (!fp) {
		LOG("config: cannot open '%s': %m", path);
		return -1;
	}

	root = toml_parse_file(fp, errbuf, sizeof(errbuf));
	fclose(fp);

	if (!root) {
		LOG("config: parse error in '%s': %s", path, errbuf);
		return -1;
	}

	tbl = toml_table_in(root, "server");
	if (tbl)
		parse_server(cfg, tbl);

	tbl = toml_table_in(root, "backend");
	if (tbl)
		parse_backend(cfg, tbl);

	tbl = toml_table_in(root, "iouring");
	if (tbl)
		parse_iouring(cfg, tbl);

	arr = toml_array_in(root, "export");
	if (arr) {
		nexports = toml_array_nelem(arr);
		if (nexports > REFFS_CONFIG_MAX_EXPORTS)
			nexports = REFFS_CONFIG_MAX_EXPORTS;
		cfg->nexports = (unsigned int)nexports;
		for (int i = 0; i < nexports; i++)
			parse_one_export(&cfg->exports[i],
					 toml_table_at(arr, i));
	}

	arr = toml_array_in(root, "data_server");
	if (arr) {
		int nds = toml_array_nelem(arr);

		if (nds > REFFS_CONFIG_MAX_DATA_SERVERS)
			nds = REFFS_CONFIG_MAX_DATA_SERVERS;
		cfg->ndata_servers = (unsigned int)nds;
		for (int i = 0; i < nds; i++) {
			toml_table_t *ds_tbl = toml_table_at(arr, i);
			struct reffs_data_server_config *dsc =
				&cfg->data_servers[i];
			toml_datum_t d;

			d = toml_int_in(ds_tbl, "id");
			if (d.ok)
				dsc->id = (uint32_t)d.u.i;

			d = toml_string_in(ds_tbl, "address");
			if (d.ok) {
				strncpy(dsc->address, d.u.s,
					sizeof(dsc->address) - 1);
				free(d.u.s);
			}

			d = toml_string_in(ds_tbl, "path");
			if (d.ok) {
				strncpy(dsc->path, d.u.s,
					sizeof(dsc->path) - 1);
				free(d.u.s);
			}
		}
	}

	toml_free(root);
	return 0;
}

const char *reffs_role_str(enum reffs_role role)
{
	switch (role) {
	case REFFS_ROLE_STANDALONE:
		return "standalone";
	case REFFS_ROLE_MDS:
		return "mds";
	case REFFS_ROLE_DS:
		return "ds";
	case REFFS_ROLE_COMBINED:
		return "combined";
	case REFFS_ROLE_DS_ERASURE:
		return "ds_erasure";
	}
	return "standalone";
}

uint32_t reffs_role_exchgid_flags(enum reffs_role role)
{
	switch (role) {
	case REFFS_ROLE_STANDALONE:
		return EXCHGID4_FLAG_USE_NON_PNFS;
	case REFFS_ROLE_MDS:
		return EXCHGID4_FLAG_USE_PNFS_MDS;
	case REFFS_ROLE_DS:
		return EXCHGID4_FLAG_USE_PNFS_DS;
	case REFFS_ROLE_COMBINED:
		return EXCHGID4_FLAG_USE_PNFS_MDS | EXCHGID4_FLAG_USE_PNFS_DS;
	case REFFS_ROLE_DS_ERASURE:
		return EXCHGID4_FLAG_USE_ERASURE_DS;
	}
	return EXCHGID4_FLAG_USE_NON_PNFS;
}
