// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2007 Oracle.  All rights reserved.
 */

#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/radix-tree.h>
#include <linux/writeback.h>
#include <linux/workqueue.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/migrate.h>
#include <linux/ratelimit.h>
#include <linux/uuid.h>
#include <linux/semaphore.h>
#include <linux/error-injection.h>
#include <linux/crc32c.h>
#include <linux/sched/mm.h>
#include <asm/unaligned.h>
#include <crypto/hash.h>
#include "ctree.h"
#include "disk-io.h"
#include "transaction.h"
#include "apfs_inode.h"
#include "volumes.h"
#include "print-tree.h"
#include "locking.h"
#include "tree-log.h"
#include "free-space-cache.h"
#include "free-space-tree.h"
#include "check-integrity.h"
#include "rcu-string.h"
#include "dev-replace.h"
#include "raid56.h"
#include "sysfs.h"
#include "qgroup.h"
#include "compression.h"
#include "tree-checker.h"
#include "ref-verify.h"
#include "block-group.h"
#include "discard.h"
#include "space-info.h"
#include "zoned.h"
#include "subpage.h"
#include "apfs_trace.h"
#include "apfs_buf.h"

#define APFS_SUPER_FLAG_SUPP	(APFS_HEADER_FLAG_WRITTEN |\
				 APFS_HEADER_FLAG_RELOC |\
				 APFS_SUPER_FLAG_ERROR |\
				 APFS_SUPER_FLAG_SEEDING |\
				 APFS_SUPER_FLAG_METADUMP |\
				 APFS_SUPER_FLAG_METADUMP_V2)

static void end_workqueue_fn(struct apfs_work *work);
static void apfs_destroy_ordered_extents(struct apfs_root *root);
static int apfs_destroy_delayed_refs(struct apfs_transaction *trans,
				      struct apfs_fs_info *fs_info);
static void apfs_destroy_delalloc_inodes(struct apfs_root *root);
static int apfs_destroy_marked_extents(struct apfs_fs_info *fs_info,
					struct extent_io_tree *dirty_pages,
					int mark);
static int apfs_destroy_pinned_extent(struct apfs_fs_info *fs_info,
				       struct extent_io_tree *pinned_extents);
static int apfs_cleanup_transaction(struct apfs_fs_info *fs_info);
static void apfs_error_commit_super(struct apfs_fs_info *fs_info);

/*
 * apfs_end_io_wq structs are used to do processing in task context when an IO
 * is complete.  This is used during reads to verify checksums, and it is used
 * by writes to insert metadata for new file extents after IO is complete.
 */
struct apfs_end_io_wq {
	struct bio *bio;
	bio_end_io_t *end_io;
	void *private;
	struct apfs_fs_info *info;
	blk_status_t status;
	enum apfs_wq_endio_type metadata;
	struct apfs_work work;
};

static struct kmem_cache *apfs_end_io_wq_cache;

int __init apfs_end_io_wq_init(void)
{
	apfs_end_io_wq_cache = kmem_cache_create("apfs_end_io_wq",
					sizeof(struct apfs_end_io_wq),
					0,
					SLAB_MEM_SPREAD,
					NULL);
	if (!apfs_end_io_wq_cache)
		return -ENOMEM;
	return 0;
}

void __cold apfs_end_io_wq_exit(void)
{
	kmem_cache_destroy(apfs_end_io_wq_cache);
}

static void apfs_free_csum_hash(struct apfs_fs_info *fs_info)
{
	if (fs_info->csum_shash)
		crypto_free_shash(fs_info->csum_shash);
}

/*
 * async submit bios are used to offload expensive checksumming
 * onto the worker threads.  They checksum file and metadata bios
 * just before they are sent down the IO stack.
 */
struct async_submit_bio {
	struct inode *inode;
	struct bio *bio;
	extent_submit_bio_start_t *submit_bio_start;
	int mirror_num;

	/* Optional parameter for submit_bio_start used by direct io */
	u64 dio_file_offset;
	struct apfs_work work;
	blk_status_t status;
};

/*
 * Lockdep class keys for extent_buffer->lock's in this root.  For a given
 * eb, the lockdep key is determined by the apfs_root it belongs to and
 * the level the eb occupies in the tree.
 *
 * Different roots are used for different purposes and may nest inside each
 * other and they require separate keysets.  As lockdep keys should be
 * static, assign keysets according to the purpose of the root as indicated
 * by apfs_root->root_key.objectid.  This ensures that all special purpose
 * roots have separate keysets.
 *
 * Lock-nesting across peer nodes is always done with the immediate parent
 * node locked thus preventing deadlock.  As lockdep doesn't know this, use
 * subclass to avoid triggering lockdep warning in such cases.
 *
 * The key is set by the readpage_end_io_hook after the buffer has passed
 * csum validation but before the pages are unlocked.  It is also set by
 * apfs_init_new_buffer on freshly allocated blocks.
 *
 * We also add a check to make sure the highest level of the tree is the
 * same as our lockdep setup here.  If APFS_MAX_LEVEL changes, this code
 * needs update as well.
 */
#ifdef CONFIG_DEBUG_LOCK_ALLOC
# if APFS_MAX_LEVEL != 8
#  error
# endif

#define DEFINE_LEVEL(stem, level)					\
	.names[level] = "apfs-" stem "-0" #level,

#define DEFINE_NAME(stem)						\
	DEFINE_LEVEL(stem, 0)						\
	DEFINE_LEVEL(stem, 1)						\
	DEFINE_LEVEL(stem, 2)						\
	DEFINE_LEVEL(stem, 3)						\
	DEFINE_LEVEL(stem, 4)						\
	DEFINE_LEVEL(stem, 5)						\
	DEFINE_LEVEL(stem, 6)						\
	DEFINE_LEVEL(stem, 7)

static struct apfs_lockdep_keyset {
	u64			id;		/* root objectid */
	/* Longest entry: apfs-free-space-00 */
	char			names[APFS_MAX_LEVEL][20];
	struct lock_class_key	keys[APFS_MAX_LEVEL];
} apfs_lockdep_keysets[] = {
	{ .id = APFS_ROOT_TREE_OBJECTID,	DEFINE_NAME("root")	},
	{ .id = APFS_EXTENT_TREE_OBJECTID,	DEFINE_NAME("extent")	},
	{ .id = APFS_CHUNK_TREE_OBJECTID,	DEFINE_NAME("chunk")	},
	{ .id = APFS_DEV_TREE_OBJECTID,	DEFINE_NAME("dev")	},
	{ .id = APFS_CSUM_TREE_OBJECTID,	DEFINE_NAME("csum")	},
	{ .id = APFS_QUOTA_TREE_OBJECTID,	DEFINE_NAME("quota")	},
	{ .id = APFS_TREE_LOG_OBJECTID,	DEFINE_NAME("log")	},
	{ .id = APFS_TREE_RELOC_OBJECTID,	DEFINE_NAME("treloc")	},
	{ .id = APFS_DATA_RELOC_TREE_OBJECTID,	DEFINE_NAME("dreloc")	},
	{ .id = APFS_UUID_TREE_OBJECTID,	DEFINE_NAME("uuid")	},
	{ .id = APFS_FREE_SPACE_TREE_OBJECTID,	DEFINE_NAME("free-space") },
	{ .id = 0,				DEFINE_NAME("tree")	},
};

#undef DEFINE_LEVEL
#undef DEFINE_NAME

void apfs_set_buffer_lockdep_class(u64 objectid, struct extent_buffer *eb,
				    int level)
{
	struct apfs_lockdep_keyset *ks;

	BUG_ON(level >= ARRAY_SIZE(ks->keys));

	/* find the matching keyset, id 0 is the default entry */
	for (ks = apfs_lockdep_keysets; ks->id; ks++)
		if (ks->id == objectid)
			break;

	lockdep_set_class_and_name(&eb->lock,
				   &ks->keys[level], ks->names[level]);
}

#endif

static void csum_tree_block(struct extent_buffer *buf, u8 *result)
{
	const char *ptr;
	u64 csum;

	ptr = page_address(buf->pages[0]) + offset_in_page(buf->start);
	csum = apfs_generate_csum(ptr + APFS_CSUM_SIZE,
				  buf->len - APFS_CSUM_SIZE);

	memcpy(result, &csum, APFS_CSUM_SIZE);
}
/*
 * Compute the csum of a btree block and store the result to provided buffer.
 */
static void __csum_tree_block(struct extent_buffer *buf, u8 *result)
{
	struct apfs_fs_info *fs_info = buf->fs_info;
	const int num_pages = num_extent_pages(buf);
	const int first_page_part = min_t(u32, PAGE_SIZE, fs_info->nodesize);
	SHASH_DESC_ON_STACK(shash, fs_info->csum_shash);
	char *kaddr;
	int i;

	shash->tfm = fs_info->csum_shash;
	crypto_shash_init(shash);
	kaddr = page_address(buf->pages[0]) + offset_in_page(buf->start);
	crypto_shash_update(shash, kaddr + APFS_CSUM_SIZE,
			    first_page_part - APFS_CSUM_SIZE);

	for (i = 1; i < num_pages; i++) {
		kaddr = page_address(buf->pages[i]);
		crypto_shash_update(shash, kaddr, PAGE_SIZE);
	}
	memset(result, 0, APFS_CSUM_SIZE);
	crypto_shash_final(shash, result);
}

/*
 * we can't consider a given block up to date unless the transid of the
 * block matches the transid in the parent node's pointer.  This is how we
 * detect blocks that either didn't get written at all or got written
 * in the wrong place.
 */
static int verify_parent_transid(struct extent_io_tree *io_tree,
				 struct extent_buffer *eb, u64 parent_transid,
				 int atomic)
{
	return 0;
}

static bool apfs_supported_super_csum(u16 csum_type)
{
	switch (csum_type) {
	case APFS_CSUM_TYPE_CRC32:
	case APFS_CSUM_TYPE_XXHASH:
	case APFS_CSUM_TYPE_SHA256:
	case APFS_CSUM_TYPE_BLAKE2:
		return true;
	default:
		return false;
	}
}

/*
 * Return 0 if the superblock checksum type matches the checksum value of that
 * algorithm. Pass the raw disk superblock data.
 */
static int apfs_check_super_csum(struct apfs_fs_info *fs_info,
				  char *raw_disk_sb)
{
	struct apfs_super_block *disk_sb =
		(struct apfs_super_block *)raw_disk_sb;
	char result[APFS_CSUM_SIZE];
	SHASH_DESC_ON_STACK(shash, fs_info->csum_shash);

	shash->tfm = fs_info->csum_shash;

	/*
	 * The super_block structure does not span the whole
	 * APFS_SUPER_INFO_SIZE range, we expect that the unused space is
	 * filled with zeros and is included in the checksum.
	 */
	crypto_shash_digest(shash, raw_disk_sb + APFS_CSUM_SIZE,
			    APFS_SUPER_INFO_SIZE - APFS_CSUM_SIZE, result);

	if (memcmp(disk_sb->csum, result, fs_info->csum_size))
		return 1;

	return 0;
}

int apfs_verify_level_key(struct extent_buffer *eb, int level,
			   struct apfs_key *first_key, u64 parent_transid)
{
	struct apfs_fs_info *fs_info = eb->fs_info;
	int found_level;
	struct apfs_key found_key = {};
	int ret;

	found_level = apfs_header_level(eb);
	if (found_level != level) {
		WARN(IS_ENABLED(CONFIG_APFS_DEBUG),
		     KERN_ERR "APFS: tree level check failed\n");
		apfs_err(fs_info,
"tree level mismatch detected, bytenr=%llu level expected=%u has=%u",
			  eb->start, level, found_level);
		return -EIO;
	}

	if (!first_key)
		return 0;

	/*
	 * For live tree block (new tree blocks in current transaction),
	 * we need proper lock context to avoid race, which is impossible here.
	 * So we only checks tree blocks which is read from disk, whose
	 * generation <= fs_info->last_trans_committed.
	 */
	if (apfs_header_generation(eb) > fs_info->last_trans_committed)
		return 0;

	/* We have @first_key, so this @eb must have at least one item */
	if (apfs_header_nritems(eb) == 0) {
		apfs_err(fs_info,
		"invalid tree nritems, bytenr=%llu nritems=0 expect >0",
			  eb->start);
		WARN_ON(IS_ENABLED(CONFIG_APFS_DEBUG));
		return -EUCLEAN;
	}

	if (found_level)
		apfs_node_key_to_cpu(eb, &found_key, 0);
	else
		apfs_item_key_to_cpu(eb, &found_key, 0);
	ret = apfs_comp_cpu_keys(eb, first_key, &found_key);
	if (ret) {
		ret = -EUCLEAN;
		WARN(IS_ENABLED(CONFIG_APFS_DEBUG),
		     KERN_ERR "APFS: tree first key check failed\n");
		apfs_err(fs_info,
"tree first key mismatch detected, bytenr=%llu parent_transid=%llu key expected=(%llu,%u,%llu) has=(%llu,%u,%llu)",
			  eb->start, parent_transid, first_key->objectid,
			  first_key->type, first_key->offset,
			  found_key.objectid, found_key.type,
			  found_key.offset);
	}
	return ret;
}

/*
 * helper to read a given tree block, doing retries as required when
 * the checksums don't match and we have alternate mirrors to try.
 *
 * @parent_transid:	expected transid, skip check if 0
 * @level:		expected level, mandatory check if not -1
 * @first_key:		expected key of first slot, skip check if NULL
 */
static int btree_read_extent_buffer_pages(struct extent_buffer *eb,
					  u64 parent_transid, int level,
					  struct apfs_key *first_key)
{
	struct apfs_fs_info *fs_info = eb->fs_info;
	struct extent_io_tree *io_tree;
	int ret;
	int mirror_num = 0;

	io_tree = &APFS_I(fs_info->btree_inode)->io_tree;

	clear_bit(EXTENT_BUFFER_CORRUPT, &eb->bflags);
	ret = read_extent_buffer_pages(eb, WAIT_COMPLETE, mirror_num);
	if (!ret) {
		if (verify_parent_transid(io_tree, eb,
					  parent_transid, 0))
			ret = -EIO;
		else if (level != -1)
			ret = apfs_verify_level_key(eb, level,
						    first_key, parent_transid);
	}

	return ret;
}

static int csum_one_extent_buffer(struct extent_buffer *eb)
{
	struct apfs_fs_info *fs_info = eb->fs_info;
	u8 result[APFS_CSUM_SIZE];
	int ret;

	ASSERT(memcmp_extent_buffer(eb, fs_info->fs_devices->metadata_uuid,
				    offsetof(struct apfs_header, fsid),
				    APFS_FSID_SIZE) == 0);
	csum_tree_block(eb, result);

	if (apfs_header_level(eb))
		ret = apfs_check_node(eb);
	else
		ret = apfs_check_leaf_full(eb);

	if (ret < 0) {
		apfs_print_tree(eb, 0);
		apfs_err(fs_info,
			"block=%llu write time tree block corruption detected",
			eb->start);
		WARN_ON(IS_ENABLED(CONFIG_APFS_DEBUG));
		return ret;
	}
	write_extent_buffer(eb, result, 0, fs_info->csum_size);

	return 0;
}

/* Checksum all dirty extent buffers in one bio_vec */
static int csum_dirty_subpage_buffers(struct apfs_fs_info *fs_info,
				      struct bio_vec *bvec)
{
	struct page *page = bvec->bv_page;
	u64 bvec_start = page_offset(page) + bvec->bv_offset;
	u64 cur;
	int ret = 0;

	for (cur = bvec_start; cur < bvec_start + bvec->bv_len;
	     cur += fs_info->nodesize) {
		struct extent_buffer *eb;
		bool uptodate;

		eb = find_extent_buffer(fs_info, cur);
		uptodate = apfs_subpage_test_uptodate(fs_info, page, cur,
						       fs_info->nodesize);

		/* A dirty eb shouldn't disappear from buffer_radix */
		if (WARN_ON(!eb))
			return -EUCLEAN;

		if (WARN_ON(cur != apfs_header_bytenr(eb))) {
			free_extent_buffer(eb);
			return -EUCLEAN;
		}
		if (WARN_ON(!uptodate)) {
			free_extent_buffer(eb);
			return -EUCLEAN;
		}

		ret = csum_one_extent_buffer(eb);
		free_extent_buffer(eb);
		if (ret < 0)
			return ret;
	}
	return ret;
}

/*
 * Checksum a dirty tree block before IO.  This has extra checks to make sure
 * we only fill in the checksum field in the first page of a multi-page block.
 * For subpage extent buffers we need bvec to also read the offset in the page.
 */
static int csum_dirty_buffer(struct apfs_fs_info *fs_info, struct bio_vec *bvec)
{
	struct page *page = bvec->bv_page;
	u64 start = page_offset(page);
	u64 found_start;
	struct extent_buffer *eb;

	if (fs_info->sectorsize < PAGE_SIZE)
		return csum_dirty_subpage_buffers(fs_info, bvec);

	eb = (struct extent_buffer *)page->private;
	if (page != eb->pages[0])
		return 0;

	found_start = apfs_header_bytenr(eb);

	if (test_bit(EXTENT_BUFFER_NO_CHECK, &eb->bflags)) {
		WARN_ON(found_start != 0);
		return 0;
	}

	/*
	 * Please do not consolidate these warnings into a single if.
	 * It is useful to know what went wrong.
	 */
	if (WARN_ON(found_start != start))
		return -EUCLEAN;
	if (WARN_ON(!PageUptodate(page)))
		return -EUCLEAN;

	return csum_one_extent_buffer(eb);
}

static int check_tree_block_fsid(struct extent_buffer *eb)
{
	return 0;
}

/* Do basic extent buffer checks at read time */
static int validate_extent_buffer(struct extent_buffer *eb)
{
	struct apfs_fs_info *fs_info = eb->fs_info;
	const u32 csum_size = APFS_CSUM_SIZE;
	u8 found_level;
	u8 result[APFS_CSUM_SIZE];
	const u8 *header_csum;
	int ret = 0;

	found_level = apfs_header_level(eb);
	if (found_level >= APFS_MAX_LEVEL) {
		apfs_err(fs_info, "bad tree block level %d on %llu",
			  (int)apfs_header_level(eb), eb->start);
		ret = -EIO;
		goto out;
	}


	csum_tree_block(eb, result);
	header_csum = page_address(eb->pages[0]) +
		get_eb_offset_in_page(eb, offsetof(struct apfs_obj_header, csum));

	if (memcmp(result, header_csum, csum_size) != 0) {
		apfs_warn_rl(fs_info,
	"checksum verify failed on %llu wanted " CSUM_FMT " found " CSUM_FMT " level %d",
			      eb->start,
			      CSUM_FMT_VALUE(csum_size, header_csum),
			      CSUM_FMT_VALUE(csum_size, result),
			      apfs_header_level(eb));
		ret = -EUCLEAN;
		goto out;
	}

	/*
	 * If this is a leaf block and it is corrupt, set the corrupt bit so
	 * that we don't try and read the other copies of this block, just
	 * return -EIO.
	 */
	if (found_level == 0 && apfs_check_leaf_full(eb)) {
		set_bit(EXTENT_BUFFER_CORRUPT, &eb->bflags);
		ret = -EIO;
	}

	if (found_level > 0 && apfs_check_node(eb))
		ret = -EIO;

	if (!ret)
		set_extent_buffer_uptodate(eb);
	else
		apfs_err(fs_info,
			  "block=%llu read time tree block corruption detected",
			  eb->start);
out:
	return ret;
}

static int validate_subpage_buffer(struct page *page, u64 start, u64 end,
				   int mirror)
{
	struct apfs_fs_info *fs_info = apfs_sb(page->mapping->host->i_sb);
	struct extent_buffer *eb;
	bool reads_done;
	int ret = 0;

	/*
	 * We don't allow bio merge for subpage metadata read, so we should
	 * only get one eb for each endio hook.
	 */
	ASSERT(end == start + fs_info->nodesize - 1);
	ASSERT(PagePrivate(page));

	eb = find_extent_buffer(fs_info, start);
	/*
	 * When we are reading one tree block, eb must have been inserted into
	 * the radix tree. If not, something is wrong.
	 */
	ASSERT(eb);

	reads_done = atomic_dec_and_test(&eb->io_pages);
	/* Subpage read must finish in page read */
	ASSERT(reads_done);

	eb->read_mirror = mirror;
	if (test_bit(EXTENT_BUFFER_READ_ERR, &eb->bflags)) {
		ret = -EIO;
		goto err;
	}
	ret = validate_extent_buffer(eb);
	if (ret < 0)
		goto err;

	if (test_and_clear_bit(EXTENT_BUFFER_READAHEAD, &eb->bflags))
		btree_readahead_hook(eb, ret);

	set_extent_buffer_uptodate(eb);

	free_extent_buffer(eb);
	return ret;
err:
	/*
	 * end_bio_extent_readpage decrements io_pages in case of error,
	 * make sure it has something to decrement.
	 */
	atomic_inc(&eb->io_pages);
	clear_extent_buffer_uptodate(eb);
	free_extent_buffer(eb);
	return ret;
}

