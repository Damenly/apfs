/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2007 Oracle.  All rights reserved.
 */

#ifndef APFS_INODE_H
#define APFS_INODE_H

#include <linux/hash.h>
#include <linux/refcount.h>
#include "extent_map.h"
#include "extent_io.h"
#include "ordered-data.h"
#include "delayed-inode.h"
#include "apfs_trace.h"
/*
 * ordered_data_close is set by truncate when a file that used
 * to have good data has been truncated to zero.  When it is set
 * the apfs file release call will add this inode to the
 * ordered operations list so that we make sure to flush out any
 * new data the application may have written before commit.
 */
enum {
	APFS_INODE_FLUSH_ON_CLOSE,
	APFS_INODE_DUMMY,
	APFS_INODE_IN_DEFRAG,
	APFS_INODE_HAS_ASYNC_EXTENT,
	 /*
	  * Always set under the VFS' inode lock, otherwise it can cause races
	  * during fsync (we start as a fast fsync and then end up in a full
	  * fsync racing with ordered extent completion).
	  */
	APFS_INODE_NEEDS_FULL_SYNC,
	APFS_INODE_COPY_EVERYTHING,
	APFS_INODE_IN_DELALLOC_LIST,
	APFS_INODE_HAS_PROPS,
	APFS_INODE_SNAPSHOT_FLUSH,
	/*
	 * Set and used when logging an inode and it serves to signal that an
	 * inode does not have xattrs, so subsequent fsyncs can avoid searching
	 * for xattrs to log. This bit must be cleared whenever a xattr is added
	 * to an inode.
	 */
	APFS_INODE_NO_XATTRS,
	/*
	 * Set when we are in a context where we need to start a transaction and
	 * have dirty pages with the respective file range locked. This is to
	 * ensure that when reserving space for the transaction, if we are low
	 * on available space and need to flush delalloc, we will not flush
	 * delalloc for this inode, because that could result in a deadlock (on
	 * the file range, inode's io_tree).
	 */
	APFS_INODE_NO_DELALLOC_FLUSH,
};

/* in memory apfs inode */
struct apfs_inode {
	/* which subvolume this inode belongs to */
	struct apfs_root *root;

	u64 privateid;

	/*
	 * cache of compressed file extent objectid, used only when the inode
	 * is compressed
	 */
	u64 cid;

	/* key used to find this inode on disk.  This is used by the code
	 * to read in roots of subvolumes
	 */
	struct apfs_key location;

	/*
	 * Lock for counters and all fields used to determine if the inode is in
	 * the log or not (last_trans, last_sub_trans, last_log_commit,
	 * logged_trans), to access/update new_delalloc_bytes and to update the
	 * VFS' inode number of bytes used.
	 */
	spinlock_t lock;

	/* the extent_tree has caches of all the extent mappings to disk */
	struct extent_map_tree extent_tree;

	/* the io_tree does range state (DIRTY, LOCKED etc) */
	struct extent_io_tree io_tree;

	/* special utility tree used to record which mirrors have already been
	 * tried when checksums fail for a given block
	 */
	struct extent_io_tree io_failure_tree;

	/*
	 * Keep track of where the inode has extent items mapped in order to
	 * make sure the i_size adjustments are accurate
	 */
	struct extent_io_tree file_extent_tree;

	/* held while logging the inode in tree-log.c */
	struct mutex log_mutex;

	/* used to order data wrt metadata */
	struct apfs_ordered_inode_tree ordered_tree;

	/* list of all the delalloc inodes in the FS.  There are times we need
	 * to write all the delalloc pages to disk, and this list is used
	 * to walk them all.
	 */
	struct list_head delalloc_inodes;

	/* node for the red-black tree that links inodes in subvolume root */
	struct rb_node rb_node;

	unsigned long runtime_flags;

	/* Keep track of who's O_SYNC/fsyncing currently */
	atomic_t sync_writers;

	/* full 64 bit generation number, struct vfs_inode doesn't have a big
	 * enough field for this.
	 */
	u64 generation;

	/*
	 * transid of the trans_handle that last modified this inode
	 */
	u64 last_trans;

