// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2011 Fujitsu.  All rights reserved.
 * Written by Miao Xie <miaox@cn.fujitsu.com>
 */

#include <linux/slab.h>
#include <linux/iversion.h>
#include <linux/sched/mm.h>
#include "misc.h"
#include "delayed-inode.h"
#include "disk-io.h"
#include "transaction.h"
#include "ctree.h"
#include "qgroup.h"
#include "locking.h"

#define APFS_DELAYED_WRITEBACK		512
#define APFS_DELAYED_BACKGROUND	128
#define APFS_DELAYED_BATCH		16

static struct kmem_cache *delayed_node_cache;

int __init apfs_delayed_inode_init(void)
{
	delayed_node_cache = kmem_cache_create("apfs_delayed_node",
					sizeof(struct apfs_delayed_node),
					0,
					SLAB_MEM_SPREAD,
					NULL);
	if (!delayed_node_cache)
		return -ENOMEM;
	return 0;
}

void __cold apfs_delayed_inode_exit(void)
{
	kmem_cache_destroy(delayed_node_cache);
}

static inline void apfs_init_delayed_node(
				struct apfs_delayed_node *delayed_node,
				struct apfs_root *root, u64 inode_id)
{
	delayed_node->root = root;
	delayed_node->inode_id = inode_id;
	refcount_set(&delayed_node->refs, 0);
	delayed_node->ins_root = RB_ROOT_CACHED;
	delayed_node->del_root = RB_ROOT_CACHED;
	mutex_init(&delayed_node->mutex);
	INIT_LIST_HEAD(&delayed_node->n_list);
	INIT_LIST_HEAD(&delayed_node->p_list);
}

static inline int apfs_is_continuous_delayed_item(
					struct apfs_delayed_item *item1,
					struct apfs_delayed_item *item2)
{
	if (item1->key.type == APFS_DIR_INDEX_KEY &&
	    item1->key.objectid == item2->key.objectid &&
	    item1->key.type == item2->key.type &&
	    item1->key.offset + 1 == item2->key.offset)
		return 1;
	return 0;
}

static struct apfs_delayed_node *apfs_get_delayed_node(
		struct apfs_inode *apfs_inode)
{
	struct apfs_root *root = apfs_inode->root;
	u64 ino = apfs_ino(apfs_inode);
	struct apfs_delayed_node *node;

	node = READ_ONCE(apfs_inode->delayed_node);
	if (node) {
		refcount_inc(&node->refs);
		return node;
	}

	spin_lock(&root->inode_lock);
	node = radix_tree_lookup(&root->delayed_nodes_tree, ino);

	if (node) {
		if (apfs_inode->delayed_node) {
			refcount_inc(&node->refs);	/* can be accessed */
			BUG_ON(apfs_inode->delayed_node != node);
			spin_unlock(&root->inode_lock);
			return node;
		}

		/*
		 * It's possible that we're racing into the middle of removing
		 * this node from the radix tree.  In this case, the refcount
		 * was zero and it should never go back to one.  Just return
		 * NULL like it was never in the radix at all; our release
		 * function is in the process of removing it.
		 *
		 * Some implementations of refcount_inc refuse to bump the
		 * refcount once it has hit zero.  If we don't do this dance
		 * here, refcount_inc() may decide to just WARN_ONCE() instead
		 * of actually bumping the refcount.
		 *
		 * If this node is properly in the radix, we want to bump the
		 * refcount twice, once for the inode and once for this get
		 * operation.
		 */
		if (refcount_inc_not_zero(&node->refs)) {
			refcount_inc(&node->refs);
			apfs_inode->delayed_node = node;
		} else {
			node = NULL;
		}

		spin_unlock(&root->inode_lock);
		return node;
	}
	spin_unlock(&root->inode_lock);

	return NULL;
}

/* Will return either the node or PTR_ERR(-ENOMEM) */
static struct apfs_delayed_node *apfs_get_or_create_delayed_node(
		struct apfs_inode *apfs_inode)
{
	struct apfs_delayed_node *node;
	struct apfs_root *root = apfs_inode->root;
	u64 ino = apfs_ino(apfs_inode);
	int ret;

again:
	node = apfs_get_delayed_node(apfs_inode);
	if (node)
		return node;

	node = kmem_cache_zalloc(delayed_node_cache, GFP_NOFS);
	if (!node)
		return ERR_PTR(-ENOMEM);
	apfs_init_delayed_node(node, root, ino);

	/* cached in the apfs inode and can be accessed */
	refcount_set(&node->refs, 2);

	ret = radix_tree_preload(GFP_NOFS);
	if (ret) {
		kmem_cache_free(delayed_node_cache, node);
		return ERR_PTR(ret);
	}

	spin_lock(&root->inode_lock);
	ret = radix_tree_insert(&root->delayed_nodes_tree, ino, node);
	if (ret == -EEXIST) {
		spin_unlock(&root->inode_lock);
		kmem_cache_free(delayed_node_cache, node);
		radix_tree_preload_end();
		goto again;
	}
	apfs_inode->delayed_node = node;
	spin_unlock(&root->inode_lock);
	radix_tree_preload_end();

	return node;
}

/*
 * Call it when holding delayed_node->mutex
 *
 * If mod = 1, add this node into the prepared list.
 */
static void apfs_queue_delayed_node(struct apfs_delayed_root *root,
				     struct apfs_delayed_node *node,
				     int mod)
{
	spin_lock(&root->lock);
	if (test_bit(APFS_DELAYED_NODE_IN_LIST, &node->flags)) {
		if (!list_empty(&node->p_list))
			list_move_tail(&node->p_list, &root->prepare_list);
		else if (mod)
			list_add_tail(&node->p_list, &root->prepare_list);
	} else {
		list_add_tail(&node->n_list, &root->node_list);
		list_add_tail(&node->p_list, &root->prepare_list);
		refcount_inc(&node->refs);	/* inserted into list */
		root->nodes++;
		set_bit(APFS_DELAYED_NODE_IN_LIST, &node->flags);
	}
	spin_unlock(&root->lock);
}

/* Call it when holding delayed_node->mutex */
static void apfs_dequeue_delayed_node(struct apfs_delayed_root *root,
				       struct apfs_delayed_node *node)
{
	spin_lock(&root->lock);
	if (test_bit(APFS_DELAYED_NODE_IN_LIST, &node->flags)) {
		root->nodes--;
		refcount_dec(&node->refs);	/* not in the list */
		list_del_init(&node->n_list);
		if (!list_empty(&node->p_list))
			list_del_init(&node->p_list);
		clear_bit(APFS_DELAYED_NODE_IN_LIST, &node->flags);
	}
	spin_unlock(&root->lock);
}

static struct apfs_delayed_node *apfs_first_delayed_node(
			struct apfs_delayed_root *delayed_root)
{
	struct list_head *p;
	struct apfs_delayed_node *node = NULL;

	spin_lock(&delayed_root->lock);
	if (list_empty(&delayed_root->node_list))
		goto out;

	p = delayed_root->node_list.next;
	node = list_entry(p, struct apfs_delayed_node, n_list);
	refcount_inc(&node->refs);
out:
	spin_unlock(&delayed_root->lock);

	return node;
}

static struct apfs_delayed_node *apfs_next_delayed_node(
						struct apfs_delayed_node *node)
{
	struct apfs_delayed_root *delayed_root;
	struct list_head *p;
	struct apfs_delayed_node *next = NULL;

