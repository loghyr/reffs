/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifndef REFFS_D2_STORE_H
#define REFFS_D2_STORE_H

#include "d1_store.h"
#include "d2_files.h"

struct d2_store;

struct d2_store_config {
	struct d2_provision files;
	uint32_t chunk_bytes;
	uint64_t max_file_bytes;
};

struct d2_store_rebind {
	struct d2_rebind files;
	uint32_t chunk_bytes;
	uint64_t max_file_bytes;
};

uint32_t d2_store_provision(int dirfd, const struct d2_store_config *config,
			    struct d2_binding *binding, struct d2_store **out);
uint32_t d2_store_rebind(int dirfd, const struct d2_store_rebind *config,
			 struct d2_binding *binding, struct d2_store **out);
void d2_store_crash(struct d2_store *store);
uint32_t d2_store_close(struct d2_store *store);

uint32_t d2_store_apply(struct d2_store *store, const struct d1_envelope *env,
			struct d1_result *result);
d1_admission_id d2_store_admit(struct d2_store *store,
			       const struct d1_objkey *object, uint32_t writer,
			       uint32_t rights);
d1_admission_id
d2_store_admit_full(struct d2_store *store, const struct d1_objkey *object,
		    const struct d1_fixture_authority *authority);
void d2_store_revoke(struct d2_store *store, d1_admission_id admission);
void d2_store_expire(struct d2_store *store, d1_admission_id admission);
d1_custody_id d2_store_custody(struct d2_store *store, d1_version_id version);
void d2_store_certificate(struct d2_store *store,
			  const uint8_t certificate[D1_CERTIFICATE_BYTES]);
d1_admission_id d2_store_admission_handle(struct d2_store *store, uint64_t raw);
d1_txn_id d2_store_txn_handle(struct d2_store *store, uint64_t raw);
d1_version_id d2_store_version_handle(struct d2_store *store, uint64_t raw);
d1_custody_id d2_store_custody_handle(struct d2_store *store, uint64_t raw);
d1_repair_id d2_store_repair_handle(struct d2_store *store, uint64_t raw);
d1_episode_id d2_store_episode_handle(struct d2_store *store, uint64_t raw);
void d2_store_verifier(struct d2_store *store,
		       uint8_t verifier[D1_VERIFIER_BYTES]);
uint64_t d2_store_incarnation(struct d2_store *store);
void d2_store_fail_next_index(struct d2_store *store);
void d2_store_set_io_hook(struct d2_store *store, d2_io_hook_fn hook,
			  void *arg);

bool d2_store_visible(struct d2_store *store, const struct d1_objkey *object,
		      uint64_t index, d1_version_id *version);
bool d2_store_guard(struct d2_store *store, const struct d1_objkey *object,
		    uint64_t index, struct d1_guard *guard);
uint64_t d2_store_eof(struct d2_store *store, const struct d1_objkey *object);
uint64_t d2_store_wal_bytes(struct d2_store *store);
uint32_t d2_store_view_open(struct d2_store *store,
			    const struct d1_objkey *object,
			    d1_admission_id admission,
			    const struct d1_selection_spec *selection,
			    uint64_t begin, uint64_t end,
			    struct d1_view **view);

#endif /* REFFS_D2_STORE_H */
