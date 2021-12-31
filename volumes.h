/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2007 Oracle.  All rights reserved.
 */

#ifndef APFS_VOLUMES_H
#define APFS_VOLUMES_H

#include <linux/bio.h>
#include <linux/sort.h>
#include "apfs.h"
#include "async-thread.h"

#define APFS_MAX_DATA_CHUNK_SIZE	(10ULL * SZ_1G)

extern struct mutex uuid_mutex;

#define APFS_STRIPE_LEN	SZ_64K

struct apfs_io_geometry {
	/* remaining bytes before crossing a stripe */
	u64 len;
	/* offset of logical address in chunk */
	u64 offset;
	/* length of single IO stripe */
	u64 stripe_len;
	/* number of stripe where address falls */
	u64 stripe_nr;
	/* offset of address in stripe */
	u64 stripe_offset;
	/* offset of raid56 stripe into the chunk */
	u64 raid56_stripe_offset;
};

/*
 * Use sequence counter to get consistent device stat data on
 * 32-bit processors.
 */
#if BITS_PER_LONG==32 && defined(CONFIG_SMP)
#include <linux/seqlock.h>
#define __APFS_NEED_DEVICE_DATA_ORDERED
#define apfs_device_data_ordered_init(device)	\
	seqcount_init(&device->data_seqcount)
#else
#define apfs_device_data_ordered_init(device) do { } while (0)
#endif

#define APFS_DEV_STATE_WRITEABLE	(0)
#define APFS_DEV_STATE_IN_FS_METADATA	(1)
#define APFS_DEV_STATE_MISSING		(2)
#define APFS_DEV_STATE_REPLACE_TGT	(3)
#define APFS_DEV_STATE_FLUSH_SENT	(4)
#define APFS_DEV_STATE_NO_READA	(5)

struct apfs_zoned_device_info;

struct apfs_device {
	struct list_head dev_list; /* device_list_mutex */
	struct list_head dev_alloc_list; /* chunk mutex */
	struct list_head post_commit_list; /* chunk mutex */
	struct apfs_fs_devices *fs_devices;
	struct apfs_fs_info *fs_info;
	struct apfs_nx_info *nx_info;

	struct rcu_string __rcu *name;

	u64 generation;

	struct block_device *bdev;

	struct apfs_zoned_device_info *zone_info;

	/* the mode sent to blkdev_get */
	fmode_t mode;

	unsigned long dev_state;
	blk_status_t last_flush_error;

#ifdef __APFS_NEED_DEVICE_DATA_ORDERED
	seqcount_t data_seqcount;
#endif

	/* the internal apfs device id */
	u64 devid;

	/* size of the device in memory */
	u64 total_bytes;

	/* size of the device on disk */
	u64 disk_total_bytes;

	/* bytes used */
	u64 bytes_used;

	/* optimal io alignment for this device */
	u32 io_align;

	/* optimal io width for this device */
	u32 io_width;
	/* type and info about this device */
	u64 type;

	/* minimal io size for this device */
	u32 sector_size;

	/* physical drive uuid (or lvm uuid) */
	u8 uuid[APFS_UUID_SIZE];

	/*
	 * size of the device on the current transaction
	 *
	 * This variant is update when committing the transaction,
	 * and protected by chunk mutex
	 */
	u64 commit_total_bytes;

	/* bytes used on the current transaction */
	u64 commit_bytes_used;

	/* for sending down flush barriers */
	struct bio *flush_bio;
	struct completion flush_wait;

	/* per-device scrub information */
	struct scrub_ctx *scrub_ctx;

	/* readahead state */
	atomic_t reada_in_flight;
	u64 reada_next;
	struct reada_zone *reada_curr_zone;
	struct radix_tree_root reada_zones;
	struct radix_tree_root reada_extents;

	/* disk I/O failure stats. For detailed description refer to
	 * enum apfs_dev_stat_values in ioctl.h */
	int dev_stats_valid;

	/* Counter to record the change of device stats */
	atomic_t dev_stats_ccnt;
	atomic_t dev_stat_values[APFS_DEV_STAT_VALUES_MAX];

	struct extent_io_tree alloc_state;

	struct completion kobj_unregister;
	/* For sysfs/FSID/devinfo/devid/ */
	struct kobject devid_kobj;

	/* Bandwidth limit for scrub, in bytes */
	u64 scrub_speed_max;
};

/*
 * If we read those variants at the context of their own lock, we needn't
 * use the following helpers, reading them directly is safe.
 */
