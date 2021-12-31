// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2007 Oracle.  All rights reserved.
 */

#include "ctree.h"
#include "disk-io.h"
#include "transaction.h"
#include "print-tree.h"

struct apfs_inode_ref *apfs_find_name_in_backref(struct extent_buffer *leaf,
						   int slot, const char *name,
						   int name_len)
{
	struct apfs_inode_ref *ref;
	unsigned long ptr;
	unsigned long name_ptr;
	u32 item_size;
	u32 cur_offset = 0;
	int len;

	item_size = apfs_item_size_nr(leaf, slot);
	ptr = apfs_item_ptr_offset(leaf, slot);
	while (cur_offset < item_size) {
		ref = (struct apfs_inode_ref *)(ptr + cur_offset);
		len = apfs_inode_ref_name_len(leaf, ref);
		name_ptr = (unsigned long)(ref + 1);
		cur_offset += len + sizeof(*ref);
		if (len != name_len)
			continue;
		if (memcmp_extent_buffer(leaf, name, name_ptr, name_len) == 0)
			return ref;
	}
	return NULL;
}

struct apfs_inode_extref *apfs_find_name_in_ext_backref(
		struct extent_buffer *leaf, int slot, u64 ref_objectid,
		const char *name, int name_len)
{
	struct apfs_inode_extref *extref;
	unsigned long ptr;
	unsigned long name_ptr;
	u32 item_size;
	u32 cur_offset = 0;
	int ref_name_len;

	item_size = apfs_item_size_nr(leaf, slot);
	ptr = apfs_item_ptr_offset(leaf, slot);

	/*
	 * Search all extended backrefs in this item. We're only
	 * looking through any collisions so most of the time this is
	 * just going to compare against one buffer. If all is well,
	 * we'll return success and the inode ref object.
	 */
	while (cur_offset < item_size) {
		extref = (struct apfs_inode_extref *) (ptr + cur_offset);
		name_ptr = (unsigned long)(&extref->name);
		ref_name_len = apfs_inode_extref_name_len(leaf, extref);

		if (ref_name_len == name_len &&
		    apfs_inode_extref_parent(leaf, extref) == ref_objectid &&
		    (memcmp_extent_buffer(leaf, name, name_ptr, name_len) == 0))
			return extref;

		cur_offset += ref_name_len + sizeof(*extref);
	}
	return NULL;
}

/* Returns NULL if no extref found */
struct apfs_inode_extref *
apfs_lookup_inode_extref(struct apfs_trans_handle *trans,
			  struct apfs_root *root,
			  struct apfs_path *path,
			  const char *name, int name_len,
			  u64 inode_objectid, u64 ref_objectid, int ins_len,
			  int cow)
{
	int ret;
	struct apfs_key key = {};

	key.objectid = inode_objectid;
	key.type = APFS_INODE_EXTREF_KEY;
	key.offset = apfs_extref_hash(ref_objectid, name, name_len);

	ret = apfs_search_slot(trans, root, &key, path, ins_len, cow);
	if (ret < 0)
		return ERR_PTR(ret);
	if (ret > 0)
		return NULL;
	return apfs_find_name_in_ext_backref(path->nodes[0], path->slots[0],
					      ref_objectid, name, name_len);

}

static int apfs_del_inode_extref(struct apfs_trans_handle *trans,
				  struct apfs_root *root,
				  const char *name, int name_len,
				  u64 inode_objectid, u64 ref_objectid,
				  u64 *index)
{
	struct apfs_path *path;
	struct apfs_key key = {};
	struct apfs_inode_extref *extref;
	struct extent_buffer *leaf;
	int ret;
	int del_len = name_len + sizeof(*extref);
	unsigned long ptr;
	unsigned long item_start;
	u32 item_size;

	key.objectid = inode_objectid;
	key.type = APFS_INODE_EXTREF_KEY;
	key.offset = apfs_extref_hash(ref_objectid, name, name_len);

	path = apfs_alloc_path();
	if (!path)
		return -ENOMEM;

	ret = apfs_search_slot(trans, root, &key, path, -1, 1);
	if (ret > 0)
		ret = -ENOENT;
	if (ret < 0)
		goto out;

	/*
	 * Sanity check - did we find the right item for this name?
	 * This should always succeed so error here will make the FS
	 * readonly.
	 */
	extref = apfs_find_name_in_ext_backref(path->nodes[0], path->slots[0],
						ref_objectid, name, name_len);
	if (!extref) {
		apfs_handle_fs_error(root->fs_info, -ENOENT, NULL);
		ret = -EROFS;
		goto out;
	}

	leaf = path->nodes[0];
	item_size = apfs_item_size_nr(leaf, path->slots[0]);
	if (index)
		*index = apfs_inode_extref_index(leaf, extref);

	if (del_len == item_size) {
		/*
		 * Common case only one ref in the item, remove the
		 * whole item.
		 */
		ret = apfs_del_item(trans, root, path);
		goto out;
	}

	ptr = (unsigned long)extref;
	item_start = apfs_item_ptr_offset(leaf, path->slots[0]);

	memmove_extent_buffer(leaf, ptr, ptr + del_len,
			      item_size - (ptr + del_len - item_start));

	apfs_truncate_item(path, item_size - del_len, 1);

out:
	apfs_free_path(path);

	return ret;
}

