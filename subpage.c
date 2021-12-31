// SPDX-License-Identifier: GPL-2.0

#include <linux/slab.h>
#include "ctree.h"
#include "subpage.h"
#include "apfs_inode.h"

/*
 * Subpage (sectorsize < PAGE_SIZE) support overview:
 *
 * Limitations:
 *
 * - Only support 64K page size for now
 *   This is to make metadata handling easier, as 64K page would ensure
 *   all nodesize would fit inside one page, thus we don't need to handle
 *   cases where a tree block crosses several pages.
 *
 * - Only metadata read-write for now
 *   The data read-write part is in development.
 *
 * - Metadata can't cross 64K page boundary
 *   apfs-progs and kernel have done that for a while, thus only ancient
 *   filesystems could have such problem.  For such case, do a graceful
 *   rejection.
 *
 * Special behavior:
 *
 * - Metadata
 *   Metadata read is fully supported.
 *   Meaning when reading one tree block will only trigger the read for the
 *   needed range, other unrelated range in the same page will not be touched.
 *
 *   Metadata write support is partial.
 *   The writeback is still for the full page, but we will only submit
 *   the dirty extent buffers in the page.
 *
 *   This means, if we have a metadata page like this:
 *
 *   Page offset
 *   0         16K         32K         48K        64K
 *   |/////////|           |///////////|
 *        \- Tree block A        \- Tree block B
 *
 *   Even if we just want to writeback tree block A, we will also writeback
 *   tree block B if it's also dirty.
 *
 *   This may cause extra metadata writeback which results more COW.
 *
 * Implementation:
 *
 * - Common
 *   Both metadata and data will use a new structure, apfs_subpage, to
 *   record the status of each sector inside a page.  This provides the extra
 *   granularity needed.
 *
 * - Metadata
 *   Since we have multiple tree blocks inside one page, we can't rely on page
 *   locking anymore, or we will have greatly reduced concurrency or even
 *   deadlocks (hold one tree lock while trying to lock another tree lock in
 *   the same page).
 *
 *   Thus for metadata locking, subpage support relies on io_tree locking only.
 *   This means a slightly higher tree locking latency.
 */

int apfs_attach_subpage(const struct apfs_fs_info *fs_info,
			 struct page *page, enum apfs_subpage_type type)
{
	struct apfs_subpage *subpage = NULL;
	int ret;

	/*
	 * We have cases like a dummy extent buffer page, which is not mappped
	 * and doesn't need to be locked.
	 */
	if (page->mapping)
		ASSERT(PageLocked(page));
	/* Either not subpage, or the page already has private attached */
	if (fs_info->sectorsize == PAGE_SIZE || PagePrivate(page))
		return 0;

	ret = apfs_alloc_subpage(fs_info, &subpage, type);
	if (ret < 0)
		return ret;
	attach_page_private(page, subpage);
	return 0;
}

void apfs_detach_subpage(const struct apfs_fs_info *fs_info,
			  struct page *page)
{
	struct apfs_subpage *subpage;

	/* Either not subpage, or already detached */
	if (fs_info->sectorsize == PAGE_SIZE || !PagePrivate(page))
		return;

	subpage = (struct apfs_subpage *)detach_page_private(page);
	ASSERT(subpage);
	apfs_free_subpage(subpage);
}

int apfs_alloc_subpage(const struct apfs_fs_info *fs_info,
			struct apfs_subpage **ret,
			enum apfs_subpage_type type)
{
	if (fs_info->sectorsize == PAGE_SIZE)
		return 0;

	*ret = kzalloc(sizeof(struct apfs_subpage), GFP_NOFS);
	if (!*ret)
		return -ENOMEM;
	spin_lock_init(&(*ret)->lock);
	if (type == APFS_SUBPAGE_METADATA) {
		atomic_set(&(*ret)->eb_refs, 0);
	} else {
		atomic_set(&(*ret)->readers, 0);
		atomic_set(&(*ret)->writers, 0);
	}
	return 0;
}

void apfs_free_subpage(struct apfs_subpage *subpage)
{
	kfree(subpage);
}

/*
 * Increase the eb_refs of current subpage.
 *
 * This is important for eb allocation, to prevent race with last eb freeing
 * of the same page.
 * With the eb_refs increased before the eb inserted into radix tree,
 * detach_extent_buffer_page() won't detach the page private while we're still
 * allocating the extent buffer.
 */