int apfs_validate_metadata_buffer(struct apfs_io_bio *io_bio,
				   struct page *page, u64 start, u64 end,
				   int mirror)
{
	struct extent_buffer *eb;
	int ret = 0;
	int reads_done;

	ASSERT(page->private);

	if (apfs_sb(page->mapping->host->i_sb)->sectorsize < PAGE_SIZE)
		return validate_subpage_buffer(page, start, end, mirror);

	eb = (struct extent_buffer *)page->private;

	/*
	 * The pending IO might have been the only thing that kept this buffer
	 * in memory.  Make sure we have a ref for all this other checks
	 */
	atomic_inc(&eb->refs);

	reads_done = atomic_dec_and_test(&eb->io_pages);
	if (!reads_done)
		goto err;

	eb->read_mirror = mirror;
	if (test_bit(EXTENT_BUFFER_READ_ERR, &eb->bflags)) {
		ret = -EIO;
		goto err;
	}
	ret = validate_extent_buffer(eb);
err:
	if (reads_done &&
	    test_and_clear_bit(EXTENT_BUFFER_READAHEAD, &eb->bflags))
		btree_readahead_hook(eb, ret);

	if (ret) {
		/*
		 * our io error hook is going to dec the io pages
		 * again, we have to make sure it has something
		 * to decrement
		 */
		atomic_inc(&eb->io_pages);
		clear_extent_buffer_uptodate(eb);
	}
	free_extent_buffer(eb);

	return ret;
}

static void end_workqueue_bio(struct bio *bio)
{
	struct apfs_end_io_wq *end_io_wq = bio->bi_private;
	struct apfs_fs_info *fs_info;
	struct apfs_workqueue *wq;

	fs_info = end_io_wq->info;
	end_io_wq->status = bio->bi_status;

	if (apfs_op(bio) == APFS_MAP_WRITE) {
		if (end_io_wq->metadata == APFS_WQ_ENDIO_METADATA)
			wq = fs_info->endio_meta_write_workers;
		else if (end_io_wq->metadata == APFS_WQ_ENDIO_FREE_SPACE)
			wq = fs_info->endio_freespace_worker;
		else if (end_io_wq->metadata == APFS_WQ_ENDIO_RAID56)
			wq = fs_info->endio_raid56_workers;
		else
			wq = fs_info->endio_write_workers;
	} else {
		if (end_io_wq->metadata == APFS_WQ_ENDIO_RAID56)
			wq = fs_info->endio_raid56_workers;
		else if (end_io_wq->metadata)
			wq = fs_info->endio_meta_workers;
		else
			wq = fs_info->endio_workers;
	}

	apfs_init_work(&end_io_wq->work, end_workqueue_fn, NULL, NULL);
	apfs_queue_work(wq, &end_io_wq->work);
}

blk_status_t apfs_bio_wq_end_io(struct apfs_fs_info *info, struct bio *bio,
			enum apfs_wq_endio_type metadata)
{
	struct apfs_end_io_wq *end_io_wq;

	end_io_wq = kmem_cache_alloc(apfs_end_io_wq_cache, GFP_NOFS);
	if (!end_io_wq)
		return BLK_STS_RESOURCE;

	end_io_wq->private = bio->bi_private;
	end_io_wq->end_io = bio->bi_end_io;
	end_io_wq->info = info;
	end_io_wq->status = 0;
	end_io_wq->bio = bio;
	end_io_wq->metadata = metadata;

	bio->bi_private = end_io_wq;
	bio->bi_end_io = end_workqueue_bio;
	return 0;
}

static void run_one_async_start(struct apfs_work *work)
{
	struct async_submit_bio *async;
	blk_status_t ret;

	async = container_of(work, struct  async_submit_bio, work);
	ret = async->submit_bio_start(async->inode, async->bio,
				      async->dio_file_offset);
	if (ret)
		async->status = ret;
}

/*
 * In order to insert checksums into the metadata in large chunks, we wait
 * until bio submission time.   All the pages in the bio are checksummed and
 * sums are attached onto the ordered extent record.
 *
 * At IO completion time the csums attached on the ordered extent record are
 * inserted into the tree.
 */
static void run_one_async_done(struct apfs_work *work)
{
	struct async_submit_bio *async;
	struct inode *inode;
	blk_status_t ret;

	async = container_of(work, struct  async_submit_bio, work);
	inode = async->inode;

	/* If an error occurred we just want to clean up the bio and move on */
	if (async->status) {
		async->bio->bi_status = async->status;
		bio_endio(async->bio);
		return;
	}

	/*
	 * All of the bios that pass through here are from async helpers.
	 * Use REQ_CGROUP_PUNT to issue them from the owning cgroup's context.
	 * This changes nothing when cgroups aren't in use.
	 */
	async->bio->bi_opf |= REQ_CGROUP_PUNT;
	ret = apfs_map_bio(apfs_sb(inode->i_sb), async->bio, async->mirror_num);
	if (ret) {
		async->bio->bi_status = ret;
		bio_endio(async->bio);
	}
}

static void run_one_async_free(struct apfs_work *work)
{
	struct async_submit_bio *async;

	async = container_of(work, struct  async_submit_bio, work);
	kfree(async);
}

blk_status_t apfs_wq_submit_bio(struct inode *inode, struct bio *bio,
				 int mirror_num, unsigned long bio_flags,
				 u64 dio_file_offset,
				 extent_submit_bio_start_t *submit_bio_start)
{
	struct apfs_fs_info *fs_info = APFS_I(inode)->root->fs_info;
	struct async_submit_bio *async;

	async = kmalloc(sizeof(*async), GFP_NOFS);
	if (!async)
		return BLK_STS_RESOURCE;

	async->inode = inode;
	async->bio = bio;
	async->mirror_num = mirror_num;
	async->submit_bio_start = submit_bio_start;

	apfs_init_work(&async->work, run_one_async_start, run_one_async_done,
			run_one_async_free);

	async->dio_file_offset = dio_file_offset;

	async->status = 0;

	if (op_is_sync(bio->bi_opf))
		apfs_set_work_high_priority(&async->work);

	apfs_queue_work(fs_info->workers, &async->work);
	return 0;
}

static blk_status_t btree_csum_one_bio(struct bio *bio)
{
	struct bio_vec *bvec;
	struct apfs_root *root;
	int ret = 0;
	struct bvec_iter_all iter_all;

	ASSERT(!bio_flagged(bio, BIO_CLONED));
	bio_for_each_segment_all(bvec, bio, iter_all) {
		root = APFS_I(bvec->bv_page->mapping->host)->root;
		ret = csum_dirty_buffer(root->fs_info, bvec);
		if (ret)
			break;
	}

	return errno_to_blk_status(ret);
}

static blk_status_t btree_submit_bio_start(struct inode *inode, struct bio *bio,
					   u64 dio_file_offset)
{
	/*
	 * when we're called for a write, we're already in the async
	 * submission context.  Just jump into apfs_map_bio
	 */
	return btree_csum_one_bio(bio);
}

static bool should_async_write(struct apfs_fs_info *fs_info,
			     struct apfs_inode *bi)
{
	if (apfs_is_zoned(fs_info))
		return false;
	if (atomic_read(&bi->sync_writers))
		return false;
	if (test_bit(APFS_FS_CSUM_IMPL_FAST, &fs_info->flags))
		return false;
	return true;
}

blk_status_t apfs_submit_metadata_bio(struct inode *inode, struct bio *bio,
				       int mirror_num, unsigned long bio_flags)
{
	struct apfs_fs_info *fs_info = APFS_I(inode)->root->fs_info;
	blk_status_t ret;

	if (apfs_op(bio) != APFS_MAP_WRITE) {
		/*
		 * called for a read, do the setup so that checksum validation
		 * can happen in the async kernel threads
		 */
		ret = apfs_bio_wq_end_io(fs_info, bio,
					  APFS_WQ_ENDIO_METADATA);
		if (ret)
			goto out_w_error;
		ret = apfs_map_bio(fs_info, bio, mirror_num);
	} else if (!should_async_write(fs_info, APFS_I(inode))) {
		ret = btree_csum_one_bio(bio);
		if (ret)
			goto out_w_error;
		ret = apfs_map_bio(fs_info, bio, mirror_num);
	} else {
		/*
		 * kthread helpers are used to submit writes so that
		 * checksumming can happen in parallel across all CPUs
		 */
		ret = apfs_wq_submit_bio(inode, bio, mirror_num, 0,
					  0, btree_submit_bio_start);
	}

	if (ret)
		goto out_w_error;
	return 0;

out_w_error:
	bio->bi_status = ret;
	bio_endio(bio);
	return ret;
}

#ifdef CONFIG_MIGRATION
static int btree_migratepage(struct address_space *mapping,
			struct page *newpage, struct page *page,
			enum migrate_mode mode)
{
	/*
	 * we can't safely write a btree page from here,
	 * we haven't done the locking hook
	 */
	if (PageDirty(page))
		return -EAGAIN;
	/*
	 * Buffers may be managed in a filesystem specific way.
	 * We must have no buffers or drop them.
	 */
	if (page_has_private(page) &&
	    !try_to_release_page(page, GFP_KERNEL))
		return -EAGAIN;
	return migrate_page(mapping, newpage, page, mode);
}
#endif


static int btree_writepages(struct address_space *mapping,
			    struct writeback_control *wbc)
{
	struct apfs_fs_info *fs_info;
	int ret;

	if (wbc->sync_mode == WB_SYNC_NONE) {

		if (wbc->for_kupdate)
			return 0;

		fs_info = APFS_I(mapping->host)->root->fs_info;
		/* this is a bit racy, but that's ok */
		ret = __percpu_counter_compare(&fs_info->dirty_metadata_bytes,
					     APFS_DIRTY_METADATA_THRESH,
					     fs_info->dirty_metadata_batch);
		if (ret < 0)
			return 0;
	}
	return btree_write_cache_pages(mapping, wbc);
}

static int btree_releasepage(struct page *page, gfp_t gfp_flags)
{
	if (PageWriteback(page) || PageDirty(page))
		return 0;

	return try_release_extent_buffer(page);
}

static void btree_invalidatepage(struct page *page, unsigned int offset,
				 unsigned int length)
{
	struct extent_io_tree *tree;
	tree = &APFS_I(page->mapping->host)->io_tree;
	extent_invalidatepage(tree, page, offset);
	btree_releasepage(page, GFP_NOFS);
	if (PagePrivate(page)) {
		apfs_warn(APFS_I(page->mapping->host)->root->fs_info,
		  "page private not zero on page %llu host %llu root %llu",
			  (unsigned long long)page_offset(page),
			  apfs_ino(APFS_I(page->mapping->host)),
			  APFS_I(page->mapping->host)->root->root_key.objectid);
		detach_page_private(page);
	}
}

static int btree_set_page_dirty(struct page *page)
{
#ifdef DEBUG
	struct apfs_fs_info *fs_info = apfs_sb(page->mapping->host->i_sb);
	struct apfs_subpage *subpage;
	struct extent_buffer *eb;
	int cur_bit = 0;
	u64 page_start = page_offset(page);

	if (fs_info->sectorsize == PAGE_SIZE) {
		BUG_ON(!PagePrivate(page));
		eb = (struct extent_buffer *)page->private;
		BUG_ON(!eb);
		BUG_ON(!test_bit(EXTENT_BUFFER_DIRTY, &eb->bflags));
		BUG_ON(!atomic_read(&eb->refs));
		apfs_assert_tree_locked(eb);
		return __set_page_dirty_nobuffers(page);
	}
	ASSERT(PagePrivate(page) && page->private);
	subpage = (struct apfs_subpage *)page->private;

	ASSERT(subpage->dirty_bitmap);
	while (cur_bit < APFS_SUBPAGE_BITMAP_SIZE) {
		unsigned long flags;
		u64 cur;
		u16 tmp = (1 << cur_bit);

		spin_lock_irqsave(&subpage->lock, flags);
		if (!(tmp & subpage->dirty_bitmap)) {
			spin_unlock_irqrestore(&subpage->lock, flags);
			cur_bit++;
			continue;
		}
		spin_unlock_irqrestore(&subpage->lock, flags);
		cur = page_start + cur_bit * fs_info->sectorsize;

		eb = find_extent_buffer(fs_info, cur);
		ASSERT(eb);
		ASSERT(test_bit(EXTENT_BUFFER_DIRTY, &eb->bflags));
		ASSERT(atomic_read(&eb->refs));
		apfs_assert_tree_locked(eb);
		free_extent_buffer(eb);

		cur_bit += (fs_info->nodesize >> fs_info->sectorsize_bits);
	}
#endif
	return __set_page_dirty_nobuffers(page);
}

static const struct address_space_operations btree_aops = {
	.writepages	= btree_writepages,
	.releasepage	= btree_releasepage,
	.invalidatepage = btree_invalidatepage,
#ifdef CONFIG_MIGRATION
	.migratepage	= btree_migratepage,
#endif
	.set_page_dirty = btree_set_page_dirty,
};

struct extent_buffer *apfs_find_create_tree_block(
						struct apfs_fs_info *fs_info,
						u64 bytenr, u64 owner_root,
						int level)
{
	if (apfs_is_testing(fs_info))
		return alloc_test_extent_buffer(fs_info, bytenr);
	return alloc_extent_buffer(fs_info, bytenr, owner_root, level);
}

/*
 * Read tree block at logical address @bytenr and do variant basic but critical
 * verification.
 *
 * @owner_root:		the objectid of the root owner for this block.
 * @parent_transid:	expected transid of this tree block, skip check if 0
 * @level:		expected level, mandatory check
 * @first_key:		expected key in slot 0, skip check if NULL
 */
struct extent_buffer *read_tree_block(struct apfs_fs_info *fs_info, u64 bytenr,
				      u64 owner_root, u64 parent_transid,
				      int level, struct apfs_key *first_key)
{
	struct extent_buffer *buf = NULL;
	int ret;

	buf = apfs_find_create_tree_block(fs_info, bytenr, owner_root, level);
	if (IS_ERR(buf)) {
		apfs_err(fs_info, "failed to find create tree block bytenr %llu %d\n",
			 bytenr, (int)PTR_ERR(buf));
		return buf;
	}

	ret = btree_read_extent_buffer_pages(buf, parent_transid,
					     level, first_key);
	if (ret) {
		apfs_err(fs_info, "failed to read tree block bytenr %llu %d\n",
			 bytenr, ret);
		free_extent_buffer_stale(buf);
		return ERR_PTR(ret);
	}
	return buf;

}

void apfs_clean_tree_block(struct extent_buffer *buf)
{
	struct apfs_fs_info *fs_info = buf->fs_info;
	if (apfs_header_generation(buf) ==
	    fs_info->running_transaction->transid) {
		apfs_assert_tree_locked(buf);

		if (test_and_clear_bit(EXTENT_BUFFER_DIRTY, &buf->bflags)) {
			percpu_counter_add_batch(&fs_info->dirty_metadata_bytes,
						 -buf->len,
						 fs_info->dirty_metadata_batch);
			clear_extent_buffer_dirty(buf);
		}
	}
}

static void __setup_root(struct apfs_root *root, struct apfs_fs_info *fs_info,
			 u64 objectid)
{
	bool dummy = test_bit(APFS_FS_STATE_DUMMY_FS_INFO, &fs_info->fs_state);
	root->fs_info = fs_info;
	root->node = NULL;
	root->commit_root = NULL;
	root->state = 0;
	root->orphan_cleanup_state = 0;

	root->last_trans = 0;
	root->free_objectid = 0;
	root->nr_delalloc_inodes = 0;
	root->nr_ordered_extents = 0;
	root->inode_tree = RB_ROOT;
	INIT_RADIX_TREE(&root->delayed_nodes_tree, GFP_ATOMIC);
	root->block_rsv = NULL;

	INIT_LIST_HEAD(&root->dirty_list);
	INIT_LIST_HEAD(&root->root_list);
	INIT_LIST_HEAD(&root->delalloc_inodes);
	INIT_LIST_HEAD(&root->delalloc_root);
	INIT_LIST_HEAD(&root->ordered_extents);
	INIT_LIST_HEAD(&root->ordered_root);
	INIT_LIST_HEAD(&root->reloc_dirty_list);
	INIT_LIST_HEAD(&root->logged_list[0]);
	INIT_LIST_HEAD(&root->logged_list[1]);
	spin_lock_init(&root->inode_lock);
	spin_lock_init(&root->delalloc_lock);
	spin_lock_init(&root->ordered_extent_lock);
	spin_lock_init(&root->accounting_lock);
	spin_lock_init(&root->log_extents_lock[0]);
	spin_lock_init(&root->log_extents_lock[1]);
	spin_lock_init(&root->qgroup_meta_rsv_lock);
	mutex_init(&root->objectid_mutex);
	mutex_init(&root->log_mutex);
	mutex_init(&root->ordered_extent_mutex);
	mutex_init(&root->delalloc_mutex);
	init_waitqueue_head(&root->qgroup_flush_wait);
	init_waitqueue_head(&root->log_writer_wait);
	init_waitqueue_head(&root->log_commit_wait[0]);
	init_waitqueue_head(&root->log_commit_wait[1]);
	INIT_LIST_HEAD(&root->log_ctxs[0]);
	INIT_LIST_HEAD(&root->log_ctxs[1]);
	atomic_set(&root->log_commit[0], 0);
	atomic_set(&root->log_commit[1], 0);
	atomic_set(&root->log_writers, 0);
	atomic_set(&root->log_batch, 0);
	refcount_set(&root->refs, 1);
	atomic_set(&root->snapshot_force_cow, 0);
	atomic_set(&root->nr_swapfiles, 0);
	root->log_transid = 0;
	root->log_transid_committed = -1;
	root->last_log_commit = 0;
	if (!dummy) {
		extent_io_tree_init(fs_info, &root->dirty_log_pages,
				    IO_TREE_ROOT_DIRTY_LOG_PAGES, NULL);
		extent_io_tree_init(fs_info, &root->log_csum_range,
				    IO_TREE_LOG_CSUM_RANGE, NULL);
	}

	memset(&root->root_key, 0, sizeof(root->root_key));
	memset(&root->root_item, 0, sizeof(root->root_item));
	memset(&root->defrag_progress, 0, sizeof(root->defrag_progress));
	root->root_key.objectid = objectid;
	root->anon_dev = 0;

	spin_lock_init(&root->root_item_lock);
	apfs_qgroup_init_swapped_blocks(&root->swapped_blocks);


#ifdef CONFIG_APFS_DEBUG
	INIT_LIST_HEAD(&root->leak_list);
	spin_lock(&fs_info->fs_roots_radix_lock);
	list_add_tail(&root->leak_list, &fs_info->allocated_roots);
	spin_unlock(&fs_info->fs_roots_radix_lock);
#endif
}

static struct apfs_root *apfs_alloc_root(struct apfs_fs_info *fs_info,
					   u64 objectid, gfp_t flags)
{
	struct apfs_root *root = kzalloc(sizeof(*root), flags);
	if (root)
		__setup_root(root, fs_info, objectid);
	return root;
}

#ifdef CONFIG_APFS_FS_RUN_SANITY_TESTS
/* Should only be used by the testing infrastructure */
struct apfs_root *apfs_alloc_dummy_root(struct apfs_fs_info *fs_info)
{
	struct apfs_root *root;

	if (!fs_info)
		return ERR_PTR(-EINVAL);

	root = apfs_alloc_root(fs_info, APFS_ROOT_TREE_OBJECTID, GFP_KERNEL);
	if (!root)
		return ERR_PTR(-ENOMEM);

	/* We don't use the stripesize in selftest, set it as sectorsize */
	root->alloc_bytenr = 0;

	return root;
}
#endif

struct apfs_root *apfs_create_tree(struct apfs_trans_handle *trans,
				     u64 objectid)
{
	struct apfs_fs_info *fs_info = trans->fs_info;
	struct extent_buffer *leaf;
	struct apfs_root *tree_root = fs_info->tree_root;
	struct apfs_root *root;
	struct apfs_key key = {};
	unsigned int nofs_flag;
	int ret = 0;

	/*
	 * We're holding a transaction handle, so use a NOFS memory allocation
	 * context to avoid deadlock if reclaim happens.
	 */
	nofs_flag = memalloc_nofs_save();
	root = apfs_alloc_root(fs_info, objectid, GFP_KERNEL);
	memalloc_nofs_restore(nofs_flag);
	if (!root)
		return ERR_PTR(-ENOMEM);

	root->root_key.objectid = objectid;
	root->root_key.type = APFS_ROOT_ITEM_KEY;
	root->root_key.offset = 0;

	leaf = apfs_alloc_tree_block(trans, root, 0, objectid, NULL, 0, 0, 0,
				      APFS_NESTING_NORMAL);
	if (IS_ERR(leaf)) {
		ret = PTR_ERR(leaf);
		leaf = NULL;
		goto fail_unlock;
	}

	root->node = leaf;
	apfs_mark_buffer_dirty(leaf);

	root->commit_root = apfs_root_node(root);
	set_bit(APFS_ROOT_TRACK_DIRTY, &root->state);

	apfs_set_root_flags(&root->root_item, 0);
	apfs_set_root_limit(&root->root_item, 0);
	apfs_set_root_bytenr(&root->root_item, leaf->start);
	apfs_set_root_generation(&root->root_item, trans->transid);
	apfs_set_root_level(&root->root_item, 0);
	apfs_set_root_refs(&root->root_item, 1);
	apfs_set_root_used(&root->root_item, leaf->len);
	apfs_set_root_last_snapshot(&root->root_item, 0);
	apfs_set_root_dirid(&root->root_item, 0);
	if (is_fstree(objectid))
		generate_random_guid(root->root_item.uuid);
	else
		export_guid(root->root_item.uuid, &guid_null);
	apfs_set_root_drop_level(&root->root_item, 0);

	apfs_tree_unlock(leaf);

	key.objectid = objectid;
	key.type = APFS_ROOT_ITEM_KEY;
	key.offset = 0;
	ret = apfs_insert_root(trans, tree_root, &key, &root->root_item);
	if (ret)
		goto fail;

	return root;

fail_unlock:
	if (leaf)
		apfs_tree_unlock(leaf);
fail:
	apfs_put_root(root);

	return ERR_PTR(ret);
}

