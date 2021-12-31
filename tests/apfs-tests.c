// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2013 Fusion IO.  All rights reserved.
 */

#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/pseudo_fs.h>
#include <linux/magic.h>
#include "apfs-tests.h"
#include "../ctree.h"
#include "../free-space-cache.h"
#include "../free-space-tree.h"
#include "../transaction.h"
#include "../volumes.h"
#include "../disk-io.h"
#include "../qgroup.h"
#include "../block-group.h"

static struct vfsmount *test_mnt = NULL;

const char *test_error[] = {
	[TEST_ALLOC_FS_INFO]	     = "cannot allocate fs_info",
	[TEST_ALLOC_ROOT]	     = "cannot allocate root",
	[TEST_ALLOC_EXTENT_BUFFER]   = "cannot extent buffer",
	[TEST_ALLOC_PATH]	     = "cannot allocate path",
	[TEST_ALLOC_INODE]	     = "cannot allocate inode",
	[TEST_ALLOC_BLOCK_GROUP]     = "cannot allocate block group",
	[TEST_ALLOC_EXTENT_MAP]      = "cannot allocate extent map",
};

static const struct super_operations apfs_test_super_ops = {
	.alloc_inode	= apfs_alloc_inode,
	.destroy_inode	= apfs_test_destroy_inode,
};


static int apfs_test_init_fs_context(struct fs_context *fc)
{
	struct pseudo_fs_context *ctx = init_pseudo(fc, APFS_TEST_MAGIC);
	if (!ctx)
		return -ENOMEM;
	ctx->ops = &apfs_test_super_ops;
	return 0;
}

static struct file_system_type test_type = {
	.name		= "apfs_test_fs",
	.init_fs_context = apfs_test_init_fs_context,
	.kill_sb	= kill_anon_super,
};

struct inode *apfs_new_test_inode(void)
{
	struct inode *inode;

	inode = new_inode(test_mnt->mnt_sb);
	if (!inode)
		return NULL;

	inode->i_mode = S_IFREG;
	APFS_I(inode)->location.type = APFS_INODE_ITEM_KEY;
	APFS_I(inode)->location.objectid = APFS_FIRST_FREE_OBJECTID;
	APFS_I(inode)->location.offset = 0;
	inode_init_owner(&init_user_ns, inode, NULL, S_IFREG);

	return inode;
}

static int apfs_init_test_fs(void)
{
	int ret;

	ret = register_filesystem(&test_type);
	if (ret) {
		trace_printk(KERN_ERR "apfs: cannot register test file system\n");
		return ret;
	}

	test_mnt = kern_mount(&test_type);
	if (IS_ERR(test_mnt)) {
		trace_printk(KERN_ERR "apfs: cannot mount test file system\n");
		unregister_filesystem(&test_type);
		return PTR_ERR(test_mnt);
	}
	return 0;
}

static void apfs_destroy_test_fs(void)
{
	kern_unmount(test_mnt);
	unregister_filesystem(&test_type);
}

struct apfs_device *apfs_alloc_dummy_device(struct apfs_fs_info *fs_info)
{
	struct apfs_device *dev;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return ERR_PTR(-ENOMEM);

	extent_io_tree_init(NULL, &dev->alloc_state, 0, NULL);
	INIT_LIST_HEAD(&dev->dev_list);
	list_add(&dev->dev_list, &fs_info->fs_devices->devices);

	return dev;
}

static void apfs_free_dummy_device(struct apfs_device *dev)
{
	extent_io_tree_release(&dev->alloc_state);
	kfree(dev);
}

struct apfs_fs_info *apfs_alloc_dummy_fs_info(u32 nodesize, u32 sectorsize)
{
	struct apfs_fs_info *fs_info = kzalloc(sizeof(struct apfs_fs_info),
						GFP_KERNEL);

	if (!fs_info)
		return fs_info;
	fs_info->fs_devices = kzalloc(sizeof(struct apfs_fs_devices),
				      GFP_KERNEL);
	if (!fs_info->fs_devices) {
		kfree(fs_info);
		return NULL;
	}
	INIT_LIST_HEAD(&fs_info->fs_devices->devices);

	fs_info->super_copy = kzalloc(sizeof(struct apfs_super_block),
				      GFP_KERNEL);
	if (!fs_info->super_copy) {
		kfree(fs_info->fs_devices);
		kfree(fs_info);
		return NULL;
	}

	apfs_init_fs_info(fs_info);

	fs_info->nodesize = nodesize;
	fs_info->sectorsize = sectorsize;
	fs_info->sectorsize_bits = ilog2(sectorsize);
	set_bit(APFS_FS_STATE_DUMMY_FS_INFO, &fs_info->fs_state);

	test_mnt->mnt_sb->s_fs_info = fs_info;

	return fs_info;
}

