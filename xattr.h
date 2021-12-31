/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2007 Red Hat.  All rights reserved.
 */

#ifndef APFS_XATTR_H
#define APFS_XATTR_H

#include <linux/xattr.h>

extern const struct xattr_handler *apfs_xattr_handlers[];

int apfs_getxattr(struct inode *inode, const char *name,
		void *buffer, size_t size);
int apfs_setxattr(struct apfs_trans_handle *trans, struct inode *inode,
		   const char *name, const void *value, size_t size, int flags);
int apfs_setxattr_trans(struct inode *inode, const char *name,
			 const void *value, size_t size, int flags);
ssize_t apfs_listxattr(struct dentry *dentry, char *buffer, size_t size);

int apfs_xattr_security_init(struct apfs_trans_handle *trans,
				     struct inode *inode, struct inode *dir,
				     const struct qstr *qstr);
struct apfs_xattr_item *apfs_lookup_xattr_item(struct apfs_trans_handle *trans,
					       struct apfs_root *root,
					       struct apfs_path *path, u64 dir,
					       const char *name, int mod);
#endif
