// SPDX-License-Identifier: GPL-2.0
/*
 * myfs_main.c — точка входа модуля, параметры и регистрация в VFS.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/fs_context.h>

#include "myfs.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Educational");
MODULE_DESCRIPTION("Simple custom block-backed filesystem for kernel 6.12");
MODULE_VERSION("1.0");

/* ---------------- параметры модуля ---------------- */

char         *myfs_param_device         = NULL;
unsigned int  myfs_param_sb1_offset     = 0;
unsigned int  myfs_param_sb2_offset     = 1024;
unsigned int  myfs_param_max_name_len   = 32;
unsigned int  myfs_param_max_file_blocks = 8;

module_param_named(device,            myfs_param_device,         charp, 0444);
MODULE_PARM_DESC(device,              "Block device to use for myfs (informational)");

module_param_named(sb1_offset,        myfs_param_sb1_offset,     uint,  0444);
MODULE_PARM_DESC(sb1_offset,          "Block number for primary superblock (default 0)");

module_param_named(sb2_offset,        myfs_param_sb2_offset,     uint,  0444);
MODULE_PARM_DESC(sb2_offset,          "Block number for backup superblock (default 1024)");

module_param_named(max_filename_len,  myfs_param_max_name_len,   uint,  0444);
MODULE_PARM_DESC(max_filename_len,    "Maximum file name length (1..63)");

module_param_named(max_file_blocks,   myfs_param_max_file_blocks, uint, 0444);
MODULE_PARM_DESC(max_file_blocks,     "Maximum file size in blocks (M)");

/* --------------- регистрация ФС в VFS -------------- */

static int myfs_get_tree(struct fs_context *fc)
{
	return get_tree_bdev(fc, myfs_fill_super);
}

static void myfs_free_fc(struct fs_context *fc) { }

static const struct fs_context_operations myfs_context_ops = {
	.get_tree = myfs_get_tree,
	.free     = myfs_free_fc,
};

static int myfs_init_fs_context(struct fs_context *fc)
{
	fc->ops = &myfs_context_ops;
	return 0;
}

struct file_system_type myfs_fs_type = {
	.owner            = THIS_MODULE,
	.name             = MYFS_FS_NAME,
	.init_fs_context  = myfs_init_fs_context,
	.kill_sb          = kill_block_super,
	.fs_flags         = FS_REQUIRES_DEV,
};

/* ------------------- init / exit ------------------- */

static int __init myfs_init(void)
{
	int err;

	pr_info("myfs: loading module\n");
	pr_info("myfs: parameters device='%s' sb1_offset=%u sb2_offset=%u "
		"max_filename_len=%u max_file_blocks=%u\n",
		myfs_param_device ? myfs_param_device : "(unset)",
		myfs_param_sb1_offset,
		myfs_param_sb2_offset,
		myfs_param_max_name_len,
		myfs_param_max_file_blocks);

	if (myfs_param_sb1_offset == myfs_param_sb2_offset) {
		pr_err("myfs: sb1_offset and sb2_offset must differ\n");
		return -EINVAL;
	}
	if (myfs_param_max_name_len == 0 ||
	    myfs_param_max_name_len >= MYFS_NAME_MAX) {
		pr_err("myfs: max_filename_len must be in 1..%d\n", MYFS_NAME_MAX - 1);
		return -EINVAL;
	}
	if (myfs_param_max_file_blocks == 0) {
		pr_err("myfs: max_file_blocks must be > 0\n");
		return -EINVAL;
	}

	err = myfs_init_inodecache();
	if (err) {
		pr_err("myfs: cannot init inode cache: %d\n", err);
		return err;
	}

	err = register_filesystem(&myfs_fs_type);
	if (err) {
		pr_err("myfs: register_filesystem failed: %d\n", err);
		myfs_destroy_inodecache();
		return err;
	}

	pr_info("myfs: filesystem '%s' registered\n", MYFS_FS_NAME);
	return 0;
}

static void __exit myfs_exit(void)
{
	unregister_filesystem(&myfs_fs_type);
	myfs_destroy_inodecache();
	pr_info("myfs: module unloaded\n");
}

module_init(myfs_init);
module_exit(myfs_exit);
