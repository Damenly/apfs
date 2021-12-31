// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2007 Oracle.  All rights reserved.
 */

#include "ctree.h"
#include "disk-io.h"
#include "print-tree.h"

struct root_name_map {
	u64 id;
	char name[16];
};

static const struct root_name_map root_map[] = {
	{ APFS_ROOT_TREE_OBJECTID,		"ROOT_TREE"		},
	{ APFS_EXTENT_TREE_OBJECTID,		"EXTENT_TREE"		},
	{ APFS_CHUNK_TREE_OBJECTID,		"CHUNK_TREE"		},
	{ APFS_DEV_TREE_OBJECTID,		"DEV_TREE"		},
	{ APFS_FS_TREE_OBJECTID,		"FS_TREE"		},
	{ APFS_CSUM_TREE_OBJECTID,		"CSUM_TREE"		},
	{ APFS_TREE_LOG_OBJECTID,		"TREE_LOG"		},
	{ APFS_QUOTA_TREE_OBJECTID,		"QUOTA_TREE"		},
	{ APFS_UUID_TREE_OBJECTID,		"UUID_TREE"		},
	{ APFS_FREE_SPACE_TREE_OBJECTID,	"FREE_SPACE_TREE"	},
	{ APFS_DATA_RELOC_TREE_OBJECTID,	"DATA_RELOC_TREE"	},
};

const char *apfs_root_name(const struct apfs_key *key, char *buf)
{
	int i;

	if (key->objectid == APFS_TREE_RELOC_OBJECTID) {
		snprintf(buf, APFS_ROOT_NAME_BUF_LEN,
			 "TREE_RELOC offset=%llu", key->offset);
		return buf;
	}

	for (i = 0; i < ARRAY_SIZE(root_map); i++) {
		if (root_map[i].id == key->objectid)
			return root_map[i].name;
	}

	snprintf(buf, APFS_ROOT_NAME_BUF_LEN, "%llu", key->objectid);
	return buf;
}

void print_fskey_type(u8 type)
{
	static const char* key_to_str[256] = {
		[APFS_TYPE_ANY] = "ANY",
		[APFS_TYPE_SNAP_METADATA] = "SNAP_META",
		[APFS_TYPE_EXTENT] = "EXTENT",
		[APFS_TYPE_INODE] = "INODE",
		[APFS_TYPE_XATTR] = "XATTR",
		[APFS_TYPE_SIBLING_LINK] = "SIBLING_ID",
		[APFS_TYPE_DSTREAM_ID] = "DSTREAM",
		[APFS_TYPE_CRYPTO_STATE] = "CRYPTO_STATE",
		[APFS_TYPE_FILE_EXTENT] = "FILE EXTENT",
		[APFS_TYPE_DIR_REC] = "DIR_REC",
		[APFS_TYPE_DIR_STATS] = "DIR_STATS",
		[APFS_TYPE_SNAP_NAME] = "SNAP_INODE",
		[APFS_TYPE_SIBLING_MAP] = "SIBLING_MAP",
		[APFS_TYPE_FILE_INFO] = "FILE_INFO",
	};

	if (key_to_str[type])
		trace_printk(KERN_CONT "%s", key_to_str[type]);
	else
		trace_printk(KERN_CONT "UNKNOWN.%d", type);
}

void apfs_print_key(const struct extent_buffer *eb, const struct apfs_key *key)
{
	bool is_fs = apfs_header_subtype(eb) == APFS_OBJ_TYPE_FSTREE;
	bool is_extref = apfs_header_subtype(eb) == APFS_OBJ_TYPE_REFTREE;
	bool sensitive = false;

	trace_printk(KERN_CONT"key (%llu", key->id);
	if (is_fs) {
		trace_printk(KERN_CONT"[%llu %u] ", (u64)key->oid, key->type);
		print_fskey_type(key->type);
		sensitive = !apfs_is_normalization_insensitive(eb->fs_info->__super_copy);
	} else if (is_extref) {
		trace_printk(KERN_CONT"[%llu %u] ", key->oid * eb->fs_info->block_size,
		       key->type);
	} else {
		trace_printk(KERN_CONT"%u", key->type);
	}

	trace_printk(KERN_CONT" %llu", key->offset);

	if (!is_fs || !key->namelen) {
		trace_printk(KERN_CONT")");
		return;
	}

	if (!sensitive)
		trace_printk(KERN_CONT"[hash %d namelen %u]", key->hash, key->namelen);
	else
		trace_printk(KERN_CONT"[namelen %u]", key->namelen);

	if (key->name) {
		trace_printk(KERN_CONT" %s", key->name);
	}

	trace_printk(KERN_CONT")");
}

