/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _APFS_CTREE_H_
#define _APFS_CTREE_H_

#include "apfs.h"
#include <linux/types.h>
#ifdef __KERNEL__
#include <linux/stddef.h>
#else
#include <stddef.h>
#endif

/*
 * This header contains the structure definitions and constants used
 * by file system objects that can be retrieved using
 * the APFS_IOC_SEARCH_TREE ioctl.  That means basically anything that
 * is needed to describe a leaf node's key or item contents.
 */

/* holds pointers to all of the tree roots */
#define APFS_ROOT_TREE_OBJECTID 1ULL

/* stores information about which extents are in use, and reference counts */
#define APFS_EXTENT_TREE_OBJECTID 2ULL

/*
 * chunk tree stores translations from logical -> physical block numbering
 * the super block points to the chunk tree
 */
#define APFS_CHUNK_TREE_OBJECTID 3ULL

/*
 * stores information about which areas of a given device are in use.
 * one per device.  The tree of tree roots points to the device tree
 */
#define APFS_DEV_TREE_OBJECTID 4ULL

/* one per subvolume, storing files and directories */
#define APFS_FS_TREE_OBJECTID 5ULL

/* directory objectid inside the root tree */
#define APFS_ROOT_TREE_DIR_OBJECTID 6ULL

/* holds checksums of all the data extents */
#define APFS_CSUM_TREE_OBJECTID 7ULL

/* holds quota configuration and tracking */
#define APFS_QUOTA_TREE_OBJECTID 8ULL

/* for storing items that use the APFS_UUID_KEY* types */
#define APFS_UUID_TREE_OBJECTID 9ULL

/* tracks free space in block groups. */
#define APFS_FREE_SPACE_TREE_OBJECTID 10ULL

/* device stats in the device tree */
#define APFS_DEV_STATS_OBJECTID 0ULL

/* for storing balance parameters in the root tree */
#define APFS_BALANCE_OBJECTID -4ULL

/* orphan objectid for tracking unlinked/truncated files */
#define APFS_ORPHAN_OBJECTID -5ULL

/* does write ahead logging to speed up fsyncs */
#define APFS_TREE_LOG_OBJECTID -6ULL
#define APFS_TREE_LOG_FIXUP_OBJECTID -7ULL

/* for space balancing */
#define APFS_TREE_RELOC_OBJECTID -8ULL
#define APFS_DATA_RELOC_TREE_OBJECTID -9ULL

/*
 * extent checksums all have this objectid
 * this allows them to share the logging tree
 * for fsyncs
 */
#define APFS_EXTENT_CSUM_OBJECTID -10ULL

/* For storing free space cache */
#define APFS_FREE_SPACE_OBJECTID -11ULL

/*
 * The inode number assigned to the special inode for storing
 * free ino cache
 */
#define APFS_FREE_INO_OBJECTID -12ULL

/* dummy objectid represents multiple objectids */
#define APFS_MULTIPLE_OBJECTIDS -255ULL

/*
 * All files have objectids in this range.
 */
#define APFS_FIRST_FREE_OBJECTID 256ULL
#define APFS_LAST_FREE_OBJECTID -256ULL
#define APFS_FIRST_CHUNK_TREE_OBJECTID 256ULL


/*
 * the device items go into the chunk tree.  The key is in the form
 * [ 1 APFS_DEV_ITEM_KEY device_id ]
 */
#define APFS_DEV_ITEMS_OBJECTID 1ULL

#define APFS_BTREE_INODE_OBJECTID (u64)-1

#define APFS_EMPTY_SUBVOL_DIR_OBJECTID 2

#define APFS_DEV_REPLACE_DEVID 0ULL

/*
 * inode items have the data typically returned from stat and store other
 * info about object characteristics.  There is one for every file and dir in
 * the FS
 */
#define APFS_INODE_ITEM_KEY		1
#define APFS_INODE_REF_KEY		12
#define APFS_INODE_EXTREF_KEY		13
#define APFS_XATTR_ITEM_KEY		24
#define APFS_ORPHAN_ITEM_KEY		48
/* reserve 2-15 close to the inode for later flexibility */

