// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2021 Su Yue <l@damenly.su>.  All rights reserved.
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/zutil.h>
#include <linux/mm.h>
#include <linux/init.h>
#include <linux/err.h>
#include <linux/sched.h>
#include <linux/pagemap.h>
#include <linux/bio.h>
#include <linux/refcount.h>

#include "ctree.h"
#include "compression.h"

extern size_t lzvn_decode_scratch_size(void);
extern size_t lzvn_encode_scratch_size(void);
extern size_t lzvn_encode_buffer(void *dst, size_t dst_size,
			  const void *src, size_t src_size,
			  void *work);
extern size_t lzvn_decode_buffer(void *dst, size_t dst_size,
			  const void *src, size_t src_size,
			  void *work);

#define LZVN_COMPRESSED_BUF_SIZE (APFS_MAX_COMPRESSED + 4096)
#define LZVN_DECOMPRESSED_BUF_SIZE APFS_MAX_UNCOMPRESSED

struct workspace {
	char *scratch;
	unsigned int scratch_size;
	char *compressed_buf;
	char *decompressed_buf;
	unsigned int buf_size;
	struct list_head list;
	int level;
};

static struct workspace_manager wsm;

struct list_head *lzvn_get_workspace(unsigned int level)
{
	struct list_head *ws = apfs_get_workspace(APFS_COMPRESS_LZVN_RSRC, level);
	struct workspace *workspace = list_entry(ws, struct workspace, list);

	workspace->level = level;
	trace_printk("lzvn get workspace %p\n", ws);

	return ws;
}

void lzvn_free_workspace(struct list_head *ws)
{
	struct workspace *workspace = list_entry(ws, struct workspace, list);

	kvfree(workspace->scratch);
	kvfree(workspace->compressed_buf);
	kvfree(workspace->decompressed_buf);
	kfree(workspace);
}

struct list_head *lzvn_alloc_workspace(unsigned int level)
{
	struct workspace *workspace;
	int workspacesize;

	workspace = kzalloc(sizeof(*workspace), GFP_KERNEL);
	if (!workspace)
		return ERR_PTR(-ENOMEM);

	workspacesize = max(lzvn_encode_scratch_size(),
			    lzvn_decode_scratch_size());
	workspace->scratch = kvmalloc(workspacesize, GFP_KERNEL);
	workspace->compressed_buf = NULL;
	workspace->decompressed_buf = NULL;

	if (!workspace->compressed_buf) {
		workspace->compressed_buf = kvmalloc(LZVN_COMPRESSED_BUF_SIZE,
						     GFP_KERNEL);
		workspace->buf_size = LZVN_COMPRESSED_BUF_SIZE;
	}

	if (!workspace->decompressed_buf) {
		workspace->decompressed_buf = kvmalloc(LZVN_DECOMPRESSED_BUF_SIZE,
						     GFP_KERNEL);
		workspace->buf_size = LZVN_DECOMPRESSED_BUF_SIZE;
	}

	if (!workspace->scratch ||
	    !workspace->compressed_buf ||
	    !workspace->decompressed_buf)
		goto fail;

	workspace->scratch_size = workspacesize;
	INIT_LIST_HEAD(&workspace->list);

	return &workspace->list;
fail:
	lzvn_free_workspace(&workspace->list);
	return ERR_PTR(-ENOMEM);
}

int lzvn_compress_pages(struct list_head *ws, struct address_space *mapping,
		u64 start, struct page **pages, unsigned long *out_pages,
		unsigned long *total_in, unsigned long *total_out)
{
	return -ENOTSUPP;
}

