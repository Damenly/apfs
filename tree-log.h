/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2008 Oracle.  All rights reserved.
 */

#ifndef APFS_TREE_LOG_H
#define APFS_TREE_LOG_H

#include "ctree.h"
#include "transaction.h"

/* return value for apfs_log_dentry_safe that means we don't need to log it at all */
#define APFS_NO_LOG_SYNC 256

struct apfs_log_ctx {
	int log_ret;
	int log_transid;
	bool log_new_dentries;
	bool logging_new_name;
	struct inode *inode;
	struct list_head list;
	/* Only used for fast fsyncs. */
	struct list_head ordered_extents;
};

static inline void apfs_init_log_ctx(struct apfs_log_ctx *ctx,
				      struct inode *inode)
{
	ctx->log_ret = 0;
	ctx->log_transid = 0;
	ctx->log_new_dentries = false;
	ctx->logging_new_name = false;
	ctx->inode = inode;
	INIT_LIST_HEAD(&ctx->list);
	INIT_LIST_HEAD(&ctx->ordered_extents);
}

static inline void apfs_release_log_ctx_extents(struct apfs_log_ctx *ctx)
{
	struct apfs_ordered_extent *ordered;
	struct apfs_ordered_extent *tmp;

	ASSERT(inode_is_locked(ctx->inode));

	list_for_each_entry_safe(ordered, tmp, &ctx->ordered_extents, log_list) {
		list_del_init(&ordered->log_list);
		apfs_put_ordered_extent(ordered);
	}
}

static inline void apfs_set_log_full_commit(struct apfs_trans_handle *trans)
{
	WRITE_ONCE(trans->fs_info->last_trans_log_full_commit, trans->transid);
}

static inline int apfs_need_log_full_commit(struct apfs_trans_handle *trans)
{
	return READ_ONCE(trans->fs_info->last_trans_log_full_commit) ==
		trans->transid;
}

int apfs_sync_log(struct apfs_trans_handle *trans,
		   struct apfs_root *root, struct apfs_log_ctx *ctx);
int apfs_free_log(struct apfs_trans_handle *trans, struct apfs_root *root);
int apfs_free_log_root_tree(struct apfs_trans_handle *trans,
			     struct apfs_fs_info *fs_info);
int apfs_recover_log_trees(struct apfs_root *tree_root);
int apfs_log_dentry_safe(struct apfs_trans_handle *trans,
			  struct dentry *dentry,
			  struct apfs_log_ctx *ctx);
int apfs_del_dir_entries_in_log(struct apfs_trans_handle *trans,
				 struct apfs_root *root,
				 const char *name, int name_len,
				 struct apfs_inode *dir, u64 index);
int apfs_del_inode_ref_in_log(struct apfs_trans_handle *trans,
			       struct apfs_root *root,
			       const char *name, int name_len,
			       struct apfs_inode *inode, u64 dirid);
void apfs_end_log_trans(struct apfs_root *root);
void apfs_pin_log_trans(struct apfs_root *root);
void apfs_record_unlink_dir(struct apfs_trans_handle *trans,
			     struct apfs_inode *dir, struct apfs_inode *inode,
			     int for_rename);
void apfs_record_snapshot_destroy(struct apfs_trans_handle *trans,
				   struct apfs_inode *dir);
void apfs_log_new_name(struct apfs_trans_handle *trans,
			struct apfs_inode *inode, struct apfs_inode *old_dir,
			struct dentry *parent);

#endif
