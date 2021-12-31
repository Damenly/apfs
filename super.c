// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2007 Oracle.  All rights reserved.
 */

#include <linux/blkdev.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/highmem.h>
#include <linux/time.h>
#include <linux/init.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/backing-dev.h>
#include <linux/mount.h>
#include <linux/writeback.h>
#include <linux/statfs.h>
#include <linux/compat.h>
#include <linux/parser.h>
#include <linux/ctype.h>
#include <linux/namei.h>
#include <linux/miscdevice.h>
#include <linux/magic.h>
#include <linux/slab.h>
#include <linux/cleancache.h>
#include <linux/ratelimit.h>
#include <linux/crc32c.h>
#include "apfs.h"
#include "ctree.h"
#include "disk-io.h"
#include "transaction.h"
#include "apfs_inode.h"
#include "print-tree.h"
#include "props.h"
#include "xattr.h"
#include "volumes.h"
#include "export.h"
#include "compression.h"
#include "rcu-string.h"
#include "dev-replace.h"
#include "free-space-cache.h"
#include "backref.h"
#include "space-info.h"
#include "sysfs.h"
#include "zoned.h"
#include "tests/apfs-tests.h"
#include "block-group.h"
#include "discard.h"
#include "qgroup.h"
#include "apfs_trace.h"

static const struct super_operations apfs_super_ops;

/*
 * Types for mounting the default subvolume and a subvolume explicitly
 * requested by subvol=/path. That way the callchain is straightforward and we
 * don't have to play tricks with the mount options and recursive calls to
>84;0;0c * apfs_mount.
 *
 * The new apfs_root_fs_type also servers as a tag for the bdev_holder.
 */
static struct file_system_type apfs_fs_type;
static struct file_system_type apfs_root_fs_type;

static int apfs_remount(struct super_block *sb, int *flags, char *data);

/*
 * Generally the error codes correspond to their respective errors, but there
 * are a few special cases.
 *
 * EUCLEAN: Any sort of corruption that we encounter.  The tree-checker for
 *          instance will return EUCLEAN if any of the blocks are corrupted in
 *          a way that is problematic.  We want to reserve EUCLEAN for these
 *          sort of corruptions.
 *
 * EROFS: If we check APFS_FS_STATE_ERROR and fail out with a return error, we
 *        need to use EROFS for this case.  We will have no idea of the
 *        original failure, that will have been reported at the time we tripped
 *        over the error.  Each subsequent error that doesn't have any context
 *        of the original error should use EROFS when handling APFS_FS_STATE_ERROR.
 */
const char * __attribute_const__ apfs_decode_error(int errno)
{
	char *errstr = "unknown";

	switch (errno) {
	case -ENOENT:		/* -2 */
		errstr = "No such entry";
		break;
	case -EIO:		/* -5 */
		errstr = "IO failure";
		break;
	case -ENOMEM:		/* -12*/
		errstr = "Out of memory";
		break;
	case -EEXIST:		/* -17 */
		errstr = "Object already exists";
		break;
	case -ENOSPC:		/* -28 */
		errstr = "No space left";
		break;
	case -EROFS:		/* -30 */
		errstr = "Readonly filesystem";
		break;
	case -EOPNOTSUPP:	/* -95 */
		errstr = "Operation not supported";
		break;
	case -EUCLEAN:		/* -117 */
		errstr = "Filesystem corrupted";
		break;
	case -EDQUOT:		/* -122 */
		errstr = "Quota exceeded";
		break;
	}

	return errstr;
}

/*
 * __apfs_handle_fs_error decodes expected errors from the caller and
 * invokes the appropriate error response.
 */
__cold
void __apfs_handle_fs_error(struct apfs_fs_info *fs_info, const char *function,
		       unsigned int line, int errno, const char *fmt, ...)
{
	struct super_block *sb = fs_info->sb;
#ifdef CONFIG_PRINTK
	const char *errstr;
#endif

	/*
	 * Special case: if the error is EROFS, and we're already
	 * under SB_RDONLY, then it is safe here.
	 */
	if (errno == -EROFS && sb_rdonly(sb))
  		return;

#ifdef CONFIG_PRINTK
	errstr = apfs_decode_error(errno);
	if (fmt) {
		struct va_format vaf;
		va_list args;

		va_start(args, fmt);
		vaf.fmt = fmt;
		vaf.va = &args;

		pr_crit("APFS: error (device %s) in %s:%d: errno=%d %s (%pV)\n",
			sb->s_id, function, line, errno, errstr, &vaf);
		va_end(args);
	} else {
		pr_crit("APFS: error (device %s) in %s:%d: errno=%d %s\n",
			sb->s_id, function, line, errno, errstr);
	}
#endif

	/*
	 * Today we only save the error info to memory.  Long term we'll
	 * also send it down to the disk
	 */
	set_bit(APFS_FS_STATE_ERROR, &fs_info->fs_state);

	/* Don't go through full error handling during mount */
	if (!(sb->s_flags & SB_BORN))
		return;

	if (sb_rdonly(sb))
		return;

	apfs_discard_stop(fs_info);

	/* apfs handle error by forcing the filesystem readonly */
	apfs_set_sb_rdonly(sb);
	apfs_info(fs_info, "forced readonly");
	/*
	 * Note that a running device replace operation is not canceled here
	 * although there is no way to update the progress. It would add the
	 * risk of a deadlock, therefore the canceling is omitted. The only
	 * penalty is that some I/O remains active until the procedure
	 * completes. The next time when the filesystem is mounted writable
	 * again, the device replace operation continues.
	 */
}

#ifdef CONFIG_PRINTK
static const char * const logtypes[] = {
	"emergency",
	"alert",
	"critical",
	"error",
	"warning",
	"notice",
	"info",
	"debug",
};


/*
 * Use one ratelimit state per log level so that a flood of less important
 * messages doesn't cause more important ones to be dropped.
 */
static struct ratelimit_state printk_limits[] = {
	RATELIMIT_STATE_INIT(printk_limits[0], DEFAULT_RATELIMIT_INTERVAL, 100),
	RATELIMIT_STATE_INIT(printk_limits[1], DEFAULT_RATELIMIT_INTERVAL, 100),
	RATELIMIT_STATE_INIT(printk_limits[2], DEFAULT_RATELIMIT_INTERVAL, 100),
	RATELIMIT_STATE_INIT(printk_limits[3], DEFAULT_RATELIMIT_INTERVAL, 100),
	RATELIMIT_STATE_INIT(printk_limits[4], DEFAULT_RATELIMIT_INTERVAL, 100),
	RATELIMIT_STATE_INIT(printk_limits[5], DEFAULT_RATELIMIT_INTERVAL, 100),
	RATELIMIT_STATE_INIT(printk_limits[6], DEFAULT_RATELIMIT_INTERVAL, 100),
	RATELIMIT_STATE_INIT(printk_limits[7], DEFAULT_RATELIMIT_INTERVAL, 100),
};

void __cold apfs_printk(const struct apfs_fs_info *fs_info, const char *fmt, ...)
{
	char lvl[PRINTK_MAX_SINGLE_HEADER_LEN + 1] = "\0";
	struct va_format vaf;
	va_list args;
	int kern_level;
	const char *type = logtypes[4];
	struct ratelimit_state *ratelimit = &printk_limits[4];

	va_start(args, fmt);

	while ((kern_level = printk_get_level(fmt)) != 0) {
		size_t size = printk_skip_level(fmt) - fmt;

		if (kern_level >= '0' && kern_level <= '7') {
			memcpy(lvl, fmt,  size);
			lvl[size] = '\0';
			type = logtypes[kern_level - '0'];
			ratelimit = &printk_limits[kern_level - '0'];
		}
		fmt += size;
	}

	vaf.fmt = fmt;
	vaf.va = &args;

	if (__ratelimit(ratelimit)) {
		if (fs_info)
			printk("%sAPFS %s (device %s index %d): %pV\n", lvl, type,
				fs_info->sb->s_id, fs_info->index, &vaf);
		else
			printk("%sAPFS %s: %pV\n", lvl, type, &vaf);
	}

	va_end(args);
}
#endif

#if BITS_PER_LONG == 32
void __cold apfs_warn_32bit_limit(struct apfs_fs_info *fs_info)
{
	if (!test_and_set_bit(APFS_FS_32BIT_WARN, &fs_info->flags)) {
		apfs_warn(fs_info, "reaching 32bit limit for logical addresses");
		apfs_warn(fs_info,
"due to page cache limit on 32bit systems, apfs can't access metadata at or beyond %lluT",
			   APFS_32BIT_MAX_FILE_SIZE >> 40);
		apfs_warn(fs_info,
			   "please consider upgrading to 64bit kernel/hardware");
	}
}

void __cold apfs_err_32bit_limit(struct apfs_fs_info *fs_info)
{
	if (!test_and_set_bit(APFS_FS_32BIT_ERROR, &fs_info->flags)) {
		apfs_err(fs_info, "reached 32bit limit for logical addresses");
		apfs_err(fs_info,
"due to page cache limit on 32bit systems, metadata beyond %lluT can't be accessed",
			  APFS_32BIT_MAX_FILE_SIZE >> 40);
		apfs_err(fs_info,
			   "please consider upgrading to 64bit kernel/hardware");
	}
}
#endif

