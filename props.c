// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2014 Filipe David Borba Manana <fdmanana@gmail.com>
 */

#include <linux/hashtable.h>
#include "props.h"
#include "apfs_inode.h"
#include "transaction.h"
#include "ctree.h"
#include "xattr.h"
#include "compression.h"

#define APFS_PROP_HANDLERS_HT_BITS 8
static DEFINE_HASHTABLE(prop_handlers_ht, APFS_PROP_HANDLERS_HT_BITS);

struct prop_handler {
	struct hlist_node node;
	const char *xattr_name;
	int (*validate)(const void *value, size_t len, u16 flags);
	int (*apply)(struct inode *inode, u16 flags, const void *value,
		     size_t len);
	const char *(*extract)(struct inode *inode);
	int inheritable;
};

static const struct hlist_head *find_prop_handlers_by_hash(const u64 hash)
{
	struct hlist_head *h;

	h = &prop_handlers_ht[hash_min(hash, APFS_PROP_HANDLERS_HT_BITS)];
	if (hlist_empty(h))
		return NULL;

	return h;
}

static const struct prop_handler *
find_prop_handler(const char *name,
		  const struct hlist_head *handlers)
{
	struct prop_handler *h;

	if (!handlers) {
		u64 hash = apfs_name_hash(name, strlen(name), true);

		handlers = find_prop_handlers_by_hash(hash);
		if (!handlers)
			return NULL;
	}

	hlist_for_each_entry(h, handlers, node)
		if (!strcmp(h->xattr_name, name))
			return h;

	return NULL;
}

int apfs_validate_prop(const char *name, u16 flags, const char *value,
		       size_t value_len)

{
	const struct prop_handler *handler;

	if (strlen(name) <= XATTR_APFS_PREFIX_LEN)
		return -EINVAL;

	handler = find_prop_handler(name, NULL);
	if (!handler)
		return -EINVAL;

	if (value_len == 0)
		return 0;

	return handler->validate(value, value_len, flags);
}

int apfs_set_prop(struct apfs_trans_handle *trans, struct inode *inode,
		   const char *name, const char *value, size_t value_len,
		   int flags)
{
	const struct prop_handler *handler;
	int ret;

	handler = find_prop_handler(name, NULL);
	if (!handler)
		return -EINVAL;

	if (value_len == 0) {
		ret = apfs_setxattr(trans, inode, handler->xattr_name,
				     NULL, 0, flags);
		if (ret)
			return ret;

		ret = handler->apply(inode, 0, NULL, 0);
		ASSERT(ret == 0);

		return ret;
	}

	ret = apfs_setxattr(trans, inode, handler->xattr_name, value,
			     value_len, flags);
	if (ret)
		return ret;
	ret = handler->apply(inode, 0, value, value_len);
	if (ret) {
		apfs_setxattr(trans, inode, handler->xattr_name, NULL,
			       0, flags);
		return ret;
	}

	set_bit(APFS_INODE_HAS_PROPS, &APFS_I(inode)->runtime_flags);

	return 0;
}

static int iterate_object_props(struct apfs_root *root,
				struct apfs_path *path,
				u64 objectid,
				void (*iterator)(void *,
						 const struct prop_handler *,
						 u16 flags,
						 const char *,
						 size_t),
				void *ctx)
{
	int ret;
	char *value_buf = NULL;
	int value_buf_len = 0;

	while (1) {
		struct apfs_key key = {};
		struct apfs_xattr_item *xi;
		struct extent_buffer *leaf;
		int slot;
		const struct hlist_head *handlers;
		const struct prop_handler *handler;
		u32 name_len;
		u32 data_len;
		unsigned long data_ptr;
		u16 flags;

		slot = path->slots[0];
		leaf = path->nodes[0];

		if (slot >= apfs_header_nritems(leaf)) {
			ret = apfs_next_leaf(root, path);
			if (ret < 0)
				goto out;
			else if (ret > 0)
				break;
			continue;
		}

		apfs_item_key_to_cpu(leaf, &key, slot);
		if (key.oid != objectid)
			break;
		if (key.type != APFS_TYPE_XATTR)
			goto next_slot;
		handlers = find_prop_handlers_by_hash(
			apfs_name_hash(key.name, key.namelen - 1, true));
		if (!handlers)
			goto next_slot;

		xi = apfs_item_ptr(leaf, slot, struct apfs_xattr_item);
		flags = apfs_xattr_item_flags(leaf, xi);
		name_len = key.namelen;
		data_len = apfs_xattr_item_len(leaf, xi);
		data_ptr = (unsigned long)xi + sizeof(*xi);

		if (name_len <= APFS_XATTR_APPLE_PREFIX_LEN ||
		    strncmp(key.name, APFS_XATTR_APPLE_PREFIX,
			    APFS_XATTR_APPLE_PREFIX_LEN))
			goto next_slot;

		handler = find_prop_handler(key.name, handlers);
		if (!handler)
			goto next_slot;

		if (data_len > value_buf_len) {
			kfree(value_buf);
			value_buf_len = data_len;
			value_buf = kmalloc(data_len, GFP_NOFS);
			if (!value_buf) {
				ret = -ENOMEM;
				goto out;
			}
		}
		read_extent_buffer(leaf, value_buf, data_ptr, data_len);

		iterator(ctx, handler, flags, value_buf, data_len);
next_slot:
		path->slots[0]++;
	}

	ret = 0;
out:
	kfree(value_buf);

	return ret;
}

