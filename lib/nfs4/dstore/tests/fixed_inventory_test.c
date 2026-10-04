/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" /* IWYU pragma: keep */
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <check.h>

#include "reffs/fixed_inventory.h"
#include "libreffs_test.h"

static char state_dir[] = "/tmp/reffs-fixed-inventory-XXXXXX";

static void setup(void)
{
	strcpy(state_dir, "/tmp/reffs-fixed-inventory-XXXXXX");
	ck_assert_ptr_nonnull(mkdtemp(state_dir));
}

static void teardown(void)
{
	char path[512];

	snprintf(path, sizeof(path), "%s/ffv2-fixed-inventory-7", state_dir);
	unlink(path);
	rmdir(state_dir);
}

static void fill_config(struct reffs_prototype_registration_config *config)
{
	memset(config, 0, sizeof(*config));
	config->enabled = true;
	config->fixed_inventory = true;
	strcpy(config->auth_domain, "client.example");
	for (size_t i = 0; i < sizeof(config->store_uuid); i++)
		config->store_uuid[i] = (uint8_t)i;
	for (size_t i = 0; i < sizeof(config->binding_token); i++)
		config->binding_token[i] = (uint8_t)(0x80 + i);
	config->chunk_size = 4096;
	config->data_count = 1;
	config->parity_count = 1;
	config->writer_id = 17;
	config->pnfs_clientid = 23;
	config->object_count = 2;
	for (uint32_t i = 0; i < config->object_count; i++) {
		struct reffs_prototype_object_config *object =
			&config->objects[i];

		snprintf(object->name, sizeof(object->name), "ffv2-member-%u",
			 i);
		object->ordinary_handle_len = 4;
		memset(object->ordinary_handle, 0x10 + i,
		       object->ordinary_handle_len);
		memset(object->persisted_handle, 0x20 + i,
		       sizeof(object->persisted_handle));
	}
}

static void fill_record(struct ffv2_fixed_inventory_record *record)
{
	struct reffs_prototype_registration_config config;

	memset(record, 0, sizeof(*record));
	fill_config(&config);
	ck_assert_int_eq(ffv2_fixed_inventory_identity_init(
				 record ? &record->identity : NULL, 7,
				 "192.0.2.7", "/kernel-ds", &config),
			 0);
}

START_TEST(test_round_trip_and_token_is_not_stored)
{
	struct reffs_prototype_registration_config config;
	struct ffv2_fixed_inventory_record record, decoded;
	uint8_t *data = NULL;
	size_t len = 0;

	fill_config(&config);
	fill_record(&record);
	ck_assert_int_eq(ffv2_fixed_inventory_encode(&record, &data, &len), 0);
	ck_assert_ptr_null(memmem(data, len, config.binding_token,
				  sizeof(config.binding_token)));
	ck_assert_int_eq(ffv2_fixed_inventory_decode(data, len, &decoded), 0);
	ck_assert(ffv2_fixed_inventory_identity_equal(&record.identity,
						      &decoded.identity));
	ck_assert_int_eq(decoded.state, FFV2_INVENTORY_FREE);
	free(data);
}
END_TEST

START_TEST(test_rejects_corruption_truncation_and_wrong_digest)
{
	struct ffv2_fixed_inventory_record record, decoded;
	uint8_t *data = NULL;
	size_t len = 0;

	fill_record(&record);
	ck_assert_int_eq(ffv2_fixed_inventory_encode(&record, &data, &len), 0);
	data[20] ^= 0x80;
	ck_assert_int_eq(ffv2_fixed_inventory_decode(data, len, &decoded),
			 -EBADMSG);
	data[20] ^= 0x80;
	ck_assert_int_eq(ffv2_fixed_inventory_decode(data, len - 1, &decoded),
			 -EBADMSG);
	record.identity.digest[0] ^= 1;
	free(data);
	ck_assert_int_eq(ffv2_fixed_inventory_encode(&record, &data, &len), 0);
	ck_assert_int_eq(ffv2_fixed_inventory_decode(data, len, &decoded),
			 -EBADMSG);
	free(data);
}
END_TEST