/*
 * We only mark the transaction aborted and then set the file system read-only.
 * This will prevent new transactions from starting or trying to join this
 * one.
 *
 * This means that error recovery at the call site is limited to freeing
 * any local memory allocations and passing the error code up without
 * further cleanup. The transaction should complete as it normally would
 * in the call path but will return -EIO.
 *
 * We'll complete the cleanup in apfs_end_transaction and
 * apfs_commit_transaction.
 */
__cold
void __apfs_abort_transaction(struct apfs_trans_handle *trans,
			       const char *function,
			       unsigned int line, int errno)
{
	struct apfs_fs_info *fs_info = trans->fs_info;

	WRITE_ONCE(trans->aborted, errno);
	WRITE_ONCE(trans->transaction->aborted, errno);
	/* Wake up anybody who may be waiting on this transaction */
	wake_up(&fs_info->transaction_wait);
	wake_up(&fs_info->transaction_blocked_wait);
	__apfs_handle_fs_error(fs_info, function, line, errno, NULL);
}
/*
 * __apfs_panic decodes unexpected, fatal errors from the caller,
 * issues an alert, and either panics or BUGs, depending on mount options.
 */
__cold
void __apfs_panic(struct apfs_fs_info *fs_info, const char *function,
		   unsigned int line, int errno, const char *fmt, ...)
{
	char *s_id = "<unknown>";
	const char *errstr;
	struct va_format vaf = { .fmt = fmt };
	va_list args;

	if (fs_info)
		s_id = fs_info->sb->s_id;

	va_start(args, fmt);
	vaf.va = &args;

	errstr = apfs_decode_error(errno);
	if (fs_info && (apfs_test_opt(fs_info, PANIC_ON_FATAL_ERROR)))
		panic(KERN_CRIT "APFS panic (device %s) in %s:%d: %pV (errno=%d %s)\n",
			s_id, function, line, &vaf, errno, errstr);

	apfs_crit(fs_info, "panic in %s:%d: %pV (errno=%d %s)",
		   function, line, &vaf, errno, errstr);
	va_end(args);
	/* Caller calls BUG() */
}

static void apfs_put_super(struct super_block *sb)
{
	close_ctree(apfs_sb(sb));
}

enum {
	Opt_acl, Opt_noacl,
	Opt_clear_cache,
	Opt_commit_interval,
	Opt_compress,
	Opt_compress_force,
	Opt_compress_force_type,
	Opt_compress_type,
	Opt_degraded,
	Opt_device,
	Opt_fatal_errors,
	Opt_flushoncommit, Opt_noflushoncommit,
	Opt_max_inline,
	Opt_barrier, Opt_nobarrier,
	Opt_datacow, Opt_nodatacow,
	Opt_datasum, Opt_nodatasum,
	Opt_defrag, Opt_nodefrag,
	Opt_discard, Opt_nodiscard,
	Opt_discard_mode,
	Opt_norecovery,
	Opt_ratio,
	Opt_rescan_uuid_tree,
	Opt_skip_balance,
	Opt_space_cache, Opt_no_space_cache,
	Opt_space_cache_version,
	Opt_ssd, Opt_nossd,
	Opt_ssd_spread, Opt_nossd_spread,
	Opt_subvol,
	Opt_subvol_empty,
	Opt_subvolid,
	Opt_thread_pool,
	Opt_treelog, Opt_notreelog,
	Opt_user_subvol_rm_allowed,

	/* Rescue options */
	Opt_rescue,
	Opt_usebackuproot,
	Opt_nologreplay,
	Opt_ignorebadroots,
	Opt_ignoredatacsums,
	Opt_rescue_all,

	/* Deprecated options */
	Opt_recovery,
	Opt_inode_cache, Opt_noinode_cache,

	/* Debugging options */
	Opt_check_integrity,
	Opt_check_integrity_including_extent_data,
	Opt_check_integrity_print_mask,
	Opt_enospc_debug, Opt_noenospc_debug,
#ifdef CONFIG_APFS_DEBUG
	Opt_fragment_data, Opt_fragment_metadata, Opt_fragment_all,
#endif
#ifdef CONFIG_APFS_FS_REF_VERIFY
	Opt_ref_verify,
#endif
	Opt_err,
};

static const match_table_t tokens = {
	{Opt_subvol, "subvol=%s"},
	{Opt_subvol_empty, "subvol="},
	{Opt_subvolid, "subvolid=%s"},

#ifdef CONFIG_APFS_DEBUG
	{Opt_fragment_data, "fragment=data"},
	{Opt_fragment_metadata, "fragment=metadata"},
	{Opt_fragment_all, "fragment=all"},
#endif
#ifdef CONFIG_APFS_FS_REF_VERIFY
	{Opt_ref_verify, "ref_verify"},
#endif
	{Opt_err, NULL},
};

static const match_table_t rescue_tokens = {
	{Opt_usebackuproot, "usebackuproot"},
	{Opt_nologreplay, "nologreplay"},
	{Opt_ignorebadroots, "ignorebadroots"},
	{Opt_ignorebadroots, "ibadroots"},
	{Opt_ignoredatacsums, "ignoredatacsums"},
	{Opt_ignoredatacsums, "idatacsums"},
	{Opt_rescue_all, "all"},
	{Opt_err, NULL},
};

static bool check_ro_option(struct apfs_fs_info *fs_info, unsigned long opt,
			    const char *opt_name)
{
	if (fs_info->mount_opt & opt) {
		apfs_err(fs_info, "%s must be used with ro mount option",
			  opt_name);
		return true;
	}
	return false;
}

static int parse_rescue_options(struct apfs_fs_info *info, const char *options)
{
	char *opts;
	char *orig;
	char *p;
	substring_t args[MAX_OPT_ARGS];
	int ret = 0;

	opts = kstrdup(options, GFP_KERNEL);
	if (!opts)
		return -ENOMEM;
	orig = opts;

	while ((p = strsep(&opts, ":")) != NULL) {
		int token;

		if (!*p)
			continue;
		token = match_token(p, rescue_tokens, args);
		switch (token){
		case Opt_usebackuproot:
			apfs_info(info,
				   "trying to use backup root at mount time");
			apfs_set_opt(info->mount_opt, USEBACKUPROOT);
			break;
		case Opt_nologreplay:
			apfs_set_and_info(info, NOLOGREPLAY,
					   "disabling log replay at mount time");
			break;
		case Opt_ignorebadroots:
			apfs_set_and_info(info, IGNOREBADROOTS,
					   "ignoring bad roots");
			break;
		case Opt_ignoredatacsums:
			apfs_set_and_info(info, IGNOREDATACSUMS,
					   "ignoring data csums");
			break;
		case Opt_rescue_all:
			apfs_info(info, "enabling all of the rescue options");
			apfs_set_and_info(info, IGNOREDATACSUMS,
					   "ignoring data csums");
			apfs_set_and_info(info, IGNOREBADROOTS,
					   "ignoring bad roots");
			apfs_set_and_info(info, NOLOGREPLAY,
					   "disabling log replay at mount time");
			break;
		case Opt_err:
			apfs_info(info, "unrecognized rescue option '%s'", p);
			ret = -EINVAL;
			goto out;
		default:
			break;
		}

	}
out:
	kfree(orig);
	return ret;
}

/*
 * Regular mount options parser.  Everything that is needed only when
 * reading in a new superblock is parsed here.
 * XXX JDM: This needs to be cleaned up for remount.
 */
