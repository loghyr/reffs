/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * config_test.c -- unit tests for lib/config/config.c
 *
 * Tests:
 *   defaults  -- reffs_config_defaults() fills expected values
 *   load      -- reffs_config_load() parses TOML and overrides defaults
 *   helpers   -- reffs_role_str() and reffs_role_exchgid_flags()
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <check.h>

#include "nfsv42_xdr.h"
#include "reffs/settings.h"
#include "libreffs_test.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/*
 * Write a string to a temp file and return the path (caller must
 * unlink + free).  Returns NULL on error.
 */
static char *write_toml(const char *content)
{
	char *path = strdup("/tmp/reffs_config_test_XXXXXX");
	int fd = mkstemp(path);
	if (fd < 0) {
		free(path);
		return NULL;
	}
	if (write(fd, content, strlen(content)) < 0) {
		close(fd);
		unlink(path);
		free(path);
		return NULL;
	}
	close(fd);
	return path;
}

/* ------------------------------------------------------------------ */
/* defaults                                                            */
/* ------------------------------------------------------------------ */

START_TEST(test_defaults_port)
{
	struct reffs_config cfg;
	reffs_config_defaults(&cfg);
	ck_assert_int_eq(cfg.port, 2049);
}
END_TEST

START_TEST(test_defaults_role)
{
	struct reffs_config cfg;
	reffs_config_defaults(&cfg);
	ck_assert_int_eq(cfg.role, REFFS_ROLE_STANDALONE);
}
END_TEST

/*
 * probe_port defaults to the XDR-pinned PROBE_PORT (20490).  Config
 * surface lets operators override it so two reffsd on the same host
 * can pick non-conflicting admin ports.
 */
START_TEST(test_defaults_probe_port)
{
	struct reffs_config cfg;
	reffs_config_defaults(&cfg);
	ck_assert_int_eq(cfg.probe_port, 20490);
}
END_TEST

/*
 * [server] probe_port overrides the default; covers the same TOML
 * path as test_load_server_port but for the probe listener.
 */
START_TEST(test_load_server_probe_port)
{
	struct reffs_config cfg;

	reffs_config_defaults(&cfg);

	char *path = write_toml("[server]\nprobe_port = 30491\n");

	ck_assert_ptr_nonnull(path);

	ck_assert_int_eq(reffs_config_load(&cfg, path), 0);
	ck_assert_int_eq(cfg.probe_port, 30491);

	unlink(path);
	free(path);
}
END_TEST

START_TEST(test_defaults_minor_versions)
{
	struct reffs_config cfg;
	reffs_config_defaults(&cfg);
	ck_assert_uint_eq(cfg.n_minor_versions, 2);
	ck_assert_int_eq(cfg.minor_versions[0], 1);
	ck_assert_int_eq(cfg.minor_versions[1], 2);
}
END_TEST

START_TEST(test_defaults_grace_period)
{
	struct reffs_config cfg;
	reffs_config_defaults(&cfg);
	ck_assert_uint_eq(cfg.grace_period, 45);
}
END_TEST

START_TEST(test_defaults_tls_off)
{
	struct reffs_config cfg;
	reffs_config_defaults(&cfg);
	ck_assert(!cfg.tls);
}
END_TEST

START_TEST(test_defaults_backend_ram)
{
	struct reffs_config cfg;
	reffs_config_defaults(&cfg);
	ck_assert_int_eq(cfg.backend_type, REFFS_BACKEND_RAM);
}
END_TEST

START_TEST(test_defaults_log_level_info)
{
	struct reffs_config cfg;
	reffs_config_defaults(&cfg);
	ck_assert_int_eq(cfg.log_level, REFFS_LOG_INFO);
}
END_TEST

