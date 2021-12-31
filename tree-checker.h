/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) Qu Wenruo 2017.  All rights reserved.
 */

#ifndef APFS_TREE_CHECKER_H
#define APFS_TREE_CHECKER_H

#include "ctree.h"
#include "extent_io.h"

/*
 * Comprehensive leaf checker.
 * Will check not only the item pointers, but also every possible member
 * in item data.
 */
int apfs_check_leaf_full(struct extent_buffer *leaf);

/*
 * Less strict leaf checker.
 * Will only check item pointers, not reading item data.
 */
int apfs_check_leaf_relaxed(struct extent_buffer *leaf);
int apfs_check_node(struct extent_buffer *node);

int apfs_check_chunk_valid(struct extent_buffer *leaf,
			    struct apfs_chunk *chunk, u64 logical);

#endif
