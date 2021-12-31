// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2007 Oracle.  All rights reserved.
 */

#include <linux/bio.h>
#include <linux/slab.h>
#include <linux/pagemap.h>
#include <linux/highmem.h>
#include <linux/sched/mm.h>
#include <linux/mm.h>

#include <crypto/hash.h>
#include "misc.h"
#include "ctree.h"
#include "disk-io.h"
#include "transaction.h"
#include "volumes.h"
#include "print-tree.h"
#include "compression.h"
#include "apfs_trace.h"

#define __MAX_CSUM_ITEMS(r, size) ((unsigned long)(((APFS_LEAF_DATA_SIZE(r) - \
				   sizeof(struct apfs_item) * 2) / \
				  size) - 1))

#define MAX_CSUM_ITEMS(r, size) (min_t(u32, __MAX_CSUM_ITEMS(r, size), \
				       PAGE_SIZE))

/**
 * Set inode's size according to filesystem options
 *
 * @inode:      inode we want to update the disk_i_size for
 * @new_i_size: i_size we want to set to, 0 if we use i_size
 *
 * With NO_HOLES set this simply sets the disk_is_size to whatever i_size_read()
 * returns as it is perfectly fine with a file that has holes without hole file
 * extent items.
 *
 * However without NO_HOLES we need to only return the area that is contiguous
 * from the 0 offset of the file.  Otherwise we could end up adjust i_size up
 * to an extent that has a gap in between.
 *
 * Finally new_i_size should only be set in the case of truncate where we're not
 * ready to use i_size_read() as the limiter yet.
 */
void apfs_inode_safe_disk_i_size_write(struct apfs_inode *inode, u64 new_i_size)
{
	struct apfs_fs_info *fs_info = inode->root->fs_info;
	u64 start, end, i_size;
	int ret;

	i_size = new_i_size ?: i_size_read(&inode->vfs_inode);
	if (apfs_fs_incompat(fs_info, NO_HOLES)) {
		inode->disk_i_size = i_size;
		return;
	}

	spin_lock(&inode->lock);
	ret = find_contiguous_extent_bit(&inode->file_extent_tree, 0, &start,
					 &end, EXTENT_DIRTY);
	if (!ret && start == 0)
		i_size = min(i_size, end + 1);
	else
		i_size = 0;
	inode->disk_i_size = i_size;
	spin_unlock(&inode->lock);
}

/**
 * Mark range within a file as having a new extent inserted
 *
 * @inode: inode being modified
 * @start: start file offset of the file extent we've inserted
 * @len:   logical length of the file extent item
 *
 * Call when we are inserting a new file extent where there was none before.
 * Does not need to call this in the case where we're replacing an existing file
 * extent, however if not sure it's fine to call this multiple times.
 *
 * The start and len must match the file extent item, so thus must be sectorsize
 * aligned.
 */
int apfs_inode_set_file_extent_range(struct apfs_inode *inode, u64 start,
				      u64 len)
{
	if (len == 0)
		return 0;

	ASSERT(IS_ALIGNED(start + len, inode->root->fs_info->sectorsize));

	if (apfs_fs_incompat(inode->root->fs_info, NO_HOLES))
		return 0;
	return 0;
}

/**
 * Marks an inode range as not having a backing extent
 *
 * @inode: inode being modified
 * @start: start file offset of the file extent we've inserted
 * @len:   logical length of the file extent item
 *
 * Called when we drop a file extent, for example when we truncate.  Doesn't
 * need to be called for cases where we're replacing a file extent, like when
 * we've COWed a file extent.
 *
 * The start and len must match the file extent item, so thus must be sectorsize
 * aligned.
 */
int apfs_inode_clear_file_extent_range(struct apfs_inode *inode, u64 start,
					u64 len)
{
	if (len == 0)
		return 0;

	ASSERT(IS_ALIGNED(start + len, inode->root->fs_info->sectorsize) ||
	       len == (u64)-1);

	if (apfs_fs_incompat(inode->root->fs_info, NO_HOLES))
		return 0;
	return clear_extent_bit(&inode->file_extent_tree, start,
				start + len - 1, EXTENT_DIRTY, 0, 0, NULL);
}

static inline u32 max_ordered_sum_bytes(struct apfs_fs_info *fs_info,
					u16 csum_size)
{
	u32 ncsums = (PAGE_SIZE - sizeof(struct apfs_ordered_sum)) / csum_size;

	return ncsums * fs_info->sectorsize;
}

int apfs_insert_file_extent(struct apfs_trans_handle *trans,
			     struct apfs_root *root,
			     u64 objectid, u64 pos,
			     u64 disk_offset, u64 disk_num_bytes,
			     u64 num_bytes, u64 offset, u64 ram_bytes,
			     u8 compression, u8 encryption, u16 other_encoding)
{
	int ret = 0;
	struct apfs_file_extent_item *item;
	struct apfs_key file_key = {};
	struct apfs_path *path;
	struct extent_buffer *leaf;

	path = apfs_alloc_path();
	if (!path)
		return -ENOMEM;
	file_key.objectid = objectid;
	file_key.offset = pos;
	file_key.type = APFS_EXTENT_DATA_KEY;

	ret = apfs_insert_empty_item(trans, root, path, &file_key,
				      sizeof(*item));
	if (ret < 0)
		goto out;
	BUG_ON(ret); /* Can't happen */
	leaf = path->nodes[0];
	item = apfs_item_ptr(leaf, path->slots[0],
			      struct apfs_file_extent_item);
	apfs_set_file_extent_disk_bytenr(leaf, item, disk_offset);
	apfs_set_file_extent_disk_num_bytes(leaf, item, disk_num_bytes);
	apfs_set_file_extent_offset(leaf, item, offset);
	apfs_set_file_extent_num_bytes(leaf, item, num_bytes);
	apfs_set_file_extent_ram_bytes(leaf, item, ram_bytes);
	apfs_set_file_extent_generation(leaf, item, trans->transid);
	apfs_set_file_extent_type(leaf, item, APFS_FILE_EXTENT_REG);
	apfs_set_file_extent_compression(leaf, item, compression);
	apfs_set_file_extent_encryption(leaf, item, encryption);
	apfs_set_file_extent_other_encoding(leaf, item, other_encoding);

	apfs_mark_buffer_dirty(leaf);
out:
	apfs_free_path(path);
	return ret;
}

