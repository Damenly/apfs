/* SPDX-License-Identifier: GPL-2.0 */

#ifndef APFS_EXPORT_H
#define APFS_EXPORT_H

#include <linux/exportfs.h>

extern const struct export_operations apfs_export_ops;

struct apfs_fid {
	u64 objectid;
	u64 root_objectid;
	u32 gen;

	u64 parent_objectid;
	u32 parent_gen;

	u64 parent_root_objectid;
} __attribute__ ((packed));

struct dentry *apfs_get_dentry(struct super_block *sb, u64 objectid,
				u64 root_objectid, u32 generation,
				int check_generation);
struct dentry *apfs_get_parent(struct dentry *child);

#endif
