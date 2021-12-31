// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2007 Oracle.  All rights reserved.
 */

#include <linux/sched.h>
#include "ctree.h"
#include "disk-io.h"
#include "print-tree.h"
#include "transaction.h"
#include "locking.h"

/*
 * Defrag all the leaves in a given btree.
 * Read all the leaves and try to get key order to
 * better reflect disk order
 */

int apfs_defrag_leaves(struct apfs_trans_handle *trans,
			struct apfs_root *root)
{
	struct apfs_path *path = NULL;
	struct apfs_key key = {};
	int ret = 0;
	int wret;
	int level;
	int next_key_ret = 0;
	u64 last_ret = 0;

	if (root->fs_info->extent_root == root) {
		/*
		 * there's recursion here right now in the tree locking,
		 * we can't defrag the extent root without deadlock
		 */
		goto out;
	}

	if (!test_bit(APFS_ROOT_SHAREABLE, &root->state))
		goto out;

	path = apfs_alloc_path();
	if (!path)
		return -ENOMEM;

	level = apfs_header_level(root->node);

	if (level == 0)
		goto out;

	if (root->defrag_progress.objectid == 0) {
		struct extent_buffer *root_node;
		u32 nritems;

		root_node = apfs_lock_root_node(root);
		nritems = apfs_header_nritems(root_node);
		root->defrag_max.objectid = 0;
		/* from above we know this is not a leaf */
		apfs_node_key_to_cpu(root_node, &root->defrag_max,
				      nritems - 1);
		apfs_tree_unlock(root_node);
		free_extent_buffer(root_node);
		memset(&key, 0, sizeof(key));
	} else {
		memcpy(&key, &root->defrag_progress, sizeof(key));
	}

	path->keep_locks = 1;

	ret = apfs_search_forward(root, &key, path, APFS_OLDEST_GENERATION);
	if (ret < 0)
		goto out;
	if (ret > 0) {
		ret = 0;
		goto out;
	}
	apfs_release_path(path);
	/*
	 * We don't need a lock on a leaf. apfs_realloc_node() will lock all
	 * leafs from path->nodes[1], so set lowest_level to 1 to avoid later
	 * a deadlock (attempting to write lock an already write locked leaf).
	 */
	path->lowest_level = 1;
	wret = apfs_search_slot(trans, root, &key, path, 0, 1);

	if (wret < 0) {
		ret = wret;
		goto out;
	}
	if (!path->nodes[1]) {
		ret = 0;
		goto out;
	}
	/*
	 * The node at level 1 must always be locked when our path has
	 * keep_locks set and lowest_level is 1, regardless of the value of
	 * path->slots[1].
	 */
	BUG_ON(path->locks[1] == 0);
	ret = apfs_realloc_node(trans, root,
				 path->nodes[1], 0,
				 &last_ret,
				 &root->defrag_progress);
	if (ret) {
		WARN_ON(ret == -EAGAIN);
		goto out;
	}
	/*
	 * Now that we reallocated the node we can find the next key. Note that
	 * apfs_find_next_key() can release our path and do another search
	 * without COWing, this is because even with path->keep_locks = 1,
	 * apfs_search_slot() / ctree.c:unlock_up() does not keeps a lock on a
	 * node when path->slots[node_level - 1] does not point to the last
	 * item or a slot beyond the last item (ctree.c:unlock_up()). Therefore
	 * we search for the next key after reallocating our node.
	 */
	path->slots[1] = apfs_header_nritems(path->nodes[1]);
	next_key_ret = apfs_find_next_key(root, path, &key, 1,
					   APFS_OLDEST_GENERATION);
	if (next_key_ret == 0) {
		memcpy(&root->defrag_progress, &key, sizeof(key));
		ret = -EAGAIN;
	}
out:
	apfs_free_path(path);
	if (ret == -EAGAIN) {
		if (root->defrag_max.objectid > root->defrag_progress.objectid)
			goto done;
		if (root->defrag_max.type > root->defrag_progress.type)
			goto done;
		if (root->defrag_max.offset > root->defrag_progress.offset)
			goto done;
		ret = 0;
	}
done:
	if (ret != -EAGAIN)
		memset(&root->defrag_progress, 0,
		       sizeof(root->defrag_progress));

	return ret;
}