START_TEST(test_rejects_duplicate_and_reordered_identity)
{
	struct reffs_prototype_registration_config config;
	struct ffv2_fixed_inventory_identity original, changed;

	fill_config(&config);
	ck_assert_int_eq(
		ffv2_fixed_inventory_identity_init(&original, 7, "192.0.2.7",
						   "/kernel-ds", &config),
		0);
	config.objects[1] = config.objects[0];
	ck_assert_int_eq(
		ffv2_fixed_inventory_identity_init(&changed, 7, "192.0.2.7",
						   "/kernel-ds", &config),
		0);
	ck_assert(!ffv2_fixed_inventory_identity_equal(&original, &changed));
	fill_config(&config);
	struct reffs_prototype_object_config swap = config.objects[0];

	config.objects[0] = config.objects[1];
	config.objects[1] = swap;
	ck_assert_int_eq(
		ffv2_fixed_inventory_identity_init(&changed, 7, "192.0.2.7",
						   "/kernel-ds", &config),
		0);
	ck_assert(!ffv2_fixed_inventory_identity_equal(&original, &changed));
}
END_TEST

static void check_restart_difference(
	const struct ffv2_fixed_inventory_identity *stored,
	const struct reffs_prototype_registration_config *config,
	uint32_t dstore_id, const char *address, const char *export_path)
{
	struct ffv2_fixed_inventory_identity current;
	bool compatible = true;

	ck_assert_int_eq(
		ffv2_fixed_inventory_identity_init(&current, dstore_id, address,
						   export_path, config),
		0);
	ck_assert_int_eq(ffv2_fixed_inventory_restart_compatible(
				 stored, &current, &compatible),
			 0);
	ck_assert(!compatible);
}

