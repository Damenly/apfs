/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2014 Facebook.  All rights reserved.
 */

#ifndef APFS_REF_VERIFY_H
#define APFS_REF_VERIFY_H

#ifdef CONFIG_APFS_FS_REF_VERIFY
int apfs_build_ref_tree(struct apfs_fs_info *fs_info);
void apfs_free_ref_cache(struct apfs_fs_info *fs_info);
int apfs_ref_tree_mod(struct apfs_fs_info *fs_info,
		       struct apfs_ref *generic_ref);
void apfs_free_ref_tree_range(struct apfs_fs_info *fs_info, u64 start,
			       u64 len);

static inline void apfs_init_ref_verify(struct apfs_fs_info *fs_info)
{
	spin_lock_init(&fs_info->ref_verify_lock);
	fs_info->block_tree = RB_ROOT;
}
#else
static inline int apfs_build_ref_tree(struct apfs_fs_info *fs_info)
{
	return 0;
}

static inline void apfs_free_ref_cache(struct apfs_fs_info *fs_info)
{
}

static inline int apfs_ref_tree_mod(struct apfs_fs_info *fs_info,
		       struct apfs_ref *generic_ref)
{
	return 0;
}

static inline void apfs_free_ref_tree_range(struct apfs_fs_info *fs_info,
					     u64 start, u64 len)
{
}

static inline void apfs_init_ref_verify(struct apfs_fs_info *fs_info)
{
}

#endif /* CONFIG_APFS_FS_REF_VERIFY */

#endif