static struct apfs_root *alloc_log_tree(struct apfs_trans_handle *trans,
					 struct apfs_fs_info *fs_info)
{
	struct apfs_root *root;

	root = apfs_alloc_root(fs_info, APFS_TREE_LOG_OBJECTID, GFP_NOFS);
	if (!root)
		return ERR_PTR(-ENOMEM);

	root->root_key.objectid = APFS_TREE_LOG_OBJECTID;
	root->root_key.type = APFS_ROOT_ITEM_KEY;
	root->root_key.offset = APFS_TREE_LOG_OBJECTID;

	return root;
}

int apfs_alloc_log_tree_node(struct apfs_trans_handle *trans,
			      struct apfs_root *root)
{
	struct extent_buffer *leaf;

	/*
	 * DON'T set SHAREABLE bit for log trees.
	 *
	 * Log trees are not exposed to user space thus can't be snapshotted,
	 * and they go away before a real commit is actually done.
	 *
	 * They do store pointers to file data extents, and those reference
	 * counts still get updated (along with back refs to the log tree).
	 */

	leaf = apfs_alloc_tree_block(trans, root, 0, APFS_TREE_LOG_OBJECTID,
			NULL, 0, 0, 0, APFS_NESTING_NORMAL);
	if (IS_ERR(leaf))
		return PTR_ERR(leaf);

	root->node = leaf;

	apfs_mark_buffer_dirty(root->node);
	apfs_tree_unlock(root->node);

	return 0;
}

int apfs_init_log_root_tree(struct apfs_trans_handle *trans,
			     struct apfs_fs_info *fs_info)
{
	struct apfs_root *log_root;

	log_root = alloc_log_tree(trans, fs_info);
	if (IS_ERR(log_root))
		return PTR_ERR(log_root);

	if (!apfs_is_zoned(fs_info)) {
		int ret = apfs_alloc_log_tree_node(trans, log_root);

		if (ret) {
			apfs_put_root(log_root);
			return ret;
		}
	}

	WARN_ON(fs_info->log_root_tree);
	fs_info->log_root_tree = log_root;
	return 0;
}

int apfs_add_log_tree(struct apfs_trans_handle *trans,
		       struct apfs_root *root)
{
	struct apfs_fs_info *fs_info = root->fs_info;
	struct apfs_root *log_root;
	struct apfs_inode_item *inode_item;
	int ret;

	log_root = alloc_log_tree(trans, fs_info);
	if (IS_ERR(log_root))
		return PTR_ERR(log_root);

	ret = apfs_alloc_log_tree_node(trans, log_root);
	if (ret) {
		apfs_put_root(log_root);
		return ret;
	}

	log_root->last_trans = trans->transid;
	log_root->root_key.offset = root->root_key.objectid;

	inode_item = &log_root->root_item.inode;
	apfs_set_stack_inode_generation(inode_item, 1);
	apfs_set_stack_inode_size(inode_item, 3);
	apfs_set_stack_inode_nlink(inode_item, 1);
	apfs_set_stack_inode_nbytes(inode_item,
				     fs_info->nodesize);
	apfs_set_stack_inode_mode(inode_item, S_IFDIR | 0755);

	apfs_set_root_node(&log_root->root_item, log_root->node);

	WARN_ON(root->log_root);
	root->log_root = log_root;
	root->log_transid = 0;
	root->log_transid_committed = -1;
	root->last_log_commit = 0;
	return 0;
}

static struct apfs_root *read_tree_root_path(struct apfs_root *tree_root,
					      struct apfs_path *path,
					      struct apfs_key *key)
{
	struct apfs_root *root;
	struct apfs_fs_info *fs_info = tree_root->fs_info;
	u64 generation;
	int ret;
	int level;

	root = apfs_alloc_root(fs_info, key->objectid, GFP_NOFS);
	if (!root)
		return ERR_PTR(-ENOMEM);

	ret = apfs_find_root(tree_root, key, path,
			      &root->root_item, &root->root_key);
	if (ret) {
		if (ret > 0)
			ret = -ENOENT;
		goto fail;
	}

	generation = apfs_root_generation(&root->root_item);
	level = apfs_root_level(&root->root_item);
	root->node = read_tree_block(fs_info,
				     apfs_root_bytenr(&root->root_item),
				     key->objectid, generation, level, NULL);
	if (IS_ERR(root->node)) {
		ret = PTR_ERR(root->node);
		root->node = NULL;
		goto fail;
	} else if (!apfs_buffer_uptodate(root->node, generation, 0)) {
		ret = -EIO;
		goto fail;
	}
	root->commit_root = apfs_root_node(root);
	return root;
fail:
	apfs_put_root(root);
	return ERR_PTR(ret);
}

struct apfs_root *apfs_read_tree_root(struct apfs_root *tree_root,
					struct apfs_key *key)
{
	struct apfs_root *root;
	struct apfs_path *path;

	path = apfs_alloc_path();
	if (!path)
		return ERR_PTR(-ENOMEM);
	root = read_tree_root_path(tree_root, path, key);
	apfs_free_path(path);

	return root;
}

struct apfs_root *apfs_read_root(struct apfs_fs_info *fs_info, u8 type,
				 u64 bytenr)
{
	struct apfs_root *root = NULL;
	int ret;

	root = apfs_alloc_root(fs_info, APFS_OBJ_TYPE_OMAP, GFP_KERNEL);
	if (!root) {
		ret = -ENOMEM;
		goto fail;
	}

	root->node = read_tree_block(fs_info, bytenr, type, 0, -1, NULL);

	if (IS_ERR(root->node)) {
		ret = PTR_ERR(root->node);
		root->node = NULL;
		goto fail;
	} else if (!extent_buffer_uptodate(root->node)) {
		ret = -EIO;
		free_extent_buffer(root->node);
		root->node = NULL;
		goto fail;
	}

	root->is_fsroot = apfs_header_subtype(root->node) == APFS_OBJ_TYPE_FSTREE;
	return root;
fail:
	apfs_put_root(root);
	apfs_err(fs_info, "failed to read root %u tree at %llu",
		 type, bytenr);
	return ERR_PTR(ret);
}

/*
 * Initialize subvolume root in-memory structure
 *
 * @anon_dev:	anonymous device to attach to the root, if zero, allocate new
 */
static int apfs_init_fs_root(struct apfs_root *root, dev_t anon_dev)
{
	int ret;
	unsigned int nofs_flag;

	/*
	 * We might be called under a transaction (e.g. indirect backref
	 * resolution) which could deadlock if it triggers memory reclaim
	 */
	nofs_flag = memalloc_nofs_save();
	ret = apfs_drew_lock_init(&root->snapshot_lock);
	memalloc_nofs_restore(nofs_flag);
	if (ret)
		goto fail;

	if (root->root_key.objectid != APFS_TREE_LOG_OBJECTID &&
	    root->root_key.objectid != APFS_DATA_RELOC_TREE_OBJECTID) {
		set_bit(APFS_ROOT_SHAREABLE, &root->state);
		apfs_check_and_init_root_item(&root->root_item);
	}

	/*
	 * Don't assign anonymous block device to roots that are not exposed to
	 * userspace, the id pool is limited to 1M
	 */
	if (is_fstree(root->root_key.objectid) &&
	    apfs_root_refs(&root->root_item) > 0) {
		if (!anon_dev) {
			ret = get_anon_bdev(&root->anon_dev);
			if (ret)
				goto fail;
		} else {
			root->anon_dev = anon_dev;
		}
	}

	mutex_lock(&root->objectid_mutex);
	ret = apfs_init_root_free_objectid(root);
	if (ret) {
		mutex_unlock(&root->objectid_mutex);
		goto fail;
	}

	ASSERT(root->free_objectid <= APFS_LAST_FREE_OBJECTID);

	mutex_unlock(&root->objectid_mutex);

	return 0;
fail:
	/* The caller is responsible to call apfs_free_fs_root */
	return ret;
}

static struct apfs_root *apfs_lookup_fs_root(struct apfs_fs_info *fs_info,
					       u64 root_id)
{
	struct apfs_root *root;

	spin_lock(&fs_info->fs_roots_radix_lock);
	root = radix_tree_lookup(&fs_info->fs_roots_radix,
				 (unsigned long)root_id);
	if (root)
		root = apfs_grab_root(root);
	spin_unlock(&fs_info->fs_roots_radix_lock);
	return root;
}

static struct apfs_root *apfs_get_global_root(struct apfs_fs_info *fs_info,
						u64 objectid)
{
	if (objectid == APFS_OBJ_TYPE_OMAP)
		return apfs_grab_root(fs_info->omap_root);
	if (objectid == APFS_OBJ_TYPE_FEXT_TREE)
		return apfs_grab_root(fs_info->fext_root);
	if (objectid == APFS_OBJ_TYPE_SNAPTREE)
		return apfs_grab_root(fs_info->snap_root);
	return NULL;
}

int apfs_insert_fs_root(struct apfs_fs_info *fs_info,
			 struct apfs_root *root)
{
	int ret;

	ret = radix_tree_preload(GFP_NOFS);
	if (ret)
		return ret;

	spin_lock(&fs_info->fs_roots_radix_lock);
	ret = radix_tree_insert(&fs_info->fs_roots_radix,
				(unsigned long)root->root_key.objectid,
				root);
	if (ret == 0) {
		apfs_grab_root(root);
		set_bit(APFS_ROOT_IN_RADIX, &root->state);
	}
	spin_unlock(&fs_info->fs_roots_radix_lock);
	radix_tree_preload_end();

	return ret;
}

void apfs_check_leaked_roots(struct apfs_fs_info *fs_info)
{
#ifdef CONFIG_APFS_DEBUG
	struct apfs_root *root;

	while (!list_empty(&fs_info->allocated_roots)) {
		char buf[APFS_ROOT_NAME_BUF_LEN];

		root = list_first_entry(&fs_info->allocated_roots,
					struct apfs_root, leak_list);
		apfs_err(fs_info, "leaked root %s refcount %d",
			  apfs_root_name(&root->root_key, buf),
			  refcount_read(&root->refs));
		while (refcount_read(&root->refs) > 1)
			apfs_put_root(root);
		apfs_put_root(root);
	}
#endif
}

static void
__apfs_free_fs_info(struct apfs_fs_info *fs_info, bool dummy, bool free_self)
{
	if (fs_info == NULL)
		return ;

	percpu_counter_destroy(&fs_info->dirty_metadata_bytes);
	percpu_counter_destroy(&fs_info->delalloc_bytes);
	percpu_counter_destroy(&fs_info->ordered_bytes);
	percpu_counter_destroy(&fs_info->dev_replace.bio_counter);
	apfs_free_csum_hash(fs_info);
	apfs_free_stripe_hash_table(fs_info);
	apfs_free_ref_cache(fs_info);
	kfree(fs_info->balance_ctl);
	kfree(fs_info->delayed_root);
	apfs_put_root(fs_info->extent_root);
	apfs_put_root(fs_info->tree_root);
	apfs_put_root(fs_info->chunk_root);
	apfs_put_root(fs_info->dev_root);
	apfs_put_root(fs_info->csum_root);
	apfs_put_root(fs_info->quota_root);
	apfs_put_root(fs_info->uuid_root);
	apfs_put_root(fs_info->free_space_root);
	apfs_put_root(fs_info->fs_root);
	apfs_put_root(fs_info->data_reloc_root);

	apfs_put_root(fs_info->root_root);
	apfs_put_root(fs_info->omap_root);
	apfs_put_root(fs_info->fext_root);
	apfs_put_root(fs_info->snap_root);
	apfs_put_root(fs_info->extref_root);

	apfs_check_leaked_roots(fs_info);
	apfs_extent_buffer_leak_debug_check(fs_info);

	if (!dummy)
		apfs_put_nx_info(fs_info->nx_info);
	if (!free_self)
		return;

	kfree(fs_info->super_copy);
	kfree(fs_info->__super_copy);
	kfree(fs_info->super_for_commit);
	kvfree(fs_info);
}

void apfs_free_fs_info(struct apfs_fs_info *fs_info)
{
	__apfs_free_fs_info(fs_info, false, true);
}

void apfs_destroy_fs_info(struct apfs_fs_info *fs_info)
{
	__apfs_free_fs_info(fs_info, false, false);
}

static void apfs_free_dummy_fs_info(struct apfs_fs_info *fs_info)
{
	__apfs_free_fs_info(fs_info, true, true);
}

void apfs_free_nx_info(struct apfs_nx_info *nx_info)
{
	ASSERT(refcount_read(&nx_info->refs) <= 1);

	apfs_close_device(nx_info->device);

	kfree(nx_info->super_copy);

	kvfree(nx_info);
}

void apfs_get_nx_info(struct apfs_nx_info *nx_info)
{
	refcount_inc(&nx_info->refs);
}

void apfs_put_nx_info(struct apfs_nx_info *nx_info)
{
	if (!nx_info)
		return;

	refcount_dec(&nx_info->refs);

	/* device holds one refcount*/
	if (refcount_read(&nx_info->refs) == 1) {
		apfs_free_dummy_fs_info(nx_info->vol);
		apfs_free_nx_info(nx_info);
	}
}

/*
 * Get an in-memory reference of a root structure.
 *
 * For essential trees like root/extent tree, we grab it from fs_info directly.
 * For subvolume trees, we check the cached filesystem roots first. If not
 * found, then read it from disk and add it to cached fs roots.
 *
 * Caller should release the root by calling apfs_put_root() after the usage.
 *
 * NOTE: Reloc and log trees can't be read by this function as they share the
 *	 same root objectid.
 *
 * @objectid:	root id
 * @anon_dev:	preallocated anonymous block device number for new roots,
 * 		pass 0 for new allocation.
 * @check_ref:	whether to check root item references, If true, return -ENOENT
 *		for orphan roots
 */
static struct apfs_root *apfs_get_root_ref(struct apfs_fs_info *fs_info,
					     u64 objectid, dev_t anon_dev,
					     bool check_ref)
{
	struct apfs_root *root;
	struct apfs_path *path;
	struct apfs_key key = {};
	int ret;

	root = apfs_get_global_root(fs_info, objectid);
	if (root)
		return root;
again:
	root = apfs_lookup_fs_root(fs_info, objectid);
	if (root) {
		/* Shouldn't get preallocated anon_dev for cached roots */
		ASSERT(!anon_dev);
		if (check_ref && apfs_root_refs(&root->root_item) == 0) {
			apfs_put_root(root);
			return ERR_PTR(-ENOENT);
		}
		return root;
	}

	key.objectid = objectid;
	key.type = APFS_ROOT_ITEM_KEY;
	key.offset = (u64)-1;
	root = apfs_read_tree_root(fs_info->tree_root, &key);
	if (IS_ERR(root))
		return root;

	if (check_ref && apfs_root_refs(&root->root_item) == 0) {
		ret = -ENOENT;
		goto fail;
	}

	ret = apfs_init_fs_root(root, anon_dev);
	if (ret)
		goto fail;

	path = apfs_alloc_path();
	if (!path) {
		ret = -ENOMEM;
		goto fail;
	}
	key.objectid = APFS_ORPHAN_OBJECTID;
	key.type = APFS_ORPHAN_ITEM_KEY;
	key.offset = objectid;

	ret = apfs_search_slot(NULL, fs_info->tree_root, &key, path, 0, 0);
	apfs_free_path(path);
	if (ret < 0)
		goto fail;
	if (ret == 0)
		set_bit(APFS_ROOT_ORPHAN_ITEM_INSERTED, &root->state);

	ret = apfs_insert_fs_root(fs_info, root);
	if (ret) {
		apfs_put_root(root);
		if (ret == -EEXIST)
			goto again;
		goto fail;
	}
	return root;
fail:
	apfs_put_root(root);
	return ERR_PTR(ret);
}

/*
 * Get in-memory reference of a root structure
 *
 * @objectid:	tree objectid
 * @check_ref:	if set, verify that the tree exists and the item has at least
 *		one reference
 */
struct apfs_root *apfs_get_fs_root(struct apfs_fs_info *fs_info,
				     u64 objectid, bool check_ref)
{
	return apfs_get_root_ref(fs_info, objectid, 0, check_ref);
}

/*
 * Get in-memory reference of a root structure, created as new, optionally pass
 * the anonymous block device id
 *
 * @objectid:	tree objectid
 * @anon_dev:	if zero, allocate a new anonymous block device or use the
 *		parameter value
 */
struct apfs_root *apfs_get_new_fs_root(struct apfs_fs_info *fs_info,
					 u64 objectid, dev_t anon_dev)
{
	return apfs_get_root_ref(fs_info, objectid, anon_dev, true);
}

/*
 * apfs_get_fs_root_commit_root - return a root for the given objectid
 * @fs_info:	the fs_info
 * @objectid:	the objectid we need to lookup
 *
 * This is exclusively used for backref walking, and exists specifically because
 * of how qgroups does lookups.  Qgroups will do a backref lookup at delayed ref
 * creation time, which means we may have to read the tree_root in order to look
 * up a fs root that is not in memory.  If the root is not in memory we will
 * read the tree root commit root and look up the fs root from there.  This is a
 * temporary root, it will not be inserted into the radix tree as it doesn't
 * have the most uptodate information, it'll simply be discarded once the
 * backref code is finished using the root.
 */
struct apfs_root *apfs_get_fs_root_commit_root(struct apfs_fs_info *fs_info,
						 struct apfs_path *path,
						 u64 objectid)
{
	struct apfs_root *root;
	struct apfs_key key = {};

	ASSERT(path->search_commit_root && path->skip_locking);

	/*
	 * This can return -ENOENT if we ask for a root that doesn't exist, but
	 * since this is called via the backref walking code we won't be looking
	 * up a root that doesn't exist, unless there's corruption.  So if root
	 * != NULL just return it.
	 */
	root = apfs_get_global_root(fs_info, objectid);
	if (root)
		return root;

	root = apfs_lookup_fs_root(fs_info, objectid);
	if (root)
		return root;

	key.objectid = objectid;
	key.type = APFS_ROOT_ITEM_KEY;
	key.offset = (u64)-1;
	root = read_tree_root_path(fs_info->tree_root, path, &key);
	apfs_release_path(path);

	return root;
}

/*
 * called by the kthread helper functions to finally call the bio end_io
 * functions.  This is where read checksum verification actually happens
 */
static void end_workqueue_fn(struct apfs_work *work)
{
	struct bio *bio;
	struct apfs_end_io_wq *end_io_wq;

	end_io_wq = container_of(work, struct apfs_end_io_wq, work);
	bio = end_io_wq->bio;

	bio->bi_status = end_io_wq->status;
	bio->bi_private = end_io_wq->private;
	bio->bi_end_io = end_io_wq->end_io;
	bio_endio(bio);
	kmem_cache_free(apfs_end_io_wq_cache, end_io_wq);
}

static int cleaner_kthread(void *arg)
{
	struct apfs_root *root = arg;
	struct apfs_fs_info *fs_info = root->fs_info;
	int again;

	while (1) {
		again = 0;

		set_bit(APFS_FS_CLEANER_RUNNING, &fs_info->flags);

		/* Make the cleaner go to sleep early. */
		if (apfs_need_cleaner_sleep(fs_info))
			goto sleep;

		/*
		 * Do not do anything if we might cause open_ctree() to block
		 * before we have finished mounting the filesystem.
		 */
		if (!test_bit(APFS_FS_OPEN, &fs_info->flags))
			goto sleep;

		if (!mutex_trylock(&fs_info->cleaner_mutex))
			goto sleep;

		/*
		 * Avoid the problem that we change the status of the fs
		 * during the above check and trylock.
		 */
		if (apfs_need_cleaner_sleep(fs_info)) {
			mutex_unlock(&fs_info->cleaner_mutex);
			goto sleep;
		}

		apfs_run_delayed_iputs(fs_info);

		again = apfs_clean_one_deleted_snapshot(root);
		mutex_unlock(&fs_info->cleaner_mutex);

		/*
		 * The defragger has dealt with the R/O remount and umount,
		 * needn't do anything special here.
		 */
		apfs_run_defrag_inodes(fs_info);

		/*
		 * Acquires fs_info->reclaim_bgs_lock to avoid racing
		 * with relocation (apfs_relocate_chunk) and relocation
		 * acquires fs_info->cleaner_mutex (apfs_relocate_block_group)
		 * after acquiring fs_info->reclaim_bgs_lock. So we
		 * can't hold, nor need to, fs_info->cleaner_mutex when deleting
		 * unused block groups.
		 */
		apfs_delete_unused_bgs(fs_info);

		/*
		 * Reclaim block groups in the reclaim_bgs list after we deleted
		 * all unused block_groups. This possibly gives us some more free
		 * space.
		 */
		apfs_reclaim_bgs(fs_info);
sleep:
		clear_and_wake_up_bit(APFS_FS_CLEANER_RUNNING, &fs_info->flags);
		if (kthread_should_park())
			kthread_parkme();
		if (kthread_should_stop())
			return 0;
		if (!again) {
			set_current_state(TASK_INTERRUPTIBLE);
			schedule();
			__set_current_state(TASK_RUNNING);
		}
	}
}