void apfs_page_inc_eb_refs(const struct apfs_fs_info *fs_info,
			    struct page *page)
{
	struct apfs_subpage *subpage;

	if (fs_info->sectorsize == PAGE_SIZE)
		return;

	ASSERT(PagePrivate(page) && page->mapping);
	lockdep_assert_held(&page->mapping->private_lock);

	subpage = (struct apfs_subpage *)page->private;
	atomic_inc(&subpage->eb_refs);
}

void apfs_page_dec_eb_refs(const struct apfs_fs_info *fs_info,
			    struct page *page)
{
	struct apfs_subpage *subpage;

	if (fs_info->sectorsize == PAGE_SIZE)
		return;

	ASSERT(PagePrivate(page) && page->mapping);
	lockdep_assert_held(&page->mapping->private_lock);

	subpage = (struct apfs_subpage *)page->private;
	ASSERT(atomic_read(&subpage->eb_refs));
	atomic_dec(&subpage->eb_refs);
}

static void apfs_subpage_assert(const struct apfs_fs_info *fs_info,
		struct page *page, u64 start, u32 len)
{
	/* Basic checks */
	ASSERT(PagePrivate(page) && page->private);
	ASSERT(IS_ALIGNED(start, fs_info->sectorsize) &&
	       IS_ALIGNED(len, fs_info->sectorsize));
	/*
	 * The range check only works for mapped page, we can still have
	 * unmapped page like dummy extent buffer pages.
	 */
	if (page->mapping)
		ASSERT(page_offset(page) <= start &&
		       start + len <= page_offset(page) + PAGE_SIZE);
}

void apfs_subpage_start_reader(const struct apfs_fs_info *fs_info,
		struct page *page, u64 start, u32 len)
{
	struct apfs_subpage *subpage = (struct apfs_subpage *)page->private;
	const int nbits = len >> fs_info->sectorsize_bits;

	apfs_subpage_assert(fs_info, page, start, len);

	atomic_add(nbits, &subpage->readers);
}

void apfs_subpage_end_reader(const struct apfs_fs_info *fs_info,
		struct page *page, u64 start, u32 len)
{
	struct apfs_subpage *subpage = (struct apfs_subpage *)page->private;
	const int nbits = len >> fs_info->sectorsize_bits;
	bool is_data;
	bool last;

	apfs_subpage_assert(fs_info, page, start, len);
	is_data = is_data_inode(page->mapping->host);
	ASSERT(atomic_read(&subpage->readers) >= nbits);
	last = atomic_sub_and_test(nbits, &subpage->readers);

	/*
	 * For data we need to unlock the page if the last read has finished.
	 *
	 * And please don't replace @last with atomic_sub_and_test() call
	 * inside if () condition.
	 * As we want the atomic_sub_and_test() to be always executed.
	 */
	if (is_data && last)
		unlock_page(page);
}

static void apfs_subpage_clamp_range(struct page *page, u64 *start, u32 *len)
{
	u64 orig_start = *start;
	u32 orig_len = *len;

	*start = max_t(u64, page_offset(page), orig_start);
	*len = min_t(u64, page_offset(page) + PAGE_SIZE,
		     orig_start + orig_len) - *start;
}

void apfs_subpage_start_writer(const struct apfs_fs_info *fs_info,
		struct page *page, u64 start, u32 len)
{
	struct apfs_subpage *subpage = (struct apfs_subpage *)page->private;
	const int nbits = (len >> fs_info->sectorsize_bits);
	int ret;

	apfs_subpage_assert(fs_info, page, start, len);

	ASSERT(atomic_read(&subpage->readers) == 0);
	ret = atomic_add_return(nbits, &subpage->writers);
	ASSERT(ret == nbits);
}

bool apfs_subpage_end_and_test_writer(const struct apfs_fs_info *fs_info,
		struct page *page, u64 start, u32 len)
{
	struct apfs_subpage *subpage = (struct apfs_subpage *)page->private;
	const int nbits = (len >> fs_info->sectorsize_bits);

	apfs_subpage_assert(fs_info, page, start, len);

	ASSERT(atomic_read(&subpage->writers) >= nbits);
	return atomic_sub_and_test(nbits, &subpage->writers);
}

