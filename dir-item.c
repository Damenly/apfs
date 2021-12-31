// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2007 Oracle.  All rights reserved.
 */

#include "ctree.h"
#include "disk-io.h"
#include "transaction.h"
#include "unicode.h"
#include "apfs_trace.h"

u32 apfs_name_hash(const char *name, int len, bool case_fold)
{
	struct apfs_unicursor cursor;
	u32 hash = 0xFFFFFFFF;
	char *new_name = NULL;

	/*
	if (name[len - 1] != 0) {
		new_name = kmalloc(len, GFP_NOFS);
		memcpy(new_name, name, len);
		len++;
		new_name[len] = 0;
	}
	name = new_name;
	*/
	apfs_init_unicursor(&cursor, name);

	while (1) {
		unicode_t utf32;

		utf32 = apfs_normalize_next(&cursor, case_fold);
		if (!utf32)
			break;

		hash = crc32c(hash, &utf32, sizeof(utf32));
	}

	kfree(new_name);
	return (hash & 0x003fffff);
}

/*
 * xattrs work a lot like directories, this inserts an xattr item
 * into the tree
 */
int apfs_insert_xattr_item(struct apfs_trans_handle *trans,
			    struct apfs_root *root,
			    struct apfs_path *path, u64 objectid,
			    const char *name, u16 name_len,
			    const void *data, u16 data_len)
{
	BUG();
}

/*
 * insert a directory item in the tree, doing all the magic for
 * both indexes. 'dir' indicates which objectid to insert it into,
 * 'location' is the key to stuff into the directory item, 'type' is the
 * type of the inode we're pointing to, and 'index' is the sequence number
 * to use for the second index (if one is created).
 * Will return 0 or -ENOMEM
 */
int apfs_insert_dir_item(struct apfs_trans_handle *trans, const char *name,
			  int name_len, struct apfs_inode *dir,
			  struct apfs_key *location, u8 type, u64 index)
{
	return 0;
}

/*
 * lookup a directory item based on name.  'dir' is the objectid
 * we're searching in, and 'mod' tells us if you plan on deleting the
 * item (use mod < 0) or changing the options (use mod > 0)
 */
struct apfs_dir_item *apfs_lookup_dir_item(struct apfs_trans_handle *trans,
					     struct apfs_root *root,
					     struct apfs_path *path, u64 dir,
					     const char *name, int name_len,
					     int mod)
{
	int ret;
	struct apfs_key key = {};
	int ins_len = mod < 0 ? -1 : 0;
	int cow = mod != 0;

	key.objectid = dir;
	key.type = APFS_DIR_ITEM_KEY;

	key.offset = 0;

	apfs_release_path(path);
	ret = apfs_search_slot(trans, root, &key, path, ins_len, cow);
	if (ret < 0)
		return ERR_PTR(ret);
	if (ret > 0)
		return NULL;

	return apfs_match_dir_item_name(root->fs_info, path, name, name_len);
}

struct apfs_drec_item *apfs_match_dir_rec_name(struct apfs_fs_info *fs_info,
					       struct apfs_path *path,
					       const char *name, int name_len);
/*
 * lookup a directory record based on name.  'dir' is the objectid
 * we're searching in, and 'mod' tells us if you plan on deleting the
 * item (use mod < 0) or changing the options (use mod > 0)
 */
struct apfs_drec_item *apfs_lookup_dir_rec(struct apfs_trans_handle *trans,
					   struct apfs_root *root,
					   struct apfs_path *path, u64 dir,
					   const char *name, int name_len,
					   int mod)
{
	int ret;
	struct apfs_key key = {};
	int ins_len = mod < 0 ? -1 : 0;
	int cow = mod != 0;
	char *kname = NULL;

	key.oid = dir;
	key.type = APFS_TYPE_DIR_REC;
	key.namelen = name_len + 1;

	kname = kmalloc(name_len + 1, GFP_NOFS);
	if (kname == NULL)
		return ERR_PTR(-ENOMEM);
	memcpy(kname, name, name_len);
	kname[name_len] = 0;
	key.name = kname;

	if (root->fs_info->normalization_insensitive) {
		key.hash = apfs_name_hash(name, name_len,
			apfs_is_case_insensitive(root->fs_info->__super_copy));
	} else {
		WARN_ON(1);// TODO...
	}

	trace_printk("apfs lookup dir namelen hash %u %d %.*s in dir %llu\n",
		     key.hash, name_len, name_len, name, dir);
	apfs_release_path(path);
	ret = apfs_search_slot(trans, root, &key, path, ins_len, cow);

	kfree(kname);
	key.name = NULL;

	trace_printk("apfs lookup dir done ret %d start %llu slots %d\n", ret,
	       path->nodes[0]->start, path->slots[0]);
	if (ret < 0)
		return ERR_PTR(ret);
	if (ret > 0)
		return NULL;
	return apfs_item_ptr(path->nodes[0], path->slots[0],
			     struct apfs_drec_item);
}