static int transaction_kthread(void *arg)
{
	struct apfs_root *root = arg;
	struct apfs_fs_info *fs_info = root->fs_info;
	struct apfs_trans_handle *trans;
	struct apfs_transaction *cur;
	u64 transid;
	time64_t delta;
	unsigned long delay;
	bool cannot_commit;

	do {
		cannot_commit = false;
		delay = msecs_to_jiffies(fs_info->commit_interval * 1000);
		mutex_lock(&fs_info->transaction_kthread_mutex);

		spin_lock(&fs_info->trans_lock);
		cur = fs_info->running_transaction;
		if (!cur) {
			spin_unlock(&fs_info->trans_lock);
			goto sleep;
		}

		delta = ktime_get_seconds() - cur->start_time;
		if (cur->state < TRANS_STATE_COMMIT_START &&
		    delta < fs_info->commit_interval) {
			spin_unlock(&fs_info->trans_lock);
			delay -= msecs_to_jiffies((delta - 1) * 1000);
			delay = min(delay,
				    msecs_to_jiffies(fs_info->commit_interval * 1000));
			goto sleep;
		}
		transid = cur->transid;
		spin_unlock(&fs_info->trans_lock);

		/* If the file system is aborted, this will always fail. */
		trans = apfs_attach_transaction(root);
		if (IS_ERR(trans)) {
			if (PTR_ERR(trans) != -ENOENT)
				cannot_commit = true;
			goto sleep;
		}
		if (transid == trans->transid) {
			apfs_commit_transaction(trans);
		} else {
			apfs_end_transaction(trans);
		}
sleep:
		wake_up_process(fs_info->cleaner_kthread);
		mutex_unlock(&fs_info->transaction_kthread_mutex);

		if (unlikely(test_bit(APFS_FS_STATE_ERROR,
				      &fs_info->fs_state)))
			apfs_cleanup_transaction(fs_info);
		if (!kthread_should_stop() &&
				(!apfs_transaction_blocked(fs_info) ||
				 cannot_commit))
			schedule_timeout_interruptible(delay);
	} while (!kthread_should_stop());
	return 0;
}

/*
 * This will find the highest generation in the array of root backups.  The
 * index of the highest array is returned, or -EINVAL if we can't find
 * anything.
 *
 * We check to make sure the array is valid by comparing the
 * generation of the latest  root in the array with the generation
 * in the super block.  If they don't match we pitch it.
 */
static int find_newest_super_backup(struct apfs_fs_info *info)
{
	const u64 newest_gen = apfs_super_generation(info->super_copy);
	u64 cur;
	struct apfs_root_backup *root_backup;
	int i;

	for (i = 0; i < APFS_NUM_BACKUP_ROOTS; i++) {
		root_backup = info->super_copy->super_roots + i;
		cur = apfs_backup_tree_root_gen(root_backup);
		if (cur == newest_gen)
			return i;
	}

	return -EINVAL;
}

/*
 * copy all the root pointers into the super backup array.
 * this will bump the backup pointer by one when it is
 * done
 */
static void backup_super_roots(struct apfs_fs_info *info)
{
	const int next_backup = info->backup_root_index;
	struct apfs_root_backup *root_backup;

	root_backup = info->super_for_commit->super_roots + next_backup;

	/*
	 * make sure all of our padding and empty slots get zero filled
	 * regardless of which ones we use today
	 */
	memset(root_backup, 0, sizeof(*root_backup));

	info->backup_root_index = (next_backup + 1) % APFS_NUM_BACKUP_ROOTS;

	apfs_set_backup_tree_root(root_backup, info->tree_root->node->start);
	apfs_set_backup_tree_root_gen(root_backup,
			       apfs_header_generation(info->tree_root->node));

	apfs_set_backup_tree_root_level(root_backup,
			       apfs_header_level(info->tree_root->node));

	apfs_set_backup_chunk_root(root_backup, info->chunk_root->node->start);
	apfs_set_backup_chunk_root_gen(root_backup,
			       apfs_header_generation(info->chunk_root->node));
	apfs_set_backup_chunk_root_level(root_backup,
			       apfs_header_level(info->chunk_root->node));

	apfs_set_backup_extent_root(root_backup, info->extent_root->node->start);
	apfs_set_backup_extent_root_gen(root_backup,
			       apfs_header_generation(info->extent_root->node));
	apfs_set_backup_extent_root_level(root_backup,
			       apfs_header_level(info->extent_root->node));

	/*
	 * we might commit during log recovery, which happens before we set
	 * the fs_root.  Make sure it is valid before we fill it in.
	 */
	if (info->fs_root && info->fs_root->node) {
		apfs_set_backup_fs_root(root_backup,
					 info->fs_root->node->start);
		apfs_set_backup_fs_root_gen(root_backup,
			       apfs_header_generation(info->fs_root->node));
		apfs_set_backup_fs_root_level(root_backup,
			       apfs_header_level(info->fs_root->node));
	}

	apfs_set_backup_dev_root(root_backup, info->dev_root->node->start);
	apfs_set_backup_dev_root_gen(root_backup,
			       apfs_header_generation(info->dev_root->node));
	apfs_set_backup_dev_root_level(root_backup,
				       apfs_header_level(info->dev_root->node));

	apfs_set_backup_csum_root(root_backup, info->csum_root->node->start);
	apfs_set_backup_csum_root_gen(root_backup,
			       apfs_header_generation(info->csum_root->node));
	apfs_set_backup_csum_root_level(root_backup,
			       apfs_header_level(info->csum_root->node));

	apfs_set_backup_total_bytes(root_backup,
			     apfs_super_total_bytes(info->super_copy));
	apfs_set_backup_bytes_used(root_backup,
			     apfs_super_bytes_used(info->super_copy));
	apfs_set_backup_num_devices(root_backup,
			     apfs_super_num_devices(info->super_copy));

	/*
	 * if we don't copy this out to the super_copy, it won't get remembered
	 * for the next commit
	 */
	memcpy(&info->super_copy->super_roots,
	       &info->super_for_commit->super_roots,
	       sizeof(*root_backup) * APFS_NUM_BACKUP_ROOTS);
}

/*
 * read_backup_root - Reads a backup root based on the passed priority. Prio 0
 * is the newest, prio 1/2/3 are 2nd newest/3rd newest/4th (oldest) backup roots
 *
 * fs_info - filesystem whose backup roots need to be read
 * priority - priority of backup root required
 *
 * Returns backup root index on success and -EINVAL otherwise.
 */
static int read_backup_root(struct apfs_fs_info *fs_info, u8 priority)
{
	int backup_index = find_newest_super_backup(fs_info);
	struct apfs_super_block *super = fs_info->super_copy;
	struct apfs_root_backup *root_backup;

	if (priority < APFS_NUM_BACKUP_ROOTS && backup_index >= 0) {
		if (priority == 0)
			return backup_index;

		backup_index = backup_index + APFS_NUM_BACKUP_ROOTS - priority;
		backup_index %= APFS_NUM_BACKUP_ROOTS;
	} else {
		return -EINVAL;
	}

	root_backup = super->super_roots + backup_index;

	apfs_set_super_generation(super,
				   apfs_backup_tree_root_gen(root_backup));
	apfs_set_super_root(super, apfs_backup_tree_root(root_backup));
	apfs_set_super_root_level(super,
				   apfs_backup_tree_root_level(root_backup));
	apfs_set_super_bytes_used(super, apfs_backup_bytes_used(root_backup));

	/*
	 * Fixme: the total bytes and num_devices need to match or we should
	 * need a fsck
	 */
	apfs_set_super_total_bytes(super, apfs_backup_total_bytes(root_backup));
	apfs_set_super_num_devices(super, apfs_backup_num_devices(root_backup));

	return backup_index;
}

/* helper to cleanup workers */
static void apfs_stop_all_workers(struct apfs_fs_info *fs_info)
{
	apfs_destroy_workqueue(fs_info->fixup_workers);
	apfs_destroy_workqueue(fs_info->delalloc_workers);
	apfs_destroy_workqueue(fs_info->workers);
	apfs_destroy_workqueue(fs_info->endio_workers);
	apfs_destroy_workqueue(fs_info->endio_raid56_workers);
	apfs_destroy_workqueue(fs_info->rmw_workers);
	apfs_destroy_workqueue(fs_info->endio_write_workers);
	apfs_destroy_workqueue(fs_info->endio_freespace_worker);
	apfs_destroy_workqueue(fs_info->delayed_workers);
	apfs_destroy_workqueue(fs_info->caching_workers);
	apfs_destroy_workqueue(fs_info->readahead_workers);
	apfs_destroy_workqueue(fs_info->flush_workers);
	apfs_destroy_workqueue(fs_info->qgroup_rescan_workers);
	if (fs_info->discard_ctl.discard_workers)
		destroy_workqueue(fs_info->discard_ctl.discard_workers);
	/*
	 * Now that all other work queues are destroyed, we can safely destroy
	 * the queues used for metadata I/O, since tasks from those other work
	 * queues can do metadata I/O operations.
	 */
	apfs_destroy_workqueue(fs_info->endio_meta_workers);
	apfs_destroy_workqueue(fs_info->endio_meta_write_workers);
}

static void free_root_extent_buffers(struct apfs_root *root)
{
	if (root) {
		free_extent_buffer(root->node);
		free_extent_buffer(root->commit_root);
		root->node = NULL;
		root->commit_root = NULL;
	}
}

/* helper to cleanup tree roots */
static void free_root_pointers(struct apfs_fs_info *info, bool free_chunk_root)
{
	free_root_extent_buffers(info->tree_root);

	free_root_extent_buffers(info->dev_root);
	free_root_extent_buffers(info->extent_root);
	free_root_extent_buffers(info->csum_root);
	free_root_extent_buffers(info->quota_root);
	free_root_extent_buffers(info->uuid_root);
	free_root_extent_buffers(info->fs_root);
	free_root_extent_buffers(info->data_reloc_root);
	if (free_chunk_root)
		free_root_extent_buffers(info->chunk_root);
	free_root_extent_buffers(info->free_space_root);

	free_root_extent_buffers(info->root_root);
	free_root_extent_buffers(info->fext_root);
	free_root_extent_buffers(info->extref_root);
	free_root_extent_buffers(info->omap_root);
	free_root_extent_buffers(info->snap_root);
}

void apfs_put_root(struct apfs_root *root)
{
	if (!root)
		return;

	if (refcount_dec_and_test(&root->refs)) {
		WARN_ON(!RB_EMPTY_ROOT(&root->inode_tree));
		WARN_ON(test_bit(APFS_ROOT_DEAD_RELOC_TREE, &root->state));
		if (root->anon_dev)
			free_anon_bdev(root->anon_dev);
		apfs_drew_lock_destroy(&root->snapshot_lock);
		free_root_extent_buffers(root);
#ifdef CONFIG_APFS_DEBUG
		spin_lock(&root->fs_info->fs_roots_radix_lock);
		list_del_init(&root->leak_list);
		spin_unlock(&root->fs_info->fs_roots_radix_lock);
#endif
		kfree(root);
	}
}

void apfs_free_fs_roots(struct apfs_fs_info *fs_info)
{
	int ret;
	struct apfs_root *gang[8];
	int i;

	while (!list_empty(&fs_info->dead_roots)) {
		gang[0] = list_entry(fs_info->dead_roots.next,
				     struct apfs_root, root_list);
		list_del(&gang[0]->root_list);

		if (test_bit(APFS_ROOT_IN_RADIX, &gang[0]->state))
			apfs_drop_and_free_fs_root(fs_info, gang[0]);
		apfs_put_root(gang[0]);
	}

	while (1) {
		ret = radix_tree_gang_lookup(&fs_info->fs_roots_radix,
					     (void **)gang, 0,
					     ARRAY_SIZE(gang));
		if (!ret)
			break;
		for (i = 0; i < ret; i++)
			apfs_drop_and_free_fs_root(fs_info, gang[i]);
	}
}

static void apfs_init_scrub(struct apfs_fs_info *fs_info)
{
	mutex_init(&fs_info->scrub_lock);
	atomic_set(&fs_info->scrubs_running, 0);
	atomic_set(&fs_info->scrub_pause_req, 0);
	atomic_set(&fs_info->scrubs_paused, 0);
	atomic_set(&fs_info->scrub_cancel_req, 0);
	init_waitqueue_head(&fs_info->scrub_pause_wait);
	refcount_set(&fs_info->scrub_workers_refcnt, 0);
}

static void apfs_init_balance(struct apfs_fs_info *fs_info)
{
	spin_lock_init(&fs_info->balance_lock);
	mutex_init(&fs_info->balance_mutex);
	atomic_set(&fs_info->balance_pause_req, 0);
	atomic_set(&fs_info->balance_cancel_req, 0);
	fs_info->balance_ctl = NULL;
	init_waitqueue_head(&fs_info->balance_wait_q);
	atomic_set(&fs_info->reloc_cancel_req, 0);
}

static void apfs_init_btree_inode(struct apfs_fs_info *fs_info)
{
	struct inode *inode = fs_info->btree_inode;

	inode->i_ino = APFS_BTREE_INODE_OBJECTID;
	set_nlink(inode, 1);
	/*
	 * we set the i_size on the btree inode to the max possible int.
	 * the real end of the address space is determined by all of
	 * the devices in the system
	 */
	inode->i_size = OFFSET_MAX;
	inode->i_mapping->a_ops = &btree_aops;

	RB_CLEAR_NODE(&APFS_I(inode)->rb_node);
	extent_io_tree_init(fs_info, &APFS_I(inode)->io_tree,
			    IO_TREE_BTREE_INODE_IO, inode);
	APFS_I(inode)->io_tree.track_uptodate = false;

	extent_map_tree_init(&APFS_I(inode)->extent_tree);
	APFS_I(inode)->root = apfs_grab_root(fs_info->omap_root);
	memset(&APFS_I(inode)->location, 0, sizeof(struct apfs_key));
	set_bit(APFS_INODE_DUMMY, &APFS_I(inode)->runtime_flags);
	apfs_insert_inode_hash(inode);
}

static void apfs_init_dev_replace_locks(struct apfs_fs_info *fs_info)
{
	mutex_init(&fs_info->dev_replace.lock_finishing_cancel_unmount);
	init_rwsem(&fs_info->dev_replace.rwsem);
	init_waitqueue_head(&fs_info->dev_replace.replace_wait);
}

static void apfs_init_qgroup(struct apfs_fs_info *fs_info)
{
	spin_lock_init(&fs_info->qgroup_lock);
	mutex_init(&fs_info->qgroup_ioctl_lock);
	fs_info->qgroup_tree = RB_ROOT;
	INIT_LIST_HEAD(&fs_info->dirty_qgroups);
	fs_info->qgroup_seq = 1;
	fs_info->qgroup_ulist = NULL;
	fs_info->qgroup_rescan_running = false;
	mutex_init(&fs_info->qgroup_rescan_lock);
}

static int apfs_init_workqueues(struct apfs_fs_info *fs_info)
{
	u32 max_active = fs_info->thread_pool_size;
	unsigned int flags = WQ_MEM_RECLAIM | WQ_FREEZABLE | WQ_UNBOUND;

	fs_info->workers =
		apfs_alloc_workqueue(fs_info, "worker",
				      flags | WQ_HIGHPRI, max_active, 16);

	fs_info->delalloc_workers =
		apfs_alloc_workqueue(fs_info, "delalloc",
				      flags, max_active, 2);

	fs_info->flush_workers =
		apfs_alloc_workqueue(fs_info, "flush_delalloc",
				      flags, max_active, 0);

	fs_info->caching_workers =
		apfs_alloc_workqueue(fs_info, "cache", flags, max_active, 0);

	fs_info->fixup_workers =
		apfs_alloc_workqueue(fs_info, "fixup", flags, 1, 0);

	/*
	 * endios are largely parallel and should have a very
	 * low idle thresh
	 */
	fs_info->endio_workers =
		apfs_alloc_workqueue(fs_info, "endio", flags, max_active, 4);
	fs_info->endio_meta_workers =
		apfs_alloc_workqueue(fs_info, "endio-meta", flags,
				      max_active, 4);
	fs_info->endio_meta_write_workers =
		apfs_alloc_workqueue(fs_info, "endio-meta-write", flags,
				      max_active, 2);
	fs_info->endio_raid56_workers =
		apfs_alloc_workqueue(fs_info, "endio-raid56", flags,
				      max_active, 4);
	fs_info->rmw_workers =
		apfs_alloc_workqueue(fs_info, "rmw", flags, max_active, 2);
	fs_info->endio_write_workers =
		apfs_alloc_workqueue(fs_info, "endio-write", flags,
				      max_active, 2);
	fs_info->endio_freespace_worker =
		apfs_alloc_workqueue(fs_info, "freespace-write", flags,
				      max_active, 0);
	fs_info->delayed_workers =
		apfs_alloc_workqueue(fs_info, "delayed-meta", flags,
				      max_active, 0);
	fs_info->readahead_workers =
		apfs_alloc_workqueue(fs_info, "readahead", flags,
				      max_active, 2);
	fs_info->qgroup_rescan_workers =
		apfs_alloc_workqueue(fs_info, "qgroup-rescan", flags, 1, 0);
	fs_info->discard_ctl.discard_workers =
		alloc_workqueue("apfs_discard", WQ_UNBOUND | WQ_FREEZABLE, 1);

	if (!(fs_info->workers && fs_info->delalloc_workers &&
	      fs_info->flush_workers &&
	      fs_info->endio_workers && fs_info->endio_meta_workers &&
	      fs_info->endio_meta_write_workers &&
	      fs_info->endio_write_workers && fs_info->endio_raid56_workers &&
	      fs_info->endio_freespace_worker && fs_info->rmw_workers &&
	      fs_info->caching_workers && fs_info->readahead_workers &&
	      fs_info->fixup_workers && fs_info->delayed_workers &&
	      fs_info->qgroup_rescan_workers &&
	      fs_info->discard_ctl.discard_workers)) {
		return -ENOMEM;
	}

	return 0;
}

static int apfs_init_csum_hash(struct apfs_fs_info *fs_info, u16 csum_type)
{
	struct crypto_shash *csum_shash;
	const char *csum_driver = apfs_super_csum_driver(csum_type);

	csum_shash = crypto_alloc_shash(csum_driver, 0, 0);

	if (IS_ERR(csum_shash)) {
		apfs_err(fs_info, "error allocating %s hash for checksum",
			  csum_driver);
		return PTR_ERR(csum_shash);
	}

	fs_info->csum_shash = csum_shash;

	return 0;
}

static int apfs_replay_log(struct apfs_fs_info *fs_info,
			    struct apfs_fs_devices *fs_devices)
{
	int ret;
	struct apfs_root *log_tree_root;
	struct apfs_super_block *disk_super = fs_info->super_copy;
	u64 bytenr = apfs_super_log_root(disk_super);
	int level = apfs_super_log_root_level(disk_super);

	if (fs_devices->rw_devices == 0) {
		apfs_warn(fs_info, "log replay required on RO media");
		return -EIO;
	}

	log_tree_root = apfs_alloc_root(fs_info, APFS_TREE_LOG_OBJECTID,
					 GFP_KERNEL);
	if (!log_tree_root)
		return -ENOMEM;

	log_tree_root->node = read_tree_block(fs_info, bytenr,
					      APFS_TREE_LOG_OBJECTID,
					      fs_info->generation + 1, level,
					      NULL);
	if (IS_ERR(log_tree_root->node)) {
		apfs_warn(fs_info, "failed to read log tree");
		ret = PTR_ERR(log_tree_root->node);
		log_tree_root->node = NULL;
		apfs_put_root(log_tree_root);
		return ret;
	} else if (!extent_buffer_uptodate(log_tree_root->node)) {
		apfs_err(fs_info, "failed to read log tree");
		apfs_put_root(log_tree_root);
		return -EIO;
	}
	/* returns with log_tree_root freed on success */
	ret = apfs_recover_log_trees(log_tree_root);
	if (ret) {
		apfs_handle_fs_error(fs_info, ret,
				      "Failed to recover log tree");
		apfs_put_root(log_tree_root);
		return ret;
	}

	if (sb_rdonly(fs_info->sb)) {
		ret = apfs_commit_super(fs_info);
		if (ret)
			return ret;
	}

	return 0;
}

static int apfs_read_nx_roots(struct apfs_fs_info *fs_info)
{
	return 0;
}

static int apfs_read_vol_roots(struct apfs_fs_info *fs_info)
{
	struct apfs_root *omap_root = fs_info->omap_root;
	struct apfs_root *root;
	struct apfs_key location = {};
	struct apfs_vol_superblock *sb = fs_info->__super_copy;
	int ret;
	u64 oid;
	u64 bytenr;
	u64 xid = fs_info->generation;
	BUG_ON(!fs_info->omap_root);

	oid = apfs_volume_super_root_tree(sb);
	ret = apfs_find_omap_paddr(omap_root, oid, xid, &bytenr);
	if (ret)
		goto out;
	root = apfs_read_root(fs_info, APFS_OBJ_TYPE_FSTREE, bytenr);
	if (IS_ERR(root)) {
		apfs_err(fs_info, "failed to read fs tree root");
		ret = PTR_ERR(root);
		goto out;
	}
	fs_info->root_root = root;

	oid = apfs_volume_super_fext_tree(sb);
	if (oid) {
		ret = apfs_find_omap_paddr(omap_root, oid, xid, &bytenr);
		if (ret)
			goto out;
		root = apfs_read_root(fs_info, APFS_OBJ_TYPE_FEXT_TREE, bytenr);
		if (IS_ERR(root)) {
			apfs_err(fs_info, "failed to read fext tree root");
			ret = PTR_ERR(root);
			goto out;
		}
		fs_info->fext_root = root;
	}

	/* Oh, Apple. extref tree oid is physical block number */
	bytenr = apfs_volume_super_extref_tree(sb) << fs_info->block_size_bits;
	if (bytenr) {
		root = apfs_read_root(fs_info, APFS_OBJ_TYPE_EXTENT_LIST_TREE,
				      bytenr);
		if (IS_ERR(root)) {
			apfs_err(fs_info, "failed to read extref tree root");
			ret = PTR_ERR(root);
			goto out;
		}
		fs_info->extref_root = root;
	}

	/* Oh, Apple. snap tree oid is physical block number too */
	bytenr = apfs_volume_super_snap_tree(sb) << fs_info->block_size_bits;
	if (bytenr) {
		root = apfs_read_root(fs_info, APFS_OBJ_TYPE_SNAPTREE, bytenr);
		if (IS_ERR(root)) {
			apfs_err(fs_info, "failed to read snap tree root");
			ret = PTR_ERR(root);
			goto out;
		}

		fs_info->snap_root = root;
	}

	return 0;
out:
	free_root_pointers(fs_info, true);
	apfs_free_fs_roots(fs_info);
	apfs_err(fs_info, "failed to read root (oid=%llu): %d",
		 location.objectid, ret);
	return ret;
}