	delayed_root = node->root->fs_info->delayed_root;
	spin_lock(&delayed_root->lock);
	if (!test_bit(APFS_DELAYED_NODE_IN_LIST, &node->flags)) {
		/* not in the list */
		if (list_empty(&delayed_root->node_list))
			goto out;
		p = delayed_root->node_list.next;
	} else if (list_is_last(&node->n_list, &delayed_root->node_list))
		goto out;
	else
		p = node->n_list.next;

	next = list_entry(p, struct apfs_delayed_node, n_list);
	refcount_inc(&next->refs);
out:
	spin_unlock(&delayed_root->lock);

	return next;
}

static void __apfs_release_delayed_node(
				struct apfs_delayed_node *delayed_node,
				int mod)
{
	struct apfs_delayed_root *delayed_root;

	if (!delayed_node)
		return;

	delayed_root = delayed_node->root->fs_info->delayed_root;

	mutex_lock(&delayed_node->mutex);
	if (delayed_node->count)
		apfs_queue_delayed_node(delayed_root, delayed_node, mod);
	else
		apfs_dequeue_delayed_node(delayed_root, delayed_node);
	mutex_unlock(&delayed_node->mutex);

	if (refcount_dec_and_test(&delayed_node->refs)) {
		struct apfs_root *root = delayed_node->root;

		spin_lock(&root->inode_lock);
		/*
		 * Once our refcount goes to zero, nobody is allowed to bump it
		 * back up.  We can delete it now.
		 */
		ASSERT(refcount_read(&delayed_node->refs) == 0);
		radix_tree_delete(&root->delayed_nodes_tree,
				  delayed_node->inode_id);
		spin_unlock(&root->inode_lock);
		kmem_cache_free(delayed_node_cache, delayed_node);
	}
}

static inline void apfs_release_delayed_node(struct apfs_delayed_node *node)
{
	__apfs_release_delayed_node(node, 0);
}

static struct apfs_delayed_node *apfs_first_prepared_delayed_node(
					struct apfs_delayed_root *delayed_root)
{
	struct list_head *p;
	struct apfs_delayed_node *node = NULL;

	spin_lock(&delayed_root->lock);
	if (list_empty(&delayed_root->prepare_list))
		goto out;

	p = delayed_root->prepare_list.next;
	list_del_init(p);
	node = list_entry(p, struct apfs_delayed_node, p_list);
	refcount_inc(&node->refs);
out:
	spin_unlock(&delayed_root->lock);

	return node;
}

static inline void apfs_release_prepared_delayed_node(
					struct apfs_delayed_node *node)
{
	__apfs_release_delayed_node(node, 1);
}

static struct apfs_delayed_item *apfs_alloc_delayed_item(u32 data_len)
{
	struct apfs_delayed_item *item;
	item = kmalloc(sizeof(*item) + data_len, GFP_NOFS);
	if (item) {
		item->data_len = data_len;
		item->ins_or_del = 0;
		item->bytes_reserved = 0;
		item->delayed_node = NULL;
		refcount_set(&item->refs, 1);
	}
	return item;
}

/*
 * __apfs_lookup_delayed_item - look up the delayed item by key
 * @delayed_node: pointer to the delayed node
 * @key:	  the key to look up
 * @prev:	  used to store the prev item if the right item isn't found
 * @next:	  used to store the next item if the right item isn't found
 *
 * Note: if we don't find the right item, we will return the prev item and
 * the next item.
 */
static struct apfs_delayed_item *__apfs_lookup_delayed_item(
				struct rb_root *root,
				struct apfs_key *key,
				struct apfs_delayed_item **prev,
				struct apfs_delayed_item **next)
{
	return NULL;
}

static struct apfs_delayed_item *__apfs_lookup_delayed_insertion_item(
					struct apfs_delayed_node *delayed_node,
					struct apfs_key *key)
{
	return __apfs_lookup_delayed_item(&delayed_node->ins_root.rb_root, key,
					   NULL, NULL);
}

static int __apfs_add_delayed_item(struct apfs_delayed_node *delayed_node,
				    struct apfs_delayed_item *ins,
				    int action)
{
	return 0;
}

static int __apfs_add_delayed_insertion_item(struct apfs_delayed_node *node,
					      struct apfs_delayed_item *item)
{
	return __apfs_add_delayed_item(node, item,
					APFS_DELAYED_INSERTION_ITEM);
}

static int __apfs_add_delayed_deletion_item(struct apfs_delayed_node *node,
					     struct apfs_delayed_item *item)
{
	return __apfs_add_delayed_item(node, item,
					APFS_DELAYED_DELETION_ITEM);
}

static void finish_one_item(struct apfs_delayed_root *delayed_root)
{
	int seq = atomic_inc_return(&delayed_root->items_seq);

	/* atomic_dec_return implies a barrier */
	if ((atomic_dec_return(&delayed_root->items) <
	    APFS_DELAYED_BACKGROUND || seq % APFS_DELAYED_BATCH == 0))
		cond_wake_up_nomb(&delayed_root->wait);
}

static void __apfs_remove_delayed_item(struct apfs_delayed_item *delayed_item)
{
	struct rb_root_cached *root;
	struct apfs_delayed_root *delayed_root;

	/* Not associated with any delayed_node */
	if (!delayed_item->delayed_node)
		return;
	delayed_root = delayed_item->delayed_node->root->fs_info->delayed_root;

	BUG_ON(!delayed_root);
	BUG_ON(delayed_item->ins_or_del != APFS_DELAYED_DELETION_ITEM &&
	       delayed_item->ins_or_del != APFS_DELAYED_INSERTION_ITEM);

	if (delayed_item->ins_or_del == APFS_DELAYED_INSERTION_ITEM)
		root = &delayed_item->delayed_node->ins_root;
	else
		root = &delayed_item->delayed_node->del_root;

	rb_erase_cached(&delayed_item->rb_node, root);
	delayed_item->delayed_node->count--;

	finish_one_item(delayed_root);
}

static void apfs_release_delayed_item(struct apfs_delayed_item *item)
{
	if (item) {
		__apfs_remove_delayed_item(item);
		if (refcount_dec_and_test(&item->refs))
			kfree(item);
	}
}

static struct apfs_delayed_item *__apfs_first_delayed_insertion_item(
					struct apfs_delayed_node *delayed_node)
{
	struct rb_node *p;
	struct apfs_delayed_item *item = NULL;

	p = rb_first_cached(&delayed_node->ins_root);
	if (p)
		item = rb_entry(p, struct apfs_delayed_item, rb_node);

	return item;
}

static struct apfs_delayed_item *__apfs_first_delayed_deletion_item(
					struct apfs_delayed_node *delayed_node)
{
	struct rb_node *p;
	struct apfs_delayed_item *item = NULL;

	p = rb_first_cached(&delayed_node->del_root);
	if (p)
		item = rb_entry(p, struct apfs_delayed_item, rb_node);

	return item;
}

static struct apfs_delayed_item *__apfs_next_delayed_item(
						struct apfs_delayed_item *item)
{
	struct rb_node *p;
	struct apfs_delayed_item *next = NULL;

	p = rb_next(&item->rb_node);
	if (p)
		next = rb_entry(p, struct apfs_delayed_item, rb_node);

	return next;
}

static int apfs_delayed_item_reserve_metadata(struct apfs_trans_handle *trans,
					       struct apfs_root *root,
					       struct apfs_delayed_item *item)
{
	struct apfs_block_rsv *src_rsv;
	struct apfs_block_rsv *dst_rsv;
	struct apfs_fs_info *fs_info = root->fs_info;
	u64 num_bytes;
	int ret;

