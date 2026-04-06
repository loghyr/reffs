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
	tcase_add_test(tc_defaults, test_defaults_role);
	tcase_add_test(tc_defaults, test_defaults_minor_versions);
	tcase_add_test(tc_defaults, test_defaults_grace_period);
	tcase_add_test(tc_defaults, test_defaults_tls_off);
	tcase_add_test(tc_defaults, test_defaults_backend_ram);
	tcase_add_test(tc_defaults, test_defaults_log_level_info);
	tcase_add_test(tc_defaults, test_defaults_one_export);
	tcase_add_test(tc_defaults, test_defaults_iouring_sizes);
	suite_add_tcase(s, tc_defaults);

	TCase *tc_load = tcase_create("load");
	tcase_add_test(tc_load, test_load_empty_file);
	tcase_add_test(tc_load, test_load_server_port);
	tcase_add_test(tc_load, test_load_server_role_mds);
	tcase_add_test(tc_load, test_load_server_role_ds_erasure);
	tcase_add_test(tc_load, test_load_server_tls);
	tcase_add_test(tc_load, test_load_server_log_level_debug);
	tcase_add_test(tc_load, test_load_server_minor_versions);
	tcase_add_test(tc_load, test_load_backend_posix);
	tcase_add_test(tc_load, test_load_export_basic);
	tcase_add_test(tc_load, test_load_export_multiple_flavors);
	tcase_add_test(tc_load, test_load_export_multi_rule);
	tcase_add_test(tc_load, test_load_data_server_single);
	tcase_add_test(tc_load, test_load_data_server_multiple);
	tcase_add_test(tc_load, test_load_data_server_none);
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
