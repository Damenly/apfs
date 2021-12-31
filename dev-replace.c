// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) STRATO AG 2012.  All rights reserved.
 */

#include <linux/sched.h>
#include <linux/bio.h>
#include <linux/slab.h>
#include <linux/blkdev.h>
#include <linux/kthread.h>
#include <linux/math64.h>
#include "misc.h"
#include "ctree.h"
#include "extent_map.h"
#include "disk-io.h"
#include "transaction.h"
#include "print-tree.h"
#include "volumes.h"
#include "async-thread.h"
#include "check-integrity.h"
#include "rcu-string.h"
#include "dev-replace.h"
#include "sysfs.h"
#include "zoned.h"
#include "block-group.h"

/*
 * Device replace overview
 *
 * [Objective]
 * To copy all extents (both new and on-disk) from source device to target
 * device, while still keeping the filesystem read-write.
 *
 * [Method]
 * There are two main methods involved:
 *
 * - Write duplication
 *
 *   All new writes will be written to both target and source devices, so even
 *   if replace gets canceled, sources device still contains up-to-date data.
 *
 *   Location:		handle_ops_on_dev_replace() from __apfs_map_block()
 *   Start:		apfs_dev_replace_start()
 *   End:		apfs_dev_replace_finishing()
 *   Content:		Latest data/metadata
 *
 * - Copy existing extents
 *
 *   This happens by re-using scrub facility, as scrub also iterates through
 *   existing extents from commit root.
 *
 *   Location:		scrub_write_block_to_dev_replace() from
 *   			scrub_block_complete()
 *   Content:		Data/meta from commit root.
 *
 * Due to the content difference, we need to avoid nocow write when dev-replace
 * is happening.  This is done by marking the block group read-only and waiting
 * for NOCOW writes.
 *
 * After replace is done, the finishing part is done by swapping the target and
 * source devices.
 *
 *   Location:		apfs_dev_replace_update_device_in_mapping_tree() from
 *   			apfs_dev_replace_finishing()
 */

static int apfs_dev_replace_finishing(struct apfs_fs_info *fs_info,
				       int scrub_ret);
static int apfs_dev_replace_kthread(void *data);

int apfs_init_dev_replace(struct apfs_fs_info *fs_info)
{
	struct apfs_key key = {};
	struct apfs_root *dev_root = fs_info->dev_root;
	struct apfs_dev_replace *dev_replace = &fs_info->dev_replace;
	struct extent_buffer *eb;
	int slot;
	int ret = 0;
	struct apfs_path *path = NULL;
	int item_size;
	struct apfs_dev_replace_item *ptr;
	u64 src_devid;

	if (!dev_root)
		return 0;

	path = apfs_alloc_path();
	if (!path) {
		ret = -ENOMEM;
		goto out;
	}

	key.objectid = 0;
	key.type = APFS_DEV_REPLACE_KEY;
	key.offset = 0;
	ret = apfs_search_slot(NULL, dev_root, &key, path, 0, 0);
	if (ret) {
no_valid_dev_replace_entry_found:
		/*
		 * We don't have a replace item or it's corrupted.  If there is
		 * a replace target, fail the mount.
		 */
		if (apfs_find_device(fs_info->fs_devices,
				      APFS_DEV_REPLACE_DEVID, NULL, NULL)) {
			apfs_err(fs_info,
			"found replace target device without a valid replace item");
			ret = -EUCLEAN;
			goto out;
		}
		ret = 0;
		dev_replace->replace_state =
			APFS_IOCTL_DEV_REPLACE_STATE_NEVER_STARTED;
		dev_replace->cont_reading_from_srcdev_mode =
		    APFS_DEV_REPLACE_ITEM_CONT_READING_FROM_SRCDEV_MODE_ALWAYS;
		dev_replace->time_started = 0;
		dev_replace->time_stopped = 0;
		atomic64_set(&dev_replace->num_write_errors, 0);
		atomic64_set(&dev_replace->num_uncorrectable_read_errors, 0);
		dev_replace->cursor_left = 0;
		dev_replace->committed_cursor_left = 0;
		dev_replace->cursor_left_last_write_of_item = 0;
		dev_replace->cursor_right = 0;
		dev_replace->srcdev = NULL;
		dev_replace->tgtdev = NULL;
		dev_replace->is_valid = 0;
		dev_replace->item_needs_writeback = 0;
		goto out;
	}
	slot = path->slots[0];
	eb = path->nodes[0];
	item_size = apfs_item_size_nr(eb, slot);
	ptr = apfs_item_ptr(eb, slot, struct apfs_dev_replace_item);

	if (item_size != sizeof(struct apfs_dev_replace_item)) {
		apfs_warn(fs_info,
			"dev_replace entry found has unexpected size, ignore entry");
		goto no_valid_dev_replace_entry_found;
	}

	src_devid = apfs_dev_replace_src_devid(eb, ptr);
	dev_replace->cont_reading_from_srcdev_mode =
		apfs_dev_replace_cont_reading_from_srcdev_mode(eb, ptr);
	dev_replace->replace_state = apfs_dev_replace_replace_state(eb, ptr);
	dev_replace->time_started = apfs_dev_replace_time_started(eb, ptr);
	dev_replace->time_stopped =
		apfs_dev_replace_time_stopped(eb, ptr);
	atomic64_set(&dev_replace->num_write_errors,
		     apfs_dev_replace_num_write_errors(eb, ptr));
	atomic64_set(&dev_replace->num_uncorrectable_read_errors,
		     apfs_dev_replace_num_uncorrectable_read_errors(eb, ptr));
	dev_replace->cursor_left = apfs_dev_replace_cursor_left(eb, ptr);
	dev_replace->committed_cursor_left = dev_replace->cursor_left;
	dev_replace->cursor_left_last_write_of_item = dev_replace->cursor_left;
	dev_replace->cursor_right = apfs_dev_replace_cursor_right(eb, ptr);
	dev_replace->is_valid = 1;

	dev_replace->item_needs_writeback = 0;
	switch (dev_replace->replace_state) {
	case APFS_IOCTL_DEV_REPLACE_STATE_NEVER_STARTED:
	case APFS_IOCTL_DEV_REPLACE_STATE_FINISHED:
	case APFS_IOCTL_DEV_REPLACE_STATE_CANCELED:
		/*
		 * We don't have an active replace item but if there is a
		 * replace target, fail the mount.
		 */
		if (apfs_find_device(fs_info->fs_devices,
				      APFS_DEV_REPLACE_DEVID, NULL, NULL)) {
			apfs_err(fs_info,
			"replace devid present without an active replace item");
			ret = -EUCLEAN;
		} else {
			dev_replace->srcdev = NULL;
			dev_replace->tgtdev = NULL;
		}
		break;
	case APFS_IOCTL_DEV_REPLACE_STATE_STARTED:
	case APFS_IOCTL_DEV_REPLACE_STATE_SUSPENDED:
		dev_replace->srcdev = apfs_find_device(fs_info->fs_devices,
						src_devid, NULL, NULL);
		dev_replace->tgtdev = apfs_find_device(fs_info->fs_devices,
							APFS_DEV_REPLACE_DEVID,
							NULL, NULL);
		/*
		 * allow 'apfs dev replace_cancel' if src/tgt device is
		 * missing
		 */
		if (!dev_replace->srcdev &&
		    !apfs_test_opt(fs_info, DEGRADED)) {
			ret = -EIO;
			apfs_warn(fs_info,
			   "cannot mount because device replace operation is ongoing and");
			apfs_warn(fs_info,
			   "srcdev (devid %llu) is missing, need to run 'apfs dev scan'?",
			   src_devid);
		}
		if (!dev_replace->tgtdev &&
		    !apfs_test_opt(fs_info, DEGRADED)) {
			ret = -EIO;
			apfs_warn(fs_info,
			   "cannot mount because device replace operation is ongoing and");
			apfs_warn(fs_info,
			   "tgtdev (devid %llu) is missing, need to run 'apfs dev scan'?",
				APFS_DEV_REPLACE_DEVID);
		}
		if (dev_replace->tgtdev) {
			if (dev_replace->srcdev) {
				dev_replace->tgtdev->total_bytes =
					dev_replace->srcdev->total_bytes;
				dev_replace->tgtdev->disk_total_bytes =
					dev_replace->srcdev->disk_total_bytes;
				dev_replace->tgtdev->commit_total_bytes =
					dev_replace->srcdev->commit_total_bytes;
				dev_replace->tgtdev->bytes_used =
					dev_replace->srcdev->bytes_used;
				dev_replace->tgtdev->commit_bytes_used =
					dev_replace->srcdev->commit_bytes_used;
			}
			set_bit(APFS_DEV_STATE_REPLACE_TGT,
				&dev_replace->tgtdev->dev_state);

			WARN_ON(fs_info->fs_devices->rw_devices == 0);
			dev_replace->tgtdev->io_width = fs_info->sectorsize;
			dev_replace->tgtdev->io_align = fs_info->sectorsize;
			dev_replace->tgtdev->sector_size = fs_info->sectorsize;
			dev_replace->tgtdev->fs_info = fs_info;
			set_bit(APFS_DEV_STATE_IN_FS_METADATA,
				&dev_replace->tgtdev->dev_state);
		}
		break;
	}

out:
	apfs_free_path(path);
	return ret;
}

