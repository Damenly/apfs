// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) Qu Wenruo 2017.  All rights reserved.
 */

/*
 * The module is used to catch unexpected/corrupted tree block data.
 * Such behavior can be caused either by a fuzzed image or bugs.
 *
 * The objective is to do leaf/node validation checks when tree block is read
 * from disk, and check *every* possible member, so other code won't
 * need to checking them again.
 *
 * Due to the potential and unwanted damage, every checker needs to be
 * carefully reviewed otherwise so it does not prevent mount of valid images.
 */

#include <linux/types.h>
#include <linux/stddef.h>
#include <linux/error-injection.h>
#include "ctree.h"
#include "tree-checker.h"
#include "disk-io.h"
#include "compression.h"
#include "volumes.h"
#include "misc.h"

/*
 * Error message should follow the following format:
 * corrupt <type>: <identifier>, <reason>[, <bad_value>]
 *
 * @type:	leaf or node
 * @identifier:	the necessary info to locate the leaf/node.
 * 		It's recommended to decode key.objecitd/offset if it's
 * 		meaningful.
 * @reason:	describe the error
 * @bad_value:	optional, it's recommended to output bad value and its
 *		expected value (range).
 *
 * Since comma is used to separate the components, only space is allowed
 * inside each component.
 */

/*
 * Append generic "corrupt leaf/node root=%llu block=%llu slot=%d: " to @fmt.
 * Allows callers to customize the output.
 */
__printf(3, 4)
__cold
static void generic_err(const struct extent_buffer *eb, int slot,
			const char *fmt, ...)
{
	const struct apfs_fs_info *fs_info = eb->fs_info;
	struct va_format vaf;
	va_list args;

	va_start(args, fmt);

	vaf.fmt = fmt;
	vaf.va = &args;

	apfs_crit(fs_info,
		"corrupt %s: root=%llu block=%llu slot=%d, %pV",
		apfs_header_level(eb) == 0 ? "leaf" : "node",
		apfs_header_owner(eb), apfs_header_bytenr(eb), slot, &vaf);
	va_end(args);
}

/*
 * Customized reporter for extent data item, since its key objectid and
 * offset has its own meaning.
 */
__printf(3, 4)
__cold
static void file_extent_err(const struct extent_buffer *eb, int slot,
			    const char *fmt, ...)
{
	const struct apfs_fs_info *fs_info = eb->fs_info;
	struct apfs_key key = {};
	struct va_format vaf;
	va_list args;

	apfs_item_key_to_cpu(eb, &key, slot);
	va_start(args, fmt);

	vaf.fmt = fmt;
	vaf.va = &args;

	apfs_crit(fs_info,
	"corrupt %s: root=%llu block=%llu slot=%d ino=%llu file_offset=%llu, %pV",
		apfs_header_level(eb) == 0 ? "leaf" : "node",
		apfs_header_owner(eb), apfs_header_bytenr(eb), slot,
		key.objectid, key.offset, &vaf);
	va_end(args);
}

/*
 * Return 0 if the apfs_file_extent_##name is aligned to @alignment
 * Else return 1
 */
