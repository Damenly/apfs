// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2022 Su Yue <l@damenly.org>
 * All Rights Reserved.
 */
#ifndef __APFS_BUF_H__
#define __APFS_BUF_H__

#include <linux/types.h>
#include <linux/completion.h>

#define ABF_READ 0
#define ABF_WRITE 1

struct apfs_fs_info;

struct apfs_buf {
	struct apfs_fs_info *fs_info;
	struct page **pages;
	unsigned int page_count;
	int io_errors;
	atomic_t io_remaining;
	u64 bno; /* block number for I/O */
	size_t len; /* size of I/O */
	u32 offset;
	int error;
	int op;
	struct completion io_wait;	/* queue for I/O waiters */
};


void apfs_buf_free(struct apfs_buf *bp);
struct apfs_buf *apfs_buf_alloc(void);
int apfs_buf_alloc_pages(struct apfs_buf *bp, u32 flags);
int apfs_buf_submit(struct apfs_buf *bp, bool wait);
void apfs_buf_init(struct apfs_fs_info *fs_info, struct apfs_buf *bp, int op,
		   u64 bytenr, size_t size);

#endif	/* __APFS_BUF_H__ */
