// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2022 Su Yue <l@damenly.org>
 * All Rights Reserved.
 */
#include <linux/slab.h>
#include <linux/backing-dev.h>
#include <linux/bio.h>

#include "ctree.h"
#include "volumes.h"
#include "apfs_buf.h"

static void
apfs_buf_ioend(struct apfs_buf	*bp)
{
	complete(&bp->io_wait);
}

static void
apfs_buf_bio_end_io(struct bio *bio)
{
	struct apfs_buf *bp = (struct apfs_buf *)bio->bi_private;

	/*
	 * don't overwrite existing errors - otherwise we can lose errors on
	 * buffers that require multiple bios to complete.
	 */
	if (bio->bi_status) {
		int error = blk_status_to_errno(bio->bi_status);

		cmpxchg(&bp->io_errors, 0, error);
	}

	atomic_dec(&bp->io_remaining);
	if (atomic_read(&bp->io_remaining) == 1) {
		if (!bp->error && bp->io_errors)
			apfs_crit_in_rcu(bp->fs_info, "buf bio errors %d",
					 bp->io_errors);
		bp->error = bp->io_errors;
		complete(&bp->io_wait);
	}
	bio_put(bio);
}

static void
apfs_buf_ioapply(struct apfs_buf *bp)
{
	int offset;
	unsigned int total_nr_pages = bp->page_count;
	int page_index;
	int nr_pages;
	struct bio *bio;
	int size = bp->len;
	sector_t sector = bp->bno;

	/* skip the pages in the buffer before the start offset */
	page_index = 0;
	offset = bp->offset;
	while (offset >= PAGE_SIZE) {
		page_index++;
		offset -= PAGE_SIZE;
	}

next_chunk:
	atomic_inc(&bp->io_remaining);
	nr_pages = bio_max_segs(total_nr_pages);

	bio = bio_alloc(GFP_NOIO, nr_pages);
	bio_set_dev(bio, bp->fs_info->device->bdev);
	bio->bi_iter.bi_sector = sector;
	bio->bi_end_io = apfs_buf_bio_end_io;
	bio->bi_private = bp;
	bio->bi_opf = bp->op;


	for (; size && nr_pages; nr_pages--, page_index++) {
		int rbytes, nbytes = PAGE_SIZE - offset;

		if (nbytes > size)
			nbytes = size;

		rbytes = bio_add_page(bio, bp->pages[page_index], nbytes,
				      offset);
		if (rbytes < nbytes)
			break;

		offset = 0;
		sector += nbytes >> 9;
		size -= nbytes;
		total_nr_pages--;
	}

	if (likely(bio->bi_iter.bi_size)) {
		submit_bio(bio);
		if (size)
			goto next_chunk;
	} else {
		/*
		 * This is guaranteed not to be the last io reference count
		 * because the caller (apfs_buf_submit) holds a count itself.
		 */
		atomic_dec(&bp->io_remaining);
		bp->error = -EIO;
		bio_put(bio);
	}
}

int apfs_buf_alloc_pages(struct apfs_buf *bp, u32 flags)
{
	gfp_t		gfp_mask = __GFP_NOWARN;
	long		filled = 0;

	gfp_mask |= GFP_NOFS;

	/* Make sure that we have a page list */
	bp->page_count = DIV_ROUND_UP(bp->len, PAGE_SIZE);

	bp->pages = kcalloc(bp->page_count, sizeof(struct page *),  gfp_mask);
	if (!bp->pages)
		return -ENOMEM;

	/* Assure zeroed buffer for non-read cases. */
	if (!(flags & ABF_READ))
		gfp_mask |= __GFP_ZERO;

	/*
	 * Bulk filling of pages can take multiple calls. Not filling the entire
	 * array is not an allocation failure, so don't back off if we get at
	 * least one extra page.
	 */
	for (;;) {
		long last = filled;

		filled = alloc_pages_bulk_array(gfp_mask, bp->page_count,
						bp->pages);
		if (filled == bp->page_count)
			break;

		if (filled != last)
			continue;

		congestion_wait(BLK_RW_ASYNC, HZ / 50);
	}
	return 0;
}

static void
apfs_buf_free_pages(struct apfs_buf *bp)
{
	int i;

	for (i = 0; i < bp->page_count; i++) {
		if (bp->pages[i])
			__free_page(bp->pages[i]);
	}
	kfree(bp->pages);
}


void apfs_buf_free(struct apfs_buf *bp)
{
	apfs_buf_free_pages(bp);
	kfree(bp);
}

struct apfs_buf *apfs_buf_alloc(void)
{
	struct apfs_buf *bp;

	bp = kmalloc(sizeof(struct apfs_buf), GFP_NOFS);
	if (!bp)
		return NULL;
	init_completion(&bp->io_wait);

	return bp;
}

void apfs_buf_init(struct apfs_fs_info *fs_info, struct apfs_buf *bp,
		   int op, u64 bytenr, size_t size)
{
	bp->fs_info = fs_info;
	bp->offset = offset_in_page(bytenr);
	bp->len = ALIGN(size, 1 << 9);
	bp->page_count = DIV_ROUND_UP(bp->len, PAGE_SIZE);
	bp->bno = bytenr >> 9;

	switch (op) {
	case ABF_READ:
		bp->op = REQ_OP_READ;
		break;
	case ABF_WRITE:
		bp->op = REQ_OP_WRITE;
		break;
	default:
		BUG_ON(1);
	}
}


/*
 * Wait for I/O completion of a sync buffer and return the I/O error code.
 */
static int
apfs_buf_iowait(
	struct apfs_buf	*bp)
{
	wait_for_completion(&bp->io_wait);

	return bp->error;
}


int apfs_buf_submit(struct apfs_buf *bp, bool wait)
{

	/* clear the internal error state to avoid spurious errors */
	bp->error = 0;
	bp->io_errors = 0;

	atomic_set(&bp->io_remaining, 1);

	apfs_buf_ioapply(bp);

	if (wait)
		wait_for_completion(&bp->io_wait);

	return bp->error;
}