int apfs_parse_options(struct apfs_fs_info *info, char *options,
			unsigned long new_flags)
{
	substring_t args[MAX_OPT_ARGS];
	char *p, *num;
	int intarg;
	int ret = 0;
	char *compress_type;
	bool compress_force = false;
	enum apfs_compression_type saved_compress_type;
	int saved_compress_level;
	bool saved_compress_force;
	int no_compress = 0;

	if (apfs_fs_compat_ro(info, FREE_SPACE_TREE))
		apfs_set_opt(info->mount_opt, FREE_SPACE_TREE);
	else if (apfs_free_space_cache_v1_active(info)) {
		if (apfs_is_zoned(info)) {
			apfs_info(info,
			"zoned: clearing existing space cache");
			apfs_set_super_cache_generation(info->super_copy, 0);
		} else {
			apfs_set_opt(info->mount_opt, SPACE_CACHE);
		}
	}

	/*
	 * Even the options are empty, we still need to do extra check
	 * against new flags
	 */
	if (!options)
		goto check;

	while ((p = strsep(&options, ",")) != NULL) {
		int token;
		if (!*p)
			continue;

		token = match_token(p, tokens, args);
		switch (token) {
		case Opt_degraded:
			apfs_info(info, "allowing degraded mounts");
			apfs_set_opt(info->mount_opt, DEGRADED);
			break;
		case Opt_subvol:
		case Opt_subvol_empty:
		case Opt_subvolid:
		case Opt_device:
			/*
			 * These are parsed by apfs_parse_subvol_options or
			 * apfs_parse_device_options and can be ignored here.
			 */
			break;
		case Opt_nodatasum:
			apfs_set_and_info(info, NODATASUM,
					   "setting nodatasum");
			break;
		case Opt_datasum:
			if (apfs_test_opt(info, NODATASUM)) {
				if (apfs_test_opt(info, NODATACOW))
					apfs_info(info,
						   "setting datasum, datacow enabled");
				else
					apfs_info(info, "setting datasum");
			}
			apfs_clear_opt(info->mount_opt, NODATACOW);
			apfs_clear_opt(info->mount_opt, NODATASUM);
			break;
		case Opt_nodatacow:
			if (!apfs_test_opt(info, NODATACOW)) {
				if (!apfs_test_opt(info, COMPRESS) ||
				    !apfs_test_opt(info, FORCE_COMPRESS)) {
					apfs_info(info,
						   "setting nodatacow, compression disabled");
				} else {
					apfs_info(info, "setting nodatacow");
				}
			}
			apfs_clear_opt(info->mount_opt, COMPRESS);
			apfs_clear_opt(info->mount_opt, FORCE_COMPRESS);
			apfs_set_opt(info->mount_opt, NODATACOW);
			apfs_set_opt(info->mount_opt, NODATASUM);
			break;
		case Opt_datacow:
			apfs_clear_and_info(info, NODATACOW,
					     "setting datacow");
			break;
		case Opt_compress_force:
		case Opt_compress_force_type:
			compress_force = true;
			fallthrough;
		case Opt_compress:
		case Opt_compress_type:
			saved_compress_type = apfs_test_opt(info,
							     COMPRESS) ?
				info->compress_type : APFS_COMPRESS_NONE;
			saved_compress_force =
				apfs_test_opt(info, FORCE_COMPRESS);
			saved_compress_level = info->compress_level;
			if (token == Opt_compress ||
			    token == Opt_compress_force ||
			    strncmp(args[0].from, "zlib", 4) == 0) {
				compress_type = "zlib";

				info->compress_type = APFS_COMPRESS_ZLIB;
				info->compress_level = APFS_ZLIB_DEFAULT_LEVEL;
				/*
				 * args[0] contains uninitialized data since
				 * for these tokens we don't expect any
				 * parameter.
				 */
				if (token != Opt_compress &&
				    token != Opt_compress_force)
					info->compress_level =
					  apfs_compress_str2level(
							APFS_COMPRESS_ZLIB,
							args[0].from + 4);
				apfs_set_opt(info->mount_opt, COMPRESS);
				apfs_clear_opt(info->mount_opt, NODATACOW);
				apfs_clear_opt(info->mount_opt, NODATASUM);
				no_compress = 0;
			} else if (strncmp(args[0].from, "lzo", 3) == 0) {
				compress_type = "lzo";
				info->compress_type = APFS_COMPRESS_LZO;
				info->compress_level = 0;
				apfs_set_opt(info->mount_opt, COMPRESS);
				apfs_clear_opt(info->mount_opt, NODATACOW);
				apfs_clear_opt(info->mount_opt, NODATASUM);
				apfs_set_fs_incompat(info, COMPRESS_LZO);
				no_compress = 0;
			} else if (strncmp(args[0].from, "zstd", 4) == 0) {
				compress_type = "zstd";
				info->compress_type = APFS_COMPRESS_ZSTD;
				info->compress_level =
					apfs_compress_str2level(
							 APFS_COMPRESS_ZSTD,
							 args[0].from + 4);
				apfs_set_opt(info->mount_opt, COMPRESS);
				apfs_clear_opt(info->mount_opt, NODATACOW);
				apfs_clear_opt(info->mount_opt, NODATASUM);
				apfs_set_fs_incompat(info, COMPRESS_ZSTD);
				no_compress = 0;
			} else if (strncmp(args[0].from, "no", 2) == 0) {
				compress_type = "no";
				info->compress_level = 0;
				info->compress_type = 0;
				apfs_clear_opt(info->mount_opt, COMPRESS);
				apfs_clear_opt(info->mount_opt, FORCE_COMPRESS);
				compress_force = false;
				no_compress++;
			} else {
				ret = -EINVAL;
				goto out;
			}

			if (compress_force) {
				apfs_set_opt(info->mount_opt, FORCE_COMPRESS);
			} else {
				/*
				 * If we remount from compress-force=xxx to
				 * compress=xxx, we need clear FORCE_COMPRESS
				 * flag, otherwise, there is no way for users
				 * to disable forcible compression separately.
				 */
				apfs_clear_opt(info->mount_opt, FORCE_COMPRESS);
			}
			if (no_compress == 1) {
				apfs_info(info, "use no compression");
			} else if ((info->compress_type != saved_compress_type) ||
				   (compress_force != saved_compress_force) ||
				   (info->compress_level != saved_compress_level)) {
				apfs_info(info, "%s %s compression, level %d",
					   (compress_force) ? "force" : "use",
					   compress_type, info->compress_level);
			}
			compress_force = false;
			break;
		case Opt_ssd:
			apfs_set_and_info(info, SSD,
					   "enabling ssd optimizations");
			apfs_clear_opt(info->mount_opt, NOSSD);
			break;
		case Opt_ssd_spread:
			apfs_set_and_info(info, SSD,
					   "enabling ssd optimizations");
			apfs_set_and_info(info, SSD_SPREAD,
					   "using spread ssd allocation scheme");
			apfs_clear_opt(info->mount_opt, NOSSD);
			break;
		case Opt_nossd:
			apfs_set_opt(info->mount_opt, NOSSD);
			apfs_clear_and_info(info, SSD,
					     "not using ssd optimizations");
			fallthrough;
		case Opt_nossd_spread:
			apfs_clear_and_info(info, SSD_SPREAD,
					     "not using spread ssd allocation scheme");
			break;
		case Opt_barrier:
			apfs_clear_and_info(info, NOBARRIER,
					     "turning on barriers");
			break;
		case Opt_nobarrier:
			apfs_set_and_info(info, NOBARRIER,
					   "turning off barriers");
			break;
		case Opt_thread_pool:
			ret = match_int(&args[0], &intarg);
			if (ret) {
				goto out;
			} else if (intarg == 0) {
				ret = -EINVAL;
				goto out;
			}
			info->thread_pool_size = intarg;
			break;
		case Opt_max_inline:
			num = match_strdup(&args[0]);
			if (num) {
				info->max_inline = memparse(num, NULL);
				kfree(num);

				if (info->max_inline) {
					info->max_inline = min_t(u64,
						info->max_inline,
						info->sectorsize);
				}
				apfs_info(info, "max_inline at %llu",
					   info->max_inline);
			} else {
				ret = -ENOMEM;
				goto out;
			}
			break;
		case Opt_acl:
#ifdef CONFIG_APFS_FS_POSIX_ACL
			info->sb->s_flags |= SB_POSIXACL;
			break;
#else
			apfs_err(info, "support for ACL not compiled in!");
			ret = -EINVAL;
			goto out;
#endif
		case Opt_noacl:
			info->sb->s_flags &= ~SB_POSIXACL;
			break;
		case Opt_notreelog:
			apfs_set_and_info(info, NOTREELOG,
					   "disabling tree log");
			break;
		case Opt_treelog:
			apfs_clear_and_info(info, NOTREELOG,
					     "enabling tree log");
			break;
		case Opt_norecovery:
		case Opt_nologreplay:
			apfs_warn(info,
		"'nologreplay' is deprecated, use 'rescue=nologreplay' instead");
			apfs_set_and_info(info, NOLOGREPLAY,
					   "disabling log replay at mount time");
			break;
		case Opt_flushoncommit:
			apfs_set_and_info(info, FLUSHONCOMMIT,
					   "turning on flush-on-commit");
			break;
		case Opt_noflushoncommit:
			apfs_clear_and_info(info, FLUSHONCOMMIT,
					     "turning off flush-on-commit");
			break;
		case Opt_ratio:
			ret = match_int(&args[0], &intarg);
			if (ret)
				goto out;
			info->metadata_ratio = intarg;
			apfs_info(info, "metadata ratio %u",
				   info->metadata_ratio);
			break;
		case Opt_discard:
		case Opt_discard_mode:
			if (token == Opt_discard ||
			    strcmp(args[0].from, "sync") == 0) {
				apfs_clear_opt(info->mount_opt, DISCARD_ASYNC);
				apfs_set_and_info(info, DISCARD_SYNC,
						   "turning on sync discard");
			} else if (strcmp(args[0].from, "async") == 0) {
				apfs_clear_opt(info->mount_opt, DISCARD_SYNC);
				apfs_set_and_info(info, DISCARD_ASYNC,
						   "turning on async discard");
			} else {
				ret = -EINVAL;
				goto out;
			}
			break;
		case Opt_nodiscard:
			apfs_clear_and_info(info, DISCARD_SYNC,
					     "turning off discard");
			apfs_clear_and_info(info, DISCARD_ASYNC,
					     "turning off async discard");
			break;
		case Opt_space_cache:
		case Opt_space_cache_version:
			if (token == Opt_space_cache ||
			    strcmp(args[0].from, "v1") == 0) {
				apfs_clear_opt(info->mount_opt,
						FREE_SPACE_TREE);
				apfs_set_and_info(info, SPACE_CACHE,
					   "enabling disk space caching");
			} else if (strcmp(args[0].from, "v2") == 0) {
				apfs_clear_opt(info->mount_opt,
						SPACE_CACHE);
				apfs_set_and_info(info, FREE_SPACE_TREE,
						   "enabling free space tree");
			} else {
				ret = -EINVAL;
				goto out;
			}
			break;
		case Opt_rescan_uuid_tree:
			apfs_set_opt(info->mount_opt, RESCAN_UUID_TREE);
			break;
		case Opt_no_space_cache:
			if (apfs_test_opt(info, SPACE_CACHE)) {
				apfs_clear_and_info(info, SPACE_CACHE,
					     "disabling disk space caching");
			}
			if (apfs_test_opt(info, FREE_SPACE_TREE)) {
				apfs_clear_and_info(info, FREE_SPACE_TREE,
					     "disabling free space tree");
			}
			break;
		case Opt_inode_cache:
		case Opt_noinode_cache:
			apfs_warn(info,
	"the 'inode_cache' option is deprecated and has no effect since 5.11");
			break;
		case Opt_clear_cache:
			apfs_set_and_info(info, CLEAR_CACHE,
					   "force clearing of disk cache");
			break;
		case Opt_user_subvol_rm_allowed:
			apfs_set_opt(info->mount_opt, USER_SUBVOL_RM_ALLOWED);
			break;
		case Opt_enospc_debug:
			apfs_set_opt(info->mount_opt, ENOSPC_DEBUG);
			break;
		case Opt_noenospc_debug:
			apfs_clear_opt(info->mount_opt, ENOSPC_DEBUG);
			break;
		case Opt_defrag:
			apfs_set_and_info(info, AUTO_DEFRAG,
					   "enabling auto defrag");
			break;
		case Opt_nodefrag:
			apfs_clear_and_info(info, AUTO_DEFRAG,
					     "disabling auto defrag");
			break;
		case Opt_recovery:
		case Opt_usebackuproot:
			apfs_warn(info,
			"'%s' is deprecated, use 'rescue=usebackuproot' instead",
				   token == Opt_recovery ? "recovery" :
				   "usebackuproot");
			apfs_info(info,
				   "trying to use backup root at mount time");
			apfs_set_opt(info->mount_opt, USEBACKUPROOT);
			break;
		case Opt_skip_balance:
			apfs_set_opt(info->mount_opt, SKIP_BALANCE);
			break;
#ifdef CONFIG_APFS_FS_CHECK_INTEGRITY
		case Opt_check_integrity_including_extent_data:
			apfs_info(info,
				   "enabling check integrity including extent data");
			apfs_set_opt(info->mount_opt, CHECK_INTEGRITY_DATA);
			apfs_set_opt(info->mount_opt, CHECK_INTEGRITY);
			break;
		case Opt_check_integrity:
			apfs_info(info, "enabling check integrity");
			apfs_set_opt(info->mount_opt, CHECK_INTEGRITY);
			break;
		case Opt_check_integrity_print_mask:
			ret = match_int(&args[0], &intarg);
			if (ret)
				goto out;
			info->check_integrity_print_mask = intarg;
			apfs_info(info, "check_integrity_print_mask 0x%x",
				   info->check_integrity_print_mask);
			break;
#else
		case Opt_check_integrity_including_extent_data:
		case Opt_check_integrity:
		case Opt_check_integrity_print_mask:
			apfs_err(info,
				  "support for check_integrity* not compiled in!");
			ret = -EINVAL;
			goto out;
#endif
		case Opt_fatal_errors:
			if (strcmp(args[0].from, "panic") == 0)
				apfs_set_opt(info->mount_opt,
					      PANIC_ON_FATAL_ERROR);
			else if (strcmp(args[0].from, "bug") == 0)
				apfs_clear_opt(info->mount_opt,
					      PANIC_ON_FATAL_ERROR);
			else {
				ret = -EINVAL;
				goto out;
			}
			break;
		case Opt_commit_interval:
			intarg = 0;
			ret = match_int(&args[0], &intarg);
			if (ret)
				goto out;
			if (intarg == 0) {
				apfs_info(info,
					   "using default commit interval %us",
					   APFS_DEFAULT_COMMIT_INTERVAL);
				intarg = APFS_DEFAULT_COMMIT_INTERVAL;
			} else if (intarg > 300) {
				apfs_warn(info, "excessive commit interval %d",
					   intarg);
			}
			info->commit_interval = intarg;
			break;
		case Opt_rescue:
			ret = parse_rescue_options(info, args[0].from);
			if (ret < 0)
				goto out;
			break;
#ifdef CONFIG_APFS_DEBUG
		case Opt_fragment_all:
			apfs_info(info, "fragmenting all space");
			apfs_set_opt(info->mount_opt, FRAGMENT_DATA);
			apfs_set_opt(info->mount_opt, FRAGMENT_METADATA);
			break;
		case Opt_fragment_metadata:
			apfs_info(info, "fragmenting metadata");
			apfs_set_opt(info->mount_opt,
				      FRAGMENT_METADATA);
			break;
		case Opt_fragment_data:
			apfs_info(info, "fragmenting data");
			apfs_set_opt(info->mount_opt, FRAGMENT_DATA);
			break;
#endif
#ifdef CONFIG_APFS_FS_REF_VERIFY
		case Opt_ref_verify:
			apfs_info(info, "doing ref verification");
			apfs_set_opt(info->mount_opt, REF_VERIFY);
			break;
#endif
		case Opt_err:
			apfs_err(info, "unrecognized mount option '%s'", p);
			ret = -EINVAL;
			goto out;
		default:
			break;
		}
	}
check:
	/* We're read-only, don't have to check. */
	if (new_flags & SB_RDONLY)
		goto out;

	if (check_ro_option(info, APFS_MOUNT_NOLOGREPLAY, "nologreplay") ||
	    check_ro_option(info, APFS_MOUNT_IGNOREBADROOTS, "ignorebadroots") ||
	    check_ro_option(info, APFS_MOUNT_IGNOREDATACSUMS, "ignoredatacsums"))
		ret = -EINVAL;
out:
	if (apfs_fs_compat_ro(info, FREE_SPACE_TREE) &&
	    !apfs_test_opt(info, FREE_SPACE_TREE) &&
	    !apfs_test_opt(info, CLEAR_CACHE)) {
		apfs_err(info, "cannot disable free space tree");
		ret = -EINVAL;

	}
	if (!ret)
		ret = apfs_check_mountopts_zoned(info);
	if (!ret && apfs_test_opt(info, SPACE_CACHE))
		apfs_info(info, "disk space caching is enabled");
	if (!ret && apfs_test_opt(info, FREE_SPACE_TREE))
		apfs_info(info, "using free space tree");
	return ret;
}