int apfs_del_inode_ref(struct apfs_trans_handle *trans,
			struct apfs_root *root,
			const char *name, int name_len,
			u64 inode_objectid, u64 ref_objectid, u64 *index)
{
	struct apfs_path *path;
	struct apfs_key key = {};
	struct apfs_inode_ref *ref;
	struct extent_buffer *leaf;
	unsigned long ptr;
	unsigned long item_start;
	u32 item_size;
	u32 sub_item_len;
	int ret;
	int search_ext_refs = 0;
	int del_len = name_len + sizeof(*ref);

	key.objectid = inode_objectid;
	key.offset = ref_objectid;
	key.type = APFS_INODE_REF_KEY;

	path = apfs_alloc_path();
	if (!path)
		return -ENOMEM;

	ret = apfs_search_slot(trans, root, &key, path, -1, 1);
	if (ret > 0) {
		ret = -ENOENT;
		search_ext_refs = 1;
		goto out;
	} else if (ret < 0) {
		goto out;
	}

	ref = apfs_find_name_in_backref(path->nodes[0], path->slots[0], name,
					 name_len);
	if (!ref) {
		ret = -ENOENT;
		search_ext_refs = 1;
		goto out;
	}
	leaf = path->nodes[0];
	item_size = apfs_item_size_nr(leaf, path->slots[0]);

	if (index)
		*index = apfs_inode_ref_index(leaf, ref);

	if (del_len == item_size) {
		ret = apfs_del_item(trans, root, path);
		goto out;
	}
	ptr = (unsigned long)ref;
	sub_item_len = name_len + sizeof(*ref);
	item_start = apfs_item_ptr_offset(leaf, path->slots[0]);
	memmove_extent_buffer(leaf, ptr, ptr + sub_item_len,
			      item_size - (ptr + sub_item_len - item_start));
	apfs_truncate_item(path, item_size - sub_item_len, 1);
out:
	apfs_free_path(path);

	if (search_ext_refs) {
		/*
		 * No refs were found, or we could not find the
		 * name in our ref array. Find and remove the extended
		 * inode ref then.
		 */
		return apfs_del_inode_extref(trans, root, name, name_len,
					      inode_objectid, ref_objectid, index);
	}

	return ret;
}

/*
 * apfs_insert_inode_extref() - Inserts an extended inode ref into a tree.
 *
 * The caller must have checked against APFS_LINK_MAX already.
 */
static int apfs_insert_inode_extref(struct apfs_trans_handle *trans,
				     struct apfs_root *root,
				     const char *name, int name_len,
				     u64 inode_objectid, u64 ref_objectid, u64 index)
{
	struct apfs_inode_extref *extref;
	int ret;
	int ins_len = name_len + sizeof(*extref);
	unsigned long ptr;
	struct apfs_path *path;
	struct apfs_key key = {};
	struct extent_buffer *leaf;
	struct apfs_item *item;

	key.objectid = inode_objectid;
	key.type = APFS_INODE_EXTREF_KEY;
	key.offset = apfs_extref_hash(ref_objectid, name, name_len);

	path = apfs_alloc_path();
	if (!path)
		return -ENOMEM;

	ret = apfs_insert_empty_item(trans, root, path, &key,
				      ins_len);
	if (ret == -EEXIST) {
		if (apfs_find_name_in_ext_backref(path->nodes[0],
						   path->slots[0],
						   ref_objectid,
						   name, name_len))
			goto out;

		apfs_extend_item(path, ins_len);
		ret = 0;
	}
	if (ret < 0)
		goto out;

	leaf = path->nodes[0];
	item = apfs_item_nr(path->slots[0]);
	ptr = (unsigned long)apfs_item_ptr(leaf, path->slots[0], char);
	ptr += apfs_item_size(leaf, item) - ins_len;
	extref = (struct apfs_inode_extref *)ptr;

	apfs_set_inode_extref_name_len(path->nodes[0], extref, name_len);
	apfs_set_inode_extref_index(path->nodes[0], extref, index);
	apfs_set_inode_extref_parent(path->nodes[0], extref, ref_objectid);

	ptr = (unsigned long)&extref->name;
	write_extent_buffer(path->nodes[0], name, ptr, name_len);
	apfs_mark_buffer_dirty(path->nodes[0]);

out:
	apfs_free_path(path);
	return ret;
}

