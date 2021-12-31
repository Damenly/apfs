/* SPDX-License-Identifier: GPL-2.0 */

#ifndef APFS_SUBPAGE_H
#define APFS_SUBPAGE_H

#include <linux/spinlock.h>

/*
 * Maximum page size we support is 64K, minimum sector size is 4K, u16 bitmap
 * is sufficient. Regular bitmap_* is not used due to size reasons.
 */
#define APFS_SUBPAGE_BITMAP_SIZE	16

/*
 * Structure to trace status of each sector inside a page, attached to
 * page::private for both data and metadata inodes.
 */
struct apfs_subpage {
	/* Common members for both data and metadata pages */
	spinlock_t lock;
	u16 uptodate_bitmap;
	u16 error_bitmap;
	u16 dirty_bitmap;
	u16 writeback_bitmap;
	/*
	 * Both data and metadata needs to track how many readers are for the
	 * page.
	 * Data relies on @readers to unlock the page when last reader finished.
	 * While metadata doesn't need page unlock, it needs to prevent
	 * page::private get cleared before the last end_page_read().
	 */
	atomic_t readers;
	union {
		/*
		 * Structures only used by metadata
		 *
		 * @eb_refs should only be operated under private_lock, as it
		 * manages whether the subpage can be detached.
		 */
		atomic_t eb_refs;
		/* Structures only used by data */
		struct {
			atomic_t writers;

			/* Tracke pending ordered extent in this sector */
			u16 ordered_bitmap;
		};
	};
};

enum apfs_subpage_type {
	APFS_SUBPAGE_METADATA,
	APFS_SUBPAGE_DATA,
};

int apfs_attach_subpage(const struct apfs_fs_info *fs_info,
			 struct page *page, enum apfs_subpage_type type);
void apfs_detach_subpage(const struct apfs_fs_info *fs_info,
			  struct page *page);

/* Allocate additional data where page represents more than one sector */
int apfs_alloc_subpage(const struct apfs_fs_info *fs_info,
			struct apfs_subpage **ret,
			enum apfs_subpage_type type);
void apfs_free_subpage(struct apfs_subpage *subpage);

void apfs_page_inc_eb_refs(const struct apfs_fs_info *fs_info,
			    struct page *page);
void apfs_page_dec_eb_refs(const struct apfs_fs_info *fs_info,
			    struct page *page);

void apfs_subpage_start_reader(const struct apfs_fs_info *fs_info,
		struct page *page, u64 start, u32 len);
void apfs_subpage_end_reader(const struct apfs_fs_info *fs_info,
		struct page *page, u64 start, u32 len);

void apfs_subpage_start_writer(const struct apfs_fs_info *fs_info,
		struct page *page, u64 start, u32 len);
bool apfs_subpage_end_and_test_writer(const struct apfs_fs_info *fs_info,
		struct page *page, u64 start, u32 len);
int apfs_page_start_writer_lock(const struct apfs_fs_info *fs_info,
		struct page *page, u64 start, u32 len);
void apfs_page_end_writer_lock(const struct apfs_fs_info *fs_info,
		struct page *page, u64 start, u32 len);

/*
 * Template for subpage related operations.
 *
 * apfs_subpage_*() are for call sites where the page has subpage attached and
 * the range is ensured to be inside the page.
 *
 * apfs_page_*() are for call sites where the page can either be subpage
 * specific or regular page. The function will handle both cases.
 * But the range still needs to be inside the page.
 *
 * apfs_page_clamp_*() are similar to apfs_page_*(), except the range doesn't
 * need to be inside the page. Those functions will truncate the range
 * automatically.
 */
#define DECLARE_APFS_SUBPAGE_OPS(name)					\
void apfs_subpage_set_##name(const struct apfs_fs_info *fs_info,	\
		struct page *page, u64 start, u32 len);			\
void apfs_subpage_clear_##name(const struct apfs_fs_info *fs_info,	\
		struct page *page, u64 start, u32 len);			\
bool apfs_subpage_test_##name(const struct apfs_fs_info *fs_info,	\
		struct page *page, u64 start, u32 len);			\
void apfs_page_set_##name(const struct apfs_fs_info *fs_info,		\
		struct page *page, u64 start, u32 len);			\
void apfs_page_clear_##name(const struct apfs_fs_info *fs_info,	\
		struct page *page, u64 start, u32 len);			\
bool apfs_page_test_##name(const struct apfs_fs_info *fs_info,	\
		struct page *page, u64 start, u32 len);			\
void apfs_page_clamp_set_##name(const struct apfs_fs_info *fs_info,	\
		struct page *page, u64 start, u32 len);			\
void apfs_page_clamp_clear_##name(const struct apfs_fs_info *fs_info,	\
		struct page *page, u64 start, u32 len);			\
bool apfs_page_clamp_test_##name(const struct apfs_fs_info *fs_info,	\
		struct page *page, u64 start, u32 len);

DECLARE_APFS_SUBPAGE_OPS(uptodate);
DECLARE_APFS_SUBPAGE_OPS(error);
DECLARE_APFS_SUBPAGE_OPS(dirty);
DECLARE_APFS_SUBPAGE_OPS(writeback);
DECLARE_APFS_SUBPAGE_OPS(ordered);

bool apfs_subpage_clear_and_test_dirty(const struct apfs_fs_info *fs_info,
		struct page *page, u64 start, u32 len);

#endif