/*
 * Initialize a new device for device replace target from a given source dev
 * and path.
 *
 * Return 0 and new device in @device_out, otherwise return < 0
 */
static int apfs_init_dev_replace_tgtdev(struct apfs_fs_info *fs_info,
				  const char *device_path,
				  struct apfs_device *srcdev,
				  struct apfs_device **device_out)
{
	struct apfs_device *device;
	struct block_device *bdev;
	struct rcu_string *name;
	u64 devid = APFS_DEV_REPLACE_DEVID;
	int ret = 0;

	*device_out = NULL;
	if (srcdev->fs_devices->seeding) {
		apfs_err(fs_info, "the filesystem is a seed filesystem!");
		return -EINVAL;
	}

	bdev = blkdev_get_by_path(device_path, FMODE_WRITE | FMODE_EXCL,
				  fs_info->bdev_holder);
	if (IS_ERR(bdev)) {
		apfs_err(fs_info, "target device %s is invalid!", device_path);
		return PTR_ERR(bdev);
	}

	if (!apfs_check_device_zone_type(fs_info, bdev)) {
		apfs_err(fs_info,
		"dev-replace: zoned type of target device mismatch with filesystem");
		ret = -EINVAL;
		goto error;
	}

	sync_blockdev(bdev);

	list_for_each_entry(device, &fs_info->fs_devices->devices, dev_list) {
		if (device->bdev == bdev) {
			apfs_err(fs_info,
				  "target device is in the filesystem!");
			ret = -EEXIST;
			goto error;
		}
	}


	if (i_size_read(bdev->bd_inode) <
	    apfs_device_get_total_bytes(srcdev)) {
		apfs_err(fs_info,
			  "target device is smaller than source device!");
		ret = -EINVAL;
		goto error;
	}


	device = apfs_alloc_device(NULL, &devid, NULL);
	if (IS_ERR(device)) {
		ret = PTR_ERR(device);
		goto error;
	}

	name = rcu_string_strdup(device_path, GFP_KERNEL);
	if (!name) {
		apfs_free_device(device);
		ret = -ENOMEM;
		goto error;
	}
	rcu_assign_pointer(device->name, name);

	set_bit(APFS_DEV_STATE_WRITEABLE, &device->dev_state);
	device->generation = 0;
	device->io_width = fs_info->sectorsize;
	device->io_align = fs_info->sectorsize;
	device->sector_size = fs_info->sectorsize;
	device->total_bytes = apfs_device_get_total_bytes(srcdev);
	device->disk_total_bytes = apfs_device_get_disk_total_bytes(srcdev);
	device->bytes_used = apfs_device_get_bytes_used(srcdev);
	device->commit_total_bytes = srcdev->commit_total_bytes;
	device->commit_bytes_used = device->bytes_used;
	device->fs_info = fs_info;
	device->bdev = bdev;
	set_bit(APFS_DEV_STATE_IN_FS_METADATA, &device->dev_state);
	set_bit(APFS_DEV_STATE_REPLACE_TGT, &device->dev_state);
	device->mode = FMODE_EXCL;
	device->dev_stats_valid = 1;
	set_blocksize(device->bdev, APFS_BDEV_BLOCKSIZE);
	device->fs_devices = fs_info->fs_devices;

