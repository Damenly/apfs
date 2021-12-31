/* SPDX-License-Identifier: GPL-2.0 */

#ifndef APFS_ZONED_H
#define APFS_ZONED_H

#include <linux/types.h>
#include <linux/blkdev.h>
#include "volumes.h"
#include "disk-io.h"
#include "block-group.h"

struct apfs_ordered_extent;

/*
 * Block groups with more than this value (percents) of unusable space will be
 * scheduled for background reclaim.
 */
#define APFS_DEFAULT_RECLAIM_THRESH		75

struct apfs_zoned_device_info {
	/*
	 * Number of zones, zone size and types of zones if bdev is a
	 * zoned block device.
	 */
	u64 zone_size;
	u8  zone_size_shift;
	u64 max_zone_append_size;
	u32 nr_zones;
	unsigned long *seq_zones;
	unsigned long *empty_zones;
	struct blk_zone sb_zones[2 * APFS_SUPER_MIRROR_MAX];
};

#ifdef CONFIG_BLK_DEV_ZONED
int apfs_get_dev_zone(struct apfs_device *device, u64 pos,
		       struct blk_zone *zone);
int apfs_get_dev_zone_info_all_devices(struct apfs_fs_info *fs_info);
int apfs_get_dev_zone_info(struct apfs_device *device);
void apfs_destroy_dev_zone_info(struct apfs_device *device);
int apfs_check_zoned_mode(struct apfs_fs_info *fs_info);
int apfs_check_mountopts_zoned(struct apfs_fs_info *info);
int apfs_sb_log_location_bdev(struct block_device *bdev, int mirror, int rw,
			       u64 *bytenr_ret);
int apfs_sb_log_location(struct apfs_device *device, int mirror, int rw,
			  u64 *bytenr_ret);
void apfs_advance_sb_log(struct apfs_device *device, int mirror);
int apfs_reset_sb_log_zones(struct block_device *bdev, int mirror);
u64 apfs_find_allocatable_zones(struct apfs_device *device, u64 hole_start,
				 u64 hole_end, u64 num_bytes);
int apfs_reset_device_zone(struct apfs_device *device, u64 physical,
			    u64 length, u64 *bytes);
int apfs_ensure_empty_zones(struct apfs_device *device, u64 start, u64 size);
int apfs_load_block_group_zone_info(struct apfs_block_group *cache, bool new);
void apfs_calc_zone_unusable(struct apfs_block_group *cache);
void apfs_redirty_list_add(struct apfs_transaction *trans,
			    struct extent_buffer *eb);
void apfs_free_redirty_list(struct apfs_transaction *trans);
bool apfs_use_zone_append(struct apfs_inode *inode, u64 start);
void apfs_record_physical_zoned(struct inode *inode, u64 file_offset,
				 struct bio *bio);
void apfs_rewrite_logical_zoned(struct apfs_ordered_extent *ordered);
bool apfs_check_meta_write_pointer(struct apfs_fs_info *fs_info,
				    struct extent_buffer *eb,
				    struct apfs_block_group **cache_ret);
void apfs_revert_meta_write_pointer(struct apfs_block_group *cache,
				     struct extent_buffer *eb);
int apfs_zoned_issue_zeroout(struct apfs_device *device, u64 physical, u64 length);
int apfs_sync_zone_write_pointer(struct apfs_device *tgt_dev, u64 logical,
				  u64 physical_start, u64 physical_pos);
struct apfs_device *apfs_zoned_get_device(struct apfs_fs_info *fs_info,
					    u64 logical, u64 length);
#else /* CONFIG_BLK_DEV_ZONED */
static inline int apfs_get_dev_zone(struct apfs_device *device, u64 pos,
				     struct blk_zone *zone)
{
	return 0;
}

static inline int apfs_get_dev_zone_info_all_devices(struct apfs_fs_info *fs_info)
{
	return 0;
}

static inline int apfs_get_dev_zone_info(struct apfs_device *device)
{
	return 0;
}

static inline void apfs_destroy_dev_zone_info(struct apfs_device *device) { }

static inline int apfs_check_zoned_mode(const struct apfs_fs_info *fs_info)
{
	if (!apfs_is_zoned(fs_info))
		return 0;

	apfs_err(fs_info, "zoned block devices support is not enabled");
	return -EOPNOTSUPP;
}

static inline int apfs_check_mountopts_zoned(struct apfs_fs_info *info)
{
	return 0;
}

static inline int apfs_sb_log_location_bdev(struct block_device *bdev,
					     int mirror, int rw, u64 *bytenr_ret)
{
	*bytenr_ret = apfs_sb_offset(mirror);
	return 0;
}

static inline int apfs_sb_log_location(struct apfs_device *device, int mirror,
					int rw, u64 *bytenr_ret)
{
	*bytenr_ret = apfs_sb_offset(mirror);
	return 0;
}

static inline void apfs_advance_sb_log(struct apfs_device *device, int mirror)
{ }

static inline int apfs_reset_sb_log_zones(struct block_device *bdev, int mirror)
{
	return 0;
}

static inline u64 apfs_find_allocatable_zones(struct apfs_device *device,
					       u64 hole_start, u64 hole_end,
					       u64 num_bytes)
{
	return hole_start;
}

static inline int apfs_reset_device_zone(struct apfs_device *device,
					  u64 physical, u64 length, u64 *bytes)
{
	*bytes = 0;
	return 0;
}