/*
 * Lock a page for delalloc page writeback.
 *
 * Return -EAGAIN if the page is not properly initialized.
 * Return 0 with the page locked, and writer counter updated.
 *
 * Even with 0 returned, the page still need extra check to make sure
 * it's really the correct page, as the caller is using
 * find_get_pages_contig(), which can race with page invalidating.
 */
int apfs_page_start_writer_lock(const struct apfs_fs_info *fs_info,
		struct page *page, u64 start, u32 len)
{
	if (unlikely(!fs_info) || fs_info->sectorsize == PAGE_SIZE) {
		lock_page(page);
		return 0;
	}
	lock_page(page);
	if (!PagePrivate(page) || !page->private) {
		unlock_page(page);
		return -EAGAIN;
	}
	apfs_subpage_clamp_range(page, &start, &len);
	apfs_subpage_start_writer(fs_info, page, start, len);
	return 0;
}

void apfs_page_end_writer_lock(const struct apfs_fs_info *fs_info,
		struct page *page, u64 start, u32 len)
{
	if (unlikely(!fs_info) || fs_info->sectorsize == PAGE_SIZE)
		return unlock_page(page);
	apfs_subpage_clamp_range(page, &start, &len);
	if (apfs_subpage_end_and_test_writer(fs_info, page, start, len))
		unlock_page(page);
}

/*
 * Convert the [start, start + len) range into a u16 bitmap
 *
 * For example: if start == page_offset() + 16K, len = 16K, we get 0x00f0.
 */
static u16 apfs_subpage_calc_bitmap(const struct apfs_fs_info *fs_info,
		struct page *page, u64 start, u32 len)
{
	const int bit_start = offset_in_page(start) >> fs_info->sectorsize_bits;
	const int nbits = len >> fs_info->sectorsize_bits;

	apfs_subpage_assert(fs_info, page, start, len);

	/*
	 * Here nbits can be 16, thus can go beyond u16 range. We make the
	 * first left shift to be calculate in unsigned long (at least u32),
	 * then truncate the result to u16.
	 */
	return (u16)(((1UL << nbits) - 1) << bit_start);
}

void apfs_subpage_set_uptodate(const struct apfs_fs_info *fs_info,
		struct page *page, u64 start, u32 len)
{
	struct apfs_subpage *subpage = (struct apfs_subpage *)page->private;
	const u16 tmp = apfs_subpage_calc_bitmap(fs_info, page, start, len);
	unsigned long flags;

	spin_lock_irqsave(&subpage->lock, flags);
	subpage->uptodate_bitmap |= tmp;
	if (subpage->uptodate_bitmap == U16_MAX)
		SetPageUptodate(page);
	spin_unlock_irqrestore(&subpage->lock, flags);
}

void apfs_subpage_clear_uptodate(const struct apfs_fs_info *fs_info,
		struct page *page, u64 start, u32 len)
{
	struct apfs_subpage *subpage = (struct apfs_subpage *)page->private;
	const u16 tmp = apfs_subpage_calc_bitmap(fs_info, page, start, len);
	unsigned long flags;

	spin_lock_irqsave(&subpage->lock, flags);
	subpage->uptodate_bitmap &= ~tmp;
	ClearPageUptodate(page);
	spin_unlock_irqrestore(&subpage->lock, flags);
}

void apfs_subpage_set_error(const struct apfs_fs_info *fs_info,
		struct page *page, u64 start, u32 len)
{
	struct apfs_subpage *subpage = (struct apfs_subpage *)page->private;
	const u16 tmp = apfs_subpage_calc_bitmap(fs_info, page, start, len);
	unsigned long flags;

	spin_lock_irqsave(&subpage->lock, flags);
	subpage->error_bitmap |= tmp;
	SetPageError(page);
	spin_unlock_irqrestore(&subpage->lock, flags);
}

void apfs_subpage_clear_error(const struct apfs_fs_info *fs_info,
		struct page *page, u64 start, u32 len)
{
	struct apfs_subpage *subpage = (struct apfs_subpage *)page->private;
	const u16 tmp = apfs_subpage_calc_bitmap(fs_info, page, start, len);
	unsigned long flags;

	spin_lock_irqsave(&subpage->lock, flags);
	subpage->error_bitmap &= ~tmp;
	if (subpage->error_bitmap == 0)
		ClearPageError(page);
	spin_unlock_irqrestore(&subpage->lock, flags);
}