	ret = apfs_get_dev_zone_info(device);
	if (ret)
		goto error;

	mutex_lock(&fs_info->fs_devices->device_list_mutex);
	list_add(&device->dev_list, &fs_info->fs_devices->devices);
	fs_info->fs_devices->num_devices++;
	fs_info->fs_devices->open_devices++;
	mutex_unlock(&fs_info->fs_devices->device_list_mutex);

	*device_out = device;
	return 0;

error:
	blkdev_put(bdev, FMODE_EXCL);
	return ret;
}

/*
 * called from commit_transaction. Writes changed device replace state to
 * disk.
 */
int apfs_run_dev_replace(struct apfs_trans_handle *trans)
{
	struct apfs_fs_info *fs_info = trans->fs_info;
	int ret;
	struct apfs_root *dev_root = fs_info->dev_root;
	struct apfs_path *path;
	struct apfs_key key = {};
	struct extent_buffer *eb;
	struct apfs_dev_replace_item *ptr;
	struct apfs_dev_replace *dev_replace = &fs_info->dev_replace;

	down_read(&dev_replace->rwsem);
	if (!dev_replace->is_valid ||
	    !dev_replace->item_needs_writeback) {
		up_read(&dev_replace->rwsem);
		return 0;
	}
	up_read(&dev_replace->rwsem);

	key.objectid = 0;
	key.type = APFS_DEV_REPLACE_KEY;
	key.offset = 0;

	path = apfs_alloc_path();
	if (!path) {
		ret = -ENOMEM;
		goto out;
	}
	ret = apfs_search_slot(trans, dev_root, &key, path, -1, 1);
	if (ret < 0) {
		apfs_warn(fs_info,
			   "error %d while searching for dev_replace item!",
			   ret);
		goto out;
	}

	if (ret == 0 &&
	    apfs_item_size_nr(path->nodes[0], path->slots[0]) < sizeof(*ptr)) {
		/*
		 * need to delete old one and insert a new one.
		 * Since no attempt is made to recover any old state, if the
		 * dev_replace state is 'running', the data on the target
		 * drive is lost.
		 * It would be possible to recover the state: just make sure
		 * that the beginning of the item is never changed and always
		 * contains all the essential information. Then read this
		 * minimal set of information and use it as a base for the
		 * new state.
		 */
		ret = apfs_del_item(trans, dev_root, path);
		if (ret != 0) {
			apfs_warn(fs_info,
				   "delete too small dev_replace item failed %d!",
				   ret);
			goto out;
		}
		ret = 1;
	}

	if (ret == 1) {
		/* need to insert a new item */
		apfs_release_path(path);
		ret = apfs_insert_empty_item(trans, dev_root, path,
					      &key, sizeof(*ptr));
		if (ret < 0) {
			apfs_warn(fs_info,
				   "insert dev_replace item failed %d!", ret);
			goto out;
		}
	}

	eb = path->nodes[0];
	ptr = apfs_item_ptr(eb, path->slots[0],
			     struct apfs_dev_replace_item);

	down_write(&dev_replace->rwsem);
	if (dev_replace->srcdev)
		apfs_set_dev_replace_src_devid(eb, ptr,
			dev_replace->srcdev->devid);
	else
		apfs_set_dev_replace_src_devid(eb, ptr, (u64)-1);
	apfs_set_dev_replace_cont_reading_from_srcdev_mode(eb, ptr,
		dev_replace->cont_reading_from_srcdev_mode);
	apfs_set_dev_replace_replace_state(eb, ptr,
		dev_replace->replace_state);
	apfs_set_dev_replace_time_started(eb, ptr, dev_replace->time_started);
	apfs_set_dev_replace_time_stopped(eb, ptr, dev_replace->time_stopped);
	apfs_set_dev_replace_num_write_errors(eb, ptr,
		atomic64_read(&dev_replace->num_write_errors));
	apfs_set_dev_replace_num_uncorrectable_read_errors(eb, ptr,
		atomic64_read(&dev_replace->num_uncorrectable_read_errors));
	dev_replace->cursor_left_last_write_of_item =
		dev_replace->cursor_left;
	apfs_set_dev_replace_cursor_left(eb, ptr,
		dev_replace->cursor_left_last_write_of_item);
	apfs_set_dev_replace_cursor_right(eb, ptr,
		dev_replace->cursor_right);
	dev_replace->item_needs_writeback = 0;
	up_write(&dev_replace->rwsem);

	apfs_mark_buffer_dirty(eb);

out:
	apfs_free_path(path);

	return ret;
}

static char* apfs_dev_name(struct apfs_device *device)
{
	if (!device || test_bit(APFS_DEV_STATE_MISSING, &device->dev_state))
		return "<missing disk>";
	else
		return rcu_str_deref(device->name);
}

static int mark_block_group_to_copy(struct apfs_fs_info *fs_info,
				    struct apfs_device *src_dev)
{
	struct apfs_path *path;
	struct apfs_key key = {};
	struct apfs_key found_key = {};
	struct apfs_root *root = fs_info->dev_root;
	struct apfs_dev_extent *dev_extent = NULL;
	struct apfs_block_group *cache;
	struct apfs_trans_handle *trans;
	int ret = 0;
	u64 chunk_offset;

	/* Do not use "to_copy" on non zoned filesystem for now */
	if (!apfs_is_zoned(fs_info))
		return 0;

	mutex_lock(&fs_info->chunk_mutex);

	/* Ensure we don't have pending new block group */
	spin_lock(&fs_info->trans_lock);
	while (fs_info->running_transaction &&
	       !list_empty(&fs_info->running_transaction->dev_update_list)) {
		spin_unlock(&fs_info->trans_lock);
		mutex_unlock(&fs_info->chunk_mutex);
		trans = apfs_attach_transaction(root);
		if (IS_ERR(trans)) {
			ret = PTR_ERR(trans);
			mutex_lock(&fs_info->chunk_mutex);
			if (ret == -ENOENT) {
				spin_lock(&fs_info->trans_lock);
				continue;
			} else {
				goto unlock;
			}
		}

		ret = apfs_commit_transaction(trans);
		mutex_lock(&fs_info->chunk_mutex);
		if (ret)
			goto unlock;

		spin_lock(&fs_info->trans_lock);
	}
	spin_unlock(&fs_info->trans_lock);

