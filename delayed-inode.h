/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2011 Fujitsu.  All rights reserved.
 * Written by Miao Xie <miaox@cn.fujitsu.com>
 */

#ifndef APFS_DELAYED_INODE_H
#define APFS_DELAYED_INODE_H

#include <linux/rbtree.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/wait.h>
#include <linux/atomic.h>
#include <linux/refcount.h>
#include "ctree.h"

/* types of the delayed item */
#define APFS_DELAYED_INSERTION_ITEM	1
#define APFS_DELAYED_DELETION_ITEM	2

struct apfs_delayed_root {
	spinlock_t lock;
	struct list_head node_list;
	/*
	 * Used for delayed nodes which is waiting to be dealt with by the
	 * worker. If the delayed node is inserted into the work queue, we
	 * drop it from this list.
	 */
	struct list_head prepare_list;
	atomic_t items;		/* for delayed items */
	atomic_t items_seq;	/* for delayed items */
	int nodes;		/* for delayed nodes */
	wait_queue_head_t wait;
};

#define APFS_DELAYED_NODE_IN_LIST	0
#define APFS_DELAYED_NODE_INODE_DIRTY	1
#define APFS_DELAYED_NODE_DEL_IREF	2

struct apfs_delayed_node {
	u64 inode_id;
	u64 bytes_reserved;
	struct apfs_root *root;
	/* Used to add the node into the delayed root's node list. */
	struct list_head n_list;
	/*
	 * Used to add the node into the prepare list, the nodes in this list
	 * is waiting to be dealt with by the async worker.
	 */
	struct list_head p_list;
	struct rb_root_cached ins_root;
	struct rb_root_cached del_root;
	struct mutex mutex;
	struct apfs_inode_item inode_item;
	refcount_t refs;
	u64 index_cnt;
	unsigned long flags;
	int count;
};

struct apfs_delayed_item {
	struct rb_node rb_node;
	struct apfs_key key;
	struct list_head tree_list;	/* used for batch insert/delete items */
	struct list_head readdir_list;	/* used for readdir items */
	u64 bytes_reserved;
	struct apfs_delayed_node *delayed_node;
	refcount_t refs;
	int ins_or_del;
	u32 data_len;
	char data[];
};

static inline void apfs_init_delayed_root(
				struct apfs_delayed_root *delayed_root)
{
	atomic_set(&delayed_root->items, 0);
	atomic_set(&delayed_root->items_seq, 0);
	delayed_root->nodes = 0;
	spin_lock_init(&delayed_root->lock);
	init_waitqueue_head(&delayed_root->wait);
	INIT_LIST_HEAD(&delayed_root->node_list);
	INIT_LIST_HEAD(&delayed_root->prepare_list);
}

int apfs_insert_delayed_dir_index(struct apfs_trans_handle *trans,
				   const char *name, int name_len,
				   struct apfs_inode *dir,
				   struct apfs_disk_key *disk_key, u8 type,
				   u64 index);

int apfs_delete_delayed_dir_index(struct apfs_trans_handle *trans,
				   struct apfs_inode *dir, u64 index);

int apfs_inode_delayed_dir_index_count(struct apfs_inode *inode);

int apfs_run_delayed_items(struct apfs_trans_handle *trans);
int apfs_run_delayed_items_nr(struct apfs_trans_handle *trans, int nr);

void apfs_balance_delayed_items(struct apfs_fs_info *fs_info);

int apfs_commit_inode_delayed_items(struct apfs_trans_handle *trans,
				     struct apfs_inode *inode);
/* Used for evicting the inode. */
void apfs_remove_delayed_node(struct apfs_inode *inode);
void apfs_kill_delayed_inode_items(struct apfs_inode *inode);
int apfs_commit_inode_delayed_inode(struct apfs_inode *inode);


int apfs_delayed_update_inode(struct apfs_trans_handle *trans,
			       struct apfs_root *root,
			       struct apfs_inode *inode);
int apfs_fill_inode(struct inode *inode, u32 *rdev);
int apfs_delayed_delete_inode_ref(struct apfs_inode *inode);

/* Used for drop dead root */
void apfs_kill_all_delayed_nodes(struct apfs_root *root);

/* Used for clean the transaction */
void apfs_destroy_delayed_inodes(struct apfs_fs_info *fs_info);

/* Used for readdir() */
bool apfs_readdir_get_delayed_items(struct inode *inode,
				     struct list_head *ins_list,
				     struct list_head *del_list);
void apfs_readdir_put_delayed_items(struct inode *inode,
				     struct list_head *ins_list,
				     struct list_head *del_list);
int apfs_should_delete_dir_index(struct list_head *del_list,
				  u64 index);
int apfs_readdir_delayed_dir_index(struct dir_context *ctx,
				    struct list_head *ins_list);

/* for init */
int __init apfs_delayed_inode_init(void);
void __cold apfs_delayed_inode_exit(void);

/* for debugging */
void apfs_assert_delayed_root_empty(struct apfs_fs_info *fs_info);

#endif