static struct apfs_csum_item *
apfs_lookup_csum(struct apfs_trans_handle *trans,
		  struct apfs_root *root,
		  struct apfs_path *path,
		  u64 bytenr, int cow)
{
	struct apfs_fs_info *fs_info = root->fs_info;
	int ret;
	struct apfs_key file_key = {};
	struct apfs_key found_key = {};
	struct apfs_csum_item *item;
	struct extent_buffer *leaf;
	u64 csum_offset = 0;
	const u32 csum_size = fs_info->csum_size;
	int csums_in_item;

	file_key.objectid = APFS_EXTENT_CSUM_OBJECTID;
	file_key.offset = bytenr;
	file_key.type = APFS_EXTENT_CSUM_KEY;
	ret = apfs_search_slot(trans, root, &file_key, path, 0, cow);
	if (ret < 0)
		goto fail;
	leaf = path->nodes[0];
	if (ret > 0) {
		ret = 1;
		if (path->slots[0] == 0)
			goto fail;
		path->slots[0]--;
		apfs_item_key_to_cpu(leaf, &found_key, path->slots[0]);
		if (found_key.type != APFS_EXTENT_CSUM_KEY)
			goto fail;

		csum_offset = (bytenr - found_key.offset) >>
				fs_info->sectorsize_bits;
		csums_in_item = apfs_item_size_nr(leaf, path->slots[0]);
		csums_in_item /= csum_size;

		if (csum_offset == csums_in_item) {
			ret = -EFBIG;
			goto fail;
		} else if (csum_offset > csums_in_item) {
			goto fail;
		}
	}
	item = apfs_item_ptr(leaf, path->slots[0], struct apfs_csum_item);
	item = (struct apfs_csum_item *)((unsigned char *)item +
					  csum_offset * csum_size);
	return item;
fail:
	if (ret > 0)
		ret = -ENOENT;
	return ERR_PTR(ret);
}

int apfs_lookup_file_extent(struct apfs_trans_handle *trans,
			     struct apfs_root *root,
			     struct apfs_path *path, u64 objectid,
			     u64 offset, int mod)
{
	int ret;
	struct apfs_key file_key = {};
	int ins_len = mod < 0 ? -1 : 0;
	int cow = mod != 0;

	file_key.oid = objectid;
	file_key.offset = offset;
	file_key.type = APFS_TYPE_FILE_EXTENT;
	ret = apfs_search_slot(trans, root, &file_key, path, ins_len, cow);
	trace_printk("search file extent objectid %llu oid %llu type %u offset %llu eb %llu slot %d ret %d\n",
		     file_key.objectid, file_key.oid, file_key.type, file_key.offset,
		     path->nodes[0]->start, path->slots[0], ret);
	return ret;
}

/*
 * Find checksums for logical bytenr range [disk_bytenr, disk_bytenr + len) and
 * estore the result to @dst.
 *
 * Return >0 for the number of sectors we found.
 * Return 0 for the range [disk_bytenr, disk_bytenr + sectorsize) has no csum
 * for it. Caller may want to try next sector until one range is hit.
 * Return <0 for fatal error.
 */
static int search_csum_tree(struct apfs_fs_info *fs_info,
			    struct apfs_path *path, u64 disk_bytenr,
			    u64 len, u8 *dst)
{
	struct apfs_csum_item *item = NULL;
	struct apfs_key key = {};
	const u32 sectorsize = fs_info->sectorsize;
	const u32 csum_size = fs_info->csum_size;
	u32 itemsize;
	int ret;
	u64 csum_start;
	u64 csum_len;

	ASSERT(IS_ALIGNED(disk_bytenr, sectorsize) &&
	       IS_ALIGNED(len, sectorsize));

	/* Check if the current csum item covers disk_bytenr */
	if (path->nodes[0]) {
		item = apfs_item_ptr(path->nodes[0], path->slots[0],
				      struct apfs_csum_item);
		apfs_item_key_to_cpu(path->nodes[0], &key, path->slots[0]);
		itemsize = apfs_item_size_nr(path->nodes[0], path->slots[0]);

		csum_start = key.offset;
		csum_len = (itemsize / csum_size) * sectorsize;

		if (in_range(disk_bytenr, csum_start, csum_len))
			goto found;
	}

	/* Current item doesn't contain the desired range, search again */
	apfs_release_path(path);
	item = apfs_lookup_csum(NULL, fs_info->csum_root, path, disk_bytenr, 0);
	if (IS_ERR(item)) {
		ret = PTR_ERR(item);
		goto out;
	}
	apfs_item_key_to_cpu(path->nodes[0], &key, path->slots[0]);
	itemsize = apfs_item_size_nr(path->nodes[0], path->slots[0]);

	csum_start = key.offset;
	csum_len = (itemsize / csum_size) * sectorsize;
	ASSERT(in_range(disk_bytenr, csum_start, csum_len));

found:
	ret = (min(csum_start + csum_len, disk_bytenr + len) -
		   disk_bytenr) >> fs_info->sectorsize_bits;
	read_extent_buffer(path->nodes[0], dst, (unsigned long)item,
			ret * csum_size);
out:
	if (ret == -ENOENT)
		ret = 0;
	return ret;
}

/*
 * Locate the file_offset of @cur_disk_bytenr of a @bio.
 *
 * Bio of apfs represents read range of
 * [bi_sector << 9, bi_sector << 9 + bi_size).
 * Knowing this, we can iterate through each bvec to locate the page belong to
 * @cur_disk_bytenr and get the file offset.
 *
 * @inode is used to determine if the bvec page really belongs to @inode.
 *
 * Return 0 if we can't find the file offset
 * Return >0 if we find the file offset and restore it to @file_offset_ret
 */
static int search_file_offset_in_bio(struct bio *bio, struct inode *inode,
				     u64 disk_bytenr, u64 *file_offset_ret)
{
	struct bvec_iter iter;
	struct bio_vec bvec;
	u64 cur = bio->bi_iter.bi_sector << SECTOR_SHIFT;
	int ret = 0;

	bio_for_each_segment(bvec, bio, iter) {
		struct page *page = bvec.bv_page;

		if (cur > disk_bytenr)
			break;
		if (cur + bvec.bv_len <= disk_bytenr) {
			cur += bvec.bv_len;
			continue;
		}
		ASSERT(in_range(disk_bytenr, cur, bvec.bv_len));
		if (page->mapping && page->mapping->host &&
		    page->mapping->host == inode) {
			ret = 1;
			*file_offset_ret = page_offset(page) + bvec.bv_offset +
					   disk_bytenr - cur;
			break;
		}
	}
	return ret;
}

/**
 * Lookup the checksum for the read bio in csum tree.
 *
 * @inode: inode that the bio is for.
 * @bio: bio to look up.
 * @dst: Buffer of size nblocks * apfs_super_csum_size() used to return
 *       checksum (nblocks = bio->bi_iter.bi_size / fs_info->sectorsize). If
 *       NULL, the checksum buffer is allocated and returned in
 *       apfs_io_bio(bio)->csum instead.
 *
 * Return: BLK_STS_RESOURCE if allocating memory fails, BLK_STS_OK otherwise.
 */
