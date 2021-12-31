// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2007 Red Hat.  All rights reserved.
 */

#include <linux/init.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/rwsem.h>
#include <linux/xattr.h>
#include <linux/security.h>
#include <linux/posix_acl_xattr.h>
#include <linux/iversion.h>
#include <linux/sched/mm.h>
#include "ctree.h"
#include "apfs_inode.h"
#include "transaction.h"
#include "xattr.h"
#include "disk-io.h"
#include "props.h"
#include "locking.h"

int apfs_getxattr(struct inode *inode, const char *name,
		  void *buffer, size_t size)
{
	struct apfs_xattr_item *xi;
	struct apfs_root *root = APFS_I(inode)->root;
	struct apfs_path *path;
	struct extent_buffer *leaf;
	int ret = 0;
	unsigned long data_ptr;

	path = apfs_alloc_path();
	if (!path)
		return -ENOMEM;

	/* lookup the xattr by name */
	xi = apfs_lookup_xattr_item(NULL, root, path, apfs_ino(APFS_I(inode)),
				    name, 0);
	if (xi == NULL) {
		ret = -ENODATA;
		goto out;
	} else if (IS_ERR(xi)) {
		ret = PTR_ERR(xi);
		goto out;
	}

	leaf = path->nodes[0];
	/* if size is 0, that means we want the size of the attr */
	if (!size) {
		ret = apfs_xattr_item_len(leaf, xi);
		goto out;
	}

	if (!(apfs_xattr_item_flags(leaf, xi) & APFS_XATTR_DATA_EMBEDDED)) {
		ret = -ENODATA;
		apfs_warn(root->fs_info,
			  "data mebedded of ino %llu xattr %s (TODO)\n",
			  apfs_ino(APFS_I(inode)), name);
		goto out;

	}
	/* now get the data out of our dir_item */
	if (apfs_xattr_item_len(leaf, xi) > size) {
		ret = -ERANGE;
		goto out;
	}

	data_ptr = (unsigned long)((char *)(xi + 1));

	read_extent_buffer(leaf, buffer, data_ptr,
			   apfs_xattr_item_len(leaf, xi));

	ret = apfs_xattr_item_len(leaf, xi);
out:
	apfs_free_path(path);
	return ret;
}