static void inode_prop_iterator(void *ctx,
				const struct prop_handler *handler,
				u16 flags,
				const char *value,
				size_t len)
{
	struct inode *inode = ctx;
	struct apfs_root *root = APFS_I(inode)->root;
	int ret;

	ret = handler->apply(inode, flags, value, len);
	if (unlikely(ret))
		apfs_warn(root->fs_info,
			   "error applying prop %s to ino %llu (root %llu): %d",
			   handler->xattr_name, apfs_ino(APFS_I(inode)),
			   root->root_key.objectid, ret);
	else
		set_bit(APFS_INODE_HAS_PROPS, &APFS_I(inode)->runtime_flags);
}

int apfs_load_inode_props(struct inode *inode, struct apfs_path *path)
{
	struct apfs_root *root = APFS_I(inode)->root;
	u64 ino = apfs_ino(APFS_I(inode));
	int ret;

	ret = iterate_object_props(root, path, ino, inode_prop_iterator, inode);

	return ret;
}

static int prop_compression_validate(const void *value, size_t len, u16 flags)
{
	const struct apfs_compress_header *hdr = value;
	const struct apfs_xattr_dstream *xd = value;

	if (!value)
		return 0;
	if (!(flags & (APFS_XATTR_APPLE_PREFIX_LEN | APFS_XATTR_DATA_STREAM)))
		return -EUCLEAN;

	if (flags & APFS_XATTR_DATA_STREAM) {
		if (len < sizeof(*xd))
			return -EUCLEAN;
		return 0;
	}

	if (len < sizeof(*hdr))
		return -EUCLEAN;
	if (!apfs_compress_is_valid_type(apfs_stack_compress_header_type(hdr)))
		return -EUCLEAN;

	return 0;
}

static int resource_fork_validate(const void *value, size_t len, u16 flags)
{
	const struct apfs_xattr_dstream *xd = value;

	if (!value)
		return 0;
	if (len != sizeof(*xd))
		return -EUCLEAN;
	return 0;
}

static int prop_xattr_embedded_apply(struct inode *inode, const void *value,
				     size_t len)
{
	const struct apfs_compress_header *hdr = value;
	u32 type;
	u64 nbytes = len - sizeof(*hdr);

	type = hdr->type;

	/* Set NOCOMPRESS flag */
	if (type == APFS_COMPRESS_NONE) {
		APFS_I(inode)->flags |= APFS_INODE_NOCOMPRESS;
		APFS_I(inode)->flags &= ~APFS_INODE_COMPRESS;
		APFS_I(inode)->prop_compress = APFS_COMPRESS_NONE;

		return 0;
	}

	APFS_I(inode)->flags &= ~APFS_INODE_NOCOMPRESS;
	APFS_I(inode)->flags |= APFS_INODE_COMPRESS;
	APFS_I(inode)->prop_compress = type;

	if (!apfs_compress_data_inlined(type))
		return 0;

	if (type == APFS_COMPRESS_PLAIN_ATTR) {
		inode_set_bytes(inode, apfs_stack_compress_header_size(hdr));
		APFS_I(inode)->disk_i_size = nbytes;
	} else {
		inode_set_bytes(inode, nbytes);
		APFS_I(inode)->disk_i_size = nbytes;
	}
	return 0;
}

static int prop_xattr_dstream_apply(struct inode *inode, const void *value,
				     size_t len)
{
	const struct apfs_xattr_dstream *xd = value;
	const struct apfs_dstream_item *di = &xd->dstream;

	APFS_I(inode)->cid = xd->id;
	inode_set_bytes(inode, apfs_stack_dstream_size(di));
	APFS_I(inode)->disk_i_size = apfs_stack_dstream_size(di);

	return 0;
}


static int prop_compression_apply(struct inode *inode, u16 flags,
				  const void *value, size_t len)
{

	if (flags & APFS_XATTR_DATA_STREAM)
		return prop_xattr_dstream_apply(inode, value, len);
	else
		return prop_xattr_embedded_apply(inode, value, len);
}