static inline int apfs_read_roots(struct apfs_fs_info *fs_info)
{
	if (fs_info->index == APFS_DUMMY_FS_INDEX)
		return apfs_read_nx_roots(fs_info);
	else
		return apfs_read_vol_roots(fs_info);
}
/*
 * Real super block validation
 * NOTE: super csum type and incompat features will not be checked here.
 *
 * @sb:		super block to check
 * @mirror_num:	the super block number to check its bytenr:
 * 		0	the primary (1st) sb
 * 		1, 2	2nd and 3rd backup copy
 * 	       -1	skip bytenr check
 */
static int validate_super(struct apfs_fs_info *fs_info,
			    struct apfs_super_block *sb, int mirror_num)
{
	u64 nodesize = apfs_super_nodesize(sb);
	u64 sectorsize = apfs_super_sectorsize(sb);
	int ret = 0;

	if (apfs_super_magic(sb) != APFS_MAGIC) {
		apfs_err(fs_info, "no valid FS found");
		ret = -EINVAL;
	}
	if (apfs_super_flags(sb) & ~APFS_SUPER_FLAG_SUPP) {
		apfs_err(fs_info, "unrecognized or unsupported super flag: %llu",
				apfs_super_flags(sb) & ~APFS_SUPER_FLAG_SUPP);
		ret = -EINVAL;
	}
	if (apfs_super_root_level(sb) >= APFS_MAX_LEVEL) {
		apfs_err(fs_info, "tree_root level too big: %d >= %d",
				apfs_super_root_level(sb), APFS_MAX_LEVEL);
		ret = -EINVAL;
	}
	if (apfs_super_chunk_root_level(sb) >= APFS_MAX_LEVEL) {
		apfs_err(fs_info, "chunk_root level too big: %d >= %d",
				apfs_super_chunk_root_level(sb), APFS_MAX_LEVEL);
		ret = -EINVAL;
	}
	if (apfs_super_log_root_level(sb) >= APFS_MAX_LEVEL) {
		apfs_err(fs_info, "log_root level too big: %d >= %d",
				apfs_super_log_root_level(sb), APFS_MAX_LEVEL);
		ret = -EINVAL;
	}

	/*
	 * Check sectorsize and nodesize first, other check will need it.
	 * Check all possible sectorsize(4K, 8K, 16K, 32K, 64K) here.
	 */
	if (!is_power_of_2(sectorsize) || sectorsize < 4096 ||
	    sectorsize > APFS_MAX_METADATA_BLOCKSIZE) {
		apfs_err(fs_info, "invalid sectorsize %llu", sectorsize);
		ret = -EINVAL;
	}

	/*
	 * For 4K page size, we only support 4K sector size.
	 * For 64K page size, we support read-write for 64K sector size, and
	 * read-only for 4K sector size.
	 */
	if ((PAGE_SIZE == SZ_4K && sectorsize != PAGE_SIZE) ||
	    (PAGE_SIZE == SZ_64K && (sectorsize != SZ_4K &&
				     sectorsize != SZ_64K))) {
		apfs_err(fs_info,
			"sectorsize %llu not yet supported for page size %lu",
			sectorsize, PAGE_SIZE);
		ret = -EINVAL;
	}

	if (!is_power_of_2(nodesize) || nodesize < sectorsize ||
	    nodesize > APFS_MAX_METADATA_BLOCKSIZE) {
		apfs_err(fs_info, "invalid nodesize %llu", nodesize);
		ret = -EINVAL;
	}
	if (nodesize != le32_to_cpu(sb->__unused_leafsize)) {
		apfs_err(fs_info, "invalid leafsize %u, should be %llu",
			  le32_to_cpu(sb->__unused_leafsize), nodesize);
		ret = -EINVAL;
	}

	/* Root alignment check */
	if (!IS_ALIGNED(apfs_super_root(sb), sectorsize)) {
		apfs_warn(fs_info, "tree_root block unaligned: %llu",
			   apfs_super_root(sb));
		ret = -EINVAL;
	}
	if (!IS_ALIGNED(apfs_super_chunk_root(sb), sectorsize)) {
		apfs_warn(fs_info, "chunk_root block unaligned: %llu",
			   apfs_super_chunk_root(sb));
		ret = -EINVAL;
	}
	if (!IS_ALIGNED(apfs_super_log_root(sb), sectorsize)) {
		apfs_warn(fs_info, "log_root block unaligned: %llu",
			   apfs_super_log_root(sb));
		ret = -EINVAL;
	}

	if (memcmp(fs_info->fs_devices->fsid, fs_info->super_copy->fsid,
		   APFS_FSID_SIZE)) {
		apfs_err(fs_info,
		"superblock fsid doesn't match fsid of fs_devices: %pU != %pU",
			fs_info->super_copy->fsid, fs_info->fs_devices->fsid);
		ret = -EINVAL;
	}

	if (apfs_fs_incompat(fs_info, METADATA_UUID) &&
	    memcmp(fs_info->fs_devices->metadata_uuid,
		   fs_info->super_copy->metadata_uuid, APFS_FSID_SIZE)) {
		apfs_err(fs_info,
"superblock metadata_uuid doesn't match metadata uuid of fs_devices: %pU != %pU",
			fs_info->super_copy->metadata_uuid,
			fs_info->fs_devices->metadata_uuid);
		ret = -EINVAL;
	}

	if (memcmp(fs_info->fs_devices->metadata_uuid, sb->dev_item.fsid,
		   APFS_FSID_SIZE) != 0) {
		apfs_err(fs_info,
			"dev_item UUID does not match metadata fsid: %pU != %pU",
			fs_info->fs_devices->metadata_uuid, sb->dev_item.fsid);
		ret = -EINVAL;
	}

	/*
	 * Hint to catch really bogus numbers, bitflips or so, more exact checks are
	 * done later
	 */
	if (apfs_super_bytes_used(sb) < 6 * apfs_super_nodesize(sb)) {
		apfs_err(fs_info, "bytes_used is too small %llu",
			  apfs_super_bytes_used(sb));
		ret = -EINVAL;
	}
	if (!is_power_of_2(apfs_super_stripesize(sb))) {
		apfs_err(fs_info, "invalid stripesize %u",
			  apfs_super_stripesize(sb));
		ret = -EINVAL;
	}
	if (apfs_super_num_devices(sb) > (1UL << 31))
		apfs_warn(fs_info, "suspicious number of devices: %llu",
			   apfs_super_num_devices(sb));
	if (apfs_super_num_devices(sb) == 0) {
		apfs_err(fs_info, "number of devices is 0");
		ret = -EINVAL;
	}

	if (mirror_num >= 0 &&
	    apfs_super_bytenr(sb) != apfs_sb_offset(mirror_num)) {
		apfs_err(fs_info, "super offset mismatch %llu != %u",
			  apfs_super_bytenr(sb), APFS_SUPER_INFO_OFFSET);
		ret = -EINVAL;
	}

	/*
	 * Obvious sys_chunk_array corruptions, it must hold at least one key
	 * and one chunk
	 */
	if (apfs_super_sys_array_size(sb) > APFS_SYSTEM_CHUNK_ARRAY_SIZE) {
		apfs_err(fs_info, "system chunk array too big %u > %u",
			  apfs_super_sys_array_size(sb),
			  APFS_SYSTEM_CHUNK_ARRAY_SIZE);
		ret = -EINVAL;
	}
	if (apfs_super_sys_array_size(sb) < sizeof(struct apfs_disk_key)
			+ sizeof(struct apfs_chunk)) {
		apfs_err(fs_info, "system chunk array too small %u < %zu",
			  apfs_super_sys_array_size(sb),
			  sizeof(struct apfs_disk_key)
			  + sizeof(struct apfs_chunk));
		ret = -EINVAL;
	}

	/*
	 * The generation is a global counter, we'll trust it more than the others
	 * but it's still possible that it's the one that's wrong.
	 */
	if (apfs_super_generation(sb) < apfs_super_chunk_root_generation(sb))
		apfs_warn(fs_info,
			"suspicious: generation < chunk_root_generation: %llu < %llu",
			apfs_super_generation(sb),
			apfs_super_chunk_root_generation(sb));
	if (apfs_super_generation(sb) < apfs_super_cache_generation(sb)
	    && apfs_super_cache_generation(sb) != (u64)-1)
		apfs_warn(fs_info,
			"suspicious: generation < cache_generation: %llu < %llu",
			apfs_super_generation(sb),
			apfs_super_cache_generation(sb));

	return ret;
}

/*
 * Validation of super block at mount time.
 * Some checks already done early at mount time, like csum type and incompat
 * flags will be skipped.
 */
static int apfs_validate_mount_super(struct apfs_fs_info *fs_info)
{
	return validate_super(fs_info, fs_info->super_copy, 0);
}

/*
 * Validation of super block at write time.
 * Some checks like bytenr check will be skipped as their values will be
 * overwritten soon.
 * Extra checks like csum type and incompat flags will be done here.
 */
static int apfs_validate_write_super(struct apfs_fs_info *fs_info,
				      struct apfs_super_block *sb)
{
	int ret;

	ret = validate_super(fs_info, sb, -1);
	if (ret < 0)
		goto out;
	if (!apfs_supported_super_csum(apfs_super_csum_type(sb))) {
		ret = -EUCLEAN;
		apfs_err(fs_info, "invalid csum type, has %u want %u",
			  apfs_super_csum_type(sb), APFS_CSUM_TYPE_CRC32);
		goto out;
	}
	if (apfs_super_incompat_flags(sb) & ~APFS_FEATURE_INCOMPAT_SUPP) {
		ret = -EUCLEAN;
		apfs_err(fs_info,
		"invalid incompat flags, has 0x%llx valid mask 0x%llx",
			  apfs_super_incompat_flags(sb),
			  (unsigned long long)APFS_FEATURE_INCOMPAT_SUPP);
		goto out;
	}
out:
	if (ret < 0)
		apfs_err(fs_info,
		"super block corruption detected before writing it to disk");
	return ret;
}

static int __cold init_tree_roots(struct apfs_fs_info *fs_info)
{
	int backup_index = find_newest_super_backup(fs_info);
	struct apfs_super_block *sb = fs_info->super_copy;
	struct apfs_root *tree_root = fs_info->tree_root;
	bool handle_error = false;
	int ret = 0;
	int i;

	for (i = 0; i < APFS_NUM_BACKUP_ROOTS; i++) {
		u64 generation;
		int level;

		if (handle_error) {
			if (!IS_ERR(tree_root->node))
				free_extent_buffer(tree_root->node);
			tree_root->node = NULL;

			if (!apfs_test_opt(fs_info, USEBACKUPROOT))
				break;

			free_root_pointers(fs_info, 0);

			/*
			 * Don't use the log in recovery mode, it won't be
			 * valid
			 */
			apfs_set_super_log_root(sb, 0);

			/* We can't trust the free space cache either */
			apfs_set_opt(fs_info->mount_opt, CLEAR_CACHE);

			ret = read_backup_root(fs_info, i);
			backup_index = ret;
			if (ret < 0)
				return ret;
		}
		generation = apfs_super_generation(sb);
		level = apfs_super_root_level(sb);
		tree_root->node = read_tree_block(fs_info, apfs_super_root(sb),
						  APFS_ROOT_TREE_OBJECTID,
						  generation, level, NULL);
		if (IS_ERR(tree_root->node)) {
			handle_error = true;
			ret = PTR_ERR(tree_root->node);
			tree_root->node = NULL;
			apfs_warn(fs_info, "couldn't read tree root");
			continue;

		} else if (!extent_buffer_uptodate(tree_root->node)) {
			handle_error = true;
			ret = -EIO;
			apfs_warn(fs_info, "error while reading tree root");
			continue;
		}

		apfs_set_root_node(&tree_root->root_item, tree_root->node);
		tree_root->commit_root = apfs_root_node(tree_root);
		apfs_set_root_refs(&tree_root->root_item, 1);

		/*
		 * No need to hold apfs_root::objectid_mutex since the fs
		 * hasn't been fully initialised and we are the only user
		 */
		ret = apfs_init_root_free_objectid(tree_root);
		if (ret < 0) {
			handle_error = true;
			continue;
		}

		ASSERT(tree_root->free_objectid <= APFS_LAST_FREE_OBJECTID);

		ret = apfs_read_roots(fs_info);
		if (ret < 0) {
			handle_error = true;
			continue;
		}

		/* All successful */
		fs_info->generation = generation;
		fs_info->last_trans_committed = generation;

		/* Always begin writing backup roots after the one being used */
		if (backup_index < 0) {
			fs_info->backup_root_index = 0;
		} else {
			fs_info->backup_root_index = backup_index + 1;
			fs_info->backup_root_index %= APFS_NUM_BACKUP_ROOTS;
		}
		break;
	}

	return ret;
}

void apfs_init_nx_info(struct apfs_nx_info *nx_info)
{
	spin_lock_init(&nx_info->vol_lock);
	refcount_set(&nx_info->refs, 1);
}

void apfs_init_fs_info(struct apfs_fs_info *fs_info)
{
	INIT_RADIX_TREE(&fs_info->fs_roots_radix, GFP_ATOMIC);
	INIT_RADIX_TREE(&fs_info->buffer_radix, GFP_ATOMIC);
	INIT_LIST_HEAD(&fs_info->trans_list);
	INIT_LIST_HEAD(&fs_info->dead_roots);
	INIT_LIST_HEAD(&fs_info->delayed_iputs);
	INIT_LIST_HEAD(&fs_info->delalloc_roots);
	INIT_LIST_HEAD(&fs_info->caching_block_groups);
	spin_lock_init(&fs_info->delalloc_root_lock);
	spin_lock_init(&fs_info->trans_lock);
	spin_lock_init(&fs_info->fs_roots_radix_lock);
	spin_lock_init(&fs_info->delayed_iput_lock);
	spin_lock_init(&fs_info->defrag_inodes_lock);
	spin_lock_init(&fs_info->super_lock);
	spin_lock_init(&fs_info->buffer_lock);
	spin_lock_init(&fs_info->unused_bgs_lock);
	spin_lock_init(&fs_info->treelog_bg_lock);
	rwlock_init(&fs_info->tree_mod_log_lock);
	mutex_init(&fs_info->unused_bg_unpin_mutex);
	mutex_init(&fs_info->reclaim_bgs_lock);
	mutex_init(&fs_info->reloc_mutex);
	mutex_init(&fs_info->delalloc_root_mutex);
	mutex_init(&fs_info->zoned_meta_io_lock);
	seqlock_init(&fs_info->profiles_lock);

	INIT_LIST_HEAD(&fs_info->dirty_cowonly_roots);
	INIT_LIST_HEAD(&fs_info->space_info);
	INIT_LIST_HEAD(&fs_info->tree_mod_seq_list);
	INIT_LIST_HEAD(&fs_info->unused_bgs);
	INIT_LIST_HEAD(&fs_info->reclaim_bgs);
#ifdef CONFIG_APFS_DEBUG
	INIT_LIST_HEAD(&fs_info->allocated_roots);
	INIT_LIST_HEAD(&fs_info->allocated_ebs);
	spin_lock_init(&fs_info->eb_leak_lock);
#endif
	extent_map_tree_init(&fs_info->mapping_tree);
	apfs_init_block_rsv(&fs_info->global_block_rsv,
			     APFS_BLOCK_RSV_GLOBAL);
	apfs_init_block_rsv(&fs_info->trans_block_rsv, APFS_BLOCK_RSV_TRANS);
	apfs_init_block_rsv(&fs_info->chunk_block_rsv, APFS_BLOCK_RSV_CHUNK);
	apfs_init_block_rsv(&fs_info->empty_block_rsv, APFS_BLOCK_RSV_EMPTY);
	apfs_init_block_rsv(&fs_info->delayed_block_rsv,
			     APFS_BLOCK_RSV_DELOPS);
	apfs_init_block_rsv(&fs_info->delayed_refs_rsv,
			     APFS_BLOCK_RSV_DELREFS);

	atomic_set(&fs_info->async_delalloc_pages, 0);
	atomic_set(&fs_info->defrag_running, 0);
	atomic_set(&fs_info->reada_works_cnt, 0);
	atomic_set(&fs_info->nr_delayed_iputs, 0);
	atomic64_set(&fs_info->tree_mod_seq, 0);
	fs_info->max_inline = APFS_DEFAULT_MAX_INLINE;
	fs_info->metadata_ratio = 0;
	fs_info->defrag_inodes = RB_ROOT;
	atomic64_set(&fs_info->free_chunk_space, 0);
	fs_info->tree_mod_log = RB_ROOT;
	fs_info->commit_interval = APFS_DEFAULT_COMMIT_INTERVAL;
	fs_info->avg_delayed_ref_runtime = NSEC_PER_SEC >> 6; /* div by 64 */
	/* readahead state */
	INIT_RADIX_TREE(&fs_info->reada_tree, GFP_NOFS & ~__GFP_DIRECT_RECLAIM);
	spin_lock_init(&fs_info->reada_lock);
	apfs_init_ref_verify(fs_info);

	fs_info->thread_pool_size = min_t(unsigned long,
					  num_online_cpus() + 2, 8);

	INIT_LIST_HEAD(&fs_info->ordered_roots);
	spin_lock_init(&fs_info->ordered_root_lock);

	apfs_init_scrub(fs_info);
#ifdef CONFIG_APFS_FS_CHECK_INTEGRITY
	fs_info->check_integrity_print_mask = 0;
#endif
	apfs_init_balance(fs_info);
	apfs_init_async_reclaim_work(fs_info);

	spin_lock_init(&fs_info->block_group_cache_lock);
	fs_info->block_group_cache_tree = RB_ROOT;
	fs_info->first_logical_byte = (u64)-1;

	extent_io_tree_init(fs_info, &fs_info->excluded_extents,
			    IO_TREE_FS_EXCLUDED_EXTENTS, NULL);
	set_bit(APFS_FS_BARRIER, &fs_info->flags);

	mutex_init(&fs_info->ordered_operations_mutex);
	mutex_init(&fs_info->tree_log_mutex);
	mutex_init(&fs_info->chunk_mutex);
	mutex_init(&fs_info->transaction_kthread_mutex);
	mutex_init(&fs_info->cleaner_mutex);
	mutex_init(&fs_info->ro_block_group_mutex);
	init_rwsem(&fs_info->commit_root_sem);
	init_rwsem(&fs_info->cleanup_work_sem);
	init_rwsem(&fs_info->subvol_sem);
	sema_init(&fs_info->uuid_tree_rescan_sem, 1);

	apfs_init_dev_replace_locks(fs_info);
	apfs_init_qgroup(fs_info);
	apfs_discard_init(fs_info);

	apfs_init_free_cluster(&fs_info->meta_alloc_cluster);
	apfs_init_free_cluster(&fs_info->data_alloc_cluster);

	init_waitqueue_head(&fs_info->transaction_throttle);
	init_waitqueue_head(&fs_info->transaction_wait);
	init_waitqueue_head(&fs_info->transaction_blocked_wait);
	init_waitqueue_head(&fs_info->async_submit_wait);
	init_waitqueue_head(&fs_info->delayed_iputs_wait);

	/* Usable values until the real ones are cached from the superblock */
	fs_info->nodesize = 4096;
	fs_info->sectorsize = 4096;
	fs_info->sectorsize_bits = ilog2(4096);
	fs_info->stripesize = 4096;

	spin_lock_init(&fs_info->swapfile_pins_lock);
	fs_info->swapfile_pins = RB_ROOT;

	spin_lock_init(&fs_info->send_reloc_lock);
	fs_info->send_in_progress = 0;

	fs_info->bg_reclaim_threshold = APFS_DEFAULT_RECLAIM_THRESH;
	INIT_WORK(&fs_info->reclaim_bgs_work, apfs_reclaim_bgs_work);
}

static int init_mount_fs_info(struct apfs_fs_info *fs_info,
			      struct apfs_nx_info *nx_info,
			      struct super_block *sb)
{
	int ret;

	fs_info->sb = sb;
	fs_info->nx_info = nx_info;
	fs_info->device = nx_info->device;
	fs_info->nx_info = nx_info;

	fs_info->block_size = nx_info->block_size;
	fs_info->block_size_bits = blksize_bits(fs_info->block_size);
	fs_info->node_size = nx_info->block_size;
	fs_info->sectorsize = nx_info->block_size;
	fs_info->sectorsize_bits = fs_info->block_size_bits;

	ret = percpu_counter_init(&fs_info->ordered_bytes, 0, GFP_KERNEL);
	if (ret)
		return ret;

	ret = percpu_counter_init(&fs_info->dirty_metadata_bytes, 0, GFP_KERNEL);
	if (ret)
		return ret;

	fs_info->dirty_metadata_batch = PAGE_SIZE *
					(1 + ilog2(nr_cpu_ids));

	ret = percpu_counter_init(&fs_info->delalloc_bytes, 0, GFP_KERNEL);
	if (ret)
		return ret;

	ret = percpu_counter_init(&fs_info->dev_replace.bio_counter, 0,
			GFP_KERNEL);
	if (ret)
		return ret;