int apfs_check_dir_item_collision(struct apfs_root *root, u64 dir,
				   const char *name, int name_len)
{
	int ret;
	struct apfs_key key = {};
	struct apfs_dir_item *di;
	int data_size;
	struct extent_buffer *leaf;
	int slot;
	struct apfs_path *path;


	path = apfs_alloc_path();
	if (!path)
		return -ENOMEM;

	key.objectid = dir;
	key.type = APFS_TYPE_DIR_REC;
	key.offset = apfs_name_hash(name, name_len,
		    apfs_is_case_insensitive(root->fs_info->__super_copy));


	ret = apfs_search_slot(NULL, root, &key, path, 0, 0);

	/* return back any errors */
	if (ret < 0)
		goto out;

	/* nothing found, we're safe */
	if (ret > 0) {
		ret = 0;
		goto out;
	}

	/* we found an item, look for our name in the item */
	di = apfs_match_dir_item_name(root->fs_info, path, name, name_len);
	if (di) {
		/* our exact name was found */
		ret = -EEXIST;
		goto out;
	}

	/*
	 * see if there is room in the item to insert this
	 * name
	 */
	data_size = sizeof(*di) + name_len;
	leaf = path->nodes[0];
	slot = path->slots[0];
	if (data_size + apfs_item_size_nr(leaf, slot) +
	    sizeof(struct apfs_item) > APFS_LEAF_DATA_SIZE(root->fs_info)) {
		ret = -EOVERFLOW;
	} else {
		/* plenty of insertion room */
		ret = 0;
	}
out:
	apfs_free_path(path);
	return ret;
}

/*
 * lookup a directory item based on index.  'dir' is the objectid
 * we're searching in, and 'mod' tells us if you plan on deleting the
 * item (use mod < 0) or changing the options (use mod > 0)
 *
 * The name is used to make sure the index really points to the name you were
 * looking for.
 */
struct apfs_dir_item *
apfs_lookup_dir_index_item(struct apfs_trans_handle *trans,
			    struct apfs_root *root,
			    struct apfs_path *path, u64 dir,
			    u64 objectid, const char *name, int name_len,
			    int mod)
{
	int ret;
	struct apfs_key key = {};
	int ins_len = mod < 0 ? -1 : 0;
	int cow = mod != 0;

	key.objectid = dir;
	key.type = APFS_DIR_INDEX_KEY;
	key.offset = objectid;

	ret = apfs_search_slot(trans, root, &key, path, ins_len, cow);
	if (ret < 0)
		return ERR_PTR(ret);
	if (ret > 0)
		return ERR_PTR(-ENOENT);
	return apfs_match_dir_item_name(root->fs_info, path, name, name_len);
}

struct apfs_dir_item *
apfs_search_dir_index_item(struct apfs_root *root,
			    struct apfs_path *path, u64 dirid,
			    const char *name, int name_len)
{
	struct extent_buffer *leaf;
	struct apfs_dir_item *di;
	struct apfs_key key = {};
	u32 nritems;
	int ret;

	key.objectid = dirid;
	key.type = APFS_DIR_INDEX_KEY;
	key.offset = 0;

	ret = apfs_search_slot(NULL, root, &key, path, 0, 0);
	if (ret < 0)
		return ERR_PTR(ret);

	leaf = path->nodes[0];
	nritems = apfs_header_nritems(leaf);

	while (1) {
		if (path->slots[0] >= nritems) {
			ret = apfs_next_leaf(root, path);
			if (ret < 0)
				return ERR_PTR(ret);
			if (ret > 0)
				break;
			leaf = path->nodes[0];
			nritems = apfs_header_nritems(leaf);
			continue;
		}

		apfs_item_key_to_cpu(leaf, &key, path->slots[0]);
		if (key.objectid != dirid || key.type != APFS_DIR_INDEX_KEY)
			break;

		di = apfs_match_dir_item_name(root->fs_info, path,
					       name, name_len);
		if (di)
			return di;

		path->slots[0]++;
	}
	return NULL;
}

struct apfs_dir_item *apfs_lookup_xattr(struct apfs_trans_handle *trans,
					  struct apfs_root *root,
					  struct apfs_path *path, u64 dir,
					  const char *name, u16 name_len,
					  int mod)
{
	return NULL;
}

