/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2007 Oracle.  All rights reserved.
 */

#ifndef APFS_CTREE_H
#define APFS_CTREE_H

#include <linux/mm.h>
#include <linux/sched/signal.h>
#include <linux/highmem.h>
#include <linux/fs.h>
#include <linux/rwsem.h>
#include <linux/semaphore.h>
#include <linux/completion.h>
#include <linux/backing-dev.h>
#include <linux/wait.h>
#include <linux/slab.h>
#include <asm/unaligned.h>
#include <linux/pagemap.h>
#include "apfs.h"
#include "apfs_tree.h"
#include <linux/workqueue.h>
#include <linux/security.h>
#include <linux/sizes.h>
#include <linux/dynamic_debug.h>
#include <linux/refcount.h>
#include <linux/crc32c.h>
#include <linux/iomap.h>
#include "extent-io-tree.h"
#include "extent_io.h"
#include "extent_map.h"
#include "async-thread.h"
#include "block-rsv.h"
#include "locking.h"

#include "linux/ftrace.h"

struct apfs_trans_handle;
struct apfs_transaction;
struct apfs_pending_snapshot;
struct apfs_delayed_ref_root;
struct apfs_delayed_ref_head;
struct apfs_space_info;
struct apfs_block_group;
extern struct kmem_cache *apfs_trans_handle_cachep;
extern struct kmem_cache *apfs_bit_radix_cachep;
extern struct kmem_cache *apfs_path_cachep;
extern struct kmem_cache *apfs_free_space_cachep;
extern struct kmem_cache *apfs_free_space_bitmap_cachep;
struct apfs_ordered_sum;
struct apfs_ref;
struct apfs_obj_header;

#define APFS_MAGIC 0x4D5F53665248425FULL /* ascii _BHRfS_M, no null */
#define APFS_SUPER_MAGIC	0x42535041
#define APFS_TEST_MAGIC		0x73727279
/*
 * Block and Container Sizes
 */
#define APFS_MINIMUM_BLOCK_SIZE 4096
#define APFS_DEFAULT_BLOCK_SIZE 4096
#define APFS_MAXIMUM_BLOCK_SIZE 65536

#define APFS_MAX_METADATA_BLOCKSIZE 65536

#define APFS_MINIMUM_CONTAINER_SIZE 1048576
#define APFS_SUPER_INFO_SIZE 4096

#define APFS_FIXED_KEY_SIZE (sizeof(u64) << 1) // id + offset
#define APFS_FIXED_VAL_SIZE sizeof(u64) // node ptr

struct apfs_map_token {
	struct extent_buffer *eb;
	char *kaddr;
	unsigned long offset;
};

#define APFS_BYTES_TO_BLKS(fs_info, bytes) \
				((bytes) >> (fs_info)->sectorsize_bits)

static inline void apfs_init_map_token(struct apfs_map_token *token,
					struct extent_buffer *eb)
{
	token->eb = eb;
	token->kaddr = page_address(eb->pages[0]);
	token->offset = 0;
}

/* some macros to generate set/get functions for the struct fields.  This
 * assumes there is a lefoo_to_cpu for every type, so lets make a simple
 * one for u8:
 */
#define le8_to_cpu(v) (v)
#define cpu_to_le8(v) (v)
#define __le8 u8

static inline u8 get_unaligned_le8(const void *p)
{
       return *(u8 *)p;
}

static inline void put_unaligned_le8(u8 val, void *p)
{
       *(u8 *)p = val;
}

#define read_eb_member(eb, ptr, type, member, result) (\
	read_extent_buffer(eb, (char *)(result),			\
			   ((unsigned long)(ptr)) +			\
			    offsetof(type, member),			\
			   sizeof(((type *)0)->member)))

#define write_eb_member(eb, ptr, type, member, result) (\
	write_extent_buffer(eb, (char *)(result),			\
			   ((unsigned long)(ptr)) +			\
			    offsetof(type, member),			\
			   sizeof(((type *)0)->member)))

#define DECLARE_APFS_SETGET_BITS(bits)					\
u##bits apfs_get_token_##bits(struct apfs_map_token *token,		\
			       const void *ptr, unsigned long off);	\
void apfs_set_token_##bits(struct apfs_map_token *token,		\
			    const void *ptr, unsigned long off,		\
			    u##bits val);				\
u##bits apfs_get_##bits(const struct extent_buffer *eb,		\
			 const void *ptr, unsigned long off);		\
void apfs_set_##bits(const struct extent_buffer *eb, void *ptr,	\
		      unsigned long off, u##bits val);

DECLARE_APFS_SETGET_BITS(8)
DECLARE_APFS_SETGET_BITS(16)
DECLARE_APFS_SETGET_BITS(32)
DECLARE_APFS_SETGET_BITS(64)

#define APFS_SETGET_TOKEN_FUNCS(name, type, member, bits)		\
static inline u##bits apfs_token_##name(struct apfs_map_token *token,	\
					 const type *s)			\
{									\
	BUG();								\
}									\
									\
static inline void apfs_set_token_##name(struct apfs_map_token *token,\
					  type *s, u##bits val)		\
{									\
	BUG();								\
}