	fs_info->delayed_root = kmalloc(sizeof(struct apfs_delayed_root),
					GFP_KERNEL);
	if (!fs_info->delayed_root)
		return -ENOMEM;
	apfs_init_delayed_root(fs_info->delayed_root);

	if (sb_rdonly(sb))
		set_bit(APFS_FS_STATE_RO, &fs_info->fs_state);

	ret = apfs_init_workqueues(fs_info);
	if (ret)
		return ret;

	return apfs_alloc_stripe_hash_table(fs_info);
}

static int apfs_uuid_rescan_kthread(void *data)
{
	struct apfs_fs_info *fs_info = (struct apfs_fs_info *)data;
	int ret;

	/*
	 * 1st step is to iterate through the existing UUID tree and
	 * to delete all entries that contain outdated data.
	 * 2nd step is to add all missing entries to the UUID tree.
	 */
	ret = apfs_uuid_tree_iterate(fs_info);
	if (ret < 0) {
		if (ret != -EINTR)
			apfs_warn(fs_info, "iterating uuid_tree failed %d",
				   ret);
		up(&fs_info->uuid_tree_rescan_sem);
		return ret;
	}
	return apfs_uuid_scan_kthread(data);
}

/*
 * Some options only have meaning at mount time and shouldn't persist across
 * remounts, or be displayed. Clear these at the end of mount and remount
 * code paths.
 */
void apfs_clear_oneshot_options(struct apfs_fs_info *fs_info)
{
	apfs_clear_opt(fs_info->mount_opt, USEBACKUPROOT);
	apfs_clear_opt(fs_info->mount_opt, CLEAR_CACHE);
}

/*
 * Mounting logic specific to read-write file systems. Shared by open_ctree
 * and apfs_remount when remounting from read-only to read-write.
 */
int apfs_start_pre_rw_mount(struct apfs_fs_info *fs_info)
{
	int ret;
	const bool cache_opt = apfs_test_opt(fs_info, SPACE_CACHE);
	bool clear_free_space_tree = false;

	if (apfs_test_opt(fs_info, CLEAR_CACHE) &&
	    apfs_fs_compat_ro(fs_info, FREE_SPACE_TREE)) {
		clear_free_space_tree = true;
	} else if (apfs_fs_compat_ro(fs_info, FREE_SPACE_TREE) &&
		   !apfs_fs_compat_ro(fs_info, FREE_SPACE_TREE_VALID)) {
		apfs_warn(fs_info, "free space tree is invalid");
		clear_free_space_tree = true;
	}

	if (clear_free_space_tree) {
		apfs_info(fs_info, "clearing free space tree");
		ret = apfs_clear_free_space_tree(fs_info);
		if (ret) {
			apfs_warn(fs_info,
				   "failed to clear free space tree: %d", ret);
			goto out;
		}
	}

	/*
	 * apfs_find_orphan_roots() is responsible for finding all the dead
	 * roots (with 0 refs), flag them with APFS_ROOT_DEAD_TREE and load
	 * them into the fs_info->fs_roots_radix tree. This must be done before
	 * calling apfs_orphan_cleanup() on the tree root. If we don't do it
	 * first, then apfs_orphan_cleanup() will delete a dead root's orphan
	 * item before the root's tree is deleted - this means that if we unmount
	 * or crash before the deletion completes, on the next mount we will not
	 * delete what remains of the tree because the orphan item does not
	 * exists anymore, which is what tells us we have a pending deletion.
	 */
	ret = apfs_find_orphan_roots(fs_info);
	if (ret)
		goto out;

	ret = apfs_cleanup_fs_roots(fs_info);
	if (ret)
		goto out;

	down_read(&fs_info->cleanup_work_sem);
	if ((ret = apfs_orphan_cleanup(fs_info->fs_root)) ||
	    (ret = apfs_orphan_cleanup(fs_info->tree_root))) {
		up_read(&fs_info->cleanup_work_sem);
		goto out;
	}
	up_read(&fs_info->cleanup_work_sem);

	mutex_lock(&fs_info->cleaner_mutex);
	ret = apfs_recover_relocation(fs_info->tree_root);
	mutex_unlock(&fs_info->cleaner_mutex);
	if (ret < 0) {
		apfs_warn(fs_info, "failed to recover relocation: %d", ret);
		goto out;
	}

	if (apfs_test_opt(fs_info, FREE_SPACE_TREE) &&
	    !apfs_fs_compat_ro(fs_info, FREE_SPACE_TREE)) {
		apfs_info(fs_info, "creating free space tree");
		ret = apfs_create_free_space_tree(fs_info);
		if (ret) {
			apfs_warn(fs_info,
				"failed to create free space tree: %d", ret);
			goto out;
		}
	}

	if (cache_opt != apfs_free_space_cache_v1_active(fs_info)) {
		ret = apfs_set_free_space_cache_v1_active(fs_info, cache_opt);
		if (ret)
			goto out;
	}

	ret = apfs_resume_balance_async(fs_info);
	if (ret)
		goto out;

	ret = apfs_resume_dev_replace_async(fs_info);
	if (ret) {
		apfs_warn(fs_info, "failed to resume dev_replace");
		goto out;
	}

	apfs_qgroup_rescan_resume(fs_info);

	if (!fs_info->uuid_root) {
		apfs_info(fs_info, "creating UUID tree");
		ret = apfs_create_uuid_tree(fs_info);
		if (ret) {
			apfs_warn(fs_info,
				   "failed to create the UUID tree %d", ret);
			goto out;
		}
	}

out:
	return ret;
}

static int
apfs_read_omap_phys(struct apfs_fs_info *fs_info, u64 bytenr,
	       struct apfs_omap_phys *omap)
{
	return apfs_read_generic(fs_info->device->bdev, bytenr, sizeof(*omap),
				 omap);
}

static int apfs_check_omap(struct apfs_omap_phys *omap)
{
	int ret;

	return 0;
	ret = apfs_verify_obj_csum(omap, sizeof(*omap));
	return ret;
}

static noinline int
apfs_setup_omap_root(struct apfs_fs_info *fs_info)
{
	struct apfs_omap_phys omap;
	struct apfs_root *omap_root = fs_info->omap_root;
	int ret;
	u64 oid;
	u64 bytenr;

	if (fs_info->index == APFS_DUMMY_FS_INDEX)
		oid = apfs_nx_super_omap_oid(fs_info->nx_info->super_copy);
	else
		oid = apfs_volume_super_omap_oid(fs_info->__super_copy);

	bytenr = oid << fs_info->block_size_bits;

	ret = apfs_read_omap_phys(fs_info, bytenr, &omap);
	if (ret) {
		apfs_err(fs_info, "failed to read omap at %llu", bytenr);
		goto out;
	}

	ret = apfs_check_omap(&omap);
	if (ret) {
		apfs_err(fs_info, "found bad omap at %llu %d", bytenr,
			ret == -EUCLEAN);
		goto out;
	}

	oid = apfs_omap_phys_tree_oid(&omap);

	omap_root->node = read_tree_block(fs_info, oid << fs_info->block_size_bits,
					  APFS_OMAP_ROOT, 0, -1, NULL);
	if (IS_ERR(omap_root->node) ||
		!extent_buffer_uptodate(omap_root->node)) {
		apfs_err(fs_info, "failed to read omap tree: %llu %d %d\n", oid,
			 IS_ERR(omap_root->node), !extent_buffer_uptodate(omap_root->node));
		omap_root->node = NULL;
		goto out;
	}

out:
	return ret;
}

static noinline int __cold apfs_alloc_dummy_fs_info(struct super_block *super,
						    struct apfs_nx_info *nx_info)
{
	struct apfs_fs_info *fs_info;

	if (nx_info->vol)
		return 0;

	fs_info = kvzalloc(sizeof(struct apfs_fs_info), GFP_KERNEL);
	if (!fs_info)
		return -ENOMEM;

	fs_info->index = APFS_DUMMY_FS_INDEX;

	nx_info->vol = fs_info;
	fs_info->nx_info = nx_info;

	return 0;
}

static noinline int __cold apfs_setup_dummy_fs_info(struct super_block *sb,
						    struct apfs_nx_info *nx_info)
{
	int ret;
	struct apfs_fs_info *fs_info;
	struct apfs_root *omap_root;

	nx_info->sb = sb;
	ret = apfs_alloc_dummy_fs_info(sb, nx_info);
	if (ret)
		goto fail;

	fs_info = nx_info->vol;
	apfs_init_fs_info(fs_info);

	ret = init_mount_fs_info(fs_info, nx_info, nx_info->sb);
	if (ret) {
		apfs_crit(NULL, "failed to init dummy mount fs_info %d", ret);
		goto fail_init_fs_info;
	}

	omap_root = apfs_alloc_root(fs_info, APFS_OBJ_TYPE_OMAP,
				    GFP_KERNEL);
	if (!omap_root) {
		ret = -ENOMEM;
		goto fail_init_mount_fs_info;
	}

	nx_info->omap_root = omap_root;
	fs_info->omap_root = nx_info->omap_root;

	fs_info->btree_inode = new_inode(nx_info->sb);
	if (!fs_info->btree_inode) {
		ret = -ENOMEM;
		goto fail_alloc_omap_root;
	}

	mapping_set_gfp_mask(fs_info->btree_inode->i_mapping, GFP_NOFS);
	apfs_init_btree_inode(fs_info);

	ret = apfs_setup_omap_root(fs_info);
	if (ret) {
		free_root_pointers(fs_info, true);
		goto fail_init_btree_inode;
	}
	return 0;

fail_init_btree_inode:
	invalidate_inode_pages2(fs_info->btree_inode->i_mapping);
	iput(fs_info->btree_inode);
fail_alloc_omap_root:
	kfree(omap_root);
	nx_info->omap_root = NULL;
	fs_info->omap_root = NULL;
fail_init_mount_fs_info:
	apfs_stop_all_workers(fs_info);
fail_init_fs_info:
	apfs_mapping_tree_free(&fs_info->mapping_tree);
fail:
	return ret;
}

struct apfs_vol_superblock *
apfs_read_volume_super(struct apfs_nx_info *nx_info, int index);

static int apfs_validate_nx_super(const struct apfs_nx_superblock *sb)
{
	int ret;

	ret = apfs_verify_obj_csum(&sb->o, APFS_SUPER_INFO_SIZE);
	if (ret) {
		apfs_err(NULL, "nx superblock csum mismatch");
	}

	return ret;
}

static int apfs_validate_volume_super(const struct apfs_vol_superblock *sb,
				      size_t size)
{
	int ret;

	/* don't about csum of a snapshot volume super */
	if (size != APFS_SUPER_INFO_SIZE)
		return 0;

	ret = apfs_verify_obj_csum(&sb->o, APFS_SUPER_INFO_SIZE);
	if (ret) {
		apfs_err(NULL, "volume superblock csum mismatch");
	}

	return ret;
}

static int
apfs_check_volume_incompat_features(const struct apfs_vol_superblock *sb)
{
	u64 features = apfs_volume_super_incompat_features(sb);

	if (features & APFS_INCOMPAT_SEALED_VOLUME) {
		apfs_info(NULL, "sealed volume is not supported");
		return -EOPNOTSUPP;
	}
	return 0;
}

static struct apfs_vol_superblock * __cold
read_snapshot_super(struct apfs_fs_info *fs_info, u64 xid)
{
	struct apfs_key key;
	struct apfs_path *path;
	struct apfs_snap_meta *sm;
	struct apfs_vol_superblock *super;
	int ret;
	u64 bytenr;

	if (!fs_info->snap_root)
		return ERR_PTR(-ENOENT);

	path = apfs_alloc_path();
	if (!path)
		return ERR_PTR(-ENOMEM);

	key.objectid = xid;
	key.type = APFS_TYPE_SNAP_METADATA;
	key.offset = 0;
	ret = apfs_search_slot(NULL, fs_info->snap_root, &key, path, 0, 0);
	if (ret > 0)
		ret = -ENOENT;
	if (ret)
		goto out;

	sm = apfs_item_ptr(path->nodes[0], path->slots[0], struct apfs_snap_meta);
	bytenr = apfs_snap_super_bno(path->nodes[0], sm) << fs_info->block_size_bits;

	super = apfs_read_dev_volume_super(fs_info, bytenr, sizeof(*super));
	if (IS_ERR(super)) {
		ret = PTR_ERR(super);
		goto out;
	}
	super->extref_tree_oid = cpu_to_le64(apfs_snap_extref_tree_oid(path->nodes[0], sm));
	super->extref_tree_type = cpu_to_le32(apfs_snap_extref_tree_type(path->nodes[0], sm));
	apfs_free_path(path);
	return super;
out:
	apfs_free_path(path);
	return ERR_PTR(ret);
}

static void __cold close_dummy_fs_info(struct apfs_nx_info *nx_info);
int __cold open_ctree(struct super_block *sb, struct apfs_device *device,
		      char *options)
{
	struct apfs_vol_superblock *disk_super;
	struct apfs_nx_superblock *disk_nx_super;
	struct apfs_fs_info *fs_info = apfs_sb(sb);
	struct apfs_nx_info *nx_info = device->nx_info;
	struct apfs_root *omap_root;
	u64 snap_xid = 0;
	int ret;
	u64 bytenr;

	fs_info->nx_info = NULL;
	if (device->nx_info)
		goto open_fs_info;

	nx_info = kvzalloc(sizeof(*nx_info), GFP_KERNEL);
	nx_info->super_copy = kzalloc(sizeof(struct apfs_nx_superblock),
				      GFP_KERNEL);
	if (!nx_info || !nx_info->super_copy) {
		ret = -ENOMEM;
		kvfree(nx_info);
		kfree(nx_info->super_copy);
		goto fail;
	}

	nx_info->device = device;
	device->nx_info = nx_info;

	apfs_init_nx_info(nx_info);

	invalidate_bdev(device->bdev);
	disk_nx_super = apfs_read_nx_super(device->bdev);
	if (IS_ERR(disk_super)) {
		ret = PTR_ERR(disk_nx_super);
		kvfree(nx_info);
		kfree(nx_info->super_copy);
		goto fail;
	}

	memcpy(nx_info->super_copy, disk_nx_super, sizeof(*nx_info->super_copy));
	apfs_release_nx_super(disk_nx_super);

	nx_info->block_size = apfs_nx_super_block_size(nx_info->super_copy);
	nx_info->block_count = apfs_nx_super_block_count(nx_info->super_copy);
	nx_info->block_size_bits = blksize_bits(nx_info->block_size);
	nx_info->generation = apfs_nx_super_xid(nx_info->super_copy);

	ret = apfs_setup_dummy_fs_info(sb, nx_info);
	if (ret) {
		kvfree(nx_info);
		kfree(nx_info->super_copy);
		goto fail;
	}

open_fs_info:
	nx_info = device->nx_info;
	fs_info->nx_info = nx_info;
	apfs_get_nx_info(nx_info);

	disk_super = apfs_read_volume_super(nx_info, fs_info->index);
	if (!disk_super)
		disk_super = ERR_PTR(-ENOENT);
	if (IS_ERR(disk_super)) {
		ret = PTR_ERR(disk_super);
		goto fail_setup_nx_info;
	}

	ret = apfs_validate_volume_super(disk_super, APFS_SUPER_INFO_SIZE);
	if (ret) {
		apfs_release_volume_super(disk_super);
		goto fail_setup_nx_info;
	}

	ret = apfs_check_volume_incompat_features(disk_super);
	if (ret) {
		apfs_release_volume_super(disk_super);
		goto fail_setup_nx_info;
	}
	fs_info = APFS_SB(sb);
	fs_info->__super_copy = kzalloc(sizeof(struct apfs_vol_superblock),
					GFP_KERNEL);
	if (fs_info->__super_copy == NULL) {
		apfs_release_volume_super(disk_super);
		ret = -ENOMEM;
		goto fail_setup_nx_info;
	}
	memcpy(fs_info->__super_copy, disk_super, sizeof(struct apfs_vol_superblock));
	apfs_release_volume_super(disk_super);

	fs_info->normalization_insensitive = apfs_is_normalization_insensitive(fs_info->__super_copy);
	fs_info->generation = apfs_volume_super_xid(fs_info->__super_copy);

	ret = init_mount_fs_info(fs_info, nx_info, sb);
	if (ret) {
		goto fail_alloc_super;
	}

	omap_root = apfs_alloc_root(fs_info, APFS_OBJ_TYPE_OMAP, GFP_KERNEL);
	if (!omap_root) {
		ret = -ENOMEM;
		goto fail_init_mount_fs_info;
	}
	fs_info->omap_root = omap_root;

	fs_info->btree_inode = new_inode(sb);
	if (!fs_info->btree_inode) {
		ret = -ENOMEM;
		goto fail_alloc_omap_root;
	}
	mapping_set_gfp_mask(fs_info->btree_inode->i_mapping, GFP_NOFS);
	apfs_init_btree_inode(fs_info);

	ret = apfs_setup_omap_root(fs_info);
	if (ret)
		goto fail_init_btree_inode;

	sb->s_bdi->ra_pages = max(sb->s_bdi->ra_pages, SZ_4M / PAGE_SIZE);
	sb->s_blocksize = nx_info->block_size;
	sb->s_blocksize_bits = blksize_bits(nx_info->block_size);

	memcpy(&sb->s_uuid, &fs_info->__super_copy->uuid, APFS_UUID_SIZE);

	ret = apfs_read_roots(fs_info);
	if (ret)
		goto fail_setup_omap_root;

	set_bit(APFS_FS_OPEN, &fs_info->flags);
	/* snapshot mounted */
	if (snap_xid)
		return 0;

	/* let's mount the snapshot */
	if (!fs_info->xid)
		return 0;

	snap_xid = fs_info->xid;
	disk_super = read_snapshot_super(fs_info, snap_xid);
	if (!disk_super)
		disk_super = ERR_PTR(-ENOENT);
	if (IS_ERR(disk_super)) {
		ret = PTR_ERR(disk_super);
		goto fail_setup_omap_root;
	}

	ret = apfs_validate_volume_super(disk_super, sizeof(*disk_super));
	if (ret) {
		apfs_release_volume_super(disk_super);
		goto fail_setup_omap_root;
	}

	memcpy(fs_info->__super_copy, disk_super, sizeof(struct apfs_vol_superblock));
	apfs_release_volume_super(disk_super);
	disk_super = fs_info->__super_copy;

	free_extent_buffer(fs_info->extref_root->node);
	apfs_put_root(fs_info->extref_root);

	bytenr = apfs_volume_super_extref_tree(disk_super) << fs_info->block_size_bits;
	fs_info->extref_root = apfs_read_root(fs_info, APFS_OBJ_TYPE_EXTENT_LIST_TREE,
					      bytenr);
	if (IS_ERR(fs_info->extref_root)) {
		apfs_err(fs_info, "failed to read extref tree root");
		ret = PTR_ERR(fs_info->extref_root);
		fs_info->extref_root = NULL;
		goto fail_setup_omap_root;
	}

	return 0;

fail_setup_omap_root:
fail_init_btree_inode:
fail_alloc_omap_root:
fail_init_mount_fs_info:
fail_alloc_super:
fail_setup_nx_info:
fail:
	close_ctree(fs_info);
	return ret;
}
ALLOW_ERROR_INJECTION(open_ctree, ERRNO);

static void apfs_end_super_write(struct bio *bio)
{
	struct apfs_device *device = bio->bi_private;
	struct bio_vec *bvec;
	struct bvec_iter_all iter_all;
	struct page *page;

	bio_for_each_segment_all(bvec, bio, iter_all) {
		page = bvec->bv_page;

		if (bio->bi_status) {
			apfs_warn_rl_in_rcu(device->fs_info,
				"lost page write due to IO error on %s (%d)",
				rcu_str_deref(device->name),
				blk_status_to_errno(bio->bi_status));
			ClearPageUptodate(page);
			SetPageError(page);
			apfs_dev_stat_inc_and_print(device,
						     APFS_DEV_STAT_WRITE_ERRS);
		} else {
			SetPageUptodate(page);
		}

		put_page(page);
		unlock_page(page);
	}

	bio_put(bio);
}

struct apfs_super_block *apfs_read_dev_one_super(struct block_device *bdev,
						   int copy_num)
{
	struct apfs_super_block *super;
	struct page *page;
	u64 bytenr, bytenr_orig;
	struct address_space *mapping = bdev->bd_inode->i_mapping;
	int ret;

	bytenr_orig = apfs_sb_offset(copy_num);
	ret = apfs_sb_log_location_bdev(bdev, copy_num, READ, &bytenr);
	if (ret == -ENOENT)
		return ERR_PTR(-EINVAL);
	else if (ret)
		return ERR_PTR(ret);

	if (bytenr + APFS_SUPER_INFO_SIZE >= i_size_read(bdev->bd_inode))
		return ERR_PTR(-EINVAL);

	invalidate_bdev(bdev);
	page = read_cache_page_gfp(mapping, bytenr >> PAGE_SHIFT, GFP_NOFS);
	if (IS_ERR(page))
		return ERR_CAST(page);

	super = page_address(page);
	if (apfs_super_magic(super) != APFS_MAGIC) {
		apfs_release_disk_super(super);
		return ERR_PTR(-ENODATA);
	}

	if (apfs_super_bytenr(super) != bytenr_orig) {
		apfs_release_disk_super(super);
		return ERR_PTR(-EINVAL);
	}

	return super;
}

noinline struct apfs_vol_superblock *
apfs_read_dev_volume_super(struct apfs_fs_info *fs_info, u64 bytenr, u64 size)

