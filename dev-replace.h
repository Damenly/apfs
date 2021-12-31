/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) STRATO AG 2012.  All rights reserved.
 */

#ifndef APFS_DEV_REPLACE_H
#define APFS_DEV_REPLACE_H

struct apfs_ioctl_dev_replace_args;

int apfs_init_dev_replace(struct apfs_fs_info *fs_info);
int apfs_run_dev_replace(struct apfs_trans_handle *trans);
int apfs_dev_replace_by_ioctl(struct apfs_fs_info *fs_info,
			    struct apfs_ioctl_dev_replace_args *args);
void apfs_dev_replace_status(struct apfs_fs_info *fs_info,
			      struct apfs_ioctl_dev_replace_args *args);
int apfs_dev_replace_cancel(struct apfs_fs_info *fs_info);
void apfs_dev_replace_suspend_for_unmount(struct apfs_fs_info *fs_info);
int apfs_resume_dev_replace_async(struct apfs_fs_info *fs_info);
int __pure apfs_dev_replace_is_ongoing(struct apfs_dev_replace *dev_replace);
bool apfs_finish_block_group_to_copy(struct apfs_device *srcdev,
				      struct apfs_block_group *cache,
				      u64 physical);

#endif
