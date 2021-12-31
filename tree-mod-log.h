// SPDX-License-Identifier: GPL-2.0

#ifndef APFS_TREE_MOD_LOG_H
#define APFS_TREE_MOD_LOG_H

#include "ctree.h"

/* Represents a tree mod log user. */
struct apfs_seq_list {
	struct list_head list;
	u64 seq;
};

#define APFS_SEQ_LIST_INIT(name) { .list = LIST_HEAD_INIT((name).list), .seq = 0 }
#define APFS_SEQ_LAST            ((u64)-1)

enum apfs_mod_log_op {
	APFS_MOD_LOG_KEY_REPLACE,
	APFS_MOD_LOG_KEY_ADD,
	APFS_MOD_LOG_KEY_REMOVE,
	APFS_MOD_LOG_KEY_REMOVE_WHILE_FREEING,
	APFS_MOD_LOG_KEY_REMOVE_WHILE_MOVING,
	APFS_MOD_LOG_MOVE_KEYS,
	APFS_MOD_LOG_ROOT_REPLACE,
};

u64 apfs_get_tree_mod_seq(struct apfs_fs_info *fs_info,
			   struct apfs_seq_list *elem);
void apfs_put_tree_mod_seq(struct apfs_fs_info *fs_info,
			    struct apfs_seq_list *elem);
int apfs_tree_mod_log_insert_root(struct extent_buffer *old_root,
				   struct extent_buffer *new_root,
				   bool log_removal);
int apfs_tree_mod_log_insert_key(struct extent_buffer *eb, int slot,
				  enum apfs_mod_log_op op, gfp_t flags);
int apfs_tree_mod_log_free_eb(struct extent_buffer *eb);
struct extent_buffer *apfs_tree_mod_log_rewind(struct apfs_fs_info *fs_info,
						struct apfs_path *path,
						struct extent_buffer *eb,
						u64 time_seq);
struct extent_buffer *apfs_get_old_root(struct apfs_root *root, u64 time_seq);
int apfs_old_root_level(struct apfs_root *root, u64 time_seq);
int apfs_tree_mod_log_eb_copy(struct extent_buffer *dst,
			       struct extent_buffer *src,
			       unsigned long dst_offset,
			       unsigned long src_offset,
			       int nr_items);
int apfs_tree_mod_log_insert_move(struct extent_buffer *eb,
				   int dst_slot, int src_slot,
				   int nr_items);
u64 apfs_tree_mod_log_lowest_seq(struct apfs_fs_info *fs_info);

#endif