#if BITS_PER_LONG==32 && defined(CONFIG_SMP)
#define APFS_DEVICE_GETSET_FUNCS(name)					\
static inline u64							\
apfs_device_get_##name(const struct apfs_device *dev)			\
{									\
	u64 size;							\
	unsigned int seq;						\
									\
	do {								\
		seq = read_seqcount_begin(&dev->data_seqcount);		\
		size = dev->name;					\
	} while (read_seqcount_retry(&dev->data_seqcount, seq));	\
	return size;							\
}									\
									\
static inline void							\
apfs_device_set_##name(struct apfs_device *dev, u64 size)		\
{									\
	preempt_disable();						\
	write_seqcount_begin(&dev->data_seqcount);			\
	dev->name = size;						\
	write_seqcount_end(&dev->data_seqcount);			\
	preempt_enable();						\
}
#elif BITS_PER_LONG==32 && defined(CONFIG_PREEMPTION)
#define APFS_DEVICE_GETSET_FUNCS(name)					\
static inline u64							\
apfs_device_get_##name(const struct apfs_device *dev)			\
{									\
	u64 size;							\
									\
	preempt_disable();						\
	size = dev->name;						\
	preempt_enable();						\
	return size;							\
}									\
									\
static inline void							\
apfs_device_set_##name(struct apfs_device *dev, u64 size)		\
{									\
	preempt_disable();						\
	dev->name = size;						\
	preempt_enable();						\
}
#else
#define APFS_DEVICE_GETSET_FUNCS(name)					\
static inline u64							\
apfs_device_get_##name(const struct apfs_device *dev)			\
{									\
	return dev->name;						\
}									\
									\
static inline void							\
apfs_device_set_##name(struct apfs_device *dev, u64 size)		\
{									\
	dev->name = size;						\
}
#endif

APFS_DEVICE_GETSET_FUNCS(total_bytes);
APFS_DEVICE_GETSET_FUNCS(disk_total_bytes);
APFS_DEVICE_GETSET_FUNCS(bytes_used);

enum apfs_chunk_allocation_policy {
	APFS_CHUNK_ALLOC_REGULAR,
	APFS_CHUNK_ALLOC_ZONED,
};

/*
 * Read policies for mirrored block group profiles, read picks the stripe based
 * on these policies.
 */
enum apfs_read_policy {
	/* Use process PID to choose the stripe */
	APFS_READ_POLICY_PID,
	APFS_NR_READ_POLICY,
};

struct apfs_fs_devices {
	u8 fsid[APFS_FSID_SIZE]; /* FS specific uuid */
	u8 metadata_uuid[APFS_FSID_SIZE];
	bool fsid_change;
	struct list_head fs_list;

	u64 num_devices;
	u64 open_devices;
	u64 rw_devices;
	u64 missing_devices;
	u64 total_rw_bytes;
	u64 total_devices;

	/* Highest generation number of seen devices */
	u64 latest_generation;

	struct block_device *latest_bdev;

	/* all of the devices in the FS, protected by a mutex
	 * so we can safely walk it to write out the supers without
	 * worrying about add/remove by the multi-device code.
	 * Scrubbing super can kick off supers writing by holding
	 * this mutex lock.
	 */
	struct mutex device_list_mutex;

	/* List of all devices, protected by device_list_mutex */
	struct list_head devices;

	/*
	 * Devices which can satisfy space allocation. Protected by
	 * chunk_mutex
	 */
	struct list_head alloc_list;

	struct list_head seed_list;
	bool seeding;

	int opened;

	/* set when we find or add a device that doesn't have the
	 * nonrot flag set
	 */
	bool rotating;

	struct apfs_fs_info *fs_info;
	/* sysfs kobjects */
	struct kobject fsid_kobj;
	struct kobject *devices_kobj;
	struct kobject *devinfo_kobj;
	struct completion kobj_unregister;

	enum apfs_chunk_allocation_policy chunk_alloc_policy;

	/* Policy used to read the mirrored stripes */
	enum apfs_read_policy read_policy;
};

#define APFS_BIO_INLINE_CSUM_SIZE	64

#define APFS_MAX_DEVS(info) ((APFS_MAX_ITEM_SIZE(info)	\
			- sizeof(struct apfs_chunk))		\
			/ sizeof(struct apfs_stripe) + 1)

#define APFS_MAX_DEVS_SYS_CHUNK ((APFS_SYSTEM_CHUNK_ARRAY_SIZE	\
				- 2 * sizeof(struct apfs_disk_key)	\
				- 2 * sizeof(struct apfs_chunk))	\
				/ sizeof(struct apfs_stripe) + 1)