START_TEST(test_restart_compatibility_keeps_full_identity_checks)
{
	struct reffs_prototype_registration_config config;
	struct ffv2_fixed_inventory_identity stored, current, corrupt;
	struct reffs_prototype_object_config swap;
	bool compatible = false;

	fill_config(&config);
	ck_assert_int_eq(
		ffv2_fixed_inventory_identity_init(&stored, 7, "192.0.2.7",
						   "/kernel-ds", &config),
		0);
	config.pnfs_clientid++;
	ck_assert_int_eq(
		ffv2_fixed_inventory_identity_init(&current, 7, "192.0.2.7",
						   "/kernel-ds", &config),
		0);
	ck_assert(!ffv2_fixed_inventory_identity_equal(&stored, &current));
	ck_assert_int_eq(ffv2_fixed_inventory_restart_compatible(
				 &stored, &current, &compatible),
			 0);
	ck_assert(compatible);
	ck_assert_int_eq(stored.pnfs_clientid, 23);
	ck_assert_int_eq(current.pnfs_clientid, 24);
	ck_assert(!ffv2_fixed_inventory_identity_equal(&stored, &current));

	check_restart_difference(&stored, &config, 8, "192.0.2.7",
				 "/kernel-ds");
	check_restart_difference(&stored, &config, 7, "192.0.2.8",
				 "/kernel-ds");
	check_restart_difference(&stored, &config, 7, "192.0.2.7", "/other-ds");
	strcpy(config.auth_domain, "other.example");
	check_restart_difference(&stored, &config, 7, "192.0.2.7",
				 "/kernel-ds");
	strcpy(config.auth_domain, "client.example");
	config.store_uuid[0]++;
	check_restart_difference(&stored, &config, 7, "192.0.2.7",
				 "/kernel-ds");
	config.store_uuid[0]--;
	config.binding_token[0]++;
	check_restart_difference(&stored, &config, 7, "192.0.2.7",
				 "/kernel-ds");
	config.binding_token[0]--;
	config.chunk_size++;
	check_restart_difference(&stored, &config, 7, "192.0.2.7",
				 "/kernel-ds");
	config.chunk_size--;
	config.writer_id++;
	check_restart_difference(&stored, &config, 7, "192.0.2.7",
				 "/kernel-ds");
	config.writer_id--;
	config.objects[0].name[0]++;
	check_restart_difference(&stored, &config, 7, "192.0.2.7",
				 "/kernel-ds");
	config.objects[0].name[0]--;
	config.objects[1].name[0]++;
	check_restart_difference(&stored, &config, 7, "192.0.2.7",
				 "/kernel-ds");
	config.objects[1].name[0]--;
	config.objects[0].ordinary_handle[0]++;
	check_restart_difference(&stored, &config, 7, "192.0.2.7",
				 "/kernel-ds");
	config.objects[0].ordinary_handle[0]--;
	config.objects[1].ordinary_handle[0]++;
	check_restart_difference(&stored, &config, 7, "192.0.2.7",
				 "/kernel-ds");
	config.objects[1].ordinary_handle[0]--;
	config.objects[0].ordinary_handle_len++;
	check_restart_difference(&stored, &config, 7, "192.0.2.7",
				 "/kernel-ds");
	config.objects[0].ordinary_handle_len--;
	config.objects[1].ordinary_handle_len++;
	check_restart_difference(&stored, &config, 7, "192.0.2.7",
				 "/kernel-ds");
	config.objects[1].ordinary_handle_len--;
	config.objects[0].persisted_handle[0]++;
	check_restart_difference(&stored, &config, 7, "192.0.2.7",
				 "/kernel-ds");
	config.objects[0].persisted_handle[0]--;
	config.objects[1].persisted_handle[0]++;
	check_restart_difference(&stored, &config, 7, "192.0.2.7",
				 "/kernel-ds");
	config.objects[1].persisted_handle[0]--;
	swap = config.objects[0];
	config.objects[0] = config.objects[1];
	config.objects[1] = swap;
	check_restart_difference(&stored, &config, 7, "192.0.2.7",
				 "/kernel-ds");

	corrupt = stored;
	corrupt.digest[0] ^= 1;
	ck_assert_int_eq(ffv2_fixed_inventory_restart_compatible(
				 &corrupt, &current, &compatible),
			 -EBADMSG);
	corrupt = current;
	corrupt.digest[0] ^= 1;
	ck_assert_int_eq(ffv2_fixed_inventory_restart_compatible(
				 &stored, &corrupt, &compatible),
			 -EBADMSG);
	fill_config(&config);
	config.pnfs_clientid = 0;
	ck_assert_int_eq(
		ffv2_fixed_inventory_identity_init(&current, 7, "192.0.2.7",
						   "/kernel-ds", &config),
		-EINVAL);
	fill_config(&config);
	config.data_count = 2;
	ck_assert_int_eq(
		ffv2_fixed_inventory_identity_init(&current, 7, "192.0.2.7",
						   "/kernel-ds", &config),
		-EINVAL);
	fill_config(&config);
	config.parity_count = 0;
	ck_assert_int_eq(
		ffv2_fixed_inventory_identity_init(&current, 7, "192.0.2.7",
						   "/kernel-ds", &config),
		-EINVAL);
	fill_config(&config);
	config.object_count = 1;
	ck_assert_int_eq(
		ffv2_fixed_inventory_identity_init(&current, 7, "192.0.2.7",
						   "/kernel-ds", &config),
		-EINVAL);
}
END_TEST

START_TEST(test_state_machine_preserves_owner)
{
	struct ffv2_fixed_inventory_record record;
	uuid_t owner = { 1 }, wrong = { 2 };

	fill_record(&record);
	ck_assert_int_eq(ffv2_fixed_inventory_transition(
				 &record, FFV2_INVENTORY_ASSIGNED, owner, 9),
			 -EINVAL);
	ck_assert_int_eq(ffv2_fixed_inventory_transition(
				 &record, FFV2_INVENTORY_CLAIMED, owner, 9),
			 0);
	ck_assert_int_eq(ffv2_fixed_inventory_transition(
				 &record, FFV2_INVENTORY_ASSIGNED, wrong, 10),
			 -EPERM);
	ck_assert_int_eq(ffv2_fixed_inventory_transition(
				 &record, FFV2_INVENTORY_ASSIGNED, owner, 9),
			 0);
	ck_assert_int_eq(ffv2_fixed_inventory_transition(
				 &record, FFV2_INVENTORY_CLAIMED, owner, 9),
			 -EINVAL);
	ck_assert_int_eq(ffv2_fixed_inventory_transition(
				 &record, FFV2_INVENTORY_RETIRED, owner, 9),
			 0);
	ck_assert_int_eq(ffv2_fixed_inventory_transition(
				 &record, FFV2_INVENTORY_ASSIGNED, owner, 9),
			 -EINVAL);
}
END_TEST