int apfs_setxattr(struct apfs_trans_handle *trans, struct inode *inode,
		   const char *name, const void *value, size_t size, int flags)
{
	struct apfs_dir_item *di = NULL;
	struct apfs_root *root = APFS_I(inode)->root;
	struct apfs_fs_info *fs_info = root->fs_info;
	struct apfs_path *path;
	size_t name_len = strlen(name);
	int ret = 0;

	ASSERT(trans);

	if (name_len + size > APFS_MAX_XATTR_SIZE(root->fs_info))
		return -ENOSPC;

	path = apfs_alloc_path();
	if (!path)
		return -ENOMEM;
	path->skip_release_on_error = 1;

	if (!value) {
		di = apfs_lookup_xattr(trans, root, path,
				apfs_ino(APFS_I(inode)), name, name_len, -1);
		if (!di && (flags & XATTR_REPLACE))
			ret = -ENODATA;
		else if (IS_ERR(di))
			ret = PTR_ERR(di);
		else if (di)
			ret = apfs_delete_one_dir_name(trans, root, path, di);
		goto out;
	}

	/*
	 * For a replace we can't just do the insert blindly.
	 * Do a lookup first (read-only apfs_search_slot), and return if xattr
	 * doesn't exist. If it exists, fall down below to the insert/replace
	 * path - we can't race with a concurrent xattr delete, because the VFS
	 * locks the inode's i_mutex before calling setxattr or removexattr.
	 */
	if (flags & XATTR_REPLACE) {
		ASSERT(inode_is_locked(inode));
		di = apfs_lookup_xattr(NULL, root, path,
				apfs_ino(APFS_I(inode)), name, name_len, 0);
		if (!di)
			ret = -ENODATA;
		else if (IS_ERR(di))
			ret = PTR_ERR(di);
		if (ret)
			goto out;
		apfs_release_path(path);
		di = NULL;
	}

	ret = apfs_insert_xattr_item(trans, root, path, apfs_ino(APFS_I(inode)),
				      name, name_len, value, size);
	if (ret == -EOVERFLOW) {
		/*
		 * We have an existing item in a leaf, split_leaf couldn't
		 * expand it. That item might have or not a dir_item that
		 * matches our target xattr, so lets check.
		 */
		ret = 0;
		apfs_assert_tree_locked(path->nodes[0]);
		di = apfs_match_dir_item_name(fs_info, path, name, name_len);
		if (!di && !(flags & XATTR_REPLACE)) {
			ret = -ENOSPC;
			goto out;
		}
	} else if (ret == -EEXIST) {
		ret = 0;
		di = apfs_match_dir_item_name(fs_info, path, name, name_len);
		ASSERT(di); /* logic error */
	} else if (ret) {
		goto out;
	}

	if (di && (flags & XATTR_CREATE)) {
		ret = -EEXIST;
		goto out;
	}

	if (di) {
		/*
		 * We're doing a replace, and it must be atomic, that is, at
		 * any point in time we have either the old or the new xattr
		 * value in the tree. We don't want readers (getxattr and
		 * listxattrs) to miss a value, this is specially important
		 * for ACLs.
		 */
		const int slot = path->slots[0];
		struct extent_buffer *leaf = path->nodes[0];
		const u16 old_data_len = apfs_dir_data_len(leaf, di);
		const u32 item_size = apfs_item_size_nr(leaf, slot);
		const u32 data_size = sizeof(*di) + name_len + size;
		struct apfs_item *item;
		unsigned long data_ptr;
		char *ptr;

		if (size > old_data_len) {
			if (apfs_leaf_free_space(leaf) <
			    (size - old_data_len)) {
				ret = -ENOSPC;
				goto out;
			}
		}

		if (old_data_len + name_len + sizeof(*di) == item_size) {
			/* No other xattrs packed in the same leaf item. */
			if (size > old_data_len)
				apfs_extend_item(path, size - old_data_len);
			else if (size < old_data_len)
				apfs_truncate_item(path, data_size, 1);
		} else {
			/* There are other xattrs packed in the same item. */
			ret = apfs_delete_one_dir_name(trans, root, path, di);
			if (ret)
				goto out;
			apfs_extend_item(path, data_size);
		}

		item = apfs_item_nr(slot);
		ptr = apfs_item_ptr(leaf, slot, char);
		ptr += apfs_item_size(leaf, item) - data_size;
		di = (struct apfs_dir_item *)ptr;
		apfs_set_dir_data_len(leaf, di, size);
		data_ptr = ((unsigned long)(di + 1)) + name_len;
		write_extent_buffer(leaf, value, data_ptr, size);
		apfs_mark_buffer_dirty(leaf);
	} else {
		/*
		 * Insert, and we had space for the xattr, so path->slots[0] is
		 * where our xattr dir_item is and apfs_insert_xattr_item()
		 * filled it.
		 */
	}
out:
	apfs_free_path(path);
	if (!ret) {
		set_bit(APFS_INODE_COPY_EVERYTHING,
			&APFS_I(inode)->runtime_flags);
		clear_bit(APFS_INODE_NO_XATTRS, &APFS_I(inode)->runtime_flags);
	}
	return ret;
}

/*
 * @value: "" makes the attribute to empty, NULL removes it
 */
