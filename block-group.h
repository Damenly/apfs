/* SPDX-License-Identifier: GPL-2.0 */

#ifndef APFS_BLOCK_GROUP_H
#define APFS_BLOCK_GROUP_H

#include "free-space-cache.h"

enum apfs_disk_cache_state {
	APFS_DC_WRITTEN,
	APFS_DC_ERROR,
	APFS_DC_CLEAR,
	APFS_DC_SETUP,
};

/*
 * This describes the state of the block_group for async discard.  This is due
 * to the two pass nature of it where extent discarding is prioritized over
 * bitmap discarding.  APFS_DISCARD_RESET_CURSOR is set when we are resetting
 * between lists to prevent contention for discard state variables
 * (eg. discard_cursor).
 */
enum apfs_discard_state {
	APFS_DISCARD_EXTENTS,
	APFS_DISCARD_BITMAPS,
	APFS_DISCARD_RESET_CURSOR,
};

/*
 * Control flags for do_chunk_alloc's force field CHUNK_ALLOC_NO_FORCE means to
 * only allocate a chunk if we really need one.
 *
 * CHUNK_ALLOC_LIMITED means to only try and allocate one if we have very few
 * chunks already allocated.  This is used as part of the clustering code to
 * help make sure we have a good pool of storage to cluster in, without filling
 * the FS with empty chunks
 *
 * CHUNK_ALLOC_FORCE means it must try to allocate one
 */
enum apfs_chunk_alloc_enum {
	CHUNK_ALLOC_NO_FORCE,
	CHUNK_ALLOC_LIMITED,
	CHUNK_ALLOC_FORCE,
};

struct apfs_caching_control {
	struct list_head list;
	struct mutex mutex;
	wait_queue_head_t wait;
	struct apfs_work work;
	struct apfs_block_group *block_group;
	u64 progress;
	refcount_t count;
};

/* Once caching_thread() finds this much free space, it will wake up waiters. */
#define CACHING_CTL_WAKE_UP SZ_2M

struct apfs_block_group {
	struct apfs_fs_info *fs_info;
	struct inode *inode;
	spinlock_t lock;
	u64 start;
	u64 length;
	u64 pinned;
	u64 reserved;
	u64 used;
	u64 delalloc_bytes;
	u64 bytes_super;
	u64 flags;
	u64 cache_generation;

	/*
	 * If the free space extent count exceeds this number, convert the block
	 * group to bitmaps.
	 */
	u32 bitmap_high_thresh;

	/*
	 * If the free space extent count drops below this number, convert the
	 * block group back to extents.
	 */
	u32 bitmap_low_thresh;

	/*
	 * It is just used for the delayed data space allocation because
	 * only the data space allocation and the relative metadata update
	 * can be done cross the transaction.
	 */
	struct rw_semaphore data_rwsem;

	/* For raid56, this is a full stripe, without parity */
	unsigned long full_stripe_len;

	unsigned int ro;
	unsigned int iref:1;
	unsigned int has_caching_ctl:1;
	unsigned int removed:1;
	unsigned int to_copy:1;
	unsigned int relocating_repair:1;
	unsigned int chunk_item_inserted:1;

	int disk_cache_state;

	/* Cache tracking stuff */
	int cached;
	struct apfs_caching_control *caching_ctl;
	u64 last_byte_to_unpin;

	struct apfs_space_info *space_info;

	/* Free space cache stuff */
	struct apfs_free_space_ctl *free_space_ctl;

	/* Block group cache stuff */
	struct rb_node cache_node;

	/* For block groups in the same raid type */
	struct list_head list;

	refcount_t refs;

	/*
	 * List of struct apfs_free_clusters for this block group.
	 * Today it will only have one thing on it, but that may change
	 */
	struct list_head cluster_list;

	/* For delayed block group creation or deletion of empty block groups */
	struct list_head bg_list;

	/* For read-only block groups */
	struct list_head ro_list;

	/*
	 * When non-zero it means the block group's logical address and its
	 * device extents can not be reused for future block group allocations
	 * until the counter goes down to 0. This is to prevent them from being
	 * reused while some task is still using the block group after it was
	 * deleted - we want to make sure they can only be reused for new block
	 * groups after that task is done with the deleted block group.
	 */
	atomic_t frozen;