	path = apfs_alloc_path();
	if (!path) {
		ret = -ENOMEM;
		goto unlock;
	}

	path->reada = READA_FORWARD;
	path->search_commit_root = 1;
	path->skip_locking = 1;

	key.objectid = src_dev->devid;
	key.type = APFS_DEV_EXTENT_KEY;
	key.offset = 0;

	ret = apfs_search_slot(NULL, root, &key, path, 0, 0);
	if (ret < 0)
		goto free_path;
	if (ret > 0) {
		if (path->slots[0] >=
		    apfs_header_nritems(path->nodes[0])) {
			ret = apfs_next_leaf(root, path);
			if (ret < 0)
				goto free_path;
			if (ret > 0) {
				ret = 0;
				goto free_path;
			}
		} else {
			ret = 0;
		}
	}

	while (1) {
		struct extent_buffer *leaf = path->nodes[0];
		int slot = path->slots[0];

		apfs_item_key_to_cpu(leaf, &found_key, slot);

		if (found_key.objectid != src_dev->devid)
			break;

		if (found_key.type != APFS_DEV_EXTENT_KEY)
			break;

		if (found_key.offset < key.offset)
			break;

		dev_extent = apfs_item_ptr(leaf, slot, struct apfs_dev_extent);

		chunk_offset = apfs_dev_extent_chunk_offset(leaf, dev_extent);

		cache = apfs_lookup_block_group(fs_info, chunk_offset);
		if (!cache)
			goto skip;

		spin_lock(&cache->lock);
		cache->to_copy = 1;
		spin_unlock(&cache->lock);

		apfs_put_block_group(cache);

skip:
		ret = apfs_next_item(root, path);
		if (ret != 0) {
			if (ret > 0)
				ret = 0;
			break;
		}
	}

free_path:
	apfs_free_path(path);
unlock:
	mutex_unlock(&fs_info->chunk_mutex);

	return ret;
}

bool apfs_finish_block_group_to_copy(struct apfs_device *srcdev,
				      struct apfs_block_group *cache,
				      u64 physical)
{
	struct apfs_fs_info *fs_info = cache->fs_info;
	struct extent_map *em;
	struct map_lookup *map;
	u64 chunk_offset = cache->start;
	int num_extents, cur_extent;
	int i;

	/* Do not use "to_copy" on non zoned filesystem for now */
	if (!apfs_is_zoned(fs_info))
		return true;

	spin_lock(&cache->lock);
	if (cache->removed) {
		spin_unlock(&cache->lock);
		return true;
	}
	spin_unlock(&cache->lock);

	em = apfs_get_chunk_map(fs_info, chunk_offset, 1);
	ASSERT(!IS_ERR(em));
	map = em->map_lookup;

	num_extents = cur_extent = 0;
	for (i = 0; i < map->num_stripes; i++) {
		/* We have more device extent to copy */
		if (srcdev != map->stripes[i].dev)
			continue;

		num_extents++;
		if (physical == map->stripes[i].physical)
			cur_extent = i;
	}

	free_extent_map(em);

	if (num_extents > 1 && cur_extent < num_extents - 1) {
		/*
		 * Has more stripes on this device. Keep this block group
		 * readonly until we finish all the stripes.
		 */
		return false;
	}

	/* Last stripe on this device */
	spin_lock(&cache->lock);
	cache->to_copy = 0;
	spin_unlock(&cache->lock);

	return true;
}

static int apfs_dev_replace_start(struct apfs_fs_info *fs_info,
		const char *tgtdev_name, u64 srcdevid, const char *srcdev_name,
		int read_src)
{
	struct apfs_root *root = fs_info->dev_root;
	struct apfs_trans_handle *trans;
	struct apfs_dev_replace *dev_replace = &fs_info->dev_replace;
	int ret;
	struct apfs_device *tgt_device = NULL;
	struct apfs_device *src_device = NULL;

	src_device = apfs_find_device_by_devspec(fs_info, srcdevid,
						  srcdev_name);
	if (IS_ERR(src_device))
		return PTR_ERR(src_device);

	if (apfs_pinned_by_swapfile(fs_info, src_device)) {
		apfs_warn_in_rcu(fs_info,
	  "cannot replace device %s (devid %llu) due to active swapfile",
			apfs_dev_name(src_device), src_device->devid);
		return -ETXTBSY;
	}

	/*
	 * Here we commit the transaction to make sure commit_total_bytes
	 * of all the devices are updated.
	 */
	trans = apfs_attach_transaction(root);
	if (!IS_ERR(trans)) {
		ret = apfs_commit_transaction(trans);
		if (ret)
			return ret;
	} else if (PTR_ERR(trans) != -ENOENT) {
		return PTR_ERR(trans);
	}

	ret = apfs_init_dev_replace_tgtdev(fs_info, tgtdev_name,
					    src_device, &tgt_device);
	if (ret)
		return ret;

	ret = mark_block_group_to_copy(fs_info, src_device);
	if (ret)
		return ret;

	down_write(&dev_replace->rwsem);
	switch (dev_replace->replace_state) {
	case APFS_IOCTL_DEV_REPLACE_STATE_NEVER_STARTED:
	case APFS_IOCTL_DEV_REPLACE_STATE_FINISHED:
	case APFS_IOCTL_DEV_REPLACE_STATE_CANCELED:
		break;
	case APFS_IOCTL_DEV_REPLACE_STATE_STARTED:
	case APFS_IOCTL_DEV_REPLACE_STATE_SUSPENDED:
		ASSERT(0);
		ret = APFS_IOCTL_DEV_REPLACE_RESULT_ALREADY_STARTED;
		up_write(&dev_replace->rwsem);
		goto leave;
	}

	dev_replace->cont_reading_from_srcdev_mode = read_src;
	dev_replace->srcdev = src_device;
	dev_replace->tgtdev = tgt_device;

	apfs_info_in_rcu(fs_info,
		      "dev_replace from %s (devid %llu) to %s started",
		      apfs_dev_name(src_device),
		      src_device->devid,
		      rcu_str_deref(tgt_device->name));

	/*
	 * from now on, the writes to the srcdev are all duplicated to
	 * go to the tgtdev as well (refer to apfs_map_block()).
	 */
	dev_replace->replace_state = APFS_IOCTL_DEV_REPLACE_STATE_STARTED;
	dev_replace->time_started = ktime_get_real_seconds();
	dev_replace->cursor_left = 0;
	dev_replace->committed_cursor_left = 0;
	dev_replace->cursor_left_last_write_of_item = 0;
	dev_replace->cursor_right = 0;
	dev_replace->is_valid = 1;
	dev_replace->item_needs_writeback = 1;
	atomic64_set(&dev_replace->num_write_errors, 0);
	atomic64_set(&dev_replace->num_uncorrectable_read_errors, 0);
	up_write(&dev_replace->rwsem);

	ret = apfs_sysfs_add_device(tgt_device);
	if (ret)
		apfs_err(fs_info, "kobj add dev failed %d", ret);

	apfs_wait_ordered_roots(fs_info, U64_MAX, 0, (u64)-1);

	/* Commit dev_replace state and reserve 1 item for it. */
	trans = apfs_start_transaction(root, 1);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		down_write(&dev_replace->rwsem);
		dev_replace->replace_state =
			APFS_IOCTL_DEV_REPLACE_STATE_NEVER_STARTED;
		dev_replace->srcdev = NULL;
		dev_replace->tgtdev = NULL;
		up_write(&dev_replace->rwsem);
		goto leave;
	}

	ret = apfs_commit_transaction(trans);
	WARN_ON(ret);

	/* the disk copy procedure reuses the scrub code */
	ret = apfs_scrub_dev(fs_info, src_device->devid, 0,
			      apfs_device_get_total_bytes(src_device),
			      &dev_replace->scrub_progress, 0, 1);

	ret = apfs_dev_replace_finishing(fs_info, ret);
	if (ret == -EINPROGRESS)
		ret = APFS_IOCTL_DEV_REPLACE_RESULT_SCRUB_INPROGRESS;

	return ret;