blk_status_t apfs_lookup_bio_sums(struct inode *inode, struct bio *bio, u8 *dst)
{
	struct apfs_fs_info *fs_info = apfs_sb(inode->i_sb);
	struct extent_io_tree *io_tree = &APFS_I(inode)->io_tree;
	struct apfs_path *path;
	const u32 sectorsize = fs_info->sectorsize;
	const u32 csum_size = fs_info->csum_size;
	u32 orig_len = bio->bi_iter.bi_size;
	u64 orig_disk_bytenr = bio->bi_iter.bi_sector << SECTOR_SHIFT;
	u64 cur_disk_bytenr;
	u8 *csum;
	const unsigned int nblocks = orig_len >> fs_info->sectorsize_bits;
	int count = 0;

	if (!fs_info->csum_root || (APFS_I(inode)->flags & APFS_INODE_NODATASUM))
		return BLK_STS_OK;

	/*
	 * This function is only called for read bio.
	 *
	 * This means two things:
	 * - All our csums should only be in csum tree
	 *   No ordered extents csums, as ordered extents are only for write
	 *   path.
	 * - No need to bother any other info from bvec
	 *   Since we're looking up csums, the only important info is the
	 *   disk_bytenr and the length, which can be extracted from bi_iter
	 *   directly.
	 */
	ASSERT(bio_op(bio) == REQ_OP_READ);
	path = apfs_alloc_path();
	if (!path)
		return BLK_STS_RESOURCE;

	if (!dst) {
		struct apfs_io_bio *apfs_bio = apfs_io_bio(bio);

		if (nblocks * csum_size > APFS_BIO_INLINE_CSUM_SIZE) {
			apfs_bio->csum = kmalloc_array(nblocks, csum_size,
							GFP_NOFS);
			if (!apfs_bio->csum) {
				apfs_free_path(path);
				return BLK_STS_RESOURCE;
			}
		} else {
			apfs_bio->csum = apfs_bio->csum_inline;
		}
		csum = apfs_bio->csum;
	} else {
		csum = dst;
	}

	/*
	 * If requested number of sectors is larger than one leaf can contain,
	 * kick the readahead for csum tree.
	 */
	if (nblocks > fs_info->csums_per_leaf)
		path->reada = READA_FORWARD;

	/*
	 * the free space stuff is only read when it hasn't been
	 * updated in the current transaction.  So, we can safely
	 * read from the commit root and sidestep a nasty deadlock
	 * between reading the free space cache and updating the csum tree.
	 */
	if (apfs_is_free_space_inode(APFS_I(inode))) {
		path->search_commit_root = 1;
		path->skip_locking = 1;
	}

	for (cur_disk_bytenr = orig_disk_bytenr;
	     cur_disk_bytenr < orig_disk_bytenr + orig_len;
	     cur_disk_bytenr += (count * sectorsize)) {
		u64 search_len = orig_disk_bytenr + orig_len - cur_disk_bytenr;
		unsigned int sector_offset;
		u8 *csum_dst;

		/*
		 * Although both cur_disk_bytenr and orig_disk_bytenr is u64,
		 * we're calculating the offset to the bio start.
		 *
		 * Bio size is limited to UINT_MAX, thus unsigned int is large
		 * enough to contain the raw result, not to mention the right
		 * shifted result.
		 */
		ASSERT(cur_disk_bytenr - orig_disk_bytenr < UINT_MAX);
		sector_offset = (cur_disk_bytenr - orig_disk_bytenr) >>
				fs_info->sectorsize_bits;
		csum_dst = csum + sector_offset * csum_size;

		count = search_csum_tree(fs_info, path, cur_disk_bytenr,
					 search_len, csum_dst);
		if (count <= 0) {
			/*
			 * Either we hit a critical error or we didn't find
			 * the csum.
			 * Either way, we put zero into the csums dst, and skip
			 * to the next sector.
			 */
			memset(csum_dst, 0, csum_size);
			count = 1;

			/*
			 * For data reloc inode, we need to mark the range
			 * NODATASUM so that balance won't report false csum
			 * error.
			 */
			if (APFS_I(inode)->root->root_key.objectid ==
			    APFS_DATA_RELOC_TREE_OBJECTID) {
				u64 file_offset;
				int ret;

				ret = search_file_offset_in_bio(bio, inode,
						cur_disk_bytenr, &file_offset);
				if (ret)
					set_extent_bits(io_tree, file_offset,
						file_offset + sectorsize - 1,
						EXTENT_NODATASUM);
			} else {
				apfs_warn_rl(fs_info,
			"csum hole found for disk bytenr range [%llu, %llu)",
				cur_disk_bytenr, cur_disk_bytenr + sectorsize);
			}
		}
	}

	apfs_free_path(path);
	return BLK_STS_OK;
}

int apfs_lookup_csums_range(struct apfs_root *root, u64 start, u64 end,
			     struct list_head *list, int search_commit)
{
	struct apfs_fs_info *fs_info = root->fs_info;
	struct apfs_key key = {};
	struct apfs_path *path;
	struct extent_buffer *leaf;
	struct apfs_ordered_sum *sums;
	struct apfs_csum_item *item;
	LIST_HEAD(tmplist);
	unsigned long offset;
	int ret;
	size_t size;
	u64 csum_end;
	const u32 csum_size = fs_info->csum_size;

	ASSERT(IS_ALIGNED(start, fs_info->sectorsize) &&
	       IS_ALIGNED(end + 1, fs_info->sectorsize));

	path = apfs_alloc_path();
	if (!path)
		return -ENOMEM;

	if (search_commit) {
		path->skip_locking = 1;
		path->reada = READA_FORWARD;
		path->search_commit_root = 1;
	}

	key.objectid = APFS_EXTENT_CSUM_OBJECTID;
	key.offset = start;
	key.type = APFS_EXTENT_CSUM_KEY;

	ret = apfs_search_slot(NULL, root, &key, path, 0, 0);
	if (ret < 0)
		goto fail;
	if (ret > 0 && path->slots[0] > 0) {
		leaf = path->nodes[0];
		apfs_item_key_to_cpu(leaf, &key, path->slots[0] - 1);
		if (key.objectid == APFS_EXTENT_CSUM_OBJECTID &&
		    key.type == APFS_EXTENT_CSUM_KEY) {
			offset = (start - key.offset) >> fs_info->sectorsize_bits;
			if (offset * csum_size <
			    apfs_item_size_nr(leaf, path->slots[0] - 1))
				path->slots[0]--;
		}
	}

	while (start <= end) {
		leaf = path->nodes[0];
		if (path->slots[0] >= apfs_header_nritems(leaf)) {
			ret = apfs_next_leaf(root, path);
			if (ret < 0)
				goto fail;
			if (ret > 0)
				break;
			leaf = path->nodes[0];
		}

		apfs_item_key_to_cpu(leaf, &key, path->slots[0]);
		if (key.objectid != APFS_EXTENT_CSUM_OBJECTID ||
		    key.type != APFS_EXTENT_CSUM_KEY ||
		    key.offset > end)
			break;

		if (key.offset > start)
			start = key.offset;

		size = apfs_item_size_nr(leaf, path->slots[0]);
		csum_end = key.offset + (size / csum_size) * fs_info->sectorsize;
		if (csum_end <= start) {
			path->slots[0]++;
			continue;
		}

		csum_end = min(csum_end, end + 1);
		item = apfs_item_ptr(path->nodes[0], path->slots[0],
				      struct apfs_csum_item);
		while (start < csum_end) {
			size = min_t(size_t, csum_end - start,
				     max_ordered_sum_bytes(fs_info, csum_size));
			sums = kzalloc(apfs_ordered_sum_size(fs_info, size),
				       GFP_NOFS);
			if (!sums) {
				ret = -ENOMEM;
				goto fail;
			}

			sums->bytenr = start;
			sums->len = (int)size;

			offset = (start - key.offset) >> fs_info->sectorsize_bits;
			offset *= csum_size;
			size >>= fs_info->sectorsize_bits;

			read_extent_buffer(path->nodes[0],
					   sums->sums,
					   ((unsigned long)item) + offset,
					   csum_size * size);

			start += fs_info->sectorsize * size;
			list_add_tail(&sums->list, &tmplist);
		}
		path->slots[0]++;
	}
	ret = 0;
fail:
	while (ret < 0 && !list_empty(&tmplist)) {
		sums = list_entry(tmplist.next, struct apfs_ordered_sum, list);
		list_del(&sums->list);
		kfree(sums);
	}
	list_splice_tail(&tmplist, list);