START_TEST(test_atomic_save_and_load)
{
	struct ffv2_fixed_inventory_record record, loaded;
	uuid_t owner = { 1 };

	fill_record(&record);
	ck_assert_int_eq(ffv2_fixed_inventory_save(state_dir, &record), 0);
	ck_assert_int_eq(ffv2_fixed_inventory_load(state_dir, 7, &loaded), 0);
	ck_assert(ffv2_fixed_inventory_identity_equal(&record.identity,
						      &loaded.identity));
	ck_assert_int_eq(ffv2_fixed_inventory_transition(
				 &record, FFV2_INVENTORY_CLAIMED, owner, 9),
			 0);
	ck_assert_int_eq(ffv2_fixed_inventory_save(state_dir, &record), 0);
	ck_assert_int_eq(ffv2_fixed_inventory_load(state_dir, 7, &loaded), 0);
	ck_assert_int_eq(loaded.state, FFV2_INVENTORY_CLAIMED);
	ck_assert_uint_eq(loaded.owner_ino, 9);
}
END_TEST

START_TEST(test_metadata_state_machine_is_durable)
{
	struct ffv2_fixed_inventory_record record, loaded;
	uuid_t owner = { 1 };

	fill_record(&record);
	ck_assert_int_eq(ffv2_fixed_metadata_transition(&record,
							FFV2_METADATA_DIRTY),
			 -EINVAL);
	ck_assert_int_eq(ffv2_fixed_inventory_transition(
				 &record, FFV2_INVENTORY_CLAIMED, owner, 9),
			 0);
	ck_assert_int_eq(ffv2_fixed_inventory_transition(
				 &record, FFV2_INVENTORY_ASSIGNED, owner, 9),
			 0);
	ck_assert_int_eq(ffv2_fixed_metadata_transition(&record,
							FFV2_METADATA_DIRTY),
			 0);
	ck_assert_uint_eq(record.metadata_epoch, 1);
	ck_assert_int_eq(ffv2_fixed_metadata_transition(&record,
							FFV2_METADATA_CLEAN),
			 -EINVAL);
	ck_assert_int_eq(ffv2_fixed_metadata_transition(
				 &record, FFV2_METADATA_COMMITTED),
			 0);
	ck_assert_int_eq(ffv2_fixed_inventory_save(state_dir, &record), 0);
	ck_assert_int_eq(ffv2_fixed_inventory_load(state_dir, 7, &loaded), 0);
	ck_assert_int_eq(loaded.metadata_state, FFV2_METADATA_COMMITTED);
	ck_assert_uint_eq(loaded.metadata_epoch, 1);
	ck_assert_int_eq(ffv2_fixed_metadata_transition(&record,
							FFV2_METADATA_CLEAN),
			 0);
	ck_assert_int_eq(ffv2_fixed_metadata_transition(&record,
							FFV2_METADATA_DIRTY),
			 0);
	ck_assert_uint_eq(record.metadata_epoch, 2);
}
END_TEST

static Suite *fixed_inventory_suite(void)
{
	Suite *suite = suite_create("fixed_inventory");
	TCase *tc = tcase_create("format");

	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_round_trip_and_token_is_not_stored);
	tcase_add_test(tc, test_rejects_corruption_truncation_and_wrong_digest);
	tcase_add_test(tc, test_rejects_duplicate_and_reordered_identity);
	tcase_add_test(tc,
		       test_restart_compatibility_keeps_full_identity_checks);
	tcase_add_test(tc, test_state_machine_preserves_owner);
	tcase_add_test(tc, test_atomic_save_and_load);
	tcase_add_test(tc, test_metadata_state_machine_is_durable);
	suite_add_tcase(suite, tc);
	return suite;
}

int main(void)
{
	return reffs_test_run_suite(fixed_inventory_suite(), NULL, NULL);
}