START_TEST(test_defaults_one_export)
{
	struct reffs_config cfg;
	reffs_config_defaults(&cfg);
	ck_assert_uint_eq(cfg.nexports, 1);
	ck_assert_str_eq(cfg.exports[0].path, "/");
	/* Default: one "*" catch-all rule, rw, sys */
	ck_assert_uint_eq(cfg.exports[0].nrules, 1);
	ck_assert_str_eq(cfg.exports[0].rules[0].match, "*");
	ck_assert(cfg.exports[0].rules[0].rw);
	ck_assert(!cfg.exports[0].rules[0].root_squash);
	ck_assert_uint_eq(cfg.exports[0].rules[0].nflavors, 1);
	ck_assert_int_eq(cfg.exports[0].rules[0].flavors[0], REFFS_AUTH_SYS);
}
END_TEST

START_TEST(test_defaults_iouring_sizes)
{
	struct reffs_config cfg;
	reffs_config_defaults(&cfg);
	ck_assert_uint_eq(cfg.network_sq_size, 2048);
	ck_assert_uint_eq(cfg.network_cq_size, 8192);
	ck_assert_uint_eq(cfg.backend_sq_size, 512);
	ck_assert_uint_eq(cfg.backend_cq_size, 2048);
}
END_TEST

/* ------------------------------------------------------------------ */
/* load -- empty file leaves defaults intact                            */
/* ------------------------------------------------------------------ */

START_TEST(test_load_empty_file)
{
	struct reffs_config cfg;
	reffs_config_defaults(&cfg);

	char *path = write_toml("");
	ck_assert_ptr_nonnull(path);

	ck_assert_int_eq(reffs_config_load(&cfg, path), 0);
	ck_assert_int_eq(cfg.port, 2049);
	ck_assert_int_eq(cfg.role, REFFS_ROLE_STANDALONE);

	unlink(path);
	free(path);
}
END_TEST

/* ------------------------------------------------------------------ */
/* load -- [server] overrides                                           */
/* ------------------------------------------------------------------ */

START_TEST(test_load_server_port)
{
	struct reffs_config cfg;
	reffs_config_defaults(&cfg);

	char *path = write_toml("[server]\nport = 2050\n");
	ck_assert_ptr_nonnull(path);

	ck_assert_int_eq(reffs_config_load(&cfg, path), 0);
	ck_assert_int_eq(cfg.port, 2050);

	unlink(path);
	free(path);
}
END_TEST

START_TEST(test_load_server_role_mds)
{
	struct reffs_config cfg;
	reffs_config_defaults(&cfg);

	char *path = write_toml("[server]\nrole = \"mds\"\n");
	ck_assert_ptr_nonnull(path);

	ck_assert_int_eq(reffs_config_load(&cfg, path), 0);
	ck_assert_int_eq(cfg.role, REFFS_ROLE_MDS);

	unlink(path);
	free(path);
}
END_TEST

START_TEST(test_load_server_role_ds_erasure)
{
	struct reffs_config cfg;
	reffs_config_defaults(&cfg);

	char *path = write_toml("[server]\nrole = \"ds_erasure\"\n");
	ck_assert_ptr_nonnull(path);

	ck_assert_int_eq(reffs_config_load(&cfg, path), 0);
	ck_assert_int_eq(cfg.role, REFFS_ROLE_DS_ERASURE);

	unlink(path);
	free(path);
}
END_TEST

START_TEST(test_load_server_tls)
{
	struct reffs_config cfg;
	reffs_config_defaults(&cfg);

	char *path = write_toml("[server]\ntls = true\n");
	ck_assert_ptr_nonnull(path);

	ck_assert_int_eq(reffs_config_load(&cfg, path), 0);
	ck_assert(cfg.tls);

	unlink(path);
	free(path);
}
END_TEST

/*
 * [server] register_with_rpcbind defaults to true.  This pins the
 * upgrade-safe default for NFSv3 MOUNT auto-discovery; an admin who
 * upgrades reffsd into an existing rpcbind environment without
 * reading the changelog must continue to see MOUNT discovery work.
 * See .claude/design/no-rpcbind.md.
 */
START_TEST(test_defaults_register_with_rpcbind_true)
{
	struct reffs_config cfg;

	reffs_config_defaults(&cfg);
	ck_assert(cfg.register_with_rpcbind);
}
END_TEST