	apfs_free_path(path);
	return ret;
}

/*
 * apfs_csum_one_bio - Calculates checksums of the data contained inside a bio
 * @inode:	 Owner of the data inside the bio
 * @bio:	 Contains the data to be checksummed
 * @file_start:  offset in file this bio begins to describe
 * @contig:	 Boolean. If true/1 means all bio vecs in this bio are
 *		 contiguous and they begin at @file_start in the file. False/0
 *		 means this bio can contain potentially discontiguous bio vecs
 *		 so the logical offset of each should be calculated separately.
 */
blk_status_t apfs_csum_one_bio(struct apfs_inode *inode, struct bio *bio,
		       u64 file_start, int contig)
{
	struct apfs_fs_info *fs_info = inode->root->fs_info;
	SHASH_DESC_ON_STACK(shash, fs_info->csum_shash);
	struct apfs_ordered_sum *sums;
	struct apfs_ordered_extent *ordered = NULL;
	char *data;
	struct bvec_iter iter;
	struct bio_vec bvec;
	int index;
	int nr_sectors;
	unsigned long total_bytes = 0;
	unsigned long this_sum_bytes = 0;
	int i;
	u64 offset;
	unsigned nofs_flag;

	nofs_flag = memalloc_nofs_save();
	sums = kvzalloc(apfs_ordered_sum_size(fs_info, bio->bi_iter.bi_size),
		       GFP_KERNEL);
	memalloc_nofs_restore(nofs_flag);

	if (!sums)
		return BLK_STS_RESOURCE;

	sums->len = bio->bi_iter.bi_size;
	INIT_LIST_HEAD(&sums->list);

	if (contig)
		offset = file_start;
	else
		offset = 0; /* shut up gcc */

	sums->bytenr = bio->bi_iter.bi_sector << 9;
	index = 0;

	shash->tfm = fs_info->csum_shash;

	bio_for_each_segment(bvec, bio, iter) {
		if (!contig)
			offset = page_offset(bvec.bv_page) + bvec.bv_offset;

		if (!ordered) {
			ordered = apfs_lookup_ordered_extent(inode, offset);
			BUG_ON(!ordered); /* Logic error */
		}

		nr_sectors = APFS_BYTES_TO_BLKS(fs_info,
						 bvec.bv_len + fs_info->sectorsize
						 - 1);

		for (i = 0; i < nr_sectors; i++) {
			if (offset >= ordered->file_offset + ordered->num_bytes ||
			    offset < ordered->file_offset) {
				unsigned long bytes_left;

				sums->len = this_sum_bytes;
				this_sum_bytes = 0;
				apfs_add_ordered_sum(ordered, sums);
				apfs_put_ordered_extent(ordered);

				bytes_left = bio->bi_iter.bi_size - total_bytes;

				nofs_flag = memalloc_nofs_save();
				sums = kvzalloc(apfs_ordered_sum_size(fs_info,
						      bytes_left), GFP_KERNEL);
				memalloc_nofs_restore(nofs_flag);
				BUG_ON(!sums); /* -ENOMEM */
				sums->len = bytes_left;
				ordered = apfs_lookup_ordered_extent(inode,
								offset);
				ASSERT(ordered); /* Logic error */
				sums->bytenr = (bio->bi_iter.bi_sector << 9)
					+ total_bytes;
				index = 0;
			}

			data = kmap_atomic(bvec.bv_page);
			crypto_shash_digest(shash, data + bvec.bv_offset
					    + (i * fs_info->sectorsize),
					    fs_info->sectorsize,
					    sums->sums + index);
			kunmap_atomic(data);
			index += fs_info->csum_size;
			offset += fs_info->sectorsize;
			this_sum_bytes += fs_info->sectorsize;
			total_bytes += fs_info->sectorsize;
		}

	}
	this_sum_bytes = 0;
	apfs_add_ordered_sum(ordered, sums);
	apfs_put_ordered_extent(ordered);
	return 0;
}

/*
 * helper function for csum removal, this expects the
 * key to describe the csum pointed to by the path, and it expects
 * the csum to overlap the range [bytenr, len]
 *
 * The csum should not be entirely contained in the range and the
 * range should not be entirely contained in the csum.
 *
 * This calls apfs_truncate_item with the correct args based on the
 * overlap, and fixes up the key as required.
 */
static noinline void truncate_one_csum(struct apfs_fs_info *fs_info,
				       struct apfs_path *path,
				       struct apfs_key *key,
				       u64 bytenr, u64 len)
{
	struct extent_buffer *leaf;
	const u32 csum_size = fs_info->csum_size;
	u64 csum_end;
	u64 end_byte = bytenr + len;
	u32 blocksize_bits = fs_info->sectorsize_bits;

	leaf = path->nodes[0];
	csum_end = apfs_item_size_nr(leaf, path->slots[0]) / csum_size;
	csum_end <<= blocksize_bits;
	csum_end += key->offset;

	if (key->offset < bytenr && csum_end <= end_byte) {
		/*
		 *         [ bytenr - len ]
		 *         [   ]
		 *   [csum     ]
		 *   A simple truncate off the end of the item
		 */
		u32 new_size = (bytenr - key->offset) >> blocksize_bits;
		new_size *= csum_size;
		apfs_truncate_item(path, new_size, 1);
	} else if (key->offset >= bytenr && csum_end > end_byte &&
		   end_byte > key->offset) {
		/*
		 *         [ bytenr - len ]
		 *                 [ ]
		 *                 [csum     ]
		 * we need to truncate from the beginning of the csum
		 */
		u32 new_size = (csum_end - end_byte) >> blocksize_bits;
		new_size *= csum_size;

		apfs_truncate_item(path, new_size, 0);

		key->offset = end_byte;
		apfs_set_item_key_safe(fs_info, path, key);
	} else {
		BUG();
	}
}

/*
 * deletes the csum items from the csum tree for a given
 * range of bytes.
 */
int apfs_del_csums(struct apfs_trans_handle *trans,
		    struct apfs_root *root, u64 bytenr, u64 len)
{
	struct apfs_fs_info *fs_info = trans->fs_info;
	struct apfs_path *path;
	struct apfs_key key = {};
	u64 end_byte = bytenr + len;
	u64 csum_end;
	struct extent_buffer *leaf;
	int ret = 0;
	const u32 csum_size = fs_info->csum_size;
	u32 blocksize_bits = fs_info->sectorsize_bits;

	ASSERT(root == fs_info->csum_root ||
	       root->root_key.objectid == APFS_TREE_LOG_OBJECTID);

