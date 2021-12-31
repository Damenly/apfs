/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2012 Alexander Block.  All rights reserved.
 * Copyright (C) 2012 STRATO.  All rights reserved.
 */

#ifndef APFS_SEND_H
#define APFS_SEND_H

#include "ctree.h"

#define APFS_SEND_STREAM_MAGIC "apfs-stream"
#define APFS_SEND_STREAM_VERSION 1

#define APFS_SEND_BUF_SIZE SZ_64K

enum apfs_tlv_type {
	APFS_TLV_U8,
	APFS_TLV_U16,
	APFS_TLV_U32,
	APFS_TLV_U64,
	APFS_TLV_BINARY,
	APFS_TLV_STRING,
	APFS_TLV_UUID,
	APFS_TLV_TIMESPEC,
};

struct apfs_stream_header {
	char magic[sizeof(APFS_SEND_STREAM_MAGIC)];
	__le32 version;
} __attribute__ ((__packed__));

struct apfs_cmd_header {
	/* len excluding the header */
	__le32 len;
	__le16 cmd;
	/* crc including the header with zero crc field */
	__le32 crc;
} __attribute__ ((__packed__));

struct apfs_tlv_header {
	__le16 tlv_type;
	/* len excluding the header */
	__le16 tlv_len;
} __attribute__ ((__packed__));

/* commands */
enum apfs_send_cmd {
	APFS_SEND_C_UNSPEC,

	APFS_SEND_C_SUBVOL,
	APFS_SEND_C_SNAPSHOT,

	APFS_SEND_C_MKFILE,
	APFS_SEND_C_MKDIR,
	APFS_SEND_C_MKNOD,
	APFS_SEND_C_MKFIFO,
	APFS_SEND_C_MKSOCK,
	APFS_SEND_C_SYMLINK,

	APFS_SEND_C_RENAME,
	APFS_SEND_C_LINK,
	APFS_SEND_C_UNLINK,
	APFS_SEND_C_RMDIR,

	APFS_SEND_C_SET_XATTR,
	APFS_SEND_C_REMOVE_XATTR,

	APFS_SEND_C_WRITE,
	APFS_SEND_C_CLONE,

	APFS_SEND_C_TRUNCATE,
	APFS_SEND_C_CHMOD,
	APFS_SEND_C_CHOWN,
	APFS_SEND_C_UTIMES,

	APFS_SEND_C_END,
	APFS_SEND_C_UPDATE_EXTENT,
	__APFS_SEND_C_MAX,
};
#define APFS_SEND_C_MAX (__APFS_SEND_C_MAX - 1)

/* attributes in send stream */
enum {
	APFS_SEND_A_UNSPEC,

	APFS_SEND_A_UUID,
	APFS_SEND_A_CTRANSID,

	APFS_SEND_A_INO,
	APFS_SEND_A_SIZE,
	APFS_SEND_A_MODE,
	APFS_SEND_A_UID,
	APFS_SEND_A_GID,
	APFS_SEND_A_RDEV,
	APFS_SEND_A_CTIME,
	APFS_SEND_A_MTIME,
	APFS_SEND_A_ATIME,
	APFS_SEND_A_OTIME,

	APFS_SEND_A_XATTR_NAME,
	APFS_SEND_A_XATTR_DATA,

	APFS_SEND_A_PATH,
	APFS_SEND_A_PATH_TO,
	APFS_SEND_A_PATH_LINK,

	APFS_SEND_A_FILE_OFFSET,
	APFS_SEND_A_DATA,

	APFS_SEND_A_CLONE_UUID,
	APFS_SEND_A_CLONE_CTRANSID,
	APFS_SEND_A_CLONE_PATH,
	APFS_SEND_A_CLONE_OFFSET,
	APFS_SEND_A_CLONE_LEN,

	__APFS_SEND_A_MAX,
};
#define APFS_SEND_A_MAX (__APFS_SEND_A_MAX - 1)

#ifdef __KERNEL__
long apfs_ioctl_send(struct file *mnt_file, struct apfs_ioctl_send_args *arg);
#endif

#endif