/*
 * dir items are the name -> inode pointers in a directory.  There is one
 * for every name in a directory.
 */
#define APFS_DIR_LOG_ITEM_KEY  60
#define APFS_DIR_LOG_INDEX_KEY 72
#define APFS_DIR_ITEM_KEY	84
#define APFS_DIR_INDEX_KEY	96
/*
 * extent data is for file data
 */
#define APFS_EXTENT_DATA_KEY	108

/*
 * extent csums are stored in a separate tree and hold csums for
 * an entire extent on disk.
 */
#define APFS_EXTENT_CSUM_KEY	128

/*
 * root items point to tree roots.  They are typically in the root
 * tree used by the super block to find all the other trees
 */
#define APFS_ROOT_ITEM_KEY	132

/*
 * root backrefs tie subvols and snapshots to the directory entries that
 * reference them
 */
#define APFS_ROOT_BACKREF_KEY	144

/*
 * root refs make a fast index for listing all of the snapshots and
 * subvolumes referenced by a given root.  They point directly to the
 * directory item in the root that references the subvol
 */
#define APFS_ROOT_REF_KEY	156

/*
 * extent items are in the extent map tree.  These record which blocks
 * are used, and how many references there are to each block
 */
#define APFS_EXTENT_ITEM_KEY	168

/*
 * The same as the APFS_EXTENT_ITEM_KEY, except it's metadata we already know
 * the length, so we save the level in key->offset instead of the length.
 */
#define APFS_METADATA_ITEM_KEY	169

#define APFS_TREE_BLOCK_REF_KEY	176

#define APFS_EXTENT_DATA_REF_KEY	178

#define APFS_EXTENT_REF_V0_KEY		180

#define APFS_SHARED_BLOCK_REF_KEY	182

#define APFS_SHARED_DATA_REF_KEY	184

/*
 * block groups give us hints into the extent allocation trees.  Which
 * blocks are free etc etc
 */
#define APFS_BLOCK_GROUP_ITEM_KEY 192

/*
 * Every block group is represented in the free space tree by a free space info
 * item, which stores some accounting information. It is keyed on
 * (block_group_start, FREE_SPACE_INFO, block_group_length).
 */
#define APFS_FREE_SPACE_INFO_KEY 198

/*
 * A free space extent tracks an extent of space that is free in a block group.
 * It is keyed on (start, FREE_SPACE_EXTENT, length).
 */
#define APFS_FREE_SPACE_EXTENT_KEY 199

/*
 * When a block group becomes very fragmented, we convert it to use bitmaps
 * instead of extents. A free space bitmap is keyed on
 * (start, FREE_SPACE_BITMAP, length); the corresponding item is a bitmap with
 * (length / sectorsize) bits.
 */
#define APFS_FREE_SPACE_BITMAP_KEY 200

#define APFS_DEV_EXTENT_KEY	204
#define APFS_DEV_ITEM_KEY	216
#define APFS_CHUNK_ITEM_KEY	228

/*
 * Records the overall state of the qgroups.
 * There's only one instance of this key present,
 * (0, APFS_QGROUP_STATUS_KEY, 0)
 */
#define APFS_QGROUP_STATUS_KEY         240
/*
 * Records the currently used space of the qgroup.
 * One key per qgroup, (0, APFS_QGROUP_INFO_KEY, qgroupid).
 */
#define APFS_QGROUP_INFO_KEY           242
/*
 * Contains the user configured limits for the qgroup.
 * One key per qgroup, (0, APFS_QGROUP_LIMIT_KEY, qgroupid).
 */
#define APFS_QGROUP_LIMIT_KEY          244
/*
 * Records the child-parent relationship of qgroups. For
 * each relation, 2 keys are present:
 * (childid, APFS_QGROUP_RELATION_KEY, parentid)
 * (parentid, APFS_QGROUP_RELATION_KEY, childid)
 */
#define APFS_QGROUP_RELATION_KEY       246

/*
 * Obsolete name, see APFS_TEMPORARY_ITEM_KEY.
 */
#define APFS_BALANCE_ITEM_KEY	248

