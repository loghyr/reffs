/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" /* IWYU pragma: keep */
#endif

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <check.h>

#include "reffs/dstore.h"
#include "reffs/dstore_ops.h"
#include "reffs/fixed_inventory.h"
#include "reffs/layout_segment.h"
#include "libreffs_test.h"

#include "ds_renewal_internal.h"

static pthread_mutex_t call_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t call_condition = PTHREAD_COND_INITIALIZER;
static bool call_entered;
static bool call_release;
static bool block_create;

static int mock_create(struct dstore *ds __attribute__((unused)),
		       const uint8_t *fh __attribute__((unused)),
		       uint32_t fh_len __attribute__((unused)),
		       const char *name __attribute__((unused)),
		       uint8_t *out __attribute__((unused)),
		       uint32_t *out_len __attribute__((unused)))
{
	if (!block_create) {
		*out_len = 4;
		memset(out, !strcmp(name, "member-0") ? 0x10 : 0x11, *out_len);
		return 0;
	}
	pthread_mutex_lock(&call_mutex);
	call_entered = true;
	pthread_cond_broadcast(&call_condition);
	while (!call_release)
		pthread_cond_wait(&call_condition, &call_mutex);
	pthread_mutex_unlock(&call_mutex);
	return 0;
}

static int mock_getattr(struct dstore *ds __attribute__((unused)),
			const uint8_t *fh __attribute__((unused)),
			uint32_t fh_len __attribute__((unused)),
			struct layout_data_file *attributes)
{
	attributes->ldf_size = 0;
	return 0;
}

static int mock_remove(struct dstore *ds __attribute__((unused)),
		       const uint8_t *fh __attribute__((unused)),
		       uint32_t fh_len __attribute__((unused)),
		       const char *name __attribute__((unused)))
{
	return 0;
}

static const struct dstore_ops mock_ops = {
	.name = "ordinary-gate-test",
	.create = mock_create,
	.remove = mock_remove,
	.getattr = mock_getattr,
};

static struct dstore *make_dstore(void)
{
	struct dstore *ds;

	ck_assert_int_eq(dstore_init(), 0);
	ds = dstore_alloc(91, "192.0.2.91", 0, 0, "/ds", REFFS_DS_PROTO_NFSV3,
			  false, false);
	ck_assert_ptr_nonnull(ds);
	ds->ds_ops = &mock_ops;
	return ds;
}

static void destroy_dstore(struct dstore *ds)
{
	dstore_put(ds);
	dstore_fini();
}

static void *create_thread(void *argument)
{
	struct dstore *ds = argument;
	uint8_t handle[8];
	uint32_t handle_len = 0;

	return (void *)(intptr_t)dstore_data_file_create(ds, NULL, 0, "member",
							 handle, &handle_len);
}

static void *close_thread(void *argument)
{
	return (void *)(intptr_t)dstore_ordinary_close(argument);
}

static int startup_worker_calls;

static int startup_after_load(const struct reffs_config *cfg, int load_result)
{
	int ret = dstore_startup_result(cfg, load_result);

	if (ret)
		return ret;
	startup_worker_calls++;
	return 0;
}

START_TEST(test_fixed_load_failure_stops_before_workers)
{
	struct reffs_config cfg = { 0 };

	cfg.ndata_servers = 1;
	cfg.data_servers[0].prototype_registration.enabled = true;
	cfg.data_servers[0].prototype_registration.fixed_inventory = true;
	startup_worker_calls = 0;
	ck_assert_int_eq(startup_after_load(&cfg, -EIO), -EIO);
	ck_assert_int_eq(startup_worker_calls, 0);

	cfg.data_servers[0].prototype_registration.fixed_inventory = false;
	ck_assert_int_eq(startup_after_load(&cfg, -EIO), 0);
	ck_assert_int_eq(startup_worker_calls, 1);
}
END_TEST