START_TEST(test_load_server_register_with_rpcbind_true)
{
	struct reffs_config cfg;

	reffs_config_defaults(&cfg);

	char *path = write_toml("[server]\nregister_with_rpcbind = true\n");

	ck_assert_ptr_nonnull(path);

	ck_assert_int_eq(reffs_config_load(&cfg, path), 0);
	ck_assert(cfg.register_with_rpcbind);

	unlink(path);
	free(path);
}
END_TEST

START_TEST(test_load_server_register_with_rpcbind_false)
{
	struct reffs_config cfg;

	reffs_config_defaults(&cfg);

	char *path = write_toml("[server]\nregister_with_rpcbind = false\n");

	ck_assert_ptr_nonnull(path);

	ck_assert_int_eq(reffs_config_load(&cfg, path), 0);
	ck_assert(!cfg.register_with_rpcbind);

	unlink(path);
	free(path);
}
END_TEST

START_TEST(test_load_server_log_level_debug)
{
	struct reffs_config cfg;
	reffs_config_defaults(&cfg);

	char *path = write_toml("[server]\nlog_level = \"debug\"\n");
	ck_assert_ptr_nonnull(path);

	ck_assert_int_eq(reffs_config_load(&cfg, path), 0);
	ck_assert_int_eq(cfg.log_level, REFFS_LOG_DEBUG);

	unlink(path);
	free(path);
}
END_TEST

START_TEST(test_load_server_minor_versions)
{
	struct reffs_config cfg;
	reffs_config_defaults(&cfg);

	char *path = write_toml("[server]\nminor_versions = [2]\n");
	ck_assert_ptr_nonnull(path);

	ck_assert_int_eq(reffs_config_load(&cfg, path), 0);
	ck_assert_uint_eq(cfg.n_minor_versions, 1);
	ck_assert_int_eq(cfg.minor_versions[0], 2);

	unlink(path);
	free(path);
}
END_TEST

/* ------------------------------------------------------------------ */
/* load -- [backend] overrides                                          */
/* ------------------------------------------------------------------ */

START_TEST(test_load_backend_posix)
{
	struct reffs_config cfg;
	reffs_config_defaults(&cfg);

	char *path =
		write_toml("[backend]\ntype = \"posix\"\npath = \"/data\"\n");
	ck_assert_ptr_nonnull(path);

	ck_assert_int_eq(reffs_config_load(&cfg, path), 0);
	ck_assert_int_eq(cfg.backend_type, REFFS_BACKEND_POSIX);
	ck_assert_str_eq(cfg.backend_path, "/data");

	unlink(path);
	free(path);
}
END_TEST

/* ------------------------------------------------------------------ */
/* load -- [[export]] overrides                                         */
/* ------------------------------------------------------------------ */

START_TEST(test_load_export_basic)
{
	struct reffs_config cfg;
	reffs_config_defaults(&cfg);

	char *path = write_toml("[[export]]\n"
				"path = \"/exports/data\"\n"
				"\n"
				"    [[export.clients]]\n"
				"    match       = \"192.168.1.0/24\"\n"
				"    access      = \"ro\"\n"
				"    root_squash = true\n"
				"    flavors     = [\"krb5\"]\n");
	ck_assert_ptr_nonnull(path);

	ck_assert_int_eq(reffs_config_load(&cfg, path), 0);
	ck_assert_uint_eq(cfg.nexports, 1);
	ck_assert_str_eq(cfg.exports[0].path, "/exports/data");
	ck_assert_uint_eq(cfg.exports[0].nrules, 1);
	ck_assert_str_eq(cfg.exports[0].rules[0].match, "192.168.1.0/24");
	ck_assert(!cfg.exports[0].rules[0].rw); /* access=ro */
	ck_assert(cfg.exports[0].rules[0].root_squash);
	ck_assert_uint_eq(cfg.exports[0].rules[0].nflavors, 1);
	ck_assert_int_eq(cfg.exports[0].rules[0].flavors[0], REFFS_AUTH_KRB5);

	unlink(path);
	free(path);
}
END_TEST