/*
 * The key type for tree items that are stored persistently, but do not need to
 * exist for extended period of time. The items can exist in any tree.
 *
 * [subtype, APFS_TEMPORARY_ITEM_KEY, data]
 *
 * Existing items:
 *
 * - balance status item
 *   (APFS_BALANCE_OBJECTID, APFS_TEMPORARY_ITEM_KEY, 0)
 */
#define APFS_TEMPORARY_ITEM_KEY	248

/*
 * Obsolete name, see APFS_PERSISTENT_ITEM_KEY
 */
#define APFS_DEV_STATS_KEY		249

/*
 * The key type for tree items that are stored persistently and usually exist
 * for a long period, eg. filesystem lifetime. The item kinds can be status
 * information, stats or preference values. The item can exist in any tree.
 *
 * [subtype, APFS_PERSISTENT_ITEM_KEY, data]
 *
 * Existing items:
 *
 * - device statistics, store IO stats in the device tree, one key for all
 *   stats
 *   (APFS_DEV_STATS_OBJECTID, APFS_DEV_STATS_KEY, 0)
 */
#define APFS_PERSISTENT_ITEM_KEY	249

/*
 * Persistently stores the device replace state in the device tree.
 * The key is built like this: (0, APFS_DEV_REPLACE_KEY, 0).
 */
#define APFS_DEV_REPLACE_KEY	250

/*
 * Stores items that allow to quickly map UUIDs to something else.
 * These items are part of the filesystem UUID tree.
 * The key is built like this:
 * (UUID_upper_64_bits, APFS_UUID_KEY*, UUID_lower_64_bits).
 */
#if APFS_UUID_SIZE != 16
#error "UUID items require APFS_UUID_SIZE == 16!"
#endif
#define APFS_UUID_KEY_SUBVOL	251	/* for UUIDs assigned to subvols */
#define APFS_UUID_KEY_RECEIVED_SUBVOL	252	/* for UUIDs assigned to
						 * received subvols */

/*
 * string items are for debugging.  They just store a short string of
 * data in the FS
 */
#define APFS_STRING_ITEM_KEY	253

/* Maximum metadata block size (nodesize) */
#define APFS_MAX_METADATA_BLOCKSIZE			65536

/* 32 bytes in various csum fields */
#define APFS_CSUM_SIZE 8

/* csum types */
enum apfs_csum_type {
	APFS_CSUM_TYPE_CRC32	= 0,
	APFS_CSUM_TYPE_XXHASH	= 1,
	APFS_CSUM_TYPE_SHA256	= 2,
	APFS_CSUM_TYPE_BLAKE2	= 3,
};

/*
 * flags definitions for directory entry item type
 *
 * Used by:
 * struct apfs_drec_item.type
 *
 * Values 0..7 must match common file type values in fs_types.h.
 */
#define APFS_FT_UNKNOWN		0
#define APFS_FT_FIFO		1
#define APFS_FT_CHRDEV		2
#define APFS_FT_DIR		4
#define APFS_FT_BLKDEV		6
#define APFS_FT_REG_FILE	8
#define APFS_FT_SYMLINK		10
#define APFS_FT_SOCK		12
#define APFS_FT_WHT		14
#define APFS_FT_MAX		14
#define APFS_FT_XATTR		14

struct apfs_key {
	union {
		u64 id;
		u64 objectid;
		u64 id_and_type;
		u64 oid;
	};

	union {
		u64 offset;
		u64 paddr; // space_free_key_key::paddr
		u64 xid;
		struct {
			u16 namelen;
			/* hash is high 22 bits */
			u32 hash;
			u16 pad;
		};
	};

	u8 type;
	const char *name;
};

/*
 * The key defines the order in the tree, and so it also defines (optimal)
 * block layout.
 *
 * objectid corresponds to the inode number.
 *
 * type tells us things about the object, and is a kind of stream selector.
 * so for a given inode, keys with type of 1 might refer to the inode data,
 * type of 2 may point to file data in the btree and type == 3 may point to
 * extents.
 *
 * offset is the starting byte offset for this key in the stream.
 *
 * apfs_disk_key is in disk byte order.  struct apfs_key is always
 * in cpu native order.  Otherwise they are identical and their sizes
 * should be the same (ie both packed)
 */