leave:
	apfs_destroy_dev_replace_tgtdev(tgt_device);
	return ret;
}

int apfs_dev_replace_by_ioctl(struct apfs_fs_info *fs_info,
			    struct apfs_ioctl_dev_replace_args *args)
{
	int ret;

	switch (args->start.cont_reading_from_srcdev_mode) {
	case APFS_IOCTL_DEV_REPLACE_CONT_READING_FROM_SRCDEV_MODE_ALWAYS:
	case APFS_IOCTL_DEV_REPLACE_CONT_READING_FROM_SRCDEV_MODE_AVOID:
		break;
	default:
		return -EINVAL;
	}

	if ((args->start.srcdevid == 0 && args->start.srcdev_name[0] == '\0') ||
	    args->start.tgtdev_name[0] == '\0')
		return -EINVAL;

	ret = apfs_dev_replace_start(fs_info, args->start.tgtdev_name,
					args->start.srcdevid,
					args->start.srcdev_name,
					args->start.cont_reading_from_srcdev_mode);
	args->result = ret;
	/* don't warn if EINPROGRESS, someone else might be running scrub */
	if (ret == APFS_IOCTL_DEV_REPLACE_RESULT_SCRUB_INPROGRESS ||
	    ret == APFS_IOCTL_DEV_REPLACE_RESULT_NO_ERROR)
		return 0;

	return ret;
}

/*
 * blocked until all in-flight bios operations are finished.
 */
static void apfs_rm_dev_replace_blocked(struct apfs_fs_info *fs_info)
{
	set_bit(APFS_FS_STATE_DEV_REPLACING, &fs_info->fs_state);
	wait_event(fs_info->dev_replace.replace_wait, !percpu_counter_sum(
		   &fs_info->dev_replace.bio_counter));
}

/*
 * we have removed target device, it is safe to allow new bios request.
 */
static void apfs_rm_dev_replace_unblocked(struct apfs_fs_info *fs_info)
{
	clear_bit(APFS_FS_STATE_DEV_REPLACING, &fs_info->fs_state);
	wake_up(&fs_info->dev_replace.replace_wait);
}

/*
 * When finishing the device replace, before swapping the source device with the
 * target device we must update the chunk allocation state in the target device,
 * as it is empty because replace works by directly copying the chunks and not
 * through the normal chunk allocation path.
 */
static int apfs_set_target_alloc_state(struct apfs_device *srcdev,
					struct apfs_device *tgtdev)
{
	struct extent_state *cached_state = NULL;
	u64 start = 0;
	u64 found_start;
	u64 found_end;
	int ret = 0;

	lockdep_assert_held(&srcdev->fs_info->chunk_mutex);

	while (!find_first_extent_bit(&srcdev->alloc_state, start,
				      &found_start, &found_end,
				      CHUNK_ALLOCATED, &cached_state)) {
		ret = set_extent_bits(&tgtdev->alloc_state, found_start,
				      found_end, CHUNK_ALLOCATED);
		if (ret)
			break;
		start = found_end + 1;
	}

	free_extent_state(cached_state);
	return ret;
}

static void apfs_dev_replace_update_device_in_mapping_tree(
						struct apfs_fs_info *fs_info,
						struct apfs_device *srcdev,
						struct apfs_device *tgtdev)
{
	struct extent_map_tree *em_tree = &fs_info->mapping_tree;
	struct extent_map *em;
	struct map_lookup *map;
	u64 start = 0;
	int i;

	write_lock(&em_tree->lock);
	do {
		em = lookup_extent_mapping(em_tree, start, (u64)-1);
		if (!em)
			break;
		map = em->map_lookup;
		for (i = 0; i < map->num_stripes; i++)
			if (srcdev == map->stripes[i].dev)
				map->stripes[i].dev = tgtdev;
		start = em->start + em->len;
		free_extent_map(em);
	} while (start);
	write_unlock(&em_tree->lock);
}