	if (!trans->bytes_reserved)
		return 0;

	src_rsv = trans->block_rsv;
	dst_rsv = &fs_info->delayed_block_rsv;

	num_bytes = apfs_calc_insert_metadata_size(fs_info, 1);

	/*
	 * Here we migrate space rsv from transaction rsv, since have already
	 * reserved space when starting a transaction.  So no need to reserve
	 * qgroup space here.
	 */
	ret = apfs_block_rsv_migrate(src_rsv, dst_rsv, num_bytes, true);
	if (!ret) {
		trace_apfs_space_reservation(fs_info, "delayed_item",
					      item->key.objectid,
					      num_bytes, 1);
		item->bytes_reserved = num_bytes;
	}

	return ret;
}

static void apfs_delayed_item_release_metadata(struct apfs_root *root,
						struct apfs_delayed_item *item)
{
	struct apfs_block_rsv *rsv;
	struct apfs_fs_info *fs_info = root->fs_info;

	if (!item->bytes_reserved)
		return;

	rsv = &fs_info->delayed_block_rsv;
	/*
	 * Check apfs_delayed_item_reserve_metadata() to see why we don't need
	 * to release/reserve qgroup space.
	 */
	trace_apfs_space_reservation(fs_info, "delayed_item",
				      item->key.objectid, item->bytes_reserved,
				      0);
	apfs_block_rsv_release(fs_info, rsv, item->bytes_reserved, NULL);
}

static int apfs_delayed_inode_reserve_metadata(
					struct apfs_trans_handle *trans,
					struct apfs_root *root,
					struct apfs_delayed_node *node)
{
	struct apfs_fs_info *fs_info = root->fs_info;
	struct apfs_block_rsv *src_rsv;
	struct apfs_block_rsv *dst_rsv;
	u64 num_bytes;
	int ret;

	src_rsv = trans->block_rsv;
	dst_rsv = &fs_info->delayed_block_rsv;

	num_bytes = apfs_calc_metadata_size(fs_info, 1);

	/*
	 * apfs_dirty_inode will update the inode under apfs_join_transaction
	 * which doesn't reserve space for speed.  This is a problem since we
	 * still need to reserve space for this update, so try to reserve the
	 * space.
	 *
	 * Now if src_rsv == delalloc_block_rsv we'll let it just steal since
	 * we always reserve enough to update the inode item.
	 */
	if (!src_rsv || (!trans->bytes_reserved &&
			 src_rsv->type != APFS_BLOCK_RSV_DELALLOC)) {
		ret = apfs_qgroup_reserve_meta(root, num_bytes,
					  APFS_QGROUP_RSV_META_PREALLOC, true);
		if (ret < 0)
			return ret;
		ret = apfs_block_rsv_add(root, dst_rsv, num_bytes,
					  APFS_RESERVE_NO_FLUSH);
		/* NO_FLUSH could only fail with -ENOSPC */
		ASSERT(ret == 0 || ret == -ENOSPC);
		if (ret)
			apfs_qgroup_free_meta_prealloc(root, num_bytes);
	} else {
		ret = apfs_block_rsv_migrate(src_rsv, dst_rsv, num_bytes, true);
	}

	if (!ret) {
		trace_apfs_space_reservation(fs_info, "delayed_inode",
					      node->inode_id, num_bytes, 1);
		node->bytes_reserved = num_bytes;
	}

	return ret;
}

static void apfs_delayed_inode_release_metadata(struct apfs_fs_info *fs_info,
						struct apfs_delayed_node *node,
						bool qgroup_free)
{
	struct apfs_block_rsv *rsv;

	if (!node->bytes_reserved)
		return;

	rsv = &fs_info->delayed_block_rsv;
	trace_apfs_space_reservation(fs_info, "delayed_inode",
				      node->inode_id, node->bytes_reserved, 0);
	apfs_block_rsv_release(fs_info, rsv, node->bytes_reserved, NULL);
	if (qgroup_free)
		apfs_qgroup_free_meta_prealloc(node->root,
				node->bytes_reserved);
	else
		apfs_qgroup_convert_reserved_meta(node->root,
				node->bytes_reserved);
	node->bytes_reserved = 0;
}

/*
 * This helper will insert some continuous items into the same leaf according
 * to the free space of the leaf.
 */
static int apfs_batch_insert_items(struct apfs_root *root,
				    struct apfs_path *path,
				    struct apfs_delayed_item *item)
{
	struct apfs_delayed_item *curr, *next;
	int free_space;
	int total_size = 0;
	struct extent_buffer *leaf;
	char *data_ptr;
	struct apfs_key *keys;
	u32 *data_size;
	struct list_head head;
	int slot;
	int nitems;
	int i;
	int ret = 0;

	BUG_ON(!path->nodes[0]);

	leaf = path->nodes[0];
	free_space = apfs_leaf_free_space(leaf);
	INIT_LIST_HEAD(&head);

	next = item;
	nitems = 0;

	/*
	 * count the number of the continuous items that we can insert in batch
	 */
	while (total_size + next->data_len + sizeof(struct apfs_item) <=
	       free_space) {
		total_size += next->data_len + sizeof(struct apfs_item);
		list_add_tail(&next->tree_list, &head);
		nitems++;

		curr = next;
		next = __apfs_next_delayed_item(curr);
		if (!next)
			break;

		if (!apfs_is_continuous_delayed_item(curr, next))
			break;
	}

	if (!nitems) {
		ret = 0;
		goto out;
	}

	keys = kmalloc_array(nitems, sizeof(struct apfs_key), GFP_NOFS);
	if (!keys) {
		ret = -ENOMEM;
		goto out;
	}

	data_size = kmalloc_array(nitems, sizeof(u32), GFP_NOFS);
	if (!data_size) {
		ret = -ENOMEM;
		goto error;
	}

	/* get keys of all the delayed items */
	i = 0;
	list_for_each_entry(next, &head, tree_list) {
		keys[i] = next->key;
		data_size[i] = next->data_len;
		i++;
	}

	/* insert the keys of the items */
	setup_items_for_insert(root, path, keys, data_size, nitems);

	/* insert the dir index items */
	slot = path->slots[0];
	list_for_each_entry_safe(curr, next, &head, tree_list) {
		data_ptr = apfs_item_ptr(leaf, slot, char);
		write_extent_buffer(leaf, &curr->data,
				    (unsigned long)data_ptr,
				    curr->data_len);
		slot++;

		apfs_delayed_item_release_metadata(root, curr);

		list_del(&curr->tree_list);
		apfs_release_delayed_item(curr);
	}

error:
	kfree(data_size);
	kfree(keys);
out:
	return ret;
}

/*
 * This helper can just do simple insertion that needn't extend item for new
 * data, such as directory name index insertion, inode insertion.
 */
static int apfs_insert_delayed_item(struct apfs_trans_handle *trans,
				     struct apfs_root *root,
				     struct apfs_path *path,
				     struct apfs_delayed_item *delayed_item)
{
	struct extent_buffer *leaf;
	unsigned int nofs_flag;
	char *ptr;
	int ret;

	nofs_flag = memalloc_nofs_save();
	ret = apfs_insert_empty_item(trans, root, path, &delayed_item->key,
				      delayed_item->data_len);
	memalloc_nofs_restore(nofs_flag);
	if (ret < 0 && ret != -EEXIST)
		return ret;

	leaf = path->nodes[0];

	ptr = apfs_item_ptr(leaf, path->slots[0], char);

	write_extent_buffer(leaf, delayed_item->data, (unsigned long)ptr,
			    delayed_item->data_len);
	apfs_mark_buffer_dirty(leaf);