/*
 * we need the mirror number and stripe index to be passed around
 * the call chain while we are processing end_io (especially errors).
 * Really, what we need is a apfs_bio structure that has this info
 * and is properly sized with its stripe array, but we're not there
 * quite yet.  We have our own apfs bioset, and all of the bios
 * we allocate are actually apfs_io_bios.  We'll cram as much of
 * struct apfs_bio as we can into this over time.
 */
struct apfs_io_bio {
	unsigned int mirror_num;
	struct apfs_device *device;
	u64 logical;
	u8 *csum;
	u8 csum_inline[APFS_BIO_INLINE_CSUM_SIZE];
	struct bvec_iter iter;
	/*
	 * This member must come last, bio_alloc_bioset will allocate enough
	 * bytes for entire apfs_io_bio but relies on bio being last.
	 */
	struct bio bio;
};

static inline struct apfs_io_bio *apfs_io_bio(struct bio *bio)
{
	return container_of(bio, struct apfs_io_bio, bio);
}

static inline void apfs_io_bio_free_csum(struct apfs_io_bio *io_bio)
{
	return;
}

struct apfs_bio_stripe {
	struct apfs_device *dev;
	u64 physical;
	u64 length; /* only used for discard mappings */
};

struct apfs_bio {
	refcount_t refs;
	atomic_t stripes_pending;
	struct apfs_fs_info *fs_info;
	u64 map_type; /* get from map_lookup->type */
	bio_end_io_t *end_io;
	struct bio *orig_bio;
	void *private;
	atomic_t error;
	int max_errors;
	int num_stripes;
	int mirror_num;
	int num_tgtdevs;
	int *tgtdev_map;
	/*
	 * logical block numbers for the start of each stripe
	 * The last one or two are p/q.  These are sorted,
	 * so raid_map[0] is the start of our full stripe
	 */
	u64 *raid_map;
	struct apfs_bio_stripe stripes[];
};

struct apfs_device_info {
	struct apfs_device *dev;
	u64 dev_offset;
	u64 max_avail;
	u64 total_avail;
};

struct apfs_raid_attr {
	u8 sub_stripes;		/* sub_stripes info for map */
	u8 dev_stripes;		/* stripes per dev */
	u8 devs_max;		/* max devs to use */
	u8 devs_min;		/* min devs needed */
	u8 tolerated_failures;	/* max tolerated fail devs */
	u8 devs_increment;	/* ndevs has to be a multiple of this */
	u8 ncopies;		/* how many copies to data has */
	u8 nparity;		/* number of stripes worth of bytes to store
				 * parity information */
	u8 mindev_error;	/* error code if min devs requisite is unmet */
	const char raid_name[8]; /* name of the raid */
	u64 bg_flag;		/* block group flag of the raid */
};

extern const struct apfs_raid_attr apfs_raid_array[APFS_NR_RAID_TYPES];

struct map_lookup {
	u64 type;
	int io_align;
	int io_width;
	u64 stripe_len;
	int num_stripes;
	int sub_stripes;
	int verified_stripes; /* For mount time dev extent verification */
	struct apfs_bio_stripe stripes[];
};

#define map_lookup_size(n) (sizeof(struct map_lookup) + \
			    (sizeof(struct apfs_bio_stripe) * (n)))

struct apfs_balance_args;
struct apfs_balance_progress;
struct apfs_balance_control {
	struct apfs_balance_args data;
	struct apfs_balance_args meta;
	struct apfs_balance_args sys;

	u64 flags;

	struct apfs_balance_progress stat;
};

enum apfs_map_op {
	APFS_MAP_READ,
	APFS_MAP_WRITE,
	APFS_MAP_DISCARD,
	APFS_MAP_GET_READ_MIRRORS,
};

static inline enum apfs_map_op apfs_op(struct bio *bio)
{
	switch (bio_op(bio)) {
	case REQ_OP_DISCARD:
		return APFS_MAP_DISCARD;
	case REQ_OP_WRITE:
	case REQ_OP_ZONE_APPEND:
		return APFS_MAP_WRITE;
	default:
		WARN_ON_ONCE(1);
		fallthrough;
	case REQ_OP_READ:
		return APFS_MAP_READ;
	}
}

void apfs_get_bbio(struct apfs_bio *bbio);
void apfs_put_bbio(struct apfs_bio *bbio);
int apfs_map_block(struct apfs_fs_info *fs_info, enum apfs_map_op op,
		    u64 logical, u64 *length,
		    struct apfs_bio **bbio_ret, int mirror_num);
