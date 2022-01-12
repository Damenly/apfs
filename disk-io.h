/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2007 Oracle.  All rights reserved.
 */

#ifndef APFS_DISK_IO_H
#define APFS_DISK_IO_H

#define APFS_NX_SUPER_INFO_OFFSET 0
#define APFS_SUPER_INFO_OFFSET SZ_64K
#define APFS_SUPER_INFO_SIZE 4096

#define APFS_SUPER_MIRROR_MAX	 3
#define APFS_SUPER_MIRROR_SHIFT 12

/*
 * Fixed blocksize for all devices, applies to specific ways of reading
 * metadata like superblock. Must meet the set_blocksize requirements.
 *
 * Do not change.
 */
#define APFS_BDEV_BLOCKSIZE	(4096)

enum apfs_wq_endio_type {
	APFS_WQ_ENDIO_DATA,
	APFS_WQ_ENDIO_METADATA,
	APFS_WQ_ENDIO_FREE_SPACE,
	APFS_WQ_ENDIO_RAID56,
};

static inline u64 apfs_nx_offset(void)
{
	return 0;
}

static inline u64 apfs_sb_offset(int mirror)
{
	u64 start = SZ_16K;
	if (mirror)
		return start << (APFS_SUPER_MIRROR_SHIFT * mirror);
	return APFS_SUPER_INFO_OFFSET;
}

struct apfs_device;
struct apfs_fs_devices;

void apfs_check_leaked_roots(struct apfs_fs_info *fs_info);
void apfs_init_fs_info(struct apfs_fs_info *fs_info);
void apfs_init_nx_info(struct apfs_nx_info *nx_info);
int apfs_verify_level_key(struct extent_buffer *eb, int level,
			   struct apfs_key *first_key, u64 parent_transid);
struct extent_buffer *read_tree_block(struct apfs_fs_info *fs_info, u64 bytenr,
				      u64 owner_root, u64 parent_transid,
				      int level, struct apfs_key *first_key);
struct extent_buffer *apfs_find_create_tree_block(
						struct apfs_fs_info *fs_info,
						u64 bytenr, u64 owner_root,
						int level);
void apfs_clean_tree_block(struct extent_buffer *buf);
void apfs_clear_oneshot_options(struct apfs_fs_info *fs_info);
int apfs_start_pre_rw_mount(struct apfs_fs_info *fs_info);
int __cold open_ctree(struct super_block *sb, struct apfs_device *device,
		      char *options);
void __cold close_ctree(struct apfs_fs_info *fs_info);
int write_all_supers(struct apfs_fs_info *fs_info, int max_mirrors);
struct apfs_super_block *apfs_read_dev_super(struct block_device *bdev);
struct apfs_super_block *apfs_read_dev_one_super(struct block_device *bdev,
						   int copy_num);
int apfs_commit_super(struct apfs_fs_info *fs_info);
struct apfs_root *apfs_read_tree_root(struct apfs_root *tree_root,
					struct apfs_key *key);
int apfs_insert_fs_root(struct apfs_fs_info *fs_info,
			 struct apfs_root *root);
void apfs_free_fs_roots(struct apfs_fs_info *fs_info);

struct apfs_root *apfs_get_fs_root(struct apfs_fs_info *fs_info,
				     u64 objectid, bool check_ref);
struct apfs_root *apfs_get_new_fs_root(struct apfs_fs_info *fs_info,
					 u64 objectid, dev_t anon_dev);
struct apfs_root *apfs_get_fs_root_commit_root(struct apfs_fs_info *fs_info,
						 struct apfs_path *path,
						 u64 objectid);

void apfs_destroy_fs_info(struct apfs_fs_info *fs_info);
void apfs_free_fs_info(struct apfs_fs_info *fs_info);
void apfs_free_nx_info(struct apfs_nx_info *nx_info);
void apfs_get_nx_info(struct apfs_nx_info *nx_info);
void apfs_put_nx_info(struct apfs_nx_info *nx_info);
int apfs_cleanup_fs_roots(struct apfs_fs_info *fs_info);
void apfs_btree_balance_dirty(struct apfs_fs_info *fs_info);
void apfs_btree_balance_dirty_nodelay(struct apfs_fs_info *fs_info);
void apfs_drop_and_free_fs_root(struct apfs_fs_info *fs_info,
				 struct apfs_root *root);