static int apfs_dev_replace_finishing(struct apfs_fs_info *fs_info,
				       int scrub_ret)
{
	struct apfs_dev_replace *dev_replace = &fs_info->dev_replace;
	struct apfs_device *tgt_device;
	struct apfs_device *src_device;
	struct apfs_root *root = fs_info->tree_root;
	u8 uuid_tmp[APFS_UUID_SIZE];
	struct apfs_trans_handle *trans;
	int ret = 0;

	/* don't allow cancel or unmount to disturb the finishing procedure */
	mutex_lock(&dev_replace->lock_finishing_cancel_unmount);

	down_read(&dev_replace->rwsem);
	/* was the operation canceled, or is it finished? */
	if (dev_replace->replace_state !=
	    APFS_IOCTL_DEV_REPLACE_STATE_STARTED) {
		up_read(&dev_replace->rwsem);
		mutex_unlock(&dev_replace->lock_finishing_cancel_unmount);
		return 0;
	}

	tgt_device = dev_replace->tgtdev;
	src_device = dev_replace->srcdev;
	up_read(&dev_replace->rwsem);

	/*
	 * flush all outstanding I/O and inode extent mappings before the
	 * copy operation is declared as being finished
	 */
	ret = apfs_start_delalloc_roots(fs_info, LONG_MAX, false);
	if (ret) {
		mutex_unlock(&dev_replace->lock_finishing_cancel_unmount);
		return ret;
	}
	apfs_wait_ordered_roots(fs_info, U64_MAX, 0, (u64)-1);

	if (!scrub_ret)
		apfs_reada_remove_dev(src_device);

	/*
	 * We have to use this loop approach because at this point src_device
	 * has to be available for transaction commit to complete, yet new
	 * chunks shouldn't be allocated on the device.
	 */
	while (1) {
		trans = apfs_start_transaction(root, 0);
		if (IS_ERR(trans)) {
			apfs_reada_undo_remove_dev(src_device);
			mutex_unlock(&dev_replace->lock_finishing_cancel_unmount);
			return PTR_ERR(trans);
		}
		ret = apfs_commit_transaction(trans);
		WARN_ON(ret);

		/* Prevent write_all_supers() during the finishing procedure */
		mutex_lock(&fs_info->fs_devices->device_list_mutex);
		/* Prevent new chunks being allocated on the source device */
		mutex_lock(&fs_info->chunk_mutex);

		if (!list_empty(&src_device->post_commit_list)) {
			mutex_unlock(&fs_info->fs_devices->device_list_mutex);
			mutex_unlock(&fs_info->chunk_mutex);
		} else {
			break;
		}
	}

	down_write(&dev_replace->rwsem);
	dev_replace->replace_state =
		scrub_ret ? APFS_IOCTL_DEV_REPLACE_STATE_CANCELED
			  : APFS_IOCTL_DEV_REPLACE_STATE_FINISHED;
	dev_replace->tgtdev = NULL;
	dev_replace->srcdev = NULL;
	dev_replace->time_stopped = ktime_get_real_seconds();
	dev_replace->item_needs_writeback = 1;

	/*
	 * Update allocation state in the new device and replace the old device
	 * with the new one in the mapping tree.
	 */
	if (!scrub_ret) {
		scrub_ret = apfs_set_target_alloc_state(src_device, tgt_device);
		if (scrub_ret)
			goto error;
		apfs_dev_replace_update_device_in_mapping_tree(fs_info,
								src_device,
								tgt_device);
	} else {
		if (scrub_ret != -ECANCELED)
			apfs_err_in_rcu(fs_info,
				 "apfs_scrub_dev(%s, %llu, %s) failed %d",
				 apfs_dev_name(src_device),
				 src_device->devid,
				 rcu_str_deref(tgt_device->name), scrub_ret);
error:
		up_write(&dev_replace->rwsem);
		mutex_unlock(&fs_info->chunk_mutex);
		mutex_unlock(&fs_info->fs_devices->device_list_mutex);
		apfs_reada_undo_remove_dev(src_device);
		apfs_rm_dev_replace_blocked(fs_info);
		if (tgt_device)
			apfs_destroy_dev_replace_tgtdev(tgt_device);
		apfs_rm_dev_replace_unblocked(fs_info);
		mutex_unlock(&dev_replace->lock_finishing_cancel_unmount);

		return scrub_ret;
	}

	apfs_info_in_rcu(fs_info,
			  "dev_replace from %s (devid %llu) to %s finished",
			  apfs_dev_name(src_device),
			  src_device->devid,
			  rcu_str_deref(tgt_device->name));
	clear_bit(APFS_DEV_STATE_REPLACE_TGT, &tgt_device->dev_state);
	tgt_device->devid = src_device->devid;
	src_device->devid = APFS_DEV_REPLACE_DEVID;
	memcpy(uuid_tmp, tgt_device->uuid, sizeof(uuid_tmp));
	memcpy(tgt_device->uuid, src_device->uuid, sizeof(tgt_device->uuid));
	memcpy(src_device->uuid, uuid_tmp, sizeof(src_device->uuid));
	apfs_device_set_total_bytes(tgt_device, src_device->total_bytes);
	apfs_device_set_disk_total_bytes(tgt_device,
					  src_device->disk_total_bytes);
	apfs_device_set_bytes_used(tgt_device, src_device->bytes_used);
	tgt_device->commit_bytes_used = src_device->bytes_used;

	apfs_assign_next_active_device(src_device, tgt_device);

	list_add(&tgt_device->dev_alloc_list, &fs_info->fs_devices->alloc_list);
	fs_info->fs_devices->rw_devices++;

	up_write(&dev_replace->rwsem);
	apfs_rm_dev_replace_blocked(fs_info);

	apfs_rm_dev_replace_remove_srcdev(src_device);

	apfs_rm_dev_replace_unblocked(fs_info);

	/*
	 * Increment dev_stats_ccnt so that apfs_run_dev_stats() will
	 * update on-disk dev stats value during commit transaction
	 */
	atomic_inc(&tgt_device->dev_stats_ccnt);

	/*
	 * this is again a consistent state where no dev_replace procedure
	 * is running, the target device is part of the filesystem, the
	 * source device is not part of the filesystem anymore and its 1st
	 * superblock is scratched out so that it is no longer marked to
	 * belong to this filesystem.
	 */
	mutex_unlock(&fs_info->chunk_mutex);
	mutex_unlock(&fs_info->fs_devices->device_list_mutex);

	/* replace the sysfs entry */
	apfs_sysfs_remove_device(src_device);
	apfs_sysfs_update_devid(tgt_device);
	if (test_bit(APFS_DEV_STATE_WRITEABLE, &src_device->dev_state))
		apfs_scratch_superblocks(fs_info, src_device->bdev,
					  src_device->name->str);

	/* write back the superblocks */
	trans = apfs_start_transaction(root, 0);
	if (!IS_ERR(trans))
		apfs_commit_transaction(trans);

	mutex_unlock(&dev_replace->lock_finishing_cancel_unmount);

	apfs_rm_dev_replace_free_srcdev(src_device);

	return 0;
}