	/* For discard operations */
	struct list_head discard_list;
	int discard_index;
	u64 discard_eligible_time;
	u64 discard_cursor;
	enum apfs_discard_state discard_state;

	/* For dirty block groups */
	struct list_head dirty_list;
	struct list_head io_list;

	struct apfs_io_ctl io_ctl;

	/*
	 * Incremented when doing extent allocations and holding a read lock
	 * on the space_info's groups_sem semaphore.
	 * Decremented when an ordered extent that represents an IO against this
	 * block group's range is created (after it's added to its inode's
	 * root's list of ordered extents) or immediately after the allocation
	 * if it's a metadata extent or fallocate extent (for these cases we
	 * don't create ordered extents).
	 */
	atomic_t reservations;

	/*
	 * Incremented while holding the spinlock *lock* by a task checking if
	 * it can perform a nocow write (incremented if the value for the *ro*
	 * field is 0). Decremented by such tasks once they create an ordered
	 * extent or before that if some error happens before reaching that step.
	 * This is to prevent races between block group relocation and nocow
	 * writes through direct IO.
	 */
	atomic_t nocow_writers;

	/* Lock for free space tree operations. */
	struct mutex free_space_lock;

	/*
	 * Does the block group need to be added to the free space tree?
	 * Protected by free_space_lock.
	 */
	int needs_free_space;

	/* Flag indicating this block group is placed on a sequential zone */
	bool seq_zone;

	/*
	 * Number of extents in this block group used for swap files.
	 * All accesses protected by the spinlock 'lock'.
	 */
	int swap_extents;

	/* Record locked full stripes for RAID5/6 block group */
	struct apfs_full_stripe_locks_tree full_stripe_locks_root;

	/*
	 * Allocation offset for the block group to implement sequential
	 * allocation. This is used only on a zoned filesystem.
	 */
	u64 alloc_offset;
	u64 zone_unusable;
	u64 meta_write_pointer;
};

static inline u64 apfs_block_group_end(struct apfs_block_group *block_group)
{
	return (block_group->start + block_group->length);
}

static inline bool apfs_is_block_group_data_only(
					struct apfs_block_group *block_group)
{
	/*
	 * In mixed mode the fragmentation is expected to be high, lowering the
	 * efficiency, so only proper data block groups are considered.
	 */
	return (block_group->flags & APFS_BLOCK_GROUP_DATA) &&
	       !(block_group->flags & APFS_BLOCK_GROUP_METADATA);
}

#ifdef CONFIG_APFS_DEBUG
static inline int apfs_should_fragment_free_space(
		struct apfs_block_group *block_group)
{
	struct apfs_fs_info *fs_info = block_group->fs_info;

	return (apfs_test_opt(fs_info, FRAGMENT_METADATA) &&
		block_group->flags & APFS_BLOCK_GROUP_METADATA) ||
	       (apfs_test_opt(fs_info, FRAGMENT_DATA) &&
		block_group->flags &  APFS_BLOCK_GROUP_DATA);
}
#endif

struct apfs_block_group *apfs_lookup_first_block_group(
		struct apfs_fs_info *info, u64 bytenr);
struct apfs_block_group *apfs_lookup_block_group(
		struct apfs_fs_info *info, u64 bytenr);
struct apfs_block_group *apfs_next_block_group(
		struct apfs_block_group *cache);
void apfs_get_block_group(struct apfs_block_group *cache);
void apfs_put_block_group(struct apfs_block_group *cache);
void apfs_dec_block_group_reservations(struct apfs_fs_info *fs_info,
					const u64 start);
void apfs_wait_block_group_reservations(struct apfs_block_group *bg);
bool apfs_inc_nocow_writers(struct apfs_fs_info *fs_info, u64 bytenr);
void apfs_dec_nocow_writers(struct apfs_fs_info *fs_info, u64 bytenr);
void apfs_wait_nocow_writers(struct apfs_block_group *bg);
void apfs_wait_block_group_cache_progress(struct apfs_block_group *cache,
				           u64 num_bytes);
int apfs_wait_block_group_cache_done(struct apfs_block_group *cache);
int apfs_cache_block_group(struct apfs_block_group *cache,
			    int load_cache_only);
