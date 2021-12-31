/* SPDX-License-Identifier: GPL-2.0 */

#ifndef APFS_DISCARD_H
#define APFS_DISCARD_H

#include <linux/sizes.h>

struct apfs_fs_info;
struct apfs_discard_ctl;
struct apfs_block_group;

/* Discard size limits */
#define APFS_ASYNC_DISCARD_DEFAULT_MAX_SIZE		(SZ_64M)
#define APFS_ASYNC_DISCARD_MAX_FILTER			(SZ_1M)
#define APFS_ASYNC_DISCARD_MIN_FILTER			(SZ_32K)

/* List operations */
void apfs_discard_check_filter(struct apfs_block_group *block_group, u64 bytes);

/* Work operations */
void apfs_discard_cancel_work(struct apfs_discard_ctl *discard_ctl,
			       struct apfs_block_group *block_group);
void apfs_discard_queue_work(struct apfs_discard_ctl *discard_ctl,
			      struct apfs_block_group *block_group);
void apfs_discard_schedule_work(struct apfs_discard_ctl *discard_ctl,
				 bool override);
bool apfs_run_discard_work(struct apfs_discard_ctl *discard_ctl);

/* Update operations */
void apfs_discard_calc_delay(struct apfs_discard_ctl *discard_ctl);
void apfs_discard_update_discardable(struct apfs_block_group *block_group);

/* Setup/cleanup operations */
void apfs_discard_punt_unused_bgs_list(struct apfs_fs_info *fs_info);
void apfs_discard_resume(struct apfs_fs_info *fs_info);
void apfs_discard_stop(struct apfs_fs_info *fs_info);
void apfs_discard_init(struct apfs_fs_info *fs_info);
void apfs_discard_cleanup(struct apfs_fs_info *fs_info);

#endif