START_TEST(test_load_export_multiple_flavors)
{
	struct reffs_config cfg;
	reffs_config_defaults(&cfg);

	char *path = write_toml(
		"[[export]]\n"
		"path = \"/\"\n"
		"\n"
		"    [[export.clients]]\n"
		"    match   = \"*\"\n"
		"    flavors = [\"sys\", \"krb5\", \"krb5i\", \"krb5p\"]\n");
	ck_assert_ptr_nonnull(path);

	ck_assert_int_eq(reffs_config_load(&cfg, path), 0);
	ck_assert_uint_eq(cfg.exports[0].nrules, 1);
	ck_assert_uint_eq(cfg.exports[0].rules[0].nflavors, 4);
	ck_assert_int_eq(cfg.exports[0].rules[0].flavors[0], REFFS_AUTH_SYS);
	ck_assert_int_eq(cfg.exports[0].rules[0].flavors[1], REFFS_AUTH_KRB5);
	ck_assert_int_eq(cfg.exports[0].rules[0].flavors[2], REFFS_AUTH_KRB5I);
	ck_assert_int_eq(cfg.exports[0].rules[0].flavors[3], REFFS_AUTH_KRB5P);

	unlink(path);
	free(path);
}
END_TEST

/*
 * Intent: an export with multiple [[export.clients]] rules is parsed
 * in order with the correct per-rule options.
 */
START_TEST(test_load_export_multi_rule)
{
	struct reffs_config cfg;
	reffs_config_defaults(&cfg);

	char *path = write_toml("[[export]]\n"
				"path = \"/multi\"\n"
				"\n"
				"    [[export.clients]]\n"
				"    match       = \"10.0.0.0/8\"\n"
				"    access      = \"rw\"\n"
				"    root_squash = false\n"
				"    flavors     = [\"sys\"]\n"
				"\n"
				"    [[export.clients]]\n"
				"    match       = \"*\"\n"
				"    access      = \"ro\"\n"
				"    root_squash = true\n"
				"    flavors     = [\"krb5\"]\n");
	ck_assert_ptr_nonnull(path);

	ck_assert_int_eq(reffs_config_load(&cfg, path), 0);
	ck_assert_uint_eq(cfg.nexports, 1);
	ck_assert_uint_eq(cfg.exports[0].nrules, 2);
	/* First rule */
	ck_assert_str_eq(cfg.exports[0].rules[0].match, "10.0.0.0/8");
	ck_assert(cfg.exports[0].rules[0].rw);
	ck_assert(!cfg.exports[0].rules[0].root_squash);
	ck_assert_int_eq(cfg.exports[0].rules[0].flavors[0], REFFS_AUTH_SYS);
	/* Second rule */
	ck_assert_str_eq(cfg.exports[0].rules[1].match, "*");
	ck_assert(!cfg.exports[0].rules[1].rw);
	ck_assert(cfg.exports[0].rules[1].root_squash);
	ck_assert_int_eq(cfg.exports[0].rules[1].flavors[0], REFFS_AUTH_KRB5);

	unlink(path);
	free(path);
}
END_TEST

/* ------------------------------------------------------------------ */
/* load -- [[data_server]] entries                                      */
/* ------------------------------------------------------------------ */

START_TEST(test_load_data_server_single)
{
	struct reffs_config cfg;
	reffs_config_defaults(&cfg);

	char *path = write_toml("[[data_server]]\n"
				"id      = 42\n"
				"address = \"192.168.2.105\"\n"
				"path    = \"/ds1\"\n");
	ck_assert_ptr_nonnull(path);

	ck_assert_int_eq(reffs_config_load(&cfg, path), 0);
	ck_assert_uint_eq(cfg.ndata_servers, 1);
	ck_assert_uint_eq(cfg.data_servers[0].id, 42);
	ck_assert_str_eq(cfg.data_servers[0].address, "192.168.2.105");
	ck_assert_str_eq(cfg.data_servers[0].path, "/ds1");

	unlink(path);
	free(path);
}
END_TEST

