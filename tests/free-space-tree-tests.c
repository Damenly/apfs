// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015 Facebook.  All rights reserved.
 */

#include <linux/types.h>
#include "apfs-tests.h"
#include "../ctree.h"
#include "../disk-io.h"
#include "../free-space-tree.h"
#include "../transaction.h"
#include "../block-group.h"

struct free_space_extent {
	u64 start;
	u64 length;
};

static int __check_free_space_extents(struct apfs_trans_handle *trans,
				      struct apfs_fs_info *fs_info,
				      struct apfs_block_group *cache,
				      struct apfs_path *path,
				      const struct free_space_extent * const extents,
				      unsigned int num_extents)
{
	struct apfs_free_space_info *info;
	struct apfs_key key;
	int prev_bit = 0, bit;
	u64 extent_start = 0, offset, end;
	u32 flags, extent_count;
	unsigned int i;
	int ret;

	info = search_free_space_info(trans, cache, path, 0);
	if (IS_ERR(info)) {
		test_err("could not find free space info");
		ret = PTR_ERR(info);
		goto out;
	}
	flags = apfs_free_space_flags(path->nodes[0], info);
	extent_count = apfs_free_space_extent_count(path->nodes[0], info);

	if (extent_count != num_extents) {
		test_err("extent count is wrong");
		ret = -EINVAL;
		goto out;
	}
	if (flags & APFS_FREE_SPACE_USING_BITMAPS) {
		if (path->slots[0] != 0)
			goto invalid;
		end = cache->start + cache->length;
		i = 0;
		while (++path->slots[0] < apfs_header_nritems(path->nodes[0])) {
			apfs_item_key_to_cpu(path->nodes[0], &key, path->slots[0]);
			if (key.type != APFS_FREE_SPACE_BITMAP_KEY)
				goto invalid;
			offset = key.objectid;
			while (offset < key.objectid + key.offset) {
				bit = free_space_test_bit(cache, path, offset);
				if (prev_bit == 0 && bit == 1) {
					extent_start = offset;
				} else if (prev_bit == 1 && bit == 0) {
					if (i >= num_extents ||
					    extent_start != extents[i].start ||
					    offset - extent_start != extents[i].length)
						goto invalid;
					i++;
				}
				prev_bit = bit;
				offset += fs_info->sectorsize;
			}
		}
		if (prev_bit == 1) {
			if (i >= num_extents ||
			    extent_start != extents[i].start ||
			    end - extent_start != extents[i].length)
				goto invalid;
			i++;
		}
		if (i != num_extents)
			goto invalid;
	} else {
		if (apfs_header_nritems(path->nodes[0]) != num_extents + 1 ||
		    path->slots[0] != 0)
			goto invalid;
		for (i = 0; i < num_extents; i++) {
			path->slots[0]++;
			apfs_item_key_to_cpu(path->nodes[0], &key, path->slots[0]);
			if (key.type != APFS_FREE_SPACE_EXTENT_KEY ||
			    key.objectid != extents[i].start ||
			    key.offset != extents[i].length)
				goto invalid;
		}
	}

	ret = 0;
out:
	apfs_release_path(path);
	return ret;
invalid:
	test_err("free space tree is invalid");
	ret = -EINVAL;
	goto out;
}

static int check_free_space_extents(struct apfs_trans_handle *trans,
				    struct apfs_fs_info *fs_info,
				    struct apfs_block_group *cache,
				    struct apfs_path *path,
				    const struct free_space_extent * const extents,
				    unsigned int num_extents)
{
	struct apfs_free_space_info *info;
	u32 flags;
	int ret;

	info = search_free_space_info(trans, cache, path, 0);
	if (IS_ERR(info)) {
		test_err("could not find free space info");
		apfs_release_path(path);
		return PTR_ERR(info);
	}
	flags = apfs_free_space_flags(path->nodes[0], info);
	apfs_release_path(path);

	ret = __check_free_space_extents(trans, fs_info, cache, path, extents,
					 num_extents);
	if (ret)
		return ret;

	/* Flip it to the other format and check that for good measure. */
	if (flags & APFS_FREE_SPACE_USING_BITMAPS) {
		ret = convert_free_space_to_extents(trans, cache, path);
		if (ret) {
			test_err("could not convert to extents");
			return ret;
		}
	} else {
		ret = convert_free_space_to_bitmaps(trans, cache, path);
		if (ret) {
			test_err("could not convert to bitmaps");
			return ret;
		}
	}
	return __check_free_space_extents(trans, fs_info, cache, path, extents,
					  num_extents);
}

