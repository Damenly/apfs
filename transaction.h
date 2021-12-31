/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2007 Oracle.  All rights reserved.
 */

#ifndef APFS_TRANSACTION_H
#define APFS_TRANSACTION_H

#include <linux/refcount.h>
#include "apfs_inode.h"
#include "delayed-ref.h"
#include "ctree.h"

enum apfs_trans_state {
	TRANS_STATE_RUNNING,
	TRANS_STATE_COMMIT_START,
	TRANS_STATE_COMMIT_DOING,
	TRANS_STATE_UNBLOCKED,
	TRANS_STATE_SUPER_COMMITTED,
	TRANS_STATE_COMPLETED,
	TRANS_STATE_MAX,
};

#define APFS_TRANS_HAVE_FREE_BGS	0
#define APFS_TRANS_DIRTY_BG_RUN	1
#define APFS_TRANS_CACHE_ENOSPC	2

struct apfs_transaction {
	u64 transid;
	/*
	 * total external writers(USERSPACE/START/ATTACH) in this
	 * transaction, it must be zero before the transaction is
	 * being committed
	 */
	atomic_t num_extwriters;
	/*
	 * total writers in this transaction, it must be zero before the
	 * transaction can end
	 */
	atomic_t num_writers;
	refcount_t use_count;

	unsigned long flags;

	/* Be protected by fs_info->trans_lock when we want to change it. */
	enum apfs_trans_state state;
	int aborted;
	struct list_head list;
	struct extent_io_tree dirty_pages;
	time64_t start_time;
	wait_queue_head_t writer_wait;
	wait_queue_head_t commit_wait;
	struct list_head pending_snapshots;
	struct list_head dev_update_list;
	struct list_head switch_commits;
	struct list_head dirty_bgs;

	/*
	 * There is no explicit lock which protects io_bgs, rather its
	 * consistency is implied by the fact that all the sites which modify
	 * it do so under some form of transaction critical section, namely:
	 *
	 * - apfs_start_dirty_block_groups - This function can only ever be
	 *   run by one of the transaction committers. Refer to
	 *   APFS_TRANS_DIRTY_BG_RUN usage in apfs_commit_transaction
	 *
	 * - apfs_write_dirty_blockgroups - this is called by
	 *   commit_cowonly_roots from transaction critical section
	 *   (TRANS_STATE_COMMIT_DOING)
	 *
	 * - apfs_cleanup_dirty_bgs - called on transaction abort
	 */
	struct list_head io_bgs;
	struct list_head dropped_roots;
	struct extent_io_tree pinned_extents;

	/*
	 * we need to make sure block group deletion doesn't race with
	 * free space cache writeout.  This mutex keeps them from stomping
	 * on each other
	 */
	struct mutex cache_write_mutex;
	spinlock_t dirty_bgs_lock;
	/* Protected by spin lock fs_info->unused_bgs_lock. */
	struct list_head deleted_bgs;
	spinlock_t dropped_roots_lock;
	struct apfs_delayed_ref_root delayed_refs;
	struct apfs_fs_info *fs_info;

	/*
	 * Number of ordered extents the transaction must wait for before
	 * committing. These are ordered extents started by a fast fsync.
	 */
	atomic_t pending_ordered;
	wait_queue_head_t pending_wait;

	spinlock_t releasing_ebs_lock;
	struct list_head releasing_ebs;
};

#define __TRANS_FREEZABLE	(1U << 0)

#define __TRANS_START		(1U << 9)
#define __TRANS_ATTACH		(1U << 10)
#define __TRANS_JOIN		(1U << 11)
#define __TRANS_JOIN_NOLOCK	(1U << 12)
#define __TRANS_DUMMY		(1U << 13)
#define __TRANS_JOIN_NOSTART	(1U << 14)

#define TRANS_START		(__TRANS_START | __TRANS_FREEZABLE)
#define TRANS_ATTACH		(__TRANS_ATTACH)
#define TRANS_JOIN		(__TRANS_JOIN | __TRANS_FREEZABLE)
#define TRANS_JOIN_NOLOCK	(__TRANS_JOIN_NOLOCK)
#define TRANS_JOIN_NOSTART	(__TRANS_JOIN_NOSTART)

#define TRANS_EXTWRITERS	(__TRANS_START | __TRANS_ATTACH)

struct apfs_trans_handle {
	u64 transid;
	u64 bytes_reserved;
	u64 chunk_bytes_reserved;
	unsigned long delayed_ref_updates;
	struct apfs_transaction *transaction;
	struct apfs_block_rsv *block_rsv;
	struct apfs_block_rsv *orig_rsv;
	refcount_t use_count;
	unsigned int type;
	/*
	 * Error code of transaction abort, set outside of locks and must use
	 * the READ_ONCE/WRITE_ONCE access
	 */
	short aborted;
	bool adding_csums;
	bool allocating_chunk;
	bool removing_chunk;
	bool reloc_reserved;
	bool in_fsync;
	struct apfs_root *root;
	struct apfs_fs_info *fs_info;
	struct list_head new_bgs;
};