static void print_chunk(struct extent_buffer *eb, struct apfs_chunk *chunk)
{
	int num_stripes = apfs_chunk_num_stripes(eb, chunk);
	int i;
	pr_info("\t\tchunk length %llu owner %llu type %llu num_stripes %d\n",
	       apfs_chunk_length(eb, chunk), apfs_chunk_owner(eb, chunk),
	       apfs_chunk_type(eb, chunk), num_stripes);
	for (i = 0 ; i < num_stripes ; i++) {
		pr_info("\t\t\tstripe %d devid %llu offset %llu\n", i,
		      apfs_stripe_devid_nr(eb, chunk, i),
		      apfs_stripe_offset_nr(eb, chunk, i));
	}
}
static void print_dev_item(struct extent_buffer *eb,
			   struct apfs_dev_item *dev_item)
{
	pr_info("\t\tdev item devid %llu total_bytes %llu bytes used %llu\n",
	       apfs_device_id(eb, dev_item),
	       apfs_device_total_bytes(eb, dev_item),
	       apfs_device_bytes_used(eb, dev_item));
}
static void print_extent_data_ref(struct extent_buffer *eb,
				  struct apfs_extent_data_ref *ref)
{
	pr_cont("extent data backref root %llu objectid %llu offset %llu count %u\n",
	       apfs_extent_data_ref_root(eb, ref),
	       apfs_extent_data_ref_objectid(eb, ref),
	       apfs_extent_data_ref_offset(eb, ref),
	       apfs_extent_data_ref_count(eb, ref));
}

static void print_extent_item(struct extent_buffer *eb, int slot, int type)
{
	struct apfs_extent_item *ei;
	struct apfs_extent_inline_ref *iref;
	struct apfs_extent_data_ref *dref;
	struct apfs_shared_data_ref *sref;
	struct apfs_disk_key key;
	unsigned long end;
	unsigned long ptr;
	u32 item_size = apfs_item_size_nr(eb, slot);
	u64 flags;
	u64 offset;
	int ref_index = 0;

	if (unlikely(item_size < sizeof(*ei))) {
		apfs_print_v0_err(eb->fs_info);
		apfs_handle_fs_error(eb->fs_info, -EINVAL, NULL);
	}

	ei = apfs_item_ptr(eb, slot, struct apfs_extent_item);
	flags = apfs_extent_flags(eb, ei);

	pr_info("\t\textent refs %llu gen %llu flags %llu\n",
	       apfs_extent_refs(eb, ei), apfs_extent_generation(eb, ei),
	       flags);

	if ((type == APFS_EXTENT_ITEM_KEY) &&
	    flags & APFS_EXTENT_FLAG_TREE_BLOCK) {
		struct apfs_tree_block_info *info;
		info = (struct apfs_tree_block_info *)(ei + 1);
		apfs_tree_block_key(eb, info, &key);
		pr_info("\t\ttree block key (%llu %u %llu) level %d\n",
		       apfs_disk_key_objectid(&key), apfs_disk_key_type(&key),
		       apfs_disk_key_offset(&key),
		       apfs_tree_block_level(eb, info));
		iref = (struct apfs_extent_inline_ref *)(info + 1);
	} else {
		iref = (struct apfs_extent_inline_ref *)(ei + 1);
	}

	ptr = (unsigned long)iref;
	end = (unsigned long)ei + item_size;
	while (ptr < end) {
		iref = (struct apfs_extent_inline_ref *)ptr;
		type = apfs_extent_inline_ref_type(eb, iref);
		offset = apfs_extent_inline_ref_offset(eb, iref);
		pr_info("\t\tref#%d: ", ref_index++);
		switch (type) {
		case APFS_TREE_BLOCK_REF_KEY:
			pr_cont("tree block backref root %llu\n", offset);
			break;
		case APFS_SHARED_BLOCK_REF_KEY:
			pr_cont("shared block backref parent %llu\n", offset);
			/*
			 * offset is supposed to be a tree block which
			 * must be aligned to nodesize.
			 */
			if (!IS_ALIGNED(offset, eb->fs_info->sectorsize))
				pr_info(
			"\t\t\t(parent %llu not aligned to sectorsize %u)\n",
					offset, eb->fs_info->sectorsize);
			break;
		case APFS_EXTENT_DATA_REF_KEY:
			dref = (struct apfs_extent_data_ref *)(&iref->offset);
			print_extent_data_ref(eb, dref);
			break;
		case APFS_SHARED_DATA_REF_KEY:
			sref = (struct apfs_shared_data_ref *)(iref + 1);
			pr_cont("shared data backref parent %llu count %u\n",
			       offset, apfs_shared_data_ref_count(eb, sref));
			/*
			 * offset is supposed to be a tree block which
			 * must be aligned to nodesize.
			 */
			if (!IS_ALIGNED(offset, eb->fs_info->nodesize))
				pr_info(
			"\t\t\t(parent %llu not aligned to sectorsize %u)\n",
				     offset, eb->fs_info->sectorsize);
			break;
		default:
			pr_cont("(extent %llu has INVALID ref type %d)\n",
				  eb->start, type);
			return;
		}
		ptr += apfs_extent_inline_ref_size(type);
	}
	WARN_ON(ptr > end);
}