struct apfs_disk_key {
	union {
		__le64 id;
		__le64 id_and_type;
		__le64 oid;
		__le64 objectid;
		__le64 type; //trivial
	};

	union {
		__le64 offset;
		struct {
			__le16 namelen1;
			u8 name1[0];
		};

		struct {
			__le32 namelen_and_hash;
			u8 name2[0];
		};
	};
};

struct apfs_dev_item {
	/* the internal apfs device id */
	__le64 devid;

	/* size of the device */
	__le64 total_bytes;

	/* bytes used */
	__le64 bytes_used;

	/* optimal io alignment for this device */
	__le32 io_align;

	/* optimal io width for this device */
	__le32 io_width;

	/* minimal io size for this device */
	__le32 sector_size;

	/* type and info about this device */
	__le64 type;

	/* expected generation for this device */
	__le64 generation;

	/*
	 * starting byte of this partition on the device,
	 * to allow for stripe alignment in the future
	 */
	__le64 start_offset;

	/* grouping information for allocation decisions */
	__le32 dev_group;

	/* seek speed 0-100 where 100 is fastest */
	__u8 seek_speed;

	/* bandwidth 0-100 where 100 is fastest */
	__u8 bandwidth;

	/* apfs generated uuid for this device */
	__u8 uuid[APFS_UUID_SIZE];

	/* uuid of FS who owns this device */
	__u8 fsid[APFS_UUID_SIZE];
} __attribute__ ((__packed__));

struct apfs_stripe {
	__le64 devid;
	__le64 offset;
	__u8 dev_uuid[APFS_UUID_SIZE];
} __attribute__ ((__packed__));

struct apfs_chunk {
	/* size of this chunk in bytes */
	__le64 length;

	/* objectid of the root referencing this chunk */
	__le64 owner;

	__le64 stripe_len;
	__le64 type;

	/* optimal io alignment for this chunk */
	__le32 io_align;

	/* optimal io width for this chunk */
	__le32 io_width;

	/* minimal io size for this chunk */
	__le32 sector_size;

	/* 2^16 stripes is quite a lot, a second limit is the size of a single
	 * item in the btree
	 */
	__le16 num_stripes;

	/* sub stripes only matter for raid10 */
	__le16 sub_stripes;
	struct apfs_stripe stripe;
	/* additional stripes go here */
} __attribute__ ((__packed__));

#define APFS_FREE_SPACE_EXTENT	1
#define APFS_FREE_SPACE_BITMAP	2

struct apfs_free_space_entry {
	__le64 offset;
	__le64 bytes;
	__u8 type;
} __attribute__ ((__packed__));

struct apfs_free_space_header {
	struct apfs_disk_key location;
	__le64 generation;
	__le64 num_entries;
	__le64 num_bitmaps;
} __attribute__ ((__packed__));

#define APFS_HEADER_FLAG_WRITTEN	(1ULL << 0)
#define APFS_HEADER_FLAG_RELOC		(1ULL << 1)

/* Super block flags */
/* Errors detected */
#define APFS_SUPER_FLAG_ERROR		(1ULL << 2)

#define APFS_SUPER_FLAG_SEEDING	(1ULL << 32)
#define APFS_SUPER_FLAG_METADUMP	(1ULL << 33)
#define APFS_SUPER_FLAG_METADUMP_V2	(1ULL << 34)
#define APFS_SUPER_FLAG_CHANGING_FSID	(1ULL << 35)
#define APFS_SUPER_FLAG_CHANGING_FSID_V2 (1ULL << 36)


/*
 * items in the extent btree are used to record the objectid of the
 * owner of the block and the number of references
 */

struct apfs_extent_item {
	__le64 refs;
	__le64 generation;
	__le64 flags;
} __attribute__ ((__packed__));

struct apfs_extent_item_v0 {
	__le32 refs;
} __attribute__ ((__packed__));


#define APFS_EXTENT_FLAG_DATA		(1ULL << 0)
#define APFS_EXTENT_FLAG_TREE_BLOCK	(1ULL << 1)

