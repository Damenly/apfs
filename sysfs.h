/* SPDX-License-Identifier: GPL-2.0 */

#ifndef APFS_SYSFS_H
#define APFS_SYSFS_H

#include <linux/kobject.h>

struct apfs_qgroup;

enum apfs_feature_set {
	FEAT_COMPAT,
	FEAT_COMPAT_RO,
	FEAT_INCOMPAT,
	FEAT_MAX
};

char *apfs_printable_features(enum apfs_feature_set set, u64 flags);
const char *apfs_feature_set_name(enum apfs_feature_set set);
int apfs_sysfs_add_device(struct apfs_device *device);
void apfs_sysfs_remove_device(struct apfs_device *device);
int apfs_sysfs_add_fsid(struct apfs_fs_devices *fs_devs);
void apfs_sysfs_remove_fsid(struct apfs_fs_devices *fs_devs);
void apfs_sysfs_update_sprout_fsid(struct apfs_fs_devices *fs_devices);
void apfs_sysfs_feature_update(struct apfs_fs_info *fs_info,
		u64 bit, enum apfs_feature_set set);
void apfs_kobject_uevent(struct block_device *bdev, enum kobject_action action);

int __init apfs_init_sysfs(void);
void __cold apfs_exit_sysfs(void);
int apfs_sysfs_add_mounted(struct apfs_fs_info *fs_info);
void apfs_sysfs_remove_mounted(struct apfs_fs_info *fs_info);
void apfs_sysfs_add_block_group_type(struct apfs_block_group *cache);
int apfs_sysfs_add_space_info_type(struct apfs_fs_info *fs_info,
				    struct apfs_space_info *space_info);
void apfs_sysfs_remove_space_info(struct apfs_space_info *space_info);
void apfs_sysfs_update_devid(struct apfs_device *device);

int apfs_sysfs_add_one_qgroup(struct apfs_fs_info *fs_info,
				struct apfs_qgroup *qgroup);
void apfs_sysfs_del_qgroups(struct apfs_fs_info *fs_info);
int apfs_sysfs_add_qgroups(struct apfs_fs_info *fs_info);
void apfs_sysfs_del_one_qgroup(struct apfs_fs_info *fs_info,
				struct apfs_qgroup *qgroup);

#endif
