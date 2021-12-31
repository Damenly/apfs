/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2008 Oracle.  All rights reserved.
 */

#ifndef APFS_COMPRESSION_H
#define APFS_COMPRESSION_H

#include <linux/sizes.h>

struct apfs_inode;

/*
 * We want to make sure that amount of RAM required to uncompress an extent is
 * reasonable, so we limit the total size in ram of a compressed extent to
 * 128k.  This is a crucial number because it also controls how easily we can
 * spread reads across cpus for decompression.
 *
 * We also want to make sure the amount of IO required to do a random read is
 * reasonably small, so we limit the size of a compressed extent to 128k.
 */

/* Maximum length of compressed data stored on disk */
#define APFS_MAX_COMPRESSED		SZ_64K
/* Maximum size of data before compression */
#define APFS_MAX_UNCOMPRESSED		(SZ_64K)

#define	APFS_ZLIB_DEFAULT_LEVEL		3

struct compressed_bio {
	/* number of bios pending for this compressed extent */
	refcount_t pending_bios;

	/* Number of compressed pages in the array */
	unsigned int nr_pages;

	/* the pages with the compressed data on them */
	struct page **compressed_pages;

	/* inode that owns this data */
	struct inode *inode;

	/* starting offset in the inode for our pages */
	u64 start;

	/* starting offset in the extent for our pages */
	u64 offset;

	/* Number of bytes in the inode we're working on */
	unsigned int len;

	/* Number of bytes on disk */
	unsigned int compressed_len;

	/* The compression algorithm for this bio */
	u8 compress_type;

	/* IO errors */
	u8 errors;
	int mirror_num;

	/* for reads, this is the bio we are copying the data into */
	struct bio *orig_bio;

	/*
	 * the start of a variable length array of checksums only
	 * used by reads
	 */
	u8 sums[];
};

static inline unsigned int apfs_compress_type(unsigned int type_level)
{
	return (type_level & 0xF);
}

static inline unsigned int apfs_compress_level(unsigned int type_level)
{
	return ((type_level & 0xF0) >> 4);
}

void __init apfs_init_compress(void);
void __cold apfs_exit_compress(void);

int apfs_compress_pages(unsigned int type_level, struct address_space *mapping,
			 u64 start, struct page **pages,
			 unsigned long *out_pages,
			 unsigned long *total_in,
			 unsigned long *total_out);
int apfs_decompress(int type, unsigned char *data_in, struct page *dest_page,
		     unsigned long start_byte, size_t srclen, size_t destlen);
int apfs_decompress_buf2page(const char *buf, unsigned long buf_start,
			      unsigned long total_out, u64 disk_start,
			      struct bio *bio);

blk_status_t apfs_submit_compressed_write(struct apfs_inode *inode, u64 start,
				  unsigned int len, u64 disk_start,
				  unsigned int compressed_len,
				  struct page **compressed_pages,
				  unsigned int nr_pages,
				  unsigned int write_flags,
				  struct cgroup_subsys_state *blkcg_css);
blk_status_t apfs_submit_compressed_read(struct inode *inode, struct bio *bio,
				 int mirror_num, unsigned long bio_flags);

unsigned int apfs_compress_str2level(unsigned int type, const char *str);

enum apfs_compression_type {
	APFS_COMPRESS_NONE  = 0,
	APFS_COMPRESS_ZLIB  = 1,
	APFS_COMPRESS_LZO   = 2,
	APFS_COMPRESS_ZSTD  = 3,
	APFS_COMPRESS_ZLIB_ATTR  = 3,
	APFS_COMPRESS_ZLIB_RSRC  = 4,
	APFS_COMPRESS_UNUSED_5 = 5,
	APFS_COMPRESS_UNUSED_6 = 6,
	APFS_COMPRESS_LZVN_ATTR  = 7,
	APFS_COMPRESS_LZVN_RSRC  = 8,
	APFS_COMPRESS_PLAIN_ATTR = 9,  /* uncompressed data */
	APFS_COMPRESS_PLAIN_RSRC = 10,  /* uncompressed data in in 64K chunks */
	APFS_COMPRESS_LZFSE_ATTR = 11,
	APFS_COMPRESS_LZFSE_RSRC = 12,
	APFS_NR_COMPRESS_TYPES = 13,
	APFS_COMPRESS_MAX = 255,
};

struct workspace_manager {
	struct list_head idle_ws;
	spinlock_t ws_lock;
	/* Number of free workspaces */
	int free_ws;
	/* Total number of allocated workspaces */
	atomic_t total_ws;
	/* Waiters for a free workspace */
	wait_queue_head_t ws_wait;
};