/* following flags only apply to tree blocks */

/* use full backrefs for extent pointers in the block */
#define APFS_BLOCK_FLAG_FULL_BACKREF	(1ULL << 8)

/*
 * this flag is only used internally by scrub and may be changed at any time
 * it is only declared here to avoid collisions
 */
#define APFS_EXTENT_FLAG_SUPER		(1ULL << 48)

struct apfs_tree_block_info {
	struct apfs_disk_key key;
	__u8 level;
} __attribute__ ((__packed__));

struct apfs_extent_data_ref {
	__le64 root;
	__le64 objectid;
	__le64 offset;
	__le32 count;
} __attribute__ ((__packed__));

struct apfs_shared_data_ref {
	__le32 count;
} __attribute__ ((__packed__));

struct apfs_extent_inline_ref {
	__u8 type;
	__le64 offset;
} __attribute__ ((__packed__));

/* dev extents record free space on individual devices.  The owner
 * field points back to the chunk allocation mapping tree that allocated
 * the extent.  The chunk tree uuid field is a way to double check the owner
 */
struct apfs_dev_extent {
	__le64 chunk_tree;
	__le64 chunk_objectid;
	__le64 chunk_offset;
	__le64 length;
	__u8 chunk_tree_uuid[APFS_UUID_SIZE];
} __attribute__ ((__packed__));

struct apfs_inode_ref {
	__le64 index;
	__le16 name_len;
	/* name goes here */
} __attribute__ ((__packed__));

struct apfs_inode_extref {
	__le64 parent_objectid;
	__le64 index;
	__le16 name_len;
	__u8   name[0];
	/* name goes here */
} __attribute__ ((__packed__));

struct apfs_timespec {
	__le64 sec;
	__le32 nsec;
} __attribute__ ((__packed__));

struct apfs_inode_item {
	/* nfs style generation number */
	__le64 generation;
	/* transid that last touched this inode */
	__le64 transid;
	__le64 size;
	__le64 nbytes;
	__le64 block_group;
	__le32 nlink;
	__le32 uid;
	__le32 gid;
	__le32 mode;
	__le64 rdev;
	__le64 flags;

	/* modification sequence number for NFS */
	__le64 sequence;

	/*
	 * a little future expansion, for more than this we can
	 * just grow the inode item and version it
	 */
	__le64 reserved[4];
	struct apfs_timespec atime;
	struct apfs_timespec ctime;
	struct apfs_timespec mtime;
	struct apfs_timespec otime;
} __attribute__ ((__packed__));

struct apfs_dir_log_item {
	__le64 end;
} __attribute__ ((__packed__));

struct apfs_dir_item {
	struct apfs_disk_key location;
	__le64 transid;
	__le16 data_len;
	__le16 name_len;
	__u8 type;
} __attribute__ ((__packed__));

#define APFS_ROOT_SUBVOL_RDONLY	(1ULL << 0)

/*
 * Internal in-memory flag that a subvolume has been marked for deletion but
 * still visible as a directory
 */
#define APFS_ROOT_SUBVOL_DEAD		(1ULL << 48)

struct apfs_root_item {
	struct apfs_inode_item inode;
	__le64 generation;
	__le64 root_dirid;
	__le64 bytenr;
	__le64 byte_limit;
	__le64 bytes_used;
	__le64 last_snapshot;
	__le64 flags;
	__le32 refs;
	struct apfs_disk_key drop_progress;
	__u8 drop_level;
	__u8 level;

	/*
	 * The following fields appear after subvol_uuids+subvol_times
	 * were introduced.
	 */

	/*
	 * This generation number is used to test if the new fields are valid
	 * and up to date while reading the root item. Every time the root item
	 * is written out, the "generation" field is copied into this field. If
	 * anyone ever mounted the fs with an older kernel, we will have
	 * mismatching generation values here and thus must invalidate the
	 * new fields. See apfs_update_root and apfs_find_last_root for
	 * details.
	 * the offset of generation_v2 is also used as the start for the memset
	 * when invalidating the fields.
	 */
	__le64 generation_v2;
	__u8 uuid[APFS_UUID_SIZE];
	__u8 parent_uuid[APFS_UUID_SIZE];
	__u8 received_uuid[APFS_UUID_SIZE];
	__le64 ctransid; /* updated when an inode changes */
	__le64 otransid; /* trans when created */
	__le64 stransid; /* trans when sent. non-zero for received subvol */
	__le64 rtransid; /* trans when received. non-zero for received subvol */
	struct apfs_timespec ctime;
	struct apfs_timespec otime;
	struct apfs_timespec stime;
	struct apfs_timespec rtime;
	__le64 reserved[8]; /* for future */
} __attribute__ ((__packed__));