void apfs_put_caching_control(struct apfs_caching_control *ctl);
struct apfs_caching_control *apfs_get_caching_control(
		struct apfs_block_group *cache);
u64 add_new_free_space(struct apfs_block_group *block_group,
		       u64 start, u64 end);
struct apfs_trans_handle *apfs_start_trans_remove_block_group(
				struct apfs_fs_info *fs_info,
				const u64 chunk_offset);
int apfs_remove_block_group(struct apfs_trans_handle *trans,
			     u64 group_start, struct extent_map *em);
void apfs_delete_unused_bgs(struct apfs_fs_info *fs_info);
void apfs_mark_bg_unused(struct apfs_block_group *bg);
void apfs_reclaim_bgs_work(struct work_struct *work);
void apfs_reclaim_bgs(struct apfs_fs_info *fs_info);
void apfs_mark_bg_to_reclaim(struct apfs_block_group *bg);
int apfs_read_block_groups(struct apfs_fs_info *info);
struct apfs_block_group *apfs_make_block_group(struct apfs_trans_handle *trans,
						 u64 bytes_used, u64 type,
						 u64 chunk_offset, u64 size);
void apfs_create_pending_block_groups(struct apfs_trans_handle *trans);
int apfs_inc_block_group_ro(struct apfs_block_group *cache,
			     bool do_chunk_alloc);
void apfs_dec_block_group_ro(struct apfs_block_group *cache);
int apfs_start_dirty_block_groups(struct apfs_trans_handle *trans);
int apfs_write_dirty_block_groups(struct apfs_trans_handle *trans);
int apfs_setup_space_cache(struct apfs_trans_handle *trans);
int apfs_update_block_group(struct apfs_trans_handle *trans,
			     u64 bytenr, u64 num_bytes, int alloc);
int apfs_add_reserved_bytes(struct apfs_block_group *cache,
			     u64 ram_bytes, u64 num_bytes, int delalloc);
void apfs_free_reserved_bytes(struct apfs_block_group *cache,
			       u64 num_bytes, int delalloc);
int apfs_chunk_alloc(struct apfs_trans_handle *trans, u64 flags,
		      enum apfs_chunk_alloc_enum force);
int apfs_force_chunk_alloc(struct apfs_trans_handle *trans, u64 type);
void check_system_chunk(struct apfs_trans_handle *trans, const u64 type);
u64 apfs_get_alloc_profile(struct apfs_fs_info *fs_info, u64 orig_flags);
void apfs_put_block_group_cache(struct apfs_fs_info *info);
int apfs_free_block_groups(struct apfs_fs_info *info);
void apfs_wait_space_cache_v1_finished(struct apfs_block_group *cache,
				struct apfs_caching_control *caching_ctl);
int apfs_rmap_block(struct apfs_fs_info *fs_info, u64 chunk_start,
		       struct block_device *bdev, u64 physical, u64 **logical,
		       int *naddrs, int *stripe_len);

static inline u64 apfs_data_alloc_profile(struct apfs_fs_info *fs_info)
{
	return apfs_get_alloc_profile(fs_info, APFS_BLOCK_GROUP_DATA);
}

static inline u64 apfs_metadata_alloc_profile(struct apfs_fs_info *fs_info)
{
	return apfs_get_alloc_profile(fs_info, APFS_BLOCK_GROUP_METADATA);
}

static inline u64 apfs_system_alloc_profile(struct apfs_fs_info *fs_info)
{
	return apfs_get_alloc_profile(fs_info, APFS_BLOCK_GROUP_SYSTEM);
}

static inline int apfs_block_group_done(struct apfs_block_group *cache)
{
	smp_mb();
	return cache->cached == APFS_CACHE_FINISHED ||
		cache->cached == APFS_CACHE_ERROR;
}

void apfs_freeze_block_group(struct apfs_block_group *cache);
void apfs_unfreeze_block_group(struct apfs_block_group *cache);

bool apfs_inc_block_group_swap_extents(struct apfs_block_group *bg);
void apfs_dec_block_group_swap_extents(struct apfs_block_group *bg, int amount);

#endif /* APFS_BLOCK_GROUP_H */