START_TEST(test_close_drains_and_refuses_new_calls)
{
	struct dstore *ds = make_dstore();
	pthread_t creator, closer;
	void *result;

	call_entered = false;
	call_release = false;
	block_create = true;
	ck_assert_int_eq(pthread_create(&creator, NULL, create_thread, ds), 0);
	pthread_mutex_lock(&call_mutex);
	while (!call_entered)
		pthread_cond_wait(&call_condition, &call_mutex);
	pthread_mutex_unlock(&call_mutex);
	ck_assert_int_eq(pthread_create(&closer, NULL, close_thread, ds), 0);
	for (;;) {
		pthread_mutex_lock(&ds->ds_ordinary_gate.mutex);
		enum dstore_ordinary_state state = ds->ds_ordinary_gate.state;

		pthread_mutex_unlock(&ds->ds_ordinary_gate.mutex);
		if (state == DSTORE_ORDINARY_REGISTRATION_PENDING)
			break;
		sched_yield();
	}
	ck_assert_int_eq(dstore_data_file_remove(ds, NULL, 0, "member"),
			 -ESHUTDOWN);
	ck_assert_uint_eq(
		atomic_load_explicit(
			&ds->ds_ordinary_gate.refused[DSTORE_ORDINARY_REMOVE],
			memory_order_relaxed),
		1);
	pthread_mutex_lock(&call_mutex);
	call_release = true;
	pthread_cond_broadcast(&call_condition);
	pthread_mutex_unlock(&call_mutex);
	ck_assert_int_eq(pthread_join(creator, &result), 0);
	ck_assert_int_eq((intptr_t)result, 0);
	ck_assert_int_eq(pthread_join(closer, &result), 0);
	ck_assert_int_eq((intptr_t)result, 0);
	ck_assert_uint_eq(ds->ds_ordinary_gate.in_flight, 0);
	dstore_ordinary_activate(ds);
	ck_assert_int_eq(dstore_data_file_remove(ds, NULL, 0, "member"),
			 -ESHUTDOWN);
	ck_assert_int_eq(ds_nfsv3_renewal_test(ds), -ESHUTDOWN);
	ck_assert_uint_eq(
		atomic_load_explicit(
			&ds->ds_ordinary_gate.refused[DSTORE_ORDINARY_NULL],
			memory_order_relaxed),
		1);
	dstore_ordinary_retire(ds);
	ck_assert_int_eq(dstore_ordinary_reopen(ds), 0);
	destroy_dstore(ds);
}
END_TEST

static void fill_fixed_config(struct dstore *ds, const char *directory)
{
	struct reffs_prototype_registration_config *config =
		&ds->ds_prototype_config;

	memset(config, 0, sizeof(*config));
	config->enabled = true;
	config->fixed_inventory = true;
	strcpy(config->auth_domain, "client.example");
	memset(config->store_uuid, 0x20, sizeof(config->store_uuid));
	memset(config->binding_token, 0x30, sizeof(config->binding_token));
	config->chunk_size = 4096;
	config->data_count = 1;
	config->parity_count = 1;
	config->writer_id = 17;
	config->pnfs_clientid = 23;
	config->object_count = 2;
	for (uint32_t i = 0; i < 2; i++) {
		snprintf(config->objects[i].name,
			 sizeof(config->objects[i].name), "member-%u", i);
		config->objects[i].ordinary_handle_len = 4;
		memset(config->objects[i].ordinary_handle, 0x10 + i, 4);
		memset(config->objects[i].persisted_handle, 0x40 + i,
		       sizeof(config->objects[i].persisted_handle));
	}
	strcpy(ds->ds_state_dir, directory);
}