/*
 * Btrfs root item used to be smaller than current size.  The old format ends
 * at where member generation_v2 is.
 */
static inline __u32 apfs_legacy_root_item_size(void)
{
	return offsetof(struct apfs_root_item, generation_v2);
}

/*
 * this is used for both forward and backward root refs
 */
struct apfs_root_ref {
	__le64 dirid;
	__le64 sequence;
	__le16 name_len;
} __attribute__ ((__packed__));

struct apfs_disk_balance_args {
	/*
	 * profiles to operate on, single is denoted by
	 * APFS_AVAIL_ALLOC_BIT_SINGLE
	 */
	__le64 profiles;

	/*
	 * usage filter
	 * APFS_BALANCE_ARGS_USAGE with a single value means '0..N'
	 * APFS_BALANCE_ARGS_USAGE_RANGE - range syntax, min..max
	 */
	union {
		__le64 usage;
		struct {
			__le32 usage_min;
			__le32 usage_max;
		};
	};

	/* devid filter */
	__le64 devid;

	/* devid subset filter [pstart..pend) */
	__le64 pstart;
	__le64 pend;

	/* apfs virtual address space subset filter [vstart..vend) */
	__le64 vstart;
	__le64 vend;

	/*
	 * profile to convert to, single is denoted by
	 * APFS_AVAIL_ALLOC_BIT_SINGLE
	 */
	__le64 target;

	/* APFS_BALANCE_ARGS_* */
	__le64 flags;

	/*
	 * APFS_BALANCE_ARGS_LIMIT with value 'limit'
	 * APFS_BALANCE_ARGS_LIMIT_RANGE - the extend version can use minimum
	 * and maximum
	 */
	union {
		__le64 limit;
		struct {
			__le32 limit_min;
			__le32 limit_max;
		};
	};

	/*
	 * Process chunks that cross stripes_min..stripes_max devices,
	 * APFS_BALANCE_ARGS_STRIPES_RANGE
	 */
	__le32 stripes_min;
	__le32 stripes_max;

	__le64 unused[6];
} __attribute__ ((__packed__));

/*
 * store balance parameters to disk so that balance can be properly
 * resumed after crash or unmount
 */
struct apfs_balance_item {
	/* APFS_BALANCE_* */
	__le64 flags;

	struct apfs_disk_balance_args data;
	struct apfs_disk_balance_args meta;
	struct apfs_disk_balance_args sys;

	__le64 unused[4];
} __attribute__ ((__packed__));

enum {
	APFS_FILE_EXTENT_INLINE   = 0,
	APFS_FILE_EXTENT_REG      = 1,
	APFS_FILE_EXTENT_PREALLOC = 2,
	APFS_NR_FILE_EXTENT_TYPES = 3,
};

struct apfs_file_extent_item {
	/*
	 * transaction id that created this extent
	 */
	__le64 generation;
	/*
	 * max number of bytes to hold this extent in ram
	 * when we split a compressed extent we can't know how big
	 * each of the resulting pieces will be.  So, this is
	 * an upper limit on the size of the extent in ram instead of
	 * an exact limit.
	 */
	__le64 ram_bytes;

	/*
	 * 32 bits for the various ways we might encode the data,
	 * including compression and encryption.  If any of these
	 * are set to something a given disk format doesn't understand
	 * it is treated like an incompat flag for reading and writing,
	 * but not for stat.
	 */
	__u8 compression;
	__u8 encryption;
	__le16 other_encoding; /* spare for later use */

	/* are we inline data or a real extent? */
	__u8 type;

