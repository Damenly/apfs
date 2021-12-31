/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2009 Oracle.  All rights reserved.
 */

#ifndef APFS_FREE_SPACE_CACHE_H
#define APFS_FREE_SPACE_CACHE_H

/*
 * This is the trim state of an extent or bitmap.
 *
 * APFS_TRIM_STATE_TRIMMING is special and used to maintain the state of a
 * bitmap as we may need several trims to fully trim a single bitmap entry.
 * This is reset should any free space other than trimmed space be added to the
 * bitmap.
 */
enum apfs_trim_state {
	APFS_TRIM_STATE_UNTRIMMED,
	APFS_TRIM_STATE_TRIMMED,
	APFS_TRIM_STATE_TRIMMING,
};

struct apfs_free_space {
	struct rb_node offset_index;
	u64 offset;
	u64 bytes;
	u64 max_extent_size;
	unsigned long *bitmap;
	struct list_head list;
	enum apfs_trim_state trim_state;
	s32 bitmap_extents;
};

static inline bool apfs_free_space_trimmed(struct apfs_free_space *info)
{
	return (info->trim_state == APFS_TRIM_STATE_TRIMMED);
}

static inline bool apfs_free_space_trimming_bitmap(
					    struct apfs_free_space *info)
{
	return (info->trim_state == APFS_TRIM_STATE_TRIMMING);
}

struct apfs_free_space_ctl {
	spinlock_t tree_lock;
	struct rb_root free_space_offset;
	u64 free_space;
	int extents_thresh;
	int free_extents;
	int total_bitmaps;
	int unit;
	u64 start;
	s32 discardable_extents[APFS_STAT_NR_ENTRIES];
	s64 discardable_bytes[APFS_STAT_NR_ENTRIES];
	const struct apfs_free_space_op *op;
	void *private;
	struct mutex cache_writeout_mutex;
	struct list_head trimming_ranges;
};

struct apfs_free_space_op {
	bool (*use_bitmap)(struct apfs_free_space_ctl *ctl,
			   struct apfs_free_space *info);
};

struct apfs_io_ctl {
	void *cur, *orig;
	struct page *page;
	struct page **pages;
	struct apfs_fs_info *fs_info;
	struct inode *inode;
	unsigned long size;
	int index;
	int num_pages;
	int entries;
	int bitmaps;
};

struct inode *lookup_free_space_inode(struct apfs_block_group *block_group,
		struct apfs_path *path);
int create_free_space_inode(struct apfs_trans_handle *trans,
			    struct apfs_block_group *block_group,
			    struct apfs_path *path);
int apfs_remove_free_space_inode(struct apfs_trans_handle *trans,
				  struct inode *inode,
				  struct apfs_block_group *block_group);

int apfs_check_trunc_cache_free_space(struct apfs_fs_info *fs_info,
				       struct apfs_block_rsv *rsv);
int apfs_truncate_free_space_cache(struct apfs_trans_handle *trans,
				    struct apfs_block_group *block_group,
				    struct inode *inode);
int load_free_space_cache(struct apfs_block_group *block_group);
int apfs_wait_cache_io(struct apfs_trans_handle *trans,
			struct apfs_block_group *block_group,
			struct apfs_path *path);
int apfs_write_out_cache(struct apfs_trans_handle *trans,
			  struct apfs_block_group *block_group,
			  struct apfs_path *path);

void apfs_init_free_space_ctl(struct apfs_block_group *block_group,
			       struct apfs_free_space_ctl *ctl);
int __apfs_add_free_space(struct apfs_fs_info *fs_info,
			   struct apfs_free_space_ctl *ctl,
			   u64 bytenr, u64 size,
			   enum apfs_trim_state trim_state);
int apfs_add_free_space(struct apfs_block_group *block_group,
			 u64 bytenr, u64 size);
int apfs_add_free_space_unused(struct apfs_block_group *block_group,
				u64 bytenr, u64 size);
int apfs_add_free_space_async_trimmed(struct apfs_block_group *block_group,
				       u64 bytenr, u64 size);
int apfs_remove_free_space(struct apfs_block_group *block_group,
			    u64 bytenr, u64 size);
void __apfs_remove_free_space_cache(struct apfs_free_space_ctl *ctl);
void apfs_remove_free_space_cache(struct apfs_block_group *block_group);
bool apfs_is_free_space_trimmed(struct apfs_block_group *block_group);
u64 apfs_find_space_for_alloc(struct apfs_block_group *block_group,
			       u64 offset, u64 bytes, u64 empty_size,
			       u64 *max_extent_size);
void apfs_dump_free_space(struct apfs_block_group *block_group,
			   u64 bytes);
int apfs_find_space_cluster(struct apfs_block_group *block_group,
			     struct apfs_free_cluster *cluster,
			     u64 offset, u64 bytes, u64 empty_size);
void apfs_init_free_cluster(struct apfs_free_cluster *cluster);
u64 apfs_alloc_from_cluster(struct apfs_block_group *block_group,
			     struct apfs_free_cluster *cluster, u64 bytes,
			     u64 min_start, u64 *max_extent_size);
void apfs_return_cluster_to_free_space(
			       struct apfs_block_group *block_group,
			       struct apfs_free_cluster *cluster);
int apfs_trim_block_group(struct apfs_block_group *block_group,
			   u64 *trimmed, u64 start, u64 end, u64 minlen);
int apfs_trim_block_group_extents(struct apfs_block_group *block_group,
				   u64 *trimmed, u64 start, u64 end, u64 minlen,
				   bool async);
int apfs_trim_block_group_bitmaps(struct apfs_block_group *block_group,
				   u64 *trimmed, u64 start, u64 end, u64 minlen,
				   u64 maxlen, bool async);

bool apfs_free_space_cache_v1_active(struct apfs_fs_info *fs_info);
int apfs_set_free_space_cache_v1_active(struct apfs_fs_info *fs_info, bool active);
/* Support functions for running our sanity tests */
#ifdef CONFIG_APFS_FS_RUN_SANITY_TESTS
int test_add_free_space_entry(struct apfs_block_group *cache,
			      u64 offset, u64 bytes, bool bitmap);
int test_check_exists(struct apfs_block_group *cache, u64 offset, u64 bytes);
#endif

#endif