int lzvn_decompress_bio(struct list_head *ws, struct compressed_bio *cb)
{
	struct workspace *workspace = list_entry(ws, struct workspace, list);
	int ret = 0;
	char *data_in;
	size_t total_out = 0;
	size_t srclen = cb->compressed_len;
	unsigned long total_pages_in = cb->nr_pages;
	struct page **pages_in = cb->compressed_pages;
	u64 disk_start = cb->start;
	struct bio *orig_bio = cb->orig_bio;
	void *compressed_buf = workspace->compressed_buf;
	void *uncompressed_buf = workspace->decompressed_buf;
	int i;
	u64 copied;
	u64 extent_offset = cb->offset;
	u8 *cdata;
	u64 prev_offset = 0;
	u32 pg_offset = extent_offset % PAGE_SIZE;

	copied = 0;
	for (i = 0; i < total_pages_in; i++) {
		u32 len = PAGE_SIZE;

		data_in = kmap(pages_in[i]);

		if (i == 0) {
			data_in += pg_offset;
			len = PAGE_SIZE - pg_offset;
		}
		if (i == total_pages_in - 1) {
			len = srclen + pg_offset;
			if (len <= PAGE_SIZE)
				len = srclen;
			else if (IS_ALIGNED(len, PAGE_SIZE))
				len = PAGE_SIZE;
			else
				len = len % PAGE_SIZE;
		}

		ASSERT(len <= PAGE_SIZE);
		ASSERT(copied < srclen);

		ASSERT(copied + len <= srclen);
		ASSERT(copied + len <= LZVN_COMPRESSED_BUF_SIZE);

		memcpy(compressed_buf + copied, data_in, len);
		copied += len;

		kunmap(pages_in[i]);
	}

	if (copied != srclen) {
		trace_printk("APFS: ERROR cb->start %llu compressed len %zu offset %llu copied %llu nr_pages %d",
			     cb->start, srclen, extent_offset, copied,
			     cb->nr_pages);
	}

	ASSERT(copied == srclen);

	cdata = compressed_buf;
	/* uncompressed data */
	if (*cdata == 0x06) {
		total_out = APFS_MAX_UNCOMPRESSED;
		uncompressed_buf = cdata + 1;
		goto buf2page;
	}

	total_out = lzvn_decode_buffer(uncompressed_buf, APFS_MAX_UNCOMPRESSED,
				       compressed_buf, srclen, workspace->scratch);
	if (total_out == 0 || total_out > APFS_MAX_UNCOMPRESSED) {
		pr_info("APFS: lzvn decompressed bio failed,total out %lu cb start %llu compressed len %lu",
			total_out, cb->start, srclen);
		trace_printk("APFS: ERROR cb->start %llu compressed len %zu offset %llu copied %llu nr_pages %d cdata 0x%x",
			     cb->start, srclen, extent_offset, copied,
			     cb->nr_pages, *(u8 *)compressed_buf);
		return -EIO;
	}

buf2page:
	ret = apfs_decompress_buf2page(uncompressed_buf, 0, total_out,
				       disk_start, orig_bio);
	if (ret < 0) {
		trace_printk("APFS: lzvn failed to copy data to page total out %lu cb start %llu compressed len %lu prev_offset %llu offset %llu  orig_start %llu len\n",
			total_out, cb->start, srclen,
			prev_offset,
			page_offset(bio_iter_iovec(orig_bio, orig_bio->bi_iter).bv_page),
			disk_start);

		return -EIO;
	}
	zero_fill_bio(orig_bio);

	return 0;
}

int
lzvn_decompress(struct list_head *ws, unsigned char *data_in,
		 struct page *dest_page, unsigned long start_byte,
		 size_t srclen, size_t destlen)
{
	struct workspace *workspace = list_entry(ws, struct workspace, list);
	size_t outlen;
	int ret = 0;
	char *kaddr;
	unsigned long bytes;
	void *compressed_buf = data_in;
	void *uncompressed_buf = workspace->decompressed_buf;
	u8 *cdata;

	cdata = data_in;
	if (*cdata == 0x06) {
		uncompressed_buf = compressed_buf + 1;
		outlen = srclen - 1;
		goto no_decomp;
	}

	memset(workspace->scratch, 0, workspace->scratch_size);
	outlen = lzvn_decode_buffer(uncompressed_buf,
				    LZVN_DECOMPRESSED_BUF_SIZE,
				    compressed_buf, srclen,
				    workspace->scratch);
	if (outlen == 0) {
		pr_warn("APFS: decompress failed! first block 0x%x\n",
			*cdata);
		ret = -EIO;
		goto out;
	} else if (outlen == workspace->buf_size) {
		pr_warn("APFS: too small buf size, decompress failed!\n");
		ret = -E2BIG;
		goto out;
	}

no_decomp:
	/*
	 * the caller is already checking against PAGE_SIZE, but lets
	 * move this check closer to the memcpy/memset
	 */
	destlen = min_t(unsigned long, destlen, PAGE_SIZE);
	bytes = min_t(unsigned long, destlen, outlen - start_byte);

	kaddr = kmap_local_page(dest_page);
	memcpy(kaddr, uncompressed_buf + start_byte, bytes);

	/*
	 * apfs_getblock is doing a zero on the tail of the page too,
	 * but this will cover anything missing from the decompressed
	 * data.
	 */
	if (bytes < destlen)
		memset(kaddr+bytes, 0, destlen - bytes);
	kunmap_local(kaddr);
out:
	return ret;
}

const struct apfs_compress_op apfs_lzvn_compress = {
	.workspace_manager	= &wsm,
	.max_level		= 0,
	.default_level		= 0,
};