static int test_empty_block_group(struct apfs_trans_handle *trans,
				  struct apfs_fs_info *fs_info,
				  struct apfs_block_group *cache,
				  struct apfs_path *path,
				  u32 alignment)
{
	const struct free_space_extent extents[] = {
		{cache->start, cache->length},
	};

	return check_free_space_extents(trans, fs_info, cache, path,
					extents, ARRAY_SIZE(extents));
}

static int test_remove_all(struct apfs_trans_handle *trans,
			   struct apfs_fs_info *fs_info,
			   struct apfs_block_group *cache,
			   struct apfs_path *path,
			   u32 alignment)
{
	const struct free_space_extent extents[] = {};
	int ret;

	ret = __remove_from_free_space_tree(trans, cache, path,
					    cache->start,
					    cache->length);
	if (ret) {
		test_err("could not remove free space");
		return ret;
	}

	return check_free_space_extents(trans, fs_info, cache, path,
					extents, ARRAY_SIZE(extents));
}

static int test_remove_beginning(struct apfs_trans_handle *trans,
				 struct apfs_fs_info *fs_info,
				 struct apfs_block_group *cache,
				 struct apfs_path *path,
				 u32 alignment)
{
	const struct free_space_extent extents[] = {
		{cache->start + alignment, cache->length - alignment},
	};
	int ret;

	ret = __remove_from_free_space_tree(trans, cache, path,
					    cache->start, alignment);
	if (ret) {
		test_err("could not remove free space");
		return ret;
	}

	return check_free_space_extents(trans, fs_info, cache, path,
					extents, ARRAY_SIZE(extents));

}

static int test_remove_end(struct apfs_trans_handle *trans,
			   struct apfs_fs_info *fs_info,
			   struct apfs_block_group *cache,
			   struct apfs_path *path,
			   u32 alignment)
{
	const struct free_space_extent extents[] = {
		{cache->start, cache->length - alignment},
	};
	int ret;

	ret = __remove_from_free_space_tree(trans, cache, path,
				    cache->start + cache->length - alignment,
				    alignment);
	if (ret) {
		test_err("could not remove free space");
		return ret;
	}

	return check_free_space_extents(trans, fs_info, cache, path,
					extents, ARRAY_SIZE(extents));
}

static int test_remove_middle(struct apfs_trans_handle *trans,
			      struct apfs_fs_info *fs_info,
			      struct apfs_block_group *cache,
			      struct apfs_path *path,
			      u32 alignment)
{
	const struct free_space_extent extents[] = {
		{cache->start, alignment},
		{cache->start + 2 * alignment, cache->length - 2 * alignment},
	};
	int ret;

	ret = __remove_from_free_space_tree(trans, cache, path,
					    cache->start + alignment,
					    alignment);
	if (ret) {
		test_err("could not remove free space");
		return ret;
	}

	return check_free_space_extents(trans, fs_info, cache, path,
					extents, ARRAY_SIZE(extents));
}

static int test_merge_left(struct apfs_trans_handle *trans,
			   struct apfs_fs_info *fs_info,
			   struct apfs_block_group *cache,
			   struct apfs_path *path,
			   u32 alignment)
{
	const struct free_space_extent extents[] = {
		{cache->start, 2 * alignment},
	};
	int ret;

	ret = __remove_from_free_space_tree(trans, cache, path,
					    cache->start, cache->length);
	if (ret) {
		test_err("could not remove free space");
		return ret;
	}

	ret = __add_to_free_space_tree(trans, cache, path, cache->start,
				       alignment);
	if (ret) {
		test_err("could not add free space");
		return ret;
	}

	ret = __add_to_free_space_tree(trans, cache, path,
				       cache->start + alignment,
				       alignment);
	if (ret) {
		test_err("could not add free space");
		return ret;
	}

	return check_free_space_extents(trans, fs_info, cache, path,
					extents, ARRAY_SIZE(extents));
}