START_TEST(test_load_data_server_multiple)
{
	struct reffs_config cfg;
	reffs_config_defaults(&cfg);

	char *path = write_toml("[[data_server]]\n"
				"id      = 100\n"
				"address = \"192.168.2.105\"\n"
				"path    = \"/foo\"\n"
				"\n"
				"[[data_server]]\n"
				"id      = 200\n"
				"address = \"192.168.2.106\"\n"
				"path    = \"/bar\"\n"
				"\n"
				"[[data_server]]\n"
				"id      = 300\n"
				"address = \"10.0.0.1\"\n"
				"path    = \"/baz\"\n");
	ck_assert_ptr_nonnull(path);

	ck_assert_int_eq(reffs_config_load(&cfg, path), 0);
	ck_assert_uint_eq(cfg.ndata_servers, 3);
	ck_assert_uint_eq(cfg.data_servers[0].id, 100);
	ck_assert_str_eq(cfg.data_servers[0].address, "192.168.2.105");
	ck_assert_str_eq(cfg.data_servers[0].path, "/foo");
	ck_assert_uint_eq(cfg.data_servers[1].id, 200);
	ck_assert_str_eq(cfg.data_servers[1].address, "192.168.2.106");
	ck_assert_str_eq(cfg.data_servers[1].path, "/bar");
	ck_assert_uint_eq(cfg.data_servers[2].id, 300);
	ck_assert_str_eq(cfg.data_servers[2].address, "10.0.0.1");
	ck_assert_str_eq(cfg.data_servers[2].path, "/baz");

	unlink(path);
	free(path);
}
END_TEST

START_TEST(test_load_data_server_none)
{
	struct reffs_config cfg;
	reffs_config_defaults(&cfg);

	char *path = write_toml("[server]\nrole = \"mds\"\n");
	ck_assert_ptr_nonnull(path);

	ck_assert_int_eq(reffs_config_load(&cfg, path), 0);
	ck_assert_uint_eq(cfg.ndata_servers, 0);

	unlink(path);
	free(path);
}
END_TEST

/* ------------------------------------------------------------------ */
/* load -- [[proxy_mds]] entries                                        */
/* ------------------------------------------------------------------ */

START_TEST(test_load_proxy_mds_single)
{
	struct reffs_config cfg;
	reffs_config_defaults(&cfg);

	char *path = write_toml("[[proxy_mds]]\n"
				"id   = 1\n"
				"port = 4098\n"
				"bind = \"127.0.0.1\"\n");
	ck_assert_ptr_nonnull(path);

	ck_assert_int_eq(reffs_config_load(&cfg, path), 0);
	ck_assert_uint_eq(cfg.nproxy_mds, 1);
	ck_assert_uint_eq(cfg.proxy_mds[0].id, 1);
	ck_assert_uint_eq(cfg.proxy_mds[0].port, 4098);
	ck_assert_str_eq(cfg.proxy_mds[0].bind, "127.0.0.1");

	unlink(path);
	free(path);
}
END_TEST

START_TEST(test_load_proxy_mds_multiple)
{
	struct reffs_config cfg;
	reffs_config_defaults(&cfg);

	char *path = write_toml("[[proxy_mds]]\n"
				"id   = 1\n"
				"port = 4098\n"
				"\n"
				"[[proxy_mds]]\n"
				"id   = 2\n"
				"port = 4099\n"
				"bind = \"::1\"\n");
	ck_assert_ptr_nonnull(path);

	ck_assert_int_eq(reffs_config_load(&cfg, path), 0);
	ck_assert_uint_eq(cfg.nproxy_mds, 2);
	ck_assert_uint_eq(cfg.proxy_mds[0].id, 1);
	ck_assert_uint_eq(cfg.proxy_mds[0].port, 4098);
	ck_assert_str_eq(cfg.proxy_mds[0].bind, "*");
	ck_assert_uint_eq(cfg.proxy_mds[1].id, 2);
	ck_assert_uint_eq(cfg.proxy_mds[1].port, 4099);
	ck_assert_str_eq(cfg.proxy_mds[1].bind, "::1");

	unlink(path);
	free(path);
}
END_TEST

START_TEST(test_load_proxy_mds_none)
{
	struct reffs_config cfg;
	reffs_config_defaults(&cfg);

	char *path = write_toml("[server]\nrole = \"mds\"\n");
	ck_assert_ptr_nonnull(path);

	ck_assert_int_eq(reffs_config_load(&cfg, path), 0);
	ck_assert_uint_eq(cfg.nproxy_mds, 0);

	unlink(path);
	free(path);
}
END_TEST

