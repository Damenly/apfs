/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2015 Facebook.  All rights reserved.
 */

#ifndef APFS_FREE_SPACE_TREE_H
#define APFS_FREE_SPACE_TREE_H

struct apfs_caching_control;

/*
 * The default size for new free space bitmap items. The last bitmap in a block
 * group may be truncated, and none of the free space tree code assumes that
 * existing bitmaps are this size.
 */
#define APFS_FREE_SPACE_BITMAP_SIZE 256
#define APFS_FREE_SPACE_BITMAP_BITS (APFS_FREE_SPACE_BITMAP_SIZE * BITS_PER_BYTE)

void set_free_space_tree_thresholds(struct apfs_block_group *block_group);
int apfs_create_free_space_tree(struct apfs_fs_info *fs_info);
int apfs_clear_free_space_tree(struct apfs_fs_info *fs_info);
int load_free_space_tree(struct apfs_caching_control *caching_ctl);
int add_block_group_free_space(struct apfs_trans_handle *trans,
			       struct apfs_block_group *block_group);
int remove_block_group_free_space(struct apfs_trans_handle *trans,
				  struct apfs_block_group *block_group);
int add_to_free_space_tree(struct apfs_trans_handle *trans,
			   u64 start, u64 size);
int remove_from_free_space_tree(struct apfs_trans_handle *trans,
				u64 start, u64 size);

#ifdef CONFIG_APFS_FS_RUN_SANITY_TESTS
struct apfs_free_space_info *
search_free_space_info(struct apfs_trans_handle *trans,
		       struct apfs_block_group *block_group,
		       struct apfs_path *path, int cow);
int __add_to_free_space_tree(struct apfs_trans_handle *trans,
			     struct apfs_block_group *block_group,
			     struct apfs_path *path, u64 start, u64 size);
int __remove_from_free_space_tree(struct apfs_trans_handle *trans,
				  struct apfs_block_group *block_group,
				  struct apfs_path *path, u64 start, u64 size);
int convert_free_space_to_bitmaps(struct apfs_trans_handle *trans,
				  struct apfs_block_group *block_group,
				  struct apfs_path *path);
int convert_free_space_to_extents(struct apfs_trans_handle *trans,
				  struct apfs_block_group *block_group,
				  struct apfs_path *path);
int free_space_test_bit(struct apfs_block_group *block_group,
			struct apfs_path *path, u64 offset);
#endif

#endif
