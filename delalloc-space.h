/* SPDX-License-Identifier: GPL-2.0 */

#ifndef APFS_DELALLOC_SPACE_H
#define APFS_DELALLOC_SPACE_H

struct extent_changeset;

int apfs_alloc_data_chunk_ondemand(struct apfs_inode *inode, u64 bytes);
int apfs_check_data_free_space(struct apfs_inode *inode,
			struct extent_changeset **reserved, u64 start, u64 len);
void apfs_free_reserved_data_space(struct apfs_inode *inode,
			struct extent_changeset *reserved, u64 start, u64 len);
void apfs_delalloc_release_space(struct apfs_inode *inode,
				  struct extent_changeset *reserved,
				  u64 start, u64 len, bool qgroup_free);
void apfs_free_reserved_data_space_noquota(struct apfs_fs_info *fs_info,
					    u64 len);
void apfs_delalloc_release_metadata(struct apfs_inode *inode, u64 num_bytes,
				     bool qgroup_free);
int apfs_delalloc_reserve_space(struct apfs_inode *inode,
			struct extent_changeset **reserved, u64 start, u64 len);

#endif /* APFS_DELALLOC_SPACE_H */
