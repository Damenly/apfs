/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2014 Filipe David Borba Manana <fdmanana@gmail.com>
 */

#ifndef APFS_PROPS_H
#define APFS_PROPS_H

#include "ctree.h"

#define APFS_XATTR_APPLE_PREFIX "com.apple."
#define APFS_XATTR_APPLE_PREFIX_LEN (sizeof(APFS_XATTR_APPLE_PREFIX) - 1)

void __init apfs_props_init(void);

int apfs_set_prop(struct apfs_trans_handle *trans, struct inode *inode,
		   const char *name, const char *value, size_t value_len,
		   int flags);
int apfs_validate_prop(const char *name, u16 flags, const char *value,
		       size_t value_len);

int apfs_load_inode_props(struct inode *inode, struct apfs_path *path);

int apfs_inode_inherit_props(struct apfs_trans_handle *trans,
			      struct inode *inode,
			      struct inode *dir);

int apfs_subvol_inherit_props(struct apfs_trans_handle *trans,
			       struct apfs_root *root,
			       struct apfs_root *parent_root);

#endif