int apfs_map_sblock(struct apfs_fs_info *fs_info, enum apfs_map_op op,
		     u64 logical, u64 *length,
		     struct apfs_bio **bbio_ret);
int apfs_get_io_geometry(struct apfs_fs_info *fs_info, struct extent_map *map,
			  enum apfs_map_op op, u64 logical,
			  struct apfs_io_geometry *io_geom);
int apfs_read_sys_array(struct apfs_fs_info *fs_info);
int apfs_read_chunk_tree(struct apfs_fs_info *fs_info);
struct apfs_block_group *apfs_alloc_chunk(struct apfs_trans_handle *trans,
					    u64 type);
void apfs_mapping_tree_free(struct extent_map_tree *tree);
blk_status_t apfs_map_bio(struct apfs_fs_info *fs_info, struct bio *bio,
			   int mirror_num);
int apfs_open_devices(struct apfs_fs_devices *fs_devices,
		       fmode_t flags, void *holder);
struct apfs_device *apfs_scan_one_device(const char *path,
					 fmode_t flags, void *holder);
int apfs_forget_devices(const char *path);
void apfs_close_devices(struct apfs_fs_devices *fs_devices);
void apfs_free_extra_devids(struct apfs_fs_devices *fs_devices);
void apfs_assign_next_active_device(struct apfs_device *device,
				     struct apfs_device *this_dev);
struct apfs_device *apfs_find_device_by_devspec(struct apfs_fs_info *fs_info,
						  u64 devid,
						  const char *devpath);
struct apfs_device *apfs_alloc_device(struct apfs_fs_info *fs_info,
					const u64 *devid,
					const u8 *uuid);
void apfs_free_device(struct apfs_device *device);
int apfs_rm_device(struct apfs_fs_info *fs_info,
		    const char *device_path, u64 devid);
void __exit apfs_cleanup_fs_uuids(void);
int apfs_num_copies(struct apfs_fs_info *fs_info, u64 logical, u64 len);
int apfs_grow_device(struct apfs_trans_handle *trans,
		      struct apfs_device *device, u64 new_size);
struct apfs_device *apfs_find_device(struct apfs_fs_devices *fs_devices,
				       u64 devid, u8 *uuid, u8 *fsid);
int apfs_shrink_device(struct apfs_device *device, u64 new_size);
int apfs_init_new_device(struct apfs_fs_info *fs_info, const char *path);
int apfs_balance(struct apfs_fs_info *fs_info,
		  struct apfs_balance_control *bctl,
		  struct apfs_ioctl_balance_args *bargs);
void apfs_describe_block_groups(u64 flags, char *buf, u32 size_buf);
int apfs_resume_balance_async(struct apfs_fs_info *fs_info);
int apfs_recover_balance(struct apfs_fs_info *fs_info);
int apfs_pause_balance(struct apfs_fs_info *fs_info);
int apfs_relocate_chunk(struct apfs_fs_info *fs_info, u64 chunk_offset);
int apfs_cancel_balance(struct apfs_fs_info *fs_info);
int apfs_create_uuid_tree(struct apfs_fs_info *fs_info);
int apfs_uuid_scan_kthread(void *data);
int apfs_chunk_readonly(struct apfs_fs_info *fs_info, u64 chunk_offset);
int find_free_dev_extent(struct apfs_device *device, u64 num_bytes,
			 u64 *start, u64 *max_avail);
void apfs_dev_stat_inc_and_print(struct apfs_device *dev, int index);
int apfs_get_dev_stats(struct apfs_fs_info *fs_info,
			struct apfs_ioctl_get_dev_stats *stats);
void apfs_init_devices_late(struct apfs_fs_info *fs_info);
int apfs_init_dev_stats(struct apfs_fs_info *fs_info);
int apfs_run_dev_stats(struct apfs_trans_handle *trans);
void apfs_rm_dev_replace_remove_srcdev(struct apfs_device *srcdev);
void apfs_rm_dev_replace_free_srcdev(struct apfs_device *srcdev);
void apfs_destroy_dev_replace_tgtdev(struct apfs_device *tgtdev);
int apfs_is_parity_mirror(struct apfs_fs_info *fs_info,
			   u64 logical, u64 len);
unsigned long apfs_full_stripe_len(struct apfs_fs_info *fs_info,
				    u64 logical);