static void print_uuid_item(struct extent_buffer *l, unsigned long offset,
			    u32 item_size)
{
	if (!IS_ALIGNED(item_size, sizeof(u64))) {
		pr_warn("APFS: uuid item with illegal size %lu!\n",
			(unsigned long)item_size);
		return;
	}
	while (item_size) {
		__le64 subvol_id;

		read_extent_buffer(l, &subvol_id, offset, sizeof(subvol_id));
		pr_info("\t\tsubvol_id %llu\n", le64_to_cpu(subvol_id));
		item_size -= sizeof(u64);
		offset += sizeof(u64);
	}
}

/*
 * Helper to output refs and locking status of extent buffer.  Useful to debug
 * race condition related problems.
 */
static void print_eb_refs_lock(struct extent_buffer *eb)
{
#ifdef CONFIG_APFS_DEBUG
	apfs_info(eb->fs_info, "refs %u lock_owner %u current %u",
		   atomic_read(&eb->refs), eb->lock_owner, current->pid);
#endif
}

static void print_omap_item(const struct extent_buffer *eb, int slot)

{
	struct apfs_omap_item oi;

	read_extent_buffer(eb, &oi, apfs_item_offset_nr(eb, slot),
			   apfs_item_size_nr(eb, slot));
	pr_info("\t\t flags %u size %u paddr %llu\n",
		apfs_omap_flags(&oi), apfs_omap_size(&oi),
		apfs_omap_paddr(&oi) * eb->fs_info->block_size);
}