{
	struct apfs_vol_superblock *super;
	struct apfs_vol_superblock *super_tmp;
	struct block_device *bdev = fs_info->device->bdev;
	struct apfs_buf *buf;
	int ret;

	if (bytenr + size >= i_size_read(bdev->bd_inode))
		return ERR_PTR(-EINVAL);

	super = kzalloc(max_t(size_t, APFS_SUPER_INFO_SIZE, size), GFP_NOFS);
	if (!super)
		return ERR_PTR(-ENOMEM);

	buf = apfs_buf_alloc();
	if (!buf) {
		ret = -ENOMEM;
		goto fail_super;
	}

	apfs_buf_init(fs_info, buf, ABF_READ, bytenr, size);
	ret = apfs_buf_alloc_pages(buf, GFP_NOFS);
	if (ret) {
		ret = -ENOMEM;
		goto fail_buf;
	}

	ret = apfs_buf_submit(buf, true);
	if (ret)
		goto fail_buf;

	super_tmp = page_address(buf->pages[0]) + buf->offset;
	if (apfs_volume_super_magic(super_tmp) != APFS_VOLUME_MAGIC) {
		ret = -ENODATA;
		goto fail_buf;
	}

	memcpy(super, super_tmp, min_t(size_t, buf->len, size));
	apfs_buf_free(buf);
	return super;

fail_buf:
	apfs_buf_free(buf);
fail_super:
	kfree(super);
	return ERR_PTR(ret);
}

struct apfs_vol_superblock *
apfs_read_volume_super(struct apfs_nx_info *nx_info, int index)
{
	struct apfs_fs_info *fs_info = nx_info->vol;
	struct apfs_root *omap_root = fs_info->omap_root;
	u64 fs_oid = apfs_fs_oid(nx_info->super_copy, index);
	u64 xid = apfs_nx_super_xid(nx_info->super_copy);
	u64 paddr;
	int ret;

	ret = apfs_find_omap_paddr(omap_root, fs_oid, xid, &paddr);
	if (ret)
		return ERR_PTR(ret);

	return apfs_read_dev_volume_super(nx_info->vol, paddr,
					  APFS_SUPER_INFO_SIZE);
}

struct apfs_super_block *apfs_read_dev_super(struct block_device *bdev)
{
	struct apfs_super_block *super, *latest = NULL;
	int i;
	u64 transid = 0;

	/* we would like to check all the supers, but that would make
	 * a apfs mount succeed after a mkfs from a different FS.
	 * So, we need to add a special mount option to scan for
	 * later supers, using APFS_SUPER_MIRROR_MAX instead
	 */
	for (i = 0; i < 1; i++) {
		super = apfs_read_dev_one_super(bdev, i);
		if (IS_ERR(super))
			continue;

		if (!latest || apfs_super_generation(super) > transid) {
			if (latest)
				apfs_release_disk_super(super);

			latest = super;
			transid = apfs_super_generation(super);
		}
	}

	return super;
}

/*
 * Write superblock @sb to the @device. Do not wait for completion, all the
 * pages we use for writing are locked.
 *
 * Write @max_mirrors copies of the superblock, where 0 means default that fit
 * the expected device size at commit time. Note that max_mirrors must be
 * same for write and wait phases.
 *
 * Return number of errors when page is not found or submission fails.
 */
static int write_dev_supers(struct apfs_device *device,
			    struct apfs_super_block *sb, int max_mirrors)
{
	struct apfs_fs_info *fs_info = device->fs_info;
	struct address_space *mapping = device->bdev->bd_inode->i_mapping;
	SHASH_DESC_ON_STACK(shash, fs_info->csum_shash);
	int i;
	int errors = 0;
	int ret;
	u64 bytenr, bytenr_orig;

	if (max_mirrors == 0)
		max_mirrors = APFS_SUPER_MIRROR_MAX;

	shash->tfm = fs_info->csum_shash;

	for (i = 0; i < max_mirrors; i++) {
		struct page *page;
		struct bio *bio;
		struct apfs_super_block *disk_super;

		bytenr_orig = apfs_sb_offset(i);
		ret = apfs_sb_log_location(device, i, WRITE, &bytenr);
		if (ret == -ENOENT) {
			continue;
		} else if (ret < 0) {
			apfs_err(device->fs_info,
				"couldn't get super block location for mirror %d",
				i);
			errors++;
			continue;
		}
		if (bytenr + APFS_SUPER_INFO_SIZE >=
		    device->commit_total_bytes)
			break;

		apfs_set_super_bytenr(sb, bytenr_orig);

		crypto_shash_digest(shash, (const char *)sb + APFS_CSUM_SIZE,
				    APFS_SUPER_INFO_SIZE - APFS_CSUM_SIZE,
				    sb->csum);

		page = find_or_create_page(mapping, bytenr >> PAGE_SHIFT,
					   GFP_NOFS);
		if (!page) {
			apfs_err(device->fs_info,
			    "couldn't get super block page for bytenr %llu",
			    bytenr);
			errors++;
			continue;
		}

		/* Bump the refcount for wait_dev_supers() */
		get_page(page);

		disk_super = page_address(page);
		memcpy(disk_super, sb, APFS_SUPER_INFO_SIZE);

		/*
		 * Directly use bios here instead of relying on the page cache
		 * to do I/O, so we don't lose the ability to do integrity
		 * checking.
		 */
		bio = bio_alloc(GFP_NOFS, 1);
		bio_set_dev(bio, device->bdev);
		bio->bi_iter.bi_sector = bytenr >> SECTOR_SHIFT;
		bio->bi_private = device;
		bio->bi_end_io = apfs_end_super_write;
		__bio_add_page(bio, page, APFS_SUPER_INFO_SIZE,
			       offset_in_page(bytenr));

		/*
		 * We FUA only the first super block.  The others we allow to
		 * go down lazy and there's a short window where the on-disk
		 * copies might still contain the older version.
		 */
		bio->bi_opf = REQ_OP_WRITE | REQ_SYNC | REQ_META | REQ_PRIO;
		if (i == 0 && !apfs_test_opt(device->fs_info, NOBARRIER))
			bio->bi_opf |= REQ_FUA;

		apfsic_submit_bio(bio);
		apfs_advance_sb_log(device, i);
	}
	return errors < i ? 0 : -1;
}

/*
 * Wait for write completion of superblocks done by write_dev_supers,
 * @max_mirrors same for write and wait phases.
 *
 * Return number of errors when page is not found or not marked up to
 * date.
 */
static int wait_dev_supers(struct apfs_device *device, int max_mirrors)
{
	int i;
	int errors = 0;
	bool primary_failed = false;
	int ret;
	u64 bytenr;

	if (max_mirrors == 0)
		max_mirrors = APFS_SUPER_MIRROR_MAX;

	for (i = 0; i < max_mirrors; i++) {
		struct page *page;

		ret = apfs_sb_log_location(device, i, READ, &bytenr);
		if (ret == -ENOENT) {
			break;
		} else if (ret < 0) {
			errors++;
			if (i == 0)
				primary_failed = true;
			continue;
		}
		if (bytenr + APFS_SUPER_INFO_SIZE >=
		    device->commit_total_bytes)
			break;

		page = find_get_page(device->bdev->bd_inode->i_mapping,
				     bytenr >> PAGE_SHIFT);
		if (!page) {
			errors++;
			if (i == 0)
				primary_failed = true;
			continue;
		}
		/* Page is submitted locked and unlocked once the IO completes */
		wait_on_page_locked(page);
		if (PageError(page)) {
			errors++;
			if (i == 0)
				primary_failed = true;
		}

		/* Drop our reference */
		put_page(page);

		/* Drop the reference from the writing run */
		put_page(page);
	}

	/* log error, force error return */
	if (primary_failed) {
		apfs_err(device->fs_info, "error writing primary super block to device %llu",
			  device->devid);
		return -1;
	}

	return errors < i ? 0 : -1;
}

/*
 * endio for the write_dev_flush, this will wake anyone waiting
 * for the barrier when it is done
 */
static void apfs_end_empty_barrier(struct bio *bio)
{
	complete(bio->bi_private);
}

/*
 * Submit a flush request to the device if it supports it. Error handling is
 * done in the waiting counterpart.
 */
static void write_dev_flush(struct apfs_device *device)
{
	struct request_queue *q = bdev_get_queue(device->bdev);
	struct bio *bio = device->flush_bio;

	if (!test_bit(QUEUE_FLAG_WC, &q->queue_flags))
		return;

	bio_reset(bio);
	bio->bi_end_io = apfs_end_empty_barrier;
	bio_set_dev(bio, device->bdev);
	bio->bi_opf = REQ_OP_WRITE | REQ_SYNC | REQ_PREFLUSH;
	init_completion(&device->flush_wait);
	bio->bi_private = &device->flush_wait;

	apfsic_submit_bio(bio);
	set_bit(APFS_DEV_STATE_FLUSH_SENT, &device->dev_state);
}

/*
 * If the flush bio has been submitted by write_dev_flush, wait for it.
 */
static blk_status_t wait_dev_flush(struct apfs_device *device)
{
	struct bio *bio = device->flush_bio;

	if (!test_bit(APFS_DEV_STATE_FLUSH_SENT, &device->dev_state))
		return BLK_STS_OK;

	clear_bit(APFS_DEV_STATE_FLUSH_SENT, &device->dev_state);
	wait_for_completion_io(&device->flush_wait);

	return bio->bi_status;
}

static int check_barrier_error(struct apfs_fs_info *fs_info)
{
	if (!apfs_check_rw_degradable(fs_info, NULL))
		return -EIO;
	return 0;
}

/*
 * send an empty flush down to each device in parallel,
 * then wait for them
 */
static int barrier_all_devices(struct apfs_fs_info *info)
{
	struct list_head *head;
	struct apfs_device *dev;
	int errors_wait = 0;
	blk_status_t ret;

	lockdep_assert_held(&info->fs_devices->device_list_mutex);
	/* send down all the barriers */
	head = &info->fs_devices->devices;
	list_for_each_entry(dev, head, dev_list) {
		if (test_bit(APFS_DEV_STATE_MISSING, &dev->dev_state))
			continue;
		if (!dev->bdev)
			continue;
		if (!test_bit(APFS_DEV_STATE_IN_FS_METADATA, &dev->dev_state) ||
		    !test_bit(APFS_DEV_STATE_WRITEABLE, &dev->dev_state))
			continue;

		write_dev_flush(dev);
		dev->last_flush_error = BLK_STS_OK;
	}

	/* wait for all the barriers */
	list_for_each_entry(dev, head, dev_list) {
		if (test_bit(APFS_DEV_STATE_MISSING, &dev->dev_state))
			continue;
		if (!dev->bdev) {
			errors_wait++;
			continue;
		}
		if (!test_bit(APFS_DEV_STATE_IN_FS_METADATA, &dev->dev_state) ||
		    !test_bit(APFS_DEV_STATE_WRITEABLE, &dev->dev_state))
			continue;

		ret = wait_dev_flush(dev);
		if (ret) {
			dev->last_flush_error = ret;
			apfs_dev_stat_inc_and_print(dev,
					APFS_DEV_STAT_FLUSH_ERRS);
			errors_wait++;
		}
	}

	if (errors_wait) {
		/*
		 * At some point we need the status of all disks
		 * to arrive at the volume status. So error checking
		 * is being pushed to a separate loop.
		 */
		return check_barrier_error(info);
	}
	return 0;
}

int apfs_get_num_tolerated_disk_barrier_failures(u64 flags)
{
	int raid_type;
	int min_tolerated = INT_MAX;

	if ((flags & APFS_BLOCK_GROUP_PROFILE_MASK) == 0 ||
	    (flags & APFS_AVAIL_ALLOC_BIT_SINGLE))
		min_tolerated = min_t(int, min_tolerated,
				    apfs_raid_array[APFS_RAID_SINGLE].
				    tolerated_failures);

	for (raid_type = 0; raid_type < APFS_NR_RAID_TYPES; raid_type++) {
		if (raid_type == APFS_RAID_SINGLE)
			continue;
		if (!(flags & apfs_raid_array[raid_type].bg_flag))
			continue;
		min_tolerated = min_t(int, min_tolerated,
				    apfs_raid_array[raid_type].
				    tolerated_failures);
	}

	if (min_tolerated == INT_MAX) {
		pr_warn("APFS: unknown raid flag: %llu", flags);
		min_tolerated = 0;
	}

	return min_tolerated;
}

int write_all_supers(struct apfs_fs_info *fs_info, int max_mirrors)
{
	struct list_head *head;
	struct apfs_device *dev;
	struct apfs_super_block *sb;
	struct apfs_dev_item *dev_item;
	int ret;
	int do_barriers;
	int max_errors;
	int total_errors = 0;
	u64 flags;

	do_barriers = !apfs_test_opt(fs_info, NOBARRIER);

	/*
	 * max_mirrors == 0 indicates we're from commit_transaction,
	 * not from fsync where the tree roots in fs_info have not
	 * been consistent on disk.
	 */
	if (max_mirrors == 0)
		backup_super_roots(fs_info);

	sb = fs_info->super_for_commit;
	dev_item = &sb->dev_item;

	mutex_lock(&fs_info->fs_devices->device_list_mutex);
	head = &fs_info->fs_devices->devices;
	max_errors = apfs_super_num_devices(fs_info->super_copy) - 1;

	if (do_barriers) {
		ret = barrier_all_devices(fs_info);
		if (ret) {
			mutex_unlock(
				&fs_info->fs_devices->device_list_mutex);
			apfs_handle_fs_error(fs_info, ret,
					      "errors while submitting device barriers.");
			return ret;
		}
	}

	list_for_each_entry(dev, head, dev_list) {
		if (!dev->bdev) {
			total_errors++;
			continue;
		}
		if (!test_bit(APFS_DEV_STATE_IN_FS_METADATA, &dev->dev_state) ||
		    !test_bit(APFS_DEV_STATE_WRITEABLE, &dev->dev_state))
			continue;

		apfs_set_stack_device_generation(dev_item, 0);
		apfs_set_stack_device_type(dev_item, dev->type);
		apfs_set_stack_device_id(dev_item, dev->devid);
		apfs_set_stack_device_total_bytes(dev_item,
						   dev->commit_total_bytes);
		apfs_set_stack_device_bytes_used(dev_item,
						  dev->commit_bytes_used);
		apfs_set_stack_device_io_align(dev_item, dev->io_align);
		apfs_set_stack_device_io_width(dev_item, dev->io_width);
		apfs_set_stack_device_sector_size(dev_item, dev->sector_size);
		memcpy(dev_item->uuid, dev->uuid, APFS_UUID_SIZE);
		memcpy(dev_item->fsid, dev->fs_devices->metadata_uuid,
		       APFS_FSID_SIZE);

		flags = apfs_super_flags(sb);
		apfs_set_super_flags(sb, flags | APFS_HEADER_FLAG_WRITTEN);

		ret = apfs_validate_write_super(fs_info, sb);
		if (ret < 0) {
			mutex_unlock(&fs_info->fs_devices->device_list_mutex);
			apfs_handle_fs_error(fs_info, -EUCLEAN,
				"unexpected superblock corruption detected");
			return -EUCLEAN;
		}

		ret = write_dev_supers(dev, sb, max_mirrors);
		if (ret)
			total_errors++;
	}
	if (total_errors > max_errors) {
		apfs_err(fs_info, "%d errors while writing supers",
			  total_errors);
		mutex_unlock(&fs_info->fs_devices->device_list_mutex);

		/* FUA is masked off if unsupported and can't be the reason */
		apfs_handle_fs_error(fs_info, -EIO,
				      "%d errors while writing supers",
				      total_errors);
		return -EIO;
	}

	total_errors = 0;
	list_for_each_entry(dev, head, dev_list) {
		if (!dev->bdev)
			continue;
		if (!test_bit(APFS_DEV_STATE_IN_FS_METADATA, &dev->dev_state) ||
		    !test_bit(APFS_DEV_STATE_WRITEABLE, &dev->dev_state))
			continue;

		ret = wait_dev_supers(dev, max_mirrors);
		if (ret)
			total_errors++;
	}
	mutex_unlock(&fs_info->fs_devices->device_list_mutex);
	if (total_errors > max_errors) {
		apfs_handle_fs_error(fs_info, -EIO,
				      "%d errors while writing supers",
				      total_errors);
		return -EIO;
	}
	return 0;
}

/* Drop a fs root from the radix tree and free it. */
void apfs_drop_and_free_fs_root(struct apfs_fs_info *fs_info,
				  struct apfs_root *root)
{
	bool drop_ref = false;

	spin_lock(&fs_info->fs_roots_radix_lock);
	radix_tree_delete(&fs_info->fs_roots_radix,
			  (unsigned long)root->root_key.objectid);
	if (test_and_clear_bit(APFS_ROOT_IN_RADIX, &root->state))
		drop_ref = true;
	spin_unlock(&fs_info->fs_roots_radix_lock);

	if (test_bit(APFS_FS_STATE_ERROR, &fs_info->fs_state)) {
		ASSERT(root->log_root == NULL);
		if (root->reloc_root) {
			apfs_put_root(root->reloc_root);
			root->reloc_root = NULL;
		}
	}

	if (drop_ref)
		apfs_put_root(root);
}

int apfs_cleanup_fs_roots(struct apfs_fs_info *fs_info)
{
	u64 root_objectid = 0;
	struct apfs_root *gang[8];
	int i = 0;
	int err = 0;
	unsigned int ret = 0;

	while (1) {
		spin_lock(&fs_info->fs_roots_radix_lock);
		ret = radix_tree_gang_lookup(&fs_info->fs_roots_radix,
					     (void **)gang, root_objectid,
					     ARRAY_SIZE(gang));
		if (!ret) {
			spin_unlock(&fs_info->fs_roots_radix_lock);
			break;
		}
		root_objectid = gang[ret - 1]->root_key.objectid + 1;

		for (i = 0; i < ret; i++) {
			/* Avoid to grab roots in dead_roots */
			if (apfs_root_refs(&gang[i]->root_item) == 0) {
				gang[i] = NULL;
				continue;
			}
			/* grab all the search result for later use */
			gang[i] = apfs_grab_root(gang[i]);
		}
		spin_unlock(&fs_info->fs_roots_radix_lock);

		for (i = 0; i < ret; i++) {
			if (!gang[i])
				continue;
			root_objectid = gang[i]->root_key.objectid;
			err = apfs_orphan_cleanup(gang[i]);
			if (err)
				break;
			apfs_put_root(gang[i]);
		}
		root_objectid++;
	}

	/* release the uncleaned roots due to error */
	for (; i < ret; i++) {
		if (gang[i])
			apfs_put_root(gang[i]);
	}
	return err;
}

int apfs_commit_super(struct apfs_fs_info *fs_info)
{
	struct apfs_root *root = fs_info->tree_root;
	struct apfs_trans_handle *trans;

	mutex_lock(&fs_info->cleaner_mutex);
	apfs_run_delayed_iputs(fs_info);
	mutex_unlock(&fs_info->cleaner_mutex);
	wake_up_process(fs_info->cleaner_kthread);

	/* wait until ongoing cleanup work done */
	down_write(&fs_info->cleanup_work_sem);
	up_write(&fs_info->cleanup_work_sem);

	trans = apfs_join_transaction(root);
	if (IS_ERR(trans))
		return PTR_ERR(trans);
	return apfs_commit_transaction(trans);
}

static void __cold __close_ctree(struct apfs_fs_info *fs_info)
{
	set_bit(APFS_FS_CLOSING_START, &fs_info->flags);
	/*
	 * we must make sure there is not any read request to
	 * submit after we stopping all workers.
	 */
	if (fs_info->btree_inode)
		invalidate_inode_pages2(fs_info->btree_inode->i_mapping);
	apfs_stop_all_workers(fs_info);

	clear_bit(APFS_FS_OPEN, &fs_info->flags);
	free_root_pointers(fs_info, true);
	apfs_free_fs_roots(fs_info);

	iput(fs_info->btree_inode);

	apfs_mapping_tree_free(&fs_info->mapping_tree);

	set_bit(APFS_FS_CLOSING_DONE, &fs_info->flags);
}

static void __cold close_dummy_fs_info(struct apfs_nx_info *nx_info)
{
	if(!nx_info->vol)
		return;

	__close_ctree(nx_info->vol);
}

void __cold close_ctree(struct apfs_fs_info *fs_info)
{
	struct apfs_nx_info *nx_info = fs_info->nx_info;

	__close_ctree(fs_info);
	/*
	 * the real fs_info holds 1, and nx_info itself holds one.
	 */
	if (refcount_read(&nx_info->refs) <= 2)
		close_dummy_fs_info(nx_info);
	return ;
}

int apfs_buffer_uptodate(struct extent_buffer *buf, u64 parent_transid,
			  int atomic)
{
	int ret;
	struct inode *btree_inode = buf->pages[0]->mapping->host;

	ret = extent_buffer_uptodate(buf);
	if (!ret)
		return ret;

	ret = verify_parent_transid(&APFS_I(btree_inode)->io_tree, buf,
				    parent_transid, atomic);
	if (ret == -EAGAIN)
		return ret;
	return !ret;
}