	apfs_delayed_item_release_metadata(root, delayed_item);
	return 0;
}

/*
 * we insert an item first, then if there are some continuous items, we try
 * to insert those items into the same leaf.
 */
static int apfs_insert_delayed_items(struct apfs_trans_handle *trans,
				      struct apfs_path *path,
				      struct apfs_root *root,
				      struct apfs_delayed_node *node)
{
	struct apfs_delayed_item *curr, *prev;
	int ret = 0;

do_again:
	mutex_lock(&node->mutex);
	curr = __apfs_first_delayed_insertion_item(node);
	if (!curr)
		goto insert_end;

	ret = apfs_insert_delayed_item(trans, root, path, curr);
	if (ret < 0) {
		apfs_release_path(path);
		goto insert_end;
	}

	prev = curr;
	curr = __apfs_next_delayed_item(prev);
	if (curr && apfs_is_continuous_delayed_item(prev, curr)) {
		/* insert the continuous items into the same leaf */
		path->slots[0]++;
		apfs_batch_insert_items(root, path, curr);
	}
	apfs_release_delayed_item(prev);
	apfs_mark_buffer_dirty(path->nodes[0]);

	apfs_release_path(path);
	mutex_unlock(&node->mutex);
	goto do_again;

insert_end:
	mutex_unlock(&node->mutex);
	return ret;
}

static int apfs_batch_delete_items(struct apfs_trans_handle *trans,
				    struct apfs_root *root,
				    struct apfs_path *path,
				    struct apfs_delayed_item *item)
{
	struct apfs_delayed_item *curr, *next;
	struct extent_buffer *leaf;
	struct apfs_key key = {};
	struct list_head head;
	int nitems, i, last_item;
	int ret = 0;

	BUG_ON(!path->nodes[0]);

	leaf = path->nodes[0];

	i = path->slots[0];
	last_item = apfs_header_nritems(leaf) - 1;
	if (i > last_item)
		return -ENOENT;	/* FIXME: Is errno suitable? */

	next = item;
	INIT_LIST_HEAD(&head);
	apfs_item_key_to_cpu(leaf, &key, i);
	nitems = 0;
	/*
	 * count the number of the dir index items that we can delete in batch
	 */
	while (apfs_comp_cpu_keys(leaf, &next->key, &key) == 0) {
		list_add_tail(&next->tree_list, &head);
		nitems++;

		curr = next;
		next = __apfs_next_delayed_item(curr);
		if (!next)
			break;

		if (!apfs_is_continuous_delayed_item(curr, next))
			break;

		i++;
		if (i > last_item)
			break;
		apfs_item_key_to_cpu(leaf, &key, i);
	}

	if (!nitems)
		return 0;

	ret = apfs_del_items(trans, root, path, path->slots[0], nitems);
	if (ret)
		goto out;

	list_for_each_entry_safe(curr, next, &head, tree_list) {
		apfs_delayed_item_release_metadata(root, curr);
		list_del(&curr->tree_list);
		apfs_release_delayed_item(curr);
	}

out:
	return ret;
}

static int apfs_delete_delayed_items(struct apfs_trans_handle *trans,
				      struct apfs_path *path,
				      struct apfs_root *root,
				      struct apfs_delayed_node *node)
{
	struct apfs_delayed_item *curr, *prev;
	unsigned int nofs_flag;
	int ret = 0;

do_again:
	mutex_lock(&node->mutex);
	curr = __apfs_first_delayed_deletion_item(node);
	if (!curr)
		goto delete_fail;

	nofs_flag = memalloc_nofs_save();
	ret = apfs_search_slot(trans, root, &curr->key, path, -1, 1);
	memalloc_nofs_restore(nofs_flag);
	if (ret < 0)
		goto delete_fail;
	else if (ret > 0) {
		/*
		 * can't find the item which the node points to, so this node
		 * is invalid, just drop it.
		 */
		prev = curr;
		curr = __apfs_next_delayed_item(prev);
		apfs_release_delayed_item(prev);
		ret = 0;
		apfs_release_path(path);
		if (curr) {
			mutex_unlock(&node->mutex);
			goto do_again;
		} else
			goto delete_fail;
	}

	apfs_batch_delete_items(trans, root, path, curr);
	apfs_release_path(path);
	mutex_unlock(&node->mutex);
	goto do_again;

delete_fail:
	apfs_release_path(path);
	mutex_unlock(&node->mutex);
	return ret;
}

static void apfs_release_delayed_inode(struct apfs_delayed_node *delayed_node)
{
	struct apfs_delayed_root *delayed_root;

	if (delayed_node &&
	    test_bit(APFS_DELAYED_NODE_INODE_DIRTY, &delayed_node->flags)) {
		BUG_ON(!delayed_node->root);
		clear_bit(APFS_DELAYED_NODE_INODE_DIRTY, &delayed_node->flags);
		delayed_node->count--;

		delayed_root = delayed_node->root->fs_info->delayed_root;
		finish_one_item(delayed_root);
	}
}

static void apfs_release_delayed_iref(struct apfs_delayed_node *delayed_node)
{

	if (test_and_clear_bit(APFS_DELAYED_NODE_DEL_IREF, &delayed_node->flags)) {
		struct apfs_delayed_root *delayed_root;

		ASSERT(delayed_node->root);
		delayed_node->count--;

		delayed_root = delayed_node->root->fs_info->delayed_root;
		finish_one_item(delayed_root);
	}
}

static int __apfs_update_delayed_inode(struct apfs_trans_handle *trans,
					struct apfs_root *root,
					struct apfs_path *path,
					struct apfs_delayed_node *node)
{
	struct apfs_fs_info *fs_info = root->fs_info;
	struct apfs_key key = {};
	struct apfs_inode_item *inode_item;
	struct extent_buffer *leaf;
	unsigned int nofs_flag;
	int mod;
	int ret;

	key.objectid = node->inode_id;
	key.type = APFS_INODE_ITEM_KEY;
	key.offset = 0;

	if (test_bit(APFS_DELAYED_NODE_DEL_IREF, &node->flags))
		mod = -1;
	else
		mod = 1;

	nofs_flag = memalloc_nofs_save();
	ret = apfs_lookup_inode(trans, root, path, &key, mod);
	memalloc_nofs_restore(nofs_flag);
	if (ret > 0)
		ret = -ENOENT;
	if (ret < 0)
		goto out;

	leaf = path->nodes[0];
	inode_item = apfs_item_ptr(leaf, path->slots[0],
				    struct apfs_inode_item);
	write_extent_buffer(leaf, &node->inode_item, (unsigned long)inode_item,
			    sizeof(struct apfs_inode_item));
	apfs_mark_buffer_dirty(leaf);

	if (!test_bit(APFS_DELAYED_NODE_DEL_IREF, &node->flags))
		goto out;

	path->slots[0]++;
	if (path->slots[0] >= apfs_header_nritems(leaf))
		goto search;
again:
	apfs_item_key_to_cpu(leaf, &key, path->slots[0]);
	if (key.objectid != node->inode_id)
		goto out;

	if (key.type != APFS_INODE_REF_KEY &&
	    key.type != APFS_INODE_EXTREF_KEY)
		goto out;

	/*
	 * Delayed iref deletion is for the inode who has only one link,
	 * so there is only one iref. The case that several irefs are
	 * in the same item doesn't exist.
	 */
	apfs_del_item(trans, root, path);
out:
	apfs_release_delayed_iref(node);
	apfs_release_path(path);
err_out:
	apfs_delayed_inode_release_metadata(fs_info, node, (ret < 0));
	apfs_release_delayed_inode(node);