	path = apfs_alloc_path();
	if (!path)
		return -ENOMEM;

	while (1) {
		key.objectid = APFS_EXTENT_CSUM_OBJECTID;
		key.offset = end_byte - 1;
		key.type = APFS_EXTENT_CSUM_KEY;

		ret = apfs_search_slot(trans, root, &key, path, -1, 1);
		if (ret > 0) {
			ret = 0;
			if (path->slots[0] == 0)
				break;
			path->slots[0]--;
		} else if (ret < 0) {
			break;
		}

		leaf = path->nodes[0];
		apfs_item_key_to_cpu(leaf, &key, path->slots[0]);

		if (key.objectid != APFS_EXTENT_CSUM_OBJECTID ||
		    key.type != APFS_EXTENT_CSUM_KEY) {
			break;
		}

		if (key.offset >= end_byte)
			break;

		csum_end = apfs_item_size_nr(leaf, path->slots[0]) / csum_size;
		csum_end <<= blocksize_bits;
		csum_end += key.offset;

		/* this csum ends before we start, we're done */
		if (csum_end <= bytenr)
			break;

		/* delete the entire item, it is inside our range */
		if (key.offset >= bytenr && csum_end <= end_byte) {
			int del_nr = 1;

			/*
			 * Check how many csum items preceding this one in this
			 * leaf correspond to our range and then delete them all
			 * at once.
			 */
			if (key.offset > bytenr && path->slots[0] > 0) {
				int slot = path->slots[0] - 1;

				while (slot >= 0) {
					struct apfs_key pk = {};

					apfs_item_key_to_cpu(leaf, &pk, slot);
					if (pk.offset < bytenr ||
					    pk.type != APFS_EXTENT_CSUM_KEY ||
					    pk.objectid !=
					    APFS_EXTENT_CSUM_OBJECTID)
						break;
					path->slots[0] = slot;
					del_nr++;
					key.offset = pk.offset;
					slot--;
				}
			}
			ret = apfs_del_items(trans, root, path,
					      path->slots[0], del_nr);
			if (ret)
				break;
			if (key.offset == bytenr)
				break;
		} else if (key.offset < bytenr && csum_end > end_byte) {
			unsigned long offset;
			unsigned long shift_len;
			unsigned long item_offset;
			/*
			 *        [ bytenr - len ]
			 *     [csum                ]
			 *
			 * Our bytes are in the middle of the csum,
			 * we need to split this item and insert a new one.
			 *
			 * But we can't drop the path because the
			 * csum could change, get removed, extended etc.
			 *
			 * The trick here is the max size of a csum item leaves
			 * enough room in the tree block for a single
			 * item header.  So, we split the item in place,
			 * adding a new header pointing to the existing
			 * bytes.  Then we loop around again and we have
			 * a nicely formed csum item that we can neatly
			 * truncate.
			 */
			offset = (bytenr - key.offset) >> blocksize_bits;
			offset *= csum_size;

			shift_len = (len >> blocksize_bits) * csum_size;

			item_offset = apfs_item_ptr_offset(leaf,
							    path->slots[0]);

			memzero_extent_buffer(leaf, item_offset + offset,
					     shift_len);
			key.offset = bytenr;

			/*
			 * apfs_split_item returns -EAGAIN when the
			 * item changed size or key
			 */
			ret = apfs_split_item(trans, root, path, &key, offset);
			if (ret && ret != -EAGAIN) {
				apfs_abort_transaction(trans, ret);
				break;
			}
			ret = 0;

			key.offset = end_byte - 1;
		} else {
			truncate_one_csum(fs_info, path, &key, bytenr, len);
			if (key.offset < bytenr)
				break;
		}
		apfs_release_path(path);
	}
	apfs_free_path(path);
	return ret;
}

static int find_next_csum_offset(struct apfs_root *root,
				 struct apfs_path *path,
				 u64 *next_offset)
{
	const u32 nritems = apfs_header_nritems(path->nodes[0]);
	struct apfs_key found_key = {};
	int slot = path->slots[0] + 1;
	int ret;

	if (nritems == 0 || slot >= nritems) {
		ret = apfs_next_leaf(root, path);
		if (ret < 0) {
			return ret;
		} else if (ret > 0) {
			*next_offset = (u64)-1;
			return 0;
		}
		slot = path->slots[0];
	}

	apfs_item_key_to_cpu(path->nodes[0], &found_key, slot);

	if (found_key.objectid != APFS_EXTENT_CSUM_OBJECTID ||
	    found_key.type != APFS_EXTENT_CSUM_KEY)
		*next_offset = (u64)-1;
	else
		*next_offset = found_key.offset;

	return 0;
}

int apfs_csum_file_blocks(struct apfs_trans_handle *trans,
			   struct apfs_root *root,
			   struct apfs_ordered_sum *sums)
{
	struct apfs_fs_info *fs_info = root->fs_info;
	struct apfs_key file_key = {};
	struct apfs_key found_key = {};
	struct apfs_path *path;
	struct apfs_csum_item *item;
	struct apfs_csum_item *item_end;
	struct extent_buffer *leaf = NULL;
	u64 next_offset;
	u64 total_bytes = 0;
	u64 csum_offset;
	u64 bytenr;
	u32 ins_size;
	int index = 0;
	int found_next;
	int ret;
	const u32 csum_size = fs_info->csum_size;

	path = apfs_alloc_path();
	if (!path)
		return -ENOMEM;
again:
	next_offset = (u64)-1;
	found_next = 0;
	bytenr = sums->bytenr + total_bytes;
	file_key.objectid = APFS_EXTENT_CSUM_OBJECTID;
	file_key.offset = bytenr;
	file_key.type = APFS_EXTENT_CSUM_KEY;

	item = apfs_lookup_csum(trans, root, path, bytenr, 1);
	if (!IS_ERR(item)) {
		ret = 0;
		leaf = path->nodes[0];
		item_end = apfs_item_ptr(leaf, path->slots[0],
					  struct apfs_csum_item);
		item_end = (struct apfs_csum_item *)((char *)item_end +
			   apfs_item_size_nr(leaf, path->slots[0]));
		goto found;
	}
	ret = PTR_ERR(item);
	if (ret != -EFBIG && ret != -ENOENT)
		goto out;

	if (ret == -EFBIG) {
		u32 item_size;
		/* we found one, but it isn't big enough yet */
		leaf = path->nodes[0];
		item_size = apfs_item_size_nr(leaf, path->slots[0]);
		if ((item_size / csum_size) >=
		    MAX_CSUM_ITEMS(fs_info, csum_size)) {
			/* already at max size, make a new one */
			goto insert;
		}
	} else {
		/* We didn't find a csum item, insert one. */
		ret = find_next_csum_offset(root, path, &next_offset);
		if (ret < 0)
			goto out;
		found_next = 1;
		goto insert;
	}

