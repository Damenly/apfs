/* SPDX-License-Identifier: GPL-2.0 */

#ifndef APFS_BLOCK_RSV_H
#define APFS_BLOCK_RSV_H

struct apfs_trans_handle;
enum apfs_reserve_flush_enum;

/*
 * Types of block reserves
 */
enum {
	APFS_BLOCK_RSV_GLOBAL,
	APFS_BLOCK_RSV_DELALLOC,
	APFS_BLOCK_RSV_TRANS,
	APFS_BLOCK_RSV_CHUNK,
	APFS_BLOCK_RSV_DELOPS,
	APFS_BLOCK_RSV_DELREFS,
	APFS_BLOCK_RSV_EMPTY,
	APFS_BLOCK_RSV_TEMP,
};

struct apfs_block_rsv {
	u64 size;
	u64 reserved;
	struct apfs_space_info *space_info;
	spinlock_t lock;
	unsigned short full;
	unsigned short type;
	unsigned short failfast;

	/*
	 * Qgroup equivalent for @size @reserved
	 *
	 * Unlike normal @size/@reserved for inode rsv, qgroup doesn't care
	 * about things like csum size nor how many tree blocks it will need to
	 * reserve.
	 *
	 * Qgroup cares more about net change of the extent usage.
	 *
	 * So for one newly inserted file extent, in worst case it will cause
	 * leaf split and level increase, nodesize for each file extent is
	 * already too much.
	 *
	 * In short, qgroup_size/reserved is the upper limit of possible needed
	 * qgroup metadata reservation.
	 */
	u64 qgroup_rsv_size;
	u64 qgroup_rsv_reserved;
};

void apfs_init_block_rsv(struct apfs_block_rsv *rsv, unsigned short type);
struct apfs_block_rsv *apfs_alloc_block_rsv(struct apfs_fs_info *fs_info,
					      unsigned short type);
void apfs_init_metadata_block_rsv(struct apfs_fs_info *fs_info,
				   struct apfs_block_rsv *rsv,
				   unsigned short type);
void apfs_free_block_rsv(struct apfs_fs_info *fs_info,
			  struct apfs_block_rsv *rsv);
int apfs_block_rsv_add(struct apfs_root *root,
			struct apfs_block_rsv *block_rsv, u64 num_bytes,
			enum apfs_reserve_flush_enum flush);
int apfs_block_rsv_check(struct apfs_block_rsv *block_rsv, int min_factor);
int apfs_block_rsv_refill(struct apfs_root *root,
			   struct apfs_block_rsv *block_rsv, u64 min_reserved,
			   enum apfs_reserve_flush_enum flush);
int apfs_block_rsv_migrate(struct apfs_block_rsv *src_rsv,
			    struct apfs_block_rsv *dst_rsv, u64 num_bytes,
			    bool update_size);
int apfs_block_rsv_use_bytes(struct apfs_block_rsv *block_rsv, u64 num_bytes);
int apfs_cond_migrate_bytes(struct apfs_fs_info *fs_info,
			     struct apfs_block_rsv *dest, u64 num_bytes,
			     int min_factor);
void apfs_block_rsv_add_bytes(struct apfs_block_rsv *block_rsv,
			       u64 num_bytes, bool update_size);
u64 apfs_block_rsv_release(struct apfs_fs_info *fs_info,
			      struct apfs_block_rsv *block_rsv,
			      u64 num_bytes, u64 *qgroup_to_release);
void apfs_update_global_block_rsv(struct apfs_fs_info *fs_info);
void apfs_init_global_block_rsv(struct apfs_fs_info *fs_info);
void apfs_release_global_block_rsv(struct apfs_fs_info *fs_info);
struct apfs_block_rsv *apfs_use_block_rsv(struct apfs_trans_handle *trans,
					    struct apfs_root *root,
					    u32 blocksize);
static inline void apfs_unuse_block_rsv(struct apfs_fs_info *fs_info,
					 struct apfs_block_rsv *block_rsv,
					 u32 blocksize)
{
	apfs_block_rsv_add_bytes(block_rsv, blocksize, false);
	apfs_block_rsv_release(fs_info, block_rsv, 0, NULL);
}

#endif /* APFS_BLOCK_RSV_H */
