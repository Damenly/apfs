/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2007 Oracle.  All rights reserved.
 */

#ifndef APFS_PRINT_TREE_H
#define APFS_PRINT_TREE_H

/* Buffer size to contain tree name and possibly additional data (offset) */
#define APFS_ROOT_NAME_BUF_LEN				48

void apfs_print_leaf(struct extent_buffer *l);
void apfs_print_tree(struct extent_buffer *c, bool follow);
const char *apfs_root_name(const struct apfs_key *key, char *buf);
void apfs_print_key(const struct extent_buffer *eb,
		    const struct apfs_key *key);
void print_fskey_type(u8 type);
#endif