#define CHECK_FE_ALIGNED(leaf, slot, fi, name, alignment)		      \
({									      \
	if (unlikely(!IS_ALIGNED(apfs_file_extent_##name((leaf), (fi)),      \
				 (alignment))))				      \
		file_extent_err((leaf), (slot),				      \
	"invalid %s for file extent, have %llu, should be aligned to %u",     \
			(#name), apfs_file_extent_##name((leaf), (fi)),      \
			(alignment));					      \
	(!IS_ALIGNED(apfs_file_extent_##name((leaf), (fi)), (alignment)));   \
})

static u64 file_extent_end(struct extent_buffer *leaf,
			   struct apfs_key *key,
			   struct apfs_file_extent_item *extent)
{
	u64 end;
	u64 len;

	if (apfs_file_extent_type(leaf, extent) == APFS_FILE_EXTENT_INLINE) {
		len = apfs_file_extent_ram_bytes(leaf, extent);
		end = ALIGN(key->offset + len, leaf->fs_info->sectorsize);
	} else {
		len = apfs_file_extent_num_bytes(leaf, extent);
		end = key->offset + len;
	}
	return end;
}

/*
 * Customized report for dir_item, the only new important information is
 * key->objectid, which represents inode number
 */
__printf(3, 4)
__cold
static void dir_item_err(const struct extent_buffer *eb, int slot,
			 const char *fmt, ...)
{
	const struct apfs_fs_info *fs_info = eb->fs_info;
	struct apfs_key key = {};
	struct va_format vaf;
	va_list args;

	apfs_item_key_to_cpu(eb, &key, slot);
	va_start(args, fmt);

	vaf.fmt = fmt;
	vaf.va = &args;

	apfs_crit(fs_info,
		"corrupt %s: root=%llu block=%llu slot=%d ino=%llu, %pV",
		apfs_header_level(eb) == 0 ? "leaf" : "node",
		apfs_header_owner(eb), apfs_header_bytenr(eb), slot,
		key.objectid, &vaf);
	va_end(args);
}

/*
 * This functions checks prev_key->objectid, to ensure current key and prev_key
 * share the same objectid as inode number.
 *
 * This is to detect missing INODE_ITEM in subvolume trees.
 *
 * Return true if everything is OK or we don't need to check.
 * Return false if anything is wrong.
 */
static bool check_prev_ino(struct extent_buffer *leaf,
			   struct apfs_key *key, int slot,
			   struct apfs_key *prev_key)
{
	/* No prev key, skip check */
	if (slot == 0)
		return true;

	/* Only these key->types needs to be checked */
	ASSERT(key->type == APFS_XATTR_ITEM_KEY ||
	       key->type == APFS_INODE_REF_KEY ||
	       key->type == APFS_DIR_INDEX_KEY ||
	       key->type == APFS_DIR_ITEM_KEY ||
	       key->type == APFS_EXTENT_DATA_KEY);

	/*
	 * Only subvolume trees along with their reloc trees need this check.
	 * Things like log tree doesn't follow this ino requirement.
	 */
	if (!is_fstree(apfs_header_owner(leaf)))
		return true;

	if (key->objectid == prev_key->objectid)
		return true;

	/* Error found */
	dir_item_err(leaf, slot,
		"invalid previous key objectid, have %llu expect %llu",
		prev_key->objectid, key->objectid);
	return false;
}
static int check_extent_data_item(struct extent_buffer *leaf,
				  struct apfs_key *key, int slot,
				  struct apfs_key *prev_key)
{
	struct apfs_fs_info *fs_info = leaf->fs_info;
	struct apfs_file_extent_item *fi;
	u32 sectorsize = fs_info->sectorsize;
	u32 item_size = apfs_item_size_nr(leaf, slot);
	u64 extent_end;

	if (unlikely(!IS_ALIGNED(key->offset, sectorsize))) {
		file_extent_err(leaf, slot,
"unaligned file_offset for file extent, have %llu should be aligned to %u",
			key->offset, sectorsize);
		return -EUCLEAN;
	}

	/*
	 * Previous key must have the same key->objectid (ino).
	 * It can be XATTR_ITEM, INODE_ITEM or just another EXTENT_DATA.
	 * But if objectids mismatch, it means we have a missing
	 * INODE_ITEM.
	 */
	if (unlikely(!check_prev_ino(leaf, key, slot, prev_key)))
		return -EUCLEAN;

	fi = apfs_item_ptr(leaf, slot, struct apfs_file_extent_item);

	/*
	 * Make sure the item contains at least inline header, so the file
	 * extent type is not some garbage.
	 */
	if (unlikely(item_size < APFS_FILE_EXTENT_INLINE_DATA_START)) {
		file_extent_err(leaf, slot,
				"invalid item size, have %u expect [%zu, %u)",
				item_size, APFS_FILE_EXTENT_INLINE_DATA_START,
				SZ_4K);
		return -EUCLEAN;
	}
	if (unlikely(apfs_file_extent_type(leaf, fi) >=
		     APFS_NR_FILE_EXTENT_TYPES)) {
		file_extent_err(leaf, slot,
		"invalid type for file extent, have %u expect range [0, %u]",
			apfs_file_extent_type(leaf, fi),
			APFS_NR_FILE_EXTENT_TYPES - 1);
		return -EUCLEAN;
	}

	/*
	 * Support for new compression/encryption must introduce incompat flag,
	 * and must be caught in open_ctree().
	 */
	if (unlikely(apfs_file_extent_compression(leaf, fi) >=
		     APFS_NR_COMPRESS_TYPES)) {
		file_extent_err(leaf, slot,
	"invalid compression for file extent, have %u expect range [0, %u]",
			apfs_file_extent_compression(leaf, fi),
			APFS_NR_COMPRESS_TYPES - 1);
		return -EUCLEAN;
	}
	if (unlikely(apfs_file_extent_encryption(leaf, fi))) {
		file_extent_err(leaf, slot,
			"invalid encryption for file extent, have %u expect 0",
			apfs_file_extent_encryption(leaf, fi));
		return -EUCLEAN;
	}
	if (apfs_file_extent_type(leaf, fi) == APFS_FILE_EXTENT_INLINE) {
		/* Inline extent must have 0 as key offset */
		if (unlikely(key->offset)) {
			file_extent_err(leaf, slot,
		"invalid file_offset for inline file extent, have %llu expect 0",
				key->offset);
			return -EUCLEAN;
		}

		/* Compressed inline extent has no on-disk size, skip it */
		if (apfs_file_extent_compression(leaf, fi) !=
		    APFS_COMPRESS_NONE)
			return 0;

		/* Uncompressed inline extent size must match item size */
		if (unlikely(item_size != APFS_FILE_EXTENT_INLINE_DATA_START +
					  apfs_file_extent_ram_bytes(leaf, fi))) {
			file_extent_err(leaf, slot,
	"invalid ram_bytes for uncompressed inline extent, have %u expect %llu",
				item_size, APFS_FILE_EXTENT_INLINE_DATA_START +
				apfs_file_extent_ram_bytes(leaf, fi));
			return -EUCLEAN;
		}
		return 0;
	}

	/* Regular or preallocated extent has fixed item size */
	if (unlikely(item_size != sizeof(*fi))) {
		file_extent_err(leaf, slot,
	"invalid item size for reg/prealloc file extent, have %u expect %zu",
			item_size, sizeof(*fi));
		return -EUCLEAN;
	}
	if (unlikely(CHECK_FE_ALIGNED(leaf, slot, fi, ram_bytes, sectorsize) ||
		     CHECK_FE_ALIGNED(leaf, slot, fi, disk_bytenr, sectorsize) ||
		     CHECK_FE_ALIGNED(leaf, slot, fi, disk_num_bytes, sectorsize) ||
		     CHECK_FE_ALIGNED(leaf, slot, fi, offset, sectorsize) ||
		     CHECK_FE_ALIGNED(leaf, slot, fi, num_bytes, sectorsize)))
		return -EUCLEAN;

	/* Catch extent end overflow */
	if (unlikely(check_add_overflow(apfs_file_extent_num_bytes(leaf, fi),
					key->offset, &extent_end))) {
		file_extent_err(leaf, slot,
	"extent end overflow, have file offset %llu extent num bytes %llu",
				key->offset,
				apfs_file_extent_num_bytes(leaf, fi));
		return -EUCLEAN;
	}

	/*
	 * Check that no two consecutive file extent items, in the same leaf,
	 * present ranges that overlap each other.
	 */
	if (slot > 0 &&
	    prev_key->objectid == key->objectid &&
	    prev_key->type == APFS_EXTENT_DATA_KEY) {
		struct apfs_file_extent_item *prev_fi;
		u64 prev_end;

		prev_fi = apfs_item_ptr(leaf, slot - 1,
					 struct apfs_file_extent_item);
		prev_end = file_extent_end(leaf, prev_key, prev_fi);
		if (unlikely(prev_end > key->offset)) {
			file_extent_err(leaf, slot - 1,
"file extent end range (%llu) goes beyond start offset (%llu) of the next file extent",
					prev_end, key->offset);
			return -EUCLEAN;
		}
	}

	return 0;
}

static int check_csum_item(struct extent_buffer *leaf, struct apfs_key *key,
			   int slot, struct apfs_key *prev_key)
{
	struct apfs_fs_info *fs_info = leaf->fs_info;
	u32 sectorsize = fs_info->sectorsize;
	const u32 csumsize = fs_info->csum_size;

	if (unlikely(key->objectid != APFS_EXTENT_CSUM_OBJECTID)) {
		generic_err(leaf, slot,
		"invalid key objectid for csum item, have %llu expect %llu",
			key->objectid, APFS_EXTENT_CSUM_OBJECTID);
		return -EUCLEAN;
	}
	if (unlikely(!IS_ALIGNED(key->offset, sectorsize))) {
		generic_err(leaf, slot,
	"unaligned key offset for csum item, have %llu should be aligned to %u",
			key->offset, sectorsize);
		return -EUCLEAN;
	}
	if (unlikely(!IS_ALIGNED(apfs_item_size_nr(leaf, slot), csumsize))) {
		generic_err(leaf, slot,
	"unaligned item size for csum item, have %u should be aligned to %u",
			apfs_item_size_nr(leaf, slot), csumsize);
		return -EUCLEAN;
	}
	if (slot > 0 && prev_key->type == APFS_EXTENT_CSUM_KEY) {
		u64 prev_csum_end;
		u32 prev_item_size;

		prev_item_size = apfs_item_size_nr(leaf, slot - 1);
		prev_csum_end = (prev_item_size / csumsize) * sectorsize;
		prev_csum_end += prev_key->offset;
		if (unlikely(prev_csum_end > key->offset)) {
			generic_err(leaf, slot - 1,
"csum end range (%llu) goes beyond the start range (%llu) of the next csum item",
				    prev_csum_end, key->offset);
			return -EUCLEAN;
		}
	}
	return 0;
}

/* Inode item error output has the same format as dir_item_err() */
#define inode_item_err(eb, slot, fmt, ...)			\
	dir_item_err(eb, slot, fmt, __VA_ARGS__)

static int check_inode_key(struct extent_buffer *leaf, struct apfs_key *key,
			   int slot)
{
	struct apfs_key item_key = {};
	bool is_inode_item;

	apfs_item_key_to_cpu(leaf, &item_key, slot);
	is_inode_item = (item_key.type == APFS_INODE_ITEM_KEY);

	/* For XATTR_ITEM, location key should be all 0 */
	if (item_key.type == APFS_XATTR_ITEM_KEY) {
		if (unlikely(key->objectid != 0 || key->type != 0 ||
			     key->offset != 0))
			return -EUCLEAN;
		return 0;
	}

	if (unlikely((key->objectid < APFS_FIRST_FREE_OBJECTID ||
		      key->objectid > APFS_LAST_FREE_OBJECTID) &&
		     key->objectid != APFS_ROOT_TREE_DIR_OBJECTID &&
		     key->objectid != APFS_FREE_INO_OBJECTID)) {
		if (is_inode_item) {
			generic_err(leaf, slot,
	"invalid key objectid: has %llu expect %llu or [%llu, %llu] or %llu",
				key->objectid, APFS_ROOT_TREE_DIR_OBJECTID,
				APFS_FIRST_FREE_OBJECTID,
				APFS_LAST_FREE_OBJECTID,
				APFS_FREE_INO_OBJECTID);
		} else {
			dir_item_err(leaf, slot,
"invalid location key objectid: has %llu expect %llu or [%llu, %llu] or %llu",
				key->objectid, APFS_ROOT_TREE_DIR_OBJECTID,
				APFS_FIRST_FREE_OBJECTID,
				APFS_LAST_FREE_OBJECTID,
				APFS_FREE_INO_OBJECTID);
		}
		return -EUCLEAN;
	}
	if (unlikely(key->offset != 0)) {
		if (is_inode_item)
			inode_item_err(leaf, slot,
				       "invalid key offset: has %llu expect 0",
				       key->offset);
		else
			dir_item_err(leaf, slot,
				"invalid location key offset:has %llu expect 0",
				key->offset);
		return -EUCLEAN;
	}
	return 0;
}

static int check_root_key(struct extent_buffer *leaf, struct apfs_key *key,
			  int slot)
{
	struct apfs_key item_key = {};
	bool is_root_item;

	apfs_item_key_to_cpu(leaf, &item_key, slot);
	is_root_item = (item_key.type == APFS_ROOT_ITEM_KEY);

	/* No such tree id */
	if (unlikely(key->objectid == 0)) {
		if (is_root_item)
			generic_err(leaf, slot, "invalid root id 0");
		else
			dir_item_err(leaf, slot,
				     "invalid location key root id 0");
		return -EUCLEAN;
	}

	/* DIR_ITEM/INDEX/INODE_REF is not allowed to point to non-fs trees */
	if (unlikely(!is_fstree(key->objectid) && !is_root_item)) {
		dir_item_err(leaf, slot,
		"invalid location key objectid, have %llu expect [%llu, %llu]",
				key->objectid, APFS_FIRST_FREE_OBJECTID,
				APFS_LAST_FREE_OBJECTID);
		return -EUCLEAN;
	}

	/*
	 * ROOT_ITEM with non-zero offset means this is a snapshot, created at
	 * @offset transid.
	 * Furthermore, for location key in DIR_ITEM, its offset is always -1.
	 *
	 * So here we only check offset for reloc tree whose key->offset must
	 * be a valid tree.
	 */
	if (unlikely(key->objectid == APFS_TREE_RELOC_OBJECTID &&
		     key->offset == 0)) {
		generic_err(leaf, slot, "invalid root id 0 for reloc tree");
		return -EUCLEAN;
	}
	return 0;
}

static int check_dir_item(struct extent_buffer *leaf,
			  struct apfs_key *key, struct apfs_key *prev_key,
			  int slot)
{
	struct apfs_fs_info *fs_info = leaf->fs_info;
	struct apfs_dir_item *di;
	u32 item_size = apfs_item_size_nr(leaf, slot);
	u32 cur = 0;

	if (unlikely(!check_prev_ino(leaf, key, slot, prev_key)))
		return -EUCLEAN;

	di = apfs_item_ptr(leaf, slot, struct apfs_dir_item);
	while (cur < item_size) {
		struct apfs_key location_key = {};
		u32 name_len;
		u32 data_len;
		u32 max_name_len;
		u32 total_size;
		u32 name_hash;
		u8 dir_type;
		int ret;

		/* header itself should not cross item boundary */
		if (unlikely(cur + sizeof(*di) > item_size)) {
			dir_item_err(leaf, slot,
		"dir item header crosses item boundary, have %zu boundary %u",
				cur + sizeof(*di), item_size);
			return -EUCLEAN;
		}

		/* Location key check */
		apfs_dir_item_key_to_cpu(leaf, di, &location_key);
		if (location_key.type == APFS_ROOT_ITEM_KEY) {
			ret = check_root_key(leaf, &location_key, slot);
			if (unlikely(ret < 0))
				return ret;
		} else if (location_key.type == APFS_INODE_ITEM_KEY ||
			   location_key.type == 0) {
			ret = check_inode_key(leaf, &location_key, slot);
			if (unlikely(ret < 0))
				return ret;
		} else {
			dir_item_err(leaf, slot,
			"invalid location key type, have %u, expect %u or %u",
				     location_key.type, APFS_ROOT_ITEM_KEY,
				     APFS_INODE_ITEM_KEY);
			return -EUCLEAN;
		}

		/* dir type check */
		dir_type = apfs_dir_type(leaf, di);
		if (unlikely(dir_type >= APFS_FT_MAX)) {
			dir_item_err(leaf, slot,
			"invalid dir item type, have %u expect [0, %u)",
				dir_type, APFS_FT_MAX);
			return -EUCLEAN;
		}

		if (unlikely(key->type == APFS_XATTR_ITEM_KEY &&
			     dir_type != APFS_FT_XATTR)) {
			dir_item_err(leaf, slot,
		"invalid dir item type for XATTR key, have %u expect %u",
				dir_type, APFS_FT_XATTR);
			return -EUCLEAN;
		}
		if (unlikely(dir_type == APFS_FT_XATTR &&
			     key->type != APFS_XATTR_ITEM_KEY)) {
			dir_item_err(leaf, slot,
			"xattr dir type found for non-XATTR key");
			return -EUCLEAN;
		}
		if (dir_type == APFS_FT_XATTR)
			max_name_len = XATTR_NAME_MAX;
		else
			max_name_len = APFS_NAME_LEN;

		/* Name/data length check */
		name_len = apfs_dir_name_len(leaf, di);
		data_len = apfs_dir_data_len(leaf, di);
		if (unlikely(name_len > max_name_len)) {
			dir_item_err(leaf, slot,
			"dir item name len too long, have %u max %u",
				name_len, max_name_len);
			return -EUCLEAN;
		}
		if (unlikely(name_len + data_len > APFS_MAX_XATTR_SIZE(fs_info))) {
			dir_item_err(leaf, slot,
			"dir item name and data len too long, have %u max %u",
				name_len + data_len,
				APFS_MAX_XATTR_SIZE(fs_info));
			return -EUCLEAN;
		}

		if (unlikely(data_len && dir_type != APFS_FT_XATTR)) {
			dir_item_err(leaf, slot,
			"dir item with invalid data len, have %u expect 0",
				data_len);
			return -EUCLEAN;
		}

		total_size = sizeof(*di) + name_len + data_len;

		/* header and name/data should not cross item boundary */
		if (unlikely(cur + total_size > item_size)) {
			dir_item_err(leaf, slot,
		"dir item data crosses item boundary, have %u boundary %u",
				cur + total_size, item_size);
			return -EUCLEAN;
		}

		/*
		 * Special check for XATTR/DIR_ITEM, as key->offset is name
		 * hash, should match its name
		 */
		if (key->type == APFS_DIR_ITEM_KEY ||
		    key->type == APFS_XATTR_ITEM_KEY) {
			char namebuf[max(APFS_NAME_LEN, XATTR_NAME_MAX)];

			read_extent_buffer(leaf, namebuf,
					(unsigned long)(di + 1), name_len);
			name_hash = apfs_name_hash(namebuf, name_len,
				apfs_is_case_insensitive(leaf->fs_info->__super_copy));
			if (unlikely(key->offset != name_hash)) {
				dir_item_err(leaf, slot,
		"name hash mismatch with key, have 0x%016x expect 0x%016llx",
					name_hash, key->offset);
				return -EUCLEAN;
			}
		}
		cur += total_size;
		di = (struct apfs_dir_item *)((void *)di + total_size);
	}
	return 0;
}

__printf(3, 4)
__cold
static void block_group_err(const struct extent_buffer *eb, int slot,
			    const char *fmt, ...)
{
	const struct apfs_fs_info *fs_info = eb->fs_info;
	struct apfs_key key = {};
	struct va_format vaf;
	va_list args;

	apfs_item_key_to_cpu(eb, &key, slot);
	va_start(args, fmt);

	vaf.fmt = fmt;
	vaf.va = &args;

	apfs_crit(fs_info,
	"corrupt %s: root=%llu block=%llu slot=%d bg_start=%llu bg_len=%llu, %pV",
		apfs_header_level(eb) == 0 ? "leaf" : "node",
		apfs_header_owner(eb), apfs_header_bytenr(eb), slot,
		key.objectid, key.offset, &vaf);
	va_end(args);
}

static int check_block_group_item(struct extent_buffer *leaf,
				  struct apfs_key *key, int slot)
{
	struct apfs_block_group_item bgi;
	u32 item_size = apfs_item_size_nr(leaf, slot);
	u64 flags;
	u64 type;

	/*
	 * Here we don't really care about alignment since extent allocator can
	 * handle it.  We care more about the size.
	 */
	if (unlikely(key->offset == 0)) {
		block_group_err(leaf, slot,
				"invalid block group size 0");
		return -EUCLEAN;
	}

	if (unlikely(item_size != sizeof(bgi))) {
		block_group_err(leaf, slot,
			"invalid item size, have %u expect %zu",
				item_size, sizeof(bgi));
		return -EUCLEAN;
	}

	read_extent_buffer(leaf, &bgi, apfs_item_ptr_offset(leaf, slot),
			   sizeof(bgi));
	if (unlikely(apfs_stack_block_group_chunk_objectid(&bgi) !=
		     APFS_FIRST_CHUNK_TREE_OBJECTID)) {
		block_group_err(leaf, slot,
		"invalid block group chunk objectid, have %llu expect %llu",
				apfs_stack_block_group_chunk_objectid(&bgi),
				APFS_FIRST_CHUNK_TREE_OBJECTID);
		return -EUCLEAN;
	}

	if (unlikely(apfs_stack_block_group_used(&bgi) > key->offset)) {
		block_group_err(leaf, slot,
			"invalid block group used, have %llu expect [0, %llu)",
				apfs_stack_block_group_used(&bgi), key->offset);
		return -EUCLEAN;
	}

	flags = apfs_stack_block_group_flags(&bgi);
	if (unlikely(hweight64(flags & APFS_BLOCK_GROUP_PROFILE_MASK) > 1)) {
		block_group_err(leaf, slot,
"invalid profile flags, have 0x%llx (%lu bits set) expect no more than 1 bit set",
			flags & APFS_BLOCK_GROUP_PROFILE_MASK,
			hweight64(flags & APFS_BLOCK_GROUP_PROFILE_MASK));
		return -EUCLEAN;
	}

	type = flags & APFS_BLOCK_GROUP_TYPE_MASK;
	if (unlikely(type != APFS_BLOCK_GROUP_DATA &&
		     type != APFS_BLOCK_GROUP_METADATA &&
		     type != APFS_BLOCK_GROUP_SYSTEM &&
		     type != (APFS_BLOCK_GROUP_METADATA |
			      APFS_BLOCK_GROUP_DATA))) {
		block_group_err(leaf, slot,
"invalid type, have 0x%llx (%lu bits set) expect either 0x%llx, 0x%llx, 0x%llx or 0x%llx",
			type, hweight64(type),
			APFS_BLOCK_GROUP_DATA, APFS_BLOCK_GROUP_METADATA,
			APFS_BLOCK_GROUP_SYSTEM,
			APFS_BLOCK_GROUP_METADATA | APFS_BLOCK_GROUP_DATA);
		return -EUCLEAN;
	}
	return 0;
}

__printf(4, 5)
__cold
static void chunk_err(const struct extent_buffer *leaf,
		      const struct apfs_chunk *chunk, u64 logical,
		      const char *fmt, ...)
{
	const struct apfs_fs_info *fs_info = leaf->fs_info;
	bool is_sb;
	struct va_format vaf;
	va_list args;
	int i;
	int slot = -1;

	/* Only superblock eb is able to have such small offset */
	is_sb = (leaf->start == APFS_SUPER_INFO_OFFSET);

	if (!is_sb) {
		/*
		 * Get the slot number by iterating through all slots, this
		 * would provide better readability.
		 */
		for (i = 0; i < apfs_header_nritems(leaf); i++) {
			if (apfs_item_ptr_offset(leaf, i) ==
					(unsigned long)chunk) {
				slot = i;
				break;
			}
		}
	}
	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;

	if (is_sb)
		apfs_crit(fs_info,
		"corrupt superblock syschunk array: chunk_start=%llu, %pV",
			   logical, &vaf);
	else
		apfs_crit(fs_info,
	"corrupt leaf: root=%llu block=%llu slot=%d chunk_start=%llu, %pV",
			   APFS_CHUNK_TREE_OBJECTID, leaf->start, slot,
			   logical, &vaf);
	va_end(args);
}

/*
 * The common chunk check which could also work on super block sys chunk array.
 *
 * Return -EUCLEAN if anything is corrupted.
 * Return 0 if everything is OK.
 */
int apfs_check_chunk_valid(struct extent_buffer *leaf,
			    struct apfs_chunk *chunk, u64 logical)
{
	struct apfs_fs_info *fs_info = leaf->fs_info;
	u64 length;
	u64 chunk_end;
	u64 stripe_len;
	u16 num_stripes;
	u16 sub_stripes;
	u64 type;
	u64 features;
	bool mixed = false;
	int raid_index;
	int nparity;
	int ncopies;

	length = apfs_chunk_length(leaf, chunk);
	stripe_len = apfs_chunk_stripe_len(leaf, chunk);
	num_stripes = apfs_chunk_num_stripes(leaf, chunk);
	sub_stripes = apfs_chunk_sub_stripes(leaf, chunk);
	type = apfs_chunk_type(leaf, chunk);
	raid_index = apfs_bg_flags_to_raid_index(type);
	ncopies = apfs_raid_array[raid_index].ncopies;
	nparity = apfs_raid_array[raid_index].nparity;

	if (unlikely(!num_stripes)) {
		chunk_err(leaf, chunk, logical,
			  "invalid chunk num_stripes, have %u", num_stripes);
		return -EUCLEAN;
	}
	if (unlikely(num_stripes < ncopies)) {
		chunk_err(leaf, chunk, logical,
			  "invalid chunk num_stripes < ncopies, have %u < %d",
			  num_stripes, ncopies);
		return -EUCLEAN;
	}
	if (unlikely(nparity && num_stripes == nparity)) {
		chunk_err(leaf, chunk, logical,
			  "invalid chunk num_stripes == nparity, have %u == %d",
			  num_stripes, nparity);
		return -EUCLEAN;
	}
	if (unlikely(!IS_ALIGNED(logical, fs_info->sectorsize))) {
		chunk_err(leaf, chunk, logical,
		"invalid chunk logical, have %llu should aligned to %u",
			  logical, fs_info->sectorsize);
		return -EUCLEAN;
	}
	if (unlikely(apfs_chunk_sector_size(leaf, chunk) != fs_info->sectorsize)) {
		chunk_err(leaf, chunk, logical,
			  "invalid chunk sectorsize, have %u expect %u",
			  apfs_chunk_sector_size(leaf, chunk),
			  fs_info->sectorsize);
		return -EUCLEAN;
	}
	if (unlikely(!length || !IS_ALIGNED(length, fs_info->sectorsize))) {
		chunk_err(leaf, chunk, logical,
			  "invalid chunk length, have %llu", length);
		return -EUCLEAN;
	}
	if (unlikely(check_add_overflow(logical, length, &chunk_end))) {
		chunk_err(leaf, chunk, logical,
"invalid chunk logical start and length, have logical start %llu length %llu",
			  logical, length);
		return -EUCLEAN;
	}
	if (unlikely(!is_power_of_2(stripe_len) || stripe_len != APFS_STRIPE_LEN)) {
		chunk_err(leaf, chunk, logical,
			  "invalid chunk stripe length: %llu",
			  stripe_len);
		return -EUCLEAN;
	}
	if (unlikely(type & ~(APFS_BLOCK_GROUP_TYPE_MASK |
			      APFS_BLOCK_GROUP_PROFILE_MASK))) {
		chunk_err(leaf, chunk, logical,
			  "unrecognized chunk type: 0x%llx",
			  ~(APFS_BLOCK_GROUP_TYPE_MASK |
			    APFS_BLOCK_GROUP_PROFILE_MASK) &
			  apfs_chunk_type(leaf, chunk));
		return -EUCLEAN;
	}

	if (unlikely(!has_single_bit_set(type & APFS_BLOCK_GROUP_PROFILE_MASK) &&
		     (type & APFS_BLOCK_GROUP_PROFILE_MASK) != 0)) {
		chunk_err(leaf, chunk, logical,
		"invalid chunk profile flag: 0x%llx, expect 0 or 1 bit set",
			  type & APFS_BLOCK_GROUP_PROFILE_MASK);
		return -EUCLEAN;
	}
	if (unlikely((type & APFS_BLOCK_GROUP_TYPE_MASK) == 0)) {
		chunk_err(leaf, chunk, logical,
	"missing chunk type flag, have 0x%llx one bit must be set in 0x%llx",
			  type, APFS_BLOCK_GROUP_TYPE_MASK);
		return -EUCLEAN;
	}

	if (unlikely((type & APFS_BLOCK_GROUP_SYSTEM) &&
		     (type & (APFS_BLOCK_GROUP_METADATA |
			      APFS_BLOCK_GROUP_DATA)))) {
		chunk_err(leaf, chunk, logical,
			  "system chunk with data or metadata type: 0x%llx",
			  type);
		return -EUCLEAN;
	}

	features = apfs_super_incompat_flags(fs_info->super_copy);
	if (features & APFS_FEATURE_INCOMPAT_MIXED_GROUPS)
		mixed = true;

	if (!mixed) {
		if (unlikely((type & APFS_BLOCK_GROUP_METADATA) &&
			     (type & APFS_BLOCK_GROUP_DATA))) {
			chunk_err(leaf, chunk, logical,
			"mixed chunk type in non-mixed mode: 0x%llx", type);
			return -EUCLEAN;
		}
	}

	if (unlikely((type & APFS_BLOCK_GROUP_RAID10 && sub_stripes != 2) ||
		     (type & APFS_BLOCK_GROUP_RAID1 && num_stripes != 2) ||
		     (type & APFS_BLOCK_GROUP_RAID5 && num_stripes < 2) ||
		     (type & APFS_BLOCK_GROUP_RAID6 && num_stripes < 3) ||
		     (type & APFS_BLOCK_GROUP_DUP && num_stripes != 2) ||
		     ((type & APFS_BLOCK_GROUP_PROFILE_MASK) == 0 &&
		      num_stripes != 1))) {
		chunk_err(leaf, chunk, logical,
			"invalid num_stripes:sub_stripes %u:%u for profile %llu",
			num_stripes, sub_stripes,
			type & APFS_BLOCK_GROUP_PROFILE_MASK);
		return -EUCLEAN;
	}

	return 0;
}

/*
 * Enhanced version of chunk item checker.
 *
 * The common apfs_check_chunk_valid() doesn't check item size since it needs
 * to work on super block sys_chunk_array which doesn't have full item ptr.
 */
static int check_leaf_chunk_item(struct extent_buffer *leaf,
				 struct apfs_chunk *chunk,
				 struct apfs_key *key, int slot)
{
	int num_stripes;

	if (unlikely(apfs_item_size_nr(leaf, slot) < sizeof(struct apfs_chunk))) {
		chunk_err(leaf, chunk, key->offset,
			"invalid chunk item size: have %u expect [%zu, %u)",
			apfs_item_size_nr(leaf, slot),
			sizeof(struct apfs_chunk),
			APFS_LEAF_DATA_SIZE(leaf->fs_info));
		return -EUCLEAN;
	}

	num_stripes = apfs_chunk_num_stripes(leaf, chunk);
	/* Let apfs_check_chunk_valid() handle this error type */
	if (num_stripes == 0)
		goto out;

	if (unlikely(apfs_chunk_item_size(num_stripes) !=
		     apfs_item_size_nr(leaf, slot))) {
		chunk_err(leaf, chunk, key->offset,
			"invalid chunk item size: have %u expect %lu",
			apfs_item_size_nr(leaf, slot),
			apfs_chunk_item_size(num_stripes));
		return -EUCLEAN;
	}
out:
	return apfs_check_chunk_valid(leaf, chunk, key->offset);
}

__printf(3, 4)
__cold
static void dev_item_err(const struct extent_buffer *eb, int slot,
			 const char *fmt, ...)
{
	struct apfs_key key = {};
	struct va_format vaf;
	va_list args;

	apfs_item_key_to_cpu(eb, &key, slot);
	va_start(args, fmt);

	vaf.fmt = fmt;
	vaf.va = &args;

	apfs_crit(eb->fs_info,
	"corrupt %s: root=%llu block=%llu slot=%d devid=%llu %pV",
		apfs_header_level(eb) == 0 ? "leaf" : "node",
		apfs_header_owner(eb), apfs_header_bytenr(eb), slot,
		key.objectid, &vaf);
	va_end(args);
}

static int check_dev_item(struct extent_buffer *leaf,
			  struct apfs_key *key, int slot)
{
	struct apfs_dev_item *ditem;

	if (unlikely(key->objectid != APFS_DEV_ITEMS_OBJECTID)) {
		dev_item_err(leaf, slot,
			     "invalid objectid: has=%llu expect=%llu",
			     key->objectid, APFS_DEV_ITEMS_OBJECTID);
		return -EUCLEAN;
	}
	ditem = apfs_item_ptr(leaf, slot, struct apfs_dev_item);
	if (unlikely(apfs_device_id(leaf, ditem) != key->offset)) {
		dev_item_err(leaf, slot,
			     "devid mismatch: key has=%llu item has=%llu",
			     key->offset, apfs_device_id(leaf, ditem));
		return -EUCLEAN;
	}

	/*
	 * For device total_bytes, we don't have reliable way to check it, as
	 * it can be 0 for device removal. Device size check can only be done
	 * by dev extents check.
	 */
	if (unlikely(apfs_device_bytes_used(leaf, ditem) >
		     apfs_device_total_bytes(leaf, ditem))) {
		dev_item_err(leaf, slot,
			     "invalid bytes used: have %llu expect [0, %llu]",
			     apfs_device_bytes_used(leaf, ditem),
			     apfs_device_total_bytes(leaf, ditem));
		return -EUCLEAN;
	}
	/*
	 * Remaining members like io_align/type/gen/dev_group aren't really
	 * utilized.  Skip them to make later usage of them easier.
	 */
	return 0;
}

static int check_inode_item(struct extent_buffer *leaf,
			    struct apfs_key *key, int slot)
{
	struct apfs_fs_info *fs_info = leaf->fs_info;
	struct apfs_inode_item *iitem;
	u64 super_gen = apfs_super_generation(fs_info->super_copy);
	u32 valid_mask = (S_IFMT | S_ISUID | S_ISGID | S_ISVTX | 0777);
	u32 mode;
	int ret;

	ret = check_inode_key(leaf, key, slot);
	if (unlikely(ret < 0))
		return ret;

	iitem = apfs_item_ptr(leaf, slot, struct apfs_inode_item);

	/* Here we use super block generation + 1 to handle log tree */
	if (unlikely(apfs_inode_generation(leaf, iitem) > super_gen + 1)) {
		inode_item_err(leaf, slot,
			"invalid inode generation: has %llu expect (0, %llu]",
			       apfs_inode_generation(leaf, iitem),
			       super_gen + 1);
		return -EUCLEAN;
	}
	/* Note for ROOT_TREE_DIR_ITEM, mkfs could set its transid 0 */
	if (unlikely(apfs_inode_transid(leaf, iitem) > super_gen + 1)) {
		inode_item_err(leaf, slot,
			"invalid inode transid: has %llu expect [0, %llu]",
			       apfs_inode_transid(leaf, iitem), super_gen + 1);
		return -EUCLEAN;
	}

	/*
	 * For size and nbytes it's better not to be too strict, as for dir
	 * item its size/nbytes can easily get wrong, but doesn't affect
	 * anything in the fs. So here we skip the check.
	 */
	mode = apfs_inode_mode(leaf, iitem);
	if (unlikely(mode & ~valid_mask)) {
		inode_item_err(leaf, slot,
			       "unknown mode bit detected: 0x%x",
			       mode & ~valid_mask);
		return -EUCLEAN;
	}

	/*
	 * S_IFMT is not bit mapped so we can't completely rely on
	 * is_power_of_2/has_single_bit_set, but it can save us from checking
	 * FIFO/CHR/DIR/REG.  Only needs to check BLK, LNK and SOCKS
	 */
	if (!has_single_bit_set(mode & S_IFMT)) {
		if (unlikely(!S_ISLNK(mode) && !S_ISBLK(mode) && !S_ISSOCK(mode))) {
			inode_item_err(leaf, slot,
			"invalid mode: has 0%o expect valid S_IF* bit(s)",
				       mode & S_IFMT);
			return -EUCLEAN;
		}
	}
	if (unlikely(S_ISDIR(mode) && apfs_inode_nlink(leaf, iitem) > 1)) {
		inode_item_err(leaf, slot,
		       "invalid nlink: has %u expect no more than 1 for dir",
			apfs_inode_nlink(leaf, iitem));
		return -EUCLEAN;
	}
	if (unlikely(apfs_inode_flags(leaf, iitem) & ~APFS_INODE_FLAG_MASK)) {
		inode_item_err(leaf, slot,
			       "unknown flags detected: 0x%llx",
			       apfs_inode_flags(leaf, iitem) &
			       ~APFS_INODE_FLAG_MASK);
		return -EUCLEAN;
	}
	return 0;
}

static int check_root_item(struct extent_buffer *leaf, struct apfs_key *key,
			   int slot)
{
	struct apfs_fs_info *fs_info = leaf->fs_info;
	struct apfs_root_item ri = { 0 };
	const u64 valid_root_flags = APFS_ROOT_SUBVOL_RDONLY |
				     APFS_ROOT_SUBVOL_DEAD;
	int ret;

	ret = check_root_key(leaf, key, slot);
	if (unlikely(ret < 0))
		return ret;

	if (unlikely(apfs_item_size_nr(leaf, slot) != sizeof(ri) &&
		     apfs_item_size_nr(leaf, slot) !=
		     apfs_legacy_root_item_size())) {
		generic_err(leaf, slot,
			    "invalid root item size, have %u expect %zu or %u",
			    apfs_item_size_nr(leaf, slot), sizeof(ri),
			    apfs_legacy_root_item_size());
		return -EUCLEAN;
	}

	/*
	 * For legacy root item, the members starting at generation_v2 will be
	 * all filled with 0.
	 * And since we allow geneartion_v2 as 0, it will still pass the check.
	 */
	read_extent_buffer(leaf, &ri, apfs_item_ptr_offset(leaf, slot),
			   apfs_item_size_nr(leaf, slot));

	/* Generation related */
	if (unlikely(apfs_root_generation(&ri) >
		     apfs_super_generation(fs_info->super_copy) + 1)) {
		generic_err(leaf, slot,
			"invalid root generation, have %llu expect (0, %llu]",
			    apfs_root_generation(&ri),
			    apfs_super_generation(fs_info->super_copy) + 1);
		return -EUCLEAN;
	}
	if (unlikely(apfs_root_generation_v2(&ri) >
		     apfs_super_generation(fs_info->super_copy) + 1)) {
		generic_err(leaf, slot,
		"invalid root v2 generation, have %llu expect (0, %llu]",
			    apfs_root_generation_v2(&ri),
			    apfs_super_generation(fs_info->super_copy) + 1);
		return -EUCLEAN;
	}
	if (unlikely(apfs_root_last_snapshot(&ri) >
		     apfs_super_generation(fs_info->super_copy) + 1)) {
		generic_err(leaf, slot,
		"invalid root last_snapshot, have %llu expect (0, %llu]",
			    apfs_root_last_snapshot(&ri),
			    apfs_super_generation(fs_info->super_copy) + 1);
		return -EUCLEAN;
	}

	/* Alignment and level check */
	if (unlikely(!IS_ALIGNED(apfs_root_bytenr(&ri), fs_info->sectorsize))) {
		generic_err(leaf, slot,
		"invalid root bytenr, have %llu expect to be aligned to %u",
			    apfs_root_bytenr(&ri), fs_info->sectorsize);
		return -EUCLEAN;
	}
	if (unlikely(apfs_root_level(&ri) >= APFS_MAX_LEVEL)) {
		generic_err(leaf, slot,
			    "invalid root level, have %u expect [0, %u]",
			    apfs_root_level(&ri), APFS_MAX_LEVEL - 1);
		return -EUCLEAN;
	}
	if (unlikely(apfs_root_drop_level(&ri) >= APFS_MAX_LEVEL)) {
		generic_err(leaf, slot,
			    "invalid root level, have %u expect [0, %u]",
			    apfs_root_drop_level(&ri), APFS_MAX_LEVEL - 1);
		return -EUCLEAN;
	}

	/* Flags check */
	if (unlikely(apfs_root_flags(&ri) & ~valid_root_flags)) {
		generic_err(leaf, slot,
			    "invalid root flags, have 0x%llx expect mask 0x%llx",
			    apfs_root_flags(&ri), valid_root_flags);
		return -EUCLEAN;
	}
	return 0;
}

__printf(3,4)
__cold
static void extent_err(const struct extent_buffer *eb, int slot,
		       const char *fmt, ...)
{
	struct apfs_key key = {};
	struct va_format vaf;
	va_list args;
	u64 bytenr;
	u64 len;

	apfs_item_key_to_cpu(eb, &key, slot);
	bytenr = key.objectid;
	if (key.type == APFS_METADATA_ITEM_KEY ||
	    key.type == APFS_TREE_BLOCK_REF_KEY ||
	    key.type == APFS_SHARED_BLOCK_REF_KEY)
		len = eb->fs_info->nodesize;
	else
		len = key.offset;
	va_start(args, fmt);

	vaf.fmt = fmt;
	vaf.va = &args;

	apfs_crit(eb->fs_info,
	"corrupt %s: block=%llu slot=%d extent bytenr=%llu len=%llu %pV",
		apfs_header_level(eb) == 0 ? "leaf" : "node",
		eb->start, slot, bytenr, len, &vaf);
	va_end(args);
}

static int check_extent_item(struct extent_buffer *leaf,
			     struct apfs_key *key, int slot)
{
	struct apfs_fs_info *fs_info = leaf->fs_info;
	struct apfs_extent_item *ei;
	bool is_tree_block = false;
	unsigned long ptr;	/* Current pointer inside inline refs */
	unsigned long end;	/* Extent item end */
	const u32 item_size = apfs_item_size_nr(leaf, slot);
	u64 flags;
	u64 generation;
	u64 total_refs;		/* Total refs in apfs_extent_item */
	u64 inline_refs = 0;	/* found total inline refs */

	if (unlikely(key->type == APFS_METADATA_ITEM_KEY &&
		     !apfs_fs_incompat(fs_info, SKINNY_METADATA))) {
		generic_err(leaf, slot,
"invalid key type, METADATA_ITEM type invalid when SKINNY_METADATA feature disabled");
		return -EUCLEAN;
	}
	/* key->objectid is the bytenr for both key types */
	if (unlikely(!IS_ALIGNED(key->objectid, fs_info->sectorsize))) {
		generic_err(leaf, slot,
		"invalid key objectid, have %llu expect to be aligned to %u",
			   key->objectid, fs_info->sectorsize);
		return -EUCLEAN;
	}

	/* key->offset is tree level for METADATA_ITEM_KEY */
	if (unlikely(key->type == APFS_METADATA_ITEM_KEY &&
		     key->offset >= APFS_MAX_LEVEL)) {
		extent_err(leaf, slot,
			   "invalid tree level, have %llu expect [0, %u]",
			   key->offset, APFS_MAX_LEVEL - 1);
		return -EUCLEAN;
	}

	/*
	 * EXTENT/METADATA_ITEM consists of:
	 * 1) One apfs_extent_item
	 *    Records the total refs, type and generation of the extent.
	 *
	 * 2) One apfs_tree_block_info (for EXTENT_ITEM and tree backref only)
	 *    Records the first key and level of the tree block.
	 *
	 * 2) Zero or more apfs_extent_inline_ref(s)
	 *    Each inline ref has one apfs_extent_inline_ref shows:
	 *    2.1) The ref type, one of the 4
	 *         TREE_BLOCK_REF	Tree block only
	 *         SHARED_BLOCK_REF	Tree block only
	 *         EXTENT_DATA_REF	Data only
	 *         SHARED_DATA_REF	Data only
	 *    2.2) Ref type specific data
	 *         Either using apfs_extent_inline_ref::offset, or specific
	 *         data structure.
	 */
	if (unlikely(item_size < sizeof(*ei))) {
		extent_err(leaf, slot,
			   "invalid item size, have %u expect [%zu, %u)",
			   item_size, sizeof(*ei),
			   APFS_LEAF_DATA_SIZE(fs_info));
		return -EUCLEAN;
	}
	end = item_size + apfs_item_ptr_offset(leaf, slot);

	/* Checks against extent_item */
	ei = apfs_item_ptr(leaf, slot, struct apfs_extent_item);
	flags = apfs_extent_flags(leaf, ei);
	total_refs = apfs_extent_refs(leaf, ei);
	generation = apfs_extent_generation(leaf, ei);
	if (unlikely(generation >
		     apfs_super_generation(fs_info->super_copy) + 1)) {
		extent_err(leaf, slot,
			   "invalid generation, have %llu expect (0, %llu]",
			   generation,
			   apfs_super_generation(fs_info->super_copy) + 1);
		return -EUCLEAN;
	}
	if (unlikely(!has_single_bit_set(flags & (APFS_EXTENT_FLAG_DATA |
						  APFS_EXTENT_FLAG_TREE_BLOCK)))) {
		extent_err(leaf, slot,
		"invalid extent flag, have 0x%llx expect 1 bit set in 0x%llx",
			flags, APFS_EXTENT_FLAG_DATA |
			APFS_EXTENT_FLAG_TREE_BLOCK);
		return -EUCLEAN;
	}
	is_tree_block = !!(flags & APFS_EXTENT_FLAG_TREE_BLOCK);
	if (is_tree_block) {
		if (unlikely(key->type == APFS_EXTENT_ITEM_KEY &&
			     key->offset != fs_info->nodesize)) {
			extent_err(leaf, slot,
				   "invalid extent length, have %llu expect %u",
				   key->offset, fs_info->nodesize);
			return -EUCLEAN;
		}
	} else {
		if (unlikely(key->type != APFS_EXTENT_ITEM_KEY)) {
			extent_err(leaf, slot,
			"invalid key type, have %u expect %u for data backref",
				   key->type, APFS_EXTENT_ITEM_KEY);
			return -EUCLEAN;
		}
		if (unlikely(!IS_ALIGNED(key->offset, fs_info->sectorsize))) {
			extent_err(leaf, slot,
			"invalid extent length, have %llu expect aligned to %u",
				   key->offset, fs_info->sectorsize);
			return -EUCLEAN;
		}
		if (unlikely(flags & APFS_BLOCK_FLAG_FULL_BACKREF)) {
			extent_err(leaf, slot,
			"invalid extent flag, data has full backref set");
			return -EUCLEAN;
		}
	}
	ptr = (unsigned long)(struct apfs_extent_item *)(ei + 1);

	/* Check the special case of apfs_tree_block_info */
	if (is_tree_block && key->type != APFS_METADATA_ITEM_KEY) {
		struct apfs_tree_block_info *info;

		info = (struct apfs_tree_block_info *)ptr;
		if (unlikely(apfs_tree_block_level(leaf, info) >= APFS_MAX_LEVEL)) {
			extent_err(leaf, slot,
			"invalid tree block info level, have %u expect [0, %u]",
				   apfs_tree_block_level(leaf, info),
				   APFS_MAX_LEVEL - 1);
			return -EUCLEAN;
		}
		ptr = (unsigned long)(struct apfs_tree_block_info *)(info + 1);
	}

	/* Check inline refs */
	while (ptr < end) {
		struct apfs_extent_inline_ref *iref;
		struct apfs_extent_data_ref *dref;
		struct apfs_shared_data_ref *sref;
		u64 dref_offset;
		u64 inline_offset;
		u8 inline_type;

		if (unlikely(ptr + sizeof(*iref) > end)) {
			extent_err(leaf, slot,
"inline ref item overflows extent item, ptr %lu iref size %zu end %lu",
				   ptr, sizeof(*iref), end);
			return -EUCLEAN;
		}
		iref = (struct apfs_extent_inline_ref *)ptr;
		inline_type = apfs_extent_inline_ref_type(leaf, iref);
		inline_offset = apfs_extent_inline_ref_offset(leaf, iref);
		if (unlikely(ptr + apfs_extent_inline_ref_size(inline_type) > end)) {
			extent_err(leaf, slot,
"inline ref item overflows extent item, ptr %lu iref size %u end %lu",
				   ptr, inline_type, end);
			return -EUCLEAN;
		}

		switch (inline_type) {
		/* inline_offset is subvolid of the owner, no need to check */
		case APFS_TREE_BLOCK_REF_KEY:
			inline_refs++;
			break;
		/* Contains parent bytenr */
		case APFS_SHARED_BLOCK_REF_KEY:
			if (unlikely(!IS_ALIGNED(inline_offset,
						 fs_info->sectorsize))) {
				extent_err(leaf, slot,
		"invalid tree parent bytenr, have %llu expect aligned to %u",
					   inline_offset, fs_info->sectorsize);
				return -EUCLEAN;
			}
			inline_refs++;
			break;
		/*
		 * Contains owner subvolid, owner key objectid, adjusted offset.
		 * The only obvious corruption can happen in that offset.
		 */
		case APFS_EXTENT_DATA_REF_KEY:
			dref = (struct apfs_extent_data_ref *)(&iref->offset);
			dref_offset = apfs_extent_data_ref_offset(leaf, dref);
			if (unlikely(!IS_ALIGNED(dref_offset,
						 fs_info->sectorsize))) {
				extent_err(leaf, slot,
		"invalid data ref offset, have %llu expect aligned to %u",
					   dref_offset, fs_info->sectorsize);
				return -EUCLEAN;
			}
			inline_refs += apfs_extent_data_ref_count(leaf, dref);
			break;
		/* Contains parent bytenr and ref count */
		case APFS_SHARED_DATA_REF_KEY:
			sref = (struct apfs_shared_data_ref *)(iref + 1);
			if (unlikely(!IS_ALIGNED(inline_offset,
						 fs_info->sectorsize))) {
				extent_err(leaf, slot,
		"invalid data parent bytenr, have %llu expect aligned to %u",
					   inline_offset, fs_info->sectorsize);
				return -EUCLEAN;
			}
			inline_refs += apfs_shared_data_ref_count(leaf, sref);
			break;
		default:
			extent_err(leaf, slot, "unknown inline ref type: %u",
				   inline_type);
			return -EUCLEAN;
		}
		ptr += apfs_extent_inline_ref_size(inline_type);
	}
	/* No padding is allowed */
	if (unlikely(ptr != end)) {
		extent_err(leaf, slot,
			   "invalid extent item size, padding bytes found");
		return -EUCLEAN;
	}

	/* Finally, check the inline refs against total refs */
	if (unlikely(inline_refs > total_refs)) {
		extent_err(leaf, slot,
			"invalid extent refs, have %llu expect >= inline %llu",
			   total_refs, inline_refs);
		return -EUCLEAN;
	}
	return 0;
}

static int check_simple_keyed_refs(struct extent_buffer *leaf,
				   struct apfs_key *key, int slot)
{
	u32 expect_item_size = 0;

	if (key->type == APFS_SHARED_DATA_REF_KEY)
		expect_item_size = sizeof(struct apfs_shared_data_ref);

	if (unlikely(apfs_item_size_nr(leaf, slot) != expect_item_size)) {
		generic_err(leaf, slot,
		"invalid item size, have %u expect %u for key type %u",
			    apfs_item_size_nr(leaf, slot),
			    expect_item_size, key->type);
		return -EUCLEAN;
	}
	if (unlikely(!IS_ALIGNED(key->objectid, leaf->fs_info->sectorsize))) {
		generic_err(leaf, slot,
"invalid key objectid for shared block ref, have %llu expect aligned to %u",
			    key->objectid, leaf->fs_info->sectorsize);
		return -EUCLEAN;
	}
	if (unlikely(key->type != APFS_TREE_BLOCK_REF_KEY &&
		     !IS_ALIGNED(key->offset, leaf->fs_info->sectorsize))) {
		extent_err(leaf, slot,
		"invalid tree parent bytenr, have %llu expect aligned to %u",
			   key->offset, leaf->fs_info->sectorsize);
		return -EUCLEAN;
	}
	return 0;
}

static int check_extent_data_ref(struct extent_buffer *leaf,
				 struct apfs_key *key, int slot)
{
	struct apfs_extent_data_ref *dref;
	unsigned long ptr = apfs_item_ptr_offset(leaf, slot);
	const unsigned long end = ptr + apfs_item_size_nr(leaf, slot);

	if (unlikely(apfs_item_size_nr(leaf, slot) % sizeof(*dref) != 0)) {
		generic_err(leaf, slot,
	"invalid item size, have %u expect aligned to %zu for key type %u",
			    apfs_item_size_nr(leaf, slot),
			    sizeof(*dref), key->type);
		return -EUCLEAN;
	}
	if (unlikely(!IS_ALIGNED(key->objectid, leaf->fs_info->sectorsize))) {
		generic_err(leaf, slot,
"invalid key objectid for shared block ref, have %llu expect aligned to %u",
			    key->objectid, leaf->fs_info->sectorsize);
		return -EUCLEAN;
	}
	for (; ptr < end; ptr += sizeof(*dref)) {
		u64 offset;

		/*
		 * We cannot check the extent_data_ref hash due to possible
		 * overflow from the leaf due to hash collisions.
		 */
		dref = (struct apfs_extent_data_ref *)ptr;
		offset = apfs_extent_data_ref_offset(leaf, dref);
		if (unlikely(!IS_ALIGNED(offset, leaf->fs_info->sectorsize))) {
			extent_err(leaf, slot,
	"invalid extent data backref offset, have %llu expect aligned to %u",
				   offset, leaf->fs_info->sectorsize);
			return -EUCLEAN;
		}
	}
	return 0;
}

#define inode_ref_err(eb, slot, fmt, args...)			\
	inode_item_err(eb, slot, fmt, ##args)
static int check_inode_ref(struct extent_buffer *leaf,
			   struct apfs_key *key, struct apfs_key *prev_key,
			   int slot)
{
	struct apfs_inode_ref *iref;
	unsigned long ptr;
	unsigned long end;

	if (unlikely(!check_prev_ino(leaf, key, slot, prev_key)))
		return -EUCLEAN;
	/* namelen can't be 0, so item_size == sizeof() is also invalid */
	if (unlikely(apfs_item_size_nr(leaf, slot) <= sizeof(*iref))) {
		inode_ref_err(leaf, slot,
			"invalid item size, have %u expect (%zu, %u)",
			apfs_item_size_nr(leaf, slot),
			sizeof(*iref), APFS_LEAF_DATA_SIZE(leaf->fs_info));
		return -EUCLEAN;
	}

	ptr = apfs_item_ptr_offset(leaf, slot);
	end = ptr + apfs_item_size_nr(leaf, slot);
	while (ptr < end) {
		u16 namelen;

		if (unlikely(ptr + sizeof(iref) > end)) {
			inode_ref_err(leaf, slot,
			"inode ref overflow, ptr %lu end %lu inode_ref_size %zu",
				ptr, end, sizeof(iref));
			return -EUCLEAN;
		}

		iref = (struct apfs_inode_ref *)ptr;
		namelen = apfs_inode_ref_name_len(leaf, iref);
		if (unlikely(ptr + sizeof(*iref) + namelen > end)) {
			inode_ref_err(leaf, slot,
				"inode ref overflow, ptr %lu end %lu namelen %u",
				ptr, end, namelen);
			return -EUCLEAN;
		}

		/*
		 * NOTE: In theory we should record all found index numbers
		 * to find any duplicated indexes, but that will be too time
		 * consuming for inodes with too many hard links.
		 */
		ptr += sizeof(*iref) + namelen;
	}
	return 0;
}

/*
 * Common point to switch the item-specific validation.
 */
static int check_leaf_item(struct extent_buffer *leaf,
			   struct apfs_key *key, int slot,
			   struct apfs_key *prev_key)
{
	int ret = 0;
	struct apfs_chunk *chunk;

	switch (key->type) {
	case APFS_EXTENT_DATA_KEY:
		ret = check_extent_data_item(leaf, key, slot, prev_key);
		break;
	case APFS_EXTENT_CSUM_KEY:
		ret = check_csum_item(leaf, key, slot, prev_key);
		break;
	case APFS_DIR_ITEM_KEY:
	case APFS_DIR_INDEX_KEY:
	case APFS_XATTR_ITEM_KEY:
		ret = check_dir_item(leaf, key, prev_key, slot);
		break;
	case APFS_INODE_REF_KEY:
		ret = check_inode_ref(leaf, key, prev_key, slot);
		break;
	case APFS_BLOCK_GROUP_ITEM_KEY:
		ret = check_block_group_item(leaf, key, slot);
		break;
	case APFS_CHUNK_ITEM_KEY:
		chunk = apfs_item_ptr(leaf, slot, struct apfs_chunk);
		ret = check_leaf_chunk_item(leaf, chunk, key, slot);
		break;
	case APFS_DEV_ITEM_KEY:
		ret = check_dev_item(leaf, key, slot);
		break;
	case APFS_INODE_ITEM_KEY:
		ret = check_inode_item(leaf, key, slot);
		break;
	case APFS_ROOT_ITEM_KEY:
		ret = check_root_item(leaf, key, slot);
		break;
	case APFS_EXTENT_ITEM_KEY:
	case APFS_METADATA_ITEM_KEY:
		ret = check_extent_item(leaf, key, slot);
		break;
	case APFS_TREE_BLOCK_REF_KEY:
	case APFS_SHARED_DATA_REF_KEY:
	case APFS_SHARED_BLOCK_REF_KEY:
		ret = check_simple_keyed_refs(leaf, key, slot);
		break;
	case APFS_EXTENT_DATA_REF_KEY:
		ret = check_extent_data_ref(leaf, key, slot);
		break;
	}
	return ret;
}

static int check_leaf(struct extent_buffer *leaf, bool check_item_data)
{
	struct apfs_fs_info *fs_info = leaf->fs_info;
	/* No valid key type is 0, so all key should be larger than this key */
	struct apfs_key prev_key = {};
	struct apfs_key key = {};
	u32 nritems = apfs_header_nritems(leaf);
	int slot;

	return 0;

	if (unlikely(apfs_header_level(leaf) != 0)) {
		generic_err(leaf, 0,
			"invalid level for leaf, have %d expect 0",
			apfs_header_level(leaf));
		return -EUCLEAN;
	}

	/*
	 * Extent buffers from a relocation tree have a owner field that
	 * corresponds to the subvolume tree they are based on. So just from an
	 * extent buffer alone we can not find out what is the id of the
	 * corresponding subvolume tree, so we can not figure out if the extent
	 * buffer corresponds to the root of the relocation tree or not. So
	 * skip this check for relocation trees.
	 */
	if (nritems == 0 && !apfs_header_flag(leaf, APFS_HEADER_FLAG_RELOC)) {
		u64 owner = apfs_header_owner(leaf);

		/* These trees must never be empty */
		if (unlikely(owner == APFS_ROOT_TREE_OBJECTID ||
			     owner == APFS_CHUNK_TREE_OBJECTID ||
			     owner == APFS_EXTENT_TREE_OBJECTID ||
			     owner == APFS_DEV_TREE_OBJECTID ||
			     owner == APFS_FS_TREE_OBJECTID ||
			     owner == APFS_DATA_RELOC_TREE_OBJECTID)) {
			generic_err(leaf, 0,
			"invalid root, root %llu must never be empty",
				    owner);
			return -EUCLEAN;
		}
		/* Unknown tree */
		if (unlikely(owner == 0)) {
			generic_err(leaf, 0,
				"invalid owner, root 0 is not defined");
			return -EUCLEAN;
		}
		return 0;
	}

	if (unlikely(nritems == 0))
		return 0;

	/*
	 * Check the following things to make sure this is a good leaf, and
	 * leaf users won't need to bother with similar sanity checks:
	 *
	 * 1) key ordering
	 * 2) item offset and size
	 *    No overlap, no hole, all inside the leaf.
	 * 3) item content
	 *    If possible, do comprehensive sanity check.
	 *    NOTE: All checks must only rely on the item data itself.
	 */
	for (slot = 0; slot < nritems; slot++) {
		u32 item_end_expected;
		int ret;

		apfs_item_key_to_cpu(leaf, &key, slot);

		/* Make sure the keys are in the right order */
		if (unlikely(apfs_comp_cpu_keys(leaf, &prev_key, &key) >= 0)) {
			generic_err(leaf, slot,
	"bad key order, prev (%llu %u %llu) current (%llu %u %llu)",
				prev_key.objectid, prev_key.type,
				prev_key.offset, key.objectid, key.type,
				key.offset);
			return -EUCLEAN;
		}

		/*
		 * Make sure the offset and ends are right, remember that the
		 * item data starts at the end of the leaf and grows towards the
		 * front.
		 */
		if (slot == 0)
			item_end_expected = APFS_LEAF_DATA_SIZE(fs_info);
		else
			item_end_expected = apfs_item_offset_nr(leaf,
								 slot - 1);
		if (unlikely(apfs_item_end_nr(leaf, slot) != item_end_expected)) {
			generic_err(leaf, slot,
				"unexpected item end, have %u expect %u",
				apfs_item_end_nr(leaf, slot),
				item_end_expected);
			return -EUCLEAN;
		}

		/*
		 * Check to make sure that we don't point outside of the leaf,
		 * just in case all the items are consistent to each other, but
		 * all point outside of the leaf.
		 */
		if (unlikely(apfs_item_end_nr(leaf, slot) >
			     APFS_LEAF_DATA_SIZE(fs_info))) {
			generic_err(leaf, slot,
			"slot end outside of leaf, have %u expect range [0, %u]",
				apfs_item_end_nr(leaf, slot),
				APFS_LEAF_DATA_SIZE(fs_info));
			return -EUCLEAN;
		}

		if (check_item_data) {
			/*
			 * Check if the item size and content meet other
			 * criteria
			 */
			ret = check_leaf_item(leaf, &key, slot, &prev_key);
			if (unlikely(ret < 0))
				return ret;
		}

		prev_key.objectid = key.objectid;
		prev_key.type = key.type;
		prev_key.offset = key.offset;
	}

	return 0;
}

int apfs_check_leaf_full(struct extent_buffer *leaf)
{
	return check_leaf(leaf, true);
}
ALLOW_ERROR_INJECTION(apfs_check_leaf_full, ERRNO);

int apfs_check_leaf_relaxed(struct extent_buffer *leaf)
{
	return check_leaf(leaf, false);
}

int apfs_check_node(struct extent_buffer *node)
{
	struct apfs_fs_info *fs_info = node->fs_info;
	unsigned long nr = apfs_header_nritems(node);
	struct apfs_key key, next_key;
	int slot;
	int level = apfs_header_level(node);
	u64 bytenr;
	int ret = 0;

	return 0;

	if (unlikely(level <= 0 || level >= APFS_MAX_LEVEL)) {
		generic_err(node, 0,
			"invalid level for node, have %d expect [1, %d]",
			level, APFS_MAX_LEVEL - 1);
		return -EUCLEAN;
	}
	if (unlikely(nr == 0)) {
		apfs_crit(fs_info,
			  "corrupt node: root=%llu block=%llu, have 0 items",
			   apfs_header_owner(node), node->start);
		return -EUCLEAN;
	}

	for (slot = 0; slot < nr - 1; slot++) {
		bytenr = apfs_node_blockptr(node, slot);
		apfs_node_key_to_cpu(node, &key, slot);
		apfs_node_key_to_cpu(node, &next_key, slot + 1);

		if (unlikely(!bytenr)) {
			generic_err(node, slot,
				"invalid NULL node pointer");
			ret = -EUCLEAN;
			goto out;
		}
		if (unlikely(!IS_ALIGNED(bytenr, fs_info->sectorsize))) {
			generic_err(node, slot,
			"unaligned pointer, have %llu should be aligned to %u",
				bytenr, fs_info->sectorsize);
			ret = -EUCLEAN;
			goto out;
		}

		if (unlikely(apfs_comp_cpu_keys(node, &key, &next_key) >= 0)) {
			generic_err(node, slot,
	"bad key order, current (%llu %u %llu) next (%llu %u %llu)",
				key.objectid, key.type, key.offset,
				next_key.objectid, next_key.type,
				next_key.offset);
			ret = -EUCLEAN;
			goto out;
		}
	}
out:
	return ret;
}
ALLOW_ERROR_INJECTION(apfs_check_node, ERRNO);