	/*
	 * At this point, we know the tree has a checksum item that ends at an
	 * offset matching the start of the checksum range we want to insert.
	 * We try to extend that item as much as possible and then add as many
	 * checksums to it as they fit.
	 *
	 * First check if the leaf has enough free space for at least one
	 * checksum. If it has go directly to the item extension code, otherwise
	 * release the path and do a search for insertion before the extension.
	 */
	if (apfs_leaf_free_space(leaf) >= csum_size) {
		apfs_item_key_to_cpu(leaf, &found_key, path->slots[0]);
		csum_offset = (bytenr - found_key.offset) >>
			fs_info->sectorsize_bits;
		goto extend_csum;
	}

	apfs_release_path(path);
	path->search_for_extension = 1;
	ret = apfs_search_slot(trans, root, &file_key, path,
				csum_size, 1);
	path->search_for_extension = 0;
	if (ret < 0)
		goto out;

	if (ret > 0) {
		if (path->slots[0] == 0)
			goto insert;
		path->slots[0]--;
	}

	leaf = path->nodes[0];
	apfs_item_key_to_cpu(leaf, &found_key, path->slots[0]);
	csum_offset = (bytenr - found_key.offset) >> fs_info->sectorsize_bits;

	if (found_key.type != APFS_EXTENT_CSUM_KEY ||
	    found_key.objectid != APFS_EXTENT_CSUM_OBJECTID ||
	    csum_offset >= MAX_CSUM_ITEMS(fs_info, csum_size)) {
		goto insert;
	}

extend_csum:
	if (csum_offset == apfs_item_size_nr(leaf, path->slots[0]) /
	    csum_size) {
		int extend_nr;
		u64 tmp;
		u32 diff;

		tmp = sums->len - total_bytes;
		tmp >>= fs_info->sectorsize_bits;
		WARN_ON(tmp < 1);
		extend_nr = max_t(int, 1, tmp);

		/*
		 * A log tree can already have checksum items with a subset of
		 * the checksums we are trying to log. This can happen after
		 * doing a sequence of partial writes into prealloc extents and
		 * fsyncs in between, with a full fsync logging a larger subrange
		 * of an extent for which a previous fast fsync logged a smaller
		 * subrange. And this happens in particular due to merging file
		 * extent items when we complete an ordered extent for a range
		 * covered by a prealloc extent - this is done at
		 * apfs_mark_extent_written().
		 *
		 * So if we try to extend the previous checksum item, which has
		 * a range that ends at the start of the range we want to insert,
		 * make sure we don't extend beyond the start offset of the next
		 * checksum item. If we are at the last item in the leaf, then
		 * forget the optimization of extending and add a new checksum
		 * item - it is not worth the complexity of releasing the path,
		 * getting the first key for the next leaf, repeat the btree
		 * search, etc, because log trees are temporary anyway and it
		 * would only save a few bytes of leaf space.
		 */
		if (root->root_key.objectid == APFS_TREE_LOG_OBJECTID) {
			if (path->slots[0] + 1 >=
			    apfs_header_nritems(path->nodes[0])) {
				ret = find_next_csum_offset(root, path, &next_offset);
				if (ret < 0)
					goto out;
				found_next = 1;
				goto insert;
			}

			ret = find_next_csum_offset(root, path, &next_offset);
			if (ret < 0)
				goto out;

			tmp = (next_offset - bytenr) >> fs_info->sectorsize_bits;
			if (tmp <= INT_MAX)
				extend_nr = min_t(int, extend_nr, tmp);
		}

		diff = (csum_offset + extend_nr) * csum_size;
		diff = min(diff,
			   MAX_CSUM_ITEMS(fs_info, csum_size) * csum_size);

		diff = diff - apfs_item_size_nr(leaf, path->slots[0]);
		diff = min_t(u32, apfs_leaf_free_space(leaf), diff);
		diff /= csum_size;
		diff *= csum_size;

		apfs_extend_item(path, diff);
		ret = 0;
		goto csum;
	}

insert:
	apfs_release_path(path);
	csum_offset = 0;
	if (found_next) {
		u64 tmp;

		tmp = sums->len - total_bytes;
		tmp >>= fs_info->sectorsize_bits;
		tmp = min(tmp, (next_offset - file_key.offset) >>
					 fs_info->sectorsize_bits);

		tmp = max_t(u64, 1, tmp);
		tmp = min_t(u64, tmp, MAX_CSUM_ITEMS(fs_info, csum_size));
		ins_size = csum_size * tmp;
	} else {
		ins_size = csum_size;
	}
	ret = apfs_insert_empty_item(trans, root, path, &file_key,
				      ins_size);
	if (ret < 0)
		goto out;
	if (WARN_ON(ret != 0))
		goto out;
	leaf = path->nodes[0];
csum:
	item = apfs_item_ptr(leaf, path->slots[0], struct apfs_csum_item);
	item_end = (struct apfs_csum_item *)((unsigned char *)item +
				      apfs_item_size_nr(leaf, path->slots[0]));
	item = (struct apfs_csum_item *)((unsigned char *)item +
					  csum_offset * csum_size);
found:
	ins_size = (u32)(sums->len - total_bytes) >> fs_info->sectorsize_bits;
	ins_size *= csum_size;
	ins_size = min_t(u32, (unsigned long)item_end - (unsigned long)item,
			      ins_size);
	write_extent_buffer(leaf, sums->sums + index, (unsigned long)item,
			    ins_size);

	index += ins_size;
	ins_size /= csum_size;
	total_bytes += ins_size * fs_info->sectorsize;

	apfs_mark_buffer_dirty(path->nodes[0]);
	if (total_bytes < sums->len) {
		apfs_release_path(path);
		cond_resched();
		goto again;
	}
out:
	apfs_free_path(path);
	return ret;
}

/*
 * This function reads first block size from the extent, parses
 * offset maps and inserts extent maps into inode extent tree
 *
 * TODO: If start is located in first block, cache it.
 *
 */