/*
 * Read progress of device replace status according to the state and last
 * stored position. The value format is the same as for
 * apfs_dev_replace::progress_1000
 */
static u64 apfs_dev_replace_progress(struct apfs_fs_info *fs_info)
{
	struct apfs_dev_replace *dev_replace = &fs_info->dev_replace;
	u64 ret = 0;

	switch (dev_replace->replace_state) {
	case APFS_IOCTL_DEV_REPLACE_STATE_NEVER_STARTED:
	case APFS_IOCTL_DEV_REPLACE_STATE_CANCELED:
		ret = 0;
		break;
	case APFS_IOCTL_DEV_REPLACE_STATE_FINISHED:
		ret = 1000;
		break;
	case APFS_IOCTL_DEV_REPLACE_STATE_STARTED:
	case APFS_IOCTL_DEV_REPLACE_STATE_SUSPENDED:
		ret = div64_u64(dev_replace->cursor_left,
				div_u64(apfs_device_get_total_bytes(
						dev_replace->srcdev), 1000));
		break;
	}

	return ret;
}

void apfs_dev_replace_status(struct apfs_fs_info *fs_info,
			      struct apfs_ioctl_dev_replace_args *args)
{
	struct apfs_dev_replace *dev_replace = &fs_info->dev_replace;

	down_read(&dev_replace->rwsem);
	/* even if !dev_replace_is_valid, the values are good enough for
	 * the replace_status ioctl */
	args->result = APFS_IOCTL_DEV_REPLACE_RESULT_NO_ERROR;
	args->status.replace_state = dev_replace->replace_state;
	args->status.time_started = dev_replace->time_started;
	args->status.time_stopped = dev_replace->time_stopped;
	args->status.num_write_errors =
		atomic64_read(&dev_replace->num_write_errors);
	args->status.num_uncorrectable_read_errors =
		atomic64_read(&dev_replace->num_uncorrectable_read_errors);
	args->status.progress_1000 = apfs_dev_replace_progress(fs_info);
	up_read(&dev_replace->rwsem);
}

int apfs_dev_replace_cancel(struct apfs_fs_info *fs_info)
{
	struct apfs_dev_replace *dev_replace = &fs_info->dev_replace;
	struct apfs_device *tgt_device = NULL;
	struct apfs_device *src_device = NULL;
	struct apfs_trans_handle *trans;
	struct apfs_root *root = fs_info->tree_root;
	int result;
	int ret;

	if (sb_rdonly(fs_info->sb))
		return -EROFS;

	mutex_lock(&dev_replace->lock_finishing_cancel_unmount);
	down_write(&dev_replace->rwsem);
	switch (dev_replace->replace_state) {
	case APFS_IOCTL_DEV_REPLACE_STATE_NEVER_STARTED:
	case APFS_IOCTL_DEV_REPLACE_STATE_FINISHED:
	case APFS_IOCTL_DEV_REPLACE_STATE_CANCELED:
		result = APFS_IOCTL_DEV_REPLACE_RESULT_NOT_STARTED;
		up_write(&dev_replace->rwsem);
		break;
	case APFS_IOCTL_DEV_REPLACE_STATE_STARTED:
		tgt_device = dev_replace->tgtdev;
		src_device = dev_replace->srcdev;
		up_write(&dev_replace->rwsem);
		ret = apfs_scrub_cancel(fs_info);
		if (ret < 0) {
			result = APFS_IOCTL_DEV_REPLACE_RESULT_NOT_STARTED;
		} else {
			result = APFS_IOCTL_DEV_REPLACE_RESULT_NO_ERROR;
			/*
			 * apfs_dev_replace_finishing() will handle the
			 * cleanup part
			 */
			apfs_info_in_rcu(fs_info,
				"dev_replace from %s (devid %llu) to %s canceled",
				apfs_dev_name(src_device), src_device->devid,
				apfs_dev_name(tgt_device));
		}
		break;
	case APFS_IOCTL_DEV_REPLACE_STATE_SUSPENDED:
		/*
		 * Scrub doing the replace isn't running so we need to do the
		 * cleanup step of apfs_dev_replace_finishing() here
		 */
		result = APFS_IOCTL_DEV_REPLACE_RESULT_NO_ERROR;
		tgt_device = dev_replace->tgtdev;
		src_device = dev_replace->srcdev;
		dev_replace->tgtdev = NULL;
		dev_replace->srcdev = NULL;
		dev_replace->replace_state =
				APFS_IOCTL_DEV_REPLACE_STATE_CANCELED;
		dev_replace->time_stopped = ktime_get_real_seconds();
		dev_replace->item_needs_writeback = 1;

		up_write(&dev_replace->rwsem);

		/* Scrub for replace must not be running in suspended state */
		ret = apfs_scrub_cancel(fs_info);
		ASSERT(ret != -ENOTCONN);

		trans = apfs_start_transaction(root, 0);
		if (IS_ERR(trans)) {
			mutex_unlock(&dev_replace->lock_finishing_cancel_unmount);
			return PTR_ERR(trans);
		}
		ret = apfs_commit_transaction(trans);
		WARN_ON(ret);

		apfs_info_in_rcu(fs_info,
		"suspended dev_replace from %s (devid %llu) to %s canceled",
			apfs_dev_name(src_device), src_device->devid,
			apfs_dev_name(tgt_device));

		if (tgt_device)
			apfs_destroy_dev_replace_tgtdev(tgt_device);
		break;
	default:
		up_write(&dev_replace->rwsem);
		result = -EINVAL;
	}

	mutex_unlock(&dev_replace->lock_finishing_cancel_unmount);
	return result;
}