	/*
	 * If we fail to update the delayed inode we need to abort the
	 * transaction, because we could leave the inode with the improper
	 * counts behind.
	 */
	if (ret && ret != -ENOENT)
		apfs_abort_transaction(trans, ret);

	return ret;

search:
	apfs_release_path(path);

	key.type = APFS_INODE_EXTREF_KEY;
	key.offset = -1;

	nofs_flag = memalloc_nofs_save();
	ret = apfs_search_slot(trans, root, &key, path, -1, 1);
	memalloc_nofs_restore(nofs_flag);
	if (ret < 0)
		goto err_out;
	ASSERT(ret);

	ret = 0;
	leaf = path->nodes[0];
	path->slots[0]--;
	goto again;
}

static inline int apfs_update_delayed_inode(struct apfs_trans_handle *trans,
					     struct apfs_root *root,
					     struct apfs_path *path,
					     struct apfs_delayed_node *node)
{
	int ret;

	mutex_lock(&node->mutex);
	if (!test_bit(APFS_DELAYED_NODE_INODE_DIRTY, &node->flags)) {
		mutex_unlock(&node->mutex);
		return 0;
	}

	ret = __apfs_update_delayed_inode(trans, root, path, node);
	mutex_unlock(&node->mutex);
	return ret;
}

static inline int
__apfs_commit_inode_delayed_items(struct apfs_trans_handle *trans,
				   struct apfs_path *path,
				   struct apfs_delayed_node *node)
{
	int ret;

	ret = apfs_insert_delayed_items(trans, path, node->root, node);
	if (ret)
		return ret;

	ret = apfs_delete_delayed_items(trans, path, node->root, node);
	if (ret)
		return ret;

	ret = apfs_update_delayed_inode(trans, node->root, path, node);
	return ret;
}

/*
 * Called when committing the transaction.
 * Returns 0 on success.
 * Returns < 0 on error and returns with an aborted transaction with any
 * outstanding delayed items cleaned up.
 */
static int __apfs_run_delayed_items(struct apfs_trans_handle *trans, int nr)
{
	struct apfs_fs_info *fs_info = trans->fs_info;
	struct apfs_delayed_root *delayed_root;
	struct apfs_delayed_node *curr_node, *prev_node;
	struct apfs_path *path;
	struct apfs_block_rsv *block_rsv;
	int ret = 0;
	bool count = (nr > 0);

	if (TRANS_ABORTED(trans))
		return -EIO;

	path = apfs_alloc_path();
	if (!path)
		return -ENOMEM;

	block_rsv = trans->block_rsv;
	trans->block_rsv = &fs_info->delayed_block_rsv;

	delayed_root = fs_info->delayed_root;

	curr_node = apfs_first_delayed_node(delayed_root);
	while (curr_node && (!count || nr--)) {
		ret = __apfs_commit_inode_delayed_items(trans, path,
							 curr_node);
		if (ret) {
			apfs_release_delayed_node(curr_node);
			curr_node = NULL;
			apfs_abort_transaction(trans, ret);
			break;
		}

		prev_node = curr_node;
		curr_node = apfs_next_delayed_node(curr_node);
		apfs_release_delayed_node(prev_node);
	}

	if (curr_node)
		apfs_release_delayed_node(curr_node);
	apfs_free_path(path);
	trans->block_rsv = block_rsv;

	return ret;
}

int apfs_run_delayed_items(struct apfs_trans_handle *trans)
{
	return __apfs_run_delayed_items(trans, -1);
}

int apfs_run_delayed_items_nr(struct apfs_trans_handle *trans, int nr)
{
	return __apfs_run_delayed_items(trans, nr);
}

int apfs_commit_inode_delayed_items(struct apfs_trans_handle *trans,
				     struct apfs_inode *inode)
{
	struct apfs_delayed_node *delayed_node = apfs_get_delayed_node(inode);
	struct apfs_path *path;
	struct apfs_block_rsv *block_rsv;
	int ret;

	if (!delayed_node)
		return 0;

	mutex_lock(&delayed_node->mutex);
	if (!delayed_node->count) {
		mutex_unlock(&delayed_node->mutex);
		apfs_release_delayed_node(delayed_node);
		return 0;
	}
	mutex_unlock(&delayed_node->mutex);

	path = apfs_alloc_path();
	if (!path) {
		apfs_release_delayed_node(delayed_node);
		return -ENOMEM;
	}

	block_rsv = trans->block_rsv;
	trans->block_rsv = &delayed_node->root->fs_info->delayed_block_rsv;

	ret = __apfs_commit_inode_delayed_items(trans, path, delayed_node);

	apfs_release_delayed_node(delayed_node);
	apfs_free_path(path);
	trans->block_rsv = block_rsv;

	return ret;
}

int apfs_commit_inode_delayed_inode(struct apfs_inode *inode)
{
	struct apfs_fs_info *fs_info = inode->root->fs_info;
	struct apfs_trans_handle *trans;
	struct apfs_delayed_node *delayed_node = apfs_get_delayed_node(inode);
	struct apfs_path *path;
	struct apfs_block_rsv *block_rsv;
	int ret;

	if (!delayed_node)
		return 0;

	mutex_lock(&delayed_node->mutex);
	if (!test_bit(APFS_DELAYED_NODE_INODE_DIRTY, &delayed_node->flags)) {
		mutex_unlock(&delayed_node->mutex);
		apfs_release_delayed_node(delayed_node);
		return 0;
	}
	mutex_unlock(&delayed_node->mutex);

	trans = apfs_join_transaction(delayed_node->root);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		goto out;
	}

	path = apfs_alloc_path();
	if (!path) {
		ret = -ENOMEM;
		goto trans_out;
	}

	block_rsv = trans->block_rsv;
	trans->block_rsv = &fs_info->delayed_block_rsv;

	mutex_lock(&delayed_node->mutex);
	if (test_bit(APFS_DELAYED_NODE_INODE_DIRTY, &delayed_node->flags))
		ret = __apfs_update_delayed_inode(trans, delayed_node->root,
						   path, delayed_node);
	else
		ret = 0;
	mutex_unlock(&delayed_node->mutex);

	apfs_free_path(path);
	trans->block_rsv = block_rsv;
trans_out:
	apfs_end_transaction(trans);
	apfs_btree_balance_dirty(fs_info);
out:
	apfs_release_delayed_node(delayed_node);

	return ret;
}

void apfs_remove_delayed_node(struct apfs_inode *inode)
{
	struct apfs_delayed_node *delayed_node;

	delayed_node = READ_ONCE(inode->delayed_node);
	if (!delayed_node)
		return;

	inode->delayed_node = NULL;
	apfs_release_delayed_node(delayed_node);
}

struct apfs_async_delayed_work {
	struct apfs_delayed_root *delayed_root;
	int nr;
	struct apfs_work work;
};