static struct extent_map *
lzfse_extent_item_to_extent_map(struct apfs_inode *inode,
				const struct apfs_path *path,
				u64 start, u64 len)
{
	struct apfs_fs_info *fs_info = inode->root->fs_info;
	struct apfs_file_extent_val *fi;
	u64 bytenr;
	int ret;
	struct page *page = NULL;
	u32 *entry;
	struct address_space *mapping = fs_info->device->bdev->bd_inode->i_mapping;
	struct extent_map *em;
	struct apfs_key key = {};
	u32 entry_nr;
	u64 entry_bytenr;
	struct extent_map_tree *em_tree = &inode->extent_tree;

	em = alloc_extent_map();
	if (em == NULL) {
		ret = -ENOMEM;
		goto out;
	}

	fi = apfs_item_ptr(path->nodes[0], path->slots[0],
			   struct apfs_file_extent_val);
	apfs_item_key_to_cpu(path->nodes[0], &key, path->slots[0]);

	ASSERT(key.oid == inode->cid);
	ASSERT(key.type == APFS_TYPE_FILE_EXTENT);
	ASSERT(key.offset == 0);

	bytenr = apfs_file_extent_bno(path->nodes[0], fi)
		<< fs_info->block_size_bits;
	entry_nr = DIV_ROUND_DOWN_ULL(start - key.offset, APFS_MAX_UNCOMPRESSED);
	entry_bytenr = bytenr + entry_nr * sizeof(*entry);

	trace_printk("base bytenr %llu entry bytenr %llu entrynr %u\n",
			bytenr, entry_bytenr, entry_nr);
	entry = apfs_read_cache_page_unaligned(mapping, entry_bytenr, &page);
	if (IS_ERR(entry))  {
		ret = PTR_ERR(entry);
		goto out;
	}

	set_bit(EXTENT_FLAG_COMPRESSED, &em->flags);
	em->compress_type = inode->prop_compress;
	em->block_start = bytenr + *entry;
	em->start = ALIGN_DOWN(start, APFS_MAX_UNCOMPRESSED);
	em->orig_start = key.offset + ALIGN_DOWN(start, APFS_MAX_UNCOMPRESSED);
	em->offset = *entry;

	em->len = APFS_MAX_COMPRESSED;

	if ((entry_bytenr % PAGE_SIZE) + sizeof(*entry) >= PAGE_SIZE) {
		kunmap(page);
		put_page(page);
		page = NULL;
		entry_nr++;
		entry = apfs_read_cache_page_unaligned(mapping,
			       bytenr + entry_nr * sizeof(*entry), &page);
		if (IS_ERR(entry))  {
			ret = PTR_ERR(entry);
			goto out;
		}
	} else {
		entry++;
	}

	em->block_len = *entry - em->offset;
	kunmap(page);
	put_page(page);
	page = NULL;

	BUG_ON(em->block_len >= APFS_MAX_COMPRESSED + 4096);
	/* do not forget the last em */
	if (em->orig_start == ALIGN_DOWN(i_size_read(&inode->vfs_inode), APFS_MAX_UNCOMPRESSED))
	    em->len = i_size_read(&inode->vfs_inode) %  APFS_MAX_UNCOMPRESSED;
	else
		em->len = APFS_MAX_UNCOMPRESSED;

	write_lock(&em_tree->lock);

	trace_printk("add extent map em start %llu len %llu orig_start %llu block start %llu block len %llu em offset %llu\n",
			em->start, em->len, em->orig_start, em->block_start, em->block_len,
			em->offset);

	ret = apfs_add_extent_mapping(fs_info, em_tree, &em,
				      em->start, em->len);
	BUG_ON(ret);
	write_unlock(&em_tree->lock);

	return em;

out:
	free_extent_map(em);
	if (page) {
		kunmap(page);
		put_page(page);
	}
	apfs_err(fs_info,
		 "failed to map extent inode %llu: %d\n", apfs_ino(inode), ret);
	return ERR_PTR(ret);
}

/*
 * This function reads first block size from the extent, parses
 * offset maps and inserts extent maps into inode extent tree
 *
 * TODO: If start is located in first block, cache it.
 *
 */
static noinline struct extent_map *
zlib_extent_item_to_extent_map(struct apfs_inode *inode,
			       const struct apfs_path *path,
			       u64 start, u64 len)

{
	struct apfs_fs_info *fs_info = inode->root->fs_info;
	struct apfs_file_extent_val *fi;
	u64 bytenr;
	int ret;
	struct page *page = NULL;
	u32 nr_ems;
	struct address_space *mapping = fs_info->device->bdev->bd_inode->i_mapping;
	struct extent_map *em;
	struct apfs_key key = { };
	struct extent_map_tree *em_tree = &inode->extent_tree;
	struct apfs_resource_fork_header hdr;
	struct apfs_resource_fork_data fork_data;
	struct apfs_resource_fork_entries entries;
	u64 data_offset;
	u64 disk_bytenr;
	u32 first_entry_offset;
	u32 pg_offset;
	u32 entry_nr;
	void *kaddr;
	struct apfs_resource_fork_entry *entry;

	ASSERT(len <= APFS_MAX_UNCOMPRESSED);

	em = alloc_extent_map();
	if (em == NULL) {
		ret = -ENOMEM;
		goto out;
	}

	fi = apfs_item_ptr(path->nodes[0], path->slots[0],
			   struct apfs_file_extent_val);
	apfs_item_key_to_cpu(path->nodes[0], &key, path->slots[0]);

	ASSERT(key.oid == inode->cid);
	ASSERT(key.type == APFS_TYPE_FILE_EXTENT);
	ASSERT(key.offset == 0);

	bytenr = apfs_file_extent_bno(path->nodes[0], fi)
		<< fs_info->block_size_bits;

	kaddr = apfs_read_cache_page_unaligned(mapping, bytenr, &page);
	if (IS_ERR(kaddr))  {
		ret = PTR_ERR(kaddr);
		goto out;
	}
	memcpy(&hdr, kaddr, sizeof(hdr));

	data_offset = apfs_resource_fork_data_offset(&hdr);

	/* we do not care about the fork_data at all */
	data_offset += sizeof(fork_data);
	bytenr = bytenr + data_offset;

//	if (data_offset >= PAGE_SIZE ||
//	    data_offset + sizeof(entries) > PAGE_SIZE) {
		kunmap(page);
		put_page(page);
		page = NULL;
		kaddr = apfs_read_cache_page_unaligned(mapping, bytenr, &page);
		if (IS_ERR(kaddr))  {
			ret = PTR_ERR(kaddr);
			goto out;
		}
//	}

	ASSERT(data_offset % PAGE_SIZE + sizeof(entries) <= PAGE_SIZE);
	memcpy(&entries, kaddr, sizeof(entries));

	kunmap(page);
	put_page(page);
	page = NULL;

	nr_ems = apfs_resource_fork_entries_count(&entries);

	first_entry_offset = data_offset + sizeof(entries);
	entry_nr = DIV_ROUND_DOWN_ULL(start - key.offset, APFS_MAX_UNCOMPRESSED);

	ASSERT(entry_nr < nr_ems);

	disk_bytenr = bytenr + sizeof(struct apfs_resource_fork_entries) +
		entry_nr * sizeof(*entry);

	page = read_cache_page_gfp(mapping,  disk_bytenr >> PAGE_SHIFT,
				   GFP_NOFS);

	if (IS_ERR(page))  {
		ret = PTR_ERR(page);
		goto out;
	}

	pg_offset = (first_entry_offset + entry_nr * sizeof(*entry)) % PAGE_SIZE;

	entry = kmap(page) + pg_offset;

	set_bit(EXTENT_FLAG_COMPRESSED, &em->flags);
	em->compress_type = inode->prop_compress;
	em->block_len = apfs_resource_fork_entry_size(entry);
	em->start = key.offset + ALIGN_DOWN(start, APFS_MAX_UNCOMPRESSED);
	em->orig_start = key.offset + ALIGN_DOWN(start, APFS_MAX_UNCOMPRESSED);
	em->offset = apfs_resource_fork_entry_off(entry) + data_offset;
	em->block_start = bytenr + apfs_resource_fork_entry_off(entry);

	kunmap(page);
	put_page(page);
	page = NULL;

	if (em->block_len >= APFS_MAX_UNCOMPRESSED + 4096) {
		apfs_err(fs_info, "invalid extent map em start %llu len %llu orig_start %llu block start %llu block len %llu offset %llu\n",
			 em->start, em->len, em->orig_start, em->block_start,
			 em->block_len,
			 em->offset - data_offset);
		ret = -EUCLEAN;
		goto out;
	 }

	/* do not forget the last em */
	if (em->orig_start == ALIGN_DOWN(i_size_read(&inode->vfs_inode), APFS_MAX_UNCOMPRESSED))
	    em->len = i_size_read(&inode->vfs_inode) %  APFS_MAX_UNCOMPRESSED;
	else
		em->len = APFS_MAX_COMPRESSED;

	write_lock(&em_tree->lock);

	trace_printk("add extent map em start %llu len %llu orig_start %llu block start %llu block len %llu offset %llu\n",
		     em->start, em->len, em->orig_start, em->block_start,
		     em->block_len, em->offset - data_offset);

	ret = apfs_add_extent_mapping(fs_info, em_tree, &em,
				      em->start, em->len);
	BUG_ON(ret);
	write_unlock(&em_tree->lock);

	return em;

out:
	free_extent_map(em);
	trace_printk("failed to map extent inode %llu: %d\n", apfs_ino(inode), ret);
	return ERR_PTR(ret);
}