void apfs_subpage_set_dirty(const struct apfs_fs_info *fs_info,
		struct page *page, u64 start, u32 len)
{
	struct apfs_subpage *subpage = (struct apfs_subpage *)page->private;
	u16 tmp = apfs_subpage_calc_bitmap(fs_info, page, start, len);
	unsigned long flags;

	spin_lock_irqsave(&subpage->lock, flags);
	subpage->dirty_bitmap |= tmp;
	spin_unlock_irqrestore(&subpage->lock, flags);
	set_page_dirty(page);
}

/*
 * Extra clear_and_test function for subpage dirty bitmap.
 *
 * Return true if we're the last bits in the dirty_bitmap and clear the
 * dirty_bitmap.
 * Return false otherwise.
 *
 * NOTE: Callers should manually clear page dirty for true case, as we have
 * extra handling for tree blocks.
 */
bool apfs_subpage_clear_and_test_dirty(const struct apfs_fs_info *fs_info,
		struct page *page, u64 start, u32 len)
{
	struct apfs_subpage *subpage = (struct apfs_subpage *)page->private;
	u16 tmp = apfs_subpage_calc_bitmap(fs_info, page, start, len);
	unsigned long flags;
	bool last = false;

	spin_lock_irqsave(&subpage->lock, flags);
	subpage->dirty_bitmap &= ~tmp;
	if (subpage->dirty_bitmap == 0)
		last = true;
	spin_unlock_irqrestore(&subpage->lock, flags);
	return last;
}

void apfs_subpage_clear_dirty(const struct apfs_fs_info *fs_info,
		struct page *page, u64 start, u32 len)
{
	bool last;

	last = apfs_subpage_clear_and_test_dirty(fs_info, page, start, len);
	if (last)
		clear_page_dirty_for_io(page);
}

void apfs_subpage_set_writeback(const struct apfs_fs_info *fs_info,
		struct page *page, u64 start, u32 len)
{
	struct apfs_subpage *subpage = (struct apfs_subpage *)page->private;
	u16 tmp = apfs_subpage_calc_bitmap(fs_info, page, start, len);
	unsigned long flags;

	spin_lock_irqsave(&subpage->lock, flags);
	subpage->writeback_bitmap |= tmp;
	set_page_writeback(page);
	spin_unlock_irqrestore(&subpage->lock, flags);
}

void apfs_subpage_clear_writeback(const struct apfs_fs_info *fs_info,
		struct page *page, u64 start, u32 len)
{
	struct apfs_subpage *subpage = (struct apfs_subpage *)page->private;
	u16 tmp = apfs_subpage_calc_bitmap(fs_info, page, start, len);
	unsigned long flags;

	spin_lock_irqsave(&subpage->lock, flags);
	subpage->writeback_bitmap &= ~tmp;
	if (subpage->writeback_bitmap == 0)
		end_page_writeback(page);
	spin_unlock_irqrestore(&subpage->lock, flags);
}

void apfs_subpage_set_ordered(const struct apfs_fs_info *fs_info,
		struct page *page, u64 start, u32 len)
{
	struct apfs_subpage *subpage = (struct apfs_subpage *)page->private;
	const u16 tmp = apfs_subpage_calc_bitmap(fs_info, page, start, len);
	unsigned long flags;

	spin_lock_irqsave(&subpage->lock, flags);
	subpage->ordered_bitmap |= tmp;
	SetPageOrdered(page);
	spin_unlock_irqrestore(&subpage->lock, flags);
}

void apfs_subpage_clear_ordered(const struct apfs_fs_info *fs_info,
		struct page *page, u64 start, u32 len)
{
	struct apfs_subpage *subpage = (struct apfs_subpage *)page->private;
	const u16 tmp = apfs_subpage_calc_bitmap(fs_info, page, start, len);
	unsigned long flags;

	spin_lock_irqsave(&subpage->lock, flags);
	subpage->ordered_bitmap &= ~tmp;
	if (subpage->ordered_bitmap == 0)
		ClearPageOrdered(page);
	spin_unlock_irqrestore(&subpage->lock, flags);
}
/*
 * Unlike set/clear which is dependent on each page status, for test all bits
 * are tested in the same way.
 */
