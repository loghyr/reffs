/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "reffs/STRUCT.h"
#include "reffs/log.h"
#include <urcu.h>
#include <urcu/rculist.h>
#include <urcu/ref.h>

CDS_LIST_HEAD(STRUCT_list);

static void STRUCT_free_rcu(struct rcu_head *rcu)
{
	struct STRUCT *VAR = caa_container_of(rcu, struct STRUCT, FIELD_rcu);

	free(VAR);
}

static void STRUCT_release(struct urcu_ref *ref)
{
	struct STRUCT *VAR = caa_container_of(ref, struct STRUCT, FIELD_ref);

	call_rcu(&VAR->FIELD_rcu, STRUCT_free_rcu);
}

struct STRUCT *STRUCT_alloc(uint64_t id)
{
	struct STRUCT *VAR;

	VAR = calloc(1, sizeof(*VAR));
	if (!VAR) {
		LOG("Could not alloc a VAR");
		return NULL;
	}

	VAR->FIELD_id = id;
	cds_list_add_rcu(&VAR->FIELD_link, &STRUCT_list);
	urcu_ref_init(&VAR->FIELD_ref);

	return VAR;
}

struct STRUCT *STRUCT_find(uint64_t id)
{
	struct STRUCT *VAR = NULL;
	struct STRUCT *tmp;

	rcu_read_lock();
	cds_list_for_each_entry_rcu(tmp, &STRUCT_list, FIELD_link)
		if (id == tmp->FIELD_id) {
			VAR = STRUCT_get(tmp);
			break;
		}
	rcu_read_unlock();

	return VAR;
}

struct STRUCT *STRUCT_get(struct STRUCT *VAR)
{
	if (!VAR)
		return NULL;

	if (!urcu_ref_get_unless_zero(&VAR->FIELD_ref))
		return NULL;

	return VAR;
}

void STRUCT_put(struct STRUCT *VAR)
{
	if (!VAR)
		return;

	urcu_ref_put(&VAR->FIELD_ref, STRUCT_release);
}