START_TEST(test_load_proxy_mds_upstream_defaults)
{
	/*
	 * A [[proxy_mds]] entry that omits mds_port / mds_probe must
	 * get the standard NFS + probe ports (2049, 20490) so that the
	 * MDS-client session in a later phase can open sockets without
	 * admin ceremony for the common case.
	 */
	struct reffs_config cfg;
	reffs_config_defaults(&cfg);

	char *path = write_toml("[[proxy_mds]]\n"
				"id      = 1\n"
				"port    = 4098\n"
				"address = \"10.1.1.5\"\n");
	ck_assert_ptr_nonnull(path);

	ck_assert_int_eq(reffs_config_load(&cfg, path), 0);
	ck_assert_uint_eq(cfg.nproxy_mds, 1);
	ck_assert_str_eq(cfg.proxy_mds[0].address, "10.1.1.5");
	ck_assert_uint_eq(cfg.proxy_mds[0].mds_port, 2049);
	ck_assert_uint_eq(cfg.proxy_mds[0].mds_probe, 20490);

	unlink(path);
	free(path);
}
END_TEST

START_TEST(test_load_proxy_mds_upstream_explicit)
{
	/*
	 * Explicit mds_port / mds_probe override the defaults, letting
	 * the PS point at an MDS running on non-standard ports (useful
	 * for container deployments that multiplex multiple MDSes on
	 * one host).
	 */
	struct reffs_config cfg;
	reffs_config_defaults(&cfg);

	char *path = write_toml("[[proxy_mds]]\n"
				"id        = 2\n"
				"port      = 4099\n"
				"address   = \"fd00::5\"\n"
				"mds_port  = 12049\n"
				"mds_probe = 30490\n");
	ck_assert_ptr_nonnull(path);

	ck_assert_int_eq(reffs_config_load(&cfg, path), 0);
	ck_assert_uint_eq(cfg.nproxy_mds, 1);
	ck_assert_str_eq(cfg.proxy_mds[0].address, "fd00::5");
	ck_assert_uint_eq(cfg.proxy_mds[0].mds_port, 12049);
	ck_assert_uint_eq(cfg.proxy_mds[0].mds_probe, 30490);

	unlink(path);
	free(path);
}
END_TEST

START_TEST(test_load_proxy_mds_upstream_absent)
{
	/*
	 * Missing address leaves the field empty string.  Later phases
	 * treat empty address as "upstream unconfigured" rather than
	 * failing reffsd startup, so the listener can still come up and
	 * serve an empty proxy namespace.  Explicit here so the next
	 * slice doesn't accidentally make empty-address fatal.
	 */
	struct reffs_config cfg;
	reffs_config_defaults(&cfg);

	char *path = write_toml("[[proxy_mds]]\n"
				"id   = 1\n"
				"port = 4098\n");
	ck_assert_ptr_nonnull(path);

	ck_assert_int_eq(reffs_config_load(&cfg, path), 0);
	ck_assert_uint_eq(cfg.nproxy_mds, 1);
	ck_assert_str_eq(cfg.proxy_mds[0].address, "");
	ck_assert_uint_eq(cfg.proxy_mds[0].mds_port, 2049);
	ck_assert_uint_eq(cfg.proxy_mds[0].mds_probe, 20490);

	unlink(path);
	free(path);
}
END_TEST

/* ------------------------------------------------------------------ */
/* load -- [[allowed_ps]] entries (slice 6b-i)                          */
/* ------------------------------------------------------------------ */

START_TEST(test_load_allowed_ps_single)
{
	struct reffs_config cfg;
	reffs_config_defaults(&cfg);

	char *path = write_toml("[[allowed_ps]]\n"
				"principal = \"host/ps.example.com@REALM\"\n");
	ck_assert_ptr_nonnull(path);

	ck_assert_int_eq(reffs_config_load(&cfg, path), 0);
	ck_assert_uint_eq(cfg.nallowed_ps, 1);
	ck_assert_str_eq(cfg.allowed_ps[0].principal,
			 "host/ps.example.com@REALM");

	unlink(path);
	free(path);
}
END_TEST