int apfs_setxattr_trans(struct inode *inode, const char *name,
			 const void *value, size_t size, int flags)
{
	struct apfs_root *root = APFS_I(inode)->root;
	struct apfs_trans_handle *trans;
	const bool start_trans = (current->journal_info == NULL);
	int ret;

	if (start_trans) {
		/*
		 * 1 unit for inserting/updating/deleting the xattr
		 * 1 unit for the inode item update
		 */
		trans = apfs_start_transaction(root, 2);
		if (IS_ERR(trans))
			return PTR_ERR(trans);
	} else {
		/*
		 * This can happen when smack is enabled and a directory is being
		 * created. It happens through d_instantiate_new(), which calls
		 * smack_d_instantiate(), which in turn calls __vfs_setxattr() to
		 * set the transmute xattr (XATTR_NAME_SMACKTRANSMUTE) on the
		 * inode. We have already reserved space for the xattr and inode
		 * update at apfs_mkdir(), so just use the transaction handle.
		 * We don't join or start a transaction, as that will reset the
		 * block_rsv of the handle and trigger a warning for the start
		 * case.
		 */
		ASSERT(strncmp(name, XATTR_SECURITY_PREFIX,
			       XATTR_SECURITY_PREFIX_LEN) == 0);
		trans = current->journal_info;
	}

	ret = apfs_setxattr(trans, inode, name, value, size, flags);
	if (ret)
		goto out;

	inode_inc_iversion(inode);
	inode->i_ctime = current_time(inode);
	ret = apfs_update_inode(trans, root, APFS_I(inode));
	BUG_ON(ret);
out:
	if (start_trans)
		apfs_end_transaction(trans);
	return ret;
}

ssize_t apfs_listxattr(struct dentry *dentry, char *buffer, size_t size)
{
	struct apfs_key key = {};
	struct inode *inode = d_inode(dentry);
	struct apfs_root *root = APFS_I(inode)->root;
	struct apfs_path *path;
	int ret = 0;
	size_t total_size = 0, size_left = size;
	u64 ino = apfs_ino(APFS_I(inode));

	/*
	 * ok we want all objects associated with this id.
	 * NOTE: we set key.offset = 0; because we want to start with the
	 * first xattr that we find and walk forward
	 */
	key.oid = apfs_ino(APFS_I(inode));
	key.type = APFS_TYPE_XATTR;
	key.name = NULL;

	path = apfs_alloc_path();
	if (!path)
		return -ENOMEM;
	path->reada = READA_FORWARD;

	/* search for our xattrs */
	ret = apfs_search_slot(NULL, root, &key, path, 0, 0);
	if (ret < 0)
		goto err;

	while (1) {
		struct extent_buffer *leaf;
		int slot;

		leaf = path->nodes[0];
		slot = path->slots[0];

		/* this is where we start walking through the path */
		if (slot >= apfs_header_nritems(leaf)) {
			/*
			 * if we've reached the last slot in this leaf we need
			 * to go to the next leaf and reset everything
			 */
			ret = apfs_next_leaf(root, path);
			if (ret < 0)
				goto err;
			else if (ret > 0)
				break;
			continue;
		}

		memset(&key, 0, sizeof(key));
		apfs_item_key_to_cpu(leaf, &key, slot);

		/* check to make sure this item is what we want */
		if (key.oid != ino)
			break;
		if (key.type > APFS_TYPE_XATTR)
			break;
		if (key.type < APFS_TYPE_XATTR)
			goto next_item;

		if (!strncasecmp(key.name, APFS_XATTR_APPLE_PREFIX,
				 APFS_XATTR_APPLE_PREFIX_LEN))
			goto next_item;

		total_size += key.namelen;

		/*
		 * We are just looking for how big our buffer needs to
		 * be.
		 */
		if (!size)
			goto next_item;

		if (!buffer || (key.namelen) > size_left) {
			ret = -ERANGE;
			goto err;
		}

		strncpy(buffer, key.name, key.namelen);

		size_left -= key.namelen;
		buffer += key.namelen;

next_item:
		path->slots[0]++;
	}
	ret = total_size;

err:
	apfs_free_path(path);

	return ret;
}

static int apfs_xattr_handler_get(const struct xattr_handler *handler,
				   struct dentry *unused, struct inode *inode,
				   const char *name, void *buffer, size_t size)
{
	name = xattr_full_name(handler, name);
	return apfs_getxattr(inode, name, buffer, size);
}