/*
 * Parse mount options that are required early in the mount process.
 *
 * All other options will be parsed on much later in the mount process and
 * only when we need to allocate a new super block.
 */
static int apfs_parse_device_options(const char *options, fmode_t flags,
				      void *holder)
{
	substring_t args[MAX_OPT_ARGS];
	char *device_name, *opts, *orig, *p;
	struct apfs_device *device = NULL;
	int error = 0;

	lockdep_assert_held(&uuid_mutex);

	if (!options)
		return 0;

	/*
	 * strsep changes the string, duplicate it because apfs_parse_options
	 * gets called later
	 */
	opts = kstrdup(options, GFP_KERNEL);
	if (!opts)
		return -ENOMEM;
	orig = opts;

	while ((p = strsep(&opts, ",")) != NULL) {
		int token;

		if (!*p)
			continue;

		token = match_token(p, tokens, args);
		if (token == Opt_device) {
			device_name = match_strdup(&args[0]);
			if (!device_name) {
				error = -ENOMEM;
				goto out;
			}
			device = apfs_scan_one_device(device_name, flags,
					holder);
			kfree(device_name);
			if (IS_ERR(device)) {
				error = PTR_ERR(device);
				goto out;
			}
		}
	}

out:
	kfree(orig);
	return error;
}

/*
 * Parse mount options that are related to subvolume id
 *
 * The value is later passed to mount_subvol()
 */
static int apfs_parse_subvol_options(const char *options, char **subvol_name,
		u64 *subvol_objectid)
{
	substring_t args[MAX_OPT_ARGS];
	char *opts, *orig, *p;
	int error = 0;
	u64 subvolid = (u64)-1;

	if (!options)
		return 0;

	/*
	 * strsep changes the string, duplicate it because
	 * apfs_parse_device_options gets called later
	 */
	opts = kstrdup(options, GFP_KERNEL);
	if (!opts)
		return -ENOMEM;
	orig = opts;

	while ((p = strsep(&opts, ",")) != NULL) {
		int token;
		if (!*p)
			continue;

		token = match_token(p, tokens, args);
		switch (token) {
		case Opt_subvolid:
			error = match_u64(&args[0], &subvolid);
			if (error)
				goto out;

			*subvol_objectid = subvolid;
			break;
		default:
			break;
		}
	}

out:
	kfree(orig);

	if (!error && subvolid == (u64)-1)
		error = -EINVAL;
	return error;
}