void apfs_dev_replace_suspend_for_unmount(struct apfs_fs_info *fs_info)
{
	struct apfs_dev_replace *dev_replace = &fs_info->dev_replace;

	mutex_lock(&dev_replace->lock_finishing_cancel_unmount);
	down_write(&dev_replace->rwsem);

	switch (dev_replace->replace_state) {
	case APFS_IOCTL_DEV_REPLACE_STATE_NEVER_STARTED:
	case APFS_IOCTL_DEV_REPLACE_STATE_FINISHED:
	case APFS_IOCTL_DEV_REPLACE_STATE_CANCELED:
	case APFS_IOCTL_DEV_REPLACE_STATE_SUSPENDED:
		break;
	case APFS_IOCTL_DEV_REPLACE_STATE_STARTED:
		dev_replace->replace_state =
			APFS_IOCTL_DEV_REPLACE_STATE_SUSPENDED;
		dev_replace->time_stopped = ktime_get_real_seconds();
		dev_replace->item_needs_writeback = 1;
		apfs_info(fs_info, "suspending dev_replace for unmount");
		break;
	}

	up_write(&dev_replace->rwsem);
	mutex_unlock(&dev_replace->lock_finishing_cancel_unmount);
}

/* resume dev_replace procedure that was interrupted by unmount */
int apfs_resume_dev_replace_async(struct apfs_fs_info *fs_info)
{
	struct task_struct *task;
	struct apfs_dev_replace *dev_replace = &fs_info->dev_replace;

	down_write(&dev_replace->rwsem);

	switch (dev_replace->replace_state) {
	case APFS_IOCTL_DEV_REPLACE_STATE_NEVER_STARTED:
	case APFS_IOCTL_DEV_REPLACE_STATE_FINISHED:
	case APFS_IOCTL_DEV_REPLACE_STATE_CANCELED:
		up_write(&dev_replace->rwsem);
		return 0;
	case APFS_IOCTL_DEV_REPLACE_STATE_STARTED:
		break;
	case APFS_IOCTL_DEV_REPLACE_STATE_SUSPENDED:
		dev_replace->replace_state =
			APFS_IOCTL_DEV_REPLACE_STATE_STARTED;
		break;
	}
	if (!dev_replace->tgtdev || !dev_replace->tgtdev->bdev) {
		apfs_info(fs_info,
			   "cannot continue dev_replace, tgtdev is missing");
		apfs_info(fs_info,
			   "you may cancel the operation after 'mount -o degraded'");
		dev_replace->replace_state =
					APFS_IOCTL_DEV_REPLACE_STATE_SUSPENDED;
		up_write(&dev_replace->rwsem);
		return 0;
	}
	up_write(&dev_replace->rwsem);

	/*
	 * This could collide with a paused balance, but the exclusive op logic
	 * should never allow both to start and pause. We don't want to allow
	 * dev-replace to start anyway.
	 */
	if (!apfs_exclop_start(fs_info, APFS_EXCLOP_DEV_REPLACE)) {
		down_write(&dev_replace->rwsem);
		dev_replace->replace_state =
					APFS_IOCTL_DEV_REPLACE_STATE_SUSPENDED;
		up_write(&dev_replace->rwsem);
		apfs_info(fs_info,
		"cannot resume dev-replace, other exclusive operation running");
		return 0;
	}

	task = kthread_run(apfs_dev_replace_kthread, fs_info, "apfs-devrepl");
	return PTR_ERR_OR_ZERO(task);
}

static int apfs_dev_replace_kthread(void *data)
{
	struct apfs_fs_info *fs_info = data;
	struct apfs_dev_replace *dev_replace = &fs_info->dev_replace;
	u64 progress;
	int ret;

	progress = apfs_dev_replace_progress(fs_info);
	progress = div_u64(progress, 10);
	apfs_info_in_rcu(fs_info,
		"continuing dev_replace from %s (devid %llu) to target %s @%u%%",
		apfs_dev_name(dev_replace->srcdev),
		dev_replace->srcdev->devid,
		apfs_dev_name(dev_replace->tgtdev),
		(unsigned int)progress);

	ret = apfs_scrub_dev(fs_info, dev_replace->srcdev->devid,
			      dev_replace->committed_cursor_left,
			      apfs_device_get_total_bytes(dev_replace->srcdev),
			      &dev_replace->scrub_progress, 0, 1);
	ret = apfs_dev_replace_finishing(fs_info, ret);
	WARN_ON(ret && ret != -ECANCELED);

	apfs_exclop_finish(fs_info);
	return 0;
}

int __pure apfs_dev_replace_is_ongoing(struct apfs_dev_replace *dev_replace)
{
	if (!dev_replace->is_valid)
		return 0;

	switch (dev_replace->replace_state) {
	case APFS_IOCTL_DEV_REPLACE_STATE_NEVER_STARTED:
	case APFS_IOCTL_DEV_REPLACE_STATE_FINISHED:
	case APFS_IOCTL_DEV_REPLACE_STATE_CANCELED:
		return 0;
	case APFS_IOCTL_DEV_REPLACE_STATE_STARTED:
	case APFS_IOCTL_DEV_REPLACE_STATE_SUSPENDED:
		/*
		 * return true even if tgtdev is missing (this is
		 * something that can happen if the dev_replace
		 * procedure is suspended by an umount and then
		 * the tgtdev is missing (or "apfs dev scan") was
		 * not called and the filesystem is remounted
		 * in degraded state. This does not stop the
		 * dev_replace procedure. It needs to be canceled
		 * manually if the cancellation is wanted.
		 */
		break;
	}
	return 1;
}

void apfs_bio_counter_inc_noblocked(struct apfs_fs_info *fs_info)
{
	percpu_counter_inc(&fs_info->dev_replace.bio_counter);
}

void apfs_bio_counter_sub(struct apfs_fs_info *fs_info, s64 amount)
{
	percpu_counter_sub(&fs_info->dev_replace.bio_counter, amount);
	cond_wake_up_nomb(&fs_info->dev_replace.replace_wait);
}

void apfs_bio_counter_inc_blocked(struct apfs_fs_info *fs_info)
{
	while (1) {
		percpu_counter_inc(&fs_info->dev_replace.bio_counter);
		if (likely(!test_bit(APFS_FS_STATE_DEV_REPLACING,
				     &fs_info->fs_state)))
			break;

		apfs_bio_counter_dec(fs_info);
		wait_event(fs_info->dev_replace.replace_wait,
			   !test_bit(APFS_FS_STATE_DEV_REPLACING,
				     &fs_info->fs_state));
	}
}