struct apfs_xattr_item *apfs_lookup_xattr_item(struct apfs_trans_handle *trans,
					       struct apfs_root *root,
					       struct apfs_path *path, u64 dir,
					       const char *name, int mod)
{
	int ret;
	struct apfs_key key = {};
	int ins_len = mod < 0 ? -1 : 0;
	int cow = mod != 0;

	key.oid = dir;
	key.type = APFS_TYPE_XATTR;
	key.name = name;

	ret = apfs_search_slot(trans, root, &key, path, ins_len, cow);
	if (ret < 0)
		return ERR_PTR(ret);
	if (ret > 0)
		return NULL;

	return apfs_item_ptr(path->nodes[0], path->slots[0],
			     struct apfs_xattr_item);
}

/*
 * helper function to look at the directory item pointed to by 'path'
 * this walks through all the entries in a dir item and finds one
 * for a specific name.
 */
struct apfs_dir_item *apfs_match_dir_item_name(struct apfs_fs_info *fs_info,
						 struct apfs_path *path,
						 const char *name, int name_len)
{
	struct apfs_dir_item *dir_item;
	unsigned long name_ptr;
	u32 total_len;
	u32 cur = 0;
	u32 this_len;
	struct extent_buffer *leaf;

	leaf = path->nodes[0];
	dir_item = apfs_item_ptr(leaf, path->slots[0], struct apfs_dir_item);

	total_len = apfs_item_size_nr(leaf, path->slots[0]);
	while (cur < total_len) {
		this_len = sizeof(*dir_item) +
			apfs_dir_name_len(leaf, dir_item) +
			apfs_dir_data_len(leaf, dir_item);
		name_ptr = (unsigned long)(dir_item + 1);

		if (apfs_dir_name_len(leaf, dir_item) == name_len &&
		    memcmp_extent_buffer(leaf, name, name_ptr, name_len) == 0)
			return dir_item;

		cur += this_len;
		dir_item = (struct apfs_dir_item *)((char *)dir_item +
						     this_len);
	}
	return NULL;
}

/*
 * helper function to look at the directory item pointed to by 'path'
 * this walks through all the entries in a dir item and finds one
 * for a specific name.
 */
struct apfs_drec_item *apfs_match_dir_rec_name(struct apfs_fs_info *fs_info,
					       struct apfs_path *path,
					       const char *name, int name_len)
{
	struct apfs_root *root = fs_info->root_root;
	int ret;
	struct apfs_key key = {};
	u64 oid;

	apfs_item_key_to_cpu(path->nodes[0], &key, path->slots[0]);
	oid = key.oid;

	while (1) {
		struct extent_buffer *leaf = path->nodes[0];
		int slot = path->slots[0];
		struct apfs_key key = {};

		if (slot >= apfs_header_nritems(leaf)) {
			ret = apfs_next_leaf(root, path);
			if (ret)
				goto out;

			continue;
		}

		apfs_item_key_to_cpu(leaf, &key, slot);
		if (key.oid != oid || key.type != APFS_TYPE_DIR_REC)
			goto out;
		trace_printk("key namelen %d name %.*s arg namelen %d name %.*s\n",
			key.namelen, key.namelen, key.name,
			name_len, name_len, name);
		if (key.namelen - 1 != name_len) {
			path->slots[0]++;
			continue;
		}
		if (memcmp(key.name, name, name_len)) {
			path->slots[0]++;
			continue;
		}

		return apfs_item_ptr(leaf, slot, struct apfs_drec_item);
	}
out:
	return NULL;
}

/*
 * given a pointer into a directory item, delete it.  This
 * handles items that have more than one entry in them.
 */
int apfs_delete_one_dir_name(struct apfs_trans_handle *trans,
			      struct apfs_root *root,
			      struct apfs_path *path,
			      struct apfs_dir_item *di)
{

	struct extent_buffer *leaf;
	u32 sub_item_len;
	u32 item_len;
	int ret = 0;

	leaf = path->nodes[0];
	sub_item_len = sizeof(*di) + apfs_dir_name_len(leaf, di) +
		apfs_dir_data_len(leaf, di);
	item_len = apfs_item_size_nr(leaf, path->slots[0]);
	if (sub_item_len == item_len) {
		ret = apfs_del_item(trans, root, path);
	} else {
		/* MARKER */
		unsigned long ptr = (unsigned long)di;
		unsigned long start;

		start = apfs_item_ptr_offset(leaf, path->slots[0]);
		memmove_extent_buffer(leaf, ptr, ptr + sub_item_len,
			item_len - (ptr + sub_item_len - start));
		apfs_truncate_item(path, item_len - sub_item_len, 1);
	}
	return ret;
}