static int resource_fork_apply(struct inode *inode, u16 flags,
			       const void *value, size_t len)
{
	const struct apfs_xattr_dstream *xd = value;
	const struct apfs_dstream_item *di = value + sizeof(u64);

	APFS_I(inode)->cid = apfs_stack_xattr_dstream_id(xd);
	inode_set_bytes(inode, apfs_stack_dstream_size(di));
	return 0;
}


static const char *prop_compression_extract(struct inode *inode)
{
	switch (APFS_I(inode)->prop_compress) {
	case APFS_COMPRESS_ZLIB:
	case APFS_COMPRESS_LZO:
	case APFS_COMPRESS_ZSTD:
		return apfs_compress_type2str(APFS_I(inode)->prop_compress);
	default:
		break;
	}

	return NULL;
}

static struct prop_handler prop_handlers[] = {
	{
		.xattr_name = APFS_XATTR_APPLE_PREFIX "decmpfs",
		.validate = prop_compression_validate,
		.apply = prop_compression_apply,
		.extract = NULL,
		.inheritable = 0
	},
	{
		.xattr_name = APFS_XATTR_APPLE_PREFIX "ResourceFork",
		.validate = resource_fork_validate,
		.apply = resource_fork_apply,
		.extract = NULL,
		.inheritable = 0
	},
};

static int inherit_props(struct apfs_trans_handle *trans,
			 struct inode *inode,
			 struct inode *parent)
{
	struct apfs_root *root = APFS_I(inode)->root;
	struct apfs_fs_info *fs_info = root->fs_info;
	int ret;
	int i;
	bool need_reserve = false;

	if (!test_bit(APFS_INODE_HAS_PROPS,
		      &APFS_I(parent)->runtime_flags))
		return 0;

	for (i = 0; i < ARRAY_SIZE(prop_handlers); i++) {
		const struct prop_handler *h = &prop_handlers[i];
		const char *value;
		u64 num_bytes = 0;

		if (!h->inheritable)
			continue;

		value = h->extract(parent);
		if (!value)
			continue;

		/*
		 * This is not strictly necessary as the property should be
		 * valid, but in case it isn't, don't propagate it further.
		 */
		ret = h->validate(value, strlen(value), 0);
		if (ret)
			continue;

		/*
		 * Currently callers should be reserving 1 item for properties,
		 * since we only have 1 property that we currently support.  If
		 * we add more in the future we need to try and reserve more
		 * space for them.  But we should also revisit how we do space
		 * reservations if we do add more properties in the future.
		 */
		if (need_reserve) {
			num_bytes = apfs_calc_insert_metadata_size(fs_info, 1);
			ret = apfs_block_rsv_add(root, trans->block_rsv,
					num_bytes, APFS_RESERVE_NO_FLUSH);
			if (ret)
				return ret;
		}

		ret = apfs_setxattr(trans, inode, h->xattr_name, value,
				     strlen(value), 0);
		if (!ret) {
			ret = h->apply(inode, 0, value, strlen(value));
			if (ret)
				apfs_setxattr(trans, inode, h->xattr_name,
					       NULL, 0, 0);
			else
				set_bit(APFS_INODE_HAS_PROPS,
					&APFS_I(inode)->runtime_flags);
		}

		if (need_reserve) {
			apfs_block_rsv_release(fs_info, trans->block_rsv,
					num_bytes, NULL);
			if (ret)
				return ret;
		}
		need_reserve = true;
	}

	return 0;
}

int apfs_inode_inherit_props(struct apfs_trans_handle *trans,
			      struct inode *inode,
			      struct inode *dir)
{
	if (!dir)
		return 0;

	return inherit_props(trans, inode, dir);
}

int apfs_subvol_inherit_props(struct apfs_trans_handle *trans,
			       struct apfs_root *root,
			       struct apfs_root *parent_root)
{
	struct super_block *sb = root->fs_info->sb;
	struct inode *parent_inode, *child_inode;
	int ret;

	parent_inode = apfs_iget(sb, APFS_FIRST_FREE_OBJECTID, parent_root);
	if (IS_ERR(parent_inode))
		return PTR_ERR(parent_inode);

	child_inode = apfs_iget(sb, APFS_FIRST_FREE_OBJECTID, root);
	if (IS_ERR(child_inode)) {
		iput(parent_inode);
		return PTR_ERR(child_inode);
	}

	ret = inherit_props(trans, child_inode, parent_inode);
	iput(child_inode);
	iput(parent_inode);

	return ret;
}

void __init apfs_props_init(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(prop_handlers); i++) {
		struct prop_handler *p = &prop_handlers[i];
		u64 h = apfs_name_hash(p->xattr_name, strlen(p->xattr_name),
				       true);
		hash_add(prop_handlers_ht, &p->node, h);
	}
}
