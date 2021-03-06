// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2007 Oracle.  All rights reserved.
 */

#include <asm/unaligned.h>

#include "ctree.h"

static bool check_setget_bounds(const struct extent_buffer *eb,
				const void *ptr, unsigned off, int size)
{
	const unsigned long member_offset = (unsigned long)ptr + off;

	if (member_offset > eb->len) {
		apfs_warn(eb->fs_info,
	"bad eb member start: ptr 0x%lx start %llu member offset %lu size %d",
			(unsigned long)ptr, eb->start, member_offset, size);
		return false;
	}
	if (member_offset + size > eb->len) {
		apfs_warn(eb->fs_info,
	"bad eb member end: ptr 0x%lx start %llu member offset %lu size %d",
			(unsigned long)ptr, eb->start, member_offset, size);
		return false;
	}

	return true;
}

/*
 * Macro templates that define helpers to read/write extent buffer data of a
 * given size, that are also used via ctree.h for access to item members by
 * specialized helpers.
 *
 * Generic helpers:
 * - apfs_set_8 (for 8/16/32/64)
 * - apfs_get_8 (for 8/16/32/64)
 *
 * Generic helpers with a token (cached address of the most recently accessed
 * page):
 * - apfs_set_token_8 (for 8/16/32/64)
 * - apfs_get_token_8 (for 8/16/32/64)
 *
 * The set/get functions handle data spanning two pages transparently, in case
 * metadata block size is larger than page.  Every pointer to metadata items is
 * an offset into the extent buffer page array, cast to a specific type.  This
 * gives us all the type checking.
 *
 * The extent buffer pages stored in the array pages do not form a contiguous
 * phyusical range, but the API functions assume the linear offset to the range
 * from 0 to metadata node size.
 */

#define DEFINE_APFS_SETGET_BITS(bits)					\
u##bits apfs_get_token_##bits(struct apfs_map_token *token,		\
			       const void *ptr, unsigned long off)	\
{									\
	const unsigned long member_offset = (unsigned long)ptr + off;	\
	const unsigned long idx = get_eb_page_index(member_offset);	\
	const unsigned long oip = get_eb_offset_in_page(token->eb,	\
							member_offset);	\
	const int size = sizeof(u##bits);				\
	u8 lebytes[sizeof(u##bits)];					\
	const int part = PAGE_SIZE - oip;				\
									\
	ASSERT(token);							\
	ASSERT(token->kaddr);						\
	ASSERT(check_setget_bounds(token->eb, ptr, off, size));		\
	if (token->offset <= member_offset &&				\
	    member_offset + size <= token->offset + PAGE_SIZE) {	\
		return get_unaligned_le##bits(token->kaddr + oip);	\
	}								\
	token->kaddr = page_address(token->eb->pages[idx]);		\
	token->offset = idx << PAGE_SHIFT;				\
	if (oip + size <= PAGE_SIZE)					\
		return get_unaligned_le##bits(token->kaddr + oip);	\
									\
	memcpy(lebytes, token->kaddr + oip, part);			\
	token->kaddr = page_address(token->eb->pages[idx + 1]);		\
	token->offset = (idx + 1) << PAGE_SHIFT;			\
	memcpy(lebytes + part, token->kaddr, size - part);		\
	return get_unaligned_le##bits(lebytes);				\
}									\
u##bits apfs_get_##bits(const struct extent_buffer *eb,		\
			 const void *ptr, unsigned long off)		\
{									\
	const unsigned long member_offset = (unsigned long)ptr + off;	\
	const unsigned long oip = get_eb_offset_in_page(eb, member_offset); \
	const unsigned long idx = get_eb_page_index(member_offset);	\
	char *kaddr = page_address(eb->pages[idx]);			\
	const int size = sizeof(u##bits);				\
	const int part = PAGE_SIZE - oip;				\
	u8 lebytes[sizeof(u##bits)];					\
									\
	ASSERT(check_setget_bounds(eb, ptr, off, size));		\
	if (oip + size <= PAGE_SIZE)					\
		return get_unaligned_le##bits(kaddr + oip);		\
									\
	memcpy(lebytes, kaddr + oip, part);				\
	kaddr = page_address(eb->pages[idx + 1]);			\
	memcpy(lebytes + part, kaddr, size - part);			\
	return get_unaligned_le##bits(lebytes);				\
}									\
void apfs_set_token_##bits(struct apfs_map_token *token,		\
			    const void *ptr, unsigned long off,		\
			    u##bits val)				\
{									\
	const unsigned long member_offset = (unsigned long)ptr + off;	\
	const unsigned long idx = get_eb_page_index(member_offset);	\
	const unsigned long oip = get_eb_offset_in_page(token->eb,	\
							member_offset);	\
	const int size = sizeof(u##bits);				\
	u8 lebytes[sizeof(u##bits)];					\
	const int part = PAGE_SIZE - oip;				\
									\
	ASSERT(token);							\
	ASSERT(token->kaddr);						\
	ASSERT(check_setget_bounds(token->eb, ptr, off, size));		\
	if (token->offset <= member_offset &&				\
	    member_offset + size <= token->offset + PAGE_SIZE) {	\
		put_unaligned_le##bits(val, token->kaddr + oip);	\
		return;							\
	}								\
	token->kaddr = page_address(token->eb->pages[idx]);		\
	token->offset = idx << PAGE_SHIFT;				\
	if (oip + size <= PAGE_SIZE) {					\
		put_unaligned_le##bits(val, token->kaddr + oip);	\
		return;							\
	}								\
	put_unaligned_le##bits(val, lebytes);				\
	memcpy(token->kaddr + oip, lebytes, part);			\
	token->kaddr = page_address(token->eb->pages[idx + 1]);		\
	token->offset = (idx + 1) << PAGE_SHIFT;			\
	memcpy(token->kaddr, lebytes + part, size - part);		\
}									\
void apfs_set_##bits(const struct extent_buffer *eb, void *ptr,	\
		      unsigned long off, u##bits val)			\
{									\
	const unsigned long member_offset = (unsigned long)ptr + off;	\
	const unsigned long oip = get_eb_offset_in_page(eb, member_offset); \
	const unsigned long idx = get_eb_page_index(member_offset);	\
	char *kaddr = page_address(eb->pages[idx]);			\
	const int size = sizeof(u##bits);				\
	const int part = PAGE_SIZE - oip;				\
	u8 lebytes[sizeof(u##bits)];					\
									\
	ASSERT(check_setget_bounds(eb, ptr, off, size));		\
	if (oip + size <= PAGE_SIZE) {					\
		put_unaligned_le##bits(val, kaddr + oip);		\
		return;							\
	}								\
									\
	put_unaligned_le##bits(val, lebytes);				\
	memcpy(kaddr + oip, lebytes, part);				\
	kaddr = page_address(eb->pages[idx + 1]);			\
	memcpy(kaddr, lebytes + part, size - part);			\
}

DEFINE_APFS_SETGET_BITS(8)
DEFINE_APFS_SETGET_BITS(16)
DEFINE_APFS_SETGET_BITS(32)
DEFINE_APFS_SETGET_BITS(64)

void apfs_node_key(const struct extent_buffer *eb,
		    struct apfs_disk_key *disk_key, int nr)
{
	unsigned long ptr = apfs_node_key_ptr_offset(nr);
	read_eb_member(eb, (struct apfs_key_ptr *)ptr,
		       struct apfs_key_ptr, key, disk_key);
}