int apfs_validate_metadata_buffer(struct apfs_io_bio *io_bio,
				   struct page *page, u64 start, u64 end,
				   int mirror);
blk_status_t apfs_submit_metadata_bio(struct inode *inode, struct bio *bio,
				       int mirror_num, unsigned long bio_flags);
#ifdef CONFIG_APFS_FS_RUN_SANITY_TESTS
struct apfs_root *apfs_alloc_dummy_root(struct apfs_fs_info *fs_info);
#endif

/*
 * This function is used to grab the root, and avoid it is freed when we
 * access it. But it doesn't ensure that the tree is not dropped.
 *
 * If you want to ensure the whole tree is safe, you should use
 * 	fs_info->subvol_srcu
 */
static inline struct apfs_root *apfs_grab_root(struct apfs_root *root)
{
	if (!root)
		return NULL;
	if (refcount_inc_not_zero(&root->refs))
		return root;
	return NULL;
}

void apfs_put_root(struct apfs_root *root);
void apfs_mark_buffer_dirty(struct extent_buffer *buf);
int apfs_buffer_uptodate(struct extent_buffer *buf, u64 parent_transid,
			  int atomic);
int apfs_read_buffer(struct extent_buffer *buf, u64 parent_transid, int level,
		      struct apfs_key *first_key);
blk_status_t apfs_bio_wq_end_io(struct apfs_fs_info *info, struct bio *bio,
			enum apfs_wq_endio_type metadata);
blk_status_t apfs_wq_submit_bio(struct inode *inode, struct bio *bio,
				 int mirror_num, unsigned long bio_flags,
				 u64 dio_file_offset,
				 extent_submit_bio_start_t *submit_bio_start);
blk_status_t apfs_submit_bio_done(void *private_data, struct bio *bio,
			  int mirror_num);
int apfs_alloc_log_tree_node(struct apfs_trans_handle *trans,
			      struct apfs_root *root);
int apfs_init_log_root_tree(struct apfs_trans_handle *trans,
			     struct apfs_fs_info *fs_info);
int apfs_add_log_tree(struct apfs_trans_handle *trans,
		       struct apfs_root *root);
void apfs_cleanup_dirty_bgs(struct apfs_transaction *trans,
			     struct apfs_fs_info *fs_info);
void apfs_cleanup_one_transaction(struct apfs_transaction *trans,
				  struct apfs_fs_info *fs_info);
struct apfs_root *apfs_create_tree(struct apfs_trans_handle *trans,
				     u64 objectid);
int btree_lock_page_hook(struct page *page, void *data,
				void (*flush_fn)(void *));
int apfs_get_num_tolerated_disk_barrier_failures(u64 flags);
int apfs_get_free_objectid(struct apfs_root *root, u64 *objectid);
int apfs_init_root_free_objectid(struct apfs_root *root);
int __init apfs_end_io_wq_init(void);
void __cold apfs_end_io_wq_exit(void);

#ifdef CONFIG_DEBUG_LOCK_ALLOC
void apfs_set_buffer_lockdep_class(u64 objectid,
			            struct extent_buffer *eb, int level);
#else
static inline void apfs_set_buffer_lockdep_class(u64 objectid,
					struct extent_buffer *eb, int level)
{
}
#endif


int apfs_find_omap_paddr(struct apfs_root *root, u64 oid, u64 xid, u64 *paddr);
u64 apfs_node_blockptr(const struct extent_buffer *eb, int nr);
int apfs_read_checkpoint_map(struct apfs_device *device, u64 bytenr,
			     struct apfs_checkpoint_map_phys *cmp);
struct apfs_vol_superblock *
apfs_read_dev_volume_super(struct apfs_fs_info *fs_info, u64 bytenr, u64 size);
int apfs_find_ephemeral_paddr(struct apfs_nx_info *info, u64 oid, u64 *paddr_res);
#endif
