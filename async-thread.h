/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2007 Oracle.  All rights reserved.
 * Copyright (C) 2014 Fujitsu.  All rights reserved.
 */

#ifndef APFS_ASYNC_THREAD_H
#define APFS_ASYNC_THREAD_H

#include <linux/workqueue.h>

struct apfs_fs_info;
struct apfs_workqueue;
/* Internal use only */
struct __apfs_workqueue;
struct apfs_work;
typedef void (*apfs_func_t)(struct apfs_work *arg);
typedef void (*apfs_work_func_t)(struct work_struct *arg);

struct apfs_work {
	apfs_func_t func;
	apfs_func_t ordered_func;
	apfs_func_t ordered_free;

	/* Don't touch things below */
	struct work_struct normal_work;
	struct list_head ordered_list;
	struct __apfs_workqueue *wq;
	unsigned long flags;
};

struct apfs_workqueue *apfs_alloc_workqueue(struct apfs_fs_info *fs_info,
					      const char *name,
					      unsigned int flags,
					      int limit_active,
					      int thresh);
void apfs_init_work(struct apfs_work *work, apfs_func_t func,
		     apfs_func_t ordered_func, apfs_func_t ordered_free);
void apfs_queue_work(struct apfs_workqueue *wq,
		      struct apfs_work *work);
void apfs_destroy_workqueue(struct apfs_workqueue *wq);
void apfs_workqueue_set_max(struct apfs_workqueue *wq, int max);
void apfs_set_work_high_priority(struct apfs_work *work);
struct apfs_fs_info * __pure apfs_work_owner(const struct apfs_work *work);
struct apfs_fs_info * __pure apfs_workqueue_owner(const struct __apfs_workqueue *wq);
bool apfs_workqueue_normal_congested(const struct apfs_workqueue *wq);
void apfs_flush_workqueue(struct apfs_workqueue *wq);

#endif