struct list_head *apfs_get_workspace(int type, unsigned int level);
void apfs_put_workspace(int type, struct list_head *ws);

struct apfs_compress_op {
	struct workspace_manager *workspace_manager;
	/* Maximum level supported by the compression algorithm */
	unsigned int max_level;
	unsigned int default_level;
};

/* The heuristic workspaces are managed via the 0th workspace manager */
#define APFS_NR_WORKSPACE_MANAGERS	APFS_NR_COMPRESS_TYPES

extern const struct apfs_compress_op apfs_heuristic_compress;
extern const struct apfs_compress_op apfs_zlib_compress;
extern const struct apfs_compress_op apfs_lzo_compress;
extern const struct apfs_compress_op apfs_zstd_compress;
extern const struct apfs_compress_op apfs_lzfse_compress;
extern const struct apfs_compress_op apfs_lzvn_compress;

const char* apfs_compress_type2str(enum apfs_compression_type type);
bool apfs_compress_is_valid_type(u32 type);

int apfs_compress_heuristic(struct inode *inode, u64 start, u64 end);

int zlib_compress_pages(struct list_head *ws, struct address_space *mapping,
		u64 start, struct page **pages, unsigned long *out_pages,
		unsigned long *total_in, unsigned long *total_out);
int zlib_decompress_bio(struct list_head *ws, struct compressed_bio *cb);
int zlib_decompress(struct list_head *ws, unsigned char *data_in,
		struct page *dest_page, unsigned long start_byte, size_t srclen,
		size_t destlen);
struct list_head *zlib_alloc_workspace(unsigned int level);
void zlib_free_workspace(struct list_head *ws);
struct list_head *zlib_get_workspace(unsigned int level);

struct list_head *lzfse_get_workspace(unsigned int level);
int lzfse_compress_pages(struct list_head *ws, struct address_space *mapping,
		u64 start, struct page **pages, unsigned long *out_pages,
		unsigned long *total_in, unsigned long *total_out);
int lzfse_decompress_bio(struct list_head *ws, struct compressed_bio *cb);
int lzfse_decompress(struct list_head *ws, unsigned char *data_in,
		struct page *dest_page, unsigned long start_byte, size_t srclen,
		size_t destlen);
struct list_head *lzfse_alloc_workspace(unsigned int level);
void lzfse_free_workspace(struct list_head *ws);

struct list_head *lzvn_get_workspace(unsigned int level);
int lzvn_compress_pages(struct list_head *ws, struct address_space *mapping,
		u64 start, struct page **pages, unsigned long *out_pages,
		unsigned long *total_in, unsigned long *total_out);
int lzvn_decompress_bio(struct list_head *ws, struct compressed_bio *cb);
int lzvn_decompress(struct list_head *ws, unsigned char *data_in,
		struct page *dest_page, unsigned long start_byte, size_t srclen,
		size_t destlen);
struct list_head *lzvn_alloc_workspace(unsigned int level);
void lzvn_free_workspace(struct list_head *ws);


int lzo_compress_pages(struct list_head *ws, struct address_space *mapping,
		u64 start, struct page **pages, unsigned long *out_pages,
		unsigned long *total_in, unsigned long *total_out);
int lzo_decompress_bio(struct list_head *ws, struct compressed_bio *cb);
int lzo_decompress(struct list_head *ws, unsigned char *data_in,
		struct page *dest_page, unsigned long start_byte, size_t srclen,
		size_t destlen);
struct list_head *lzo_alloc_workspace(unsigned int level);
void lzo_free_workspace(struct list_head *ws);

int zstd_compress_pages(struct list_head *ws, struct address_space *mapping,
		u64 start, struct page **pages, unsigned long *out_pages,
		unsigned long *total_in, unsigned long *total_out);
int zstd_decompress_bio(struct list_head *ws, struct compressed_bio *cb);
int zstd_decompress(struct list_head *ws, unsigned char *data_in,
		struct page *dest_page, unsigned long start_byte, size_t srclen,
		size_t destlen);
void zstd_init_workspace_manager(void);
void zstd_cleanup_workspace_manager(void);
struct list_head *zstd_alloc_workspace(unsigned int level);
void zstd_free_workspace(struct list_head *ws);
struct list_head *zstd_get_workspace(unsigned int level);
void zstd_put_workspace(struct list_head *ws);

#endif