void apfs_print_leaf(struct extent_buffer *l)
{
	struct apfs_fs_info *fs_info;
	int i;
	u32 type, nr;
	struct apfs_item *item;
	struct apfs_root_item *ri;
	struct apfs_dir_item *di;
	struct apfs_inode_item *ii;
	struct apfs_block_group_item *bi;
	struct apfs_file_extent_item *fi;
	struct apfs_extent_data_ref *dref;
	struct apfs_shared_data_ref *sref;
	struct apfs_dev_extent *dev_extent;
	struct apfs_key key = {};
	struct apfs_key found_key = {};
	bool is_omap_node = apfs_header_subtype(l) == APFS_OBJ_TYPE_OMAP;

	if (!l)
		return;

	fs_info = l->fs_info;
	nr = apfs_header_nritems(l);


	pr_info("leaf %llu gen %llu total ptrs %d free space %d owner %llu",
		 apfs_header_bytenr(l), apfs_header_generation(l), nr,
		 apfs_leaf_free_space(l), apfs_header_owner(l));

	print_eb_refs_lock(l);
	for (i = 0 ; i < nr ; i++) {
		item = apfs_item_nr(i);
		apfs_item_key_to_cpu(l, &key, i);

		type = key.type;
		trace_printk(KERN_CONT "\titem %d ", i);

		apfs_print_key(l, &key);
		pr_info(KERN_CONT " itemoff %d itemsize %d\n",
			apfs_item_offset(l, item), apfs_item_size(l, item));

		if (is_omap_node) {
			print_omap_item(l, i);
			continue;
		}
		switch (type) {
		case APFS_INODE_ITEM_KEY:
			ii = apfs_item_ptr(l, i, struct apfs_inode_item);
			pr_info("\t\tinode generation %llu size %llu mode %o\n",
			       apfs_inode_generation(l, ii),
			       apfs_inode_size(l, ii),
			       apfs_inode_mode(l, ii));
			break;
		case APFS_DIR_ITEM_KEY:
			di = apfs_item_ptr(l, i, struct apfs_dir_item);
			apfs_dir_item_key_to_cpu(l, di, &found_key);
			pr_info("\t\tdir oid %llu type %u\n",
				found_key.objectid,
				apfs_dir_type(l, di));
			break;
		case APFS_ROOT_ITEM_KEY:
			ri = apfs_item_ptr(l, i, struct apfs_root_item);
			pr_info("\t\troot data bytenr %llu refs %u\n",
				apfs_disk_root_bytenr(l, ri),
				apfs_disk_root_refs(l, ri));
			break;
		case APFS_EXTENT_ITEM_KEY:
		case APFS_METADATA_ITEM_KEY:
			print_extent_item(l, i, type);
			break;
		case APFS_TREE_BLOCK_REF_KEY:
			pr_info("\t\ttree block backref\n");
			break;
		case APFS_SHARED_BLOCK_REF_KEY:
			pr_info("\t\tshared block backref\n");
			break;
		case APFS_EXTENT_DATA_REF_KEY:
			dref = apfs_item_ptr(l, i,
					      struct apfs_extent_data_ref);
			print_extent_data_ref(l, dref);
			break;
		case APFS_SHARED_DATA_REF_KEY:
			sref = apfs_item_ptr(l, i,
					      struct apfs_shared_data_ref);
			pr_info("\t\tshared data backref count %u\n",
			       apfs_shared_data_ref_count(l, sref));
			break;
		case APFS_EXTENT_DATA_KEY:
			fi = apfs_item_ptr(l, i,
					    struct apfs_file_extent_item);
			if (apfs_file_extent_type(l, fi) ==
			    APFS_FILE_EXTENT_INLINE) {
				pr_info("\t\tinline extent data size %llu\n",
				       apfs_file_extent_ram_bytes(l, fi));
				break;
			}
			pr_info("\t\textent data disk bytenr %llu nr %llu\n",
			       apfs_file_extent_disk_bytenr(l, fi),
			       apfs_file_extent_disk_num_bytes(l, fi));
			pr_info("\t\textent data offset %llu nr %llu ram %llu\n",
			       apfs_file_extent_offset(l, fi),
			       apfs_file_extent_num_bytes(l, fi),
			       apfs_file_extent_ram_bytes(l, fi));
			break;
		case APFS_EXTENT_REF_V0_KEY:
			apfs_print_v0_err(fs_info);
			apfs_handle_fs_error(fs_info, -EINVAL, NULL);
			break;
		case APFS_BLOCK_GROUP_ITEM_KEY:
			bi = apfs_item_ptr(l, i,
					    struct apfs_block_group_item);
			pr_info(
		   "\t\tblock group used %llu chunk_objectid %llu flags %llu\n",
				apfs_block_group_used(l, bi),
				apfs_block_group_chunk_objectid(l, bi),
				apfs_block_group_flags(l, bi));
			break;
		case APFS_CHUNK_ITEM_KEY:
			print_chunk(l, apfs_item_ptr(l, i,
						      struct apfs_chunk));
			break;
		case APFS_DEV_ITEM_KEY:
			print_dev_item(l, apfs_item_ptr(l, i,
					struct apfs_dev_item));
			break;
		case APFS_DEV_EXTENT_KEY:
			dev_extent = apfs_item_ptr(l, i,
						    struct apfs_dev_extent);
			pr_info("\t\tdev extent chunk_tree %llu\n\t\tchunk objectid %llu chunk offset %llu length %llu\n",
			       apfs_dev_extent_chunk_tree(l, dev_extent),
			       apfs_dev_extent_chunk_objectid(l, dev_extent),
			       apfs_dev_extent_chunk_offset(l, dev_extent),
			       apfs_dev_extent_length(l, dev_extent));
			break;
		case APFS_PERSISTENT_ITEM_KEY:
			pr_info("\t\tpersistent item objectid %llu offset %llu\n",
					key.objectid, key.offset);
			switch (key.objectid) {
			case APFS_DEV_STATS_OBJECTID:
				pr_info("\t\tdevice stats\n");
				break;
			default:
				pr_info("\t\tunknown persistent item\n");
			}
			break;
		case APFS_TEMPORARY_ITEM_KEY:
			pr_info("\t\ttemporary item objectid %llu offset %llu\n",
					key.objectid, key.offset);
			switch (key.objectid) {
			case APFS_BALANCE_OBJECTID:
				pr_info("\t\tbalance status\n");
				break;
			default:
				pr_info("\t\tunknown temporary item\n");
			}
			break;
		case APFS_DEV_REPLACE_KEY:
			pr_info("\t\tdev replace\n");
			break;
		case APFS_UUID_KEY_SUBVOL:
		case APFS_UUID_KEY_RECEIVED_SUBVOL:
			print_uuid_item(l, apfs_item_ptr_offset(l, i),
					apfs_item_size_nr(l, i));
			break;
		}
	}
}