char *apfs_get_subvol_name_from_objectid(struct apfs_fs_info *fs_info,
					  u64 subvol_objectid)
{
	struct apfs_root *root = fs_info->tree_root;
	struct apfs_root *fs_root = NULL;
	struct apfs_root_ref *root_ref;
	struct apfs_inode_ref *inode_ref;
	struct apfs_key key = {};
	struct apfs_path *path = NULL;
	char *name = NULL, *ptr;
	u64 dirid;
	int len;
	int ret;

	path = apfs_alloc_path();
	if (!path) {
		ret = -ENOMEM;
		goto err;
	}

	name = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!name) {
		ret = -ENOMEM;
		goto err;
	}
	ptr = name + PATH_MAX - 1;
	ptr[0] = '\0';

	/*
	 * Walk up the subvolume trees in the tree of tree roots by root
	 * backrefs until we hit the top-level subvolume.
	 */
	while (subvol_objectid != APFS_FS_TREE_OBJECTID) {
		key.objectid = subvol_objectid;
		key.type = APFS_ROOT_BACKREF_KEY;
		key.offset = (u64)-1;

		ret = apfs_search_slot(NULL, root, &key, path, 0, 0);
		if (ret < 0) {
			goto err;
		} else if (ret > 0) {
			ret = apfs_previous_item(root, path, subvol_objectid,
						  APFS_ROOT_BACKREF_KEY);
			if (ret < 0) {
				goto err;
			} else if (ret > 0) {
				ret = -ENOENT;
				goto err;
			}
		}

		apfs_item_key_to_cpu(path->nodes[0], &key, path->slots[0]);
		subvol_objectid = key.offset;

		root_ref = apfs_item_ptr(path->nodes[0], path->slots[0],
					  struct apfs_root_ref);
		len = apfs_root_ref_name_len(path->nodes[0], root_ref);
		ptr -= len + 1;
		if (ptr < name) {
			ret = -ENAMETOOLONG;
			goto err;
		}
		read_extent_buffer(path->nodes[0], ptr + 1,
				   (unsigned long)(root_ref + 1), len);
		ptr[0] = '/';
		dirid = apfs_root_ref_dirid(path->nodes[0], root_ref);
		apfs_release_path(path);

		fs_root = apfs_get_fs_root(fs_info, subvol_objectid, true);
		if (IS_ERR(fs_root)) {
			ret = PTR_ERR(fs_root);
			fs_root = NULL;
			goto err;
		}

		/*
		 * Walk up the filesystem tree by inode refs until we hit the
		 * root directory.
		 */
		while (dirid != APFS_FIRST_FREE_OBJECTID) {
			key.objectid = dirid;
			key.type = APFS_INODE_REF_KEY;
			key.offset = (u64)-1;

			ret = apfs_search_slot(NULL, fs_root, &key, path, 0, 0);
			if (ret < 0) {
				goto err;
			} else if (ret > 0) {
				ret = apfs_previous_item(fs_root, path, dirid,
							  APFS_INODE_REF_KEY);
				if (ret < 0) {
					goto err;
				} else if (ret > 0) {
					ret = -ENOENT;
					goto err;
				}
			}

			apfs_item_key_to_cpu(path->nodes[0], &key, path->slots[0]);
			dirid = key.offset;

			inode_ref = apfs_item_ptr(path->nodes[0],
						   path->slots[0],
						   struct apfs_inode_ref);
			len = apfs_inode_ref_name_len(path->nodes[0],
						       inode_ref);
			ptr -= len + 1;
			if (ptr < name) {
				ret = -ENAMETOOLONG;
				goto err;
			}
			read_extent_buffer(path->nodes[0], ptr + 1,
					   (unsigned long)(inode_ref + 1), len);
			ptr[0] = '/';
			apfs_release_path(path);
		}
		apfs_put_root(fs_root);
		fs_root = NULL;
	}

	apfs_free_path(path);
	if (ptr == name + PATH_MAX - 1) {
		name[0] = '/';
		name[1] = '\0';
	} else {
		memmove(name, ptr, name + PATH_MAX - ptr);
	}
	return name;

err:
	apfs_put_root(fs_root);
	apfs_free_path(path);
	kfree(name);
	return ERR_PTR(ret);
}

static int get_default_subvol_objectid(struct apfs_fs_info *fs_info, u64 *objectid)
{
	struct apfs_root *root = fs_info->tree_root;
	struct apfs_dir_item *di;
	struct apfs_path *path;
	struct apfs_key location = {};
	u64 dir_id;

	path = apfs_alloc_path();
	if (!path)
		return -ENOMEM;

	/*
	 * Find the "default" dir item which points to the root item that we
	 * will mount by default if we haven't been given a specific subvolume
	 * to mount.
	 */
	dir_id = apfs_super_root_dir(fs_info->super_copy);
	di = apfs_lookup_dir_item(NULL, root, path, dir_id, "default", 7, 0);
	if (IS_ERR(di)) {
		apfs_free_path(path);
		return PTR_ERR(di);
	}
	if (!di) {
		/*
		 * Ok the default dir item isn't there.  This is weird since
		 * it's always been there, but don't freak out, just try and
		 * mount the top-level subvolume.
		 */
		apfs_free_path(path);
		*objectid = APFS_FS_TREE_OBJECTID;
		return 0;
	}

	apfs_dir_item_key_to_cpu(path->nodes[0], di, &location);
	apfs_free_path(path);
	*objectid = location.objectid;
	return 0;
}

static int apfs_fill_super(struct super_block *sb,
			    struct apfs_device *device,
			    void *data)
{
	struct inode *inode;
	struct apfs_fs_info *fs_info = apfs_sb(sb);
	int err;

	sb->s_maxbytes = MAX_LFS_FILESIZE;
	sb->s_magic = APFS_VOLUME_MAGIC;
	sb->s_op = &apfs_super_ops;
	sb->s_d_op = &apfs_dentry_operations;
	sb->s_export_op = &apfs_export_ops;
	sb->s_xattr = apfs_xattr_handlers;
	sb->s_time_gran = 1;
#ifdef CONFIG_APFS_FS_POSIX_ACL
	sb->s_flags |= SB_POSIXACL;
#endif
	sb->s_flags |= SB_I_VERSION;
	sb->s_iflags |= SB_I_CGROUPWB;

	err = super_setup_bdi(sb);
	if (err) {
		apfs_err(fs_info, "super_setup_bdi failed");
		return err;
	}

	err = open_ctree(sb, device, (char *)data);
	if (err) {
		apfs_err(NULL, "open_ctree failed");
		return err;
	}

	inode = apfs_iget(sb, APFS_ROOT_DIR_INO, fs_info->root_root);
	if (IS_ERR(inode)) {
		err = PTR_ERR(inode);
		apfs_err(NULL, "apfs iget failed %ld", PTR_ERR(inode));
		goto fail_close;
	}

	sb->s_root = d_make_root(inode);
	if (!sb->s_root) {
		iput(inode);
		err = -ENOMEM;
		goto fail_close;
	}

	cleancache_init_fs(sb);
	sb->s_flags |= SB_ACTIVE;
	return 0;

fail_close:
	apfs_printk(NULL, "failed to fill_super %d\n", err);
	close_ctree(fs_info);
	return err;
}

int apfs_sync_fs(struct super_block *sb, int wait)
{
	struct apfs_trans_handle *trans;
	struct apfs_fs_info *fs_info = apfs_sb(sb);
	struct apfs_root *root = fs_info->tree_root;

	trace_apfs_sync_fs(fs_info, wait);

	if (!wait) {
		filemap_flush(fs_info->btree_inode->i_mapping);
		return 0;
	}

	apfs_wait_ordered_roots(fs_info, U64_MAX, 0, (u64)-1);

	trans = apfs_attach_transaction_barrier(root);
	if (IS_ERR(trans)) {
		/* no transaction, don't bother */
		if (PTR_ERR(trans) == -ENOENT) {
			/*
			 * Exit unless we have some pending changes
			 * that need to go through commit
			 */
			if (fs_info->pending_changes == 0)
				return 0;
			/*
			 * A non-blocking test if the fs is frozen. We must not
			 * start a new transaction here otherwise a deadlock
			 * happens. The pending operations are delayed to the
			 * next commit after thawing.
			 */
			if (sb_start_write_trylock(sb))
				sb_end_write(sb);
			else
				return 0;
			trans = apfs_start_transaction(root, 0);
		}
		if (IS_ERR(trans))
			return PTR_ERR(trans);
	}
	return apfs_commit_transaction(trans);
}

static void print_rescue_option(struct seq_file *seq, const char *s, bool *printed)
{
	seq_printf(seq, "%s%s", (*printed) ? ":" : ",rescue=", s);
	*printed = true;
}

static int apfs_show_options(struct seq_file *seq, struct dentry *dentry)
{
	struct apfs_fs_info *info = apfs_sb(dentry->d_sb);

	seq_printf(seq, ",subvolid=%dtest", info->index);

	return 0;
}

static int apfs_test_super(struct super_block *s, void *data)
{
	struct apfs_fs_info *p = data;
	struct apfs_fs_info *fs_info = APFS_SB(s);

	if (fs_info->device != p->device)
		return false;
	if (fs_info->index != p->index)
		return false;

	return true;
}

static int apfs_set_super(struct super_block *s, void *data)
{
	int err = set_anon_super(s, data);
	if (!err)
		s->s_fs_info = data;
	return err;
}

/*
 * subvolumes are identified by ino 256
 */
static inline int is_subvolume_inode(struct inode *inode)
{
	return 0;
}

/*
 * Find a superblock for the given device / mount point.
 *
 * Note: This is based on mount_bdev from fs/super.c with a few additions
 *       for multiple device setup.  Make sure to keep it in sync.
 */