START_TEST(test_load_allowed_ps_multiple)
{
	struct reffs_config cfg;
	reffs_config_defaults(&cfg);

	char *path = write_toml("[[allowed_ps]]\n"
				"principal = \"host/ps1.example.com@REALM\"\n"
				"\n"
				"[[allowed_ps]]\n"
				"principal = \"host/ps2.example.com@REALM\"\n");
	ck_assert_ptr_nonnull(path);

	ck_assert_int_eq(reffs_config_load(&cfg, path), 0);
	ck_assert_uint_eq(cfg.nallowed_ps, 2);
	ck_assert_str_eq(cfg.allowed_ps[0].principal,
			 "host/ps1.example.com@REALM");
	ck_assert_str_eq(cfg.allowed_ps[1].principal,
			 "host/ps2.example.com@REALM");

	unlink(path);
	free(path);
}
END_TEST

START_TEST(test_load_allowed_ps_tls_cert_fingerprint)
{
	struct reffs_config cfg;
	reffs_config_defaults(&cfg);

	char *path = write_toml(
		"[[allowed_ps]]\n"
		"tls_cert_fingerprint = \"AB:CD:EF:01:23:45:67:89:AB:CD:EF\"\n");
	ck_assert_ptr_nonnull(path);

	ck_assert_int_eq(reffs_config_load(&cfg, path), 0);
	ck_assert_uint_eq(cfg.nallowed_ps, 1);
	ck_assert_str_eq(cfg.allowed_ps[0].tls_cert_fingerprint,
			 "AB:CD:EF:01:23:45:67:89:AB:CD:EF");
	ck_assert_str_eq(cfg.allowed_ps[0].principal, "");

	unlink(path);
	free(path);
}
END_TEST

START_TEST(test_load_allowed_ps_none)
{
	/*
	 * Default-deny: a config without any [[allowed_ps]] block
	 * yields nallowed_ps=0; the runtime check rejects every
	 * registration.
	 */
	struct reffs_config cfg;
	reffs_config_defaults(&cfg);

	char *path = write_toml("[server]\nrole = \"mds\"\n");
	ck_assert_ptr_nonnull(path);

	ck_assert_int_eq(reffs_config_load(&cfg, path), 0);
	ck_assert_uint_eq(cfg.nallowed_ps, 0);

	unlink(path);
	free(path);
}
END_TEST

/* ------------------------------------------------------------------ */
/* load -- bad file path                                                */
/* ------------------------------------------------------------------ */

START_TEST(test_load_missing_file)
{
	struct reffs_config cfg;
	reffs_config_defaults(&cfg);
	ck_assert_int_eq(reffs_config_load(&cfg, "/tmp/reffs_no_such_file_xyz"),
			 -1);
}
END_TEST

/* ------------------------------------------------------------------ */
/* helpers -- reffs_role_str                                            */
/* ------------------------------------------------------------------ */

START_TEST(test_role_str)
{
	ck_assert_str_eq(reffs_role_str(REFFS_ROLE_STANDALONE), "standalone");
	ck_assert_str_eq(reffs_role_str(REFFS_ROLE_MDS), "mds");
	ck_assert_str_eq(reffs_role_str(REFFS_ROLE_DS), "ds");
	ck_assert_str_eq(reffs_role_str(REFFS_ROLE_COMBINED), "combined");
	ck_assert_str_eq(reffs_role_str(REFFS_ROLE_DS_ERASURE), "ds_erasure");
}
END_TEST

/* ------------------------------------------------------------------ */
/* helpers -- reffs_role_exchgid_flags                                  */
/* ------------------------------------------------------------------ */