	/*
	 * transid that last logged this inode
	 */
	u64 logged_trans;

	/*
	 * log transid when this inode was last modified
	 */
	int last_sub_trans;

	/* a local copy of root's last_log_commit */
	int last_log_commit;

	/* total number of bytes pending delalloc, used by stat to calc the
	 * real block usage of the file
	 */
	u64 delalloc_bytes;

	/*
	 * Total number of bytes pending delalloc that fall within a file
	 * range that is either a hole or beyond EOF (and no prealloc extent
	 * exists in the range). This is always <= delalloc_bytes.
	 */
	u64 new_delalloc_bytes;

	/*
	 * total number of bytes pending defrag, used by stat to check whether
	 * it needs COW.
	 */
	u64 defrag_bytes;

	/*
	 * the size of the file stored in the metadata on disk.  data=ordered
	 * means the in-memory i_size might be larger than the size on disk
	 * because not all the blocks are written yet.
	 */
	u64 disk_i_size;

	/*
	 * if this is a directory then index_cnt is the counter for the index
	 * number for new files that are created
	 */
	u64 index_cnt;

	/* Cache the directory index number to speed the dir/file remove */
	u64 dir_index;

	/* the fsync log has some corner cases that mean we have to check
	 * directories to see if any unlinks have been done before
	 * the directory was logged.  See tree-log.c for all the
	 * details
	 */
	u64 last_unlink_trans;

	/*
	 * The id/generation of the last transaction where this inode was
	 * either the source or the destination of a clone/dedupe operation.
	 * Used when logging an inode to know if there are shared extents that
	 * need special care when logging checksum items, to avoid duplicate
	 * checksum items in a log (which can lead to a corruption where we end
	 * up with missing checksum ranges after log replay).
	 * Protected by the vfs inode lock.
	 */
	u64 last_reflink_trans;

	/*
	 * Number of bytes outstanding that are going to need csums.  This is
	 * used in ENOSPC accounting.
	 */
	u64 csum_bytes;

	/* flags field from the on disk inode */
	u64 flags;

	/* bsd flags field from the on disk inode */
	u32 bsd_flags;
	/*
	 * Counters to keep track of the number of extent item's we may use due
	 * to delalloc and such.  outstanding_extents is the number of extent
	 * items we think we'll end up using, and reserved_extents is the number
	 * of extent items we've reserved metadata for.
	 */
	unsigned outstanding_extents;

	struct apfs_block_rsv block_rsv;

	/*
	 * Cached values of inode properties
	 */
	unsigned prop_compress;		/* per-file compression algorithm */
	/*
	 * Force compression on the file using the defrag ioctl, could be
	 * different from prop_compress and takes precedence if set
	 */
	unsigned defrag_compress;

	struct apfs_delayed_node *delayed_node;

	/* File creation time. */
	struct timespec64 i_otime;

	/* Hook into fs_info->delayed_iputs */
	struct list_head delayed_iput;

	struct rw_semaphore i_mmap_lock;
	struct inode vfs_inode;
};

static inline u32 apfs_inode_sectorsize(const struct apfs_inode *inode)
{
	return inode->root->fs_info->sectorsize;
}

static inline struct apfs_inode *APFS_I(const struct inode *inode)
{
	return container_of(inode, struct apfs_inode, vfs_inode);
}

static inline unsigned long apfs_inode_hash(u64 objectid,
					    const struct apfs_root *root)
{
	u64 h = objectid ^ (root->root_key.objectid * GOLDEN_RATIO_PRIME *
			    (root->fs_info->index + abs(APFS_DUMMY_FS_INDEX) + 1));

#if BITS_PER_LONG == 32
	h = (h >> 32) ^ (h & 0xffffffff);
#endif

	return (unsigned long)h;
}

static inline void apfs_insert_inode_hash(struct inode *inode)
{
	unsigned long h = apfs_inode_hash(inode->i_ino, APFS_I(inode)->root);
	__insert_inode_hash(inode, h);
}