static struct dentry *
apfs_mount(struct file_system_type *fs_type,
	   int flags, const char *device_name, void *data)
{
	struct block_device *bdev = NULL;
	struct super_block *s;
	struct apfs_device *device = NULL;
	struct apfs_fs_info *fs_info = NULL;
	void *new_sec_opts = NULL;
	fmode_t mode = FMODE_READ | FMODE_EXCL;
	int error = 0;
	u64 subvol_objectid = -1;

	error = apfs_parse_subvol_options(data, NULL, &subvol_objectid);
	if (error)
		return ERR_PTR(error);

	if (!(flags & SB_RDONLY)) {
		apfs_info(NULL, "mount with rw not supported now, mount with ro");
		flags |= SB_RDONLY;
	}

	if (data) {
		error = security_sb_eat_lsm_opts(data, &new_sec_opts);
		if (error)
			return ERR_PTR(error);
	}

	/*
	 * Setup a dummy root and fs_info for test/set super.  This is because
	 * we don't actually fill this stuff out until open_ctree, but we need
	 * then open_ctree will properly initialize the file system specific
	 * settings later.  apfs_init_fs_info initializes the static elements
	 * of the fs_info (locks and such) to make cleanup easier if we find a
	 * superblock with our given fs_devices later on at sget() time.
	 */
	fs_info = kvzalloc(sizeof(struct apfs_fs_info), GFP_KERNEL);
	if (!fs_info) {
		error = -ENOMEM;
		goto error_sec_opts;
	}
	apfs_init_fs_info(fs_info);
	fs_info->index = subvol_objectid;

	fs_info->super_copy = kzalloc(APFS_SUPER_INFO_SIZE, GFP_KERNEL);
	fs_info->super_for_commit = kzalloc(APFS_SUPER_INFO_SIZE, GFP_KERNEL);
	if (!fs_info->super_copy || !fs_info->super_for_commit) {
		error = -ENOMEM;
		goto error_fs_info;
	}

	mutex_lock(&uuid_mutex);
	/*
	error = apfs_parse_device_options(data, mode, fs_type);
	if (error) {
		mutex_unlock(&uuid_mutex);
		goto error_fs_info;
	}
	*/
	device = apfs_scan_one_device(device_name, mode, fs_type);
	if (IS_ERR(device)) {
		mutex_unlock(&uuid_mutex);
		error = PTR_ERR(device);
		goto error_fs_info;
	}
	mutex_unlock(&uuid_mutex);

	if (!(flags & SB_RDONLY) &&
	    !test_bit(APFS_DEV_STATE_WRITEABLE, &device->dev_state)) {
		error = -EACCES;
		goto error_close_devices;
	}

	fs_info->device = device;
	bdev = device->bdev;
	s = sget(fs_type, apfs_test_super, apfs_set_super, flags | SB_NOSEC,
		 fs_info);

	if (IS_ERR(s)) {
		error = PTR_ERR(s);
		goto error_close_devices;
	}

	if (s->s_root) {
		apfs_close_device(device);
		apfs_free_fs_info(fs_info);
		if ((flags ^ s->s_flags) & SB_RDONLY)
			error = -EBUSY;
	} else {
		snprintf(s->s_id, sizeof(s->s_id), "%pg", bdev);
		apfs_sb(s)->bdev_holder = fs_type;
		if (!strstr(crc32c_impl(), "generic"))
			set_bit(APFS_FS_CSUM_IMPL_FAST, &fs_info->flags);
		error = apfs_fill_super(s, device, data);
	}
	if (!error)
		error = security_sb_set_mnt_opts(s, new_sec_opts, 0, NULL);
	security_free_mnt_opts(&new_sec_opts);
	if (error) {
		deactivate_locked_super(s);
		return ERR_PTR(error);
	}

	return dget(s->s_root);

error_close_devices:
	apfs_close_device(device);
error_fs_info:
	apfs_free_fs_info(fs_info);
error_sec_opts:
	security_free_mnt_opts(&new_sec_opts);
	return ERR_PTR(error);
}

static void apfs_resize_thread_pool(struct apfs_fs_info *fs_info,
				     u32 new_pool_size, u32 old_pool_size)
{
	if (new_pool_size == old_pool_size)
		return;

	fs_info->thread_pool_size = new_pool_size;

	apfs_info(fs_info, "resize thread pool %d -> %d",
	       old_pool_size, new_pool_size);

	apfs_workqueue_set_max(fs_info->workers, new_pool_size);
	apfs_workqueue_set_max(fs_info->delalloc_workers, new_pool_size);
	apfs_workqueue_set_max(fs_info->caching_workers, new_pool_size);
	apfs_workqueue_set_max(fs_info->endio_workers, new_pool_size);
	apfs_workqueue_set_max(fs_info->endio_meta_workers, new_pool_size);
	apfs_workqueue_set_max(fs_info->endio_meta_write_workers,
				new_pool_size);
	apfs_workqueue_set_max(fs_info->endio_write_workers, new_pool_size);
	apfs_workqueue_set_max(fs_info->endio_freespace_worker, new_pool_size);
	apfs_workqueue_set_max(fs_info->delayed_workers, new_pool_size);
	apfs_workqueue_set_max(fs_info->readahead_workers, new_pool_size);
	apfs_workqueue_set_max(fs_info->scrub_wr_completion_workers,
				new_pool_size);
}

static inline void apfs_remount_begin(struct apfs_fs_info *fs_info,
				       unsigned long old_opts, int flags)
{
	if (apfs_raw_test_opt(old_opts, AUTO_DEFRAG) &&
	    (!apfs_raw_test_opt(fs_info->mount_opt, AUTO_DEFRAG) ||
	     (flags & SB_RDONLY))) {
		/* wait for any defraggers to finish */
		wait_event(fs_info->transaction_wait,
			   (atomic_read(&fs_info->defrag_running) == 0));
		if (flags & SB_RDONLY)
			sync_filesystem(fs_info->sb);
	}
}

static inline void apfs_remount_cleanup(struct apfs_fs_info *fs_info,
					 unsigned long old_opts)
{
	const bool cache_opt = apfs_test_opt(fs_info, SPACE_CACHE);

	/*
	 * We need to cleanup all defragable inodes if the autodefragment is
	 * close or the filesystem is read only.
	 */
	if (apfs_raw_test_opt(old_opts, AUTO_DEFRAG) &&
	    (!apfs_raw_test_opt(fs_info->mount_opt, AUTO_DEFRAG) || sb_rdonly(fs_info->sb))) {
		apfs_cleanup_defrag_inodes(fs_info);
	}

	/* If we toggled discard async */
	if (!apfs_raw_test_opt(old_opts, DISCARD_ASYNC) &&
	    apfs_test_opt(fs_info, DISCARD_ASYNC))
		apfs_discard_resume(fs_info);
	else if (apfs_raw_test_opt(old_opts, DISCARD_ASYNC) &&
		 !apfs_test_opt(fs_info, DISCARD_ASYNC))
		apfs_discard_cleanup(fs_info);

	/* If we toggled space cache */
	if (cache_opt != apfs_free_space_cache_v1_active(fs_info))
		apfs_set_free_space_cache_v1_active(fs_info, cache_opt);
}

