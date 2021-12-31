/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2013 Fusion IO.  All rights reserved.
 */

#ifndef APFS_TESTS_H
#define APFS_TESTS_H

#ifdef CONFIG_APFS_FS_RUN_SANITY_TESTS
int apfs_run_sanity_tests(void);

#define test_msg(fmt, ...) pr_info("APFS: selftest: " fmt "\n", ##__VA_ARGS__)
#define test_err(fmt, ...) pr_err("APFS: selftest: %s:%d " fmt "\n",	\
		__FILE__, __LINE__, ##__VA_ARGS__)

#define test_std_err(index)	test_err("%s", test_error[index])

enum {
	TEST_ALLOC_FS_INFO,
	TEST_ALLOC_ROOT,
	TEST_ALLOC_EXTENT_BUFFER,
	TEST_ALLOC_PATH,
	TEST_ALLOC_INODE,
	TEST_ALLOC_BLOCK_GROUP,
	TEST_ALLOC_EXTENT_MAP,
};

extern const char *test_error[];

struct apfs_root;
struct apfs_trans_handle;

int apfs_test_extent_buffer_operations(u32 sectorsize, u32 nodesize);
int apfs_test_free_space_cache(u32 sectorsize, u32 nodesize);
int apfs_test_extent_io(u32 sectorsize, u32 nodesize);
int apfs_test_inodes(u32 sectorsize, u32 nodesize);
int apfs_test_qgroups(u32 sectorsize, u32 nodesize);
int apfs_test_free_space_tree(u32 sectorsize, u32 nodesize);
int apfs_test_extent_map(void);
struct inode *apfs_new_test_inode(void);
struct apfs_fs_info *apfs_alloc_dummy_fs_info(u32 nodesize, u32 sectorsize);
void apfs_free_dummy_fs_info(struct apfs_fs_info *fs_info);
void apfs_free_dummy_root(struct apfs_root *root);
struct apfs_block_group *
apfs_alloc_dummy_block_group(struct apfs_fs_info *fs_info, unsigned long length);
void apfs_free_dummy_block_group(struct apfs_block_group *cache);
void apfs_init_dummy_trans(struct apfs_trans_handle *trans,
			    struct apfs_fs_info *fs_info);
struct apfs_device *apfs_alloc_dummy_device(struct apfs_fs_info *fs_info);
#else
static inline int apfs_run_sanity_tests(void)
{
	return 0;
}
#endif

#endif