static void apfs_async_run_delayed_root(struct apfs_work *work)
{
	struct apfs_async_delayed_work *async_work;
	struct apfs_delayed_root *delayed_root;
	struct apfs_trans_handle *trans;
	struct apfs_path *path;
	struct apfs_delayed_node *delayed_node = NULL;
	struct apfs_root *root;
	struct apfs_block_rsv *block_rsv;
	int total_done = 0;

	async_work = container_of(work, struct apfs_async_delayed_work, work);
	delayed_root = async_work->delayed_root;

	path = apfs_alloc_path();
	if (!path)
		goto out;

	do {
		if (atomic_read(&delayed_root->items) <
		    APFS_DELAYED_BACKGROUND / 2)
			break;

		delayed_node = apfs_first_prepared_delayed_node(delayed_root);
		if (!delayed_node)
			break;

		root = delayed_node->root;

		trans = apfs_join_transaction(root);
		if (IS_ERR(trans)) {
			apfs_release_path(path);
			apfs_release_prepared_delayed_node(delayed_node);
			total_done++;
			continue;
		}

		block_rsv = trans->block_rsv;
		trans->block_rsv = &root->fs_info->delayed_block_rsv;

		__apfs_commit_inode_delayed_items(trans, path, delayed_node);

		trans->block_rsv = block_rsv;
		apfs_end_transaction(trans);
		apfs_btree_balance_dirty_nodelay(root->fs_info);

		apfs_release_path(path);
		apfs_release_prepared_delayed_node(delayed_node);
		total_done++;

	} while ((async_work->nr == 0 && total_done < APFS_DELAYED_WRITEBACK)
		 || total_done < async_work->nr);

	apfs_free_path(path);
out:
	wake_up(&delayed_root->wait);
	kfree(async_work);
}


static int apfs_wq_run_delayed_node(struct apfs_delayed_root *delayed_root,
				     struct apfs_fs_info *fs_info, int nr)
{
	struct apfs_async_delayed_work *async_work;

	async_work = kmalloc(sizeof(*async_work), GFP_NOFS);
	if (!async_work)
		return -ENOMEM;

	async_work->delayed_root = delayed_root;
	apfs_init_work(&async_work->work, apfs_async_run_delayed_root, NULL,
			NULL);
	async_work->nr = nr;

	apfs_queue_work(fs_info->delayed_workers, &async_work->work);
	return 0;
}

void apfs_assert_delayed_root_empty(struct apfs_fs_info *fs_info)
{
	WARN_ON(apfs_first_delayed_node(fs_info->delayed_root));
}

static int could_end_wait(struct apfs_delayed_root *delayed_root, int seq)
{
	int val = atomic_read(&delayed_root->items_seq);

	if (val < seq || val >= seq + APFS_DELAYED_BATCH)
		return 1;

	if (atomic_read(&delayed_root->items) < APFS_DELAYED_BACKGROUND)
		return 1;

	return 0;
}

void apfs_balance_delayed_items(struct apfs_fs_info *fs_info)
{
	struct apfs_delayed_root *delayed_root = fs_info->delayed_root;

	if ((atomic_read(&delayed_root->items) < APFS_DELAYED_BACKGROUND) ||
		apfs_workqueue_normal_congested(fs_info->delayed_workers))
		return;

	if (atomic_read(&delayed_root->items) >= APFS_DELAYED_WRITEBACK) {
		int seq;
		int ret;

		seq = atomic_read(&delayed_root->items_seq);

		ret = apfs_wq_run_delayed_node(delayed_root, fs_info, 0);
		if (ret)
			return;

		wait_event_interruptible(delayed_root->wait,
					 could_end_wait(delayed_root, seq));
		return;
	}

	apfs_wq_run_delayed_node(delayed_root, fs_info, APFS_DELAYED_BATCH);
}

/* Will return 0 or -ENOMEM */
int apfs_insert_delayed_dir_index(struct apfs_trans_handle *trans,
				   const char *name, int name_len,
				   struct apfs_inode *dir,
				   struct apfs_disk_key *disk_key, u8 type,
				   u64 index)
{
	struct apfs_delayed_node *delayed_node;
	struct apfs_delayed_item *delayed_item;
	struct apfs_dir_item *dir_item;
	int ret;

	delayed_node = apfs_get_or_create_delayed_node(dir);
	if (IS_ERR(delayed_node))
		return PTR_ERR(delayed_node);

	delayed_item = apfs_alloc_delayed_item(sizeof(*dir_item) + name_len);
	if (!delayed_item) {
		ret = -ENOMEM;
		goto release_node;
	}

	delayed_item->key.objectid = apfs_ino(dir);
	delayed_item->key.type = APFS_DIR_INDEX_KEY;
	delayed_item->key.offset = index;

	dir_item = (struct apfs_dir_item *)delayed_item->data;
	dir_item->location = *disk_key;
	apfs_set_stack_dir_transid(dir_item, trans->transid);
	apfs_set_stack_dir_data_len(dir_item, 0);
	apfs_set_stack_dir_name_len(dir_item, name_len);
	apfs_set_stack_dir_type(dir_item, type);
	memcpy((char *)(dir_item + 1), name, name_len);

	ret = apfs_delayed_item_reserve_metadata(trans, dir->root, delayed_item);
	/*
	 * we have reserved enough space when we start a new transaction,
	 * so reserving metadata failure is impossible
	 */
	BUG_ON(ret);

	mutex_lock(&delayed_node->mutex);
	ret = __apfs_add_delayed_insertion_item(delayed_node, delayed_item);
	if (unlikely(ret)) {
		apfs_err(trans->fs_info,
			  "err add delayed dir index item(name: %.*s) into the insertion tree of the delayed node(root id: %llu, inode id: %llu, errno: %d)",
			  name_len, name, delayed_node->root->root_key.objectid,
			  delayed_node->inode_id, ret);
		BUG();
	}
	mutex_unlock(&delayed_node->mutex);

release_node:
	apfs_release_delayed_node(delayed_node);
	return ret;
}

static int apfs_delete_delayed_insertion_item(struct apfs_fs_info *fs_info,
					       struct apfs_delayed_node *node,
					       struct apfs_key *key)
{
	struct apfs_delayed_item *item;

	mutex_lock(&node->mutex);
	item = __apfs_lookup_delayed_insertion_item(node, key);
	if (!item) {
		mutex_unlock(&node->mutex);
		return 1;
	}

	apfs_delayed_item_release_metadata(node->root, item);
	apfs_release_delayed_item(item);
	mutex_unlock(&node->mutex);
	return 0;
}

int apfs_delete_delayed_dir_index(struct apfs_trans_handle *trans,
				   struct apfs_inode *dir, u64 index)
{
	struct apfs_delayed_node *node;
	struct apfs_delayed_item *item;
	struct apfs_key item_key = {};
	int ret;

	node = apfs_get_or_create_delayed_node(dir);
	if (IS_ERR(node))
		return PTR_ERR(node);

	item_key.objectid = apfs_ino(dir);
	item_key.type = APFS_DIR_INDEX_KEY;
	item_key.offset = index;

	ret = apfs_delete_delayed_insertion_item(trans->fs_info, node,
						  &item_key);
	if (!ret)
		goto end;

	item = apfs_alloc_delayed_item(0);
	if (!item) {
		ret = -ENOMEM;
		goto end;
	}

	item->key = item_key;

	ret = apfs_delayed_item_reserve_metadata(trans, dir->root, item);
	/*
	 * we have reserved enough space when we start a new transaction,
	 * so reserving metadata failure is impossible.
	 */
	if (ret < 0) {
		apfs_err(trans->fs_info,
"metadata reservation failed for delayed dir item deltiona, should have been reserved");
		apfs_release_delayed_item(item);
		goto end;
	}

	mutex_lock(&node->mutex);
	ret = __apfs_add_delayed_deletion_item(node, item);
	if (unlikely(ret)) {
		apfs_err(trans->fs_info,
			  "err add delayed dir index item(index: %llu) into the deletion tree of the delayed node(root id: %llu, inode id: %llu, errno: %d)",
			  index, node->root->root_key.objectid,
			  node->inode_id, ret);
		apfs_delayed_item_release_metadata(dir->root, item);
		apfs_release_delayed_item(item);
	}
	mutex_unlock(&node->mutex);
end:
	apfs_release_delayed_node(node);
	return ret;
}