void apfs_free_dummy_fs_info(struct apfs_fs_info *fs_info)
{
	struct radix_tree_iter iter;
	void **slot;
	struct apfs_device *dev, *tmp;

	if (!fs_info)
		return;

	if (WARN_ON(!test_bit(APFS_FS_STATE_DUMMY_FS_INFO,
			      &fs_info->fs_state)))
		return;

	test_mnt->mnt_sb->s_fs_info = NULL;

	spin_lock(&fs_info->buffer_lock);
	radix_tree_for_each_slot(slot, &fs_info->buffer_radix, &iter, 0) {
		struct extent_buffer *eb;

		eb = radix_tree_deref_slot_protected(slot, &fs_info->buffer_lock);
		if (!eb)
			continue;
		/* Shouldn't happen but that kind of thinking creates CVE's */
		if (radix_tree_exception(eb)) {
			if (radix_tree_deref_retry(eb))
				slot = radix_tree_iter_retry(&iter);
			continue;
		}
		slot = radix_tree_iter_resume(slot, &iter);
		spin_unlock(&fs_info->buffer_lock);
		free_extent_buffer_stale(eb);
		spin_lock(&fs_info->buffer_lock);
	}
	spin_unlock(&fs_info->buffer_lock);

	apfs_mapping_tree_free(&fs_info->mapping_tree);
	list_for_each_entry_safe(dev, tmp, &fs_info->fs_devices->devices,
				 dev_list) {
		apfs_free_dummy_device(dev);
	}
	apfs_free_qgroup_config(fs_info);
	apfs_free_fs_roots(fs_info);
	kfree(fs_info->super_copy);
	apfs_check_leaked_roots(fs_info);
	apfs_extent_buffer_leak_debug_check(fs_info);
	kfree(fs_info->fs_devices);
	kfree(fs_info);
}

void apfs_free_dummy_root(struct apfs_root *root)
{
	if (!root)
		return;
	/* Will be freed by apfs_free_fs_roots */
	if (WARN_ON(test_bit(APFS_ROOT_IN_RADIX, &root->state)))
		return;
	apfs_put_root(root);
}

struct apfs_block_group *
apfs_alloc_dummy_block_group(struct apfs_fs_info *fs_info,
			      unsigned long length)
{
	struct apfs_block_group *cache;

	cache = kzalloc(sizeof(*cache), GFP_KERNEL);
	if (!cache)
		return NULL;
	cache->free_space_ctl = kzalloc(sizeof(*cache->free_space_ctl),
					GFP_KERNEL);
	if (!cache->free_space_ctl) {
		kfree(cache);
		return NULL;
	}

	cache->start = 0;
	cache->length = length;
	cache->full_stripe_len = fs_info->sectorsize;
	cache->fs_info = fs_info;

	INIT_LIST_HEAD(&cache->list);
	INIT_LIST_HEAD(&cache->cluster_list);
	INIT_LIST_HEAD(&cache->bg_list);
	apfs_init_free_space_ctl(cache, cache->free_space_ctl);
	mutex_init(&cache->free_space_lock);

	return cache;
}

void apfs_free_dummy_block_group(struct apfs_block_group *cache)
{
	if (!cache)
		return;
	__apfs_remove_free_space_cache(cache->free_space_ctl);
	kfree(cache->free_space_ctl);
	kfree(cache);
}

void apfs_init_dummy_trans(struct apfs_trans_handle *trans,
			    struct apfs_fs_info *fs_info)
{
	memset(trans, 0, sizeof(*trans));
	trans->transid = 1;
	trans->type = __TRANS_DUMMY;
	trans->fs_info = fs_info;
}

int apfs_run_sanity_tests(void)
{
	int ret, i;
	u32 sectorsize, nodesize;
	u32 test_sectorsize[] = {
		PAGE_SIZE,
	};
	ret = apfs_init_test_fs();
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(test_sectorsize); i++) {
		sectorsize = test_sectorsize[i];
		for (nodesize = sectorsize;
		     nodesize <= APFS_MAX_METADATA_BLOCKSIZE;
		     nodesize <<= 1) {
			pr_info("APFS: selftest: sectorsize: %u  nodesize: %u\n",
				sectorsize, nodesize);
			ret = apfs_test_free_space_cache(sectorsize, nodesize);
			if (ret)
				goto out;
			ret = apfs_test_extent_buffer_operations(sectorsize,
				nodesize);
			if (ret)
				goto out;
			ret = apfs_test_extent_io(sectorsize, nodesize);
			if (ret)
				goto out;
			ret = apfs_test_inodes(sectorsize, nodesize);
			if (ret)
				goto out;
			ret = apfs_test_qgroups(sectorsize, nodesize);
			if (ret)
				goto out;
			ret = apfs_test_free_space_tree(sectorsize, nodesize);
			if (ret)
				goto out;
		}
	}
	ret = apfs_test_extent_map();

out:
	apfs_destroy_test_fs();
	return ret;
}