	/*
	 * disk space consumed by the extent, checksum blocks are included
	 * in these numbers
	 *
	 * At this offset in the structure, the inline extent data start.
	 */
	__le64 disk_bytenr;
	__le64 disk_num_bytes;
	/*
	 * the logical offset in file blocks (no csums)
	 * this extent record is for.  This allows a file extent to point
	 * into the middle of an existing extent on disk, sharing it
	 * between two snapshots (useful if some bytes in the middle of the
	 * extent have changed
	 */
	__le64 offset;
	/*
	 * the logical number of file blocks (no csums included).  This
	 * always reflects the size uncompressed and without encoding.
	 */
	__le64 num_bytes;

} __attribute__ ((__packed__));

struct apfs_csum_item {
	__u8 csum;
} __attribute__ ((__packed__));

struct apfs_dev_stats_item {
	/*
	 * grow this item struct at the end for future enhancements and keep
	 * the existing values unchanged
	 */
	__le64 values[APFS_DEV_STAT_VALUES_MAX];
} __attribute__ ((__packed__));

#define APFS_DEV_REPLACE_ITEM_CONT_READING_FROM_SRCDEV_MODE_ALWAYS	0
#define APFS_DEV_REPLACE_ITEM_CONT_READING_FROM_SRCDEV_MODE_AVOID	1

struct apfs_dev_replace_item {
	/*
	 * grow this item struct at the end for future enhancements and keep
	 * the existing values unchanged
	 */
	__le64 src_devid;
	__le64 cursor_left;
	__le64 cursor_right;
	__le64 cont_reading_from_srcdev_mode;

	__le64 replace_state;
	__le64 time_started;
	__le64 time_stopped;
	__le64 num_write_errors;
	__le64 num_uncorrectable_read_errors;
} __attribute__ ((__packed__));

/* different types of block groups (and chunks) */
#define APFS_BLOCK_GROUP_DATA		(1ULL << 0)
#define APFS_BLOCK_GROUP_SYSTEM	(1ULL << 1)
#define APFS_BLOCK_GROUP_METADATA	(1ULL << 2)
#define APFS_BLOCK_GROUP_RAID0		(1ULL << 3)
#define APFS_BLOCK_GROUP_RAID1		(1ULL << 4)
#define APFS_BLOCK_GROUP_DUP		(1ULL << 5)
#define APFS_BLOCK_GROUP_RAID10	(1ULL << 6)
#define APFS_BLOCK_GROUP_RAID5         (1ULL << 7)
#define APFS_BLOCK_GROUP_RAID6         (1ULL << 8)
#define APFS_BLOCK_GROUP_RAID1C3       (1ULL << 9)
#define APFS_BLOCK_GROUP_RAID1C4       (1ULL << 10)
#define APFS_BLOCK_GROUP_RESERVED	(APFS_AVAIL_ALLOC_BIT_SINGLE | \
					 APFS_SPACE_INFO_GLOBAL_RSV)

enum apfs_raid_types {
	APFS_RAID_RAID10,
	APFS_RAID_RAID1,
	APFS_RAID_DUP,
	APFS_RAID_RAID0,
	APFS_RAID_SINGLE,
	APFS_RAID_RAID5,
	APFS_RAID_RAID6,
	APFS_RAID_RAID1C3,
	APFS_RAID_RAID1C4,
	APFS_NR_RAID_TYPES
};

#define APFS_BLOCK_GROUP_TYPE_MASK	(APFS_BLOCK_GROUP_DATA |    \
					 APFS_BLOCK_GROUP_SYSTEM |  \
					 APFS_BLOCK_GROUP_METADATA)

#define APFS_BLOCK_GROUP_PROFILE_MASK	(APFS_BLOCK_GROUP_RAID0 |   \
					 APFS_BLOCK_GROUP_RAID1 |   \
					 APFS_BLOCK_GROUP_RAID1C3 | \
					 APFS_BLOCK_GROUP_RAID1C4 | \
					 APFS_BLOCK_GROUP_RAID5 |   \
					 APFS_BLOCK_GROUP_RAID6 |   \
					 APFS_BLOCK_GROUP_DUP |     \
					 APFS_BLOCK_GROUP_RAID10)