static int test_merge_right(struct apfs_trans_handle *trans,
			   struct apfs_fs_info *fs_info,
			   struct apfs_block_group *cache,
			   struct apfs_path *path,
			   u32 alignment)
{
	const struct free_space_extent extents[] = {
		{cache->start + alignment, 2 * alignment},
	};
	int ret;

	ret = __remove_from_free_space_tree(trans, cache, path,
					    cache->start, cache->length);
	if (ret) {
		test_err("could not remove free space");
		return ret;
	}

	ret = __add_to_free_space_tree(trans, cache, path,
				       cache->start + 2 * alignment,
				       alignment);
	if (ret) {
		test_err("could not add free space");
		return ret;
	}

	ret = __add_to_free_space_tree(trans, cache, path,
				       cache->start + alignment,
				       alignment);
	if (ret) {
		test_err("could not add free space");
		return ret;
	}

	return check_free_space_extents(trans, fs_info, cache, path,
					extents, ARRAY_SIZE(extents));
}

static int test_merge_both(struct apfs_trans_handle *trans,
			   struct apfs_fs_info *fs_info,
			   struct apfs_block_group *cache,
			   struct apfs_path *path,
			   u32 alignment)
{
	const struct free_space_extent extents[] = {
		{cache->start, 3 * alignment},
	};
	int ret;

	ret = __remove_from_free_space_tree(trans, cache, path,
					    cache->start, cache->length);
	if (ret) {
		test_err("could not remove free space");
		return ret;
	}

	ret = __add_to_free_space_tree(trans, cache, path, cache->start,
				       alignment);
	if (ret) {
		test_err("could not add free space");
		return ret;
	}

	ret = __add_to_free_space_tree(trans, cache, path,
				       cache->start + 2 * alignment, alignment);
	if (ret) {
		test_err("could not add free space");
		return ret;
	}

	ret = __add_to_free_space_tree(trans, cache, path,
				       cache->start + alignment, alignment);
	if (ret) {
		test_err("could not add free space");
		return ret;
	}

	return check_free_space_extents(trans, fs_info, cache, path,
					extents, ARRAY_SIZE(extents));
}

static int test_merge_none(struct apfs_trans_handle *trans,
			   struct apfs_fs_info *fs_info,
			   struct apfs_block_group *cache,
			   struct apfs_path *path,
			   u32 alignment)
{
	const struct free_space_extent extents[] = {
		{cache->start, alignment},
		{cache->start + 2 * alignment, alignment},
		{cache->start + 4 * alignment, alignment},
	};
	int ret;

	ret = __remove_from_free_space_tree(trans, cache, path,
					    cache->start, cache->length);
	if (ret) {
		test_err("could not remove free space");
		return ret;
	}

	ret = __add_to_free_space_tree(trans, cache, path, cache->start,
				       alignment);
	if (ret) {
		test_err("could not add free space");
		return ret;
	}

	ret = __add_to_free_space_tree(trans, cache, path,
				       cache->start + 4 * alignment, alignment);
	if (ret) {
		test_err("could not add free space");
		return ret;
	}

	ret = __add_to_free_space_tree(trans, cache, path,
				       cache->start + 2 * alignment, alignment);
	if (ret) {
		test_err("could not add free space");
		return ret;
	}

	return check_free_space_extents(trans, fs_info, cache, path,
					extents, ARRAY_SIZE(extents));
}

typedef int (*test_func_t)(struct apfs_trans_handle *,
			   struct apfs_fs_info *,
			   struct apfs_block_group *,
			   struct apfs_path *,
			   u32 alignment);