#define APFS_SETGET_FUNCS(name, type, member, bits)			\
static inline u##bits apfs_##name(const struct extent_buffer *eb,	\
				   const type *s)			\
{									\
	BUILD_BUG_ON(sizeof(u##bits) != sizeof(((type *)0))->member);	\
	return apfs_get_##bits(eb, s, offsetof(type, member));		\
}									\
static inline void apfs_set_##name(const struct extent_buffer *eb, type *s, \
				    u##bits val)			\
{									\
	BUILD_BUG_ON(sizeof(u##bits) != sizeof(((type *)0))->member);	\
	apfs_set_##bits(eb, s, offsetof(type, member), val);		\
}									\
static inline u##bits apfs_token_##name(struct apfs_map_token *token,	\
					 const type *s)			\
{									\
	BUILD_BUG_ON(sizeof(u##bits) != sizeof(((type *)0))->member);	\
	return apfs_get_token_##bits(token, s, offsetof(type, member));\
}									\
static inline void apfs_set_token_##name(struct apfs_map_token *token,\
					  type *s, u##bits val)		\
{									\
	BUILD_BUG_ON(sizeof(u##bits) != sizeof(((type *)0))->member);	\
	apfs_set_token_##bits(token, s, offsetof(type, member), val);	\
}

#define APFS_SETGET_HEADER_FUNCS(name, type, member, bits)		\
static inline u##bits apfs_##name(const struct extent_buffer *eb)	\
{									\
	const type *p = page_address(eb->pages[0]) +			\
			offset_in_page(eb->start);			\
	return get_unaligned_le##bits(&p->member);			\
}									\
static inline void apfs_set_##name(const struct extent_buffer *eb,	\
				    u##bits val)			\
{									\
	type *p = page_address(eb->pages[0]) + offset_in_page(eb->start); \
	put_unaligned_le##bits(val, &p->member);			\
}

#define APFS_SETGET_STACK_FUNCS(name, type, member, bits)		\
static inline u##bits apfs_##name(const type *s)			\
{									\
	return get_unaligned_le##bits(&s->member);			\
}									\
static inline void apfs_set_##name(type *s, u##bits val)		\
{									\
	put_unaligned_le##bits(val, &s->member);			\
}

#define APFS_SETGET_OBJ_FUNCS(name, stype)				\
static inline u64 apfs_##name##_oid(const stype *s)			\
{									\
	return get_unaligned_le64(&((const struct apfs_obj_header *)s)->oid); \
}									\
static inline void apfs_set_##name##_oid(stype *s, u64 val)		\
{									\
	put_unaligned_le64(val, &((struct apfs_obj_header *)s)->oid);	\
}									\
static inline u64 apfs_##name##_xid(const stype *s)			\
{									\
	return get_unaligned_le64(&s->o.xid); \
}									\
static inline void apfs_set_##name##_xid(stype *s, u64 val)		\
{									\
	put_unaligned_le64(val, &((struct apfs_obj_header *)s)->xid);	\
}									\
static inline u32 apfs_##name##_type(const stype *s)			\
{									\
	return get_unaligned_le64(&((const struct apfs_obj_header *)s)->type); \
}									\
static inline void apfs_set_##name##_type(stype *s, u32 val)		\
{									\
	put_unaligned_le64(val, &((struct apfs_obj_header *)s)->type);	\
}									\
static inline u32 apfs_##name##_subtype(const stype *s)			\
{									\
	return get_unaligned_le64(&((const struct apfs_obj_header *)s)->subtype); \
}									\
static inline void apfs_set_##name##_subtype(stype *s, u32 val)		\
{									\
	put_unaligned_le64(val, &((struct apfs_obj_header *)s)->subtype); \
}

static inline __printf(2, 3) __cold
void apfs_no_printk(const struct apfs_fs_info *fs_info, const char *fmt, ...)
{
}

#ifdef CONFIG_PRINTK
__printf(2, 3)
__cold
void apfs_printk(const struct apfs_fs_info *fs_info, const char *fmt, ...);
#else
#define apfs_printk(fs_info, fmt, args...) \
	apfs_no_printk(fs_info, fmt, ##args)
#endif

#define apfs_emerg(fs_info, fmt, args...) \
	apfs_printk(fs_info, KERN_EMERG fmt, ##args)
#define apfs_alert(fs_info, fmt, args...) \
	apfs_printk(fs_info, KERN_ALERT fmt, ##args)
#define apfs_crit(fs_info, fmt, args...) \
	apfs_printk(fs_info, KERN_CRIT fmt, ##args)
#define apfs_err(fs_info, fmt, args...) \
	apfs_printk(fs_info, KERN_ERR fmt, ##args)
#define apfs_warn(fs_info, fmt, args...) \
	apfs_printk(fs_info, KERN_WARNING fmt, ##args)
#define apfs_notice(fs_info, fmt, args...) \
	apfs_printk(fs_info, KERN_NOTICE fmt, ##args)
#define apfs_info(fs_info, fmt, args...) \
	apfs_printk(fs_info, KERN_INFO fmt, ##args)

/*
 * Wrappers that use printk_in_rcu
 */
#define apfs_emerg_in_rcu(fs_info, fmt, args...) \
	apfs_printk_in_rcu(fs_info, KERN_EMERG fmt, ##args)
#define apfs_alert_in_rcu(fs_info, fmt, args...) \
	apfs_printk_in_rcu(fs_info, KERN_ALERT fmt, ##args)
#define apfs_crit_in_rcu(fs_info, fmt, args...) \
	apfs_printk_in_rcu(fs_info, KERN_CRIT fmt, ##args)
#define apfs_err_in_rcu(fs_info, fmt, args...) \
	apfs_printk_in_rcu(fs_info, KERN_ERR fmt, ##args)
#define apfs_warn_in_rcu(fs_info, fmt, args...) \
	apfs_printk_in_rcu(fs_info, KERN_WARNING fmt, ##args)
#define apfs_notice_in_rcu(fs_info, fmt, args...) \
	apfs_printk_in_rcu(fs_info, KERN_NOTICE fmt, ##args)
#define apfs_info_in_rcu(fs_info, fmt, args...) \
	apfs_printk_in_rcu(fs_info, KERN_INFO fmt, ##args)

/*
 * Wrappers that use a ratelimited printk_in_rcu
 */
#define apfs_emerg_rl_in_rcu(fs_info, fmt, args...) \
	apfs_printk_rl_in_rcu(fs_info, KERN_EMERG fmt, ##args)
#define apfs_alert_rl_in_rcu(fs_info, fmt, args...) \
	apfs_printk_rl_in_rcu(fs_info, KERN_ALERT fmt, ##args)
#define apfs_crit_rl_in_rcu(fs_info, fmt, args...) \
	apfs_printk_rl_in_rcu(fs_info, KERN_CRIT fmt, ##args)
#define apfs_err_rl_in_rcu(fs_info, fmt, args...) \
	apfs_printk_rl_in_rcu(fs_info, KERN_ERR fmt, ##args)
#define apfs_warn_rl_in_rcu(fs_info, fmt, args...) \
	apfs_printk_rl_in_rcu(fs_info, KERN_WARNING fmt, ##args)
#define apfs_notice_rl_in_rcu(fs_info, fmt, args...) \
	apfs_printk_rl_in_rcu(fs_info, KERN_NOTICE fmt, ##args)
#define apfs_info_rl_in_rcu(fs_info, fmt, args...) \
	apfs_printk_rl_in_rcu(fs_info, KERN_INFO fmt, ##args)

/*
 * Wrappers that use a ratelimited printk
 */
#define apfs_emerg_rl(fs_info, fmt, args...) \
	apfs_printk_ratelimited(fs_info, KERN_EMERG fmt, ##args)
#define apfs_alert_rl(fs_info, fmt, args...) \
	apfs_printk_ratelimited(fs_info, KERN_ALERT fmt, ##args)
#define apfs_crit_rl(fs_info, fmt, args...) \
	apfs_printk_ratelimited(fs_info, KERN_CRIT fmt, ##args)
#define apfs_err_rl(fs_info, fmt, args...) \
	apfs_printk_ratelimited(fs_info, KERN_ERR fmt, ##args)
#define apfs_warn_rl(fs_info, fmt, args...) \
	apfs_printk_ratelimited(fs_info, KERN_WARNING fmt, ##args)
#define apfs_notice_rl(fs_info, fmt, args...) \
	apfs_printk_ratelimited(fs_info, KERN_NOTICE fmt, ##args)
#define apfs_info_rl(fs_info, fmt, args...) \
	apfs_printk_ratelimited(fs_info, KERN_INFO fmt, ##args)

#if defined(CONFIG_DYNAMIC_DEBUG)
#define apfs_debug(fs_info, fmt, args...)				\
	_dynamic_func_call_no_desc(fmt, apfs_printk,			\
				   fs_info, KERN_DEBUG fmt, ##args)
#define apfs_debug_in_rcu(fs_info, fmt, args...)			\
	_dynamic_func_call_no_desc(fmt, apfs_printk_in_rcu,		\
				   fs_info, KERN_DEBUG fmt, ##args)
#define apfs_debug_rl_in_rcu(fs_info, fmt, args...)			\
	_dynamic_func_call_no_desc(fmt, apfs_printk_rl_in_rcu,		\
				   fs_info, KERN_DEBUG fmt, ##args)
#define apfs_debug_rl(fs_info, fmt, args...)				\
	_dynamic_func_call_no_desc(fmt, apfs_printk_ratelimited,	\
				   fs_info, KERN_DEBUG fmt, ##args)
#elif defined(DEBUG)
#define apfs_debug(fs_info, fmt, args...) \
	apfs_printk(fs_info, KERN_DEBUG fmt, ##args)
#define apfs_debug_in_rcu(fs_info, fmt, args...) \
	apfs_printk_in_rcu(fs_info, KERN_DEBUG fmt, ##args)
#define apfs_debug_rl_in_rcu(fs_info, fmt, args...) \
	apfs_printk_rl_in_rcu(fs_info, KERN_DEBUG fmt, ##args)
#define apfs_debug_rl(fs_info, fmt, args...) \
	apfs_printk_ratelimited(fs_info, KERN_DEBUG fmt, ##args)
#else
#define apfs_debug(fs_info, fmt, args...) \
	apfs_no_printk(fs_info, KERN_DEBUG fmt, ##args)
#define apfs_debug_in_rcu(fs_info, fmt, args...) \
	apfs_no_printk_in_rcu(fs_info, KERN_DEBUG fmt, ##args)
#define apfs_debug_rl_in_rcu(fs_info, fmt, args...) \
	apfs_no_printk_in_rcu(fs_info, KERN_DEBUG fmt, ##args)
#define apfs_debug_rl(fs_info, fmt, args...) \
	apfs_no_printk(fs_info, KERN_DEBUG fmt, ##args)
#endif

#define apfs_printk_in_rcu(fs_info, fmt, args...)	\
do {							\
	rcu_read_lock();				\
	apfs_printk(fs_info, fmt, ##args);		\
	rcu_read_unlock();				\
} while (0)

#define apfs_no_printk_in_rcu(fs_info, fmt, args...)	\
do {							\
	rcu_read_lock();				\
	apfs_no_printk(fs_info, fmt, ##args);		\
	rcu_read_unlock();				\
} while (0)

#define apfs_printk_ratelimited(fs_info, fmt, args...)		\
do {								\
	static DEFINE_RATELIMIT_STATE(_rs,			\
		DEFAULT_RATELIMIT_INTERVAL,			\
		DEFAULT_RATELIMIT_BURST);       		\
	if (__ratelimit(&_rs))					\
		apfs_printk(fs_info, fmt, ##args);		\
} while (0)

#define apfs_printk_rl_in_rcu(fs_info, fmt, args...)		\
do {								\
	rcu_read_lock();					\
	apfs_printk_ratelimited(fs_info, fmt, ##args);		\
	rcu_read_unlock();					\
} while (0)

#ifdef CONFIG_APFS_ASSERT
__cold __noreturn
static inline void assertfail(const char *expr, const char *file, int line)
{
	pr_err("assertion failed: %s, in %s:%d\n", expr, file, line);
	BUG();
}

#define ASSERT(expr)						\
	(likely(expr) ? (void)0 : assertfail(#expr, __FILE__, __LINE__))

#else
static inline void assertfail(const char *expr, const char* file, int line) { }
#define ASSERT(expr)	(void)(expr)
#endif

#if BITS_PER_LONG == 32
#define APFS_32BIT_MAX_FILE_SIZE (((u64)ULONG_MAX + 1) << PAGE_SHIFT)
/*
 * The warning threshold is 5/8th of the MAX_LFS_FILESIZE that limits the logical
 * addresses of extents.
 *
 * For 4K page size it's about 10T, for 64K it's 160T.
 */
#define APFS_32BIT_EARLY_WARN_THRESHOLD (APFS_32BIT_MAX_FILE_SIZE * 5 / 8)
void apfs_warn_32bit_limit(struct apfs_fs_info *fs_info);
void apfs_err_32bit_limit(struct apfs_fs_info *fs_info);
#endif

/*
 * Get the correct offset inside the page of extent buffer.
 *
 * @eb:		target extent buffer
 * @start:	offset inside the extent buffer
 *
 * Will handle both sectorsize == PAGE_SIZE and sectorsize < PAGE_SIZE cases.
 */
static inline size_t get_eb_offset_in_page(const struct extent_buffer *eb,
					   unsigned long offset)
{
	/*
	 * For sectorsize == PAGE_SIZE case, eb->start will always be aligned
	 * to PAGE_SIZE, thus adding it won't cause any difference.
	 *
	 * For sectorsize < PAGE_SIZE, we must only read the data that belongs
	 * to the eb, thus we have to take the eb->start into consideration.
	 */
	return offset_in_page(offset + eb->start);
}

static inline unsigned long get_eb_page_index(unsigned long offset)
{
	/*
	 * For sectorsize == PAGE_SIZE case, plain >> PAGE_SHIFT is enough.
	 *
	 * For sectorsize < PAGE_SIZE case, we only support 64K PAGE_SIZE,
	 * and have ensured that all tree blocks are contained in one page,
	 * thus we always get index == 0.
	 */
	return offset >> PAGE_SHIFT;
}

/*
 * Use that for functions that are conditionally exported for sanity tests but
 * otherwise static
 */
#ifndef CONFIG_APFS_FS_RUN_SANITY_TESTS
#define EXPORT_FOR_TESTS static
#else
#define EXPORT_FOR_TESTS
#endif

__cold
static inline void apfs_print_v0_err(struct apfs_fs_info *fs_info)
{
	apfs_err(fs_info,
"Unsupported V0 extent filesystem detected. Aborting. Please re-create your filesystem with a newer kernel");
}

__printf(5, 6)
__cold
void __apfs_handle_fs_error(struct apfs_fs_info *fs_info, const char *function,
		     unsigned int line, int errno, const char *fmt, ...);

const char * __attribute_const__ apfs_decode_error(int errno);

__cold
void __apfs_abort_transaction(struct apfs_trans_handle *trans,
			       const char *function,
			       unsigned int line, int errno);

/*
 * Call apfs_abort_transaction as early as possible when an error condition is
 * detected, that way the exact line number is reported.
 */
#define apfs_abort_transaction(trans, errno)		\
do {								\
	/* Report first abort since mount */			\
	if (!test_and_set_bit(APFS_FS_STATE_TRANS_ABORTED,	\
			&((trans)->fs_info->fs_state))) {	\
		if ((errno) != -EIO && (errno) != -EROFS) {		\
			WARN(1, KERN_DEBUG				\
			"APFS: Transaction aborted (error %d)\n",	\
			(errno));					\
		} else {						\
			apfs_debug((trans)->fs_info,			\
				    "Transaction aborted (error %d)", \
				  (errno));			\
		}						\
	}							\
	__apfs_abort_transaction((trans), __func__,		\
				  __LINE__, (errno));		\
} while (0)

#define apfs_handle_fs_error(fs_info, errno, fmt, args...)		\
do {								\
	__apfs_handle_fs_error((fs_info), __func__, __LINE__,	\
			  (errno), fmt, ##args);		\
} while (0)

__printf(5, 6)
__cold
void __apfs_panic(struct apfs_fs_info *fs_info, const char *function,
		   unsigned int line, int errno, const char *fmt, ...);
/*
 * If APFS_MOUNT_PANIC_ON_FATAL_ERROR is in mount_opt, __apfs_panic
 * will panic().  Otherwise we BUG() here.
 */
#define apfs_panic(fs_info, errno, fmt, args...)			\
do {									\
	__apfs_panic(fs_info, __func__, __LINE__, errno, fmt, ##args);	\
	BUG();								\
} while (0)

/* fs_info */
struct reloc_control;
struct apfs_device;
struct apfs_fs_devices;
struct apfs_balance_control;
struct apfs_delayed_root;

int apfs_read_generic(struct block_device *bdev, u64 bytenr, unsigned long len,
		      void *res);

/*
 * A range of physical addresses.
 */
struct apfs_prange {
	__le64 start;
	__le64 block_count;
};

/* Header of all objects */
struct apfs_obj_header {
	u8 csum[APFS_CSUM_SIZE];
	__le64 oid;
	__le64 xid;
	/*
	 * The low 16 bits value means object type.
	 * The high 16 bits are mixed flags.
	 */
	__le32 type;
	__le32 subtype;
};

APFS_SETGET_FUNCS(obj_oid, struct apfs_obj_header, oid, 64);
APFS_SETGET_FUNCS(obj_xid, struct apfs_obj_header, xid, 64);
APFS_SETGET_FUNCS(obj_type, struct apfs_obj_header, type, 32);
APFS_SETGET_FUNCS(obj_subtype, struct apfs_obj_header, subtype, 32);

APFS_SETGET_STACK_FUNCS(stack_obj_oid, struct apfs_obj_header, oid, 64);
APFS_SETGET_STACK_FUNCS(stack_obj_xid, struct apfs_obj_header, xid, 64);
APFS_SETGET_STACK_FUNCS(stack_obj_type, struct apfs_obj_header, type, 32);
APFS_SETGET_STACK_FUNCS(stack_obj_subtype, struct apfs_obj_header, subtype, 32);

/* Object identifier constants */
enum {
	APFS_OID_INVALID = 0,
	APFS_OID_SUPERBLOCK = 1, //container superblock
	APFS_OID_RESERVED = 1024, //oids < 1024 are reserved.
};

#define APFS_OBJ_TYPE_MASK			0x0000ffff
#define APFS_OBJ_TYPE_FLAGS_MASK		0xffff0000
#define APFS_OBJ_STG_TYPE_MASK			0xc0000000
#define APFS_OBJ_TYPE_FLAGS_DEFINED_MASK	0xf8000000

/* Object types */
enum {
	APFS_OBJ_TYPE_INVALID = 0,
	APFS_OBJ_TYPE_SUPERBLOCK = 1,
	APFS_OBJ_TYPE_BTREE = 2,
	APFS_OBJ_TYPE_BTREE_NODE = 3,
	APFS_OBJ_TYPE_SPACEMAN = 5,
	APFS_OBJ_TYPE_SPACEMAN_CAB,
	APFS_OBJ_TYPE_SPACEMAN_CIB,
	APFS_OBJ_TYPE_SPACEMAN_BITMAP,
	APFS_OBJ_TYPE_SPACEMAN_FREE_QUEUE,
	APFS_OBJ_TYPE_EXTENT_LIST_TREE,
	APFS_OBJ_TYPE_OMAP,
	APFS_OBJ_TYPE_CHECKPOINT_MAP,
	APFS_OBJ_TYPE_FS,
	APFS_OBJ_TYPE_FSTREE,
	APFS_OBJ_TYPE_REFTREE,
	APFS_OBJ_TYPE_SNAPTREE,
	APFS_OBJ_TYPE_NX_REAPER,
	APFS_OBJ_TYPE_NX_REAP_LIST,
	APFS_OBJ_TYPE_OMAP_SNAPSHOT,
	APFS_OBJ_TYPE_EFI_JUMPSTART,
	APFS_OBJ_TYPE_FUSION_MIDDLE_TREE,
	APFS_OBJ_TYPE_NX_FUSION_WBC,
	APFS_OBJ_TYPE_NX_FUSION_WBC_LIST,
	APFS_OBJ_TYPE_ER_STATE,
	APFS_OBJ_TYPE_GBITMAP,
	APFS_OBJ_TYPE_GBITMAP_TREE,
	APFS_OBJ_TYPE_GBITMAP_BLOCK,
	APFS_OBJ_TYPE_ER_RECOVERY_BLOCK,
	APFS_OBJ_TYPE_SNAP_META_EXT,
	APFS_OBJ_TYPE_INTEGRITY_META,
	APFS_OBJ_TYPE_FEXT_TREE,
	APFS_OBJ_TYPE_RESERVED_20,
	APFS_OBJ_TYPE_TEST = 0xff,
};

enum apfs_storage {
	// virtual object
	APFS_STG_VIRTUAL = 0x00000000,
	// ephemeral object
	APFS_STG_EPHEMERAL = 0x80000000,
	// physical object
	APFS_STG_PHYSICAL = 0x40000000,
	// object without apfs_obj_header
	APFS_STG_NOHEADER = 0x20000000,
	// encrypted object.
	APFS_STG_ENCRYPTED = 0x10000000,
	// ephemeral object isn't persisted across unmounting
	// SHOULD NEVER HAPPEN
	APFS_STG_NONPERSISTENT = 0x08000000,
};

#define APFS_OBJ_TYPE_CONTAINER_KEYBAG	"keys"
#define APFS_OBJ_TYPE_VOLUME_KEYBAG	"recs"
#define APFS_OBJ_TYPE_MEDIA_KEYBAG	"mkey"


#define APFS_NX_MAGIC 1112758350 // (u32)'BSXN'
#define APFS_MAX_FILE_SYSTEMS 100

/* length of apfs_nx_superblock::ephemeral_info */
#define APFS_EPH_INFO_COUNT 4
/*
 * Minimum size in blocks used While picking a new checkpoint data area
 */
#define APFS_EPH_MIN_BLOCK_COUNT 8

#define APFS_MAX_FILE_SYSTEM_EPH_STRUCTS 4

#define APFS_TX_MIN_CHECKPOINT_COUNT 4

#define APFS_EPH_INFO_VERSION_1 1

/* Container Flags */
enum apfs_container_flags {
	APFS_RESERVED_1 = 0x00000001LL,
	APFS_RESERVED_2 = 0x00000002LL,
	/* The container uses software cryptography. */
	APFS_CRYPTO_SW = 0x00000004LL,
};

enum apfs_container_features {
	/*
	 * The volumes in this container support defragmentation.
	 */
	APFS_FEATURE_DEFRAG = 0x0000000000000001ULL,
	/* This container is using low-capacity Fusion Drive mode. */
	/*
	 * The volumes in this container support defragmentation.
	 * Low-capacity Fusion Drive mode is enabled when the solid-state
	 * drive has a smaller capacity and so the cache must be smaller.
	 */
	APFS_FEATURE_LCFD = 0x0000000000000002ULL,
	APFS_SUPPORTED_FEATURES_MASK = (APFS_FEATURE_DEFRAG | APFS_FEATURE_LCFD)
};

#define APFS_SUPPORTED_ROCOMPAT_MASK (0x0ULL)

enum apfs_container_incompat_ro_features {
	/*
	 * macOS 10.12 uses version 1
	 */
	APFS_INCOMPAT_VERSION1 = 0x1ULL,
	/*
	 * used by macOS 10.13 and iOS 10.3.
	 */
	APFS_INCOMPAT_VERSION2 = 0x2ULL,
	/*
	 * The container supports Fusion Drives. Since Apple has not released
	 * the detailed disk layout, not supported yet.
	 */
	APFS_INCOMPAT_FUSION = 0x100ULL,
	/*
	 * A bit mask of all the backward-incompatible features.
	 */
	APFS_SUPPORTED_INCOMPAT_MASK =
	(APFS_INCOMPAT_VERSION2 | APFS_INCOMPAT_FUSION),
};

enum apfs_counter_id {
	/*
	 * Times of csuming objects.
	 */
	APFS_COUNT_OBJ_CSUM_SET = 0,
	/*
	 * Times of csuming objects on error.
	 */
	APFS_COUNT_OBJ_CSUM_FAIL = 1,
	/*
	 * Maximum counters.
	 */
	APFS_NUM_COUNTERS = 32
};

struct apfs_nx_superblock {
	struct apfs_obj_header o;
	__le32 magic; // must be APFS_MAGIC
	/*
	 * mutipile/single dev blocksize
	 */
	__le32 block_size;
	/*
	 * block counts avaiable
	 */
	__le64 block_count;
	/*
	 * For write, not used as for now
	 */
	__le64 features;

	__le64 readonly_compatible_features;
	__le64 incompatible_features;
	uuid_t uuid;
	__le64 next_oid;
	__le64 next_xid;

	/*
	 * Number of blocks occupied by checkpoint descriptor area.
	 * The highest bit is used as a flag.
	 */
	__le32 xp_desc_blocks;

	/*
	 * Number of blocks occupied by checkpoint data area.
	 * The highest bit is used as a flag.
	 */
	__le32 xp_data_blocks;

	/*
	 * The start address of the checkpoint descriptor area,
	 * or the physical object identifier of a tree that contains the
	 * address information.
	 */
	__le64 xp_desc_base;
	/*
	 * The start address of the checkpoint data area,
	 * or the physical object identifier of a tree that contains the
	 * address information.
	 */
	__le64 xp_data_base;
	/*
	 * next checkpoint desc index
	 */
	__le32 xp_desc_next;
	/*
	 * next checkpoint data index
	 */
	__le32 xp_data_next;
	/*
	 * Start index in desc area. Ignore it if superblock is not in
	 * desc area e.g. start at block 0.
	 */
	__le32 xp_desc_index;
	/*
	 * blocks number of desc area.
	 */
	__le32 xp_desc_len;
	/*
	 * Start index in data area. Ignore it if superblock is not in
	 * desc area e.g. start at block 0.
	 */
	__le32 xp_data_index;
	/*
	 * blocks number of data area.
	 */
	__le32 xp_data_len;
	/*
	 * space manager ephemeral object identifier.
	 */
	__le64 spaceman_oid;
	/*
	 * physical address of object map.
	 */
	__le64 omap_oid;
	/*
	 * ephemeral address of reaper.
	 */
	__le64 reaper_oid;
	/*
	 * for testing
	 */
	__le32 test_type;
	/*
	 * Maximum numbers of the container can contain.
	 * DIV_ROUND_UP(container_size, 512MiB)
	 *
	 * Maximum: APFS_MAX_FILE_SYSTEMS (100).
	 */
	__le32 max_file_systems;

	/*
	 * Virtual address for apfs volumes
	 */
	__le64 fs_oid[APFS_MAX_FILE_SYSTEMS];
	/*
	 * counters for debug intention
	 */
	__le64 counters[APFS_NUM_COUNTERS];

	/*
	 * The area should never be allocated. For shrink purpose.
	 */
	__le64 pinned_block_start;
	__le64 pinned_block_count;

	/*
	 * Physical address points a tree containing blocks should be unpinned.
	 */
	__le64 evict_mapping_tree_oid;
	__le64 flags;

	uuid_t fusion_uuid;

	/*
	 * range of keybag.
	 */
	__le64 keylocker_start;
	__le64 keylocker_count;

	__le64 ephemeral_info[APFS_EPH_INFO_COUNT];
	/*
	 * For test only.
	 */
	__le64 test_oid;

	__le64 fusion_mt_oid;
	__le64 fusion_wbc_oid;

	struct apfs_prange fusion_wbc;
	/*
	 * Reserved.
	 * Do NOT touch this field.
	 */
	__le64 newest_mounted_version;
	/*
	 * Wrapped media key.
	 */
	struct apfs_prange mkb_locker;
};

APFS_SETGET_OBJ_FUNCS(nx_super, struct apfs_nx_superblock);

/* struct apfs_nx_superblock */
APFS_SETGET_STACK_FUNCS(nx_super_magic, struct apfs_nx_superblock, magic, 32);
APFS_SETGET_STACK_FUNCS(nx_super_block_size, struct apfs_nx_superblock,
			block_size, 32);
APFS_SETGET_STACK_FUNCS(nx_super_block_count, struct apfs_nx_superblock,
			block_count, 64);
APFS_SETGET_STACK_FUNCS(nx_super_features, struct apfs_nx_superblock, features,
			64);
APFS_SETGET_STACK_FUNCS(nx_super_ro_compat_features, struct apfs_nx_superblock,
			readonly_compatible_features, 64);
APFS_SETGET_STACK_FUNCS(nx_super_incompat_features, struct apfs_nx_superblock,
			incompatible_features, 64);
APFS_SETGET_STACK_FUNCS(nx_super_next_oid, struct apfs_nx_superblock, next_oid,
			64);
APFS_SETGET_STACK_FUNCS(nx_super_next_xid, struct apfs_nx_superblock, next_xid,
			64);
APFS_SETGET_STACK_FUNCS(nx_super_xp_desc_blocks, struct apfs_nx_superblock,
			xp_desc_blocks, 32);
APFS_SETGET_STACK_FUNCS(nx_super_xp_data_blocks, struct apfs_nx_superblock,
			xp_data_blocks, 32);
APFS_SETGET_STACK_FUNCS(nx_super_xp_desc_base, struct apfs_nx_superblock,
			xp_desc_base, 64);
APFS_SETGET_STACK_FUNCS(nx_super_xp_desc_index, struct apfs_nx_superblock,
			xp_desc_index, 32);
APFS_SETGET_STACK_FUNCS(nx_super_xp_desc_len, struct apfs_nx_superblock,
			xp_desc_len, 32);
APFS_SETGET_STACK_FUNCS(nx_super_xp_data_base, struct apfs_nx_superblock,
			xp_data_base, 64);
APFS_SETGET_STACK_FUNCS(nx_super_omap_oid, struct apfs_nx_superblock,
			omap_oid, 64);
APFS_SETGET_STACK_FUNCS(nx_super_max_index, struct apfs_nx_superblock,
			max_file_systems, 64);

static inline u64
apfs_fs_oid(struct apfs_nx_superblock *sb, int nr)
{
	ASSERT(0 <= nr && nr < APFS_MAX_FILE_SYSTEMS);
	return __le64_to_cpu(sb->fs_oid[nr]);
}

/*
 * It depends on the highest bit of the xp_desc/data base.
 * If 0, it means areas are contiuguous.
 * If 1, the values points to a tree consists of address ranges.
 */
static inline u64
__apfs_xp_base(const struct apfs_nx_superblock *super, bool desc)
{
	return 0;
}

static inline u64
apfs_xp_desc_base(const struct apfs_nx_superblock *super, bool desc)
{
	return __apfs_xp_base(super, super->xp_desc_base);
}

static inline u64
apfs_xp_data_base(const struct apfs_nx_superblock *super, bool desc)
{
	return __apfs_xp_base(super, super->xp_data_base);
}

struct apfs_checkpoint_mapping {
	/*
	 * The low 16 bits means object type,
	 * the high 16 bits are flags.
	 */
	__le32 type;

	__le32 subtype;

	__le32 size;
	__le32 pad;
	/*
	 * virtual address of one volume
	 */
	__le64 fs_oid;
	/*
	 * ephemeral address
	 */
	__le64 oid;
	/*
	 * address in the checkpoint data
	 */
	__le64 paddr;
};


#define APFS_CHECKPOINT_MAP_LAST 0x00000001
/*
 * flags in last block is with the APFS_CHECKPOINT_MAP_LAST.
 */
struct apfs_checkpoint_map_phys {
	struct apfs_obj_header o;
	__le32 flags;
	__le32 count;
	struct apfs_checkpoint_mapping map[];
};

/* struct apfs_nx_superblock */
APFS_SETGET_OBJ_FUNCS(checkpoint_map, struct apfs_checkpoint_map_phys);

/* struct apfs_nx_superblock */
APFS_SETGET_STACK_FUNCS(checkpoint_map_count, struct apfs_checkpoint_map_phys,
			count, 32);
/* struct apfs_nx_superblock */
APFS_SETGET_STACK_FUNCS(checkpoint_map_flags, struct apfs_checkpoint_map_phys,
			flags, 32);

/*
 * see apfs_nx_superblock::evict_mapping_tree_oid
 */
struct apfs_evict_mapping_val {
	u64 dst_paddr;
	__le64 len;
} __attribute__((__packed__));

#define APFS_MAX_OMAP_SNAP_COUNT UINT32_MAX

/* Object map reaper constants */
/* map tree is in progress of deleting. */
#define APFS_OMAP_REAP_PHASE_MAP_TREE 1
/* snap tree is in progress of deleting. */
#define APFS_OMAP_REAP_PHASE_SNAPSHOT_TREE 2

/* object map Flags */

/* The object map doesn't support snapshots. */
#define APFS_OMAP_MANUALLY_MANAGED 0x00000001
#define APFS_OMAP_ENCRYPTING 0x00000002
#define APFS_OMAP_DECRYPTING 0x00000004
/*
 * Even newkey is assigned, the older key is used for encrypted.
 */
#define APFS_OMAP_KEYROLLING 0x00000008
#define APFS_OMAP_CRYPTO_GENERATION 0x00000010
#define APFS_OMAP_VALID_FLAGS 0x0000001f

struct apfs_omap_phys {
	struct apfs_obj_header o;
	__le32 flags;
	__le32 snap_count;
	__le32 tree_type;
	__le32 snap_tree_type;
	__le64 tree_oid;
	__le64 snap_tree_oid;
	__le64 latest_snap;
	/* minimum xid for revert. */
	__le64 revert_min;
	/* maximum xid for revert. */
	__le64 revert_max;
};

/* struct apfs_omap_phys */
APFS_SETGET_FUNCS(omap_phys_flags, struct apfs_omap_phys, flags, 32);
APFS_SETGET_FUNCS(omap_phys_snap_count, struct apfs_omap_phys,
			snap_count, 32);
APFS_SETGET_FUNCS(omap_phys_tree_type, struct apfs_omap_phys,
			tree_type, 32);
APFS_SETGET_FUNCS(omap_phys_snap_tree_type, struct apfs_omap_phys,
			snap_tree_type, 32);
APFS_SETGET_STACK_FUNCS(omap_phys_tree_oid, struct apfs_omap_phys, tree_oid, 64);
APFS_SETGET_FUNCS(omap_phys_snap_tree_oid, struct apfs_omap_phys,
			snap_tree_oid, 64);
APFS_SETGET_FUNCS(omap_phys_latest_snap, struct apfs_omap_phys,
			latest_snap, 64);
APFS_SETGET_FUNCS(omap_phys_revert_min, struct apfs_omap_phys,
			revert_min, 64);
APFS_SETGET_FUNCS(omap_phys_revert_max, struct apfs_omap_phys,
			revert_max, 64);

struct apfs_omap_key {
	__le64 oid;
	__le64 xid;
};

/* Object map value Flags */
#define APFS_OMAP_VAL_DELETED 0x00000001
/* not used */
#define APFS_OMAP_VAL_SAVED 0x00000002
#define APFS_OMAP_VAL_ENCRYPTED 0x00000004
#define APFS_OMAP_VAL_NOHEADER 0x00000008
#define APFS_OMAP_VAL_CRYPTO_GENERATION 0x00000010

struct apfs_omap_item {
	__le32 flags;
	__le32 size;
	u64 paddr;
};

APFS_SETGET_STACK_FUNCS(omap_flags, struct apfs_omap_item, flags, 32);
APFS_SETGET_STACK_FUNCS(omap_size, struct apfs_omap_item, size, 32);
APFS_SETGET_STACK_FUNCS(omap_paddr, struct apfs_omap_item, paddr, 64);

/* Snapshot flags */
#define APFS_OMAP_SNAPSHOT_DELETED 0x00000001
/* Deleted in a revert operation */
#define APFS_OMAP_SNAPSHOT_REVERTED 0x00000002

struct apfs_omap_snapshot {
	__le32 flags;
	/* Reserved */
	__le32 pad;
	/* Reserved */
	__le64 oid;
};

/* crytption things */
#define APFS_CP_MAX_WRAPPEDKEYSIZE 128
struct apfs_wrapped_crypto_state {
	/*
	 * is 5 now.
	 */
	__le16 major_version;
	/* 0 now */
	__le16 minor_version;
	/* state flags, not been used */
	__le32 flags;
	__le32 persistent_class;
	/*
	 * OS version
	 */
	__le32 key_os_version;
	/*
	 * key version
	 */
	__le16 key_revision;
	__le16 key_len;
	u8 persistent_key[0];
} __attribute__((aligned(2), __packed__));

struct apfs_wrapped_meta_crypto_state {
	__le16 major_version;
	__le16 minor_version;
	__le32 cpflags;
	__le32 persistent_class;
	__le32 key_os_version;
	__le16 key_revision;
	__le16 unused;
} __attribute__((aligned(2), __packed__));

/* volume superblock flags*/
#define APFS_FS_UNENCRYPTED 0x00000001LL
#define APFS_FS_RESERVED_2 0x00000002LL
#define APFS_FS_RESERVED_4 0x00000004LL
#define APFS_FS_ONEKEY 0x00000008LL // using one VEK
#define APFS_FS_SPILLEDOVER 0x00000010LL // enospc
/* enospc and should do clean. */
#define APFS_FS_RUN_SPILLOVER_CLEANER 0x00000020LL
/*
 * Always check extent ref tree while do writes
 */
#define APFS_FS_ALWAYS_CHECK_EXTENTREF 0x00000040LL
#define APFS_FS_RESERVED_80 0x00000080LL
#define APFS_FS_RESERVED_100 0x00000100LL

#define APFS_FS_FLAGS_VALID_MASK (APFS_FS_UNENCRYPTED			\
				  | APFS_FS_RESERVED_2			\
				  | APFS_FS_RESERVED_4			\
				  | APFS_FS_ONEKEY			\
				  | APFS_FS_SPILLEDOVER			\
				  | APFS_FS_RUN_SPILLOVER_CLEANER	\
				  | APFS_FS_ALWAYS_CHECK_EXTENTREF	\
				  | APFS_FS_RESERVED_80			\
				  | APFS_FS_RESERVED_100)

#define APFS_FS_CRYPTOFLAGS (APFS_FS_UNENCRYPTED	\
			     | APFS_FS_ONEKEY)

/*
 * values of apfs_volume_superblock::role.
 * For historical reasons, the underlying values of these constants have two
 * variations. The roles whose constants use only the six least
 * significant bits and the APFS_VOL_ROLE_DATA and APFS_VOL_ROLE_BASEBAND
 * roles are supported by all versions of macOS and iOS.
 * The remaining roles that are stored using the ten most significant bits are
 * supported only by devices running macOS 10.15, iOS 13, and later.
 */
#define APFS_VOL_ROLE_NONE 0x0000
/* ROOT fs mounted at /. READONLY */
#define APFS_VOL_ROLE_SYSTEM 0x0001
/* /Users */
#define APFS_VOL_ROLE_USER 0x0002
/* Recovery volume */
#define APFS_VOL_ROLE_RECOVERY 0x0004
/* Virtual memory. */
#define APFS_VOL_ROLE_VM 0x0008
/* Preboot before mounting an encrypted volume. */
#define APFS_VOL_ROLE_PREBOOT 0x0010
/* OS installer. */
#define APFS_VOL_ROLE_INSTALLER 0x0020

#define APFS_VOLUME_ENUM_SHIFT 6

/*
 * Mutable volume
 */
#define APFS_VOL_ROLE_DATA (1 << APFS_VOLUME_ENUM_SHIFT)

/*
 * IOS only.
 */
#define APFS_VOL_ROLE_BASEBAND (2 << APFS_VOLUME_ENUM_SHIFT)
/*
 * IOS only.
 */
#define APFS_VOL_ROLE_UPDATE (3 << APFS_VOLUME_ENUM_SHIFT)
/*
 * IOS only.
 */
#define APFS_VOL_ROLE_XART (4 << APFS_VOLUME_ENUM_SHIFT)
/*
 * IOS only.
 */
#define APFS_VOL_ROLE_HARDWARE (5 << APFS_VOLUME_ENUM_SHIFT)
/*
 * Used by Time Machine.
 */
#define APFS_VOL_ROLE_BACKUP (6 << APFS_VOLUME_ENUM_SHIFT)
#define APFS_VOL_ROLE_RESERVED_7 (7 << APFS_VOLUME_ENUM_SHIFT)
#define APFS_VOL_ROLE_RESERVED_8 (8 << APFS_VOLUME_ENUM_SHIFT)

#define APFS_VOL_ROLE_ENTERPRISE (9 << APFS_VOLUME_ENUM_SHIFT)
#define APFS_VOL_ROLE_RESERVED_10 (10 << APFS_VOLUME_ENUM_SHIFT)
#define APFS_VOL_ROLE_PRELOGIN (11 << APFS_VOLUME_ENUM_SHIFT)

struct wrapped_crypto_state;

#define APFS_MODIFIED_NAMELEN 32
struct apfs_modified_by {
	u8 id[APFS_MODIFIED_NAMELEN];
	__le64 timestamp;
	__le64 last_xid;
};

#define APFS_VOLUME_MAGIC APFS_SUPER_MAGIC /* 'BSPA' */
#define APFS_MAX_HIST 8
#define APFS_VOLNAME_LEN 256

/* Volume feature flags */

/* Not used. */
#define APFS_FEATURE_DEFRAG_PRERELEASE 0x00000001LL
/*  hardlink map records*/
#define APFS_FEATURE_HARDLINK_MAP_RECORDS 0x00000002LL
/* can do defragmentation. */
#define APFS_FEATURE_DEFRAG 0x00000004LL
/* updates atime while the file is read. */
#define APFS_FEATURE_STRICT_ATIME 0x00000008LL
/*
 * Volume group relateed
 */
#define APFS_FEATURE_VOLUME_GROUP_SYSTEM_INO_SPACE 0x00000010LL

#define APFS_SUPPORTED_FEATURES_MASK (APFS_FEATURE_DEFRAG		\
				      | APFS_FEATURE_DEFRAG_PRERELEASE	\
				      | APFS_FEATURE_HARDLINK_MAP_RECORDS \
				      | APFS_FEATURE_STRICTATIME	\
				      | APFS_FEATURE_VOLGRP_SYSTEM_INO_SPACE)

/* Readonly volume feature flags, no defined */
#define APFS_SUPPORTED_ROCOMPAT_MASK (0x0ULL)

/* Incompatible volume feature flags */
#define APFS_INCOMPAT_CASE_INSENSITIVE 0x00000001LL
/* has at least one dataless snapshot of the volume*/
#define APFS_INCOMPAT_DATALESS_SNAPS 0x00000002LL
/* encryption key changed */
#define APFS_INCOMPAT_ENC_ROLLED 0x00000004LL
/* normalization insensitive. */
#define APFS_INCOMPAT_NORMALIZATION_INSENSITIVE 0x00000008LL
/*
 * aborted in restore progress
 */
#define APFS_INCOMPAT_INCOMPLETE_RESTORE 0x00000010LL
/* can't be modified.*/
#define APFS_INCOMPAT_SEALED_VOLUME 0x00000020LL
#define APFS_INCOMPAT_RESERVED_40 0x00000040LL
#define APFS_SUPPORTED_INCOMPAT_MASK (APFS_INCOMPAT_CASE_INSENSITIVE	\
				      | APFS_INCOMPAT_DATALESS_SNAPS	\
				      | APFS_INCOMPAT_ENC_ROLLED	\
				      | APFS_INCOMPAT_NORMALIZATION_INSENSITIVE \
				      | APFS_INCOMPAT_INCOMPLETE_RESTORE \
				      | APFS_INCOMPAT_SEALED_VOLUME	\
				      | APFS_INCOMPAT_RESERVED_40)


/* Volume superblock*/
struct apfs_vol_superblock {
	struct apfs_obj_header o;
	__le32 magic; /* Must be 'BSPA' */
	/* index of arrary in container superblock */
	__le32 fs_index;
	__le64 features;
	__le64 readonly_compat_features;
	__le64 incompat_features;
	/* nanoseconds, since January 1, 1970 at 0:00 UTC */
	__le64 unmount_time;
	/* blocks reserved to allocate. */
	__le64 reserved;
	/* quota in blocks */
	__le64 quota;
	/* allocated blocks */
	__le64 allocated;
	struct apfs_wrapped_meta_crypto_state meta_crypto;
	/*
	 * APFS_OBJ_VIRTUAL | APFS_OBJ_TYPE_BTREE,
	 * subtypeOBJECT_TYPE_FSTREE.
	 */
	__le32 root_tree_type;
	/*
	 * APFS_OBJ_PHYSICAL | APFS_OBJ_TYPE_BLOCKREF,
	 * subtype of OBJECT_BLOCK_REF.
	 */
	__le32 extref_tree_type;
	/*
	 * APFS_OBJ_PHYSICAL | APFS_OBJ_TYPE_BLOCKREF,
	 * subtype OBJECT_BLOCK_REF.
	 */
	__le32 snap_meta_tree_type;
	/* physical address of an object map. */
	__le64 omap_oid;
	/* virtual address of the root tree. */
	__le64 root_tree_oid;
	/*
	 * Each snapshot has its own extent ref tree.
	 */
	__le64 extref_tree_oid;
	/* virtual address of the snapshot metadata tree. */
	__le64 snap_meta_tree_oid;
	/*
	 * if nonzero, revert to the snapshot and delete all snapshots after
	 * the trans.
	 */
	__le64 revert_to_xid;
	/* physical address of a volume_superblock to revert to. */
	__le64 revert_to_sblock_oid;
	__le64 next_obj_id;
	/* number of regular files. */
	__le64 num_files;
	/* number of directories. */
	__le64 num_dirs;
	/* number of symbolic links. */
	__le64 num_symlinks;
	/* The number of other files. */
	__le64 num_other_objs;
	__le64 num_snapshots;
	/*
	 * Increases, never decreases.
	 */
	__le64 total_blocks_allocated;
	__le64 total_blocks_freed;
	uuid_t uuid;
	/* in nanoseconds */
	__le64 last_mod_time;
	__le64 fs_flags;
	/*
	 * Time volume is created.
	 */
	struct apfs_modified_by formatted_by;
	struct apfs_modified_by modified_by[APFS_MAX_HIST];
	u8 volname[APFS_VOLNAME_LEN];
	__le32 next_doc_id;
	__le16 role;
	__le16 __reserved;
	/*
	 * xid of the snapshot or zero.
	 */
	__le64 root_to_xid;

	/* nonzero if the volume is in progress of encrytion */
	__le64 crypt_state_oid;
	/* see APFS_INODE_WAS_EVER_CLONED. */
	__le64 cloneinfo_id_epoch;
	/*
	 * last trans id, same as apfs_modified_by field.
	 */
	__le64 cloneinfo_xid;
	/*
	 * virtual address of extended snapshot metadata object.
	 */
	__le64 snap_meta_ext_oid;

	uuid_t volume_group_id;
	/*
	 * nonzero if the volume is sealed.
	 */
	__le64 integrity_meta_oid;
	/*
	 * virtual address of file extent tree.
	 * nonzero for sealed volumes.
	 */
	__le64 fext_tree_oid;
	__le32 fext_tree_type;
	__le32 reserved_type;
	__le64 reserved_oid;
};

APFS_SETGET_STACK_FUNCS(volume_super_magic, struct apfs_vol_superblock, magic, 32);
APFS_SETGET_STACK_FUNCS(volume_super_index, struct apfs_vol_superblock, fs_index, 32);
APFS_SETGET_STACK_FUNCS(volume_super_features, struct apfs_vol_superblock, features, 64);
APFS_SETGET_STACK_FUNCS(volume_super_ro_compat_features, struct apfs_vol_superblock,
			readonly_compat_features, 64);
APFS_SETGET_STACK_FUNCS(volume_super_incompat_features, struct apfs_vol_superblock,
			incompat_features, 64);
APFS_SETGET_STACK_FUNCS(volume_super_unmount_time, struct apfs_vol_superblock,
			unmount_time, 64);
APFS_SETGET_STACK_FUNCS(volume_super_omap_oid, struct apfs_vol_superblock,
			omap_oid, 64);
APFS_SETGET_STACK_FUNCS(volume_super_fext_tree, struct apfs_vol_superblock,
			fext_tree_oid, 64);
APFS_SETGET_STACK_FUNCS(volume_super_extref_tree, struct apfs_vol_superblock,
			extref_tree_oid, 64);
APFS_SETGET_STACK_FUNCS(volume_super_root_tree, struct apfs_vol_superblock,
			root_tree_oid, 64);
APFS_SETGET_STACK_FUNCS(volume_super_snap_tree, struct apfs_vol_superblock,
			snap_meta_tree_oid, 64);
APFS_SETGET_STACK_FUNCS(volume_super_num_snaps, struct apfs_vol_superblock,
			num_snapshots, 64);
APFS_SETGET_STACK_FUNCS(volume_super_num_files, struct apfs_vol_superblock,
			num_files, 64);
APFS_SETGET_STACK_FUNCS(volume_super_num_dirs, struct apfs_vol_superblock,
			num_dirs, 64);
APFS_SETGET_STACK_FUNCS(volume_super_num_symlinks, struct apfs_vol_superblock,
			num_symlinks, 64);
APFS_SETGET_STACK_FUNCS(volume_super_total_blocks_freed, struct apfs_vol_superblock,
			total_blocks_freed, 64);
APFS_SETGET_STACK_FUNCS(volume_super_total_blocks_allocated, struct apfs_vol_superblock,
			total_blocks_allocated, 64);
APFS_SETGET_OBJ_FUNCS(volume_super, struct apfs_vol_superblock);

/* File-System Constants */
enum apfs_key_types {
	APFS_TYPE_ANY = 0,
	APFS_TYPE_OMAP = 0,
	APFS_TYPE_SNAP_METADATA = 1,
	APFS_TYPE_EXTENT = 2,
	APFS_TYPE_INODE = 3,
	APFS_TYPE_XATTR = 4,
	APFS_TYPE_SIBLING_LINK = 5,
	APFS_TYPE_DSTREAM_ID = 6,
	APFS_TYPE_CRYPTO_STATE = 7,
	APFS_TYPE_FILE_EXTENT = 8,
	APFS_TYPE_DIR_REC = 9,
	APFS_TYPE_DIR_STATS = 10,
	APFS_TYPE_SNAP_NAME = 11,
	APFS_TYPE_SIBLING_MAP = 12,
	APFS_TYPE_FILE_INFO = 13,

	APFS_TYPE_MAX_VALID = 13,
	APFS_TYPE_MAX = 13,
	APFS_TYPE_INVALID = 14,
};

enum apfs_fs_kind {
	APFS_KIND_ANY = 0,
	APFS_KIND_NEW = 1,
	/* updated. */
	APFS_KIND_UPDATE = 2,
	/* deleted. */
	APFS_KIND_DEAD = 3,
	APFS_KIND_REFCNT = 4,
	APFS_KIND_INVALID = 255
};

#define APFS_FSKEY_ID_MASK 0x0fffffffffffffffULL
#define APFS_FSKEY_TYPE_MASK 0xf000000000000000ULL
#define APFS_FSKEY_TYPE_SHIFT 60
#define APFS_FSYSTEM_OBJ_ID_MARK 0x0fffffff00000000ULL

enum apfs_inode_flags {
	/*
	 * Temporary files.
	 */
	APFS_INODE_IS_PRIVATE = 0x00000001,
	/*
	 * Subdir inherit the flag.
	 */
	APFS_INODE_MAINTAIN_DIR_STATS = 0x00000002,
	/*
	 * NO inheritance.
	 */
	APFS_INODE_DIR_STATS_ORIGIN = 0x00000004,

	APFS_INODE_PROT_CLASS_EXPLICIT = 0x00000008,
	/* the inode is a clone. */
	APFS_INODE_CLONED = 0x00000010,
	APFS_INODE_FLAG_UNUSED = 0x00000020,
	/* has acl. */
	APFS_INODE_HAS_SECURITY_EA = 0x00000040,
	/*
	 * The inode was truncated but crash happend.
	 */
	APFS_INODE_BEING_TRUNCATED = 0x00000080,
	APFS_INODE_HAS_FINDER_INFO = 0x00000100,
	/* has a sparse bytes extended field. */
	APFS_INODE_IS_SPARSE = 0x00000200,
	/* was cloned */
	APFS_INODE_WAS_EVER_CLONED = 0x00000400,
	/*
	 * IOS only.
	 */
	APFS_INODE_ACTIVE_FILE_TRIMMED = 0x00000800,
	/*
	 * For fusion.
	 */
	APFS_INODE_PINNED_TO_MAIN = 0x00001000,
	/* For fusion */
	APFS_INODE_PINNED_TO_TIER2 = 0x00002000,
	/* has a resource fork. */
	APFS_INODE_HAS_RSRC_FORK = 0x00004000,
	/* not has a resource fork. */
	APFS_INODE_NO_RSRC_FORK = 0x00008000,

	APFS_INODE_ALLOCATION_SPILLEDOVER = 0x00010000,
	/* fusion */
	APFS_INODE_FAST_PROMOTE = 0x00020000,
	/*uncompressed size */
	APFS_INODE_HAS_UNCOMPRESSED_SIZE = 0x00040000,
	/* to be deleted at next purge. */
	APFS_INODE_IS_PURGEABLE = 0x00080000,
	/* purgeable */
	APFS_INODE_WANTS_TO_BE_PURGEABLE = 0x00100000,
	/* DO NOT TOUCH IT */
	APFS_INODE_IS_SYNC_ROOT = 0x00200000,
	/*
	 * Do not COW the inode if do snapshot.
	 */
	APFS_INODE_SNAPSHOT_COW_EXEMPTION = 0x00400000,

	APFS_INODE_INHERITED_FLAGS = (APFS_INODE_MAINTAIN_DIR_STATS |
				      APFS_INODE_SNAPSHOT_COW_EXEMPTION),
	/* preserved flags when cloning. */
	APFS_INODE_CLONED_FLAGS = (APFS_INODE_HAS_RSRC_FORK |
				   APFS_INODE_NO_RSRC_FORK |
				   APFS_INODE_HAS_FINDER_INFO |
				   APFS_INODE_SNAPSHOT_COW_EXEMPTION),
};

#define __APFS_INODE_FLAG_MASK (APFS_INODE_IS_APFS_PRIVATE		\
				| APFS_INODE_MAINTAIN_DIR_STATS		\
				| APFS_INODE_DIR_STATS_ORIGIN		\
				| APFS_INODE_PROT_CLASS_EXPLICIT	\
				| APFS_INODE_WAS_CLONED			\
				| APFS_INODE_HAS_SECURITY_EA		\
				| APFS_INODE_BEING_TRUNCATED		\
				| APFS_INODE_HAS_FINDER_INFO		\
				| APFS_INODE_IS_SPARSE			\
				| APFS_INODE_WAS_EVER_CLONED		\
				| APFS_INODE_ACTIVE_FILE_TRIMMED	\
				| APFS_INODE_PINNED_TO_MAIN		\
				| APFS_INODE_PINNED_TO_TIER2		\
				| APFS_INODE_HAS_RSRC_FORK		\
				| APFS_INODE_NO_RSRC_FORK		\
				| APFS_INODE_ALLOCATION_SPILLEDOVER	\
				| APFS_INODE_FAST_PROMOTE		\
				| APFS_INODE_HAS_UNCOMPRESSED_SIZE	\
				| APFS_INODE_IS_PURGEABLE		\
				| APFS_INODE_WANTS_TO_BE_PURGEABLE	\
				| APFS_INODE_IS_SYNC_ROOT		\
				| APFS_INODE_SNAPSHOT_COW_EXEMPTION)

#define APFS_INODE_PINNED_MASK (APFS_INODE_PINNED_TO_MAIN |	\
				APFS_INODE_PINNED_TO_TIER2)
/* Inode Numbers */
/*
 * If a volume belongs to a volume group, every inode ino should be equal to
 * inode + UNIFIED_ID_SPACE_MARK
 */
#define APFS_INVALID_INO_NUM 0
/* parent inode of root */
#define APFS_ROOT_DIR_PARENT 1
/* root ino */
#define APFS_ROOT_DIR_INO 2
#define APFS_ROOT_DIR "root"

/*
 * The inode number of "private-dir".
 */
#define APFS_PRIVATE_DIR_INO 3
/* snap meta data dir ino */
#define APFS_SNAP_DIR_INO_NUM 6
/* purgeable files. */
#define APFS_PURGEABLE_DIR_INO_NUM 7
#define APFS_MIN_USER_INO_NUM 16
#define APFS_UNIFIED_ID_SPACE_MARK 0x0800000000000000ULL

#define XATTR_APFS_PREFIX "apfs."
#define XATTR_APFS_PREFIX_LEN (sizeof(XATTR_BTRFS_PREFIX) - 1)

#define APFS_XATTR_MAX_EMBEDDED_SIZE 3804
#define APFS_SYMLINK_EA_NAME "com.apple.fs.symlink"
#define APFS_FIRMLINK_EA_NAME "com.apple.fs.firmlink"
#define APFS_COW_EXEMPT_COUNT_NAME "com.apple.fs.cow-exempt-file-count"
#define APFS_EMAIL_METADATA_NAME "com.apple.metadata"
#define APFS_DECOMP_FS_NAME "com.apple.decmpfs"
#define APFS_RESOURCE_FORK_NAME "com.apple.ResourceFork"

#define APFS_OWNING_OBJ_ID_INVALID ~0ULL
#define APFS_OWNING_OBJ_ID_UNKNOWN ~1ULL
#define APFS_JOBJ_MAX_KEY_SIZE 832
#define APFS_JOBJ_MAX_VALUE_SIZE 3808
#define APFS_MIN_DOC_ID 3
#define APFS_FEXT_CRYPTO_ID_IS_TWEAK 0x01

#define S_IFWHT 0160000

struct apfs_inode_key {
	__le64 id_and_type;
} __attribute__((__packed__));

enum apfs_inode_bsd_flags {
	APFS_UF_NODUMP = 0x1,
	APFS_UF_IMMUTABLE = 0x2,
	APFS_UF_APPEND = 0x4,
	APFS_UF_OPAQUE = 0x8,
	APFS_UF_NOUNLINK = 0x10, // Reserved on macOS
	APFS_UF_COMPRESSED = 0x20,
	APFS_UF_TRACKED = 0x40,
	APFS_UF_DATAVAULT = 0x80,

	// 0x100 - 0x4000 reserved
	APFS_UF_HIDDEN = 0x8000,
	APFS_SF_ARCHIVED = 0x10000,
	APFS_SF_IMMUTABLE = 0x20000,
	APFS_SF_APPEND = 0x40000,
	APFS_SF_RESTRICTED = 0x80000,
	APFS_SF_NOUNLINK = 0x100000,
	APFS_SF_SNAPSHOT = 0x200000, // Reserved on macOS
	APFS_SF_FIRMLINK = 0x800000,
	APFS_SF_DATALESS = 0x40000000
};

struct apfs_inode_val {
	__le64 parent;
	__le64 privateid;
	__le64 btime;
	__le64 mtime;
	__le64 ctime;
	__le64 atime;
	__le64 flags;
	union {
		__le32 nchildren;
		/* hard links count */
		__le32 nlink;
	};
	__le32 default_protection_class;
	__le32 transid;
	/* The inode's BSD flags. */
	__le32 bsd_flags;
	__le32 uid;
	__le32 gid;
	/* apfs uses 16 bits mode_t */
	__le16 mode;
	__le16 pad1;
	/*
	 * ignore it if inode flags not has APFS_INODE_HAS_UNCOMPRESSED_SIZE
	 */
	__le64 size;
	/* extended fields. */
	u8 xfields[];
} __attribute__((__packed__));

APFS_SETGET_STACK_FUNCS(stack_inode_val_parent, struct apfs_inode_val, parent, 64);
APFS_SETGET_STACK_FUNCS(stack_inode_val_privateid, struct apfs_inode_val,
			privateid, 64);
APFS_SETGET_STACK_FUNCS(stack_inode_val_btime, struct apfs_inode_val, btime, 64);
APFS_SETGET_STACK_FUNCS(stack_inode_val_mtime, struct apfs_inode_val, mtime, 64);
APFS_SETGET_STACK_FUNCS(stack_inode_val_ctime, struct apfs_inode_val, ctime, 64);
APFS_SETGET_STACK_FUNCS(stack_inode_val_atime, struct apfs_inode_val, atime, 64);
APFS_SETGET_STACK_FUNCS(stack_inode_val_flags, struct apfs_inode_val, flags, 64);
APFS_SETGET_STACK_FUNCS(stack_inode_val_nchildren, struct apfs_inode_val, nchildren, 32);
APFS_SETGET_STACK_FUNCS(stack_inode_val_nlink, struct apfs_inode_val, nlink, 32);
APFS_SETGET_STACK_FUNCS(stack_inode_val_default_protection_class,
			struct apfs_inode_val, default_protection_class, 32);
APFS_SETGET_STACK_FUNCS(stack_inode_val_transid,
			struct apfs_inode_val, transid, 32);
APFS_SETGET_STACK_FUNCS(stack_inode_val_bsd_flags, struct apfs_inode_val,
			bsd_flags, 32);
APFS_SETGET_STACK_FUNCS(stack_inode_val_uid, struct apfs_inode_val, uid, 32);
APFS_SETGET_STACK_FUNCS(stack_inode_val_gid, struct apfs_inode_val, gid, 32);
APFS_SETGET_STACK_FUNCS(stack_inode_val_mode, struct apfs_inode_val, mode, 16);
APFS_SETGET_STACK_FUNCS(stack_inode_val_size, struct apfs_inode_val, size, 64);

APFS_SETGET_FUNCS(inode_val_parent, struct apfs_inode_val, parent, 64);
APFS_SETGET_FUNCS(inode_val_privateid, struct apfs_inode_val,
			privateid, 64);
APFS_SETGET_FUNCS(inode_val_btime, struct apfs_inode_val, btime, 64);
APFS_SETGET_FUNCS(inode_val_mtime, struct apfs_inode_val, mtime, 64);
APFS_SETGET_FUNCS(inode_val_ctime, struct apfs_inode_val, ctime, 64);
APFS_SETGET_FUNCS(inode_val_atime, struct apfs_inode_val, atime, 64);
APFS_SETGET_FUNCS(inode_val_flags, struct apfs_inode_val, flags, 64);
APFS_SETGET_FUNCS(inode_val_nchildren, struct apfs_inode_val, nchildren, 32);
APFS_SETGET_FUNCS(inode_val_nlink, struct apfs_inode_val, nlink, 32);
APFS_SETGET_FUNCS(inode_val_default_protection_class,
			struct apfs_inode_val, default_protection_class, 32);
APFS_SETGET_FUNCS(inode_val_transid, struct apfs_inode_val, transid, 32);
APFS_SETGET_FUNCS(inode_val_bsd_flags, struct apfs_inode_val, bsd_flags, 32);
APFS_SETGET_FUNCS(inode_val_uid, struct apfs_inode_val, uid, 32);
APFS_SETGET_FUNCS(inode_val_gid, struct apfs_inode_val, gid, 32);
APFS_SETGET_FUNCS(inode_val_mode, struct apfs_inode_val, mode, 16);
APFS_SETGET_FUNCS(inode_val_size, struct apfs_inode_val, size, 64);

/* BSD flags */
#define APFS_INODE_BSD_NODUMP			0x00000001
#define APFS_INODE_BSD_IMMUTABLE		0x00000002
#define APFS_INODE_BSD_APPEND			0x00000004
#define APFS_INODE_COMPRESSED			0x00000020

/* Directory Entry File Types */
#define APFS_DT_UNKNOWN 0
#define APFS_DT_FIFO 1
#define APFS_DT_CHR 2
#define APFS_DT_DIR 4
#define APFS_DT_BLK 6
#define APFS_DT_REG 8
#define APFS_DT_LINK 10
#define APFS_DT_SOCK 12
#define APFS_DT_WHT 14

struct apfs_drec_key {
	__le64 id_and_type;
	__le16 name_len;
	u8 name[0];
} __attribute__((__packed__));

#define APFS_DREC_LEN_MASK 0x000003ff
#define APFS_DREC_HASH_MASK 0xfffffc00
#define APFS_DREC_HASH_SHIFT 10

struct apfs_drec_hashed_key {
	__le64 id_and_type;
	__le32 name_len_and_hash;
	u8 name[0];
} __attribute__((__packed__));

enum apfs_dir_rec_flags {
	APFS_DREC_TYPE_MASK = 0x000f,
	APFS_RESERVED_10 = 0x0010
};

struct apfs_drec_item {
	__le64 ino;
	__le64 btime; /* date borned */
	__le16 flags;
	u8 xfields[];
} __attribute__((__packed__));

APFS_SETGET_STACK_FUNCS(drec_item_ino, struct apfs_drec_item, ino, 64);
APFS_SETGET_STACK_FUNCS(drec_item_btime, struct apfs_drec_item, btime, 64);
APFS_SETGET_STACK_FUNCS(drec_item_flags, struct apfs_drec_item, flags, 64);

static inline
u8 apfs_drec_item_type(const struct apfs_drec_item *di)
{
	return apfs_drec_item_flags(di) & APFS_DREC_TYPE_MASK;
}

APFS_SETGET_FUNCS(drec_ino, struct apfs_drec_item, ino, 64);
APFS_SETGET_FUNCS(drec_btime, struct apfs_drec_item, btime, 64);
APFS_SETGET_FUNCS(drec_flags, struct apfs_drec_item, flags, 64);

static inline
u8 apfs_drec_type(const struct extent_buffer *eb,
		  const struct apfs_drec_item *di)
{
	__le16 type;

	read_eb_member(eb, di, struct apfs_drec_item, flags, &type);
	return le16_to_cpu(type) & APFS_DREC_TYPE_MASK;
}

struct apfs_dir_stats_key {
	__le64 id_and_type;
} __attribute__((__packed__));

struct apfs_dir_stats_val {
	__le64 num_children;
	__le64 total_size;
	/* parent */
	__le64 chained_key;
	__le64 gen;
} __attribute__((__packed__));

enum apfs_xattr_flag {
	/* stored in a file extent. */
	APFS_XATTR_DATA_STREAM = 0x00000001,
	/* stored inlined directly*/
	APFS_XATTR_DATA_EMBEDDED = 0x00000002,
	/* owned by the file system, e.g. symbolic links. */
	APFS_XATTR_FILE_SYSTEM_OWNED = 0x00000004,
	APFS_XATTR_RESERVED_8 = 0x00000008,
};

struct apfs_xattr_key {
	__le64 id_and_type;
	__le16 name_len;
	u8 name[0];
} __attribute__((__packed__));

struct apfs_xattr_item {
	__le16 flags;
	__le16 len;
	u8 data[0];
} __attribute__((__packed__));

APFS_SETGET_STACK_FUNCS(stack_xattr_item_flags, struct apfs_xattr_item, flags, 16);
APFS_SETGET_STACK_FUNCS(stack_xattr_item_len, struct apfs_xattr_item, len, 16);

APFS_SETGET_FUNCS(xattr_item_flags, struct apfs_xattr_item, flags, 16);
APFS_SETGET_FUNCS(xattr_item_len, struct apfs_xattr_item, len, 16);

/* Data Streams */

#define APFS_PEXT_LEN_MASK 0x0fffffffffffffffULL
#define APFS_PEXT_KIND_MASK 0xf000000000000000ULL
#define APFS_PEXT_KIND_SHIFT 60

struct apfs_phys_extent_key {
	__le64 id_and_type;
} __attribute__((__packed__));

struct apfs_phys_extent_item {
	__le64 len_and_kind;
	/*
	 * points to an inode/xattr item.
	 */
	__le64 owner;
	__le32 refs;
} __attribute__((__packed__));

struct apfs_file_extent_key {
	__le64 id_and_type;
	/* file offset */
	__le64 offset;
} __attribute__((__packed__));

#define APFS_FILE_EXTENT_LEN_MASK 0x00ffffffffffffffULL
#define APFS_FILE_EXTENT_FLAG_MASK 0xff00000000000000ULL
#define APFS_FILE_EXTENT_FLAG_SHIFT 56

struct apfs_file_extent_val {
	__le64 len_and_flags;
	__le64 bno;
	__le64 cryptoid;
} __attribute__((__packed__));

APFS_SETGET_FUNCS(file_extent_len_and_flags, struct apfs_file_extent_val,
		  len_and_flags, 64);
APFS_SETGET_FUNCS(file_extent_bno, struct apfs_file_extent_val, bno, 64);
APFS_SETGET_FUNCS(file_extent_cryptoid, struct apfs_file_extent_val, cryptoid,
		  64);

static inline u64
apfs_file_extent_len(const struct extent_buffer *eb,
		     const struct apfs_file_extent_val *fe)
{
	return apfs_file_extent_len_and_flags(eb, fe) & APFS_FILE_EXTENT_LEN_MASK;
}

static inline u64
apfs_file_extent_raw_bytes(const struct extent_buffer *eb,
			   const struct apfs_file_extent_val *fe)
{
	if (apfs_file_extent_bno(eb, fe) == 0)
		return 0;
	return apfs_file_extent_len(eb, fe);

}

static inline u8
apfs_file_extent_flags(const struct extent_buffer *eb,
		       const struct apfs_file_extent_val *fe)
{
	return (apfs_file_extent_len_and_flags(eb, fe) & APFS_FILE_EXTENT_FLAG_MASK)
		>> APFS_FILE_EXTENT_FLAG_SHIFT;
}

struct apfs_dstream_id_key {
	__le64 id_and_type;
} __attribute__((__packed__));

struct apfs_dstream_id_val {
	__le32 refcnt;
} __attribute__((__packed__));

struct apfs_dstream_item {
	__le64 size;
	__le64 allocated;
	__le64 cryptoid;
	__le64 written; // in bytes
	__le64 read; // in bytes, increaments every read time
} __attribute__((__packed__, aligned(8)));

APFS_SETGET_FUNCS(dstream_size, struct apfs_dstream_item, size, 64);
APFS_SETGET_FUNCS(dstream_allocated, struct apfs_dstream_item, allocated, 64);
APFS_SETGET_FUNCS(dstream_cryptoid, struct apfs_dstream_item, cryptoid, 64);
APFS_SETGET_FUNCS(dstream_written, struct apfs_dstream_item, written, 64);
APFS_SETGET_FUNCS(dstream_read, struct apfs_dstream_item, read, 64);

APFS_SETGET_STACK_FUNCS(stack_dstream_size, struct apfs_dstream_item, size, 64);
APFS_SETGET_STACK_FUNCS(statck_dstream_allocated, struct apfs_dstream_item, allocated, 64);
APFS_SETGET_STACK_FUNCS(stack_dstream_cryptoid, struct apfs_dstream_item, cryptoid, 64);
APFS_SETGET_STACK_FUNCS(stack_dstream_written, struct apfs_dstream_item, written, 64);
APFS_SETGET_STACK_FUNCS(stack_dstream_read, struct apfs_dstream_item, read, 64);

struct apfs_xattr_dstream {
	__le64 id; // oid of file extent
	struct apfs_dstream_item dstream;
};

APFS_SETGET_FUNCS(xattr_dstream_id, struct apfs_xattr_dstream, id, 64);
APFS_SETGET_STACK_FUNCS(stack_xattr_dstream_id, struct apfs_xattr_dstream, id, 64);


/* Inode extended field types */

enum apfs_xfield_type {
	APFS_EXT_SIBLING = 1, /* hard link  */
	APFS_EXT_SNAP_XID = 1,
	APFS_EXT_DELTA_TREE_OID = 2,
	APFS_EXT_DOCUMENT_ID = 3,
	APFS_EXT_NAME = 4, /* name of the inode */
	APFS_EXT_PREV_FSIZE = 5, /* __le64 */
	APFS_EXT_TYPE_RESERVED6 = 6,
	APFS_EXT_FINDER_INFO = 7, /* __le32 */
	APFS_EXT_DSTREAM  = 8,
	APFS_EXT_RESERVED_9 = 9,
	APFS_EXT_DIR_STATS_KEY = 10, /* apfs_dir_stats */
	APFS_EXT_FS_UUID = 11, /* uuid of a volume to be mounted on this dir */
	APFS_EXT_TYPE_RESERVED_12 = 12,
	APFS_EXT_SPARSE_BYTES = 13, /* __le64 */
	APFS_EXT_RDEV = 14, /* __le32 */
	APFS_EXT_PURGEABLE_FLAGS = 15, /* do not use it */
	APFS_EXT_ORIG_SYNC_ROOT_ID = 16
};


/* Inode extended field flags */

#define APFS_XF_DATA_DEPENDENT 0x0001
#define APFS_XF_DO_NOT_COPY 0x0002
#define APFS_XF_RESERVED_4 0x0004
#define APFS_XF_CHILDREN_INHERIT 0x0008
#define APFS_XF_USER_FIELD 0x0010
#define APFS_XF_SYSTEM_FIELD 0x0020
#define APFS_XF_RESERVED_40 0x0040
#define APFS_XF_RESERVED_80 0x0080

/* Extended Fields */
struct apfs_xfield_blob {
	/* num of exts */
	__le16 num;
	/* used in bytes */
	__le16 used;
	u8 data[];
};

APFS_SETGET_STACK_FUNCS(stack_xfield_blob_num, struct apfs_xfield_blob, num, 16);
APFS_SETGET_STACK_FUNCS(stack_xfield_blob_used, struct apfs_xfield_blob, used, 16);

APFS_SETGET_FUNCS(xfield_blob_num, struct apfs_xfield_blob, num, 16);
APFS_SETGET_FUNCS(xfield_blob_used, struct apfs_xfield_blob, used, 16);

struct apfs_xfield {
	u8 type;
	u8 flags;
	__le16 size;
};

APFS_SETGET_STACK_FUNCS(stack_xfield_type, struct apfs_xfield, type, 8);
APFS_SETGET_STACK_FUNCS(stack_xfield_flags, struct apfs_xfield, flags, 8);
APFS_SETGET_STACK_FUNCS(stack_xfield_size, struct apfs_xfield, flags, 16);

APFS_SETGET_FUNCS(xfield_type, struct apfs_xfield, type, 8);
APFS_SETGET_FUNCS(xfield_flags, struct apfs_xfield, flags, 8);
APFS_SETGET_FUNCS(xfield_size, struct apfs_xfield, size, 16);


/* Siblings */
struct apfs_sibling_key {
	__le64 id_and_type;
	/*
	 * matches id of corrsponded apfs_sibling_map
	 */
	__le64 id;
} __attribute__((__packed__));

struct apfs_sibling_val {
	__le64 parent_id;
	__le16 name_len;
	u8 name[0];
} __attribute__((__packed__));

/* reverse map of apfs_sibling_key */
struct apfs_sibling_map_key {
	__le64 id_and_type;
} __attribute__((__packed__));

struct apfs_sibling_map_val {
	uint64_t file_id;
} __attribute__((packed));

/* Snapshot Metadata */
struct apfs_snap_meta_key {
	/* id is xid */
	__le64 id_and_type;
} __attribute__((__packed__));

/* snap_meta_flags */
enum apfs_snap_meta_flags {
	SNAP_META_PENDING_DATALESS = 0x00000001,
	SNAP_META_MERGE_IN_PROGRESS = 0x00000002,
};

struct apfs_snap_meta {
	/* physical object identifier of the B-tree stores extents info. */
	__le64 extref_tree_oid;
	/* The physical object identifier of the volume superblock. */
	__le64 super_bno;
	/* The time that this snapshot was created. */
	__le64 btime;
	__le64 mtime;
	__le64 inum;
	__le32 extref_tree_type;
	__le32 flags;
	__le16 namelen;
	u8 name[0];
} __attribute__((__packed__));

APFS_SETGET_FUNCS(snap_extref_tree_oid, struct apfs_snap_meta, extref_tree_oid, 64);
APFS_SETGET_FUNCS(snap_super_bno, struct apfs_snap_meta, super_bno, 64);
APFS_SETGET_FUNCS(snap_btime, struct apfs_snap_meta, btime, 64);
APFS_SETGET_FUNCS(snap_mtime, struct apfs_snap_meta, mtime, 64);
APFS_SETGET_FUNCS(snap_inum, struct apfs_snap_meta, inum, 64);
APFS_SETGET_FUNCS(snap_extref_tree_type, struct apfs_snap_meta, extref_tree_type, 32);
APFS_SETGET_FUNCS(snap_flags, struct apfs_snap_meta, flags, 32);
APFS_SETGET_FUNCS(snap_namelen, struct apfs_snap_meta, namelen, 16);


struct apfs_snap_name_key {
	/* id should always be 0 */
	__le64 id_and_type;
	__le16 name_len;
	u8 name[0];
} __attribute__((__packed__));

struct apfs_snap_name_val {
	/* last trans id */
	__le64 xid;
} __attribute__((__packed__));

APFS_SETGET_STACK_FUNCS(snap_name_xid, struct apfs_snap_name_val, xid, 64);

struct apfs_snap_ext {
	__le32 version;
	__le32 flags;
	__le64 xid;
	uuid_t uuid;
	__le64 token;
} __attribute__((__packed__));

APFS_SETGET_STACK_FUNCS(snap_ext_version, struct apfs_snap_ext, version, 32);
APFS_SETGET_STACK_FUNCS(snap_ext_flags, struct apfs_snap_ext, flags, 32);
APFS_SETGET_STACK_FUNCS(snap_ext_xid, struct apfs_snap_ext, xid, 64);
APFS_SETGET_STACK_FUNCS(snap_ext_token, struct apfs_snap_ext, token, 64);

struct apfs_snap_ext_phys {
	struct apfs_obj_header o;
	struct apfs_snap_ext se;
};

#define BTREE_UINT64_KEYS 0x00000001
/*
 * Don't split nodes if half full.
 */
#define BTREE_SEQUENTIAL_INSERT 0x00000002
/* allow keys without corresponded values*/
#define BTREE_ALLOW_GHOSTS 0x00000004
/*
 * The nodes in the B-tree use ephemeral address ptrs
 */
#define BTREE_EPHEMERAL 0x00000008
#define BTREE_PHYSICAL 0x00000010
/* disapears after unmounting. */
#define BTREE_NONPERSISTENT 0x00000020
/* unaligned to 8-bytes boundaries. */
#define BTREE_KV_NONALIGNED 0x00000040
/*
 * Parent nodes has csum of its children.
 */
#define BTREE_HASHED 0x00000080
#define BTREE_NOHEADER 0x00000100

/*
 * entries number add/remove while modifying table of contents.
 */
#define BTREE_TOC_ENTRY_INCREMENT 8
/*
 * The maximum unused entries in toc.
 */
#define BTREE_TOC_ENTRY_MAX_UNUSED (2 * BTREE_TOC_ENTRY_INCREMENT)

/* node flags */
#define APFS_NODE_ROOT 0x0001
#define APFS_NODE_LEAF 0x0002
#define APFS_NODE_FIXED_KV_SIZE 0x0004
#define APFS_NODE_HASHED 0x0008
#define APFS_NODE_NOHEADER 0x0010
/* NEVER APPEARS ON DISK */
#define APFS_NODE_CHECK_KOFF_INVAL 0x8000

#define APFS_DEFAULT_NODE_SIZE 4096

#define APFS_NODE_MIN_ENTRY_COUNT 4

/*
 * if off is 0xffff, means offset is 0
 */
#define APFS_BTOFF_INVALID 0xffff
struct apfs_disk_loc {
	__le16 off;
	__le16 len;
};

struct apfs_loc {
	u16 off;
	u16 len;
};

static inline void
apfs_disk_loc_to_cpu(struct apfs_disk_loc *disk,
		     struct apfs_loc *dst)
{
	dst->off = le16_to_cpu(disk->off);
	dst->len = le16_to_cpu(disk->len);
}

struct apfs_disk_kv {
	struct apfs_disk_loc k;
	struct apfs_disk_loc v;
};

struct apfs_kv {
	struct apfs_loc k;
	struct apfs_loc v;
};

static inline void
apfs_disk_kv_to_cpu(struct apfs_disk_kv *disk,
		    struct apfs_kv *dst)
{
	apfs_disk_loc_to_cpu(&disk->k, &dst->k);
	apfs_disk_loc_to_cpu(&disk->v, &dst->v);
}

struct apfs_disk_fixed_kv {
	__le16 k;
	__le16 v;
};

struct apfs_fixed_kv {
	u16 k;
	u16 v;
};

static inline void
apfs_disk_fixed_kv_to_cpu(struct apfs_disk_fixed_kv *disk,
			  struct apfs_fixed_kv *dst)
{
	dst->k = le16_to_cpu(disk->k);
	dst->v = le16_to_cpu(disk->v);
}

struct apfs_disk_node {
	struct apfs_obj_header o;
	__le16 flags;
	__le16 level;
	__le32 nkeys;
	struct apfs_disk_loc table_space;
	struct apfs_disk_loc free_space;
	struct apfs_disk_loc key_free_list;
	struct apfs_disk_loc val_free_list;
	__le64 data[];
};

/* struct apfs_disk_node */
APFS_SETGET_FUNCS(disk_node_flags, struct apfs_disk_node, flags, 16);
APFS_SETGET_FUNCS(disk_node_level, struct apfs_disk_node, level, 16);
APFS_SETGET_FUNCS(disk_node_nkeys, struct apfs_disk_node, nkeys, 32);

APFS_SETGET_STACK_FUNCS(stack_disk_node_flags, struct apfs_disk_node, flags, 16);
APFS_SETGET_STACK_FUNCS(stack_disk_node_level, struct apfs_disk_node, level, 16);
APFS_SETGET_STACK_FUNCS(stack_disk_node_nkeys, struct apfs_disk_node, nkeys, 32);

struct apfs_node_header
{
	struct apfs_obj_header o;
	__le16 flags;
	__le16 level;
	__le32 nkeys;
	struct apfs_disk_loc table_space;
	struct apfs_disk_loc free_space;
	struct apfs_disk_loc key_free_list;
	struct apfs_disk_loc val_free_list;
};

struct apfs_btree_info {
	__le32 flags;
	__le32 node_size;
	/*
	 * Nonzero if all keys is in a fixed size.
	 */
	__le32 key_size;
	/*
	 * Nonzero if all keys is in a fixed size.
	 */
	__le32 val_size;
};

APFS_SETGET_STACK_FUNCS(btree_flags, struct apfs_btree_info, flags, 32);
APFS_SETGET_STACK_FUNCS(btree_nodesize, struct apfs_btree_info, node_size, 32);
APFS_SETGET_STACK_FUNCS(btree_keysize, struct apfs_btree_info, key_size, 32);
APFS_SETGET_STACK_FUNCS(btree_valsize, struct apfs_btree_info, val_size, 32);

struct apfs_root_info {
	struct apfs_btree_info info;
	__le32 longest_key;
	__le32 longest_val;
	__le64 key_count;
	__le64 node_count;
};

APFS_SETGET_FUNCS(root_info_longest_key, struct apfs_root_info,	longest_key, 32);
APFS_SETGET_FUNCS(root_info_longest_val, struct apfs_root_info,	longest_val, 32);
APFS_SETGET_FUNCS(root_info_key_count, struct apfs_root_info, key_count, 64);
APFS_SETGET_FUNCS(root_info_node_count, struct apfs_root_info, node_count, 64);

#define APFS_ROOT_INFO_OFFSET (APFS_DEFAULT_NODE_SIZE -		\
			       sizeof(struct apfs_root_info))

#define APFS_NODE_HASH_SIZE_MAX 64
struct apfs_node_val {
	__le64 child_oid;
	u8 child_hash[APFS_NODE_HASH_SIZE_MAX];
};

/* Sealed Volumes */

enum {
	APFS_INTEGRITY_META_VERSION_INVALID = 0,
	APFS_INTEGRITY_META_VERSION_1 = 1,
	APFS_INTEGRITY_META_VERSION_2 = 2,
	APFS_INTEGRITY_META_VERSION_HIGHEST = APFS_INTEGRITY_META_VERSION_2
};

#define APFS_SEAL_BROKEN (1U << 0)

enum apfs_hash_type {
	APFS_HASH_INVALID = 0,
	APFS_HASH_SHA256 = 0x1,
	APFS_HASH_SHA512_256 = 0x2,
	APFS_HASH_SHA384 = 0x3,
	APFS_HASH_SHA512 = 0x4,
	APFS_HASH_MIN = APFS_HASH_SHA256,
	APFS_HASH_MAX = APFS_HASH_SHA512,
	APFS_HASH_DEFAULT = APFS_HASH_SHA256,

};

struct apfs_integrity_meta_phys {
	__le64 o;
	__le32 version;
	__le32 flags;
	enum apfs_hash_type hash_type;
	__le32 root_hash_offset;
	__le64 broken_xid;
	__le64 reserved[9];
} __attribute__((__packed__));

struct apfs_fext_tree_key {
	/* ino */
	__le64 private_id;
	__le64 logical_addr;
} __attribute__((__packed__));

struct fext_tree_val {
	__le64 len_and_flags;
	__le64 phys_block_num;
} __attribute__((__packed__));

#define APFS_FILE_INFO_LBA_MASK 0x00ffffffffffffffULL
#define APFS_FILE_INFO_TYPE_MASK 0xff00000000000000ULL
#define APFS_FILE_INFO_TYPE_SHIFT 56

struct apfs_file_info_key {
	__le64 id_and_type;
	__le64 info_and_lba;
} __attribute__((__packed__));

enum apfs_file_info_type {
	APFS_FILE_INFO_DATA_HASH = 1,
};

struct apfs_file_data_hash {
	__le16 hashed_len;
	u8 hash_size;
	u8 hash[0];
} __attribute__((__packed__));

struct apfs_file_info_val {
	union {
		struct apfs_file_data_hash dhash;
	};
} __attribute__((__packed__));

struct apfs_chunk_info {
	__le64 xid;
	__le64 addr;
	__le32 block_count;
	__le32 free_count;
	u64 bitmap_addr;
};

struct apfs_chunk_info_block {
	__le64 o;
	__le32 index;
	__le32 chunk_info_count;
	struct apfs_chunk_info chunk_info[];
};

struct cib_addr_block {
	__le64 o;
	__le32 index;
	__le32 cib_count;
	u64 cib_addr[];
};

struct space_free_queue_key {
	__le64 xid;
	u64 paddr;
};

struct space_free_queue_val {
	__le64 count;
	__le64 tree_oid;
	__le64 oldest_xid;
	__le16 tree_node_limit;
	__le16 pad16;
	__le32 pad32;
	__le64 reserved;
};

struct space_free_queue {
	struct space_free_queue_key key;
	struct space_free_queue_val count;
};

struct space_device {
	__le64 block_count;
	__le64 chunk_count;
	__le32 cib_count;
	__le32 cab_count;
	__le64 free_count;
	__le32 addr_offset;
	__le32 reserved;
	__le64 reserved2;
};

struct space_zone {
	__le64 start;
	__le64 end;
};

#define APFS_PRE_ZONE_NUM 7
#define APFS_INVALID_ZONE 0

struct space_zone_info_phys {
	struct space_zone cur;
	struct space_zone pre[APFS_PRE_ZONE_NUM];
	__le16 zone_id;
	__le16 pre_index;
	__le32 reserved;
};

#define SM_FLAG_VERSIONED 0x00000001
enum apfs_sfq {
	SFQ_IP = 0,
	SFQ_MAIN = 1,
	SFQ_TIER2 = 2,
	SFQ_COUNT = 3
};

enum smdev {
	SD_MAIN = 0,
	SD_TIER2 = 1,
	SD_COUNT = 2
};

#define APFS_CI_COUNT_MASK 0x000fffff
#define APFS_CI_COUNT_RESERVED_MASK 0xfff00000

/* Internal-Pool Bitmap */
#define SPACEMAN_IP_BM_TX_MULTIPLIER 16
#define SPACEMAN_IP_BM_INDEX_INVALID 0xffff
#define SPACEMAN_IP_BM_BLOCK_COUNT_MAX 0xfffe

struct spaceman_phys {
	__le64 o;
	__le32 block_size;
	__le32 blocks_per_chunk;
	__le32 chunks_per_cib;
	__le32 cibs_per_cab;
	struct space_device dev[SD_COUNT];
	__le32 flags;
	__le32 ip_bm_tx_multiplier;
	__le64 ip_block_count;
	__le32 ip_bm_size_in_blocks;
	__le32 ip_bm_block_count;
	u64 ip_bm_base;
	u64 ip_base;
	__le64 fs_reserve_block_count;
	__le64 fs_reserve_alloc_count;
	struct space_free_queue fq[SFQ_COUNT];
	__le16 ip_bm_free_head;
	__le16 ip_bm_free_tail;
	__le32 ip_bm_xid_offset;
	__le32 ip_bitmap_offset;
	__le32 ip_bm_free_next_offset;
	__le32 version;
	__le32 struct_size;
	struct space_zone_info_phys datazone;
};

/* Reaper */

#define APFS_REAPER_NR_BHM_FLAG 0x00000001
#define APFS_REAPER_NR_CONTINUE 0x00000002


#define APFS_NRLE_VALID 0x00000001
#define APFS_NRLE_REAP_ID_RECORD 0x00000002
#define APFS_NRLE_CALL 0x00000004
#define APFS_NRLE_COMPLETION 0x00000008
#define APFS_NRLE_CLEANUP 0x00000010

struct apfs_reaper_phys {
	__le64 o;
	__le64 next_reap_id;
	__le64 completed_id;
	__le64 head;
	__le64 tail;
	__le32 flags;
	__le32 rlcount;
	__le32 type;
	__le32 size;
	__le64 fs_oid;
	__le64 oid;
	__le64 xid;
	__le32 nrle_flags;
	__le32 state_buffer_size;
	u8 state_buffer[];
};

struct apfs_omap_reap_state {
	__le32 phase;
	/* last freed key */
	struct apfs_omap_key ok;
};

struct apfs_omap_cleanup_state {
	__le32 cleaning;

	__le32 flags;
	__le64 prev_xid;
	__le64 start_xid;
	__le64 end_xid;
	__le64 next_xid;
	struct apfs_omap_key cur;
};

struct apfs_reap_state {
	__le64 last_phy_block_number;
	__le64 cur_snap_xid;
	__le32 phase;

} __attribute__((__packed__));

struct apfs_reap_list_entry {
	__le32 next;
	__le32 flags;
	__le32 type;
	__le32 size;
	__le64 fs_oid;
	__le64 oid;
	__le64 xid;
};

struct apfs_reap_list_phys {
	__le64 o;
	__le64 next;
	__le32 flags;
	__le32 max;
	__le32 count;
	__le32 first;
	__le32 last;
	__le32 free;
	struct apfs_reap_list_entry entries[];
};

enum {
	APFS_REAP_PHASE_START = 0,
	APFS_REAP_PHASE_SNAPSHOTS = 1,
	APFS_REAP_PHASE_ACTIVE_FS = 2,
	APFS_REAP_PHASE_DESTROY_OMAP = 3,
	APFS_REAP_PHASE_DONE = 4
};

struct apfs_crypto_key {
	u64 id_and_type;
} __attribute__((packed));

struct apfs_crypto_val {
	uint32_t refcnt;
	struct apfs_wrapped_crypto_state state;
} __attribute__((aligned(4),packed));

static inline u16
apfs_header_flags(const struct extent_buffer *eb)
{
	u16 flags;

	read_eb_member(eb, 0, struct apfs_node_header, flags, &flags);
	return flags;
}

static inline void
apfs_set_header_flags(const struct extent_buffer *eb, u16 flags)
{
	return ;
}

enum APFS_ROOT_TYPE{
	APFS_MIN_ROOT_TYPE = 0,
	APFS_OMAP_ROOT = 0,
	APFS_SNAP_META_ROOT = 1,
	APFS_FS_ROOT = 2,
	APFS_FEXT_ROOT = 3,
	APFS_EXTREF_ROOT = 4,
	APFS_INVALID_ROOT_TYPE = 5,
	APFS_MAX_ROOT_TYPE = 5
};

static inline bool is_apfs_kv_size_fixed(const struct extent_buffer *eb)
{
	return apfs_header_flags(eb) & APFS_NODE_FIXED_KV_SIZE;
}

static inline u32
apfs_header_nkeys(const struct extent_buffer *eb)
{
	u32 nkeys;

	read_eb_member(eb, 0, struct apfs_node_header, nkeys, &nkeys);
	return nkeys;
}

static inline u32
apfs_header_nritems(const struct extent_buffer *eb)
{
	return apfs_header_nkeys(eb);
}

static inline u32
apfs_set_header_nritems(const struct extent_buffer *eb, u32 nritems)
{
	BUG();
}

/* original btrfs raw layout structs */
/*
 * Maximum number of mirrors that can be available for all profiles counting
 * the target device of dev-replace as one. During an active device replace
 * procedure, the target device of the copy operation is a mirror for the
 * filesystem data as well that can be used to read data in order to repair
 * read errors on other disks.
 *
 * Current value is derived from RAID1C4 with 4 copies.
 */
#define APFS_MAX_MIRRORS (4 + 1)

#define APFS_MAX_LEVEL 16

#define APFS_OLDEST_GENERATION	0ULL

/*
 * we can actually store much bigger names, but lets not confuse the rest
 * of linux
 */
#define APFS_NAME_LEN 255

/*
 * Theoretical limit is larger, but we keep this down to a sane
 * value. That should limit greatly the possibility of collisions on
 * inode ref items.
 */
#define APFS_LINK_MAX 65535U

#define APFS_EMPTY_DIR_SIZE 0

/* ioprio of readahead is set to idle */
#define APFS_IOPRIO_READA (IOPRIO_PRIO_VALUE(IOPRIO_CLASS_IDLE, 0))

#define APFS_DIRTY_METADATA_THRESH	SZ_32M

/*
 * Use large batch size to reduce overhead of metadata updates.  On the reader
 * side, we only read it when we are close to ENOSPC and the read overhead is
 * mostly related to the number of CPUs, so it is OK to use arbitrary large
 * value here.
 */
#define APFS_TOTAL_BYTES_PINNED_BATCH	SZ_128M

#define APFS_MAX_EXTENT_SIZE SZ_128M

/*
 * Deltas are an effective way to populate global statistics.  Give macro names
 * to make it clear what we're doing.  An example is discard_extents in
 * apfs_free_space_ctl.
 */
#define APFS_STAT_NR_ENTRIES	2
#define APFS_STAT_CURR		0
#define APFS_STAT_PREV		1

/*
 * Count how many APFS_MAX_EXTENT_SIZE cover the @size
 */
static inline u32 count_max_extents(u64 size)
{
	return div_u64(size + APFS_MAX_EXTENT_SIZE - 1, APFS_MAX_EXTENT_SIZE);
}

static inline unsigned long apfs_chunk_item_size(int num_stripes)
{
	BUG_ON(num_stripes == 0);
	return sizeof(struct apfs_chunk) +
		sizeof(struct apfs_stripe) * (num_stripes - 1);
}

/*
 * Runtime (in-memory) states of filesystem
 */
enum {
	/* Global indicator of serious filesystem errors */
	APFS_FS_STATE_ERROR,
	/*
	 * Filesystem is being remounted, allow to skip some operations, like
	 * defrag
	 */
	APFS_FS_STATE_REMOUNTING,
	/* Filesystem in RO mode */
	APFS_FS_STATE_RO,
	/* Track if a transaction abort has been reported on this filesystem */
	APFS_FS_STATE_TRANS_ABORTED,
	/*
	 * Bio operations should be blocked on this filesystem because a source
	 * or target device is being destroyed as part of a device replace
	 */
	APFS_FS_STATE_DEV_REPLACING,
	/* The apfs_fs_info created for self-tests */
	APFS_FS_STATE_DUMMY_FS_INFO,
};

#define APFS_BACKREF_REV_MAX		256
#define APFS_BACKREF_REV_SHIFT		56
#define APFS_BACKREF_REV_MASK		(((u64)APFS_BACKREF_REV_MAX - 1) << \
					 APFS_BACKREF_REV_SHIFT)

#define APFS_OLD_BACKREF_REV		0
#define APFS_MIXED_BACKREF_REV		1

/*
 * every tree block (leaf or node) starts with this header.
 */
struct apfs_header {
	/* these first four must match the super block */
	u8 csum[APFS_CSUM_SIZE];
	u8 fsid[APFS_FSID_SIZE]; /* FS specific uuid */
	__le64 bytenr; /* which block this node is supposed to live in */
	__le64 flags;

	/* allowed to be different from the super from here on down */
	u8 chunk_tree_uuid[APFS_UUID_SIZE];
	__le64 generation;
	__le64 owner;
	__le32 nritems;
	u8 level;
} __attribute__ ((__packed__));

/*
 * this is a very generous portion of the super block, giving us
 * room to translate 14 chunks with 3 stripes each.
 */
#define APFS_SYSTEM_CHUNK_ARRAY_SIZE 2048

/*
 * just in case we somehow lose the roots and are not able to mount,
 * we store an array of the roots from previous transactions
 * in the super.
 */
#define APFS_NUM_BACKUP_ROOTS 4
struct apfs_root_backup {
	__le64 tree_root;
	__le64 tree_root_gen;

	__le64 chunk_root;
	__le64 chunk_root_gen;

	__le64 extent_root;
	__le64 extent_root_gen;

	__le64 fs_root;
	__le64 fs_root_gen;

	__le64 dev_root;
	__le64 dev_root_gen;

	__le64 csum_root;
	__le64 csum_root_gen;

	__le64 total_bytes;
	__le64 bytes_used;
	__le64 num_devices;
	/* future */
	__le64 unused_64[4];

	u8 tree_root_level;
	u8 chunk_root_level;
	u8 extent_root_level;
	u8 fs_root_level;
	u8 dev_root_level;
	u8 csum_root_level;
	/* future and to align */
	u8 unused_8[10];
} __attribute__ ((__packed__));

/*
 * the super block basically lists the main trees of the FS
 * it currently lacks any block count etc etc
 */
struct apfs_super_block {
	/* the first 4 fields must match struct apfs_header */
	u8 csum[APFS_CSUM_SIZE];
	/* FS specific UUID, visible to user */
	u8 fsid[APFS_FSID_SIZE];
	__le64 bytenr; /* this block number */
	__le64 flags;

	/* allowed to be different from the apfs_header from here own down */
	__le64 magic;
	__le64 generation;
	__le64 root;
	__le64 chunk_root;
	__le64 log_root;

	/* this will help find the new super based on the log root */
	__le64 log_root_transid;
	__le64 total_bytes;
	__le64 bytes_used;
	__le64 root_dir_objectid;
	__le64 num_devices;
	__le32 sectorsize;
	__le32 nodesize;
	__le32 __unused_leafsize;
	__le32 stripesize;
	__le32 sys_chunk_array_size;
	__le64 chunk_root_generation;
	__le64 compat_flags;
	__le64 compat_ro_flags;
	__le64 incompat_flags;
	__le16 csum_type;
	u8 root_level;
	u8 chunk_root_level;
	u8 log_root_level;
	struct apfs_dev_item dev_item;

	char label[APFS_LABEL_SIZE];

	__le64 cache_generation;
	__le64 uuid_tree_generation;

	/* the UUID written into btree blocks */
	u8 metadata_uuid[APFS_FSID_SIZE];

	/* future expansion */
	__le64 reserved[28];
	u8 sys_chunk_array[APFS_SYSTEM_CHUNK_ARRAY_SIZE];
	struct apfs_root_backup super_roots[APFS_NUM_BACKUP_ROOTS];
} __attribute__ ((__packed__));

/*
 * Compat flags that we support.  If any incompat flags are set other than the
 * ones specified below then we will fail to mount
 */
#define APFS_FEATURE_COMPAT_SUPP		0ULL
#define APFS_FEATURE_COMPAT_SAFE_SET		0ULL
#define APFS_FEATURE_COMPAT_SAFE_CLEAR		0ULL

#define APFS_FEATURE_COMPAT_RO_SUPP			\
	(APFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE |	\
	 APFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE_VALID)

#define APFS_FEATURE_COMPAT_RO_SAFE_SET	0ULL
#define APFS_FEATURE_COMPAT_RO_SAFE_CLEAR	0ULL

#define APFS_FEATURE_INCOMPAT_SUPP			\
	(APFS_FEATURE_INCOMPAT_MIXED_BACKREF |		\
	 APFS_FEATURE_INCOMPAT_DEFAULT_SUBVOL |	\
	 APFS_FEATURE_INCOMPAT_MIXED_GROUPS |		\
	 APFS_FEATURE_INCOMPAT_BIG_METADATA |		\
	 APFS_FEATURE_INCOMPAT_COMPRESS_LZO |		\
	 APFS_FEATURE_INCOMPAT_COMPRESS_ZSTD |		\
	 APFS_FEATURE_INCOMPAT_COMPRESS_LZVN |		\
	 APFS_FEATURE_INCOMPAT_RAID56 |		\
	 APFS_FEATURE_INCOMPAT_EXTENDED_IREF |		\
	 APFS_FEATURE_INCOMPAT_SKINNY_METADATA |	\
	 APFS_FEATURE_INCOMPAT_NO_HOLES	|	\
	 APFS_FEATURE_INCOMPAT_METADATA_UUID	|	\
	 APFS_FEATURE_INCOMPAT_RAID1C34	|	\
	 APFS_FEATURE_INCOMPAT_ZONED)

#define APFS_FEATURE_INCOMPAT_SAFE_SET			\
	(APFS_FEATURE_INCOMPAT_EXTENDED_IREF)
#define APFS_FEATURE_INCOMPAT_SAFE_CLEAR		0ULL

/*
 * A leaf is full of items. offset and size tell us where to find
 * the item in the leaf (relative to the start of the data area)
 */
struct apfs_item {
	struct apfs_disk_key key;
	__le32 offset;
	__le32 size;
} __attribute__ ((__packed__));

/*
 * leaves have an item area and a data area:
 * [item0, item1....itemN] [free space] [dataN...data1, data0]
 *
 * The data is separate from the items to get the keys closer together
 * during searches.
 */
struct apfs_leaf {
	struct apfs_header header;
	struct apfs_item items[];
} __attribute__ ((__packed__));

/*
 * all non-leaf blocks are nodes, they hold only keys and pointers to
 * other blocks
 */
struct apfs_key_ptr {
	struct apfs_disk_key key;
	__le64 blockptr;
	__le64 generation;
} __attribute__ ((__packed__));

struct apfs_node {
	struct apfs_header header;
	struct apfs_key_ptr ptrs[];
} __attribute__ ((__packed__));

/* Read ahead values for struct apfs_path.reada */
enum {
	READA_NONE,
	READA_BACK,
	READA_FORWARD,
	/*
	 * Similar to READA_FORWARD but unlike it:
	 *
	 * 1) It will trigger readahead even for leaves that are not close to
	 *    each other on disk;
	 * 2) It also triggers readahead for nodes;
	 * 3) During a search, even when a node or leaf is already in memory, it
	 *    will still trigger readahead for other nodes and leaves that follow
	 *    it.
	 *
	 * This is meant to be used only when we know we are iterating over the
	 * entire tree or a very large part of it.
	 */
	READA_FORWARD_ALWAYS,
};

/*
 * apfs_paths remember the path taken from the root down to the leaf.
 * level 0 is always the leaf, and nodes[1...APFS_MAX_LEVEL] will point
 * to any other levels that are present.
 *
 * The slots array records the index of the item or block pointer
 * used while walking the tree.
 */
struct apfs_path {
	struct extent_buffer *nodes[APFS_MAX_LEVEL];
	int slots[APFS_MAX_LEVEL];
	/* if there is real range locking, this locks field will change */
	u8 locks[APFS_MAX_LEVEL];
	u8 reada;
	/* keep some upper locks as we walk down */
	u8 lowest_level;

	/*
	 * set by apfs_split_item, tells search_slot to keep all locks
	 * and to force calls to keep space in the nodes
	 */
	unsigned int search_for_split:1;
	unsigned int keep_locks:1;
	unsigned int skip_locking:1;
	unsigned int search_commit_root:1;
	unsigned int need_commit_sem:1;
	unsigned int skip_release_on_error:1;
	/*
	 * Indicate that new item (apfs_search_slot) is extending already
	 * existing item and ins_len contains only the data size and not item
	 * header (ie. sizeof(struct apfs_item) is not included).
	 */
	unsigned int search_for_extension:1;
};
#define APFS_MAX_EXTENT_ITEM_SIZE(r) ((APFS_LEAF_DATA_SIZE(r->fs_info) >> 4) - \
					sizeof(struct apfs_item))
struct apfs_dev_replace {
	u64 replace_state;	/* see #define above */
	time64_t time_started;	/* seconds since 1-Jan-1970 */
	time64_t time_stopped;	/* seconds since 1-Jan-1970 */
	atomic64_t num_write_errors;
	atomic64_t num_uncorrectable_read_errors;

	u64 cursor_left;
	u64 committed_cursor_left;
	u64 cursor_left_last_write_of_item;
	u64 cursor_right;

	u64 cont_reading_from_srcdev_mode;	/* see #define above */

	int is_valid;
	int item_needs_writeback;
	struct apfs_device *srcdev;
	struct apfs_device *tgtdev;

	struct mutex lock_finishing_cancel_unmount;
	struct rw_semaphore rwsem;

	struct apfs_scrub_progress scrub_progress;

	struct percpu_counter bio_counter;
	wait_queue_head_t replace_wait;
};

/*
 * free clusters are used to claim free space in relatively large chunks,
 * allowing us to do less seeky writes. They are used for all metadata
 * allocations. In ssd_spread mode they are also used for data allocations.
 */
struct apfs_free_cluster {
	spinlock_t lock;
	spinlock_t refill_lock;
	struct rb_root root;

	/* largest extent in this cluster */
	u64 max_size;

	/* first extent starting offset */
	u64 window_start;

	/* We did a full search and couldn't create a cluster */
	bool fragmented;

	struct apfs_block_group *block_group;
	/*
	 * when a cluster is allocated from a block group, we put the
	 * cluster onto a list in the block group so that it can
	 * be freed before the block group is freed.
	 */
	struct list_head block_group_list;
};

enum apfs_caching_type {
	APFS_CACHE_NO,
	APFS_CACHE_STARTED,
	APFS_CACHE_FAST,
	APFS_CACHE_FINISHED,
	APFS_CACHE_ERROR,
};

/*
 * Tree to record all locked full stripes of a RAID5/6 block group
 */
struct apfs_full_stripe_locks_tree {
	struct rb_root root;
	struct mutex lock;
};

/* Discard control. */
/*
 * Async discard uses multiple lists to differentiate the discard filter
 * parameters.  Index 0 is for completely free block groups where we need to
 * ensure the entire block group is trimmed without being lossy.  Indices
 * afterwards represent monotonically decreasing discard filter sizes to
 * prioritize what should be discarded next.
 */
#define APFS_NR_DISCARD_LISTS		3
#define APFS_DISCARD_INDEX_UNUSED	0
#define APFS_DISCARD_INDEX_START	1

struct apfs_discard_ctl {
	struct workqueue_struct *discard_workers;
	struct delayed_work work;
	spinlock_t lock;
	struct apfs_block_group *block_group;
	struct list_head discard_list[APFS_NR_DISCARD_LISTS];
	u64 prev_discard;
	u64 prev_discard_time;
	atomic_t discardable_extents;
	atomic64_t discardable_bytes;
	u64 max_discard_size;
	u64 delay_ms;
	u32 iops_limit;
	u32 kbps_limit;
	u64 discard_extent_bytes;
	u64 discard_bitmap_bytes;
	atomic64_t discard_bytes_saved;
};

enum apfs_orphan_cleanup_state {
	ORPHAN_CLEANUP_STARTED	= 1,
	ORPHAN_CLEANUP_DONE	= 2,
};

void apfs_init_async_reclaim_work(struct apfs_fs_info *fs_info);



/*
 * Block group or device which contains an active swapfile. Used for preventing
 * unsafe operations while a swapfile is active.
 *
 * These are sorted on (ptr, inode) (note that a block group or device can
 * contain more than one swapfile). We compare the pointer values because we
 * don't actually care what the object is, we just need a quick check whether
 * the object exists in the rbtree.
 */
struct apfs_swapfile_pin {
	struct rb_node node;
	void *ptr;
	struct inode *inode;
	/*
	 * If true, ptr points to a struct apfs_block_group. Otherwise, ptr
	 * points to a struct apfs_device.
	 */
	bool is_block_group;
	/*
	 * Only used when 'is_block_group' is true and it is the number of
	 * extents used by a swapfile for this block group ('ptr' field).
	 */
	int bg_extent_count;
};

bool apfs_pinned_by_swapfile(struct apfs_fs_info *fs_info, void *ptr);

enum {
	APFS_FS_BARRIER,
	APFS_FS_CLOSING_START,
	APFS_FS_CLOSING_DONE,
	APFS_FS_LOG_RECOVERING,
	APFS_FS_OPEN,
	APFS_FS_QUOTA_ENABLED,
	APFS_FS_UPDATE_UUID_TREE_GEN,
	APFS_FS_CREATING_FREE_SPACE_TREE,
	APFS_FS_BTREE_ERR,
	APFS_FS_LOG1_ERR,
	APFS_FS_LOG2_ERR,
	APFS_FS_QUOTA_OVERRIDE,
	/* Used to record internally whether fs has been frozen */
	APFS_FS_FROZEN,
	/*
	 * Indicate that balance has been set up from the ioctl and is in the
	 * main phase. The fs_info::balance_ctl is initialized.
	 */
	APFS_FS_BALANCE_RUNNING,

	/*
	 * Indicate that relocation of a chunk has started, it's set per chunk
	 * and is toggled between chunks.
	 * Set, tested and cleared while holding fs_info::send_reloc_lock.
	 */
	APFS_FS_RELOC_RUNNING,

	/* Indicate that the cleaner thread is awake and doing something. */
	APFS_FS_CLEANER_RUNNING,

	/*
	 * The checksumming has an optimized version and is considered fast,
	 * so we don't need to offload checksums to workqueues.
	 */
	APFS_FS_CSUM_IMPL_FAST,

	/* Indicate that the discard workqueue can service discards. */
	APFS_FS_DISCARD_RUNNING,

	/* Indicate that we need to cleanup space cache v1 */
	APFS_FS_CLEANUP_SPACE_CACHE_V1,

	/* Indicate that we can't trust the free space tree for caching yet */
	APFS_FS_FREE_SPACE_TREE_UNTRUSTED,

	/* Indicate whether there are any tree modification log users */
	APFS_FS_TREE_MOD_LOG_USERS,

#if BITS_PER_LONG == 32
	/* Indicate if we have error/warn message printed on 32bit systems */
	APFS_FS_32BIT_ERROR,
	APFS_FS_32BIT_WARN,
#endif
};

/*
 * Exclusive operations (device replace, resize, device add/remove, balance)
 */
enum apfs_exclusive_operation {
	APFS_EXCLOP_NONE,
	APFS_EXCLOP_BALANCE,
	APFS_EXCLOP_DEV_ADD,
	APFS_EXCLOP_DEV_REMOVE,
	APFS_EXCLOP_DEV_REPLACE,
	APFS_EXCLOP_RESIZE,
	APFS_EXCLOP_SWAP_ACTIVATE,
};

struct apfs_nx_info {
	struct apfs_nx_superblock *super_copy;
	struct apfs_fs_info *vol; //dummy
	struct apfs_root *omap_root;
	u64 sb_bytenr;
	u64 block_size;
	u64 block_count;
	int readonly;
	u64 generation;

	int block_size_bits;
	/* temp */
	struct rw_semaphore sem;
	spinlock_t vol_lock;

	struct super_block *sb;
	struct apfs_device *device;

	refcount_t refs;
};

#define APFS_DUMMY_FS_INDEX -1

struct apfs_fs_info {
	u8 chunk_tree_uuid[APFS_UUID_SIZE];
	unsigned long flags;
	struct apfs_root *extent_root;
	struct apfs_root *tree_root;
	struct apfs_root *chunk_root;
	struct apfs_root *dev_root;
	struct apfs_root *fs_root;
	struct apfs_root *csum_root;
	struct apfs_root *quota_root;
	struct apfs_root *uuid_root;
	struct apfs_root *free_space_root;
	struct apfs_root *data_reloc_root;

	/* the log root tree is a directory of all the other log roots */
	struct apfs_root *log_root_tree;

	spinlock_t fs_roots_radix_lock;
	struct radix_tree_root fs_roots_radix;

	/* block group cache stuff */
	spinlock_t block_group_cache_lock;
	u64 first_logical_byte;
	struct rb_root block_group_cache_tree;

	/* keep track of unallocated space */
	atomic64_t free_chunk_space;

	/* Track ranges which are used by log trees blocks/logged data extents */
	struct extent_io_tree excluded_extents;

	/* logical->physical extent mapping */
	struct extent_map_tree mapping_tree;

	/*
	 * block reservation for extent, checksum, root tree and
	 * delayed dir index item
	 */
	struct apfs_block_rsv global_block_rsv;
	/* block reservation for metadata operations */
	struct apfs_block_rsv trans_block_rsv;
	/* block reservation for chunk tree */
	struct apfs_block_rsv chunk_block_rsv;
	/* block reservation for delayed operations */
	struct apfs_block_rsv delayed_block_rsv;
	/* block reservation for delayed refs */
	struct apfs_block_rsv delayed_refs_rsv;

	struct apfs_block_rsv empty_block_rsv;

	u64 generation;
	u64 last_trans_committed;
	u64 avg_delayed_ref_runtime;

	/*
	 * this is updated to the current trans every time a full commit
	 * is required instead of the faster short fsync log commits
	 */
	u64 last_trans_log_full_commit;
	unsigned long mount_opt;
	/*
	 * Track requests for actions that need to be done during transaction
	 * commit (like for some mount options).
	 */
	unsigned long pending_changes;
	unsigned long compress_type:4;
	unsigned int compress_level;
	u32 commit_interval;
	/*
	 * It is a suggestive number, the read side is safe even it gets a
	 * wrong number because we will write out the data into a regular
	 * extent. The write side(mount/remount) is under ->s_umount lock,
	 * so it is also safe.
	 */
	u64 max_inline;

	struct apfs_transaction *running_transaction;
	wait_queue_head_t transaction_throttle;
	wait_queue_head_t transaction_wait;
	wait_queue_head_t transaction_blocked_wait;
	wait_queue_head_t async_submit_wait;

	/*
	 * Used to protect the incompat_flags, compat_flags, compat_ro_flags
	 * when they are updated.
	 *
	 * Because we do not clear the flags for ever, so we needn't use
	 * the lock on the read side.
	 *
	 * We also needn't use the lock when we mount the fs, because
	 * there is no other task which will update the flag.
	 */
	spinlock_t super_lock;
	struct apfs_super_block *super_copy;
	struct apfs_super_block *super_for_commit;
	struct super_block *sb;
	struct inode *btree_inode;
	struct mutex tree_log_mutex;
	struct mutex transaction_kthread_mutex;
	struct mutex cleaner_mutex;
	struct mutex chunk_mutex;

	/*
	 * this is taken to make sure we don't set block groups ro after
	 * the free space cache has been allocated on them
	 */
	struct mutex ro_block_group_mutex;

	/* this is used during read/modify/write to make sure
	 * no two ios are trying to mod the same stripe at the same
	 * time
	 */
	struct apfs_stripe_hash_table *stripe_hash_table;

	/*
	 * this protects the ordered operations list only while we are
	 * processing all of the entries on it.  This way we make
	 * sure the commit code doesn't find the list temporarily empty
	 * because another function happens to be doing non-waiting preflush
	 * before jumping into the main commit.
	 */
	struct mutex ordered_operations_mutex;

	struct rw_semaphore commit_root_sem;

	struct rw_semaphore cleanup_work_sem;

	struct rw_semaphore subvol_sem;

	spinlock_t trans_lock;
	/*
	 * the reloc mutex goes with the trans lock, it is taken
	 * during commit to protect us from the relocation code
	 */
	struct mutex reloc_mutex;

	struct list_head trans_list;
	struct list_head dead_roots;
	struct list_head caching_block_groups;

	spinlock_t delayed_iput_lock;
	struct list_head delayed_iputs;
	atomic_t nr_delayed_iputs;
	wait_queue_head_t delayed_iputs_wait;

	atomic64_t tree_mod_seq;

	/* this protects tree_mod_log and tree_mod_seq_list */
	rwlock_t tree_mod_log_lock;
	struct rb_root tree_mod_log;
	struct list_head tree_mod_seq_list;

	atomic_t async_delalloc_pages;

	/*
	 * this is used to protect the following list -- ordered_roots.
	 */
	spinlock_t ordered_root_lock;

	/*
	 * all fs/file tree roots in which there are data=ordered extents
	 * pending writeback are added into this list.
	 *
	 * these can span multiple transactions and basically include
	 * every dirty data page that isn't from nodatacow
	 */
	struct list_head ordered_roots;

	struct mutex delalloc_root_mutex;
	spinlock_t delalloc_root_lock;
	/* all fs/file tree roots that have delalloc inodes. */
	struct list_head delalloc_roots;

	/*
	 * there is a pool of worker threads for checksumming during writes
	 * and a pool for checksumming after reads.  This is because readers
	 * can run with FS locks held, and the writers may be waiting for
	 * those locks.  We don't want ordering in the pending list to cause
	 * deadlocks, and so the two are serviced separately.
	 *
	 * A third pool does submit_bio to avoid deadlocking with the other
	 * two
	 */
	struct apfs_workqueue *workers;
	struct apfs_workqueue *delalloc_workers;
	struct apfs_workqueue *flush_workers;
	struct apfs_workqueue *endio_workers;
	struct apfs_workqueue *endio_meta_workers;
	struct apfs_workqueue *endio_raid56_workers;
	struct apfs_workqueue *rmw_workers;
	struct apfs_workqueue *endio_meta_write_workers;
	struct apfs_workqueue *endio_write_workers;
	struct apfs_workqueue *endio_freespace_worker;
	struct apfs_workqueue *caching_workers;
	struct apfs_workqueue *readahead_workers;

	/*
	 * fixup workers take dirty pages that didn't properly go through
	 * the cow mechanism and make them safe to write.  It happens
	 * for the sys_munmap function call path
	 */
	struct apfs_workqueue *fixup_workers;
	struct apfs_workqueue *delayed_workers;

	struct task_struct *transaction_kthread;
	struct task_struct *cleaner_kthread;
	u32 thread_pool_size;

	struct kobject *space_info_kobj;
	struct kobject *qgroups_kobj;

	/* used to keep from writing metadata until there is a nice batch */
	struct percpu_counter dirty_metadata_bytes;
	struct percpu_counter delalloc_bytes;
	struct percpu_counter ordered_bytes;
	s32 dirty_metadata_batch;
	s32 delalloc_batch;

	struct list_head dirty_cowonly_roots;

	struct apfs_fs_devices *fs_devices;

	/*
	 * The space_info list is effectively read only after initial
	 * setup.  It is populated at mount time and cleaned up after
	 * all block groups are removed.  RCU is used to protect it.
	 */
	struct list_head space_info;

	struct apfs_space_info *data_sinfo;

	struct reloc_control *reloc_ctl;

	/* data_alloc_cluster is only used in ssd_spread mode */
	struct apfs_free_cluster data_alloc_cluster;

	/* all metadata allocations go through this cluster */
	struct apfs_free_cluster meta_alloc_cluster;

	/* auto defrag inodes go here */
	spinlock_t defrag_inodes_lock;
	struct rb_root defrag_inodes;
	atomic_t defrag_running;

	/* Used to protect avail_{data, metadata, system}_alloc_bits */
	seqlock_t profiles_lock;
	/*
	 * these three are in extended format (availability of single
	 * chunks is denoted by APFS_AVAIL_ALLOC_BIT_SINGLE bit, other
	 * types are denoted by corresponding APFS_BLOCK_GROUP_* bits)
	 */
	u64 avail_data_alloc_bits;
	u64 avail_metadata_alloc_bits;
	u64 avail_system_alloc_bits;

	/* restriper state */
	spinlock_t balance_lock;
	struct mutex balance_mutex;
	atomic_t balance_pause_req;
	atomic_t balance_cancel_req;
	struct apfs_balance_control *balance_ctl;
	wait_queue_head_t balance_wait_q;

	/* Cancellation requests for chunk relocation */
	atomic_t reloc_cancel_req;

	u32 data_chunk_allocations;
	u32 metadata_ratio;

	void *bdev_holder;

	/* private scrub information */
	struct mutex scrub_lock;
	atomic_t scrubs_running;
	atomic_t scrub_pause_req;
	atomic_t scrubs_paused;
	atomic_t scrub_cancel_req;
	wait_queue_head_t scrub_pause_wait;
	/*
	 * The worker pointers are NULL iff the refcount is 0, ie. scrub is not
	 * running.
	 */
	refcount_t scrub_workers_refcnt;
	struct apfs_workqueue *scrub_workers;
	struct apfs_workqueue *scrub_wr_completion_workers;
	struct apfs_workqueue *scrub_parity_workers;

	struct apfs_discard_ctl discard_ctl;

#ifdef CONFIG_APFS_FS_CHECK_INTEGRITY
	u32 check_integrity_print_mask;
#endif
	/* is qgroup tracking in a consistent state? */
	u64 qgroup_flags;

	/* holds configuration and tracking. Protected by qgroup_lock */
	struct rb_root qgroup_tree;
	spinlock_t qgroup_lock;

	/*
	 * used to avoid frequently calling ulist_alloc()/ulist_free()
	 * when doing qgroup accounting, it must be protected by qgroup_lock.
	 */
	struct ulist *qgroup_ulist;

	/*
	 * Protect user change for quota operations. If a transaction is needed,
	 * it must be started before locking this lock.
	 */
	struct mutex qgroup_ioctl_lock;

	/* list of dirty qgroups to be written at next commit */
	struct list_head dirty_qgroups;

	/* used by qgroup for an efficient tree traversal */
	u64 qgroup_seq;

	/* qgroup rescan items */
	struct mutex qgroup_rescan_lock; /* protects the progress item */
	struct apfs_key qgroup_rescan_progress;
	struct apfs_workqueue *qgroup_rescan_workers;
	struct completion qgroup_rescan_completion;
	struct apfs_work qgroup_rescan_work;
	bool qgroup_rescan_running;	/* protected by qgroup_rescan_lock */

	/* filesystem state */
	unsigned long fs_state;

	struct apfs_delayed_root *delayed_root;

	/* readahead tree */
	spinlock_t reada_lock;
	struct radix_tree_root reada_tree;

	/* readahead works cnt */
	atomic_t reada_works_cnt;

	/* Extent buffer radix tree */
	spinlock_t buffer_lock;
	/* Entries are eb->start / sectorsize */
	struct radix_tree_root buffer_radix;

	/* next backup root to be overwritten */
	int backup_root_index;

	/* device replace state */
	struct apfs_dev_replace dev_replace;

	struct semaphore uuid_tree_rescan_sem;

	/* Used to reclaim the metadata space in the background. */
	struct work_struct async_reclaim_work;
	struct work_struct async_data_reclaim_work;
	struct work_struct preempt_reclaim_work;

	/* Reclaim partially filled block groups in the background */
	struct work_struct reclaim_bgs_work;
	struct list_head reclaim_bgs;
	int bg_reclaim_threshold;

	spinlock_t unused_bgs_lock;
	struct list_head unused_bgs;
	struct mutex unused_bg_unpin_mutex;
	/* Protect block groups that are going to be deleted */
	struct mutex reclaim_bgs_lock;

	/* Cached block sizes */
	u32 nodesize;
	u32 sectorsize;
	/* ilog2 of sectorsize, use to avoid 64bit division */
	u32 sectorsize_bits;
	u32 csum_size;
	u32 csums_per_leaf;
	u32 stripesize;

	/* Block groups and devices containing active swapfiles. */
	spinlock_t swapfile_pins_lock;
	struct rb_root swapfile_pins;

	struct crypto_shash *csum_shash;

	spinlock_t send_reloc_lock;
	/*
	 * Number of send operations in progress.
	 * Updated while holding fs_info::send_reloc_lock.
	 */
	int send_in_progress;

	/* Type of exclusive operation running, protected by super_lock */
	enum apfs_exclusive_operation exclusive_operation;

	/*
	 * Zone size > 0 when in ZONED mode, otherwise it's used for a check
	 * if the mode is enabled
	 */
	union {
		u64 zone_size;
		u64 zoned;
	};

	/* Max size to emit ZONE_APPEND write command */
	u64 max_zone_append_size;
	struct mutex zoned_meta_io_lock;
	spinlock_t treelog_bg_lock;
	u64 treelog_bg;

#ifdef CONFIG_APFS_FS_REF_VERIFY
	spinlock_t ref_verify_lock;
	struct rb_root block_tree;
#endif

#ifdef CONFIG_APFS_DEBUG
	struct kobject *debug_kobj;
	struct kobject *discard_debug_kobj;
	struct list_head allocated_roots;

	spinlock_t eb_leak_lock;
	struct list_head allocated_ebs;
#endif
	struct apfs_vol_superblock *__super_copy;
	struct apfs_nx_info *nx_info;
	struct apfs_root *omap_root;
	struct apfs_root *root_root;
	struct apfs_root *extref_root;
	struct apfs_root *snap_root;
	struct apfs_root *fext_root;

	int index;
	u64 xid; //xid when mounted

	struct apfs_device *device;
	u32 node_size;
	u32 block_size;
	u32 block_size_bits;

	bool normalization_insensitive;
};

static inline struct apfs_fs_info *apfs_sb(struct super_block *sb)
{
	return sb->s_fs_info;
}

static inline struct apfs_fs_info *APFS_SB(struct super_block *sb)
{
	return sb->s_fs_info;
}

static inline struct apfs_nx_info *APFS_NX(struct super_block *sb)
{
	return sb->s_fs_info;
}

/*
 * The state of apfs root
 */
enum {
	/*
	 * apfs_record_root_in_trans is a multi-step process, and it can race
	 * with the balancing code.   But the race is very small, and only the
	 * first time the root is added to each transaction.  So IN_TRANS_SETUP
	 * is used to tell us when more checks are required
	 */
	APFS_ROOT_IN_TRANS_SETUP,

	/*
	 * Set if tree blocks of this root can be shared by other roots.
	 * Only subvolume trees and their reloc trees have this bit set.
	 * Conflicts with TRACK_DIRTY bit.
	 *
	 * This affects two things:
	 *
	 * - How balance works
	 *   For shareable roots, we need to use reloc tree and do path
	 *   replacement for balance, and need various pre/post hooks for
	 *   snapshot creation to handle them.
	 *
	 *   While for non-shareable trees, we just simply do a tree search
	 *   with COW.
	 *
	 * - How dirty roots are tracked
	 *   For shareable roots, apfs_record_root_in_trans() is needed to
	 *   track them, while non-subvolume roots have TRACK_DIRTY bit, they
	 *   don't need to set this manually.
	 */
	APFS_ROOT_SHAREABLE,
	APFS_ROOT_TRACK_DIRTY,
	APFS_ROOT_IN_RADIX,
	APFS_ROOT_ORPHAN_ITEM_INSERTED,
	APFS_ROOT_DEFRAG_RUNNING,
	APFS_ROOT_FORCE_COW,
	APFS_ROOT_MULTI_LOG_TASKS,
	APFS_ROOT_DIRTY,
	APFS_ROOT_DELETING,

	/*
	 * Reloc tree is orphan, only kept here for qgroup delayed subtree scan
	 *
	 * Set for the subvolume tree owning the reloc tree.
	 */
	APFS_ROOT_DEAD_RELOC_TREE,
	/* Mark dead root stored on device whose cleanup needs to be resumed */
	APFS_ROOT_DEAD_TREE,
	/* The root has a log tree. Used for subvolume roots and the tree root. */
	APFS_ROOT_HAS_LOG_TREE,
	/* Qgroup flushing is in progress */
	APFS_ROOT_QGROUP_FLUSHING,
};

/*
 * Record swapped tree blocks of a subvolume tree for delayed subtree trace
 * code. For detail check comment in fs/apfs/qgroup.c.
 */
struct apfs_qgroup_swapped_blocks {
	spinlock_t lock;
	/* RM_EMPTY_ROOT() of above blocks[] */
	bool swapped;
	struct rb_root blocks[APFS_MAX_LEVEL];
};

/*
 * in ram representation of the tree.  extent_root is used for all allocations
 * and for the extent tree extent_root root.
 */
struct apfs_root {
	struct extent_buffer *node;

	struct extent_buffer *commit_root;
	struct apfs_root *log_root;
	struct apfs_root *reloc_root;

	unsigned long state;
	struct apfs_root_item root_item;
	struct apfs_key root_key;
	struct apfs_fs_info *fs_info;
	struct extent_io_tree dirty_log_pages;

	struct mutex objectid_mutex;

	spinlock_t accounting_lock;
	struct apfs_block_rsv *block_rsv;

	struct mutex log_mutex;
	wait_queue_head_t log_writer_wait;
	wait_queue_head_t log_commit_wait[2];
	struct list_head log_ctxs[2];
	/* Used only for log trees of subvolumes, not for the log root tree */
	atomic_t log_writers;
	atomic_t log_commit[2];
	/* Used only for log trees of subvolumes, not for the log root tree */
	atomic_t log_batch;
	int log_transid;
	/* No matter the commit succeeds or not*/
	int log_transid_committed;
	/* Just be updated when the commit succeeds. */
	int last_log_commit;
	pid_t log_start_pid;

	u64 last_trans;

	u32 type;

	u64 free_objectid;

	struct apfs_key defrag_progress;
	struct apfs_key defrag_max;

	/* The dirty list is only used by non-shareable roots */
	struct list_head dirty_list;

	struct list_head root_list;

	spinlock_t log_extents_lock[2];
	struct list_head logged_list[2];

	int orphan_cleanup_state;

	spinlock_t inode_lock;
	/* red-black tree that keeps track of in-memory inodes */
	struct rb_root inode_tree;

	/*
	 * radix tree that keeps track of delayed nodes of every inode,
	 * protected by inode_lock
	 */
	struct radix_tree_root delayed_nodes_tree;
	/*
	 * right now this just gets used so that a root has its own devid
	 * for stat.  It may be used for more later
	 */
	dev_t anon_dev;

	spinlock_t root_item_lock;
	refcount_t refs;

	struct mutex delalloc_mutex;
	spinlock_t delalloc_lock;
	/*
	 * all of the inodes that have delalloc bytes.  It is possible for
	 * this list to be empty even when there is still dirty data=ordered
	 * extents waiting to finish IO.
	 */
	struct list_head delalloc_inodes;
	struct list_head delalloc_root;
	u64 nr_delalloc_inodes;

	struct mutex ordered_extent_mutex;
	/*
	 * this is used by the balancing code to wait for all the pending
	 * ordered extents
	 */
	spinlock_t ordered_extent_lock;

	/*
	 * all of the data=ordered extents pending writeback
	 * these can span multiple transactions and basically include
	 * every dirty data page that isn't from nodatacow
	 */
	struct list_head ordered_extents;
	struct list_head ordered_root;
	u64 nr_ordered_extents;

	/*
	 * Not empty if this subvolume root has gone through tree block swap
	 * (relocation)
	 *
	 * Will be used by reloc_control::dirty_subvol_roots.
	 */
	struct list_head reloc_dirty_list;

	/*
	 * Number of currently running SEND ioctls to prevent
	 * manipulation with the read-only status via SUBVOL_SETFLAGS
	 */
	int send_in_progress;
	/*
	 * Number of currently running deduplication operations that have a
	 * destination inode belonging to this root. Protected by the lock
	 * root_item_lock.
	 */
	int dedupe_in_progress;
	/* For exclusion of snapshot creation and nocow writes */
	struct apfs_drew_lock snapshot_lock;

	atomic_t snapshot_force_cow;

	/* For qgroup metadata reserved space */
	spinlock_t qgroup_meta_rsv_lock;
	u64 qgroup_meta_rsv_pertrans;
	u64 qgroup_meta_rsv_prealloc;
	wait_queue_head_t qgroup_flush_wait;

	/* Number of active swapfiles */
	atomic_t nr_swapfiles;

	/* Record pairs of swapped blocks for qgroup */
	struct apfs_qgroup_swapped_blocks swapped_blocks;

	/* Used only by log trees, when logging csum items */
	struct extent_io_tree log_csum_range;

	struct apfs_root_info *root_info;

	bool is_fsroot;
#ifdef CONFIG_APFS_FS_RUN_SANITY_TESTS
	u64 alloc_bytenr;
#endif

#ifdef CONFIG_APFS_DEBUG
	struct list_head leak_list;
#endif
};

static inline const struct apfs_root_info *
apfs_get_root_info(const struct apfs_root *root)
{
	return  (void *)(root->node->len - sizeof(struct apfs_root_info));
}

/*
 * Structure that conveys information about an extent that is going to replace
 * all the extents in a file range.
 */
struct apfs_replace_extent_info {
	u64 disk_offset;
	u64 disk_len;
	u64 data_offset;
	u64 data_len;
	u64 file_offset;
	/* Pointer to a file extent item of type regular or prealloc. */
	char *extent_buf;
	/*
	 * Set to true when attempting to replace a file range with a new extent
	 * described by this structure, set to false when attempting to clone an
	 * existing extent into a file range.
	 */
	bool is_new_extent;
	/* Meaningful only if is_new_extent is true. */
	int qgroup_reserved;
	/*
	 * Meaningful only if is_new_extent is true.
	 * Used to track how many extent items we have already inserted in a
	 * subvolume tree that refer to the extent described by this structure,
	 * so that we know when to create a new delayed ref or update an existing
	 * one.
	 */
	int insertions;
};

/* Arguments for apfs_drop_extents() */
struct apfs_drop_extents_args {
	/* Input parameters */

	/*
	 * If NULL, apfs_drop_extents() will allocate and free its own path.
	 * If 'replace_extent' is true, this must not be NULL. Also the path
	 * is always released except if 'replace_extent' is true and
	 * apfs_drop_extents() sets 'extent_inserted' to true, in which case
	 * the path is kept locked.
	 */
	struct apfs_path *path;
	/* Start offset of the range to drop extents from */
	u64 start;
	/* End (exclusive, last byte + 1) of the range to drop extents from */
	u64 end;
	/* If true drop all the extent maps in the range */
	bool drop_cache;
	/*
	 * If true it means we want to insert a new extent after dropping all
	 * the extents in the range. If this is true, the 'extent_item_size'
	 * parameter must be set as well and the 'extent_inserted' field will
	 * be set to true by apfs_drop_extents() if it could insert the new
	 * extent.
	 * Note: when this is set to true the path must not be NULL.
	 */
	bool replace_extent;
	/*
	 * Used if 'replace_extent' is true. Size of the file extent item to
	 * insert after dropping all existing extents in the range
	 */
	u32 extent_item_size;

	/* Output parameters */

	/*
	 * Set to the minimum between the input parameter 'end' and the end
	 * (exclusive, last byte + 1) of the last dropped extent. This is always
	 * set even if apfs_drop_extents() returns an error.
	 */
	u64 drop_end;
	/*
	 * The number of allocated bytes found in the range. This can be smaller
	 * than the range's length when there are holes in the range.
	 */
	u64 bytes_found;
	/*
	 * Only set if 'replace_extent' is true. Set to true if we were able
	 * to insert a replacement extent after dropping all extents in the
	 * range, otherwise set to false by apfs_drop_extents().
	 * Also, if apfs_drop_extents() has set this to true it means it
	 * returned with the path locked, otherwise if it has set this to
	 * false it has returned with the path released.
	 */
	bool extent_inserted;
};

struct apfs_file_private {
	void *filldir_buf;
};


static inline u32 APFS_LEAF_DATA_SIZE(const struct apfs_fs_info *info)
{

	return info->nodesize - sizeof(struct apfs_node_header);
}

#define APFS_LEAF_DATA_OFFSET		offsetof(struct apfs_leaf, items)

static inline u32 APFS_MAX_ITEM_SIZE(const struct apfs_fs_info *info)
{
	return APFS_LEAF_DATA_SIZE(info) - sizeof(struct apfs_item);
}

static inline u32 APFS_NODEPTRS_PER_BLOCK(const struct apfs_fs_info *info)
{
	return APFS_LEAF_DATA_SIZE(info) / sizeof(struct apfs_key_ptr);
}

#define APFS_FILE_EXTENT_INLINE_DATA_START		\
		(offsetof(struct apfs_file_extent_item, disk_bytenr))
static inline u32 APFS_MAX_INLINE_DATA_SIZE(const struct apfs_fs_info *info)
{
	return APFS_MAX_ITEM_SIZE(info) -
	       APFS_FILE_EXTENT_INLINE_DATA_START;
}

static inline u32 APFS_MAX_XATTR_SIZE(const struct apfs_fs_info *info)
{
	return APFS_MAX_ITEM_SIZE(info) - sizeof(struct apfs_dir_item);
}

/*
 * Flags for mount options.
 *
 * Note: don't forget to add new options to apfs_show_options()
 */
enum {
	APFS_MOUNT_NODATASUM			= (1UL << 0),
	APFS_MOUNT_NODATACOW			= (1UL << 1),
	APFS_MOUNT_NOBARRIER			= (1UL << 2),
	APFS_MOUNT_SSD				= (1UL << 3),
	APFS_MOUNT_DEGRADED			= (1UL << 4),
	APFS_MOUNT_COMPRESS			= (1UL << 5),
	APFS_MOUNT_NOTREELOG   		= (1UL << 6),
	APFS_MOUNT_FLUSHONCOMMIT		= (1UL << 7),
	APFS_MOUNT_SSD_SPREAD			= (1UL << 8),
	APFS_MOUNT_NOSSD			= (1UL << 9),
	APFS_MOUNT_DISCARD_SYNC		= (1UL << 10),
	APFS_MOUNT_FORCE_COMPRESS      	= (1UL << 11),
	APFS_MOUNT_SPACE_CACHE			= (1UL << 12),
	APFS_MOUNT_CLEAR_CACHE			= (1UL << 13),
	APFS_MOUNT_USER_SUBVOL_RM_ALLOWED	= (1UL << 14),
	APFS_MOUNT_ENOSPC_DEBUG		= (1UL << 15),
	APFS_MOUNT_AUTO_DEFRAG			= (1UL << 16),
	APFS_MOUNT_USEBACKUPROOT		= (1UL << 17),
	APFS_MOUNT_SKIP_BALANCE		= (1UL << 18),
	APFS_MOUNT_CHECK_INTEGRITY		= (1UL << 19),
	APFS_MOUNT_CHECK_INTEGRITY_DATA	= (1UL << 20),
	APFS_MOUNT_PANIC_ON_FATAL_ERROR	= (1UL << 21),
	APFS_MOUNT_RESCAN_UUID_TREE		= (1UL << 22),
	APFS_MOUNT_FRAGMENT_DATA		= (1UL << 23),
	APFS_MOUNT_FRAGMENT_METADATA		= (1UL << 24),
	APFS_MOUNT_FREE_SPACE_TREE		= (1UL << 25),
	APFS_MOUNT_NOLOGREPLAY			= (1UL << 26),
	APFS_MOUNT_REF_VERIFY			= (1UL << 27),
	APFS_MOUNT_DISCARD_ASYNC		= (1UL << 28),
	APFS_MOUNT_IGNOREBADROOTS		= (1UL << 29),
	APFS_MOUNT_IGNOREDATACSUMS		= (1UL << 30),
};

#define APFS_DEFAULT_COMMIT_INTERVAL	(30)
#define APFS_DEFAULT_MAX_INLINE	(2048)

#define apfs_clear_opt(o, opt)		((o) &= ~APFS_MOUNT_##opt)
#define apfs_set_opt(o, opt)		((o) |= APFS_MOUNT_##opt)
#define apfs_raw_test_opt(o, opt)	((o) & APFS_MOUNT_##opt)
#define apfs_test_opt(fs_info, opt)	((fs_info)->mount_opt & \
					 APFS_MOUNT_##opt)

#define apfs_set_and_info(fs_info, opt, fmt, args...)			\
do {									\
	if (!apfs_test_opt(fs_info, opt))				\
		apfs_info(fs_info, fmt, ##args);			\
	apfs_set_opt(fs_info->mount_opt, opt);				\
} while (0)

#define apfs_clear_and_info(fs_info, opt, fmt, args...)		\
do {									\
	if (apfs_test_opt(fs_info, opt))				\
		apfs_info(fs_info, fmt, ##args);			\
	apfs_clear_opt(fs_info->mount_opt, opt);			\
} while (0)

/*
 * Requests for changes that need to be done during transaction commit.
 *
 * Internal mount options that are used for special handling of the real
 * mount options (eg. cannot be set during remount and have to be set during
 * transaction commit)
 */

#define APFS_PENDING_COMMIT			(0)

#define apfs_test_pending(info, opt)	\
	test_bit(APFS_PENDING_##opt, &(info)->pending_changes)
#define apfs_set_pending(info, opt)	\
	set_bit(APFS_PENDING_##opt, &(info)->pending_changes)
#define apfs_clear_pending(info, opt)	\
	clear_bit(APFS_PENDING_##opt, &(info)->pending_changes)

/*
 * Helpers for setting pending mount option changes.
 *
 * Expects corresponding macros
 * APFS_PENDING_SET_ and CLEAR_ + short mount option name
 */
#define apfs_set_pending_and_info(info, opt, fmt, args...)            \
do {                                                                   \
       if (!apfs_raw_test_opt((info)->mount_opt, opt)) {              \
               apfs_info((info), fmt, ##args);                        \
               apfs_set_pending((info), SET_##opt);                   \
               apfs_clear_pending((info), CLEAR_##opt);               \
       }                                                               \
} while(0)

#define apfs_clear_pending_and_info(info, opt, fmt, args...)          \
do {                                                                   \
       if (apfs_raw_test_opt((info)->mount_opt, opt)) {               \
               apfs_info((info), fmt, ##args);                        \
               apfs_set_pending((info), CLEAR_##opt);                 \
               apfs_clear_pending((info), SET_##opt);                 \
       }                                                               \
} while(0)

/*
 * Inode flags
 */
#define APFS_INODE_NODATASUM		(1 << 0)
#define APFS_INODE_NODATACOW		(1 << 1)
#define APFS_INODE_READONLY		(1 << 2)
#define APFS_INODE_NOCOMPRESS		(1 << 3)
#define APFS_INODE_PREALLOC		(1 << 4)
#define APFS_INODE_SYNC		(1 << 5)
#define APFS_INODE_IMMUTABLE		(1 << 6)
#define APFS_INODE_APPEND		(1 << 7)
#define APFS_INODE_NODUMP		(1 << 8)
#define APFS_INODE_NOATIME		(1 << 9)
#define APFS_INODE_DIRSYNC		(1 << 10)
#define APFS_INODE_COMPRESS		(1 << 11)

#define APFS_INODE_ROOT_ITEM_INIT	(1 << 31)

#define APFS_INODE_FLAG_MASK						\
	(APFS_INODE_NODATASUM |					\
	 APFS_INODE_NODATACOW |					\
	 APFS_INODE_READONLY |						\
	 APFS_INODE_NOCOMPRESS |					\
	 APFS_INODE_PREALLOC |						\
	 APFS_INODE_SYNC |						\
	 APFS_INODE_IMMUTABLE |					\
	 APFS_INODE_APPEND |						\
	 APFS_INODE_NODUMP |						\
	 APFS_INODE_NOATIME |						\
	 APFS_INODE_DIRSYNC |						\
	 APFS_INODE_COMPRESS |						\
	 APFS_INODE_ROOT_ITEM_INIT)



static inline u64 apfs_device_total_bytes(const struct extent_buffer *eb,
					   struct apfs_dev_item *s)
{
	BUILD_BUG_ON(sizeof(u64) !=
		     sizeof(((struct apfs_dev_item *)0))->total_bytes);
	return apfs_get_64(eb, s, offsetof(struct apfs_dev_item,
					    total_bytes));
}
static inline void apfs_set_device_total_bytes(const struct extent_buffer *eb,
						struct apfs_dev_item *s,
						u64 val)
{
	BUILD_BUG_ON(sizeof(u64) !=
		     sizeof(((struct apfs_dev_item *)0))->total_bytes);
	WARN_ON(!IS_ALIGNED(val, eb->fs_info->sectorsize));
	apfs_set_64(eb, s, offsetof(struct apfs_dev_item, total_bytes), val);
}


APFS_SETGET_FUNCS(device_type, struct apfs_dev_item, type, 64);
APFS_SETGET_FUNCS(device_bytes_used, struct apfs_dev_item, bytes_used, 64);
APFS_SETGET_FUNCS(device_io_align, struct apfs_dev_item, io_align, 32);
APFS_SETGET_FUNCS(device_io_width, struct apfs_dev_item, io_width, 32);
APFS_SETGET_FUNCS(device_start_offset, struct apfs_dev_item,
		   start_offset, 64);
APFS_SETGET_FUNCS(device_sector_size, struct apfs_dev_item, sector_size, 32);
APFS_SETGET_FUNCS(device_id, struct apfs_dev_item, devid, 64);
APFS_SETGET_FUNCS(device_group, struct apfs_dev_item, dev_group, 32);
APFS_SETGET_FUNCS(device_seek_speed, struct apfs_dev_item, seek_speed, 8);
APFS_SETGET_FUNCS(device_bandwidth, struct apfs_dev_item, bandwidth, 8);
APFS_SETGET_FUNCS(device_generation, struct apfs_dev_item, generation, 64);

APFS_SETGET_STACK_FUNCS(stack_device_type, struct apfs_dev_item, type, 64);
APFS_SETGET_STACK_FUNCS(stack_device_total_bytes, struct apfs_dev_item,
			 total_bytes, 64);
APFS_SETGET_STACK_FUNCS(stack_device_bytes_used, struct apfs_dev_item,
			 bytes_used, 64);
APFS_SETGET_STACK_FUNCS(stack_device_io_align, struct apfs_dev_item,
			 io_align, 32);
APFS_SETGET_STACK_FUNCS(stack_device_io_width, struct apfs_dev_item,
			 io_width, 32);
APFS_SETGET_STACK_FUNCS(stack_device_sector_size, struct apfs_dev_item,
			 sector_size, 32);
APFS_SETGET_STACK_FUNCS(stack_device_id, struct apfs_dev_item, devid, 64);
APFS_SETGET_STACK_FUNCS(stack_device_group, struct apfs_dev_item,
			 dev_group, 32);
APFS_SETGET_STACK_FUNCS(stack_device_seek_speed, struct apfs_dev_item,
			 seek_speed, 8);
APFS_SETGET_STACK_FUNCS(stack_device_bandwidth, struct apfs_dev_item,
			 bandwidth, 8);
APFS_SETGET_STACK_FUNCS(stack_device_generation, struct apfs_dev_item,
			 generation, 64);

static inline unsigned long apfs_device_uuid(struct apfs_dev_item *d)
{
	return (unsigned long)d + offsetof(struct apfs_dev_item, uuid);
}

static inline unsigned long apfs_device_fsid(struct apfs_dev_item *d)
{
	return (unsigned long)d + offsetof(struct apfs_dev_item, fsid);
}

APFS_SETGET_FUNCS(chunk_length, struct apfs_chunk, length, 64);
APFS_SETGET_FUNCS(chunk_owner, struct apfs_chunk, owner, 64);
APFS_SETGET_FUNCS(chunk_stripe_len, struct apfs_chunk, stripe_len, 64);
APFS_SETGET_FUNCS(chunk_io_align, struct apfs_chunk, io_align, 32);
APFS_SETGET_FUNCS(chunk_io_width, struct apfs_chunk, io_width, 32);
APFS_SETGET_FUNCS(chunk_sector_size, struct apfs_chunk, sector_size, 32);
APFS_SETGET_FUNCS(chunk_type, struct apfs_chunk, type, 64);
APFS_SETGET_FUNCS(chunk_num_stripes, struct apfs_chunk, num_stripes, 16);
APFS_SETGET_FUNCS(chunk_sub_stripes, struct apfs_chunk, sub_stripes, 16);
APFS_SETGET_FUNCS(stripe_devid, struct apfs_stripe, devid, 64);
APFS_SETGET_FUNCS(stripe_offset, struct apfs_stripe, offset, 64);

static inline char *apfs_stripe_dev_uuid(struct apfs_stripe *s)
{
	return (char *)s + offsetof(struct apfs_stripe, dev_uuid);
}

APFS_SETGET_STACK_FUNCS(stack_chunk_length, struct apfs_chunk, length, 64);
APFS_SETGET_STACK_FUNCS(stack_chunk_owner, struct apfs_chunk, owner, 64);
APFS_SETGET_STACK_FUNCS(stack_chunk_stripe_len, struct apfs_chunk,
			 stripe_len, 64);
APFS_SETGET_STACK_FUNCS(stack_chunk_io_align, struct apfs_chunk,
			 io_align, 32);
APFS_SETGET_STACK_FUNCS(stack_chunk_io_width, struct apfs_chunk,
			 io_width, 32);
APFS_SETGET_STACK_FUNCS(stack_chunk_sector_size, struct apfs_chunk,
			 sector_size, 32);
APFS_SETGET_STACK_FUNCS(stack_chunk_type, struct apfs_chunk, type, 64);
APFS_SETGET_STACK_FUNCS(stack_chunk_num_stripes, struct apfs_chunk,
			 num_stripes, 16);
APFS_SETGET_STACK_FUNCS(stack_chunk_sub_stripes, struct apfs_chunk,
			 sub_stripes, 16);
APFS_SETGET_STACK_FUNCS(stack_stripe_devid, struct apfs_stripe, devid, 64);
APFS_SETGET_STACK_FUNCS(stack_stripe_offset, struct apfs_stripe, offset, 64);

static inline struct apfs_stripe *apfs_stripe_nr(struct apfs_chunk *c,
						   int nr)
{
	unsigned long offset = (unsigned long)c;
	offset += offsetof(struct apfs_chunk, stripe);
	offset += nr * sizeof(struct apfs_stripe);
	return (struct apfs_stripe *)offset;
}

static inline char *apfs_stripe_dev_uuid_nr(struct apfs_chunk *c, int nr)
{
	return apfs_stripe_dev_uuid(apfs_stripe_nr(c, nr));
}

static inline u64 apfs_stripe_offset_nr(const struct extent_buffer *eb,
					 struct apfs_chunk *c, int nr)
{
	return apfs_stripe_offset(eb, apfs_stripe_nr(c, nr));
}

static inline u64 apfs_stripe_devid_nr(const struct extent_buffer *eb,
					 struct apfs_chunk *c, int nr)
{
	return apfs_stripe_devid(eb, apfs_stripe_nr(c, nr));
}

/* struct apfs_block_group_item */
APFS_SETGET_STACK_FUNCS(stack_block_group_used, struct apfs_block_group_item,
			 used, 64);
APFS_SETGET_FUNCS(block_group_used, struct apfs_block_group_item,
			 used, 64);
APFS_SETGET_STACK_FUNCS(stack_block_group_chunk_objectid,
			struct apfs_block_group_item, chunk_objectid, 64);

APFS_SETGET_FUNCS(block_group_chunk_objectid,
		   struct apfs_block_group_item, chunk_objectid, 64);
APFS_SETGET_FUNCS(block_group_flags,
		   struct apfs_block_group_item, flags, 64);
APFS_SETGET_STACK_FUNCS(stack_block_group_flags,
			struct apfs_block_group_item, flags, 64);

/* struct apfs_free_space_info */
APFS_SETGET_FUNCS(free_space_extent_count, struct apfs_free_space_info,
		   extent_count, 32);
APFS_SETGET_FUNCS(free_space_flags, struct apfs_free_space_info, flags, 32);

/* struct apfs_inode_ref */
APFS_SETGET_FUNCS(inode_ref_name_len, struct apfs_inode_ref, name_len, 16);
APFS_SETGET_FUNCS(inode_ref_index, struct apfs_inode_ref, index, 64);

/* struct apfs_inode_extref */
APFS_SETGET_FUNCS(inode_extref_parent, struct apfs_inode_extref,
		   parent_objectid, 64);
APFS_SETGET_FUNCS(inode_extref_name_len, struct apfs_inode_extref,
		   name_len, 16);
APFS_SETGET_FUNCS(inode_extref_index, struct apfs_inode_extref, index, 64);

/* struct apfs_inode_item */
APFS_SETGET_FUNCS(inode_generation, struct apfs_inode_item, generation, 64);
APFS_SETGET_FUNCS(inode_sequence, struct apfs_inode_item, sequence, 64);
APFS_SETGET_FUNCS(inode_transid, struct apfs_inode_item, transid, 64);
APFS_SETGET_FUNCS(inode_size, struct apfs_inode_item, size, 64);
APFS_SETGET_FUNCS(inode_nbytes, struct apfs_inode_item, nbytes, 64);
APFS_SETGET_FUNCS(inode_block_group, struct apfs_inode_item, block_group, 64);
APFS_SETGET_FUNCS(inode_nlink, struct apfs_inode_item, nlink, 32);
APFS_SETGET_FUNCS(inode_uid, struct apfs_inode_item, uid, 32);
APFS_SETGET_FUNCS(inode_gid, struct apfs_inode_item, gid, 32);
APFS_SETGET_FUNCS(inode_mode, struct apfs_inode_item, mode, 32);
APFS_SETGET_FUNCS(inode_rdev, struct apfs_inode_item, rdev, 64);
APFS_SETGET_FUNCS(inode_flags, struct apfs_inode_item, flags, 64);
APFS_SETGET_STACK_FUNCS(stack_inode_generation, struct apfs_inode_item,
			 generation, 64);
APFS_SETGET_STACK_FUNCS(stack_inode_sequence, struct apfs_inode_item,
			 sequence, 64);
APFS_SETGET_STACK_FUNCS(stack_inode_transid, struct apfs_inode_item,
			 transid, 64);
APFS_SETGET_STACK_FUNCS(stack_inode_size, struct apfs_inode_item, size, 64);
APFS_SETGET_STACK_FUNCS(stack_inode_nbytes, struct apfs_inode_item,
			 nbytes, 64);
APFS_SETGET_STACK_FUNCS(stack_inode_block_group, struct apfs_inode_item,
			 block_group, 64);
APFS_SETGET_STACK_FUNCS(stack_inode_nlink, struct apfs_inode_item, nlink, 32);
APFS_SETGET_STACK_FUNCS(stack_inode_uid, struct apfs_inode_item, uid, 32);
APFS_SETGET_STACK_FUNCS(stack_inode_gid, struct apfs_inode_item, gid, 32);
APFS_SETGET_STACK_FUNCS(stack_inode_mode, struct apfs_inode_item, mode, 32);
APFS_SETGET_STACK_FUNCS(stack_inode_rdev, struct apfs_inode_item, rdev, 64);
APFS_SETGET_STACK_FUNCS(stack_inode_flags, struct apfs_inode_item, flags, 64);
APFS_SETGET_FUNCS(timespec_sec, struct apfs_timespec, sec, 64);
APFS_SETGET_FUNCS(timespec_nsec, struct apfs_timespec, nsec, 32);
APFS_SETGET_STACK_FUNCS(stack_timespec_sec, struct apfs_timespec, sec, 64);
APFS_SETGET_STACK_FUNCS(stack_timespec_nsec, struct apfs_timespec, nsec, 32);

/* struct apfs_dev_extent */
APFS_SETGET_FUNCS(dev_extent_chunk_tree, struct apfs_dev_extent,
		   chunk_tree, 64);
APFS_SETGET_FUNCS(dev_extent_chunk_objectid, struct apfs_dev_extent,
		   chunk_objectid, 64);
APFS_SETGET_FUNCS(dev_extent_chunk_offset, struct apfs_dev_extent,
		   chunk_offset, 64);
APFS_SETGET_FUNCS(dev_extent_length, struct apfs_dev_extent, length, 64);
APFS_SETGET_FUNCS(extent_refs, struct apfs_extent_item, refs, 64);
APFS_SETGET_FUNCS(extent_generation, struct apfs_extent_item,
		   generation, 64);
APFS_SETGET_FUNCS(extent_flags, struct apfs_extent_item, flags, 64);

APFS_SETGET_FUNCS(tree_block_level, struct apfs_tree_block_info, level, 8);

static inline void apfs_tree_block_key(const struct extent_buffer *eb,
					struct apfs_tree_block_info *item,
					struct apfs_disk_key *key)
{
	read_eb_member(eb, item, struct apfs_tree_block_info, key, key);
}

static inline void apfs_set_tree_block_key(const struct extent_buffer *eb,
					    struct apfs_tree_block_info *item,
					    struct apfs_disk_key *key)
{
	write_eb_member(eb, item, struct apfs_tree_block_info, key, key);
}

APFS_SETGET_FUNCS(extent_data_ref_root, struct apfs_extent_data_ref,
		   root, 64);
APFS_SETGET_FUNCS(extent_data_ref_objectid, struct apfs_extent_data_ref,
		   objectid, 64);
APFS_SETGET_FUNCS(extent_data_ref_offset, struct apfs_extent_data_ref,
		   offset, 64);
APFS_SETGET_FUNCS(extent_data_ref_count, struct apfs_extent_data_ref,
		   count, 32);

APFS_SETGET_FUNCS(shared_data_ref_count, struct apfs_shared_data_ref,
		   count, 32);

APFS_SETGET_FUNCS(extent_inline_ref_type, struct apfs_extent_inline_ref,
		   type, 8);
APFS_SETGET_FUNCS(extent_inline_ref_offset, struct apfs_extent_inline_ref,
		   offset, 64);

static inline u32 apfs_extent_inline_ref_size(int type)
{
	if (type == APFS_TREE_BLOCK_REF_KEY ||
	    type == APFS_SHARED_BLOCK_REF_KEY)
		return sizeof(struct apfs_extent_inline_ref);
	if (type == APFS_SHARED_DATA_REF_KEY)
		return sizeof(struct apfs_shared_data_ref) +
		       sizeof(struct apfs_extent_inline_ref);
	if (type == APFS_EXTENT_DATA_REF_KEY)
		return sizeof(struct apfs_extent_data_ref) +
		       offsetof(struct apfs_extent_inline_ref, offset);
	return 0;
}

/* struct apfs_node */
APFS_SETGET_FUNCS(key_blockptr, struct apfs_key_ptr, blockptr, 64);
APFS_SETGET_FUNCS(key_generation, struct apfs_key_ptr, generation, 64);
APFS_SETGET_STACK_FUNCS(stack_key_blockptr, struct apfs_key_ptr,
			 blockptr, 64);
APFS_SETGET_STACK_FUNCS(stack_key_generation, struct apfs_key_ptr,
			 generation, 64);

static inline u32 apfs_item_offset_nr(const struct extent_buffer *eb, int nr);

static inline enum apfs_storage apfs_obj_stg_type(struct apfs_obj_header *o);

static inline void apfs_set_node_blockptr(const struct extent_buffer *eb,
					   int nr, u64 val)
{
	BUG();
}

static inline u64 apfs_node_ptr_generation(const struct extent_buffer *eb, int nr)
{
	return 0;
}

static inline void apfs_set_node_ptr_generation(const struct extent_buffer *eb,
						 int nr, u64 val)
{
	unsigned long ptr;
	ptr = offsetof(struct apfs_node, ptrs) +
		sizeof(struct apfs_key_ptr) * nr;
	apfs_set_key_generation(eb, (struct apfs_key_ptr *)ptr, val);
}

static inline unsigned long apfs_node_key_ptr_offset(int nr)
{
	return offsetof(struct apfs_node, ptrs) +
		sizeof(struct apfs_key_ptr) * nr;
}

void apfs_node_key(const struct extent_buffer *eb,
		    struct apfs_disk_key *disk_key, int nr);

static inline void apfs_set_node_key(const struct extent_buffer *eb,
				      struct apfs_disk_key *disk_key, int nr)
{
	unsigned long ptr;
	ptr = apfs_node_key_ptr_offset(nr);
	write_eb_member(eb, (struct apfs_key_ptr *)ptr,
		       struct apfs_key_ptr, key, disk_key);
}

/* struct apfs_item */
APFS_SETGET_TOKEN_FUNCS(item_offset, struct apfs_item, offset, 32);
APFS_SETGET_TOKEN_FUNCS(item_size, struct apfs_item, size, 32);
APFS_SETGET_STACK_FUNCS(stack_item_offset, struct apfs_item, offset, 32);
APFS_SETGET_STACK_FUNCS(stack_item_size, struct apfs_item, size, 32);

static inline u32 apfs_item_offset_nr(const struct extent_buffer *eb, int nr);
static inline u32 apfs_item_size_nr(const struct extent_buffer *eb, int nr);

static inline unsigned long apfs_item_nr_offset(const struct extent_buffer *eb,
						int nr)
{
	return apfs_item_offset_nr(eb, nr);
}

static inline u32 apfs_item_offset(const struct extent_buffer *eb,
				   struct apfs_item *item)
{
	return apfs_item_offset_nr(eb, (int)(unsigned long)item);
}

static inline u32 apfs_item_size(const struct extent_buffer *eb,
				 struct apfs_item *item)
{
	return apfs_item_size_nr(eb, (int)(unsigned long)item);
}

static inline void apfs_set_item_offset(const struct extent_buffer *eb,
				       struct apfs_item *item, u32 offset)
{
	BUG();
}

static inline void apfs_set_item_size(const struct extent_buffer *eb,
				     struct apfs_item *item, u32 size)
{
	BUG();
}

static inline struct apfs_item *apfs_item_nr(int nr)
{
	return (struct apfs_item *)(unsigned long)nr;
}

static inline u32 apfs_item_end(const struct extent_buffer *eb,
				struct apfs_item *item)
{
	return apfs_item_offset(eb, item) + apfs_item_size(eb, item);
}

static inline u32 apfs_item_end_nr(const struct extent_buffer *eb, int nr)
{
	return apfs_item_end(eb, apfs_item_nr(nr));
}

static inline bool apfs_fixed_kv_size(const struct extent_buffer *eb)
{
	return apfs_header_flags(eb) & APFS_NODE_FIXED_KV_SIZE;
}

static inline u32 apfs_header_nkeys(const struct extent_buffer *eb);

static inline u16
apfs_header_table_space_offset(const struct extent_buffer *eb);

static inline unsigned long
apfs_item_kv_offset(const struct extent_buffer *eb, int nr)
{
	size_t unit;

	ASSERT(nr < (int)apfs_header_nkeys(eb));

	if (apfs_fixed_kv_size(eb))
		unit = sizeof(struct apfs_disk_fixed_kv);
	else
		unit = sizeof(struct apfs_disk_kv);

	return apfs_header_table_space_offset(eb) +
		offsetof(struct apfs_disk_node, data) + unit * nr;
}

APFS_SETGET_FUNCS(obj_type_impl, struct apfs_obj_header, type, 32);

#define apfs_obj_type(o) (apfs_obj_type_impl(o) & APFS_OBJ_TYPE_MASK)
#define apfs_obj_flags(o) (apfs_obj_type_impl(o) & APFS_OBJ_TYPE_FLAGS_MASK)

static inline enum apfs_storage
apfs_obj_stg_type(struct apfs_obj_header *o)
{
	if ((apfs_stack_obj_type(o) & APFS_OBJ_STG_TYPE_MASK) ==
	    APFS_STG_PHYSICAL)
		return APFS_STG_PHYSICAL;
	if ((apfs_stack_obj_type(o) & APFS_OBJ_STG_TYPE_MASK) ==
	    APFS_STG_EPHEMERAL)
		return APFS_STG_EPHEMERAL;
	return APFS_STG_VIRTUAL;

}

static inline u16
apfs_header_table_space_offset(const struct extent_buffer *eb)
{
	__u16 res;

	read_eb_member(eb, offsetof(struct apfs_node_header, table_space),
		       struct apfs_disk_loc, off, &res);

	return le16_to_cpu(res);
}

static inline u16
apfs_header_toc_offset(const struct extent_buffer *eb)
{
	return apfs_header_table_space_offset(eb);
}

static inline u16
apfs_header_table_space_len(const struct extent_buffer *eb)
{
	__u16 res;

	read_eb_member(eb, offsetof(struct apfs_node_header, table_space),
		       struct apfs_disk_loc, len, &res);

	return le16_to_cpu(res);
}

static inline u16
apfs_header_toc_len(const struct extent_buffer *eb)
{
	return apfs_header_table_space_len(eb);
}

static inline u16
apfs_header_toc_end(const struct extent_buffer *eb)
{
	return sizeof(struct apfs_node_header) +
		apfs_header_toc_offset(eb) + apfs_header_toc_len(eb);
}

static inline u16
apfs_header_free_space_offset(const struct extent_buffer *eb)
{
	__u16 res;

	read_eb_member(eb, offsetof(struct apfs_node_header, free_space),
		       struct apfs_disk_loc, off, &res);
	return le16_to_cpu(res);
}

static inline u16
apfs_header_free_space_len(const struct extent_buffer *eb)
{
	__u16 res;

	read_eb_member(eb, offsetof(struct apfs_node_header, free_space),
		       struct apfs_disk_loc, len, &res);
	return le16_to_cpu(res);
}

static inline u16
apfs_header_free_keylist_offset(const struct extent_buffer *eb)
{
	__u16 res;

	read_eb_member(eb, offsetof(struct apfs_node_header, key_free_list),
		       struct apfs_disk_loc, off, &res);
	return le16_to_cpu(res);
}

static inline u16
apfs_header_free_keylist_len(const struct extent_buffer *eb)
{
	__u16 res;

	read_eb_member(eb, offsetof(struct apfs_node_header, key_free_list),
		       struct apfs_disk_loc, len, &res);
	return le16_to_cpu(res);
}

static inline u16
apfs_header_free_vallist_offset(const struct extent_buffer *eb)
{
	__u16 res;

	read_eb_member(eb, offsetof(struct apfs_node_header, val_free_list),
		       struct apfs_disk_loc, off, &res);
	return le16_to_cpu(res);
}

static inline u16
apfs_header_free_vallist_len(const struct extent_buffer *eb)
{
	__u16 res;

	read_eb_member(eb, offsetof(struct apfs_node_header, val_free_list),
		       struct apfs_disk_loc, len, &res);
	return le16_to_cpu(res);
}

static inline u16 apfs_header_toc_end(const struct extent_buffer *eb);
static inline bool
apfs_is_root_node(const struct extent_buffer *eb)
{
	return apfs_disk_node_flags(eb, 0) & APFS_NODE_ROOT;
}

static inline bool
apfs_is_leaf_node(const struct extent_buffer *eb)
{
	return apfs_disk_node_flags(eb, 0) & APFS_NODE_LEAF;
}

static inline void
apfs_item_kv_loc(const struct extent_buffer *eb, int nr, struct apfs_kv *res)
{

	unsigned int offset;
	unsigned int value_end = eb->len;
	bool fixed = apfs_fixed_kv_size(eb);
	struct apfs_fixed_kv fixed_kv;
	struct apfs_kv kv;
	struct apfs_disk_kv disk_kv;
	struct apfs_disk_fixed_kv disk_fixed_kv;

	offset = apfs_item_kv_offset(eb, nr);

	if (!fixed) {
		read_extent_buffer(eb, &disk_kv, offset, sizeof(disk_kv));
		apfs_disk_kv_to_cpu(&disk_kv, &kv);
		res->k.off = kv.k.off;
		res->v.off = kv.v.off;
		res->k.len = kv.k.len;
		res->v.len = kv.v.len;
	} else {
		read_extent_buffer(eb, &disk_fixed_kv, offset,
				   sizeof(disk_fixed_kv));
		apfs_disk_fixed_kv_to_cpu(&disk_fixed_kv, &fixed_kv);
		res->k.off = fixed_kv.k;
		res->v.off = fixed_kv.v;

		if (!apfs_is_leaf_node(eb))
			res->v.len = APFS_FIXED_VAL_SIZE;
		else
			/* for now, it's only omap nodes */
			res->v.len = sizeof(struct apfs_omap_item);

		res->k.len = APFS_FIXED_KEY_SIZE;
	}

	/*
	 * offset of key is counted from the beginning of the key area to the
	 * beginning of the key
	 */
	res->k.off += apfs_header_toc_end(eb);

	/*
	 *  The offset to a value is counted from the end of the value area to
	 *  the beginning of the value.
	 */
	if (apfs_is_root_node(eb))
		value_end = eb->len - sizeof(struct apfs_root_info);
	res->v.off = value_end - res->v.off;

	BUG_ON(res->k.off > eb->len);
	BUG_ON(res->v.off > eb->len);

	BUG_ON(res->k.off + res->k.len > eb->len);
	BUG_ON(res->v.off + res->v.len > eb->len);
}

static inline unsigned long
apfs_item_key_offset(const struct extent_buffer *eb, int nr)
{
	struct apfs_kv res;

	apfs_item_kv_loc(eb, nr, &res);
	return res.k.off;
}

static inline unsigned long
apfs_item_key_len(const struct extent_buffer *eb, int nr)
{
	struct apfs_kv res;

	apfs_item_kv_loc(eb, nr, &res);
	return res.k.len;
}

static inline unsigned long
apfs_item_val_offset(const struct extent_buffer *eb, int nr)
{
	struct apfs_kv res;

	apfs_item_kv_loc(eb, nr, &res);
	return res.v.off;
}

static inline unsigned long
apfs_item_val_len(const struct extent_buffer *eb, int nr)
{
	struct apfs_kv res;

	apfs_item_kv_loc(eb, nr, &res);
	return res.v.len;
}

static inline u32 apfs_item_offset_nr(const struct extent_buffer *eb, int nr)
{
	return apfs_item_val_offset(eb, nr);
}

static inline u32 apfs_item_size_nr(const struct extent_buffer *eb, int nr)
{
	return apfs_item_val_len(eb, nr);
}

static inline void * apfs_node_data(const struct extent_buffer *eb);

static inline const struct apfs_disk_key *
apfs_item_disk_key(const struct extent_buffer *eb, int nr)
{
	return (const void *)apfs_node_data(eb) + apfs_item_key_offset(eb, nr);
}

static inline void apfs_item_key(const struct extent_buffer *eb,
			   struct apfs_disk_key *disk_key, int nr)
{
	read_extent_buffer(eb, disk_key, apfs_item_key_offset(eb, nr),
			   sizeof(*disk_key));
}

static inline void apfs_set_item_key(struct extent_buffer *eb,
			       struct apfs_disk_key *disk_key, int nr)
{
	BUG();
}

APFS_SETGET_FUNCS(dir_log_end, struct apfs_dir_log_item, end, 64);

/*
 * struct apfs_root_ref
 */
APFS_SETGET_FUNCS(root_ref_dirid, struct apfs_root_ref, dirid, 64);
APFS_SETGET_FUNCS(root_ref_sequence, struct apfs_root_ref, sequence, 64);
APFS_SETGET_FUNCS(root_ref_name_len, struct apfs_root_ref, name_len, 16);

/* struct apfs_dir_item */
APFS_SETGET_FUNCS(dir_data_len, struct apfs_dir_item, data_len, 16);
APFS_SETGET_FUNCS(dir_type, struct apfs_dir_item, type, 8);
APFS_SETGET_FUNCS(dir_name_len, struct apfs_dir_item, name_len, 16);
APFS_SETGET_FUNCS(dir_transid, struct apfs_dir_item, transid, 64);
APFS_SETGET_STACK_FUNCS(stack_dir_type, struct apfs_dir_item, type, 8);
APFS_SETGET_STACK_FUNCS(stack_dir_data_len, struct apfs_dir_item,
			 data_len, 16);
APFS_SETGET_STACK_FUNCS(stack_dir_name_len, struct apfs_dir_item,
			 name_len, 16);
APFS_SETGET_STACK_FUNCS(stack_dir_transid, struct apfs_dir_item,
			 transid, 64);

static inline void apfs_dir_item_key(const struct extent_buffer *eb,
				      const struct apfs_dir_item *item,
				      struct apfs_disk_key *key)
{
	read_eb_member(eb, item, struct apfs_dir_item, location, key);
}

static inline void apfs_set_dir_item_key(struct extent_buffer *eb,
					  struct apfs_dir_item *item,
					  const struct apfs_disk_key *key)
{
	write_eb_member(eb, item, struct apfs_dir_item, location, key);
}

APFS_SETGET_FUNCS(free_space_entries, struct apfs_free_space_header,
		   num_entries, 64);
APFS_SETGET_FUNCS(free_space_bitmaps, struct apfs_free_space_header,
		   num_bitmaps, 64);
APFS_SETGET_FUNCS(free_space_generation, struct apfs_free_space_header,
		   generation, 64);

static inline void apfs_free_space_key(const struct extent_buffer *eb,
					const struct apfs_free_space_header *h,
					struct apfs_disk_key *key)
{
	read_eb_member(eb, h, struct apfs_free_space_header, location, key);
}

static inline void apfs_set_free_space_key(struct extent_buffer *eb,
					    struct apfs_free_space_header *h,
					    const struct apfs_disk_key *key)
{
	write_eb_member(eb, h, struct apfs_free_space_header, location, key);
}



static inline u32
apfs_header_subtype(const struct extent_buffer *eb)
{
	u32 res;

	read_eb_member(eb, 0, struct apfs_obj_header, subtype, &res);
	return res;
}

static inline bool
apfs_is_fs_node(const struct extent_buffer *eb)
{
	return apfs_header_subtype(eb) == APFS_OBJ_TYPE_FSTREE ||
		apfs_header_subtype(eb) == APFS_OBJ_TYPE_REFTREE ||
		apfs_header_subtype(eb) == APFS_OBJ_TYPE_SNAPTREE;
}

/* struct apfs_disk_key */
APFS_SETGET_STACK_FUNCS(disk_key_objectid, struct apfs_disk_key, objectid, 64);
APFS_SETGET_STACK_FUNCS(disk_key_offset, struct apfs_disk_key, offset, 64);
APFS_SETGET_FUNCS(disk_key_id, struct apfs_disk_key, id, 64);

APFS_SETGET_FUNCS(disk_key_namelen1, struct apfs_disk_key, namelen1, 16);
APFS_SETGET_FUNCS(disk_key_namelen_hash, struct apfs_disk_key,
		  namelen_and_hash, 32);

APFS_SETGET_STACK_FUNCS(stack_disk_key_id, struct apfs_disk_key, id, 64);
APFS_SETGET_STACK_FUNCS(stack_disk_key_offset, struct apfs_disk_key, offset, 64);
APFS_SETGET_STACK_FUNCS(stack_disk_key_namelen1, struct apfs_disk_key, namelen1, 16);
APFS_SETGET_STACK_FUNCS(stack_disk_key_namelen_hash, struct apfs_disk_key,
			namelen_and_hash, 32);

static inline int
__apfs_key_members(u8 type)
{
	switch (type) {
		/* id_and_type only */
	case APFS_TYPE_SNAP_METADATA:
	case APFS_TYPE_CRYPTO_STATE:
	case APFS_TYPE_DSTREAM_ID:
	case APFS_TYPE_INODE:
	case APFS_TYPE_EXTENT:
	case APFS_TYPE_SIBLING_MAP:
	case APFS_TYPE_DIR_STATS:
		return 1;
	case APFS_TYPE_FILE_INFO:
	case APFS_TYPE_FILE_EXTENT:
	case APFS_TYPE_SIBLING_LINK:

//	case APFS_TYPE_OMAP:
//	case APFS_TYPE_FEXT_TREE:
//	case APFS_TYPE_SPACE_FREE_QUEUE:
		return 2;
		/* id_and_type, namelen and name, 3 memebers */
	case APFS_TYPE_XATTR:
	case APFS_TYPE_SNAP_NAME:
	case APFS_TYPE_DIR_REC:

		return 3;

		/*
		 * we trust the disk key. So it's for type
		 *  APFS_TYPE_OMAP
		 *  APFS_TYPE_FEXT_TREE
		 *  APFS_TYPE_SPACE_FREE_QUEUE
		 */
	default:
		apfs_err(NULL, "Unknow key type %u", type);
		return 0;
	}
}

static inline int
apfs_key_members(const struct extent_buffer *eb, u8 type)
{
	if (apfs_header_subtype(eb) == APFS_OBJ_TYPE_OMAP)
		return 2;
	if (apfs_header_subtype(eb) == APFS_OBJ_TYPE_SPACEMAN_FREE_QUEUE)
		return 2;
	if (apfs_is_fs_node(eb))
		return __apfs_key_members(type);
	BUG();
	return -1;
}

static inline void
apfs_set_key_len_hash(struct apfs_key *key, u32 hash, u32 namelen)
{
	BUG();
}

static inline u16
apfs_disk_key_drec_namelen(const struct apfs_disk_key *key)
{
	return le32_to_cpu(key->namelen_and_hash) & APFS_DREC_LEN_MASK;
}

static inline u32
apfs_disk_key_hash(const struct apfs_disk_key *key)
{
	return (le32_to_cpu(key->namelen_and_hash) & APFS_DREC_HASH_MASK) >>
		APFS_DREC_HASH_SHIFT;
}

static inline u64
apfs_disk_key_oid(const struct apfs_disk_key *key)
{
	return (le64_to_cpu(key->id) & APFS_FSKEY_ID_MASK);
}

static inline size_t
apfs_obj_keylen(u64 type)
{
	switch (type) {
	case APFS_TYPE_SNAP_METADATA:
		return sizeof(struct apfs_snap_meta_key);
	case APFS_TYPE_EXTENT:
		return sizeof(struct apfs_phys_extent_key);
	case APFS_TYPE_INODE:
		return sizeof(struct apfs_inode_key);
	case APFS_TYPE_XATTR:
		return sizeof(struct apfs_xattr_key);
	case APFS_TYPE_SIBLING_LINK:
		return sizeof(struct apfs_sibling_key);
	case APFS_TYPE_DSTREAM_ID:
		return sizeof(struct apfs_dstream_id_key);
	case APFS_TYPE_CRYPTO_STATE:
		return sizeof(struct apfs_crypto_key);
	case APFS_TYPE_FILE_EXTENT:
		return sizeof(struct apfs_file_extent_key);
	case APFS_TYPE_DIR_REC:
		return sizeof(struct apfs_drec_key);
	case APFS_TYPE_DIR_STATS:
		return sizeof(struct apfs_dir_stats_key);
	case APFS_TYPE_SNAP_NAME:
		return sizeof(struct apfs_snap_name_key);
	case APFS_TYPE_SIBLING_MAP:
		return sizeof(struct apfs_sibling_map_key);
	case APFS_TYPE_FILE_INFO:
		return sizeof(struct apfs_file_info_key);
	case APFS_TYPE_ANY:
	default:
		BUG();
	}
}

static inline size_t
apfs_obj_vallen(u64 type)
{
	switch (type) {
	case APFS_TYPE_SNAP_METADATA:
		return sizeof(struct apfs_snap_meta);
		break;
	case APFS_TYPE_EXTENT:
		return sizeof(struct apfs_phys_extent_item);
		break;
	case APFS_TYPE_INODE:
		return sizeof(struct apfs_inode_item);
		break;
	case APFS_TYPE_XATTR:
		return sizeof(struct apfs_xattr_item);
		break;
	case APFS_TYPE_SIBLING_LINK:
		return sizeof(struct apfs_sibling_val);
		break;
	case APFS_TYPE_DSTREAM_ID:
		return sizeof(struct apfs_dstream_id_val);
		break;
	case APFS_TYPE_CRYPTO_STATE:
		return sizeof(struct apfs_crypto_val);
		break;
	case APFS_TYPE_FILE_EXTENT:
		return sizeof(struct apfs_file_extent_item);
		break;
	case APFS_TYPE_DIR_REC:
		return sizeof(struct apfs_drec_item);
		break;
	case APFS_TYPE_DIR_STATS:
		return sizeof(struct apfs_dir_stats_val);
		break;
	case APFS_TYPE_SNAP_NAME:
		return sizeof(struct apfs_snap_name_val);
		break;
	case APFS_TYPE_SIBLING_MAP:
		return sizeof(struct apfs_sibling_map_val);
		break;
	case APFS_TYPE_FILE_INFO:
		return sizeof(struct apfs_file_info_val);
		break;
	case APFS_TYPE_ANY:
	default:
		BUG();
	}
}

static inline u8
__apfs_disk_key_type(const struct apfs_disk_key *key)
{
	return (APFS_FSKEY_TYPE_MASK & le64_to_cpu(key->id_and_type)) >>
		APFS_FSKEY_TYPE_SHIFT;
}

static inline u8
apfs_disk_key_type(const struct apfs_disk_key *key)
{
	return __apfs_disk_key_type(key);
}

static inline void
apfs_set_disk_key_type(const struct apfs_disk_key *key, u8 val)
{
	BUG();
}

static inline u64 apfs_fskey_id(const struct apfs_key *key)
{
	return key->id & APFS_FSKEY_ID_MASK;
}

static inline void apfs_set_fskey_id(struct apfs_key *key, u64 id)
{
	key->id = id & APFS_FSKEY_ID_MASK;
}

static inline u8 apfs_fskey_type(const struct apfs_key *key)
{
	return (key->id_and_type & APFS_FSKEY_TYPE_MASK) >>
		APFS_FSKEY_TYPE_SHIFT;
}

static inline void apfs_set_fskey_type(struct apfs_key *key, u8 type)
{
	key->id_and_type &= APFS_FSKEY_TYPE_MASK;
	key->id_and_type |= APFS_FSKEY_TYPE_SHIFT << type;
}

static inline int
apfs_disk_key_members(const struct extent_buffer *eb,
		      const struct apfs_disk_key *disk)
{
	return apfs_key_members(eb, __apfs_disk_key_type(disk));
}

static inline bool
apfs_is_case_insensitive(const struct apfs_vol_superblock *sb)
{
	return apfs_volume_super_incompat_features(sb) &
		APFS_INCOMPAT_CASE_INSENSITIVE;
}

static inline bool
apfs_is_normalization_insensitive(const struct apfs_vol_superblock *sb)
{
	if (apfs_is_case_insensitive(sb))
		return true;
	if (apfs_volume_super_incompat_features(sb) &
	    APFS_INCOMPAT_NORMALIZATION_INSENSITIVE)
		return true;

	return false;
}

static inline u16
apfs_disk_key_namelen(const struct extent_buffer *eb,
		      const struct apfs_disk_key *disk)
{
	u8 type = __apfs_disk_key_type(disk);

	switch (type) {
	case APFS_TYPE_XATTR:
	case APFS_TYPE_SNAP_NAME:
		return apfs_stack_disk_key_namelen1(disk);
	case APFS_TYPE_DIR_REC:
		if (!apfs_is_normalization_insensitive(eb->fs_info->__super_copy))
			return apfs_stack_disk_key_namelen1(disk);
		else
			return apfs_disk_key_drec_namelen(disk);
	default:
		break;
	}

	return 0;
}

static inline u16
apfs_item_namelen(struct extent_buffer *eb, int nr)
{
	unsigned long offset = apfs_item_offset_nr(eb, nr);
	struct apfs_disk_key disk;

	read_extent_buffer(eb, &disk, offset, sizeof(disk));
	return apfs_disk_key_namelen(eb, &disk);
}

static inline void *
apfs_node_data(const struct extent_buffer *eb)
{
	ASSERT(eb->len <= PAGE_SIZE);
	return page_address(eb->pages[0]);
}

/*
 * Returns offset of name[0] in the extent buffer.
 * If there is no name, return 0.
 */
static inline void *
apfs_item_name_ptr(const struct extent_buffer *eb,
		   const struct apfs_disk_key *disk)
{
	u8 type = apfs_disk_key_type(disk);
	/* Magic assertion! */
	char *ptr = (void *)disk;

	switch (type) {
	case APFS_TYPE_XATTR:
	case APFS_TYPE_SNAP_NAME:
		return ptr + offsetof(struct apfs_disk_key, name1);
	case APFS_TYPE_DIR_REC:
		if (!apfs_is_normalization_insensitive(eb->fs_info->__super_copy))
			return ptr + offsetof(struct apfs_disk_key, name1);
		else
			return ptr + offsetof(struct apfs_disk_key, name2);
	default:
		BUG();
		break;
	}

	return 0;
}

static inline void
__apfs_disk_key_to_cpu(const struct extent_buffer *eb,
		       const struct apfs_disk_key *disk, struct apfs_key *cpu,
		       bool raw)
{
	int member_num;

	member_num = apfs_disk_key_members(eb, disk);
	memset(cpu, 0, sizeof(*cpu));

	cpu->oid = apfs_disk_key_oid(disk);
	cpu->type = apfs_disk_key_type(disk);

	if (member_num == 1)
		goto out;

	if (member_num == 2) {
		cpu->offset = apfs_disk_key_offset(disk);
		goto out;
	}

	if (cpu->type == APFS_TYPE_DIR_REC &&
	    eb->fs_info->normalization_insensitive) {
		cpu->hash = apfs_disk_key_hash(disk);
		cpu->namelen = apfs_disk_key_drec_namelen(disk);
	} else {
		cpu->namelen = apfs_stack_disk_key_namelen1(disk);
	}

	cpu->name = apfs_item_name_ptr(eb, disk);
out:
	return;
}

static inline void apfs_disk_key_to_cpu(const struct extent_buffer *eb,
					struct apfs_key *cpu,
					const struct apfs_disk_key *disk)
{
	__apfs_disk_key_to_cpu(eb, disk, cpu, false);
}

static inline void apfs_cpu_key_to_disk(const struct extent_buffer *eb,
					struct apfs_disk_key *disk,
					const struct apfs_key *cpu)
{
	return ;
}

static inline void apfs_node_key_to_cpu(const struct extent_buffer *eb,
					 struct apfs_key *key, int nr)
{
	apfs_disk_key_to_cpu(eb, key, apfs_item_disk_key(eb, nr));
}

static inline void apfs_item_key_to_cpu(const struct extent_buffer *eb,
					 struct apfs_key *key, int nr)
{
	apfs_disk_key_to_cpu(eb, key, apfs_item_disk_key(eb, nr));
}

static inline void apfs_dir_item_key_to_cpu(const struct extent_buffer *eb,
					     const struct apfs_dir_item *item,
					     struct apfs_key *key)
{
	return ;
}

/* struct apfs_header */
APFS_SETGET_TOKEN_FUNCS(header_bytenr, struct apfs_header, bytenr, 64);
APFS_SETGET_TOKEN_FUNCS(header_generation, struct apfs_header,
			  generation, 64);
APFS_SETGET_TOKEN_FUNCS(header_owner, struct apfs_header, owner, 64);
APFS_SETGET_TOKEN_FUNCS(header_nritems, struct apfs_header, nritems, 32);
APFS_SETGET_TOKEN_FUNCS(header_flags, struct apfs_header, flags, 64);
APFS_SETGET_TOKEN_FUNCS(header_level, struct apfs_header, level, 8);
APFS_SETGET_STACK_FUNCS(stack_header_generation, struct apfs_header,
			 generation, 64);
APFS_SETGET_STACK_FUNCS(stack_header_owner, struct apfs_header, owner, 64);
APFS_SETGET_STACK_FUNCS(stack_header_nritems, struct apfs_header,
			 nritems, 32);
APFS_SETGET_STACK_FUNCS(stack_header_bytenr, struct apfs_header, bytenr, 64);

static inline u16 apfs_header_flags(const struct extent_buffer *eb);
static inline void apfs_set_header_flags(const struct extent_buffer *eb,
					 u16 flags);
static inline u32 apfs_header_nritems(const struct extent_buffer *eb);
static inline u32 apfs_set_header_nritems(const struct extent_buffer *eb,
					  u32 nritems);
static inline u64
apfs_header_bytenr(const struct extent_buffer *eb)
{
	return eb->start;
}

static inline void
apfs_set_header_bytenr(const struct extent_buffer *eb, u64 bytenr)
{
	return ;
}

static inline int
apfs_header_level(const struct extent_buffer *eb)
{
	u16 level;

	read_eb_member(eb, 0, struct apfs_node_header, level, &level);
	return __le16_to_cpu(level);
}

static inline void
apfs_set_header_level(const struct extent_buffer *eb, u16 level)
{
	write_eb_member(eb, 0, struct apfs_node_header, level, &level);
	return;
}

static inline u64
apfs_header_generation(const struct extent_buffer *eb)
{
	u64 gen;

	read_eb_member(eb, 0, struct apfs_obj_header, xid, &gen);
	return __le64_to_cpu(gen);
}

static inline void
apfs_set_header_generation(const struct extent_buffer *eb, u64 gen)
{
	return;
}

static inline u64
apfs_header_owner(const struct extent_buffer *eb)
{
	return 0;
}

static inline void
apfs_set_header_owner(const struct extent_buffer *eb, u64 owner)
{
	return ;
}

static inline int apfs_header_flag(const struct extent_buffer *eb, u64 flag)
{
	return (apfs_header_flags(eb) & flag) == flag;
}

static inline void apfs_set_header_flag(struct extent_buffer *eb, u64 flag)
{
	u64 flags = apfs_header_flags(eb);
	apfs_set_header_flags(eb, flags | flag);
}

static inline void apfs_clear_header_flag(struct extent_buffer *eb, u64 flag)
{
	u64 flags = apfs_header_flags(eb);
	apfs_set_header_flags(eb, flags & ~flag);
}

static inline int apfs_header_backref_rev(const struct extent_buffer *eb)
{
	u64 flags = apfs_header_flags(eb);
	return flags >> APFS_BACKREF_REV_SHIFT;
}

static inline void apfs_set_header_backref_rev(struct extent_buffer *eb,
						int rev)
{
	u64 flags = apfs_header_flags(eb);
	flags &= ~APFS_BACKREF_REV_MASK;
	flags |= (u64)rev << APFS_BACKREF_REV_SHIFT;
	apfs_set_header_flags(eb, flags);
}

static inline int apfs_is_leaf(const struct extent_buffer *eb)
{
	return apfs_header_level(eb) == 0;
}

/* struct apfs_root_item */
APFS_SETGET_FUNCS(disk_root_generation, struct apfs_root_item,
		   generation, 64);
APFS_SETGET_FUNCS(disk_root_refs, struct apfs_root_item, refs, 32);
APFS_SETGET_FUNCS(disk_root_bytenr, struct apfs_root_item, bytenr, 64);
APFS_SETGET_FUNCS(disk_root_level, struct apfs_root_item, level, 8);

APFS_SETGET_STACK_FUNCS(root_generation, struct apfs_root_item,
			 generation, 64);
APFS_SETGET_STACK_FUNCS(root_bytenr, struct apfs_root_item, bytenr, 64);
APFS_SETGET_STACK_FUNCS(root_drop_level, struct apfs_root_item, drop_level, 8);
APFS_SETGET_STACK_FUNCS(root_level, struct apfs_root_item, level, 8);
APFS_SETGET_STACK_FUNCS(root_dirid, struct apfs_root_item, root_dirid, 64);
APFS_SETGET_STACK_FUNCS(root_refs, struct apfs_root_item, refs, 32);
APFS_SETGET_STACK_FUNCS(root_flags, struct apfs_root_item, flags, 64);
APFS_SETGET_STACK_FUNCS(root_used, struct apfs_root_item, bytes_used, 64);
APFS_SETGET_STACK_FUNCS(root_limit, struct apfs_root_item, byte_limit, 64);
APFS_SETGET_STACK_FUNCS(root_last_snapshot, struct apfs_root_item,
			 last_snapshot, 64);
APFS_SETGET_STACK_FUNCS(root_generation_v2, struct apfs_root_item,
			 generation_v2, 64);
APFS_SETGET_STACK_FUNCS(root_ctransid, struct apfs_root_item,
			 ctransid, 64);
APFS_SETGET_STACK_FUNCS(root_otransid, struct apfs_root_item,
			 otransid, 64);
APFS_SETGET_STACK_FUNCS(root_stransid, struct apfs_root_item,
			 stransid, 64);
APFS_SETGET_STACK_FUNCS(root_rtransid, struct apfs_root_item,
			 rtransid, 64);

static inline bool apfs_root_readonly(const struct apfs_root *root)
{
	/* Byte-swap the constant at compile time, root_item::flags is LE */
	return (root->root_item.flags & cpu_to_le64(APFS_ROOT_SUBVOL_RDONLY)) != 0;
}

static inline bool apfs_root_dead(const struct apfs_root *root)
{
	/* Byte-swap the constant at compile time, root_item::flags is LE */
	return (root->root_item.flags & cpu_to_le64(APFS_ROOT_SUBVOL_DEAD)) != 0;
}

/* struct apfs_root_backup */
APFS_SETGET_STACK_FUNCS(backup_tree_root, struct apfs_root_backup,
		   tree_root, 64);
APFS_SETGET_STACK_FUNCS(backup_tree_root_gen, struct apfs_root_backup,
		   tree_root_gen, 64);
APFS_SETGET_STACK_FUNCS(backup_tree_root_level, struct apfs_root_backup,
		   tree_root_level, 8);

APFS_SETGET_STACK_FUNCS(backup_chunk_root, struct apfs_root_backup,
		   chunk_root, 64);
APFS_SETGET_STACK_FUNCS(backup_chunk_root_gen, struct apfs_root_backup,
		   chunk_root_gen, 64);
APFS_SETGET_STACK_FUNCS(backup_chunk_root_level, struct apfs_root_backup,
		   chunk_root_level, 8);

APFS_SETGET_STACK_FUNCS(backup_extent_root, struct apfs_root_backup,
		   extent_root, 64);
APFS_SETGET_STACK_FUNCS(backup_extent_root_gen, struct apfs_root_backup,
		   extent_root_gen, 64);
APFS_SETGET_STACK_FUNCS(backup_extent_root_level, struct apfs_root_backup,
		   extent_root_level, 8);

APFS_SETGET_STACK_FUNCS(backup_fs_root, struct apfs_root_backup,
		   fs_root, 64);
APFS_SETGET_STACK_FUNCS(backup_fs_root_gen, struct apfs_root_backup,
		   fs_root_gen, 64);
APFS_SETGET_STACK_FUNCS(backup_fs_root_level, struct apfs_root_backup,
		   fs_root_level, 8);

APFS_SETGET_STACK_FUNCS(backup_dev_root, struct apfs_root_backup,
		   dev_root, 64);
APFS_SETGET_STACK_FUNCS(backup_dev_root_gen, struct apfs_root_backup,
		   dev_root_gen, 64);
APFS_SETGET_STACK_FUNCS(backup_dev_root_level, struct apfs_root_backup,
		   dev_root_level, 8);

APFS_SETGET_STACK_FUNCS(backup_csum_root, struct apfs_root_backup,
		   csum_root, 64);
APFS_SETGET_STACK_FUNCS(backup_csum_root_gen, struct apfs_root_backup,
		   csum_root_gen, 64);
APFS_SETGET_STACK_FUNCS(backup_csum_root_level, struct apfs_root_backup,
		   csum_root_level, 8);
APFS_SETGET_STACK_FUNCS(backup_total_bytes, struct apfs_root_backup,
		   total_bytes, 64);
APFS_SETGET_STACK_FUNCS(backup_bytes_used, struct apfs_root_backup,
		   bytes_used, 64);
APFS_SETGET_STACK_FUNCS(backup_num_devices, struct apfs_root_backup,
		   num_devices, 64);

/* struct apfs_balance_item */
APFS_SETGET_FUNCS(balance_flags, struct apfs_balance_item, flags, 64);

static inline void apfs_balance_data(const struct extent_buffer *eb,
				      const struct apfs_balance_item *bi,
				      struct apfs_disk_balance_args *ba)
{
	read_eb_member(eb, bi, struct apfs_balance_item, data, ba);
}

static inline void apfs_set_balance_data(struct extent_buffer *eb,
				  struct apfs_balance_item *bi,
				  const struct apfs_disk_balance_args *ba)
{
	write_eb_member(eb, bi, struct apfs_balance_item, data, ba);
}

static inline void apfs_balance_meta(const struct extent_buffer *eb,
				      const struct apfs_balance_item *bi,
				      struct apfs_disk_balance_args *ba)
{
	read_eb_member(eb, bi, struct apfs_balance_item, meta, ba);
}

static inline void apfs_set_balance_meta(struct extent_buffer *eb,
				  struct apfs_balance_item *bi,
				  const struct apfs_disk_balance_args *ba)
{
	write_eb_member(eb, bi, struct apfs_balance_item, meta, ba);
}

static inline void apfs_balance_sys(const struct extent_buffer *eb,
				     const struct apfs_balance_item *bi,
				     struct apfs_disk_balance_args *ba)
{
	read_eb_member(eb, bi, struct apfs_balance_item, sys, ba);
}

static inline void apfs_set_balance_sys(struct extent_buffer *eb,
				 struct apfs_balance_item *bi,
				 const struct apfs_disk_balance_args *ba)
{
	write_eb_member(eb, bi, struct apfs_balance_item, sys, ba);
}

static inline void
apfs_disk_balance_args_to_cpu(struct apfs_balance_args *cpu,
			       const struct apfs_disk_balance_args *disk)
{
	memset(cpu, 0, sizeof(*cpu));

	cpu->profiles = le64_to_cpu(disk->profiles);
	cpu->usage = le64_to_cpu(disk->usage);
	cpu->devid = le64_to_cpu(disk->devid);
	cpu->pstart = le64_to_cpu(disk->pstart);
	cpu->pend = le64_to_cpu(disk->pend);
	cpu->vstart = le64_to_cpu(disk->vstart);
	cpu->vend = le64_to_cpu(disk->vend);
	cpu->target = le64_to_cpu(disk->target);
	cpu->flags = le64_to_cpu(disk->flags);
	cpu->limit = le64_to_cpu(disk->limit);
	cpu->stripes_min = le32_to_cpu(disk->stripes_min);
	cpu->stripes_max = le32_to_cpu(disk->stripes_max);
}

static inline void
apfs_cpu_balance_args_to_disk(struct apfs_disk_balance_args *disk,
			       const struct apfs_balance_args *cpu)
{
	memset(disk, 0, sizeof(*disk));

	disk->profiles = cpu_to_le64(cpu->profiles);
	disk->usage = cpu_to_le64(cpu->usage);
	disk->devid = cpu_to_le64(cpu->devid);
	disk->pstart = cpu_to_le64(cpu->pstart);
	disk->pend = cpu_to_le64(cpu->pend);
	disk->vstart = cpu_to_le64(cpu->vstart);
	disk->vend = cpu_to_le64(cpu->vend);
	disk->target = cpu_to_le64(cpu->target);
	disk->flags = cpu_to_le64(cpu->flags);
	disk->limit = cpu_to_le64(cpu->limit);
	disk->stripes_min = cpu_to_le32(cpu->stripes_min);
	disk->stripes_max = cpu_to_le32(cpu->stripes_max);
}

/* struct apfs_super_block */
APFS_SETGET_STACK_FUNCS(super_bytenr, struct apfs_super_block, bytenr, 64);
APFS_SETGET_STACK_FUNCS(super_flags, struct apfs_super_block, flags, 64);
APFS_SETGET_STACK_FUNCS(super_generation, struct apfs_super_block,
			 generation, 64);
APFS_SETGET_STACK_FUNCS(super_root, struct apfs_super_block, root, 64);
APFS_SETGET_STACK_FUNCS(super_sys_array_size,
			 struct apfs_super_block, sys_chunk_array_size, 32);
APFS_SETGET_STACK_FUNCS(super_chunk_root_generation,
			 struct apfs_super_block, chunk_root_generation, 64);
APFS_SETGET_STACK_FUNCS(super_root_level, struct apfs_super_block,
			 root_level, 8);
APFS_SETGET_STACK_FUNCS(super_chunk_root, struct apfs_super_block,
			 chunk_root, 64);
APFS_SETGET_STACK_FUNCS(super_chunk_root_level, struct apfs_super_block,
			 chunk_root_level, 8);
APFS_SETGET_STACK_FUNCS(super_log_root, struct apfs_super_block,
			 log_root, 64);
APFS_SETGET_STACK_FUNCS(super_log_root_transid, struct apfs_super_block,
			 log_root_transid, 64);
APFS_SETGET_STACK_FUNCS(super_log_root_level, struct apfs_super_block,
			 log_root_level, 8);
APFS_SETGET_STACK_FUNCS(super_total_bytes, struct apfs_super_block,
			 total_bytes, 64);
APFS_SETGET_STACK_FUNCS(super_bytes_used, struct apfs_super_block,
			 bytes_used, 64);
APFS_SETGET_STACK_FUNCS(super_sectorsize, struct apfs_super_block,
			 sectorsize, 32);
APFS_SETGET_STACK_FUNCS(super_nodesize, struct apfs_super_block,
			 nodesize, 32);
APFS_SETGET_STACK_FUNCS(super_stripesize, struct apfs_super_block,
			 stripesize, 32);
APFS_SETGET_STACK_FUNCS(super_root_dir, struct apfs_super_block,
			 root_dir_objectid, 64);
APFS_SETGET_STACK_FUNCS(super_num_devices, struct apfs_super_block,
			 num_devices, 64);
APFS_SETGET_STACK_FUNCS(super_compat_flags, struct apfs_super_block,
			 compat_flags, 64);
APFS_SETGET_STACK_FUNCS(super_compat_ro_flags, struct apfs_super_block,
			 compat_ro_flags, 64);
APFS_SETGET_STACK_FUNCS(super_incompat_flags, struct apfs_super_block,
			 incompat_flags, 64);
APFS_SETGET_STACK_FUNCS(super_csum_type, struct apfs_super_block,
			 csum_type, 16);
APFS_SETGET_STACK_FUNCS(super_cache_generation, struct apfs_super_block,
			 cache_generation, 64);
APFS_SETGET_STACK_FUNCS(super_magic, struct apfs_super_block, magic, 64);
APFS_SETGET_STACK_FUNCS(super_uuid_tree_generation, struct apfs_super_block,
			 uuid_tree_generation, 64);

int apfs_super_csum_size(const struct apfs_super_block *s);
const char *apfs_super_csum_name(u16 csum_type);
const char *apfs_super_csum_driver(u16 csum_type);
size_t __attribute_const__ apfs_get_num_csums(void);


/*
 * The leaf data grows from end-to-front in the node.
 * this returns the address of the start of the last item,
 * which is the stop of the leaf data stack
 */
static inline unsigned int leaf_data_end(const struct extent_buffer *leaf)
{
	u32 nr = apfs_header_nritems(leaf);

	if (nr == 0)
		return APFS_LEAF_DATA_SIZE(leaf->fs_info);
	return apfs_item_offset_nr(leaf, nr - 1);
}

/* struct apfs_file_extent_item */
APFS_SETGET_STACK_FUNCS(stack_file_extent_type, struct apfs_file_extent_item,
			 type, 8);
APFS_SETGET_STACK_FUNCS(stack_file_extent_disk_bytenr,
			 struct apfs_file_extent_item, disk_bytenr, 64);
APFS_SETGET_STACK_FUNCS(stack_file_extent_offset,
			 struct apfs_file_extent_item, offset, 64);
APFS_SETGET_STACK_FUNCS(stack_file_extent_generation,
			 struct apfs_file_extent_item, generation, 64);
APFS_SETGET_STACK_FUNCS(stack_file_extent_num_bytes,
			 struct apfs_file_extent_item, num_bytes, 64);
APFS_SETGET_STACK_FUNCS(stack_file_extent_ram_bytes,
			 struct apfs_file_extent_item, ram_bytes, 64);
APFS_SETGET_STACK_FUNCS(stack_file_extent_disk_num_bytes,
			 struct apfs_file_extent_item, disk_num_bytes, 64);
APFS_SETGET_STACK_FUNCS(stack_file_extent_compression,
			 struct apfs_file_extent_item, compression, 8);

static inline unsigned long
apfs_file_extent_inline_start(const struct apfs_file_extent_item *e)
{
	return (unsigned long)e + APFS_FILE_EXTENT_INLINE_DATA_START;
}

static inline u32 apfs_file_extent_calc_inline_size(u32 datasize)
{
	return APFS_FILE_EXTENT_INLINE_DATA_START + datasize;
}

APFS_SETGET_FUNCS(file_extent_type, struct apfs_file_extent_item, type, 8);
APFS_SETGET_FUNCS(file_extent_disk_bytenr, struct apfs_file_extent_item,
		   disk_bytenr, 64);
APFS_SETGET_FUNCS(file_extent_generation, struct apfs_file_extent_item,
		   generation, 64);
APFS_SETGET_FUNCS(file_extent_disk_num_bytes, struct apfs_file_extent_item,
		   disk_num_bytes, 64);
APFS_SETGET_FUNCS(file_extent_offset, struct apfs_file_extent_item,
		  offset, 64);
APFS_SETGET_FUNCS(file_extent_num_bytes, struct apfs_file_extent_item,
		   num_bytes, 64);
APFS_SETGET_FUNCS(file_extent_ram_bytes, struct apfs_file_extent_item,
		   ram_bytes, 64);
APFS_SETGET_FUNCS(file_extent_compression, struct apfs_file_extent_item,
		   compression, 8);
APFS_SETGET_FUNCS(file_extent_encryption, struct apfs_file_extent_item,
		   encryption, 8);
APFS_SETGET_FUNCS(file_extent_other_encoding, struct apfs_file_extent_item,
		   other_encoding, 16);

/*
 * this returns the number of bytes used by the item on disk, minus the
 * size of any extent headers.  If a file is compressed on disk, this is
 * the compressed size
 */
static inline u32 apfs_file_extent_inline_item_len(
						const struct extent_buffer *eb,
						struct apfs_item *e)
{
	return apfs_item_size(eb, e) - APFS_FILE_EXTENT_INLINE_DATA_START;
}

/* apfs_qgroup_status_item */
APFS_SETGET_FUNCS(qgroup_status_generation, struct apfs_qgroup_status_item,
		   generation, 64);
APFS_SETGET_FUNCS(qgroup_status_version, struct apfs_qgroup_status_item,
		   version, 64);
APFS_SETGET_FUNCS(qgroup_status_flags, struct apfs_qgroup_status_item,
		   flags, 64);
APFS_SETGET_FUNCS(qgroup_status_rescan, struct apfs_qgroup_status_item,
		   rescan, 64);

/* apfs_qgroup_info_item */
APFS_SETGET_FUNCS(qgroup_info_generation, struct apfs_qgroup_info_item,
		   generation, 64);
APFS_SETGET_FUNCS(qgroup_info_rfer, struct apfs_qgroup_info_item, rfer, 64);
APFS_SETGET_FUNCS(qgroup_info_rfer_cmpr, struct apfs_qgroup_info_item,
		   rfer_cmpr, 64);
APFS_SETGET_FUNCS(qgroup_info_excl, struct apfs_qgroup_info_item, excl, 64);
APFS_SETGET_FUNCS(qgroup_info_excl_cmpr, struct apfs_qgroup_info_item,
		   excl_cmpr, 64);

APFS_SETGET_STACK_FUNCS(stack_qgroup_info_generation,
			 struct apfs_qgroup_info_item, generation, 64);
APFS_SETGET_STACK_FUNCS(stack_qgroup_info_rfer, struct apfs_qgroup_info_item,
			 rfer, 64);
APFS_SETGET_STACK_FUNCS(stack_qgroup_info_rfer_cmpr,
			 struct apfs_qgroup_info_item, rfer_cmpr, 64);
APFS_SETGET_STACK_FUNCS(stack_qgroup_info_excl, struct apfs_qgroup_info_item,
			 excl, 64);
APFS_SETGET_STACK_FUNCS(stack_qgroup_info_excl_cmpr,
			 struct apfs_qgroup_info_item, excl_cmpr, 64);

/* apfs_qgroup_limit_item */
APFS_SETGET_FUNCS(qgroup_limit_flags, struct apfs_qgroup_limit_item,
		   flags, 64);
APFS_SETGET_FUNCS(qgroup_limit_max_rfer, struct apfs_qgroup_limit_item,
		   max_rfer, 64);
APFS_SETGET_FUNCS(qgroup_limit_max_excl, struct apfs_qgroup_limit_item,
		   max_excl, 64);
APFS_SETGET_FUNCS(qgroup_limit_rsv_rfer, struct apfs_qgroup_limit_item,
		   rsv_rfer, 64);
APFS_SETGET_FUNCS(qgroup_limit_rsv_excl, struct apfs_qgroup_limit_item,
		   rsv_excl, 64);

/* apfs_dev_replace_item */
APFS_SETGET_FUNCS(dev_replace_src_devid,
		   struct apfs_dev_replace_item, src_devid, 64);
APFS_SETGET_FUNCS(dev_replace_cont_reading_from_srcdev_mode,
		   struct apfs_dev_replace_item, cont_reading_from_srcdev_mode,
		   64);
APFS_SETGET_FUNCS(dev_replace_replace_state, struct apfs_dev_replace_item,
		   replace_state, 64);
APFS_SETGET_FUNCS(dev_replace_time_started, struct apfs_dev_replace_item,
		   time_started, 64);
APFS_SETGET_FUNCS(dev_replace_time_stopped, struct apfs_dev_replace_item,
		   time_stopped, 64);
APFS_SETGET_FUNCS(dev_replace_num_write_errors, struct apfs_dev_replace_item,
		   num_write_errors, 64);
APFS_SETGET_FUNCS(dev_replace_num_uncorrectable_read_errors,
		   struct apfs_dev_replace_item, num_uncorrectable_read_errors,
		   64);
APFS_SETGET_FUNCS(dev_replace_cursor_left, struct apfs_dev_replace_item,
		   cursor_left, 64);
APFS_SETGET_FUNCS(dev_replace_cursor_right, struct apfs_dev_replace_item,
		   cursor_right, 64);

APFS_SETGET_STACK_FUNCS(stack_dev_replace_src_devid,
			 struct apfs_dev_replace_item, src_devid, 64);
APFS_SETGET_STACK_FUNCS(stack_dev_replace_cont_reading_from_srcdev_mode,
			 struct apfs_dev_replace_item,
			 cont_reading_from_srcdev_mode, 64);
APFS_SETGET_STACK_FUNCS(stack_dev_replace_replace_state,
			 struct apfs_dev_replace_item, replace_state, 64);
APFS_SETGET_STACK_FUNCS(stack_dev_replace_time_started,
			 struct apfs_dev_replace_item, time_started, 64);
APFS_SETGET_STACK_FUNCS(stack_dev_replace_time_stopped,
			 struct apfs_dev_replace_item, time_stopped, 64);
APFS_SETGET_STACK_FUNCS(stack_dev_replace_num_write_errors,
			 struct apfs_dev_replace_item, num_write_errors, 64);
APFS_SETGET_STACK_FUNCS(stack_dev_replace_num_uncorrectable_read_errors,
			 struct apfs_dev_replace_item,
			 num_uncorrectable_read_errors, 64);
APFS_SETGET_STACK_FUNCS(stack_dev_replace_cursor_left,
			 struct apfs_dev_replace_item, cursor_left, 64);
APFS_SETGET_STACK_FUNCS(stack_dev_replace_cursor_right,
			 struct apfs_dev_replace_item, cursor_right, 64);

/* helper function to cast into the data area of the leaf. */
#define apfs_item_ptr(node, slot, type)			\
	((type *)((unsigned long)apfs_item_offset_nr(node, slot)))
#define apfs_item_ptr_offset(leaf, slot) \
	(apfs_item_offset_nr(leaf, slot))
#define apfs_item_offset_ptr(leaf, offset, type) \
	((type *)offset)

static inline u32 apfs_crc32c(u32 crc, const void *address, unsigned length)
{
	return crc32c(crc, address, length);
}

static inline void apfs_crc32c_final(u32 crc, u8 *result)
{
	put_unaligned_le32(~crc, result);
}

u32 apfs_name_hash(const char *name, int len, bool case_fold);

/*
 * Figure the key offset of an extended inode ref
 */
static inline u64 apfs_extref_hash(u64 parent_objectid, const char *name,
                                   int len)
{
       return (u64) crc32c(parent_objectid, name, len);
}

static inline gfp_t apfs_alloc_write_mask(struct address_space *mapping)
{
	return mapping_gfp_constraint(mapping, ~__GFP_FS);
}

/* extent-tree.c */

enum apfs_inline_ref_type {
	APFS_REF_TYPE_INVALID,
	APFS_REF_TYPE_BLOCK,
	APFS_REF_TYPE_DATA,
	APFS_REF_TYPE_ANY,
};

int apfs_get_extent_inline_ref_type(const struct extent_buffer *eb,
				     struct apfs_extent_inline_ref *iref,
				     enum apfs_inline_ref_type is_data);
u64 hash_extent_data_ref(u64 root_objectid, u64 owner, u64 offset);

/*
 * Take the number of bytes to be checksummmed and figure out how many leaves
 * it would require to store the csums for that many bytes.
 */
static inline u64 apfs_csum_bytes_to_leaves(
			const struct apfs_fs_info *fs_info, u64 csum_bytes)
{
	const u64 num_csums = csum_bytes >> fs_info->sectorsize_bits;

	return DIV_ROUND_UP_ULL(num_csums, fs_info->csums_per_leaf);
}

/*
 * Use this if we would be adding new items, as we could split nodes as we cow
 * down the tree.
 */
static inline u64 apfs_calc_insert_metadata_size(struct apfs_fs_info *fs_info,
						  unsigned num_items)
{
	return (u64)fs_info->nodesize * APFS_MAX_LEVEL * 2 * num_items;
}

/*
 * Doing a truncate or a modification won't result in new nodes or leaves, just
 * what we need for COW.
 */
static inline u64 apfs_calc_metadata_size(struct apfs_fs_info *fs_info,
						 unsigned num_items)
{
	return (u64)fs_info->nodesize * APFS_MAX_LEVEL * num_items;
}

int apfs_add_excluded_extent(struct apfs_fs_info *fs_info,
			      u64 start, u64 num_bytes);
void apfs_free_excluded_extents(struct apfs_block_group *cache);
int apfs_run_delayed_refs(struct apfs_trans_handle *trans,
			   unsigned long count);
void apfs_cleanup_ref_head_accounting(struct apfs_fs_info *fs_info,
				  struct apfs_delayed_ref_root *delayed_refs,
				  struct apfs_delayed_ref_head *head);
int apfs_lookup_data_extent(struct apfs_fs_info *fs_info, u64 start, u64 len);
int apfs_lookup_extent_info(struct apfs_trans_handle *trans,
			     struct apfs_fs_info *fs_info, u64 bytenr,
			     u64 offset, int metadata, u64 *refs, u64 *flags);
int apfs_pin_extent(struct apfs_trans_handle *trans, u64 bytenr, u64 num,
		     int reserved);
int apfs_pin_extent_for_log_replay(struct apfs_trans_handle *trans,
				    u64 bytenr, u64 num_bytes);
int apfs_exclude_logged_extents(struct extent_buffer *eb);
int apfs_cross_ref_exist(struct apfs_root *root,
			  u64 objectid, u64 offset, u64 bytenr, bool strict);
struct extent_buffer *apfs_alloc_tree_block(struct apfs_trans_handle *trans,
					     struct apfs_root *root,
					     u64 parent, u64 root_objectid,
					     const struct apfs_disk_key *key,
					     int level, u64 hint,
					     u64 empty_size,
					     enum apfs_lock_nesting nest);
void apfs_free_tree_block(struct apfs_trans_handle *trans,
			   struct apfs_root *root,
			   struct extent_buffer *buf,
			   u64 parent, int last_ref);
int apfs_alloc_reserved_file_extent(struct apfs_trans_handle *trans,
				     struct apfs_root *root, u64 owner,
				     u64 offset, u64 ram_bytes,
				     struct apfs_key *ins);
int apfs_alloc_logged_file_extent(struct apfs_trans_handle *trans,
				   u64 root_objectid, u64 owner, u64 offset,
				   struct apfs_key *ins);
int apfs_reserve_extent(struct apfs_root *root, u64 ram_bytes, u64 num_bytes,
			 u64 min_alloc_size, u64 empty_size, u64 hint_byte,
			 struct apfs_key *ins, int is_data, int delalloc);
int apfs_inc_ref(struct apfs_trans_handle *trans, struct apfs_root *root,
		  struct extent_buffer *buf, int full_backref);
int apfs_dec_ref(struct apfs_trans_handle *trans, struct apfs_root *root,
		  struct extent_buffer *buf, int full_backref);
int apfs_set_disk_extent_flags(struct apfs_trans_handle *trans,
				struct extent_buffer *eb, u64 flags,
				int level, int is_data);
int apfs_free_extent(struct apfs_trans_handle *trans, struct apfs_ref *ref);

int apfs_free_reserved_extent(struct apfs_fs_info *fs_info,
			       u64 start, u64 len, int delalloc);
int apfs_pin_reserved_extent(struct apfs_trans_handle *trans, u64 start,
			      u64 len);
int apfs_finish_extent_commit(struct apfs_trans_handle *trans);
int apfs_inc_extent_ref(struct apfs_trans_handle *trans,
			 struct apfs_ref *generic_ref);

void apfs_clear_space_info_full(struct apfs_fs_info *info);

/*
 * Different levels for to flush space when doing space reservations.
 *
 * The higher the level, the more methods we try to reclaim space.
 */
enum apfs_reserve_flush_enum {
	/* If we are in the transaction, we can't flush anything.*/
	APFS_RESERVE_NO_FLUSH,

	/*
	 * Flush space by:
	 * - Running delayed inode items
	 * - Allocating a new chunk
	 */
	APFS_RESERVE_FLUSH_LIMIT,

	/*
	 * Flush space by:
	 * - Running delayed inode items
	 * - Running delayed refs
	 * - Running delalloc and waiting for ordered extents
	 * - Allocating a new chunk
	 */
	APFS_RESERVE_FLUSH_EVICT,

	/*
	 * Flush space by above mentioned methods and by:
	 * - Running delayed iputs
	 * - Committing transaction
	 *
	 * Can be interrupted by a fatal signal.
	 */
	APFS_RESERVE_FLUSH_DATA,
	APFS_RESERVE_FLUSH_FREE_SPACE_INODE,
	APFS_RESERVE_FLUSH_ALL,

	/*
	 * Pretty much the same as FLUSH_ALL, but can also steal space from
	 * global rsv.
	 *
	 * Can be interrupted by a fatal signal.
	 */
	APFS_RESERVE_FLUSH_ALL_STEAL,
};

enum apfs_flush_state {
	FLUSH_DELAYED_ITEMS_NR	=	1,
	FLUSH_DELAYED_ITEMS	=	2,
	FLUSH_DELAYED_REFS_NR	=	3,
	FLUSH_DELAYED_REFS	=	4,
	FLUSH_DELALLOC		=	5,
	FLUSH_DELALLOC_WAIT	=	6,
	ALLOC_CHUNK		=	7,
	ALLOC_CHUNK_FORCE	=	8,
	RUN_DELAYED_IPUTS	=	9,
	COMMIT_TRANS		=	10,
};

int apfs_subvolume_reserve_metadata(struct apfs_root *root,
				     struct apfs_block_rsv *rsv,
				     int nitems, bool use_global_rsv);
void apfs_subvolume_release_metadata(struct apfs_root *root,
				      struct apfs_block_rsv *rsv);
void apfs_delalloc_release_extents(struct apfs_inode *inode, u64 num_bytes);

int apfs_delalloc_reserve_metadata(struct apfs_inode *inode, u64 num_bytes);
u64 apfs_account_ro_block_groups_free_space(struct apfs_space_info *sinfo);
int apfs_error_unpin_extent_range(struct apfs_fs_info *fs_info,
				   u64 start, u64 end);
int apfs_discard_extent(struct apfs_fs_info *fs_info, u64 bytenr,
			 u64 num_bytes, u64 *actual_bytes);
int apfs_trim_fs(struct apfs_fs_info *fs_info, struct fstrim_range *range);

int apfs_init_space_info(struct apfs_fs_info *fs_info);
int apfs_delayed_refs_qgroup_accounting(struct apfs_trans_handle *trans,
					 struct apfs_fs_info *fs_info);
int apfs_start_write_no_snapshotting(struct apfs_root *root);
void apfs_end_write_no_snapshotting(struct apfs_root *root);
void apfs_wait_for_snapshot_creation(struct apfs_root *root);

/* ctree.c */
int apfs_bin_search(struct extent_buffer *eb, const struct apfs_key *key,
		     int *slot);
int __pure apfs_comp_cpu_keys(const struct extent_buffer *eb,
		      const struct apfs_key *k1, const struct apfs_key *k2);
int apfs_previous_item(struct apfs_root *root,
			struct apfs_path *path, u64 min_objectid,
			int type);
int apfs_previous_extent_item(struct apfs_root *root,
			struct apfs_path *path, u64 min_objectid);
void apfs_set_item_key_safe(struct apfs_fs_info *fs_info,
			     struct apfs_path *path,
			     const struct apfs_key *new_key);
struct extent_buffer *apfs_root_node(struct apfs_root *root);
int apfs_find_next_key(struct apfs_root *root, struct apfs_path *path,
			struct apfs_key *key, int lowest_level,
			u64 min_trans);
int apfs_search_forward(struct apfs_root *root, struct apfs_key *min_key,
			 struct apfs_path *path,
			 u64 min_trans);
struct extent_buffer *apfs_read_node_slot(struct extent_buffer *parent,
					   int slot);

int apfs_cow_block(struct apfs_trans_handle *trans,
		    struct apfs_root *root, struct extent_buffer *buf,
		    struct extent_buffer *parent, int parent_slot,
		    struct extent_buffer **cow_ret,
		    enum apfs_lock_nesting nest);
int apfs_copy_root(struct apfs_trans_handle *trans,
		      struct apfs_root *root,
		      struct extent_buffer *buf,
		      struct extent_buffer **cow_ret, u64 new_root_objectid);
int apfs_block_can_be_shared(struct apfs_root *root,
			      struct extent_buffer *buf);
void apfs_extend_item(struct apfs_path *path, u32 data_size);
void apfs_truncate_item(struct apfs_path *path, u32 new_size, int from_end);
int apfs_split_item(struct apfs_trans_handle *trans,
		     struct apfs_root *root,
		     struct apfs_path *path,
		     const struct apfs_key *new_key,
		     unsigned long split_offset);
int apfs_duplicate_item(struct apfs_trans_handle *trans,
			 struct apfs_root *root,
			 struct apfs_path *path,
			 const struct apfs_key *new_key);
int apfs_find_item(struct apfs_root *fs_root, struct apfs_path *path,
		u64 inum, u64 ioff, u8 key_type, struct apfs_key *found_key);
int apfs_search_slot(struct apfs_trans_handle *trans, struct apfs_root *root,
		      const struct apfs_key *key, struct apfs_path *p,
		      int ins_len, int cow);
int apfs_search_old_slot(struct apfs_root *root, const struct apfs_key *key,
			  struct apfs_path *p, u64 time_seq);
int apfs_search_slot_for_read(struct apfs_root *root,
			       const struct apfs_key *key,
			       struct apfs_path *p, int find_higher,
			       int return_any);
int apfs_realloc_node(struct apfs_trans_handle *trans,
		       struct apfs_root *root, struct extent_buffer *parent,
		       int start_slot, u64 *last_ret,
		       struct apfs_key *progress);
void apfs_release_path(struct apfs_path *p);
struct apfs_path *apfs_alloc_path(void);
void apfs_free_path(struct apfs_path *p);

int apfs_del_items(struct apfs_trans_handle *trans, struct apfs_root *root,
		   struct apfs_path *path, int slot, int nr);
static inline int apfs_del_item(struct apfs_trans_handle *trans,
				 struct apfs_root *root,
				 struct apfs_path *path)
{
	return apfs_del_items(trans, root, path, path->slots[0], 1);
}

void setup_items_for_insert(struct apfs_root *root, struct apfs_path *path,
			    const struct apfs_key *cpu_key, u32 *data_size,
			    int nr);
int apfs_insert_item(struct apfs_trans_handle *trans, struct apfs_root *root,
		      const struct apfs_key *key, void *data, u32 data_size);
int apfs_insert_empty_items(struct apfs_trans_handle *trans,
			     struct apfs_root *root,
			     struct apfs_path *path,
			     const struct apfs_key *cpu_key, u32 *data_size,
			     int nr);

static inline int apfs_insert_empty_item(struct apfs_trans_handle *trans,
					  struct apfs_root *root,
					  struct apfs_path *path,
					  const struct apfs_key *key,
					  u32 data_size)
{
	return apfs_insert_empty_items(trans, root, path, key, &data_size, 1);
}

int apfs_next_leaf(struct apfs_root *root, struct apfs_path *path);
int apfs_prev_leaf(struct apfs_root *root, struct apfs_path *path);
int apfs_next_old_leaf(struct apfs_root *root, struct apfs_path *path,
			u64 time_seq);
static inline int apfs_next_old_item(struct apfs_root *root,
				      struct apfs_path *p, u64 time_seq)
{
	++p->slots[0];
	if (p->slots[0] >= apfs_header_nritems(p->nodes[0]))
		return apfs_next_old_leaf(root, p, time_seq);
	return 0;
}
static inline int apfs_next_item(struct apfs_root *root, struct apfs_path *p)
{
	return apfs_next_old_item(root, p, 0);
}
int apfs_leaf_free_space(struct extent_buffer *leaf);
int __must_check apfs_drop_snapshot(struct apfs_root *root, int update_ref,
				     int for_reloc);
int apfs_drop_subtree(struct apfs_trans_handle *trans,
			struct apfs_root *root,
			struct extent_buffer *node,
			struct extent_buffer *parent);
static inline int apfs_fs_closing(struct apfs_fs_info *fs_info)
{
	/*
	 * Do it this way so we only ever do one test_bit in the normal case.
	 */
	if (test_bit(APFS_FS_CLOSING_START, &fs_info->flags)) {
		if (test_bit(APFS_FS_CLOSING_DONE, &fs_info->flags))
			return 2;
		return 1;
	}
	return 0;
}

/*
 * If we remount the fs to be R/O or umount the fs, the cleaner needn't do
 * anything except sleeping. This function is used to check the status of
 * the fs.
 * We check for APFS_FS_STATE_RO to avoid races with a concurrent remount,
 * since setting and checking for SB_RDONLY in the superblock's flags is not
 * atomic.
 */
static inline int apfs_need_cleaner_sleep(struct apfs_fs_info *fs_info)
{
	return test_bit(APFS_FS_STATE_RO, &fs_info->fs_state) ||
		apfs_fs_closing(fs_info);
}

static inline void apfs_set_sb_rdonly(struct super_block *sb)
{
	sb->s_flags |= SB_RDONLY;
	set_bit(APFS_FS_STATE_RO, &apfs_sb(sb)->fs_state);
}

static inline void apfs_clear_sb_rdonly(struct super_block *sb)
{
	sb->s_flags &= ~SB_RDONLY;
	clear_bit(APFS_FS_STATE_RO, &apfs_sb(sb)->fs_state);
}

/* root-item.c */
int apfs_add_root_ref(struct apfs_trans_handle *trans, u64 root_id,
		       u64 ref_id, u64 dirid, u64 sequence, const char *name,
		       int name_len);
int apfs_del_root_ref(struct apfs_trans_handle *trans, u64 root_id,
		       u64 ref_id, u64 dirid, u64 *sequence, const char *name,
		       int name_len);
int apfs_del_root(struct apfs_trans_handle *trans,
		   const struct apfs_key *key);
int apfs_insert_root(struct apfs_trans_handle *trans, struct apfs_root *root,
		      const struct apfs_key *key,
		      struct apfs_root_item *item);
int __must_check apfs_update_root(struct apfs_trans_handle *trans,
				   struct apfs_root *root,
				   struct apfs_key *key,
				   struct apfs_root_item *item);
int apfs_find_root(struct apfs_root *root, const struct apfs_key *search_key,
		    struct apfs_path *path, struct apfs_root_item *root_item,
		    struct apfs_key *root_key);
int apfs_find_orphan_roots(struct apfs_fs_info *fs_info);
void apfs_set_root_node(struct apfs_root_item *item,
			 struct extent_buffer *node);
void apfs_check_and_init_root_item(struct apfs_root_item *item);
void apfs_update_root_times(struct apfs_trans_handle *trans,
			     struct apfs_root *root);

/* uuid-tree.c */
int apfs_uuid_tree_add(struct apfs_trans_handle *trans, u8 *uuid, u8 type,
			u64 subid);
int apfs_uuid_tree_remove(struct apfs_trans_handle *trans, u8 *uuid, u8 type,
			u64 subid);
int apfs_uuid_tree_iterate(struct apfs_fs_info *fs_info);

/* dir-item.c */
int apfs_check_dir_item_collision(struct apfs_root *root, u64 dir,
			  const char *name, int name_len);
int apfs_insert_dir_item(struct apfs_trans_handle *trans, const char *name,
			  int name_len, struct apfs_inode *dir,
			  struct apfs_key *location, u8 type, u64 index);
struct apfs_dir_item *apfs_lookup_dir_item(struct apfs_trans_handle *trans,
					     struct apfs_root *root,
					     struct apfs_path *path, u64 dir,
					     const char *name, int name_len,
					     int mod);
struct apfs_drec_item *apfs_lookup_dir_rec(struct apfs_trans_handle *trans,
					   struct apfs_root *root,
					   struct apfs_path *path, u64 dir,
					   const char *name, int name_len,
					   int mod);
struct apfs_dir_item *
apfs_lookup_dir_index_item(struct apfs_trans_handle *trans,
			    struct apfs_root *root,
			    struct apfs_path *path, u64 dir,
			    u64 objectid, const char *name, int name_len,
			    int mod);
struct apfs_dir_item *
apfs_search_dir_index_item(struct apfs_root *root,
			    struct apfs_path *path, u64 dirid,
			    const char *name, int name_len);
int apfs_delete_one_dir_name(struct apfs_trans_handle *trans,
			      struct apfs_root *root,
			      struct apfs_path *path,
			      struct apfs_dir_item *di);
int apfs_insert_xattr_item(struct apfs_trans_handle *trans,
			    struct apfs_root *root,
			    struct apfs_path *path, u64 objectid,
			    const char *name, u16 name_len,
			    const void *data, u16 data_len);
struct apfs_dir_item *apfs_lookup_xattr(struct apfs_trans_handle *trans,
					  struct apfs_root *root,
					  struct apfs_path *path, u64 dir,
					  const char *name, u16 name_len,
					  int mod);
struct apfs_dir_item *apfs_match_dir_item_name(struct apfs_fs_info *fs_info,
						 struct apfs_path *path,
						 const char *name,
						 int name_len);

/* orphan.c */
int apfs_insert_orphan_item(struct apfs_trans_handle *trans,
			     struct apfs_root *root, u64 offset);
int apfs_del_orphan_item(struct apfs_trans_handle *trans,
			  struct apfs_root *root, u64 offset);
int apfs_find_orphan_item(struct apfs_root *root, u64 offset);

/* inode-item.c */
int apfs_insert_inode_ref(struct apfs_trans_handle *trans,
			   struct apfs_root *root,
			   const char *name, int name_len,
			   u64 inode_objectid, u64 ref_objectid, u64 index);
int apfs_del_inode_ref(struct apfs_trans_handle *trans,
			   struct apfs_root *root,
			   const char *name, int name_len,
			   u64 inode_objectid, u64 ref_objectid, u64 *index);
int apfs_insert_empty_inode(struct apfs_trans_handle *trans,
			     struct apfs_root *root,
			     struct apfs_path *path, u64 objectid);
int apfs_lookup_inode(struct apfs_trans_handle *trans, struct apfs_root
		       *root, struct apfs_path *path,
		       struct apfs_key *location, int mod);

struct apfs_inode_extref *
apfs_lookup_inode_extref(struct apfs_trans_handle *trans,
			  struct apfs_root *root,
			  struct apfs_path *path,
			  const char *name, int name_len,
			  u64 inode_objectid, u64 ref_objectid, int ins_len,
			  int cow);

struct apfs_inode_ref *apfs_find_name_in_backref(struct extent_buffer *leaf,
						   int slot, const char *name,
						   int name_len);
struct apfs_inode_extref *apfs_find_name_in_ext_backref(
		struct extent_buffer *leaf, int slot, u64 ref_objectid,
		const char *name, int name_len);
/* file-item.c */
struct apfs_dio_private;
int apfs_del_csums(struct apfs_trans_handle *trans,
		    struct apfs_root *root, u64 bytenr, u64 len);
blk_status_t apfs_lookup_bio_sums(struct inode *inode, struct bio *bio, u8 *dst);
int apfs_insert_file_extent(struct apfs_trans_handle *trans,
			     struct apfs_root *root,
			     u64 objectid, u64 pos,
			     u64 disk_offset, u64 disk_num_bytes,
			     u64 num_bytes, u64 offset, u64 ram_bytes,
			     u8 compression, u8 encryption, u16 other_encoding);
int apfs_lookup_file_extent(struct apfs_trans_handle *trans,
			     struct apfs_root *root,
			     struct apfs_path *path, u64 objectid,
			     u64 bytenr, int mod);
int apfs_csum_file_blocks(struct apfs_trans_handle *trans,
			   struct apfs_root *root,
			   struct apfs_ordered_sum *sums);
blk_status_t apfs_csum_one_bio(struct apfs_inode *inode, struct bio *bio,
				u64 file_start, int contig);
int apfs_lookup_csums_range(struct apfs_root *root, u64 start, u64 end,
			     struct list_head *list, int search_commit);
int apfs_extent_item_to_extent_map(struct apfs_inode *inode,
				    const struct apfs_path *path,
				    struct page *page,
				    struct extent_map **emp,
				    u64 start, u64 len);
int apfs_inode_clear_file_extent_range(struct apfs_inode *inode, u64 start,
					u64 len);
int apfs_inode_set_file_extent_range(struct apfs_inode *inode, u64 start,
				      u64 len);
void apfs_inode_safe_disk_i_size_write(struct apfs_inode *inode, u64 new_i_size);
u64 apfs_file_extent_end(const struct apfs_path *path);

/* inode.c */
blk_status_t apfs_submit_data_bio(struct inode *inode, struct bio *bio,
				   int mirror_num, unsigned long bio_flags);
unsigned int apfs_verify_data_csum(struct apfs_io_bio *io_bio, u32 bio_offset,
				    struct page *page, u64 start, u64 end);
struct extent_map *apfs_get_extent_fiemap(struct apfs_inode *inode,
					   u64 start, u64 len);
noinline int can_nocow_extent(struct inode *inode, u64 offset, u64 *len,
			      u64 *orig_start, u64 *orig_block_len,
			      u64 *ram_bytes, bool strict);

void __apfs_del_delalloc_inode(struct apfs_root *root,
				struct apfs_inode *inode);
struct inode *apfs_lookup_dentry(struct inode *dir, struct dentry *dentry);
int apfs_set_inode_index(struct apfs_inode *dir, u64 *index);
int apfs_unlink_inode(struct apfs_trans_handle *trans,
		       struct apfs_root *root,
		       struct apfs_inode *dir, struct apfs_inode *inode,
		       const char *name, int name_len);
int apfs_add_link(struct apfs_trans_handle *trans,
		   struct apfs_inode *parent_inode, struct apfs_inode *inode,
		   const char *name, int name_len, int add_backref, u64 index);
int apfs_delete_subvolume(struct inode *dir, struct dentry *dentry);
int apfs_truncate_block(struct apfs_inode *inode, loff_t from, loff_t len,
			 int front);
int apfs_truncate_inode_items(struct apfs_trans_handle *trans,
			       struct apfs_root *root,
			       struct apfs_inode *inode, u64 new_size,
			       u32 min_type, u64 *extents_found);

int apfs_start_delalloc_snapshot(struct apfs_root *root, bool in_reclaim_context);
int apfs_start_delalloc_roots(struct apfs_fs_info *fs_info, long nr,
			       bool in_reclaim_context);
int apfs_set_extent_delalloc(struct apfs_inode *inode, u64 start, u64 end,
			      unsigned int extra_bits,
			      struct extent_state **cached_state);
int apfs_create_subvol_root(struct apfs_trans_handle *trans,
			     struct apfs_root *new_root,
			     struct apfs_root *parent_root);
 void apfs_set_delalloc_extent(struct inode *inode, struct extent_state *state,
			       unsigned *bits);
void apfs_clear_delalloc_extent(struct inode *inode,
				 struct extent_state *state, unsigned *bits);
void apfs_merge_delalloc_extent(struct inode *inode, struct extent_state *new,
				 struct extent_state *other);
void apfs_split_delalloc_extent(struct inode *inode,
				 struct extent_state *orig, u64 split);
int apfs_bio_fits_in_stripe(struct page *page, size_t size, struct bio *bio,
			     unsigned long bio_flags);
void apfs_set_range_writeback(struct apfs_inode *inode, u64 start, u64 end);
vm_fault_t apfs_page_mkwrite(struct vm_fault *vmf);
int apfs_readpage(struct file *file, struct page *page);
void apfs_evict_inode(struct inode *inode);
int apfs_write_inode(struct inode *inode, struct writeback_control *wbc);
struct inode *apfs_alloc_inode(struct super_block *sb);
void apfs_destroy_inode(struct inode *inode);
void apfs_free_inode(struct inode *inode);
int apfs_drop_inode(struct inode *inode);
int __init apfs_init_cachep(void);
void __cold apfs_destroy_cachep(void);
struct inode *apfs_iget_path(struct super_block *s, u64 ino,
			      struct apfs_root *root);
struct inode *apfs_iget(struct super_block *s, u64 ino, struct apfs_root *root);
struct extent_map *apfs_get_extent(struct apfs_inode *inode,
				    struct page *page, size_t pg_offset,
				    u64 start, u64 end);
int apfs_update_inode(struct apfs_trans_handle *trans,
		       struct apfs_root *root, struct apfs_inode *inode);
int apfs_update_inode_fallback(struct apfs_trans_handle *trans,
				struct apfs_root *root, struct apfs_inode *inode);
int apfs_orphan_add(struct apfs_trans_handle *trans,
		struct apfs_inode *inode);
int apfs_orphan_cleanup(struct apfs_root *root);
int apfs_cont_expand(struct apfs_inode *inode, loff_t oldsize, loff_t size);
void apfs_add_delayed_iput(struct inode *inode);
void apfs_run_delayed_iputs(struct apfs_fs_info *fs_info);
int apfs_wait_on_delayed_iputs(struct apfs_fs_info *fs_info);
int apfs_prealloc_file_range(struct inode *inode, int mode,
			      u64 start, u64 num_bytes, u64 min_size,
			      loff_t actual_len, u64 *alloc_hint);
int apfs_prealloc_file_range_trans(struct inode *inode,
				    struct apfs_trans_handle *trans, int mode,
				    u64 start, u64 num_bytes, u64 min_size,
				    loff_t actual_len, u64 *alloc_hint);
int apfs_run_delalloc_range(struct apfs_inode *inode, struct page *locked_page,
		u64 start, u64 end, int *page_started, unsigned long *nr_written,
		struct writeback_control *wbc);
int apfs_writepage_cow_fixup(struct page *page, u64 start, u64 end);
void apfs_writepage_endio_finish_ordered(struct apfs_inode *inode,
					  struct page *page, u64 start,
					  u64 end, int uptodate);
extern const struct dentry_operations apfs_dentry_operations;
extern const struct iomap_ops apfs_dio_iomap_ops;
extern const struct iomap_dio_ops apfs_dio_ops;

/* Inode locking type flags, by default the exclusive lock is taken */
#define APFS_ILOCK_SHARED	(1U << 0)
#define APFS_ILOCK_TRY 	(1U << 1)
#define APFS_ILOCK_MMAP	(1U << 2)

int apfs_inode_lock(struct inode *inode, unsigned int ilock_flags);
void apfs_inode_unlock(struct inode *inode, unsigned int ilock_flags);
void apfs_update_inode_bytes(struct apfs_inode *inode,
			      const u64 add_bytes,
			      const u64 del_bytes);

/* ioctl.c */
long apfs_ioctl(struct file *file, unsigned int cmd, unsigned long arg);
long apfs_compat_ioctl(struct file *file, unsigned int cmd, unsigned long arg);
int apfs_fileattr_get(struct dentry *dentry, struct fileattr *fa);
int apfs_fileattr_set(struct user_namespace *mnt_userns,
		       struct dentry *dentry, struct fileattr *fa);
int apfs_ioctl_get_supported_features(void __user *arg);
void apfs_sync_bsd_flags_to_i_flags(struct inode *inode);
int __pure apfs_is_empty_uuid(u8 *uuid);
int apfs_defrag_file(struct inode *inode, struct file *file,
		      struct apfs_ioctl_defrag_range_args *range,
		      u64 newer_than, unsigned long max_pages);
void apfs_get_block_group_info(struct list_head *groups_list,
				struct apfs_ioctl_space_info *space);
void apfs_update_ioctl_balance_args(struct apfs_fs_info *fs_info,
			       struct apfs_ioctl_balance_args *bargs);
bool apfs_exclop_start(struct apfs_fs_info *fs_info,
			enum apfs_exclusive_operation type);
bool apfs_exclop_start_try_lock(struct apfs_fs_info *fs_info,
				 enum apfs_exclusive_operation type);
void apfs_exclop_start_unlock(struct apfs_fs_info *fs_info);
void apfs_exclop_finish(struct apfs_fs_info *fs_info);

/* file.c */
int __init apfs_auto_defrag_init(void);
void __cold apfs_auto_defrag_exit(void);
int apfs_add_inode_defrag(struct apfs_trans_handle *trans,
			   struct apfs_inode *inode);
int apfs_run_defrag_inodes(struct apfs_fs_info *fs_info);
void apfs_cleanup_defrag_inodes(struct apfs_fs_info *fs_info);
int apfs_sync_file(struct file *file, loff_t start, loff_t end, int datasync);
void apfs_drop_extent_cache(struct apfs_inode *inode, u64 start, u64 end,
			     int skip_pinned);
extern const struct file_operations apfs_file_operations;
int apfs_drop_extents(struct apfs_trans_handle *trans,
		       struct apfs_root *root, struct apfs_inode *inode,
		       struct apfs_drop_extents_args *args);
int apfs_replace_file_extents(struct apfs_inode *inode,
			   struct apfs_path *path, const u64 start,
			   const u64 end,
			   struct apfs_replace_extent_info *extent_info,
			   struct apfs_trans_handle **trans_out);
int apfs_mark_extent_written(struct apfs_trans_handle *trans,
			      struct apfs_inode *inode, u64 start, u64 end);
int apfs_release_file(struct inode *inode, struct file *file);
int apfs_dirty_pages(struct apfs_inode *inode, struct page **pages,
		      size_t num_pages, loff_t pos, size_t write_bytes,
		      struct extent_state **cached, bool noreserve);
int apfs_fdatawrite_range(struct inode *inode, loff_t start, loff_t end);
int apfs_check_nocow_lock(struct apfs_inode *inode, loff_t pos,
			   size_t *write_bytes);
void apfs_check_nocow_unlock(struct apfs_inode *inode);

/* tree-defrag.c */
int apfs_defrag_leaves(struct apfs_trans_handle *trans,
			struct apfs_root *root);

/* super.c */
int apfs_parse_options(struct apfs_fs_info *info, char *options,
			unsigned long new_flags);
int apfs_sync_fs(struct super_block *sb, int wait);
char *apfs_get_subvol_name_from_objectid(struct apfs_fs_info *fs_info,
					  u64 subvol_objectid);



/* compatibility and incompatibility defines */

#define apfs_set_fs_incompat(__fs_info, opt) \
	__apfs_set_fs_incompat((__fs_info), APFS_FEATURE_INCOMPAT_##opt, \
				#opt)

static inline void __apfs_set_fs_incompat(struct apfs_fs_info *fs_info,
					   u64 flag, const char* name)
{
	struct apfs_super_block *disk_super;
	u64 features;

	disk_super = fs_info->super_copy;
	features = apfs_super_incompat_flags(disk_super);
	if (!(features & flag)) {
		spin_lock(&fs_info->super_lock);
		features = apfs_super_incompat_flags(disk_super);
		if (!(features & flag)) {
			features |= flag;
			apfs_set_super_incompat_flags(disk_super, features);
			apfs_info(fs_info,
				"setting incompat feature flag for %s (0x%llx)",
				name, flag);
		}
		spin_unlock(&fs_info->super_lock);
	}
}

#define apfs_clear_fs_incompat(__fs_info, opt) \
	__apfs_clear_fs_incompat((__fs_info), APFS_FEATURE_INCOMPAT_##opt, \
				  #opt)

static inline void __apfs_clear_fs_incompat(struct apfs_fs_info *fs_info,
					     u64 flag, const char* name)
{
	struct apfs_super_block *disk_super;
	u64 features;

	disk_super = fs_info->super_copy;
	features = apfs_super_incompat_flags(disk_super);
	if (features & flag) {
		spin_lock(&fs_info->super_lock);
		features = apfs_super_incompat_flags(disk_super);
		if (features & flag) {
			features &= ~flag;
			apfs_set_super_incompat_flags(disk_super, features);
			apfs_info(fs_info,
				"clearing incompat feature flag for %s (0x%llx)",
				name, flag);
		}
		spin_unlock(&fs_info->super_lock);
	}
}

#define apfs_fs_incompat(fs_info, opt) \
	__apfs_fs_incompat((fs_info), APFS_FEATURE_INCOMPAT_##opt)

static inline bool __apfs_fs_incompat(struct apfs_fs_info *fs_info, u64 flag)
{
	struct apfs_super_block *disk_super;
	disk_super = fs_info->super_copy;
	return !!(apfs_super_incompat_flags(disk_super) & flag);
}

#define apfs_set_fs_compat_ro(__fs_info, opt) \
	__apfs_set_fs_compat_ro((__fs_info), APFS_FEATURE_COMPAT_RO_##opt, \
				 #opt)

static inline void __apfs_set_fs_compat_ro(struct apfs_fs_info *fs_info,
					    u64 flag, const char *name)
{
	struct apfs_super_block *disk_super;
	u64 features;

	disk_super = fs_info->super_copy;
	features = apfs_super_compat_ro_flags(disk_super);
	if (!(features & flag)) {
		spin_lock(&fs_info->super_lock);
		features = apfs_super_compat_ro_flags(disk_super);
		if (!(features & flag)) {
			features |= flag;
			apfs_set_super_compat_ro_flags(disk_super, features);
			apfs_info(fs_info,
				"setting compat-ro feature flag for %s (0x%llx)",
				name, flag);
		}
		spin_unlock(&fs_info->super_lock);
	}
}

#define apfs_clear_fs_compat_ro(__fs_info, opt) \
	__apfs_clear_fs_compat_ro((__fs_info), APFS_FEATURE_COMPAT_RO_##opt, \
				   #opt)

static inline void __apfs_clear_fs_compat_ro(struct apfs_fs_info *fs_info,
					      u64 flag, const char *name)
{
	struct apfs_super_block *disk_super;
	u64 features;

	disk_super = fs_info->super_copy;
	features = apfs_super_compat_ro_flags(disk_super);
	if (features & flag) {
		spin_lock(&fs_info->super_lock);
		features = apfs_super_compat_ro_flags(disk_super);
		if (features & flag) {
			features &= ~flag;
			apfs_set_super_compat_ro_flags(disk_super, features);
			apfs_info(fs_info,
				"clearing compat-ro feature flag for %s (0x%llx)",
				name, flag);
		}
		spin_unlock(&fs_info->super_lock);
	}
}

#define apfs_fs_compat_ro(fs_info, opt) \
	__apfs_fs_compat_ro((fs_info), APFS_FEATURE_COMPAT_RO_##opt)

static inline int __apfs_fs_compat_ro(struct apfs_fs_info *fs_info, u64 flag)
{
	struct apfs_super_block *disk_super;
	disk_super = fs_info->super_copy;
	return !!(apfs_super_compat_ro_flags(disk_super) & flag);
}

/* acl.c */
#ifdef CONFIG_APFS_FS_POSIX_ACL
struct posix_acl *apfs_get_acl(struct inode *inode, int type, bool rcu);
int apfs_set_acl(struct user_namespace *mnt_userns, struct inode *inode,
		  struct posix_acl *acl, int type);
int apfs_init_acl(struct apfs_trans_handle *trans,
		   struct inode *inode, struct inode *dir);
#else
#define apfs_get_acl NULL
#define apfs_set_acl NULL
static inline int apfs_init_acl(struct apfs_trans_handle *trans,
				 struct inode *inode, struct inode *dir)
{
	return 0;
}
#endif

/* relocation.c */
int apfs_relocate_block_group(struct apfs_fs_info *fs_info, u64 group_start);
int apfs_init_reloc_root(struct apfs_trans_handle *trans,
			  struct apfs_root *root);
int apfs_update_reloc_root(struct apfs_trans_handle *trans,
			    struct apfs_root *root);
int apfs_recover_relocation(struct apfs_root *root);
int apfs_reloc_clone_csums(struct apfs_inode *inode, u64 file_pos, u64 len);
int apfs_reloc_cow_block(struct apfs_trans_handle *trans,
			  struct apfs_root *root, struct extent_buffer *buf,
			  struct extent_buffer *cow);
void apfs_reloc_pre_snapshot(struct apfs_pending_snapshot *pending,
			      u64 *bytes_to_reserve);
int apfs_reloc_post_snapshot(struct apfs_trans_handle *trans,
			      struct apfs_pending_snapshot *pending);
int apfs_should_cancel_balance(struct apfs_fs_info *fs_info);
struct apfs_root *find_reloc_root(struct apfs_fs_info *fs_info,
				   u64 bytenr);
int apfs_should_ignore_reloc_root(struct apfs_root *root);

/* scrub.c */
int apfs_scrub_dev(struct apfs_fs_info *fs_info, u64 devid, u64 start,
		    u64 end, struct apfs_scrub_progress *progress,
		    int readonly, int is_dev_replace);
void apfs_scrub_pause(struct apfs_fs_info *fs_info);
void apfs_scrub_continue(struct apfs_fs_info *fs_info);
int apfs_scrub_cancel(struct apfs_fs_info *info);
int apfs_scrub_cancel_dev(struct apfs_device *dev);
int apfs_scrub_progress(struct apfs_fs_info *fs_info, u64 devid,
			 struct apfs_scrub_progress *progress);
static inline void apfs_init_full_stripe_locks_tree(
			struct apfs_full_stripe_locks_tree *locks_root)
{
	locks_root->root = RB_ROOT;
	mutex_init(&locks_root->lock);
}

/* dev-replace.c */
void apfs_bio_counter_inc_blocked(struct apfs_fs_info *fs_info);
void apfs_bio_counter_inc_noblocked(struct apfs_fs_info *fs_info);
void apfs_bio_counter_sub(struct apfs_fs_info *fs_info, s64 amount);

static inline void apfs_bio_counter_dec(struct apfs_fs_info *fs_info)
{
	apfs_bio_counter_sub(fs_info, 1);
}

/* reada.c */
struct reada_control {
	struct apfs_fs_info	*fs_info;		/* tree to prefetch */
	struct apfs_key	key_start;
	struct apfs_key	key_end;	/* exclusive */
	atomic_t		elems;
	struct kref		refcnt;
	wait_queue_head_t	wait;
};
struct reada_control *apfs_reada_add(struct apfs_root *root,
			      struct apfs_key *start, struct apfs_key *end);
int apfs_reada_wait(void *handle);
void apfs_reada_detach(void *handle);
int btree_readahead_hook(struct extent_buffer *eb, int err);
void apfs_reada_remove_dev(struct apfs_device *dev);
void apfs_reada_undo_remove_dev(struct apfs_device *dev);

static inline int is_fstree(u64 rootid)
{
	if (rootid == APFS_FS_TREE_OBJECTID ||
	    ((s64)rootid >= (s64)APFS_FIRST_FREE_OBJECTID &&
	      !apfs_qgroup_level(rootid)))
		return 1;
	return 0;
}

static inline int apfs_defrag_cancelled(struct apfs_fs_info *fs_info)
{
	return signal_pending(current);
}

/* Sanity test specific functions */
#ifdef CONFIG_APFS_FS_RUN_SANITY_TESTS
void apfs_test_destroy_inode(struct inode *inode);
static inline int apfs_is_testing(struct apfs_fs_info *fs_info)
{
	return test_bit(APFS_FS_STATE_DUMMY_FS_INFO, &fs_info->fs_state);
}
#else
static inline int apfs_is_testing(struct apfs_fs_info *fs_info)
{
	return 0;
}
#endif

static inline bool apfs_is_zoned(const struct apfs_fs_info *fs_info)
{
	return fs_info->zoned != 0;
}

/* end of btrfs layouts */

/*
 * We use page status Private2 to indicate there is an ordered extent with
 * unfinished IO.
 *
 * Rename the Private2 accessors to Ordered, to improve readability.
 */
#define PageOrdered(page)		PagePrivate2(page)
#define SetPageOrdered(page)		SetPagePrivate2(page)
#define ClearPageOrdered(page)		ClearPagePrivate2(page)

/*
 * Note that this is not a generic implementation of fletcher64, as it assumes
 * a message length that doesn't overflow sum1 and sum2.  This constraint is ok
 * for apfs, though, since the block size is limited to 2^16.  For a more
 * generic optimized implementation, see Nakassis (1988).
 */
static u64 apfs_fletcher64(const void *addr, size_t len)
{
	const __le32 *buff = addr;
	u64 sum1 = 0;
	u64 sum2 = 0;
	u64 c1, c2;
	int i;

	for (i = 0; i < len / sizeof(u32); i++) {
		sum1 += le32_to_cpu(buff[i]);
		sum2 += sum1;
	}

	c1 = sum1 + sum2;
	c1 = 0xFFFFFFFF - c1 % (u64)0xFFFFFFFF;
	c2 = sum1 + c1;
	c2 = 0xFFFFFFFF - c2 % (u64)0xFFFFFFFF;

	return (c2 << 32) | c1;
}

static inline u64
apfs_generate_csum(const void *addr, size_t len)
{
	return apfs_fletcher64(addr, len);
}

static inline int
apfs_verify_obj_csum(const void *ptr, unsigned long len)
{
	u64 csum = apfs_generate_csum(ptr + APFS_CSUM_SIZE,
				      len - APFS_CSUM_SIZE);

	if (memcmp(&csum, ptr, APFS_CSUM_SIZE))
		return -EUCLEAN;
	return 0;
}

static inline bool
apfs_item_has_xfields_nr(struct extent_buffer *eb, int nr)
{
	struct apfs_key key = {};
	u32 item_len;

	if (apfs_header_subtype(eb) != APFS_OBJ_TYPE_FSTREE)
		return false;

	item_len = apfs_item_size_nr(eb, nr);
	apfs_item_key_to_cpu(eb, &key, nr);

	if (key.type == APFS_TYPE_INODE)
		return item_len != sizeof(struct apfs_inode_val);
	if (key.type == APFS_TYPE_DIR_REC)
		return item_len != sizeof(struct apfs_drec_item);

	return false;
}

struct apfs_xfield *
apfs_find_xfield(struct extent_buffer *eb, struct apfs_xfield_blob *xb,
		 enum apfs_xfield_type type, unsigned long start,
		 unsigned long *offset_res);

unsigned long
apfs_xfield_ext_offset(struct extent_buffer *eb, struct apfs_xfield_blob *xb,
		       int nr);
struct apfs_root *apfs_read_root(struct apfs_fs_info *fs_info, u8 type,
				 u64 bytenr);

bool apfs_inode_is_compressed(const struct apfs_inode *ai);
bool apfs_compress_data_inlined(u32 type);
bool apfs_compress_data_resource(u32 type);

static inline bool apfs_xattr_data_embedded(const struct extent_buffer *eb,
					    const struct apfs_xattr_item *xi)
{
	return apfs_xattr_item_flags(eb, xi) & APFS_XATTR_DATA_EMBEDDED;
}

#define APFS_COMPRESS_CDATA_SIZE sizeof(u8)

struct apfs_compress_header {
	__le32 signature;
	__le32 type; // compress algorithm type
	__le64 size; // the file size after compres
};

APFS_SETGET_FUNCS(compress_header_signature, struct apfs_compress_header,
		  signature, 32);
APFS_SETGET_FUNCS(compress_header_type, struct apfs_compress_header, type, 32);
APFS_SETGET_FUNCS(compress_header_size, struct apfs_compress_header, size, 64);
APFS_SETGET_STACK_FUNCS(stack_compress_header_signature, struct apfs_compress_header,
			signature, 32);
APFS_SETGET_STACK_FUNCS(stack_compress_header_type, struct apfs_compress_header,
			type, 32);
APFS_SETGET_STACK_FUNCS(stack_compress_header_size, struct apfs_compress_header,
			size, 64);

struct apfs_resource_fork_header {
	__be32 data_offset;
	__be32 mng_offset;
	__be32 data_size;
	__be32 mng_size;
};

static inline u32
apfs_resource_fork_data_offset(const struct apfs_resource_fork_header *hdr)
{
	return be32_to_cpu(hdr->data_offset);
}

static inline u32
apfs_resource_fork_mg_offset(const struct apfs_resource_fork_header *hdr)
{
	return be32_to_cpu(hdr->mng_offset);
}

static inline u32
apfs_resource_fork_data_size(const struct apfs_resource_fork_header *hdr)
{
	return be32_to_cpu(hdr->data_size);
}

static inline u32
apfs_resource_fork_mg_size(const struct apfs_resource_fork_header *hdr)
{
	return be32_to_cpu(hdr->mng_size);
}

struct apfs_resource_fork_entry
{
	// 1 64K-Block
	__le32 off;
	__le32 size;
};

APFS_SETGET_STACK_FUNCS(resource_fork_entry_off, struct apfs_resource_fork_entry,
			off, 32);
APFS_SETGET_STACK_FUNCS(resource_fork_entry_size, struct apfs_resource_fork_entry,
			size, 32);

struct apfs_resource_fork_entries
{
	u32 count;
	struct apfs_resource_fork_entry entry[0];
};

APFS_SETGET_STACK_FUNCS(resource_fork_entries_count, struct apfs_resource_fork_entries,
			count, 32);

struct apfs_resource_fork_data {
	__le16 unknown;
	__le16 count;

	struct apfs_resource_fork_entries entries[0];
} __attribute__((__packed__));

APFS_SETGET_STACK_FUNCS(stack_resource_fork_data_field1,
			struct apfs_resource_fork_data, unknown, 16);
APFS_SETGET_STACK_FUNCS(stack_resource_fork_data_count,
			struct apfs_resource_fork_data, count, 16);
APFS_SETGET_FUNCS(resource_fork_data_field1, struct apfs_resource_fork_data,
			unknown, 16);
APFS_SETGET_FUNCS(resource_fork_data_count, struct apfs_resource_fork_data,
			count, 16);
/* Return entry offset of the entries start */
static inline u32
apfs_resource_fork_data_offset_nr(struct apfs_resource_fork_entries *entries,
				  int nr)
{
	/* apfs_resource_fork_entries::u32 entries */
	return le32_to_cpu(entries->entry[nr].off);
}

static inline u32
apfs_resource_fork_data_size_nr(struct apfs_resource_fork_entries *entries,
				  int nr)
{
	return le32_to_cpu(entries->entry[nr].size);
}

static inline u32
apfs_calculate_map_len(u64 size)
{
	return (((size + 0xFFFF) & 0xFFFF0000) >> 16) + 1;
}

int apfs_read_extent_page_map(struct apfs_inode *inode,
			      struct page *page, u64 bytenr,
			      u64 start, u64 end);

/* return ERR if something wrong*/
static inline
void *apfs_read_cache_page_unaligned(struct address_space *mapping,
				     u64 bytenr, struct page **page)

{
	u32 offset = bytenr % PAGE_SIZE; /* what if blocksize < PAGE_SIZE ? */

	if (offset)
		bytenr -= offset;

	*page = read_cache_page_gfp(mapping,  bytenr >> PAGE_SHIFT,
				   GFP_NOFS);
	if (IS_ERR(*page))
		return *page;
	return kmap(*page) + offset;
}

static inline
void apfs_put_page(struct page *page)

{
	kunmap(page);
	put_page(page);
}

struct extent_map *
apfs_compressed_extent_item_to_extent_map(struct apfs_inode *inode,
				     const struct apfs_path *path,
				     struct page *page,
				     u64 start, u64 len);

bool apfs_inode_data_in_dstream(struct apfs_inode *inode);

struct apfs_xattr_item *apfs_lookup_xattr_item(struct apfs_trans_handle *trans,
					       struct apfs_root *root,
					       struct apfs_path *path, u64 dir,
					       const char *name, int mod);
#endif
