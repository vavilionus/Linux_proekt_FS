// SPDX-License-Identifier: GPL-2.0
/*
 * myfs_inode.c - операции inode'ов, поиск и листинг.
 */
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/namei.h>
#include <linux/dcache.h>

#include "myfs.h"

/* «file_N» - генерим и парсим имя по индексу. */
static int myfs_format_name(char *buf, size_t bufsz, u32 idx)
{
	return scnprintf(buf, bufsz, "file_%u", idx);
}

static int myfs_parse_index(const char *name, size_t len, u32 max, u32 *out)
{
	const char prefix[] = "file_";
	const size_t plen   = sizeof(prefix) - 1;
	unsigned long v;
	char buf[16];

	if (len <= plen || memcmp(name, prefix, plen) != 0)
		return -ENOENT;
	if (len - plen >= sizeof(buf))
		return -ENOENT;
	memcpy(buf, name + plen, len - plen);
	buf[len - plen] = '\0';

	if (kstrtoul(buf, 10, &v) != 0)
		return -ENOENT;
	if (v >= max)
		return -ENOENT;
	*out = (u32)v;
	return 0;
}

/* Создание (или получение из кэша) inode для конкретного файла */
struct inode *myfs_iget_file(struct super_block *sb, u32 file_index)
{
	struct myfs_sb_info *sbi = MYFS_SB(sb);
	struct inode *inode;
	struct myfs_inode_info *mi;

	if (file_index >= sbi->num_files)
		return ERR_PTR(-ENOENT);

	inode = iget_locked(sb, file_index + 2);
	if (!inode)
		return ERR_PTR(-ENOMEM);

	if (!(inode->i_state & I_NEW))
		return inode;

	mi              = MYFS_I(inode);
	mi->file_index  = file_index;
	mi->start_block = sbi->data_start_block + file_index * sbi->file_size_blocks;
	mi->num_blocks  = sbi->file_size_blocks;

	inode->i_mode   = S_IFREG | 0644;
	inode->i_uid    = current_fsuid();
	inode->i_gid    = current_fsgid();
	inode->i_size   = (loff_t)mi->num_blocks * sbi->block_size;
	inode->i_blocks = mi->num_blocks * (sbi->block_size / 512);
	inode->i_op     = &myfs_file_iops;
	inode->i_fop    = &myfs_file_fops;
	simple_inode_init_ts(inode);
	set_nlink(inode, 1);

	unlock_new_inode(inode);
	return inode;
}

/* Корневой каталог. */
struct inode *myfs_iget_root(struct super_block *sb)
{
	struct inode *inode;
	struct myfs_inode_info *mi;

	inode = iget_locked(sb, 1);
	if (!inode)
		return ERR_PTR(-ENOMEM);

	if (!(inode->i_state & I_NEW))
		return inode;

	mi              = MYFS_I(inode);
	mi->file_index  = (u32)-1;
	mi->start_block = 0;
	mi->num_blocks  = 0;

	inode->i_mode = S_IFDIR | 0755;
	inode->i_uid  = current_fsuid();
	inode->i_gid  = current_fsgid();
	inode->i_size = MYFS_BLOCK_SIZE;
	inode->i_op   = &myfs_dir_iops;
	inode->i_fop  = &myfs_dir_fops;
	simple_inode_init_ts(inode);
	set_nlink(inode, 2);
	inode->i_blocks = 0;

	unlock_new_inode(inode);
	return inode;
}

/* lookup в корне: парсим имя, отдаем inode либо NULL */
static struct dentry *myfs_lookup(struct inode *dir, struct dentry *dentry,
                                  unsigned int flags)
{
	struct super_block *sb = dir->i_sb;
	struct myfs_sb_info *sbi = MYFS_SB(sb);
	struct inode *inode = NULL;
	u32 idx;

	if (dentry->d_name.len > sbi->max_filename_len)
		return ERR_PTR(-ENAMETOOLONG);

	if (myfs_parse_index(dentry->d_name.name, dentry->d_name.len,
	                     sbi->num_files, &idx) == 0) {
		inode = myfs_iget_file(sb, idx);
		if (IS_ERR(inode))
			return ERR_CAST(inode);
	}
	return d_splice_alias(inode, dentry);
}

/* readdir: «.» «..» затем file_0…file_{num_files-1} */
static int myfs_readdir(struct file *file, struct dir_context *ctx)
{
	struct inode *inode = file_inode(file);
	struct myfs_sb_info *sbi = MYFS_SB(inode->i_sb);
	char name[MYFS_NAME_MAX];

	if (!dir_emit_dots(file, ctx))
		return 0;

	while (ctx->pos >= 2 && (u32)(ctx->pos - 2) < sbi->num_files) {
		u32 idx = ctx->pos - 2;
		int n = myfs_format_name(name, sizeof(name), idx);
		if (!dir_emit(ctx, name, n, idx + 2, DT_REG))
			return 0;
		ctx->pos++;
	}
	return 0;
}

const struct inode_operations myfs_dir_iops = {
	.lookup = myfs_lookup,
};

const struct file_operations myfs_dir_fops = {
	.owner          = THIS_MODULE,
	.read           = generic_read_dir,
	.iterate_shared = myfs_readdir,
	.llseek         = generic_file_llseek,
	.unlocked_ioctl = myfs_ioctl_dispatch,
	.compat_ioctl   = myfs_ioctl_dispatch,
};
