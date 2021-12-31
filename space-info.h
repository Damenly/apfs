/* SPDX-License-Identifier: GPL-2.0 */

#ifndef APFS_SPACE_INFO_H
#define APFS_SPACE_INFO_H

struct apfs_space_info {
	spinlock_t lock;

	u64 total_bytes;	/* total bytes in the space,
				   this doesn't take mirrors into account */
	u64 bytes_used;		/* total bytes used,
				   this doesn't take mirrors into account */
	u64 bytes_pinned;	/* total bytes pinned, will be freed when the
				   transaction finishes */
	u64 bytes_reserved;	/* total bytes the allocator has reserved for
				   current allocations */
	u64 bytes_may_use;	/* number of bytes that may be used for
				   delalloc/allocations */
	u64 bytes_readonly;	/* total bytes that are read only */
	u64 bytes_zone_unusable;	/* total bytes that are unusable until
					   resetting the device zone */

	u64 max_extent_size;	/* This will hold the maximum extent size of
				   the space info if we had an ENOSPC in the
				   allocator. */

	int clamp;		/* Used to scale our threshold for preemptive
				   flushing. The value is >> clamp, so turns
				   out to be a 2^clamp divisor. */

	unsigned int full:1;	/* indicates that we cannot allocate any more
				   chunks for this space */
	unsigned int chunk_alloc:1;	/* set if we are allocating a chunk */

	unsigned int flush:1;		/* set if we are trying to make space */

	unsigned int force_alloc;	/* set if we need to force a chunk
					   alloc for this space */

	u64 disk_used;		/* total bytes used on disk */
	u64 disk_total;		/* total bytes on disk, takes mirrors into
				   account */

	u64 flags;

	struct list_head list;
	/* Protected by the spinlock 'lock'. */
	struct list_head ro_bgs;
	struct list_head priority_tickets;
	struct list_head tickets;

	/*
	 * Size of space that needs to be reclaimed in order to satisfy pending
	 * tickets
	 */
	u64 reclaim_size;

	/*
	 * tickets_id just indicates the next ticket will be handled, so note
	 * it's not stored per ticket.
	 */
	u64 tickets_id;

	struct rw_semaphore groups_sem;
	/* for block groups in our same type */
	struct list_head block_groups[APFS_NR_RAID_TYPES];

	struct kobject kobj;
	struct kobject *block_group_kobjs[APFS_NR_RAID_TYPES];
};

struct reserve_ticket {
	u64 bytes;
	int error;
	bool steal;
	struct list_head list;
	wait_queue_head_t wait;
};

static inline bool apfs_mixed_space_info(struct apfs_space_info *space_info)
{
	return ((space_info->flags & APFS_BLOCK_GROUP_METADATA) &&
		(space_info->flags & APFS_BLOCK_GROUP_DATA));
}

/*
 *
 * Declare a helper function to detect underflow of various space info members
 */
#define DECLARE_SPACE_INFO_UPDATE(name, trace_name)			\
static inline void							\
apfs_space_info_update_##name(struct apfs_fs_info *fs_info,		\
			       struct apfs_space_info *sinfo,		\
			       s64 bytes)				\
{									\
	const u64 abs_bytes = (bytes < 0) ? -bytes : bytes;		\
	lockdep_assert_held(&sinfo->lock);				\
	if (bytes < 0 && sinfo->name < -bytes) {			\
		WARN_ON(1);						\
		sinfo->name = 0;					\
		return;							\
	}								\
	sinfo->name += bytes;						\
}

DECLARE_SPACE_INFO_UPDATE(bytes_may_use, "space_info");
DECLARE_SPACE_INFO_UPDATE(bytes_pinned, "pinned");

int apfs_init_space_info(struct apfs_fs_info *fs_info);
void apfs_update_space_info(struct apfs_fs_info *info, u64 flags,
			     u64 total_bytes, u64 bytes_used,
			     u64 bytes_readonly, u64 bytes_zone_unusable,
			     struct apfs_space_info **space_info);
struct apfs_space_info *apfs_find_space_info(struct apfs_fs_info *info,
					       u64 flags);
u64 __pure apfs_space_info_used(struct apfs_space_info *s_info,
			  bool may_use_included);
void apfs_clear_space_info_full(struct apfs_fs_info *info);
void apfs_dump_space_info(struct apfs_fs_info *fs_info,
			   struct apfs_space_info *info, u64 bytes,
			   int dump_block_groups);
int apfs_reserve_metadata_bytes(struct apfs_root *root,
				 struct apfs_block_rsv *block_rsv,
				 u64 orig_bytes,
				 enum apfs_reserve_flush_enum flush);
void apfs_try_granting_tickets(struct apfs_fs_info *fs_info,
				struct apfs_space_info *space_info);
int apfs_can_overcommit(struct apfs_fs_info *fs_info,
			 struct apfs_space_info *space_info, u64 bytes,
			 enum apfs_reserve_flush_enum flush);

static inline void apfs_space_info_free_bytes_may_use(
				struct apfs_fs_info *fs_info,
				struct apfs_space_info *space_info,
				u64 num_bytes)
{
	spin_lock(&space_info->lock);
	apfs_space_info_update_bytes_may_use(fs_info, space_info, -num_bytes);
	apfs_try_granting_tickets(fs_info, space_info);
	spin_unlock(&space_info->lock);
}
int apfs_reserve_data_bytes(struct apfs_fs_info *fs_info, u64 bytes,
			     enum apfs_reserve_flush_enum flush);
#endif /* APFS_SPACE_INFO_H */