int apfs_inode_delayed_dir_index_count(struct apfs_inode *inode)
{
	struct apfs_delayed_node *delayed_node = apfs_get_delayed_node(inode);

	if (!delayed_node)
		return -ENOENT;

	/*
	 * Since we have held i_mutex of this directory, it is impossible that
	 * a new directory index is added into the delayed node and index_cnt
	 * is updated now. So we needn't lock the delayed node.
	 */
	if (!delayed_node->index_cnt) {
		apfs_release_delayed_node(delayed_node);
		return -EINVAL;
	}

	inode->index_cnt = delayed_node->index_cnt;
	apfs_release_delayed_node(delayed_node);
	return 0;
}

bool apfs_readdir_get_delayed_items(struct inode *inode,
				     struct list_head *ins_list,
				     struct list_head *del_list)
{
	struct apfs_delayed_node *delayed_node;
	struct apfs_delayed_item *item;

	delayed_node = apfs_get_delayed_node(APFS_I(inode));
	if (!delayed_node)
		return false;

	/*
	 * We can only do one readdir with delayed items at a time because of
	 * item->readdir_list.
	 */
	apfs_inode_unlock(inode, APFS_ILOCK_SHARED);
	apfs_inode_lock(inode, 0);

	mutex_lock(&delayed_node->mutex);
	item = __apfs_first_delayed_insertion_item(delayed_node);
	while (item) {
		refcount_inc(&item->refs);
		list_add_tail(&item->readdir_list, ins_list);
		item = __apfs_next_delayed_item(item);
	}

	item = __apfs_first_delayed_deletion_item(delayed_node);
	while (item) {
		refcount_inc(&item->refs);
		list_add_tail(&item->readdir_list, del_list);
		item = __apfs_next_delayed_item(item);
	}
	mutex_unlock(&delayed_node->mutex);
	/*
	 * This delayed node is still cached in the apfs inode, so refs
	 * must be > 1 now, and we needn't check it is going to be freed
	 * or not.
	 *
	 * Besides that, this function is used to read dir, we do not
	 * insert/delete delayed items in this period. So we also needn't
	 * requeue or dequeue this delayed node.
	 */
	refcount_dec(&delayed_node->refs);

	return true;
}

void apfs_readdir_put_delayed_items(struct inode *inode,
				     struct list_head *ins_list,
				     struct list_head *del_list)
{
	struct apfs_delayed_item *curr, *next;

	list_for_each_entry_safe(curr, next, ins_list, readdir_list) {
		list_del(&curr->readdir_list);
		if (refcount_dec_and_test(&curr->refs))
			kfree(curr);
	}

	list_for_each_entry_safe(curr, next, del_list, readdir_list) {
		list_del(&curr->readdir_list);
		if (refcount_dec_and_test(&curr->refs))
			kfree(curr);
	}

	/*
	 * The VFS is going to do up_read(), so we need to downgrade back to a
	 * read lock.
	 */
	downgrade_write(&inode->i_rwsem);
}

int apfs_should_delete_dir_index(struct list_head *del_list,
				  u64 index)
{
	struct apfs_delayed_item *curr;
	int ret = 0;

	list_for_each_entry(curr, del_list, readdir_list) {
		if (curr->key.offset > index)
			break;
		if (curr->key.offset == index) {
			ret = 1;
			break;
		}
	}
	return ret;
}

/*
 * apfs_readdir_delayed_dir_index - read dir info stored in the delayed tree
 *
 */
int apfs_readdir_delayed_dir_index(struct dir_context *ctx,
				    struct list_head *ins_list)
{
	return 0;
}

static void fill_stack_inode_item(struct apfs_trans_handle *trans,
				  struct apfs_inode_item *inode_item,
				  struct inode *inode)
{
	apfs_set_stack_inode_uid(inode_item, i_uid_read(inode));
	apfs_set_stack_inode_gid(inode_item, i_gid_read(inode));
	apfs_set_stack_inode_size(inode_item, APFS_I(inode)->disk_i_size);
	apfs_set_stack_inode_mode(inode_item, inode->i_mode);
	apfs_set_stack_inode_nlink(inode_item, inode->i_nlink);
	apfs_set_stack_inode_nbytes(inode_item, inode_get_bytes(inode));
	apfs_set_stack_inode_generation(inode_item,
					 APFS_I(inode)->generation);
	apfs_set_stack_inode_sequence(inode_item,
				       inode_peek_iversion(inode));
	apfs_set_stack_inode_transid(inode_item, trans->transid);
	apfs_set_stack_inode_rdev(inode_item, inode->i_rdev);
	apfs_set_stack_inode_flags(inode_item, APFS_I(inode)->flags);
	apfs_set_stack_inode_block_group(inode_item, 0);

	apfs_set_stack_timespec_sec(&inode_item->atime,
				     inode->i_atime.tv_sec);
	apfs_set_stack_timespec_nsec(&inode_item->atime,
				      inode->i_atime.tv_nsec);

	apfs_set_stack_timespec_sec(&inode_item->mtime,
				     inode->i_mtime.tv_sec);
	apfs_set_stack_timespec_nsec(&inode_item->mtime,
				      inode->i_mtime.tv_nsec);

	apfs_set_stack_timespec_sec(&inode_item->ctime,
				     inode->i_ctime.tv_sec);
	apfs_set_stack_timespec_nsec(&inode_item->ctime,
				      inode->i_ctime.tv_nsec);

	apfs_set_stack_timespec_sec(&inode_item->otime,
				     APFS_I(inode)->i_otime.tv_sec);
	apfs_set_stack_timespec_nsec(&inode_item->otime,
				     APFS_I(inode)->i_otime.tv_nsec);
}

int apfs_fill_inode(struct inode *inode, u32 *rdev)
{
	struct apfs_fs_info *fs_info = APFS_I(inode)->root->fs_info;
	struct apfs_delayed_node *delayed_node;
	struct apfs_inode_item *inode_item;

	delayed_node = apfs_get_delayed_node(APFS_I(inode));
	if (!delayed_node)
		return -ENOENT;

	mutex_lock(&delayed_node->mutex);
	if (!test_bit(APFS_DELAYED_NODE_INODE_DIRTY, &delayed_node->flags)) {
		mutex_unlock(&delayed_node->mutex);
		apfs_release_delayed_node(delayed_node);
		return -ENOENT;
	}

	inode_item = &delayed_node->inode_item;

	i_uid_write(inode, apfs_stack_inode_uid(inode_item));
	i_gid_write(inode, apfs_stack_inode_gid(inode_item));
	apfs_i_size_write(APFS_I(inode), apfs_stack_inode_size(inode_item));
	apfs_inode_set_file_extent_range(APFS_I(inode), 0,
			round_up(i_size_read(inode), fs_info->sectorsize));
	inode->i_mode = apfs_stack_inode_mode(inode_item);
	set_nlink(inode, apfs_stack_inode_nlink(inode_item));
	inode_set_bytes(inode, apfs_stack_inode_nbytes(inode_item));
	APFS_I(inode)->generation = apfs_stack_inode_generation(inode_item);
        APFS_I(inode)->last_trans = apfs_stack_inode_transid(inode_item);

	inode_set_iversion_queried(inode,
				   apfs_stack_inode_sequence(inode_item));
	inode->i_rdev = 0;
	*rdev = apfs_stack_inode_rdev(inode_item);
	APFS_I(inode)->flags = apfs_stack_inode_flags(inode_item);

	inode->i_atime.tv_sec = apfs_stack_timespec_sec(&inode_item->atime);
	inode->i_atime.tv_nsec = apfs_stack_timespec_nsec(&inode_item->atime);

	inode->i_mtime.tv_sec = apfs_stack_timespec_sec(&inode_item->mtime);
	inode->i_mtime.tv_nsec = apfs_stack_timespec_nsec(&inode_item->mtime);

	inode->i_ctime.tv_sec = apfs_stack_timespec_sec(&inode_item->ctime);
	inode->i_ctime.tv_nsec = apfs_stack_timespec_nsec(&inode_item->ctime);

	APFS_I(inode)->i_otime.tv_sec =
		apfs_stack_timespec_sec(&inode_item->otime);
	APFS_I(inode)->i_otime.tv_nsec =
		apfs_stack_timespec_nsec(&inode_item->otime);

	inode->i_generation = APFS_I(inode)->generation;
	APFS_I(inode)->index_cnt = (u64)-1;

	mutex_unlock(&delayed_node->mutex);
	apfs_release_delayed_node(delayed_node);
	return 0;
}

