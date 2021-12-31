// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2008 Red Hat.  All rights reserved.
 */

#include "ctree.h"
#include "disk-io.h"

int apfs_insert_orphan_item(struct apfs_trans_handle *trans,
			     struct apfs_root *root, u64 offset)
{
	struct apfs_path *path;
	struct apfs_key key = {};
	int ret = 0;

	key.objectid = APFS_ORPHAN_OBJECTID;
	key.type = APFS_ORPHAN_ITEM_KEY;
	key.offset = offset;

	path = apfs_alloc_path();
	if (!path)
		return -ENOMEM;

	ret = apfs_insert_empty_item(trans, root, path, &key, 0);

	apfs_free_path(path);
	return ret;
}

int apfs_del_orphan_item(struct apfs_trans_handle *trans,
			  struct apfs_root *root, u64 offset)
{
	struct apfs_path *path;
	struct apfs_key key = {};
	int ret = 0;

	key.objectid = APFS_ORPHAN_OBJECTID;
	key.type = APFS_ORPHAN_ITEM_KEY;
	key.offset = offset;

	path = apfs_alloc_path();
	if (!path)
		return -ENOMEM;

	ret = apfs_search_slot(trans, root, &key, path, -1, 1);
	if (ret < 0)
		goto out;
	if (ret) { /* JDM: Really? */
		ret = -ENOENT;
		goto out;
	}

	ret = apfs_del_item(trans, root, path);

out:
	apfs_free_path(path);
	return ret;
}