/* Will return 0, -ENOMEM, -EMLINK, or -EEXIST or anything from the CoW path */
int apfs_insert_inode_ref(struct apfs_trans_handle *trans,
			   struct apfs_root *root,
			   const char *name, int name_len,
			   u64 inode_objectid, u64 ref_objectid, u64 index)
{
	struct apfs_fs_info *fs_info = root->fs_info;
	struct apfs_path *path;
	struct apfs_key key = {};
	struct apfs_inode_ref *ref;
	unsigned long ptr;
	int ret;
	int ins_len = name_len + sizeof(*ref);

	key.objectid = inode_objectid;
	key.offset = ref_objectid;
	key.type = APFS_INODE_REF_KEY;

	path = apfs_alloc_path();
	if (!path)
		return -ENOMEM;

	path->skip_release_on_error = 1;
	ret = apfs_insert_empty_item(trans, root, path, &key,
				      ins_len);
	if (ret == -EEXIST) {
		u32 old_size;
		ref = apfs_find_name_in_backref(path->nodes[0], path->slots[0],
						 name, name_len);
		if (ref)
			goto out;

		old_size = apfs_item_size_nr(path->nodes[0], path->slots[0]);
		apfs_extend_item(path, ins_len);
		ref = apfs_item_ptr(path->nodes[0], path->slots[0],
				     struct apfs_inode_ref);
		ref = (struct apfs_inode_ref *)((unsigned long)ref + old_size);
		apfs_set_inode_ref_name_len(path->nodes[0], ref, name_len);
		apfs_set_inode_ref_index(path->nodes[0], ref, index);
		ptr = (unsigned long)(ref + 1);
		ret = 0;
	} else if (ret < 0) {
		if (ret == -EOVERFLOW) {
			if (apfs_find_name_in_backref(path->nodes[0],
						       path->slots[0],
						       name, name_len))
				ret = -EEXIST;
			else
				ret = -EMLINK;
		}
		goto out;
	} else {
		ref = apfs_item_ptr(path->nodes[0], path->slots[0],
				     struct apfs_inode_ref);
		apfs_set_inode_ref_name_len(path->nodes[0], ref, name_len);
		apfs_set_inode_ref_index(path->nodes[0], ref, index);
		ptr = (unsigned long)(ref + 1);
	}
	write_extent_buffer(path->nodes[0], name, ptr, name_len);
	apfs_mark_buffer_dirty(path->nodes[0]);

out:
	apfs_free_path(path);

	if (ret == -EMLINK) {
		struct apfs_super_block *disk_super = fs_info->super_copy;
		/* We ran out of space in the ref array. Need to
		 * add an extended ref. */
		if (apfs_super_incompat_flags(disk_super)
		    & APFS_FEATURE_INCOMPAT_EXTENDED_IREF)
			ret = apfs_insert_inode_extref(trans, root, name,
							name_len,
							inode_objectid,
							ref_objectid, index);
	}

	return ret;
}

int apfs_insert_empty_inode(struct apfs_trans_handle *trans,
			     struct apfs_root *root,
			     struct apfs_path *path, u64 objectid)
{
	struct apfs_key key = {};
	int ret;
	key.objectid = objectid;
	key.type = APFS_INODE_ITEM_KEY;
	key.offset = 0;

	ret = apfs_insert_empty_item(trans, root, path, &key,
				      sizeof(struct apfs_inode_item));
	return ret;
}

int apfs_lookup_inode(struct apfs_trans_handle *trans, struct apfs_root
		       *root, struct apfs_path *path,
		       struct apfs_key *location, int mod)
{
	int ins_len = mod < 0 ? -1 : 0;
	int cow = mod != 0;

	ASSERT(location->type == APFS_TYPE_INODE);

	return apfs_search_slot(trans, root, location, path, ins_len, cow);
}