#define IMPLEMENT_APFS_SUBPAGE_TEST_OP(name)				\
bool apfs_subpage_test_##name(const struct apfs_fs_info *fs_info,	\
		struct page *page, u64 start, u32 len)			\
{									\
	struct apfs_subpage *subpage = (struct apfs_subpage *)page->private; \
	const u16 tmp = apfs_subpage_calc_bitmap(fs_info, page, start, len); \
	unsigned long flags;						\
	bool ret;							\
									\
	spin_lock_irqsave(&subpage->lock, flags);			\
	ret = ((subpage->name##_bitmap & tmp) == tmp);			\
	spin_unlock_irqrestore(&subpage->lock, flags);			\
	return ret;							\
}
IMPLEMENT_APFS_SUBPAGE_TEST_OP(uptodate);
IMPLEMENT_APFS_SUBPAGE_TEST_OP(error);
IMPLEMENT_APFS_SUBPAGE_TEST_OP(dirty);
IMPLEMENT_APFS_SUBPAGE_TEST_OP(writeback);
IMPLEMENT_APFS_SUBPAGE_TEST_OP(ordered);

/*
 * Note that, in selftests (extent-io-tests), we can have empty fs_info passed
 * in.  We only test sectorsize == PAGE_SIZE cases so far, thus we can fall
 * back to regular sectorsize branch.
 */
#define IMPLEMENT_APFS_PAGE_OPS(name, set_page_func, clear_page_func,	\
			       test_page_func)				\
void apfs_page_set_##name(const struct apfs_fs_info *fs_info,		\
		struct page *page, u64 start, u32 len)			\
{									\
	if (unlikely(!fs_info) || fs_info->sectorsize == PAGE_SIZE) {	\
		set_page_func(page);					\
		return;							\
	}								\
	apfs_subpage_set_##name(fs_info, page, start, len);		\
}									\
void apfs_page_clear_##name(const struct apfs_fs_info *fs_info,	\
		struct page *page, u64 start, u32 len)			\
{									\
	if (unlikely(!fs_info) || fs_info->sectorsize == PAGE_SIZE) {	\
		clear_page_func(page);					\
		return;							\
	}								\
	apfs_subpage_clear_##name(fs_info, page, start, len);		\
}									\
bool apfs_page_test_##name(const struct apfs_fs_info *fs_info,	\
		struct page *page, u64 start, u32 len)			\
{									\
	if (unlikely(!fs_info) || fs_info->sectorsize == PAGE_SIZE)	\
		return test_page_func(page);				\
	return apfs_subpage_test_##name(fs_info, page, start, len);	\
}									\
void apfs_page_clamp_set_##name(const struct apfs_fs_info *fs_info,	\
		struct page *page, u64 start, u32 len)			\
{									\
	if (unlikely(!fs_info) || fs_info->sectorsize == PAGE_SIZE) {	\
		set_page_func(page);					\
		return;							\
	}								\
	apfs_subpage_clamp_range(page, &start, &len);			\
	apfs_subpage_set_##name(fs_info, page, start, len);		\
}									\
void apfs_page_clamp_clear_##name(const struct apfs_fs_info *fs_info, \
		struct page *page, u64 start, u32 len)			\
{									\
	if (unlikely(!fs_info) || fs_info->sectorsize == PAGE_SIZE) {	\
		clear_page_func(page);					\
		return;							\
	}								\
	apfs_subpage_clamp_range(page, &start, &len);			\
	apfs_subpage_clear_##name(fs_info, page, start, len);		\
}									\
bool apfs_page_clamp_test_##name(const struct apfs_fs_info *fs_info,	\
		struct page *page, u64 start, u32 len)			\
{									\
	if (unlikely(!fs_info) || fs_info->sectorsize == PAGE_SIZE)	\
		return test_page_func(page);				\
	apfs_subpage_clamp_range(page, &start, &len);			\
	return apfs_subpage_test_##name(fs_info, page, start, len);	\
}
IMPLEMENT_APFS_PAGE_OPS(uptodate, SetPageUptodate, ClearPageUptodate,
			 PageUptodate);
IMPLEMENT_APFS_PAGE_OPS(error, SetPageError, ClearPageError, PageError);
IMPLEMENT_APFS_PAGE_OPS(dirty, set_page_dirty, clear_page_dirty_for_io,
			 PageDirty);
IMPLEMENT_APFS_PAGE_OPS(writeback, set_page_writeback, end_page_writeback,
			 PageWriteback);
IMPLEMENT_APFS_PAGE_OPS(ordered, SetPageOrdered, ClearPageOrdered,
			 PageOrdered);
