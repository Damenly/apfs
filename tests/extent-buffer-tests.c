// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2013 Fusion IO.  All rights reserved.
 */

#include <linux/slab.h>
#include "apfs-tests.h"
#include "../ctree.h"
#include "../extent_io.h"
#include "../disk-io.h"

static int test_apfs_split_item(u32 sectorsize, u32 nodesize)
{
	struct apfs_fs_info *fs_info;
	struct apfs_path *path = NULL;
	struct apfs_root *root = NULL;
	struct extent_buffer *eb;
	struct apfs_item *item;
	char *value = "mary had a little lamb";
	char *split1 = "mary had a little";
	char *split2 = " lamb";
	char *split3 = "mary";
	char *split4 = " had a little";
	char buf[32];
	struct apfs_key key;
	u32 value_len = strlen(value);
	int ret = 0;

	test_msg("running apfs_split_item tests");

	fs_info = apfs_alloc_dummy_fs_info(nodesize, sectorsize);
	if (!fs_info) {
		test_std_err(TEST_ALLOC_FS_INFO);
		return -ENOMEM;
	}

	root = apfs_alloc_dummy_root(fs_info);
	if (IS_ERR(root)) {
		test_std_err(TEST_ALLOC_ROOT);
		ret = PTR_ERR(root);
		goto out;
	}

	path = apfs_alloc_path();
	if (!path) {
		test_std_err(TEST_ALLOC_PATH);
		ret = -ENOMEM;
		goto out;
	}

	path->nodes[0] = eb = alloc_dummy_extent_buffer(fs_info, nodesize);
	if (!eb) {
		test_std_err(TEST_ALLOC_EXTENT_BUFFER);
		ret = -ENOMEM;
		goto out;
	}
	path->slots[0] = 0;

	key.objectid = 0;
	key.type = APFS_EXTENT_CSUM_KEY;
	key.offset = 0;

	setup_items_for_insert(root, path, &key, &value_len, 1);
	item = apfs_item_nr(0);
	write_extent_buffer(eb, value, apfs_item_ptr_offset(eb, 0),
			    value_len);

	key.offset = 3;

	/*
	 * Passing NULL trans here should be safe because we have plenty of
	 * space in this leaf to split the item without having to split the
	 * leaf.
	 */
	ret = apfs_split_item(NULL, root, path, &key, 17);
	if (ret) {
		test_err("split item failed %d", ret);
		goto out;
	}

	/*
	 * Read the first slot, it should have the original key and contain only
	 * 'mary had a little'
	 */
	apfs_item_key_to_cpu(eb, &key, 0);
	if (key.objectid != 0 || key.type != APFS_EXTENT_CSUM_KEY ||
	    key.offset != 0) {
		test_err("invalid key at slot 0");
		ret = -EINVAL;
		goto out;
	}

	item = apfs_item_nr(0);
	if (apfs_item_size(eb, item) != strlen(split1)) {
		test_err("invalid len in the first split");
		ret = -EINVAL;
		goto out;
	}

	read_extent_buffer(eb, buf, apfs_item_ptr_offset(eb, 0),
			   strlen(split1));
	if (memcmp(buf, split1, strlen(split1))) {
		test_err(
"data in the buffer doesn't match what it should in the first split have='%.*s' want '%s'",
			 (int)strlen(split1), buf, split1);
		ret = -EINVAL;
		goto out;
	}

	apfs_item_key_to_cpu(eb, &key, 1);
	if (key.objectid != 0 || key.type != APFS_EXTENT_CSUM_KEY ||
	    key.offset != 3) {
		test_err("invalid key at slot 1");
		ret = -EINVAL;
		goto out;
	}

	item = apfs_item_nr(1);
	if (apfs_item_size(eb, item) != strlen(split2)) {
		test_err("invalid len in the second split");
		ret = -EINVAL;
		goto out;
	}

	read_extent_buffer(eb, buf, apfs_item_ptr_offset(eb, 1),
			   strlen(split2));
	if (memcmp(buf, split2, strlen(split2))) {
		test_err(
	"data in the buffer doesn't match what it should in the second split");
		ret = -EINVAL;
		goto out;
	}

	key.offset = 1;
	/* Do it again so we test memmoving the other items in the leaf */
	ret = apfs_split_item(NULL, root, path, &key, 4);
	if (ret) {
		test_err("second split item failed %d", ret);
		goto out;
	}

	apfs_item_key_to_cpu(eb, &key, 0);
	if (key.objectid != 0 || key.type != APFS_EXTENT_CSUM_KEY ||
	    key.offset != 0) {
		test_err("invalid key at slot 0");
		ret = -EINVAL;
		goto out;
	}

	item = apfs_item_nr(0);
	if (apfs_item_size(eb, item) != strlen(split3)) {
		test_err("invalid len in the first split");
		ret = -EINVAL;
		goto out;
	}

	read_extent_buffer(eb, buf, apfs_item_ptr_offset(eb, 0),
			   strlen(split3));
	if (memcmp(buf, split3, strlen(split3))) {
		test_err(
	"data in the buffer doesn't match what it should in the third split");
		ret = -EINVAL;
		goto out;
	}

	apfs_item_key_to_cpu(eb, &key, 1);
	if (key.objectid != 0 || key.type != APFS_EXTENT_CSUM_KEY ||
	    key.offset != 1) {
		test_err("invalid key at slot 1");
		ret = -EINVAL;
		goto out;
	}

	item = apfs_item_nr(1);
	if (apfs_item_size(eb, item) != strlen(split4)) {
		test_err("invalid len in the second split");
		ret = -EINVAL;
		goto out;
	}

	read_extent_buffer(eb, buf, apfs_item_ptr_offset(eb, 1),
			   strlen(split4));
	if (memcmp(buf, split4, strlen(split4))) {
		test_err(
	"data in the buffer doesn't match what it should in the fourth split");
		ret = -EINVAL;
		goto out;
	}

	apfs_item_key_to_cpu(eb, &key, 2);
	if (key.objectid != 0 || key.type != APFS_EXTENT_CSUM_KEY ||
	    key.offset != 3) {
		test_err("invalid key at slot 2");
		ret = -EINVAL;
		goto out;
	}

	item = apfs_item_nr(2);
	if (apfs_item_size(eb, item) != strlen(split2)) {
		test_err("invalid len in the second split");
		ret = -EINVAL;
		goto out;
	}

	read_extent_buffer(eb, buf, apfs_item_ptr_offset(eb, 2),
			   strlen(split2));
	if (memcmp(buf, split2, strlen(split2))) {
		test_err(
	"data in the buffer doesn't match what it should in the last chunk");
		ret = -EINVAL;
		goto out;
	}
out:
	apfs_free_path(path);
	apfs_free_dummy_root(root);
	apfs_free_dummy_fs_info(fs_info);
	return ret;
}

int apfs_test_extent_buffer_operations(u32 sectorsize, u32 nodesize)
{
	test_msg("running extent buffer operation tests");
	return test_apfs_split_item(sectorsize, nodesize);
}