static int apfs_remount(struct super_block *sb, int *flags, char *data)
{
	struct apfs_fs_info *fs_info = apfs_sb(sb);
	unsigned old_flags = sb->s_flags;
	unsigned long old_opts = fs_info->mount_opt;
	unsigned long old_compress_type = fs_info->compress_type;
	u64 old_max_inline = fs_info->max_inline;
	u32 old_thread_pool_size = fs_info->thread_pool_size;
	u32 old_metadata_ratio = fs_info->metadata_ratio;
	int ret;

	return -ENOTSUPP;

	sync_filesystem(sb);
	set_bit(APFS_FS_STATE_REMOUNTING, &fs_info->fs_state);

	if (data) {
		void *new_sec_opts = NULL;

		ret = security_sb_eat_lsm_opts(data, &new_sec_opts);
		if (!ret)
			ret = security_sb_remount(sb, new_sec_opts);
		security_free_mnt_opts(&new_sec_opts);
		if (ret)
			goto restore;
	}

	ret = apfs_parse_options(fs_info, data, *flags);
	if (ret)
		goto restore;

	apfs_remount_begin(fs_info, old_opts, *flags);
	apfs_resize_thread_pool(fs_info,
		fs_info->thread_pool_size, old_thread_pool_size);

	if ((bool)apfs_test_opt(fs_info, FREE_SPACE_TREE) !=
	    (bool)apfs_fs_compat_ro(fs_info, FREE_SPACE_TREE) &&
	    (!sb_rdonly(sb) || (*flags & SB_RDONLY))) {
		apfs_warn(fs_info,
		"remount supports changing free space tree only from ro to rw");
		/* Make sure free space cache options match the state on disk */
		if (apfs_fs_compat_ro(fs_info, FREE_SPACE_TREE)) {
			apfs_set_opt(fs_info->mount_opt, FREE_SPACE_TREE);
			apfs_clear_opt(fs_info->mount_opt, SPACE_CACHE);
		}
		if (apfs_free_space_cache_v1_active(fs_info)) {
			apfs_clear_opt(fs_info->mount_opt, FREE_SPACE_TREE);
			apfs_set_opt(fs_info->mount_opt, SPACE_CACHE);
		}
	}

	if ((bool)(*flags & SB_RDONLY) == sb_rdonly(sb))
		goto out;

	if (*flags & SB_RDONLY) {
		/*
		 * this also happens on 'umount -rf' or on shutdown, when
		 * the filesystem is busy.
		 */
		cancel_work_sync(&fs_info->async_reclaim_work);
		cancel_work_sync(&fs_info->async_data_reclaim_work);

		apfs_discard_cleanup(fs_info);

		/* wait for the uuid_scan task to finish */
		down(&fs_info->uuid_tree_rescan_sem);
		/* avoid complains from lockdep et al. */
		up(&fs_info->uuid_tree_rescan_sem);

		apfs_set_sb_rdonly(sb);

		/*
		 * Setting SB_RDONLY will put the cleaner thread to
		 * sleep at the next loop if it's already active.
		 * If it's already asleep, we'll leave unused block
		 * groups on disk until we're mounted read-write again
		 * unless we clean them up here.
		 */
		apfs_delete_unused_bgs(fs_info);

		/*
		 * The cleaner task could be already running before we set the
		 * flag APFS_FS_STATE_RO (and SB_RDONLY in the superblock).
		 * We must make sure that after we finish the remount, i.e. after
		 * we call apfs_commit_super(), the cleaner can no longer start
		 * a transaction - either because it was dropping a dead root,
		 * running delayed iputs or deleting an unused block group (the
		 * cleaner picked a block group from the list of unused block
		 * groups before we were able to in the previous call to
		 * apfs_delete_unused_bgs()).
		 */
		wait_on_bit(&fs_info->flags, APFS_FS_CLEANER_RUNNING,
			    TASK_UNINTERRUPTIBLE);

		/*
		 * We've set the superblock to RO mode, so we might have made
		 * the cleaner task sleep without running all pending delayed
		 * iputs. Go through all the delayed iputs here, so that if an
		 * unmount happens without remounting RW we don't end up at
		 * finishing close_ctree() with a non-empty list of delayed
		 * iputs.
		 */
		apfs_run_delayed_iputs(fs_info);

		apfs_dev_replace_suspend_for_unmount(fs_info);
		apfs_scrub_cancel(fs_info);
		apfs_pause_balance(fs_info);

		/*
		 * Pause the qgroup rescan worker if it is running. We don't want
		 * it to be still running after we are in RO mode, as after that,
		 * by the time we unmount, it might have left a transaction open,
		 * so we would leak the transaction and/or crash.
		 */
		apfs_qgroup_wait_for_completion(fs_info, false);

		ret = apfs_commit_super(fs_info);
		if (ret)
			goto restore;
	} else {
		if (test_bit(APFS_FS_STATE_ERROR, &fs_info->fs_state)) {
			apfs_err(fs_info,
				"Remounting read-write after error is not allowed");
			ret = -EINVAL;
			goto restore;
		}
		if (fs_info->fs_devices->rw_devices == 0) {
			ret = -EACCES;
			goto restore;
		}

		if (!apfs_check_rw_degradable(fs_info, NULL)) {
			apfs_warn(fs_info,
		"too many missing devices, writable remount is not allowed");
			ret = -EACCES;
			goto restore;
		}

		if (apfs_super_log_root(fs_info->super_copy) != 0) {
			apfs_warn(fs_info,
		"mount required to replay tree-log, cannot remount read-write");
			ret = -EINVAL;
			goto restore;
		}
		if (fs_info->sectorsize < PAGE_SIZE) {
			apfs_warn(fs_info,
	"read-write mount is not yet allowed for sectorsize %u page size %lu",
				   fs_info->sectorsize, PAGE_SIZE);
			ret = -EINVAL;
			goto restore;
		}

		/*
		 * NOTE: when remounting with a change that does writes, don't
		 * put it anywhere above this point, as we are not sure to be
		 * safe to write until we pass the above checks.
		 */
		ret = apfs_start_pre_rw_mount(fs_info);
		if (ret)
			goto restore;

		apfs_clear_sb_rdonly(sb);

		set_bit(APFS_FS_OPEN, &fs_info->flags);
	}
out:
	/*
	 * We need to set SB_I_VERSION here otherwise it'll get cleared by VFS,
	 * since the absence of the flag means it can be toggled off by remount.
	 */
	*flags |= SB_I_VERSION;

	wake_up_process(fs_info->transaction_kthread);
	apfs_remount_cleanup(fs_info, old_opts);
	apfs_clear_oneshot_options(fs_info);
	clear_bit(APFS_FS_STATE_REMOUNTING, &fs_info->fs_state);

	return 0;

restore:
	/* We've hit an error - don't reset SB_RDONLY */
	if (sb_rdonly(sb))
		old_flags |= SB_RDONLY;
	if (!(old_flags & SB_RDONLY))
		clear_bit(APFS_FS_STATE_RO, &fs_info->fs_state);
	sb->s_flags = old_flags;
	fs_info->mount_opt = old_opts;
	fs_info->compress_type = old_compress_type;
	fs_info->max_inline = old_max_inline;
	apfs_resize_thread_pool(fs_info,
		old_thread_pool_size, fs_info->thread_pool_size);
	fs_info->metadata_ratio = old_metadata_ratio;
	apfs_remount_cleanup(fs_info, old_opts);
	clear_bit(APFS_FS_STATE_REMOUNTING, &fs_info->fs_state);

	return ret;
}

/* Used to sort the devices by max_avail(descending sort) */
static inline int apfs_cmp_device_free_bytes(const void *dev_info1,
				       const void *dev_info2)
{
	if (((struct apfs_device_info *)dev_info1)->max_avail >
	    ((struct apfs_device_info *)dev_info2)->max_avail)
		return -1;
	else if (((struct apfs_device_info *)dev_info1)->max_avail <
		 ((struct apfs_device_info *)dev_info2)->max_avail)
		return 1;
	else
	return 0;
}

/*
 * sort the devices by max_avail, in which max free extent size of each device
 * is stored.(Descending Sort)
 */
static inline void apfs_descending_sort_devices(
					struct apfs_device_info *devices,
					size_t nr_devices)
{
	sort(devices, nr_devices, sizeof(struct apfs_device_info),
	     apfs_cmp_device_free_bytes, NULL);
}

/*
 * The helper to calc the free space on the devices that can be used to store
 * file data.
 */
static inline int apfs_calc_avail_data_space(struct apfs_fs_info *fs_info,
					      u64 *free_bytes)
{
	struct apfs_device_info *devices_info;
	struct apfs_fs_devices *fs_devices = fs_info->fs_devices;
	struct apfs_device *device;
	u64 type;
	u64 avail_space;
	u64 min_stripe_size;
	int num_stripes = 1;
	int i = 0, nr_devices;
	const struct apfs_raid_attr *rattr;

	/*
	 * We aren't under the device list lock, so this is racy-ish, but good
	 * enough for our purposes.
	 */
	nr_devices = fs_info->fs_devices->open_devices;
	if (!nr_devices) {
		smp_mb();
		nr_devices = fs_info->fs_devices->open_devices;
		ASSERT(nr_devices);
		if (!nr_devices) {
			*free_bytes = 0;
			return 0;
		}
	}

	devices_info = kmalloc_array(nr_devices, sizeof(*devices_info),
			       GFP_KERNEL);
	if (!devices_info)
		return -ENOMEM;

	/* calc min stripe number for data space allocation */
	type = apfs_data_alloc_profile(fs_info);
	rattr = &apfs_raid_array[apfs_bg_flags_to_raid_index(type)];

	if (type & APFS_BLOCK_GROUP_RAID0)
		num_stripes = nr_devices;
	else if (type & APFS_BLOCK_GROUP_RAID1)
		num_stripes = 2;
	else if (type & APFS_BLOCK_GROUP_RAID1C3)
		num_stripes = 3;
	else if (type & APFS_BLOCK_GROUP_RAID1C4)
		num_stripes = 4;
	else if (type & APFS_BLOCK_GROUP_RAID10)
		num_stripes = 4;

	/* Adjust for more than 1 stripe per device */
	min_stripe_size = rattr->dev_stripes * APFS_STRIPE_LEN;

	rcu_read_lock();
	list_for_each_entry_rcu(device, &fs_devices->devices, dev_list) {
		if (!test_bit(APFS_DEV_STATE_IN_FS_METADATA,
						&device->dev_state) ||
		    !device->bdev ||
		    test_bit(APFS_DEV_STATE_REPLACE_TGT, &device->dev_state))
			continue;

		if (i >= nr_devices)
			break;

		avail_space = device->total_bytes - device->bytes_used;

		/* align with stripe_len */
		avail_space = rounddown(avail_space, APFS_STRIPE_LEN);

		/*
		 * In order to avoid overwriting the superblock on the drive,
		 * apfs starts at an offset of at least 1MB when doing chunk
		 * allocation.
		 *
		 * This ensures we have at least min_stripe_size free space
		 * after excluding 1MB.
		 */
		if (avail_space <= SZ_1M + min_stripe_size)
			continue;

		avail_space -= SZ_1M;

		devices_info[i].dev = device;
		devices_info[i].max_avail = avail_space;

		i++;
	}
	rcu_read_unlock();

	nr_devices = i;

	apfs_descending_sort_devices(devices_info, nr_devices);

	i = nr_devices - 1;
	avail_space = 0;
	while (nr_devices >= rattr->devs_min) {
		num_stripes = min(num_stripes, nr_devices);

		if (devices_info[i].max_avail >= min_stripe_size) {
			int j;
			u64 alloc_size;

			avail_space += devices_info[i].max_avail * num_stripes;
			alloc_size = devices_info[i].max_avail;
			for (j = i + 1 - num_stripes; j <= i; j++)
				devices_info[j].max_avail -= alloc_size;
		}
		i--;
		nr_devices--;
	}

	kfree(devices_info);
	*free_bytes = avail_space;
	return 0;
}

/*
 * Calculate numbers for 'df', pessimistic in case of mixed raid profiles.
 *
 * If there's a redundant raid level at DATA block groups, use the respective
 * multiplier to scale the sizes.
 *
 * Unused device space usage is based on simulating the chunk allocator
 * algorithm that respects the device sizes and order of allocations.  This is
 * a close approximation of the actual use but there are other factors that may
 * change the result (like a new metadata chunk).
 *
 * If metadata is exhausted, f_bavail will be 0.
 */