static inline int apfs_ensure_empty_zones(struct apfs_device *device,
					   u64 start, u64 size)
{
	return 0;
}

static inline int apfs_load_block_group_zone_info(
		struct apfs_block_group *cache, bool new)
{
	return 0;
}

static inline void apfs_calc_zone_unusable(struct apfs_block_group *cache) { }

static inline void apfs_redirty_list_add(struct apfs_transaction *trans,
					  struct extent_buffer *eb) { }
static inline void apfs_free_redirty_list(struct apfs_transaction *trans) { }

static inline bool apfs_use_zone_append(struct apfs_inode *inode, u64 start)
{
	return false;
}

static inline void apfs_record_physical_zoned(struct inode *inode,
					       u64 file_offset, struct bio *bio)
{
}

static inline void apfs_rewrite_logical_zoned(
				struct apfs_ordered_extent *ordered) { }

static inline bool apfs_check_meta_write_pointer(struct apfs_fs_info *fs_info,
			       struct extent_buffer *eb,
			       struct apfs_block_group **cache_ret)
{
	return true;
}

static inline void apfs_revert_meta_write_pointer(
						struct apfs_block_group *cache,
						struct extent_buffer *eb)
{
}

static inline int apfs_zoned_issue_zeroout(struct apfs_device *device,
					    u64 physical, u64 length)
{
	return -EOPNOTSUPP;
}

static inline int apfs_sync_zone_write_pointer(struct apfs_device *tgt_dev,
						u64 logical, u64 physical_start,
						u64 physical_pos)
{
	return -EOPNOTSUPP;
}

static inline struct apfs_device *apfs_zoned_get_device(
						  struct apfs_fs_info *fs_info,
						  u64 logical, u64 length)
{
	return ERR_PTR(-EOPNOTSUPP);
}

#endif

static inline bool apfs_dev_is_sequential(struct apfs_device *device, u64 pos)
{
	struct apfs_zoned_device_info *zone_info = device->zone_info;

	if (!zone_info)
		return false;

	return test_bit(pos >> zone_info->zone_size_shift, zone_info->seq_zones);
}

static inline bool apfs_dev_is_empty_zone(struct apfs_device *device, u64 pos)
{
	struct apfs_zoned_device_info *zone_info = device->zone_info;

	if (!zone_info)
		return true;

	return test_bit(pos >> zone_info->zone_size_shift, zone_info->empty_zones);
}

static inline void apfs_dev_set_empty_zone_bit(struct apfs_device *device,
						u64 pos, bool set)
{
	struct apfs_zoned_device_info *zone_info = device->zone_info;
	unsigned int zno;

	if (!zone_info)
		return;

	zno = pos >> zone_info->zone_size_shift;
	if (set)
		set_bit(zno, zone_info->empty_zones);
	else
		clear_bit(zno, zone_info->empty_zones);
}

static inline void apfs_dev_set_zone_empty(struct apfs_device *device, u64 pos)
{
	apfs_dev_set_empty_zone_bit(device, pos, true);
}

static inline void apfs_dev_clear_zone_empty(struct apfs_device *device, u64 pos)
{
	apfs_dev_set_empty_zone_bit(device, pos, false);
}

static inline bool apfs_check_device_zone_type(const struct apfs_fs_info *fs_info,
						struct block_device *bdev)
{
	if (apfs_is_zoned(fs_info)) {
		/*
		 * We can allow a regular device on a zoned filesystem, because
		 * we will emulate the zoned capabilities.
		 */
		if (!bdev_is_zoned(bdev))
			return true;

		return fs_info->zone_size ==
			(bdev_zone_sectors(bdev) << SECTOR_SHIFT);
	}

	/* Do not allow Host Manged zoned device */
	return bdev_zoned_model(bdev) != BLK_ZONED_HM;
}

static inline bool apfs_check_super_location(struct apfs_device *device, u64 pos)
{
	/*
	 * On a non-zoned device, any address is OK. On a zoned device,
	 * non-SEQUENTIAL WRITE REQUIRED zones are capable.
	 */
	return device->zone_info == NULL || !apfs_dev_is_sequential(device, pos);
}

static inline bool apfs_can_zone_reset(struct apfs_device *device,
					u64 physical, u64 length)
{
	u64 zone_size;

	if (!apfs_dev_is_sequential(device, physical))
		return false;

	zone_size = device->zone_info->zone_size;
	if (!IS_ALIGNED(physical, zone_size) || !IS_ALIGNED(length, zone_size))
		return false;

	return true;
}

static inline void apfs_zoned_meta_io_lock(struct apfs_fs_info *fs_info)
{
	if (!apfs_is_zoned(fs_info))
		return;
	mutex_lock(&fs_info->zoned_meta_io_lock);
}

static inline void apfs_zoned_meta_io_unlock(struct apfs_fs_info *fs_info)
{
	if (!apfs_is_zoned(fs_info))
		return;
	mutex_unlock(&fs_info->zoned_meta_io_lock);
}

static inline void apfs_clear_treelog_bg(struct apfs_block_group *bg)
{
	struct apfs_fs_info *fs_info = bg->fs_info;

	if (!apfs_is_zoned(fs_info))
		return;

	spin_lock(&fs_info->treelog_bg_lock);
	if (fs_info->treelog_bg == bg->start)
		fs_info->treelog_bg = 0;
	spin_unlock(&fs_info->treelog_bg_lock);
}

#endif
