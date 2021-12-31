/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) STRATO AG 2011.  All rights reserved.
 */

#ifndef APFS_CHECK_INTEGRITY_H
#define APFS_CHECK_INTEGRITY_H

#ifdef CONFIG_APFS_FS_CHECK_INTEGRITY
void apfsic_submit_bio(struct bio *bio);
int apfsic_submit_bio_wait(struct bio *bio);
#else
#define apfsic_submit_bio submit_bio
#define apfsic_submit_bio_wait submit_bio_wait
#endif

int apfsic_mount(struct apfs_fs_info *fs_info,
		  struct apfs_fs_devices *fs_devices,
		  int including_extent_data, u32 print_mask);
void apfsic_unmount(struct apfs_fs_devices *fs_devices);

#endif