void apfs_print_tree(struct extent_buffer *c, bool follow)
{
	struct apfs_fs_info *fs_info;
	int i; u32 nr;
	struct apfs_key key = {};
	int level;

	if (!c)
		return;
	fs_info = c->fs_info;
	nr = apfs_header_nritems(c);
	level = apfs_header_level(c);

	if (level == 0) {
		apfs_print_leaf(c);
		return;
	}
	apfs_info(fs_info,
		   "node %llu level %d gen %llu total ptrs %d free spc UNKNOWN owner %llu",
		   apfs_header_bytenr(c), level, apfs_header_generation(c),
		   nr, apfs_header_owner(c));
	print_eb_refs_lock(c);
	for (i = 0; i < nr; i++) {
		apfs_node_key_to_cpu(c, &key, i);
		pr_info("\tkey %d (%llu %u %llu) block %llu gen %llu\n",
		       i, key.objectid, key.type, key.offset,
		       apfs_node_blockptr(c, i),
		       apfs_node_ptr_generation(c, i));
	}
	if (!follow)
		return;
	for (i = 0; i < nr; i++) {
		struct apfs_key first_key = {};
		struct extent_buffer *next;

		apfs_node_key_to_cpu(c, &first_key, i);
		next = read_tree_block(fs_info, apfs_node_blockptr(c, i),
				       apfs_header_owner(c),
				       apfs_node_ptr_generation(c, i),
				       level - 1, &first_key);
		if (IS_ERR(next)) {
			continue;
		} else if (!extent_buffer_uptodate(next)) {
			free_extent_buffer(next);
			continue;
		}

		if (apfs_is_leaf(next) &&
		   level != 1)
			BUG();
		if (apfs_header_level(next) !=
		       level - 1)
			BUG();
		apfs_print_tree(next, follow);
		free_extent_buffer(next);
	}
}