static int apfs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct apfs_fs_info *fs_info = apfs_sb(dentry->d_sb);
	struct apfs_vol_superblock *super = fs_info->__super_copy;
	__be32 *fsid = (__be32 *)&super->uuid;


	buf->f_bfree = apfs_volume_super_total_blocks_freed(super);
	buf->f_bavail = buf->f_bfree;
	buf->f_blocks = apfs_volume_super_total_blocks_allocated(super) +
		buf->f_bfree;

	buf->f_files = apfs_volume_super_num_files(super) +
		apfs_volume_super_num_dirs(super) +
		apfs_volume_super_num_symlinks(super);
	buf->f_type = APFS_VOLUME_MAGIC;
	buf->f_bsize = dentry->d_sb->s_blocksize;
	buf->f_namelen = APFS_NAME_LEN;

	/* We treat it as constant endianness (it doesn't matter _which_)
	   because we want the fsid to come out the same whether mounted
	   on a big-endian or little-endian host */
	buf->f_fsid.val[0] = be32_to_cpu(fsid[0]) ^ be32_to_cpu(fsid[2]);
	buf->f_fsid.val[1] = be32_to_cpu(fsid[1]) ^ be32_to_cpu(fsid[3]);

	return 0;
}

static void apfs_kill_super(struct super_block *sb)
{
	struct apfs_fs_info *fs_info = apfs_sb(sb);
	kill_anon_super(sb);
	apfs_free_fs_info(fs_info);
}

static struct file_system_type apfs_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "apfs",
	.mount		= apfs_mount,
	.kill_sb	= apfs_kill_super,
	.fs_flags	= FS_REQUIRES_DEV | FS_BINARY_MOUNTDATA,
};

MODULE_ALIAS_FS("apfs");

static int apfs_control_open(struct inode *inode, struct file *file)
{
	/*
	 * The control file's private_data is used to hold the
	 * transaction when it is started and is used to keep
	 * track of whether a transaction is already in progress.
	 */
	file->private_data = NULL;
	return 0;
}

/*
 * Used by /dev/apfs-control for devices ioctls.
 */
static long apfs_control_ioctl(struct file *file, unsigned int cmd,
				unsigned long arg)
{
	struct apfs_ioctl_vol_args *vol;
	struct apfs_device *device = NULL;
	int ret = -ENOTTY;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	vol = memdup_user((void __user *)arg, sizeof(*vol));
	if (IS_ERR(vol))
		return PTR_ERR(vol);
	vol->name[APFS_PATH_NAME_MAX] = '\0';

	switch (cmd) {
	case APFS_IOC_SCAN_DEV:
		mutex_lock(&uuid_mutex);
		/*
		device = apfs_scan_one_device(vol->name, FMODE_READ,
					       &apfs_root_fs_type);
		ret = PTR_ERR_OR_ZERO(device);
		ret = -EPERM;
		*/
		mutex_unlock(&uuid_mutex);
		break;
	case APFS_IOC_FORGET_DEV:
		ret = apfs_forget_devices(vol->name);
		break;
	case APFS_IOC_DEVICES_READY:
		mutex_lock(&uuid_mutex);
		device = apfs_scan_one_device(vol->name, FMODE_READ,
					       &apfs_root_fs_type);
		if (IS_ERR(device)) {
			mutex_unlock(&uuid_mutex);
			ret = PTR_ERR(device);
			break;
		}
		ret = !(device->fs_devices->num_devices ==
			device->fs_devices->total_devices);
		mutex_unlock(&uuid_mutex);
		break;
	case APFS_IOC_GET_SUPPORTED_FEATURES:
		ret = apfs_ioctl_get_supported_features((void __user*)arg);
		break;
	}

	kfree(vol);
	return ret;
}

static int apfs_freeze(struct super_block *sb)
{
	struct apfs_trans_handle *trans;
	struct apfs_fs_info *fs_info = apfs_sb(sb);
	struct apfs_root *root = fs_info->tree_root;

	set_bit(APFS_FS_FROZEN, &fs_info->flags);
	/*
	 * We don't need a barrier here, we'll wait for any transaction that
	 * could be in progress on other threads (and do delayed iputs that
	 * we want to avoid on a frozen filesystem), or do the commit
	 * ourselves.
	 */
	trans = apfs_attach_transaction_barrier(root);
	if (IS_ERR(trans)) {
		/* no transaction, don't bother */
		if (PTR_ERR(trans) == -ENOENT)
			return 0;
		return PTR_ERR(trans);
	}
	return apfs_commit_transaction(trans);
}

static int apfs_unfreeze(struct super_block *sb)
{
	struct apfs_fs_info *fs_info = apfs_sb(sb);

	clear_bit(APFS_FS_FROZEN, &fs_info->flags);
	return 0;
}

static int apfs_show_devname(struct seq_file *m, struct dentry *root)
{
	struct apfs_fs_info *fs_info = apfs_sb(root->d_sb);
	struct apfs_device *dev = fs_info->device;

	if (test_bit(APFS_DEV_STATE_MISSING, &dev->dev_state) ||
		!dev->name) {
		WARN_ON(1);
	} else {
		seq_escape(m, rcu_str_deref(dev->name), " \t\n\\");
	}

	return 0;
}

static const struct super_operations apfs_super_ops = {
	.drop_inode	= apfs_drop_inode,
	.evict_inode	= apfs_evict_inode,
	.put_super	= apfs_put_super,
	.sync_fs	= apfs_sync_fs,
	.show_options	= apfs_show_options,
	.show_devname	= apfs_show_devname,
	.alloc_inode	= apfs_alloc_inode,
	.destroy_inode	= apfs_destroy_inode,
	.free_inode	= apfs_free_inode,
	.statfs		= apfs_statfs,
	.remount_fs	= apfs_remount,
	.freeze_fs	= apfs_freeze,
	.unfreeze_fs	= apfs_unfreeze,
};

static int __init apfs_interface_init(void)
{
	return 0;
}

static __cold void apfs_interface_exit(void)
{
	return ;
}

static void __init apfs_print_mod_info(void)
{
	static const char options[] = ""
#ifdef CONFIG_APFS_DEBUG
			", debug=on"
#endif
#ifdef CONFIG_APFS_ASSERT
			", assert=on"
#endif
#ifdef CONFIG_APFS_FS_CHECK_INTEGRITY
			", integrity-checker=on"
#endif
#ifdef CONFIG_APFS_FS_REF_VERIFY
			", ref-verify=on"
#endif
#ifdef CONFIG_BLK_DEV_ZONED
			", zoned=yes"
#else
			", zoned=no"
#endif
			;
	pr_info("Apfs loaded, crc32c=%s%s\n", crc32c_impl(), options);
}

static int __init init_apfs_fs(void)
{
	int err;

	apfs_props_init();

	err = apfs_init_sysfs();
	if (err)
		return err;

	apfs_init_compress();

	err = apfs_init_cachep();
	if (err)
		goto free_compress;

	err = extent_io_init();
	if (err)
		goto free_cachep;

	err = extent_state_cache_init();
	if (err)
		goto free_extent_io;

	err = extent_map_init();
	if (err)
		goto free_extent_state_cache;

	err = ordered_data_init();
	if (err)
		goto free_extent_map;

	err = apfs_delayed_inode_init();
	if (err)
		goto free_ordered_data;

	err = apfs_auto_defrag_init();
	if (err)
		goto free_delayed_inode;

	err = apfs_delayed_ref_init();
	if (err)
		goto free_auto_defrag;

	err = apfs_prelim_ref_init();
	if (err)
		goto free_delayed_ref;

	err = apfs_end_io_wq_init();
	if (err)
		goto free_prelim_ref;

	err = apfs_interface_init();
	if (err)
		goto free_end_io_wq;

	apfs_print_mod_info();

	err = apfs_run_sanity_tests();
	if (err)
		goto unregister_ioctl;

	err = register_filesystem(&apfs_fs_type);
	if (err)
		goto unregister_ioctl;

	return 0;

unregister_ioctl:
	apfs_interface_exit();
free_end_io_wq:
	apfs_end_io_wq_exit();
free_prelim_ref:
	apfs_prelim_ref_exit();
free_delayed_ref:
	apfs_delayed_ref_exit();
free_auto_defrag:
	apfs_auto_defrag_exit();
free_delayed_inode:
	apfs_delayed_inode_exit();
free_ordered_data:
	ordered_data_exit();
free_extent_map:
	extent_map_exit();
free_extent_state_cache:
	extent_state_cache_exit();
free_extent_io:
	extent_io_exit();
free_cachep:
	apfs_destroy_cachep();
free_compress:
	apfs_exit_compress();
	apfs_exit_sysfs();

	return err;
}

static void __exit exit_apfs_fs(void)
{
	apfs_destroy_cachep();
	apfs_delayed_ref_exit();
	apfs_auto_defrag_exit();
	apfs_delayed_inode_exit();
	apfs_prelim_ref_exit();
	ordered_data_exit();
	extent_map_exit();
	extent_state_cache_exit();
	extent_io_exit();
	apfs_interface_exit();
	apfs_end_io_wq_exit();
	unregister_filesystem(&apfs_fs_type);
	apfs_exit_sysfs();
	apfs_cleanup_fs_uuids();
	apfs_exit_compress();
}

late_initcall(init_apfs_fs);
module_exit(exit_apfs_fs)

MODULE_LICENSE("GPL");
MODULE_SOFTDEP("pre: crc32c");
MODULE_SOFTDEP("pre: xxhash64");
MODULE_SOFTDEP("pre: sha256");
MODULE_SOFTDEP("pre: blake2b-256");