int apfs_delayed_update_inode(struct apfs_trans_handle *trans,
			       struct apfs_root *root,
			       struct apfs_inode *inode)
{
	struct apfs_delayed_node *delayed_node;
	int ret = 0;

	delayed_node = apfs_get_or_create_delayed_node(inode);
	if (IS_ERR(delayed_node))
		return PTR_ERR(delayed_node);

	mutex_lock(&delayed_node->mutex);
	if (test_bit(APFS_DELAYED_NODE_INODE_DIRTY, &delayed_node->flags)) {
		fill_stack_inode_item(trans, &delayed_node->inode_item,
				      &inode->vfs_inode);
		goto release_node;
	}

	ret = apfs_delayed_inode_reserve_metadata(trans, root, delayed_node);
	if (ret)
		goto release_node;

	fill_stack_inode_item(trans, &delayed_node->inode_item, &inode->vfs_inode);
	set_bit(APFS_DELAYED_NODE_INODE_DIRTY, &delayed_node->flags);
	delayed_node->count++;
	atomic_inc(&root->fs_info->delayed_root->items);
release_node:
	mutex_unlock(&delayed_node->mutex);
	apfs_release_delayed_node(delayed_node);
	return ret;
}

int apfs_delayed_delete_inode_ref(struct apfs_inode *inode)
{
	struct apfs_fs_info *fs_info = inode->root->fs_info;
	struct apfs_delayed_node *delayed_node;

	/*
	 * we don't do delayed inode updates during log recovery because it
	 * leads to enospc problems.  This means we also can't do
	 * delayed inode refs
	 */
	if (test_bit(APFS_FS_LOG_RECOVERING, &fs_info->flags))
		return -EAGAIN;

	delayed_node = apfs_get_or_create_delayed_node(inode);
	if (IS_ERR(delayed_node))
		return PTR_ERR(delayed_node);

	/*
	 * We don't reserve space for inode ref deletion is because:
	 * - We ONLY do async inode ref deletion for the inode who has only
	 *   one link(i_nlink == 1), it means there is only one inode ref.
	 *   And in most case, the inode ref and the inode item are in the
	 *   same leaf, and we will deal with them at the same time.
	 *   Since we are sure we will reserve the space for the inode item,
	 *   it is unnecessary to reserve space for inode ref deletion.
	 * - If the inode ref and the inode item are not in the same leaf,
	 *   We also needn't worry about enospc problem, because we reserve
	 *   much more space for the inode update than it needs.
	 * - At the worst, we can steal some space from the global reservation.
	 *   It is very rare.
	 */
	mutex_lock(&delayed_node->mutex);
	if (test_bit(APFS_DELAYED_NODE_DEL_IREF, &delayed_node->flags))
		goto release_node;

	set_bit(APFS_DELAYED_NODE_DEL_IREF, &delayed_node->flags);
	delayed_node->count++;
	atomic_inc(&fs_info->delayed_root->items);
release_node:
	mutex_unlock(&delayed_node->mutex);
	apfs_release_delayed_node(delayed_node);
	return 0;
}

static void __apfs_kill_delayed_node(struct apfs_delayed_node *delayed_node)
{
	struct apfs_root *root = delayed_node->root;
	struct apfs_fs_info *fs_info = root->fs_info;
	struct apfs_delayed_item *curr_item, *prev_item;

	mutex_lock(&delayed_node->mutex);
	curr_item = __apfs_first_delayed_insertion_item(delayed_node);
	while (curr_item) {
		apfs_delayed_item_release_metadata(root, curr_item);
		prev_item = curr_item;
		curr_item = __apfs_next_delayed_item(prev_item);
		apfs_release_delayed_item(prev_item);
	}

	curr_item = __apfs_first_delayed_deletion_item(delayed_node);
	while (curr_item) {
		apfs_delayed_item_release_metadata(root, curr_item);
		prev_item = curr_item;
		curr_item = __apfs_next_delayed_item(prev_item);
		apfs_release_delayed_item(prev_item);
	}

	apfs_release_delayed_iref(delayed_node);

	if (test_bit(APFS_DELAYED_NODE_INODE_DIRTY, &delayed_node->flags)) {
		apfs_delayed_inode_release_metadata(fs_info, delayed_node, false);
		apfs_release_delayed_inode(delayed_node);
	}
	mutex_unlock(&delayed_node->mutex);
}

void apfs_kill_delayed_inode_items(struct apfs_inode *inode)
{
	struct apfs_delayed_node *delayed_node;

	delayed_node = apfs_get_delayed_node(inode);
	if (!delayed_node)
		return;

	__apfs_kill_delayed_node(delayed_node);
	apfs_release_delayed_node(delayed_node);
}

void apfs_kill_all_delayed_nodes(struct apfs_root *root)
{
	u64 inode_id = 0;
	struct apfs_delayed_node *delayed_nodes[8];
	int i, n;

	while (1) {
		spin_lock(&root->inode_lock);
		n = radix_tree_gang_lookup(&root->delayed_nodes_tree,
					   (void **)delayed_nodes, inode_id,
					   ARRAY_SIZE(delayed_nodes));
		if (!n) {
			spin_unlock(&root->inode_lock);
			break;
		}

		inode_id = delayed_nodes[n - 1]->inode_id + 1;
		for (i = 0; i < n; i++) {
			/*
			 * Don't increase refs in case the node is dead and
			 * about to be removed from the tree in the loop below
			 */
			if (!refcount_inc_not_zero(&delayed_nodes[i]->refs))
				delayed_nodes[i] = NULL;
		}
		spin_unlock(&root->inode_lock);

		for (i = 0; i < n; i++) {
			if (!delayed_nodes[i])
				continue;
			__apfs_kill_delayed_node(delayed_nodes[i]);
			apfs_release_delayed_node(delayed_nodes[i]);
		}
	}
}

void apfs_destroy_delayed_inodes(struct apfs_fs_info *fs_info)
{
	struct apfs_delayed_node *curr_node, *prev_node;

	curr_node = apfs_first_delayed_node(fs_info->delayed_root);
	while (curr_node) {
		__apfs_kill_delayed_node(curr_node);

		prev_node = curr_node;
		curr_node = apfs_next_delayed_node(curr_node);
		apfs_release_delayed_node(prev_node);
	}
}

