// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2021, Su Yue
 * All Rights Reserved.
 */
#include "apfs.h"
#include "apfs_tree.h"
#include "apfs_inode.h"
#include "ctree.h"
#include "extent_map.h"
#include "ordered-data.h"
#include "delayed-ref.h"
#include "block-group.h"
#include "free-space-cache.h"
#include "volumes.h"
#include "async-thread.h"
#include "qgroup.h"
#include "extent_io.h"
#include "disk-io.h"
#include "backref.h"
#include "space-info.h"

/*
 * We include this last to have the helpers above available for the trace
 * event implementations.
 */
#define CREATE_TRACE_POINTS
#include "apfs_trace.h"