static int apfs_xattr_handler_set(const struct xattr_handler *handler,
				   struct user_namespace *mnt_userns,
				   struct dentry *unused, struct inode *inode,
				   const char *name, const void *buffer,
				   size_t size, int flags)
{
	name = xattr_full_name(handler, name);
	return apfs_setxattr_trans(inode, name, buffer, size, flags);
}

static int apfs_xattr_handler_set_prop(const struct xattr_handler *handler,
					struct user_namespace *mnt_userns,
					struct dentry *unused, struct inode *inode,
					const char *name, const void *value,
					size_t size, int flags)
{
	int ret;
	struct apfs_trans_handle *trans;
	struct apfs_root *root = APFS_I(inode)->root;

	name = xattr_full_name(handler, name);
	ret = apfs_validate_prop(name, 0, value, size);
	if (ret)
		return ret;

	trans = apfs_start_transaction(root, 2);
	if (IS_ERR(trans))
		return PTR_ERR(trans);

	ret = apfs_set_prop(trans, inode, name, value, size, flags);
	if (!ret) {
		inode_inc_iversion(inode);
		inode->i_ctime = current_time(inode);
		ret = apfs_update_inode(trans, root, APFS_I(inode));
		BUG_ON(ret);
	}

	apfs_end_transaction(trans);

	return ret;
}

static const struct xattr_handler apfs_security_xattr_handler = {
	.prefix = XATTR_SECURITY_PREFIX,
	.get = apfs_xattr_handler_get,
	.set = apfs_xattr_handler_set,
};

static const struct xattr_handler apfs_trusted_xattr_handler = {
	.prefix = XATTR_TRUSTED_PREFIX,
	.get = apfs_xattr_handler_get,
	.set = apfs_xattr_handler_set,
};

static const struct xattr_handler apfs_user_xattr_handler = {
	.prefix = XATTR_USER_PREFIX,
	.get = apfs_xattr_handler_get,
	.set = apfs_xattr_handler_set,
};

static const struct xattr_handler apfs_apfs_xattr_handler = {
	.prefix = XATTR_APFS_PREFIX,
	.get = apfs_xattr_handler_get,
	.set = apfs_xattr_handler_set_prop,
};

const struct xattr_handler *apfs_xattr_handlers[] = {
	&apfs_security_xattr_handler,
#ifdef CONFIG_APFS_FS_POSIX_ACL
	&posix_acl_access_xattr_handler,
	&posix_acl_default_xattr_handler,
#endif
	&apfs_trusted_xattr_handler,
	&apfs_user_xattr_handler,
	&apfs_apfs_xattr_handler,
	NULL,
};

static int apfs_initxattrs(struct inode *inode,
			    const struct xattr *xattr_array, void *fs_private)
{
	struct apfs_trans_handle *trans = fs_private;
	const struct xattr *xattr;
	unsigned int nofs_flag;
	char *name;
	int err = 0;

	/*
	 * We're holding a transaction handle, so use a NOFS memory allocation
	 * context to avoid deadlock if reclaim happens.
	 */
	nofs_flag = memalloc_nofs_save();
	for (xattr = xattr_array; xattr->name != NULL; xattr++) {
		name = kmalloc(XATTR_SECURITY_PREFIX_LEN +
			       strlen(xattr->name) + 1, GFP_KERNEL);
		if (!name) {
			err = -ENOMEM;
			break;
		}
		strcpy(name, XATTR_SECURITY_PREFIX);
		strcpy(name + XATTR_SECURITY_PREFIX_LEN, xattr->name);
		err = apfs_setxattr(trans, inode, name, xattr->value,
				     xattr->value_len, 0);
		kfree(name);
		if (err < 0)
			break;
	}
	memalloc_nofs_restore(nofs_flag);
	return err;
}

int apfs_xattr_security_init(struct apfs_trans_handle *trans,
			      struct inode *inode, struct inode *dir,
			      const struct qstr *qstr)
{
	return security_inode_init_security(inode, dir, qstr,
					    &apfs_initxattrs, trans);
}
