/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2012 Fusion-io  All rights reserved.
 * Copyright (C) 2012 Intel Corp. All rights reserved.
 */

#ifndef APFS_RAID56_H
#define APFS_RAID56_H

static inline int nr_parity_stripes(const struct map_lookup *map)
{
	if (map->type & APFS_BLOCK_GROUP_RAID5)
		return 1;
	else if (map->type & APFS_BLOCK_GROUP_RAID6)
		return 2;
	else
		return 0;
}

static inline int nr_data_stripes(const struct map_lookup *map)
{
	return map->num_stripes - nr_parity_stripes(map);
}
#define RAID5_P_STRIPE ((u64)-2)
#define RAID6_Q_STRIPE ((u64)-1)

#define is_parity_stripe(x) (((x) == RAID5_P_STRIPE) ||		\
			     ((x) == RAID6_Q_STRIPE))

struct apfs_raid_bio;
struct apfs_device;

int raid56_parity_recover(struct apfs_fs_info *fs_info, struct bio *bio,
			  struct apfs_bio *bbio, u64 stripe_len,
			  int mirror_num, int generic_io);
int raid56_parity_write(struct apfs_fs_info *fs_info, struct bio *bio,
			       struct apfs_bio *bbio, u64 stripe_len);

void raid56_add_scrub_pages(struct apfs_raid_bio *rbio, struct page *page,
			    u64 logical);

struct apfs_raid_bio *
raid56_parity_alloc_scrub_rbio(struct apfs_fs_info *fs_info, struct bio *bio,
			       struct apfs_bio *bbio, u64 stripe_len,
			       struct apfs_device *scrub_dev,
			       unsigned long *dbitmap, int stripe_nsectors);
void raid56_parity_submit_scrub_rbio(struct apfs_raid_bio *rbio);

struct apfs_raid_bio *
raid56_alloc_missing_rbio(struct apfs_fs_info *fs_info, struct bio *bio,
			  struct apfs_bio *bbio, u64 length);
void raid56_submit_missing_rbio(struct apfs_raid_bio *rbio);

int apfs_alloc_stripe_hash_table(struct apfs_fs_info *info);
void apfs_free_stripe_hash_table(struct apfs_fs_info *info);

#endif