/*
 * The abort status can be changed between calls and is not protected by locks.
 * This accepts apfs_transaction and apfs_trans_handle as types. Once it's
 * set to a non-zero value it does not change, so the macro should be in checks
 * but is not necessary for further reads of the value.
 */
#define TRANS_ABORTED(trans)		(unlikely(READ_ONCE((trans)->aborted)))

struct apfs_pending_snapshot {
	struct dentry *dentry;
	struct inode *dir;
	struct apfs_root *root;
	struct apfs_root_item *root_item;
	struct apfs_root *snap;
	struct apfs_qgroup_inherit *inherit;
	struct apfs_path *path;
	/* block reservation for the operation */
	struct apfs_block_rsv block_rsv;
	/* extra metadata reservation for relocation */
	int error;
	/* Preallocated anonymous block device number */
	dev_t anon_dev;
	bool readonly;
	struct list_head list;
};

static inline void apfs_set_inode_last_trans(struct apfs_trans_handle *trans,
					      struct apfs_inode *inode)
{
	spin_lock(&inode->lock);
	inode->last_trans = trans->transaction->transid;
	inode->last_sub_trans = inode->root->log_transid;
	inode->last_log_commit = inode->last_sub_trans - 1;
	spin_unlock(&inode->lock);
}

/*
 * Make qgroup codes to skip given qgroupid, means the old/new_roots for
 * qgroup won't contain the qgroupid in it.
 */
static inline void apfs_set_skip_qgroup(struct apfs_trans_handle *trans,
					 u64 qgroupid)
{
	struct apfs_delayed_ref_root *delayed_refs;

	delayed_refs = &trans->transaction->delayed_refs;
	WARN_ON(delayed_refs->qgroup_to_skip);
	delayed_refs->qgroup_to_skip = qgroupid;
}

static inline void apfs_clear_skip_qgroup(struct apfs_trans_handle *trans)
{
	struct apfs_delayed_ref_root *delayed_refs;

	delayed_refs = &trans->transaction->delayed_refs;
	WARN_ON(!delayed_refs->qgroup_to_skip);
	delayed_refs->qgroup_to_skip = 0;
}

int apfs_end_transaction(struct apfs_trans_handle *trans);
struct apfs_trans_handle *apfs_start_transaction(struct apfs_root *root,
						   unsigned int num_items);
struct apfs_trans_handle *apfs_start_transaction_fallback_global_rsv(
					struct apfs_root *root,
					unsigned int num_items);
struct apfs_trans_handle *apfs_join_transaction(struct apfs_root *root);
struct apfs_trans_handle *apfs_join_transaction_spacecache(struct apfs_root *root);
struct apfs_trans_handle *apfs_join_transaction_nostart(struct apfs_root *root);
struct apfs_trans_handle *apfs_attach_transaction(struct apfs_root *root);
struct apfs_trans_handle *apfs_attach_transaction_barrier(
					struct apfs_root *root);
int apfs_wait_for_commit(struct apfs_fs_info *fs_info, u64 transid);

void apfs_add_dead_root(struct apfs_root *root);
int apfs_defrag_root(struct apfs_root *root);
int apfs_clean_one_deleted_snapshot(struct apfs_root *root);
int apfs_commit_transaction(struct apfs_trans_handle *trans);
int apfs_commit_transaction_async(struct apfs_trans_handle *trans);
int apfs_end_transaction_throttle(struct apfs_trans_handle *trans);
bool apfs_should_end_transaction(struct apfs_trans_handle *trans);
void apfs_throttle(struct apfs_fs_info *fs_info);
int apfs_record_root_in_trans(struct apfs_trans_handle *trans,
				struct apfs_root *root);
int apfs_write_marked_extents(struct apfs_fs_info *fs_info,
				struct extent_io_tree *dirty_pages, int mark);
int apfs_wait_tree_log_extents(struct apfs_root *root, int mark);
int apfs_transaction_blocked(struct apfs_fs_info *info);
int apfs_transaction_in_commit(struct apfs_fs_info *info);
void apfs_put_transaction(struct apfs_transaction *transaction);
void apfs_apply_pending_changes(struct apfs_fs_info *fs_info);
void apfs_add_dropped_root(struct apfs_trans_handle *trans,
			    struct apfs_root *root);
void apfs_trans_release_chunk_metadata(struct apfs_trans_handle *trans);

#endif