static inline u64 apfs_ino(const struct apfs_inode *inode)
{
	u64 ino = inode->location.oid;

	/*
	 * !ino: btree_inode
	 * type == APFS_ROOT_ITEM_KEY: subvol dir
	 */
	if (!ino)
		ino = inode->vfs_inode.i_ino;
	return ino;
}

static inline void apfs_i_size_write(struct apfs_inode *inode, u64 size)
{
	i_size_write(&inode->vfs_inode, size);
	inode->disk_i_size = size;
}

static inline bool apfs_is_free_space_inode(struct apfs_inode *inode)
{
	struct apfs_root *root = inode->root;

	if (root == root->fs_info->tree_root &&
	    apfs_ino(inode) != APFS_BTREE_INODE_OBJECTID)
		return true;
	if (inode->location.objectid == APFS_FREE_INO_OBJECTID)
		return true;
	return false;
}

static inline bool is_data_inode(struct inode *inode)
{
	return apfs_ino(APFS_I(inode)) != APFS_BTREE_INODE_OBJECTID;
}

static inline void apfs_mod_outstanding_extents(struct apfs_inode *inode,
						 int mod)
{
	lockdep_assert_held(&inode->lock);
	inode->outstanding_extents += mod;
	if (apfs_is_free_space_inode(inode))
		return;
	trace_apfs_inode_mod_outstanding_extents(inode->root, apfs_ino(inode),
						  mod);
}

/*
 * Called every time after doing a buffered, direct IO or memory mapped write.
 *
 * This is to ensure that if we write to a file that was previously fsynced in
 * the current transaction, then try to fsync it again in the same transaction,
 * we will know that there were changes in the file and that it needs to be
 * logged.
 */
static inline void apfs_set_inode_last_sub_trans(struct apfs_inode *inode)
{
	spin_lock(&inode->lock);
	inode->last_sub_trans = inode->root->log_transid;
	spin_unlock(&inode->lock);
}

static inline bool apfs_inode_in_log(struct apfs_inode *inode, u64 generation)
{
	bool ret = false;

	spin_lock(&inode->lock);
	if (inode->logged_trans == generation &&
	    inode->last_sub_trans <= inode->last_log_commit &&
	    inode->last_sub_trans <= inode->root->last_log_commit)
		ret = true;
	spin_unlock(&inode->lock);
	return ret;
}

struct apfs_dio_private {
	struct inode *inode;
	u64 logical_offset;
	u64 disk_bytenr;
	/* Used for bio::bi_size */
	u32 bytes;

	/*
	 * References to this structure. There is one reference per in-flight
	 * bio plus one while we're still setting up.
	 */
	refcount_t refs;

	/* dio_bio came from fs/direct-io.c */
	struct bio *dio_bio;

	/* Array of checksums */
	u8 csums[];
};

/* Array of bytes with variable length, hexadecimal format 0x1234 */
#define CSUM_FMT				"0x%*phN"
#define CSUM_FMT_VALUE(size, bytes)		size, bytes

static inline void apfs_print_data_csum_error(struct apfs_inode *inode,
		u64 logical_start, u8 *csum, u8 *csum_expected, int mirror_num)
{
	struct apfs_root *root = inode->root;
	const u32 csum_size = root->fs_info->csum_size;

	/* Output minus objectid, which is more meaningful */
	if (root->root_key.objectid >= APFS_LAST_FREE_OBJECTID)
		apfs_warn_rl(root->fs_info,
"csum failed root %lld ino %lld off %llu csum " CSUM_FMT " expected csum " CSUM_FMT " mirror %d",
			root->root_key.objectid, apfs_ino(inode),
			logical_start,
			CSUM_FMT_VALUE(csum_size, csum),
			CSUM_FMT_VALUE(csum_size, csum_expected),
			mirror_num);
	else
		apfs_warn_rl(root->fs_info,
"csum failed root %llu ino %llu off %llu csum " CSUM_FMT " expected csum " CSUM_FMT " mirror %d",
			root->root_key.objectid, apfs_ino(inode),
			logical_start,
			CSUM_FMT_VALUE(csum_size, csum),
			CSUM_FMT_VALUE(csum_size, csum_expected),
			mirror_num);
}

#endif