void apfs_mark_buffer_dirty(struct extent_buffer *buf)
{
	struct apfs_fs_info *fs_info = buf->fs_info;
	u64 transid = apfs_header_generation(buf);
	int was_dirty;

#ifdef CONFIG_APFS_FS_RUN_SANITY_TESTS
	/*
	 * This is a fast path so only do this check if we have sanity tests
	 * enabled.  Normal people shouldn't be using unmapped buffers as dirty
	 * outside of the sanity tests.
	 */
	if (unlikely(test_bit(EXTENT_BUFFER_UNMAPPED, &buf->bflags)))
		return;
#endif
	apfs_assert_tree_locked(buf);
	if (transid != fs_info->generation)
		WARN(1, KERN_CRIT "apfs transid mismatch buffer %llu, found %llu running %llu\n",
			buf->start, transid, fs_info->generation);
	was_dirty = set_extent_buffer_dirty(buf);
	if (!was_dirty)
		percpu_counter_add_batch(&fs_info->dirty_metadata_bytes,
					 buf->len,
					 fs_info->dirty_metadata_batch);
#ifdef CONFIG_APFS_FS_CHECK_INTEGRITY
	/*
	 * Since apfs_mark_buffer_dirty() can be called with item pointer set
	 * but item data not updated.
	 * So here we should only check item pointers, not item data.
	 */
	if (apfs_header_level(buf) == 0 &&
	    apfs_check_leaf_relaxed(buf)) {
		apfs_print_leaf(buf);
		ASSERT(0);
	}
#endif
}

static void __apfs_btree_balance_dirty(struct apfs_fs_info *fs_info,
					int flush_delayed)
{
	/*
	 * looks as though older kernels can get into trouble with
	 * this code, they end up stuck in balance_dirty_pages forever
	 */
	int ret;

	if (current->flags & PF_MEMALLOC)
		return;

	if (flush_delayed)
		apfs_balance_delayed_items(fs_info);

	ret = __percpu_counter_compare(&fs_info->dirty_metadata_bytes,
				     APFS_DIRTY_METADATA_THRESH,
				     fs_info->dirty_metadata_batch);
	if (ret > 0) {
		balance_dirty_pages_ratelimited(fs_info->btree_inode->i_mapping);
	}
}

void apfs_btree_balance_dirty(struct apfs_fs_info *fs_info)
{
	__apfs_btree_balance_dirty(fs_info, 1);
}

void apfs_btree_balance_dirty_nodelay(struct apfs_fs_info *fs_info)
{
	__apfs_btree_balance_dirty(fs_info, 0);
}

int apfs_read_buffer(struct extent_buffer *buf, u64 parent_transid, int level,
		      struct apfs_key *first_key)
{
	return btree_read_extent_buffer_pages(buf, parent_transid,
					      level, first_key);
}

static void apfs_error_commit_super(struct apfs_fs_info *fs_info)
{
	/* cleanup FS via transaction */
	apfs_cleanup_transaction(fs_info);

	mutex_lock(&fs_info->cleaner_mutex);
	apfs_run_delayed_iputs(fs_info);
	mutex_unlock(&fs_info->cleaner_mutex);

	down_write(&fs_info->cleanup_work_sem);
	up_write(&fs_info->cleanup_work_sem);
}

static void apfs_drop_all_logs(struct apfs_fs_info *fs_info)
{
	struct apfs_root *gang[8];
	u64 root_objectid = 0;
	int ret;

	spin_lock(&fs_info->fs_roots_radix_lock);
	while ((ret = radix_tree_gang_lookup(&fs_info->fs_roots_radix,
					     (void **)gang, root_objectid,
					     ARRAY_SIZE(gang))) != 0) {
		int i;

		for (i = 0; i < ret; i++)
			gang[i] = apfs_grab_root(gang[i]);
		spin_unlock(&fs_info->fs_roots_radix_lock);

		for (i = 0; i < ret; i++) {
			if (!gang[i])
				continue;
			root_objectid = gang[i]->root_key.objectid;
			apfs_free_log(NULL, gang[i]);
			apfs_put_root(gang[i]);
		}
		root_objectid++;
		spin_lock(&fs_info->fs_roots_radix_lock);
	}
	spin_unlock(&fs_info->fs_roots_radix_lock);
	apfs_free_log_root_tree(NULL, fs_info);
}

static void apfs_destroy_ordered_extents(struct apfs_root *root)
{
	struct apfs_ordered_extent *ordered;

	spin_lock(&root->ordered_extent_lock);
	/*
	 * This will just short circuit the ordered completion stuff which will
	 * make sure the ordered extent gets properly cleaned up.
	 */
	list_for_each_entry(ordered, &root->ordered_extents,
			    root_extent_list)
		set_bit(APFS_ORDERED_IOERR, &ordered->flags);
	spin_unlock(&root->ordered_extent_lock);
}

static void apfs_destroy_all_ordered_extents(struct apfs_fs_info *fs_info)
{
	struct apfs_root *root;
	struct list_head splice;

	INIT_LIST_HEAD(&splice);

	spin_lock(&fs_info->ordered_root_lock);
	list_splice_init(&fs_info->ordered_roots, &splice);
	while (!list_empty(&splice)) {
		root = list_first_entry(&splice, struct apfs_root,
					ordered_root);
		list_move_tail(&root->ordered_root,
			       &fs_info->ordered_roots);

		spin_unlock(&fs_info->ordered_root_lock);
		apfs_destroy_ordered_extents(root);

		cond_resched();
		spin_lock(&fs_info->ordered_root_lock);
	}
	spin_unlock(&fs_info->ordered_root_lock);

	/*
	 * We need this here because if we've been flipped read-only we won't
	 * get sync() from the umount, so we need to make sure any ordered
	 * extents that haven't had their dirty pages IO start writeout yet
	 * actually get run and error out properly.
	 */
	apfs_wait_ordered_roots(fs_info, U64_MAX, 0, (u64)-1);
}

static int apfs_destroy_delayed_refs(struct apfs_transaction *trans,
				      struct apfs_fs_info *fs_info)
{
	struct rb_node *node;
	struct apfs_delayed_ref_root *delayed_refs;
	struct apfs_delayed_ref_node *ref;
	int ret = 0;

	delayed_refs = &trans->delayed_refs;

	spin_lock(&delayed_refs->lock);
	if (atomic_read(&delayed_refs->num_entries) == 0) {
		spin_unlock(&delayed_refs->lock);
		apfs_debug(fs_info, "delayed_refs has NO entry");
		return ret;
	}

	while ((node = rb_first_cached(&delayed_refs->href_root)) != NULL) {
		struct apfs_delayed_ref_head *head;
		struct rb_node *n;
		bool pin_bytes = false;

		head = rb_entry(node, struct apfs_delayed_ref_head,
				href_node);
		if (apfs_delayed_ref_lock(delayed_refs, head))
			continue;

		spin_lock(&head->lock);
		while ((n = rb_first_cached(&head->ref_tree)) != NULL) {
			ref = rb_entry(n, struct apfs_delayed_ref_node,
				       ref_node);
			ref->in_tree = 0;
			rb_erase_cached(&ref->ref_node, &head->ref_tree);
			RB_CLEAR_NODE(&ref->ref_node);
			if (!list_empty(&ref->add_list))
				list_del(&ref->add_list);
			atomic_dec(&delayed_refs->num_entries);
			apfs_put_delayed_ref(ref);
		}
		if (head->must_insert_reserved)
			pin_bytes = true;
		apfs_free_delayed_extent_op(head->extent_op);
		apfs_delete_ref_head(delayed_refs, head);
		spin_unlock(&head->lock);
		spin_unlock(&delayed_refs->lock);
		mutex_unlock(&head->mutex);

		if (pin_bytes) {
			struct apfs_block_group *cache;

			cache = apfs_lookup_block_group(fs_info, head->bytenr);
			BUG_ON(!cache);

			spin_lock(&cache->space_info->lock);
			spin_lock(&cache->lock);
			cache->pinned += head->num_bytes;
			apfs_space_info_update_bytes_pinned(fs_info,
				cache->space_info, head->num_bytes);
			cache->reserved -= head->num_bytes;
			cache->space_info->bytes_reserved -= head->num_bytes;
			spin_unlock(&cache->lock);
			spin_unlock(&cache->space_info->lock);

			apfs_put_block_group(cache);

			apfs_error_unpin_extent_range(fs_info, head->bytenr,
				head->bytenr + head->num_bytes - 1);
		}
		apfs_cleanup_ref_head_accounting(fs_info, delayed_refs, head);
		apfs_put_delayed_ref_head(head);
		cond_resched();
		spin_lock(&delayed_refs->lock);
	}
	apfs_qgroup_destroy_extent_records(trans);

	spin_unlock(&delayed_refs->lock);

	return ret;
}

static void apfs_destroy_delalloc_inodes(struct apfs_root *root)
{
	struct apfs_inode *apfs_inode;
	struct list_head splice;

	INIT_LIST_HEAD(&splice);

	spin_lock(&root->delalloc_lock);
	list_splice_init(&root->delalloc_inodes, &splice);

	while (!list_empty(&splice)) {
		struct inode *inode = NULL;
		apfs_inode = list_first_entry(&splice, struct apfs_inode,
					       delalloc_inodes);
		__apfs_del_delalloc_inode(root, apfs_inode);
		spin_unlock(&root->delalloc_lock);

		/*
		 * Make sure we get a live inode and that it'll not disappear
		 * meanwhile.
		 */
		inode = igrab(&apfs_inode->vfs_inode);
		if (inode) {
			invalidate_inode_pages2(inode->i_mapping);
			iput(inode);
		}
		spin_lock(&root->delalloc_lock);
	}
	spin_unlock(&root->delalloc_lock);
}

static void apfs_destroy_all_delalloc_inodes(struct apfs_fs_info *fs_info)
{
	struct apfs_root *root;
	struct list_head splice;

	INIT_LIST_HEAD(&splice);

	spin_lock(&fs_info->delalloc_root_lock);
	list_splice_init(&fs_info->delalloc_roots, &splice);
	while (!list_empty(&splice)) {
		root = list_first_entry(&splice, struct apfs_root,
					 delalloc_root);
		root = apfs_grab_root(root);
		BUG_ON(!root);
		spin_unlock(&fs_info->delalloc_root_lock);

		apfs_destroy_delalloc_inodes(root);
		apfs_put_root(root);

		spin_lock(&fs_info->delalloc_root_lock);
	}
	spin_unlock(&fs_info->delalloc_root_lock);
}

static int apfs_destroy_marked_extents(struct apfs_fs_info *fs_info,
					struct extent_io_tree *dirty_pages,
					int mark)
{
	int ret;
	struct extent_buffer *eb;
	u64 start = 0;
	u64 end;

	while (1) {
		ret = find_first_extent_bit(dirty_pages, start, &start, &end,
					    mark, NULL);
		if (ret)
			break;

		clear_extent_bits(dirty_pages, start, end, mark);
		while (start <= end) {
			eb = find_extent_buffer(fs_info, start);
			start += fs_info->nodesize;
			if (!eb)
				continue;
			wait_on_extent_buffer_writeback(eb);

			if (test_and_clear_bit(EXTENT_BUFFER_DIRTY,
					       &eb->bflags))
				clear_extent_buffer_dirty(eb);
			free_extent_buffer_stale(eb);
		}
	}

	return ret;
}

static int apfs_destroy_pinned_extent(struct apfs_fs_info *fs_info,
				       struct extent_io_tree *unpin)
{
	u64 start;
	u64 end;
	int ret;

	while (1) {
		struct extent_state *cached_state = NULL;

		/*
		 * The apfs_finish_extent_commit() may get the same range as
		 * ours between find_first_extent_bit and clear_extent_dirty.
		 * Hence, hold the unused_bg_unpin_mutex to avoid double unpin
		 * the same extent range.
		 */
		mutex_lock(&fs_info->unused_bg_unpin_mutex);
		ret = find_first_extent_bit(unpin, 0, &start, &end,
					    EXTENT_DIRTY, &cached_state);
		if (ret) {
			mutex_unlock(&fs_info->unused_bg_unpin_mutex);
			break;
		}

		clear_extent_dirty(unpin, start, end, &cached_state);
		free_extent_state(cached_state);
		apfs_error_unpin_extent_range(fs_info, start, end);
		mutex_unlock(&fs_info->unused_bg_unpin_mutex);
		cond_resched();
	}

	return 0;
}

static void apfs_cleanup_bg_io(struct apfs_block_group *cache)
{
	struct inode *inode;

	inode = cache->io_ctl.inode;
	if (inode) {
		invalidate_inode_pages2(inode->i_mapping);
		APFS_I(inode)->generation = 0;
		cache->io_ctl.inode = NULL;
		iput(inode);
	}
	ASSERT(cache->io_ctl.pages == NULL);
	apfs_put_block_group(cache);
}

void apfs_cleanup_dirty_bgs(struct apfs_transaction *cur_trans,
			     struct apfs_fs_info *fs_info)
{
	struct apfs_block_group *cache;

	spin_lock(&cur_trans->dirty_bgs_lock);
	while (!list_empty(&cur_trans->dirty_bgs)) {
		cache = list_first_entry(&cur_trans->dirty_bgs,
					 struct apfs_block_group,
					 dirty_list);

		if (!list_empty(&cache->io_list)) {
			spin_unlock(&cur_trans->dirty_bgs_lock);
			list_del_init(&cache->io_list);
			apfs_cleanup_bg_io(cache);
			spin_lock(&cur_trans->dirty_bgs_lock);
		}

		list_del_init(&cache->dirty_list);
		spin_lock(&cache->lock);
		cache->disk_cache_state = APFS_DC_ERROR;
		spin_unlock(&cache->lock);

		spin_unlock(&cur_trans->dirty_bgs_lock);
		apfs_put_block_group(cache);
		apfs_delayed_refs_rsv_release(fs_info, 1);
		spin_lock(&cur_trans->dirty_bgs_lock);
	}
	spin_unlock(&cur_trans->dirty_bgs_lock);

	/*
	 * Refer to the definition of io_bgs member for details why it's safe
	 * to use it without any locking
	 */
	while (!list_empty(&cur_trans->io_bgs)) {
		cache = list_first_entry(&cur_trans->io_bgs,
					 struct apfs_block_group,
					 io_list);

		list_del_init(&cache->io_list);
		spin_lock(&cache->lock);
		cache->disk_cache_state = APFS_DC_ERROR;
		spin_unlock(&cache->lock);
		apfs_cleanup_bg_io(cache);
	}
}

void apfs_cleanup_one_transaction(struct apfs_transaction *cur_trans,
				   struct apfs_fs_info *fs_info)
{
	struct apfs_device *dev, *tmp;

	apfs_cleanup_dirty_bgs(cur_trans, fs_info);
	ASSERT(list_empty(&cur_trans->dirty_bgs));
	ASSERT(list_empty(&cur_trans->io_bgs));

	list_for_each_entry_safe(dev, tmp, &cur_trans->dev_update_list,
				 post_commit_list) {
		list_del_init(&dev->post_commit_list);
	}

	apfs_destroy_delayed_refs(cur_trans, fs_info);

	cur_trans->state = TRANS_STATE_COMMIT_START;
	wake_up(&fs_info->transaction_blocked_wait);

	cur_trans->state = TRANS_STATE_UNBLOCKED;
	wake_up(&fs_info->transaction_wait);

	apfs_destroy_delayed_inodes(fs_info);

	apfs_destroy_marked_extents(fs_info, &cur_trans->dirty_pages,
				     EXTENT_DIRTY);
	apfs_destroy_pinned_extent(fs_info, &cur_trans->pinned_extents);

	apfs_free_redirty_list(cur_trans);

	cur_trans->state =TRANS_STATE_COMPLETED;
	wake_up(&cur_trans->commit_wait);
}

static int apfs_cleanup_transaction(struct apfs_fs_info *fs_info)
{
	struct apfs_transaction *t;

	mutex_lock(&fs_info->transaction_kthread_mutex);

	spin_lock(&fs_info->trans_lock);
	while (!list_empty(&fs_info->trans_list)) {
		t = list_first_entry(&fs_info->trans_list,
				     struct apfs_transaction, list);
		if (t->state >= TRANS_STATE_COMMIT_START) {
			refcount_inc(&t->use_count);
			spin_unlock(&fs_info->trans_lock);
			apfs_wait_for_commit(fs_info, t->transid);
			apfs_put_transaction(t);
			spin_lock(&fs_info->trans_lock);
			continue;
		}
		if (t == fs_info->running_transaction) {
			t->state = TRANS_STATE_COMMIT_DOING;
			spin_unlock(&fs_info->trans_lock);
			/*
			 * We wait for 0 num_writers since we don't hold a trans
			 * handle open currently for this transaction.
			 */
			wait_event(t->writer_wait,
				   atomic_read(&t->num_writers) == 0);
		} else {
			spin_unlock(&fs_info->trans_lock);
		}
		apfs_cleanup_one_transaction(t, fs_info);

		spin_lock(&fs_info->trans_lock);
		if (t == fs_info->running_transaction)
			fs_info->running_transaction = NULL;
		list_del_init(&t->list);
		spin_unlock(&fs_info->trans_lock);

		apfs_put_transaction(t);
		trace_apfs_transaction_commit(fs_info->tree_root);
		spin_lock(&fs_info->trans_lock);
	}
	spin_unlock(&fs_info->trans_lock);
	apfs_destroy_all_ordered_extents(fs_info);
	apfs_destroy_delayed_inodes(fs_info);
	apfs_assert_delayed_root_empty(fs_info);
	apfs_destroy_all_delalloc_inodes(fs_info);
	apfs_drop_all_logs(fs_info);
	mutex_unlock(&fs_info->transaction_kthread_mutex);

	return 0;
}

int apfs_init_root_free_objectid(struct apfs_root *root)
{
	struct apfs_path *path;
	int ret;
	struct extent_buffer *l;
	struct apfs_key search_key = {};
	struct apfs_key found_key = {};
	int slot;

	path = apfs_alloc_path();
	if (!path)
		return -ENOMEM;

	search_key.objectid = APFS_LAST_FREE_OBJECTID;
	search_key.type = -1;
	search_key.offset = (u64)-1;
	ret = apfs_search_slot(NULL, root, &search_key, path, 0, 0);
	if (ret < 0)
		goto error;
	BUG_ON(ret == 0); /* Corruption */
	if (path->slots[0] > 0) {
		slot = path->slots[0] - 1;
		l = path->nodes[0];
		apfs_item_key_to_cpu(l, &found_key, slot);
		root->free_objectid = max_t(u64, found_key.objectid + 1,
					    APFS_FIRST_FREE_OBJECTID);
	} else {
		root->free_objectid = APFS_FIRST_FREE_OBJECTID;
	}
	ret = 0;
error:
	apfs_free_path(path);
	return ret;
}

int apfs_get_free_objectid(struct apfs_root *root, u64 *objectid)
{
	int ret;
	mutex_lock(&root->objectid_mutex);

	if (unlikely(root->free_objectid >= APFS_LAST_FREE_OBJECTID)) {
		apfs_warn(root->fs_info,
			   "the objectid of root %llu reaches its highest value",
			   root->root_key.objectid);
		ret = -ENOSPC;
		goto out;
	}

	*objectid = root->free_objectid++;
	ret = 0;
out:
	mutex_unlock(&root->objectid_mutex);
	return ret;
}

/* return 0 if not found */
static inline u64
__apfs_find_ephemeral_paddr(struct apfs_checkpoint_map_phys *cpm, u64 oid)
{
	int i;

	for (i = 0; i < cpm->count; ++i) {
		if (cpm->map[i].oid == oid)
			return cpm->map[i].paddr;
	}

	return 0;
}

int
apfs_read_checkpoint_map(struct apfs_device *device, u64 bytenr,
			 struct apfs_checkpoint_map_phys *cmp)
{
	return apfs_read_generic(device->bdev, bytenr, sizeof(*cmp),
				 cmp);
}

/*
 * return <0 on fatal error
 * return == 0 if found
 * return > 0 means not found
 */
int
apfs_find_ephemeral_paddr(struct apfs_nx_info *info, u64 oid, u64 *paddr_res)
{
	int i = 0;
	u64 paddr;
	int ret;
	u64 desc_base = apfs_nx_super_xp_desc_base(info->super_copy);
	u64 desc_blocks = apfs_nx_super_xp_desc_blocks(info->super_copy);
	u64 desc_index = apfs_nx_super_xp_desc_index(info->super_copy);
	u64 desc_len = apfs_nx_super_xp_desc_len(info->super_copy);


	BUG_ON(desc_base == 0 || desc_len == 0);

	for (i = 0; i < desc_len - 1; ++i) {
		struct apfs_checkpoint_map_phys cpm;
		u64 cpm_paddr = (desc_base + (desc_index + i) % desc_blocks) *
			info->block_size;

		ret = apfs_read_checkpoint_map(info->device, cpm_paddr, &cpm);
		if (ret)
			return ret;
		paddr = __apfs_find_ephemeral_paddr(&cpm, oid);
		if (paddr != 0) {
			ret = 0;
			*paddr_res = paddr;
		}

	}

	/* not found */
	return -ENOENT;
}

u64 apfs_node_blockptr(const struct extent_buffer *eb, int nr)
{
	u64 item_offset = apfs_item_offset_nr(eb, nr);
	__le64 __oid;
	u64 oid;
	enum apfs_storage stg;
	struct apfs_obj_header obj;
	u64 paddr;
	int ret;

	read_extent_buffer(eb, &__oid, item_offset, sizeof(__oid));
	oid = __le64_to_cpu(__oid);

	read_extent_buffer(eb, &obj, 0, sizeof(obj));
	stg = apfs_obj_stg_type(&obj);

	if (stg == APFS_STG_PHYSICAL) {
		paddr = oid << eb->fs_info->block_size_bits;
		return paddr;
	}

	if (stg == APFS_STG_EPHEMERAL)
		ret = apfs_find_ephemeral_paddr(eb->fs_info->nx_info, oid,
						&paddr);
	else
		ret = apfs_find_omap_paddr(eb->fs_info->omap_root, oid,
			     apfs_volume_super_xid(eb->fs_info->__super_copy), &paddr);

	if (ret)
		paddr = 0;

	return paddr;
}