int apfs_finish_chunk_alloc(struct apfs_trans_handle *trans,
			     u64 chunk_offset, u64 chunk_size);
int apfs_chunk_alloc_add_chunk_item(struct apfs_trans_handle *trans,
				     struct apfs_block_group *bg);
int apfs_remove_chunk(struct apfs_trans_handle *trans, u64 chunk_offset);
struct extent_map *apfs_get_chunk_map(struct apfs_fs_info *fs_info,
				       u64 logical, u64 length);
void apfs_release_disk_super(struct apfs_super_block *super);
void apfs_release_nx_super(struct apfs_nx_superblock *super);

static inline void apfs_dev_stat_inc(struct apfs_device *dev,
				      int index)
{
	atomic_inc(dev->dev_stat_values + index);
	/*
	 * This memory barrier orders stores updating statistics before stores
	 * updating dev_stats_ccnt.
	 *
	 * It pairs with smp_rmb() in apfs_run_dev_stats().
	 */
	smp_mb__before_atomic();
	atomic_inc(&dev->dev_stats_ccnt);
}

static inline int apfs_dev_stat_read(struct apfs_device *dev,
				      int index)
{
	return atomic_read(dev->dev_stat_values + index);
}

static inline int apfs_dev_stat_read_and_reset(struct apfs_device *dev,
						int index)
{
	int ret;

	ret = atomic_xchg(dev->dev_stat_values + index, 0);
	/*
	 * atomic_xchg implies a full memory barriers as per atomic_t.txt:
	 * - RMW operations that have a return value are fully ordered;
	 *
	 * This implicit memory barriers is paired with the smp_rmb in
	 * apfs_run_dev_stats
	 */
	atomic_inc(&dev->dev_stats_ccnt);
	return ret;
}

static inline void apfs_dev_stat_set(struct apfs_device *dev,
				      int index, unsigned long val)
{
	atomic_set(dev->dev_stat_values + index, val);
	/*
	 * This memory barrier orders stores updating statistics before stores
	 * updating dev_stats_ccnt.
	 *
	 * It pairs with smp_rmb() in apfs_run_dev_stats().
	 */
	smp_mb__before_atomic();
	atomic_inc(&dev->dev_stats_ccnt);
}

/*
 * Convert block group flags (APFS_BLOCK_GROUP_*) to apfs_raid_types, which
 * can be used as index to access apfs_raid_array[].
 */
static inline enum apfs_raid_types apfs_bg_flags_to_raid_index(u64 flags)
{
	if (flags & APFS_BLOCK_GROUP_RAID10)
		return APFS_RAID_RAID10;
	else if (flags & APFS_BLOCK_GROUP_RAID1)
		return APFS_RAID_RAID1;
	else if (flags & APFS_BLOCK_GROUP_RAID1C3)
		return APFS_RAID_RAID1C3;
	else if (flags & APFS_BLOCK_GROUP_RAID1C4)
		return APFS_RAID_RAID1C4;
	else if (flags & APFS_BLOCK_GROUP_DUP)
		return APFS_RAID_DUP;
	else if (flags & APFS_BLOCK_GROUP_RAID0)
		return APFS_RAID_RAID0;
	else if (flags & APFS_BLOCK_GROUP_RAID5)
		return APFS_RAID_RAID5;
	else if (flags & APFS_BLOCK_GROUP_RAID6)
		return APFS_RAID_RAID6;

	return APFS_RAID_SINGLE; /* APFS_BLOCK_GROUP_SINGLE */
}

void apfs_commit_device_sizes(struct apfs_transaction *trans);

struct list_head * __attribute_const__ apfs_get_fs_uuids(void);
bool apfs_check_rw_degradable(struct apfs_fs_info *fs_info,
					struct apfs_device *failing_dev);
void apfs_scratch_superblocks(struct apfs_fs_info *fs_info,
			       struct block_device *bdev,
			       const char *device_path);

int apfs_bg_type_to_factor(u64 flags);
const char *apfs_bg_type_to_raid_name(u64 flags);
int apfs_verify_dev_extents(struct apfs_fs_info *fs_info);
int apfs_repair_one_zone(struct apfs_fs_info *fs_info, u64 logical);

struct list_head * __attribute_const__ apfs_get_fs_devices(void);
void apfs_close_device(struct apfs_device *device);
int apfs_add_whole_device_mapping(struct apfs_fs_info *fs_info);
void apfs_release_volume_super(struct apfs_vol_superblock *super);
struct apfs_nx_superblock *apfs_read_nx_super(struct block_device *bdev);
#endif