START_TEST(test_role_exchgid_flags)
{
	ck_assert_uint_eq(reffs_role_exchgid_flags(REFFS_ROLE_STANDALONE),
			  EXCHGID4_FLAG_USE_NON_PNFS);
	ck_assert_uint_eq(reffs_role_exchgid_flags(REFFS_ROLE_MDS),
			  EXCHGID4_FLAG_USE_PNFS_MDS);
	ck_assert_uint_eq(reffs_role_exchgid_flags(REFFS_ROLE_DS),
			  EXCHGID4_FLAG_USE_PNFS_DS);
	ck_assert_uint_eq(reffs_role_exchgid_flags(REFFS_ROLE_COMBINED),
			  EXCHGID4_FLAG_USE_PNFS_MDS |
				  EXCHGID4_FLAG_USE_PNFS_DS);
	ck_assert_uint_eq(reffs_role_exchgid_flags(REFFS_ROLE_DS_ERASURE),
			  EXCHGID4_FLAG_USE_ERASURE_DS);
}
END_TEST

/* ------------------------------------------------------------------ */

Suite *config_suite(void)
{
	Suite *s = suite_create("config");

	TCase *tc_defaults = tcase_create("defaults");
	tcase_add_test(tc_defaults, test_defaults_port);
	tcase_add_test(tc_defaults, test_defaults_probe_port);
	tcase_add_test(tc_defaults, test_defaults_role);
	tcase_add_test(tc_defaults, test_defaults_minor_versions);
	tcase_add_test(tc_defaults, test_defaults_grace_period);
	tcase_add_test(tc_defaults, test_defaults_tls_off);
	tcase_add_test(tc_defaults, test_defaults_backend_ram);
	tcase_add_test(tc_defaults, test_defaults_log_level_info);
	tcase_add_test(tc_defaults, test_defaults_one_export);
	tcase_add_test(tc_defaults, test_defaults_iouring_sizes);
	tcase_add_test(tc_defaults, test_defaults_register_with_rpcbind_true);
	suite_add_tcase(s, tc_defaults);

	TCase *tc_load = tcase_create("load");
	tcase_add_test(tc_load, test_load_empty_file);
	tcase_add_test(tc_load, test_load_server_port);
	tcase_add_test(tc_load, test_load_server_probe_port);
	tcase_add_test(tc_load, test_load_server_role_mds);
	tcase_add_test(tc_load, test_load_server_role_ds_erasure);
	tcase_add_test(tc_load, test_load_server_tls);
	tcase_add_test(tc_load, test_load_server_register_with_rpcbind_true);
	tcase_add_test(tc_load, test_load_server_register_with_rpcbind_false);
	tcase_add_test(tc_load, test_load_server_log_level_debug);
	tcase_add_test(tc_load, test_load_server_minor_versions);
	tcase_add_test(tc_load, test_load_backend_posix);
	tcase_add_test(tc_load, test_load_export_basic);
	tcase_add_test(tc_load, test_load_export_multiple_flavors);
	tcase_add_test(tc_load, test_load_export_multi_rule);
	tcase_add_test(tc_load, test_load_data_server_single);
	tcase_add_test(tc_load, test_load_data_server_multiple);
	tcase_add_test(tc_load, test_load_data_server_none);
	tcase_add_test(tc_load, test_load_proxy_mds_single);
	tcase_add_test(tc_load, test_load_proxy_mds_multiple);
	tcase_add_test(tc_load, test_load_proxy_mds_none);
	tcase_add_test(tc_load, test_load_proxy_mds_upstream_defaults);
	tcase_add_test(tc_load, test_load_proxy_mds_upstream_explicit);
	tcase_add_test(tc_load, test_load_proxy_mds_upstream_absent);
	tcase_add_test(tc_load, test_load_allowed_ps_single);
	tcase_add_test(tc_load, test_load_allowed_ps_multiple);
	tcase_add_test(tc_load, test_load_allowed_ps_tls_cert_fingerprint);
	tcase_add_test(tc_load, test_load_allowed_ps_none);
	tcase_add_test(tc_load, test_load_missing_file);
	suite_add_tcase(s, tc_load);

	TCase *tc_helpers = tcase_create("helpers");
	tcase_add_test(tc_helpers, test_role_str);
	tcase_add_test(tc_helpers, test_role_exchgid_flags);
	suite_add_tcase(s, tc_helpers);

	return s;
}

int main(void)
{
	return reffs_test_run_suite(config_suite(), NULL, NULL);
}