#define APFS_BLOCK_GROUP_RAID56_MASK	(APFS_BLOCK_GROUP_RAID5 |   \
					 APFS_BLOCK_GROUP_RAID6)

#define APFS_BLOCK_GROUP_RAID1_MASK	(APFS_BLOCK_GROUP_RAID1 |   \
					 APFS_BLOCK_GROUP_RAID1C3 | \
					 APFS_BLOCK_GROUP_RAID1C4)

/*
 * We need a bit for restriper to be able to tell when chunks of type
 * SINGLE are available.  This "extended" profile format is used in
 * fs_info->avail_*_alloc_bits (in-memory) and balance item fields
 * (on-disk).  The corresponding on-disk bit in chunk.type is reserved
 * to avoid remappings between two formats in future.
 */
#define APFS_AVAIL_ALLOC_BIT_SINGLE	(1ULL << 48)

/*
 * A fake block group type that is used to communicate global block reserve
 * size to userspace via the SPACE_INFO ioctl.
 */
#define APFS_SPACE_INFO_GLOBAL_RSV	(1ULL << 49)

#define APFS_EXTENDED_PROFILE_MASK	(APFS_BLOCK_GROUP_PROFILE_MASK | \
					 APFS_AVAIL_ALLOC_BIT_SINGLE)

static inline __u64 chunk_to_extended(__u64 flags)
{
	if ((flags & APFS_BLOCK_GROUP_PROFILE_MASK) == 0)
		flags |= APFS_AVAIL_ALLOC_BIT_SINGLE;

	return flags;
}
static inline __u64 extended_to_chunk(__u64 flags)
{
	return flags & ~APFS_AVAIL_ALLOC_BIT_SINGLE;
}

struct apfs_block_group_item {
	__le64 used;
	__le64 chunk_objectid;
	__le64 flags;
} __attribute__ ((__packed__));

struct apfs_free_space_info {
	__le32 extent_count;
	__le32 flags;
} __attribute__ ((__packed__));

#define APFS_FREE_SPACE_USING_BITMAPS (1ULL << 0)

#define APFS_QGROUP_LEVEL_SHIFT		48
static inline __u16 apfs_qgroup_level(__u64 qgroupid)
{
	return (__u16)(qgroupid >> APFS_QGROUP_LEVEL_SHIFT);
}

/*
 * is subvolume quota turned on?
 */
#define APFS_QGROUP_STATUS_FLAG_ON		(1ULL << 0)
/*
 * RESCAN is set during the initialization phase
 */
#define APFS_QGROUP_STATUS_FLAG_RESCAN		(1ULL << 1)
/*
 * Some qgroup entries are known to be out of date,
 * either because the configuration has changed in a way that
 * makes a rescan necessary, or because the fs has been mounted
 * with a non-qgroup-aware version.
 * Turning qouta off and on again makes it inconsistent, too.
 */
#define APFS_QGROUP_STATUS_FLAG_INCONSISTENT	(1ULL << 2)

#define APFS_QGROUP_STATUS_VERSION        1

struct apfs_qgroup_status_item {
	__le64 version;
	/*
	 * the generation is updated during every commit. As older
	 * versions of apfs are not aware of qgroups, it will be
	 * possible to detect inconsistencies by checking the
	 * generation on mount time
	 */
	__le64 generation;

	/* flag definitions see above */
	__le64 flags;

	/*
	 * only used during scanning to record the progress
	 * of the scan. It contains a logical address
	 */
	__le64 rescan;
} __attribute__ ((__packed__));

struct apfs_qgroup_info_item {
	__le64 generation;
	__le64 rfer;
	__le64 rfer_cmpr;
	__le64 excl;
	__le64 excl_cmpr;
} __attribute__ ((__packed__));

struct apfs_qgroup_limit_item {
	/*
	 * only updated when any of the other values change
	 */
	__le64 flags;
	__le64 max_rfer;
	__le64 max_excl;
	__le64 rsv_rfer;
	__le64 rsv_excl;
} __attribute__ ((__packed__));

#endif /* _APFS_CTREE_H_ */