static int run_test(test_func_t test_func, int bitmaps, u32 sectorsize,
		    u32 nodesize, u32 alignment)
{
	struct apfs_fs_info *fs_info;
	struct apfs_root *root = NULL;
	struct apfs_block_group *cache = NULL;
	struct apfs_trans_handle trans;
	struct apfs_path *path = NULL;
	int ret;

	fs_info = apfs_alloc_dummy_fs_info(nodesize, sectorsize);
	if (!fs_info) {
		test_std_err(TEST_ALLOC_FS_INFO);
		ret = -ENOMEM;
		goto out;
	}

	root = apfs_alloc_dummy_root(fs_info);
	if (IS_ERR(root)) {
		test_std_err(TEST_ALLOC_ROOT);
		ret = PTR_ERR(root);
		goto out;
	}

	apfs_set_super_compat_ro_flags(root->fs_info->super_copy,
					APFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE);
	root->fs_info->free_space_root = root;
	root->fs_info->tree_root = root;

	root->node = alloc_test_extent_buffer(root->fs_info, nodesize);
	if (IS_ERR(root->node)) {
		test_std_err(TEST_ALLOC_EXTENT_BUFFER);
		ret = PTR_ERR(root->node);
		goto out;
	}
	apfs_set_header_level(root->node, 0);
	apfs_set_header_nritems(root->node, 0);
	root->alloc_bytenr += 2 * nodesize;

	cache = apfs_alloc_dummy_block_group(fs_info, 8 * alignment);
	if (!cache) {
		test_std_err(TEST_ALLOC_BLOCK_GROUP);
		ret = -ENOMEM;
		goto out;
	}
	cache->bitmap_low_thresh = 0;
	cache->bitmap_high_thresh = (u32)-1;
	cache->needs_free_space = 1;
	cache->fs_info = root->fs_info;

	apfs_init_dummy_trans(&trans, root->fs_info);

	path = apfs_alloc_path();
	if (!path) {
		test_std_err(TEST_ALLOC_ROOT);
		ret = -ENOMEM;
		goto out;
	}

	ret = add_block_group_free_space(&trans, cache);
	if (ret) {
		test_err("could not add block group free space");
		goto out;
	}

	if (bitmaps) {
		ret = convert_free_space_to_bitmaps(&trans, cache, path);
		if (ret) {
			test_err("could not convert block group to bitmaps");
			goto out;
		}
	}

	ret = test_func(&trans, root->fs_info, cache, path, alignment);
	if (ret)
		goto out;

	ret = remove_block_group_free_space(&trans, cache);
	if (ret) {
		test_err("could not remove block group free space");
		goto out;
	}

	if (apfs_header_nritems(root->node) != 0) {
		test_err("free space tree has leftover items");
		ret = -EINVAL;
		goto out;
	}

	ret = 0;
out:
	apfs_free_path(path);
	apfs_free_dummy_block_group(cache);
	apfs_free_dummy_root(root);
	apfs_free_dummy_fs_info(fs_info);
	return ret;
}

static int run_test_both_formats(test_func_t test_func, u32 sectorsize,
				 u32 nodesize, u32 alignment)
{
	int test_ret = 0;
	int ret;

	ret = run_test(test_func, 0, sectorsize, nodesize, alignment);
	if (ret) {
		test_err(
	"%ps failed with extents, sectorsize=%u, nodesize=%u, alignment=%u",
			 test_func, sectorsize, nodesize, alignment);
		test_ret = ret;
	}

	ret = run_test(test_func, 1, sectorsize, nodesize, alignment);
	if (ret) {
		test_err(
	"%ps failed with bitmaps, sectorsize=%u, nodesize=%u, alignment=%u",
			 test_func, sectorsize, nodesize, alignment);
		test_ret = ret;
	}

	return test_ret;
}

int apfs_test_free_space_tree(u32 sectorsize, u32 nodesize)
{
	test_func_t tests[] = {
		test_empty_block_group,
		test_remove_all,
		test_remove_beginning,
		test_remove_end,
		test_remove_middle,
		test_merge_left,
		test_merge_right,
		test_merge_both,
		test_merge_none,
	};
	u32 bitmap_alignment;
	int test_ret = 0;
	int i;

	/*
	 * Align some operations to a page to flush out bugs in the extent
	 * buffer bitmap handling of highmem.
	 */
	bitmap_alignment = APFS_FREE_SPACE_BITMAP_BITS * PAGE_SIZE;

	test_msg("running free space tree tests");
	for (i = 0; i < ARRAY_SIZE(tests); i++) {
		int ret;

		ret = run_test_both_formats(tests[i], sectorsize, nodesize,
					    sectorsize);
		if (ret)
			test_ret = ret;

		ret = run_test_both_formats(tests[i], sectorsize, nodesize,
					    bitmap_alignment);
		if (ret)
			test_ret = ret;
	}

	return test_ret;
}