/*
 * This function reads first block size from the extent, parses
 * offset maps and inserts extent maps into inode extent tree
 *
 * TODO: If start is located in first block, cache it.
 *
 */
static noinline struct extent_map *
dstream_extent_item_to_extent_map(struct apfs_inode *inode,
			       const struct apfs_path *path,
			       u64 start, u64 len)

{
	struct apfs_fs_info *fs_info = inode->root->fs_info;
	struct apfs_file_extent_val *fi;
	u64 bytenr;
	int ret;
	struct extent_map *em;
	struct apfs_key key = { };
	struct extent_map_tree *em_tree = &inode->extent_tree;
	u64 data_offset;

	ASSERT(len <= APFS_MAX_UNCOMPRESSED);

	em = alloc_extent_map();
	if (em == NULL) {
		ret = -ENOMEM;
		goto out;
	}

	fi = apfs_item_ptr(path->nodes[0], path->slots[0],
			   struct apfs_file_extent_val);
	apfs_item_key_to_cpu(path->nodes[0], &key, path->slots[0]);

	ASSERT(key.oid == inode->cid);
	ASSERT(key.type == APFS_TYPE_FILE_EXTENT);
	ASSERT(key.offset == 0);

	data_offset = sizeof(struct apfs_compress_header);
	bytenr = apfs_file_extent_bno(path->nodes[0], fi)
		<< fs_info->block_size_bits;

	set_bit(EXTENT_FLAG_COMPRESSED, &em->flags);
	em->compress_type = inode->prop_compress;
	em->block_len = inode->disk_i_size;
	em->start = key.offset + ALIGN_DOWN(start, APFS_MAX_UNCOMPRESSED);
	em->orig_start = key.offset + ALIGN_DOWN(start, APFS_MAX_UNCOMPRESSED);
	em->offset = data_offset;
	em->block_start = bytenr + data_offset;
	em->len = i_size_read(&inode->vfs_inode);

	if (em->block_len >= APFS_MAX_UNCOMPRESSED + 4096) {
		apfs_err(fs_info, "invalid extent map em start %llu len %llu orig_start %llu block start %llu block len %llu offset %llu\n",
			 em->start, em->len, em->orig_start, em->block_start,
			 em->block_len,
			 em->offset - data_offset);
		ret = -EUCLEAN;
		goto out;
	 }

	write_lock(&em_tree->lock);

	trace_printk("add extent map em start %llu len %llu orig_start %llu block start %llu block len %llu offset %llu\n",
		     em->start, em->len, em->orig_start, em->block_start,
		     em->block_len, em->offset - data_offset);

	ret = apfs_add_extent_mapping(fs_info, em_tree, &em,
				      em->start, em->len);
	BUG_ON(ret);
	write_unlock(&em_tree->lock);

	return em;

out:
	free_extent_map(em);
	trace_printk("failed to map extent inode %llu: %d\n", apfs_ino(inode), ret);
	return ERR_PTR(ret);
}


struct extent_map *
apfs_compressed_extent_item_to_extent_map(struct apfs_inode *inode,
				     const struct apfs_path *path,
				     struct page *page,
				     u64 start, u64 len)
{
	int type = inode->prop_compress;

	/* compress header is in data stream block, alog is not known yet */
	if (apfs_inode_data_in_dstream(inode))
		return dstream_extent_item_to_extent_map(inode, path, start,
							 len);

	switch (type) {
	case APFS_COMPRESS_ZLIB_RSRC:
		return zlib_extent_item_to_extent_map(inode, path, start, len);
	case APFS_COMPRESS_LZFSE_RSRC:
	case APFS_COMPRESS_LZVN_RSRC:
		return lzfse_extent_item_to_extent_map(inode, path, start, len);
	default:
			BUG();
	}
}

int apfs_extent_item_to_extent_map(struct apfs_inode *inode,
				    const struct apfs_path *path,
				    struct page *page,
				    struct extent_map **emp,
				    u64 start, u64 len)
{
	struct extent_buffer *leaf = path->nodes[0];
	const int slot = path->slots[0];
	struct apfs_key key = {};
	struct extent_map *em = *emp;
	struct apfs_file_extent_val *fi;
	u64 extent_start, extent_end;
	u64 bytenr;
	int compress_type = inode->prop_compress;
	int ret;

	fi = apfs_item_ptr(leaf, slot, struct apfs_file_extent_val);
	apfs_item_key_to_cpu(leaf, &key, slot);
	extent_start = key.offset;
	extent_end = apfs_file_extent_end(path);
	em->ram_bytes = apfs_file_extent_raw_bytes(leaf, fi);

	em->start = extent_start;
	em->len = extent_end - extent_start;
	em->orig_start = extent_start;
	em->orig_block_len = apfs_file_extent_raw_bytes(leaf, fi);

	bytenr = apfs_file_extent_bno(leaf, fi) <<
		inode->root->fs_info->block_size_bits;

	if (bytenr == 0) {
		em->block_start = EXTENT_MAP_HOLE;
		return 0;
	}

	if (compress_type != APFS_COMPRESS_NONE &&
	    compress_type != APFS_COMPRESS_PLAIN_ATTR &&
	    compress_type != APFS_COMPRESS_PLAIN_RSRC) {
		BUG();
	} else {
		em->block_start = bytenr;
		em->block_len = em->len;
		ret = 0;
	}

	return ret;
}

/*
 * Returns the end offset (non inclusive) of the file extent item the given path
 * points to. If it points to an inline extent, the returned offset is rounded
 * up to the sector size.
 */
u64 apfs_file_extent_end(const struct apfs_path *path)
{
	const struct extent_buffer *leaf = path->nodes[0];
	const int slot = path->slots[0];
	struct apfs_file_extent_val *fi;
	struct apfs_key key = {};

	apfs_item_key_to_cpu(leaf, &key, slot);
	ASSERT(key.type == APFS_TYPE_FILE_EXTENT);
	fi = apfs_item_ptr(leaf, slot, struct apfs_file_extent_val);

	return key.offset + apfs_file_extent_len(leaf, fi);
}