START_TEST(test_preflight_persists_exact_empty_vector)
{
	char directory[] = "/tmp/reffs-preflight-XXXXXX";
	char path[512];
	struct ffv2_fixed_inventory_record loaded;
	struct dstore *ds = make_dstore();

	ck_assert_ptr_nonnull(mkdtemp(directory));
	fill_fixed_config(ds, directory);
	block_create = false;
	ck_assert_int_eq(dstore_fixed_inventory_preflight(ds), 0);
	ck_assert_ptr_nonnull(ds->ds_fixed_inventory);
	ck_assert_int_eq(
		ffv2_fixed_inventory_load(directory, ds->ds_id, &loaded), 0);
	ck_assert_int_eq(loaded.state, FFV2_INVENTORY_FREE);
	ck_assert_int_eq(dstore_fixed_status_write(ds), 0);
	snprintf(path, sizeof(path), "%s/ffv2-fixed-status-91.json", directory);
	FILE *status = fopen(path, "r");
	char status_data[4096] = { 0 };

	ck_assert_ptr_nonnull(status);
	ck_assert_int_gt(fread(status_data, 1, sizeof(status_data) - 1, status),
			 0);
	fclose(status);
	ck_assert_ptr_nonnull(strstr(status_data, "\"inventory_state\":0"));
	ck_assert_ptr_nonnull(strstr(status_data, "\"ordinary_state\":0"));
	ck_assert_ptr_nonnull(strstr(
		status_data, "\"admitted\":[2,0,0,0,0,0,2,0,0,0,0,0,0,0,0]"));
	ck_assert_ptr_null(
		memmem(status_data, strlen(status_data),
		       ds->ds_prototype_config.binding_token,
		       sizeof(ds->ds_prototype_config.binding_token)));
	ds->ds_prototype_config.objects[0].ordinary_handle[0] = 0xff;
	ck_assert_int_eq(dstore_fixed_inventory_preflight(ds), -ESTALE);
	ds->ds_prototype_config.enabled = false;
	destroy_dstore(ds);
	unlink(path);
	snprintf(path, sizeof(path), "%s/ffv2-fixed-inventory-91", directory);
	unlink(path);
	rmdir(directory);
}
END_TEST

START_TEST(test_observation_publishes_counters_off_operation_path)
{
	char directory[] = "/tmp/reffs-status-observation-XXXXXX";
	char path[512], status_data[4096] = { 0 };
	struct dstore *ds = make_dstore();
	FILE *status;

	ck_assert_ptr_nonnull(mkdtemp(directory));
	fill_fixed_config(ds, directory);
	snprintf(path, sizeof(path), "%s/ffv2-fixed-status-91.json", directory);
	ck_assert_int_eq(dstore_ordinary_get(ds, DSTORE_ORDINARY_GETATTR), 0);
	dstore_ordinary_put(ds);
	ck_assert_int_eq(access(path, F_OK), -1);
	ck_assert_int_eq(errno, ENOENT);

	ck_assert_int_eq(dstore_fixed_status_write(ds), 0);
	status = fopen(path, "r");
	ck_assert_ptr_nonnull(status);
	ck_assert_int_gt(fread(status_data, 1, sizeof(status_data) - 1, status),
			 0);
	fclose(status);
	ck_assert_ptr_nonnull(strstr(
		status_data, "\"admitted\":[0,0,0,0,0,0,1,0,0,0,0,0,0,0,0]"));
	ck_assert_ptr_nonnull(strstr(
		status_data, "\"refused\":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]"));
	ck_assert_ptr_null(
		memmem(status_data, strlen(status_data),
		       ds->ds_prototype_config.binding_token,
		       sizeof(ds->ds_prototype_config.binding_token)));
	ds->ds_prototype_config.enabled = false;
	destroy_dstore(ds);
	unlink(path);
	rmdir(directory);
}
END_TEST

static Suite *ordinary_gate_suite(void)
{
	Suite *suite = suite_create("ordinary_gate");
	TCase *tc = tcase_create("lifecycle");

	tcase_add_test(tc, test_close_drains_and_refuses_new_calls);
	tcase_add_test(tc, test_fixed_load_failure_stops_before_workers);
	tcase_add_test(tc, test_preflight_persists_exact_empty_vector);
	tcase_add_test(tc,
		       test_observation_publishes_counters_off_operation_path);
	suite_add_tcase(suite, tc);
	return suite;
}

int main(void)
{
	return reffs_test_run_suite(ordinary_gate_suite(), NULL, NULL);
}
