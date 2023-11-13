/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#include "reffs/super_block.h"
#include "reffs/log.h"
#include <urcu.h>
#include <urcu/rculist.h>
#include <urcu/ref.h>

CDS_LIST_HEAD(super_block_list);

static void super_block_free_rcu(struct rcu_head *rcu)
{
	struct super_block *sb =
		caa_container_of(rcu, struct super_block, sb_rcu);

	free(sb);
}

static void super_block_release(struct urcu_ref *ref)
{
	struct super_block *sb =
		caa_container_of(ref, struct super_block, sb_ref);

	call_rcu(&sb->sb_rcu, super_block_free_rcu);
}

struct super_block *super_block_alloc(uint64_t id)
{
        struct super_block *sb;

        sb = calloc(1, sizeof(*sb));
        if (!sb) {
                LOG("Could not alloc a sb");
		return NULL;
	}

        sb->sb_id = id;
        cds_list_add_rcu(&sb->sb_link, &super_block_list);
        urcu_ref_init(&sb->sb_ref);

        return sb;
}

struct super_block *super_block_find(uint64_t id)
{
        struct super_block *sb = NULL;
        struct super_block *tmp;

        rcu_read_lock();
        cds_list_for_each_entry_rcu(tmp, &super_block_list, sb_link)
                if(id == tmp->sb_id) {
                        sb = super_block_get(tmp);
                        break;
                }
        rcu_read_unlock();

        return sb;
}

struct super_block *super_block_get(struct super_block *sb)
{
	if (!sb)
		return NULL;

	if (!urcu_ref_get_unless_zero(&sb->sb_ref))
		return NULL;

	return sb;
}

void super_block_put(struct super_block *sb)
{
	if (!sb)
		return;

	urcu_ref_put(&sb->sb_ref, super_block_release);
}
