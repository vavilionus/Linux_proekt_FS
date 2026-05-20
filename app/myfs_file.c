// SPDX-License-Identifier: GPL-2.0
/*
 * myfs_file.c — read/write через buffer_head, обработчик IOCTL.
 */
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/buffer_head.h>
#include <linux/uio.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/crc32.h>
#include <linux/string.h>

#include "myfs.h"

/* ---------------- read / write ------------------ */

static ssize_t myfs_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	struct file *filp = iocb->ki_filp;
	struct inode *inode = file_inode(filp);
	struct super_block *sb = inode->i_sb;
	struct myfs_sb_info *sbi = MYFS_SB(sb);
	struct myfs_inode_info *mi = MYFS_I(inode);
	loff_t pos = iocb->ki_pos;
	loff_t file_sz = (loff_t)mi->num_blocks * sbi->block_size;
	size_t want = iov_iter_count(to);
	size_t copied = 0;

	if (pos < 0)
		return -EINVAL;
	if (pos >= file_sz)
		return 0;
	if (pos + want > file_sz)
		want = file_sz - pos;

	while (copied < want) {
		sector_t blk = mi->start_block + (pos + copied) / sbi->block_size;
		size_t off   = (pos + copied) % sbi->block_size;
		size_t chunk = sbi->block_size - off;
		struct buffer_head *bh;
		size_t got;

		if (chunk > want - copied)
			chunk = want - copied;

		bh = sb_bread(sb, blk);
		if (!bh)
			return copied ? (ssize_t)copied : -EIO;

		got = copy_to_iter(bh->b_data + off, chunk, to);
		brelse(bh);
		if (got == 0)
			break;
		copied += got;
	}

	iocb->ki_pos = pos + copied;
	return copied;
}

static ssize_t myfs_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct file *filp = iocb->ki_filp;
	struct inode *inode = file_inode(filp);
	struct super_block *sb = inode->i_sb;
	struct myfs_sb_info *sbi = MYFS_SB(sb);
	struct myfs_inode_info *mi = MYFS_I(inode);
	loff_t pos = iocb->ki_pos;
	loff_t file_sz = (loff_t)mi->num_blocks * sbi->block_size;
	size_t want = iov_iter_count(from);
	size_t written = 0;

	if (pos < 0)
		return -EINVAL;
	if (pos >= file_sz)
		return -ENOSPC;
	if (pos + want > file_sz)
		want = file_sz - pos;

	inode_lock(inode);

	while (written < want) {
		sector_t blk = mi->start_block + (pos + written) / sbi->block_size;
		size_t off   = (pos + written) % sbi->block_size;
		size_t chunk = sbi->block_size - off;
		struct buffer_head *bh;
		size_t got;

		if (chunk > want - written)
			chunk = want - written;

		/* Если перезаписываем целый блок — sb_getblk избежит чтения. */
		if (off == 0 && chunk == sbi->block_size)
			bh = sb_getblk(sb, blk);
		else
			bh = sb_bread(sb, blk);

		if (!bh) {
			inode_unlock(inode);
			return written ? (ssize_t)written : -EIO;
		}

		lock_buffer(bh);
		got = copy_from_iter(bh->b_data + off, chunk, from);
		if (got == 0) {
			unlock_buffer(bh);
			brelse(bh);
			break;
		}
		set_buffer_uptodate(bh);
		mark_buffer_dirty(bh);
		unlock_buffer(bh);
		brelse(bh);

		written += got;
	}

	if (written > 0) {
		simple_inode_init_ts(inode);
		mark_inode_dirty(inode);
	}

	inode_unlock(inode);
	iocb->ki_pos = pos + written;
	return written;
}

static int myfs_open(struct inode *inode, struct file *file)
{
	return generic_file_open(inode, file);
}

/* --------------------- IOCTL ------------------------ */

static int myfs_ioctl_zero_all(struct super_block *sb)
{
	struct myfs_sb_info *sbi = MYFS_SB(sb);
	u32 blk;
	u32 last = sbi->data_start_block + sbi->num_files * sbi->file_size_blocks;
	int err = 0;

	pr_info("myfs: ioctl ZERO_ALL: %u blocks\n",
	        last - sbi->data_start_block);

	for (blk = sbi->data_start_block; blk < last; blk++) {
		struct buffer_head *bh = sb_getblk(sb, blk);
		if (!bh) {
			err = -EIO;
			break;
		}
		lock_buffer(bh);
		memset(bh->b_data, 0, sb->s_blocksize);
		set_buffer_uptodate(bh);
		mark_buffer_dirty(bh);
		unlock_buffer(bh);
		sync_dirty_buffer(bh);
		brelse(bh);
	}

	return err;
}

static int myfs_ioctl_erase_fs(struct super_block *sb)
{
	struct myfs_sb_info *sbi = MYFS_SB(sb);
	struct buffer_head *bh;

	pr_warn("myfs: ioctl ERASE_FS — wiping both superblocks\n");

	bh = sb_getblk(sb, sbi->sb1_offset);
	if (bh) {
		lock_buffer(bh);
		memset(bh->b_data, 0, sb->s_blocksize);
		set_buffer_uptodate(bh);
		mark_buffer_dirty(bh);
		unlock_buffer(bh);
		sync_dirty_buffer(bh);
		brelse(bh);
	}
	bh = sb_getblk(sb, sbi->sb2_offset);
	if (bh) {
		lock_buffer(bh);
		memset(bh->b_data, 0, sb->s_blocksize);
		set_buffer_uptodate(bh);
		mark_buffer_dirty(bh);
		unlock_buffer(bh);
		sync_dirty_buffer(bh);
		brelse(bh);
	}
	return 0;
}

/* Считаем CRC32 содержимого одного файла. */
static int myfs_file_crc32(struct super_block *sb, u32 file_index, u32 *out_hash)
{
	struct myfs_sb_info *sbi = MYFS_SB(sb);
	u32 start = sbi->data_start_block + file_index * sbi->file_size_blocks;
	u32 i;
	u32 crc = 0;

	for (i = 0; i < sbi->file_size_blocks; i++) {
		struct buffer_head *bh = sb_bread(sb, start + i);
		if (!bh)
			return -EIO;
		crc = crc32_le(crc, bh->b_data, sb->s_blocksize);
		brelse(bh);
	}
	*out_hash = crc;
	return 0;
}

static int myfs_ioctl_get_hashes(struct super_block *sb,
                                 struct myfs_hashes __user *uarg)
{
	struct myfs_sb_info *sbi = MYFS_SB(sb);
	struct myfs_hashes *h;
	u32 i, want;
	int err = 0;

	h = kvzalloc(sizeof(*h), GFP_KERNEL);
	if (!h)
		return -ENOMEM;

	if (copy_from_user(h, uarg, sizeof(h->count) + sizeof(h->capacity))) {
		err = -EFAULT;
		goto out;
	}

	want = min3(sbi->num_files,
	            h->capacity ? h->capacity : MYFS_MAX_HASHES,
	            (u32)MYFS_MAX_HASHES);

	h->count    = want;
	h->capacity = MYFS_MAX_HASHES;

	for (i = 0; i < want; i++) {
		scnprintf(h->entries[i].name, MYFS_NAME_MAX, "file_%u", i);
		err = myfs_file_crc32(sb, i, &h->entries[i].hash);
		if (err)
			goto out;
	}

	if (copy_to_user(uarg, h, sizeof(*h)))
		err = -EFAULT;
out:
	kvfree(h);
	return err;
}

static int myfs_ioctl_get_mapping(struct super_block *sb,
                                  struct myfs_mapping __user *uarg)
{
	struct myfs_sb_info *sbi = MYFS_SB(sb);
	struct myfs_mapping m;
	unsigned long v;
	const char prefix[] = "file_";
	const size_t plen   = sizeof(prefix) - 1;
	size_t namelen;

	if (copy_from_user(&m, uarg, sizeof(m)))
		return -EFAULT;

	m.name[MYFS_NAME_MAX - 1] = '\0';
	namelen = strnlen(m.name, MYFS_NAME_MAX);

	if (namelen <= plen || memcmp(m.name, prefix, plen) != 0)
		return -ENOENT;
	if (kstrtoul(m.name + plen, 10, &v) != 0)
		return -ENOENT;
	if (v >= sbi->num_files)
		return -ENOENT;

	m.file_index  = (u32)v;
	m.start_block = sbi->data_start_block + (u32)v * sbi->file_size_blocks;
	m.num_blocks  = sbi->file_size_blocks;

	if (copy_to_user(uarg, &m, sizeof(m)))
		return -EFAULT;
	return 0;
}

long myfs_ioctl_dispatch(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct super_block *sb = file_inode(filp)->i_sb;

	switch (cmd) {
	case MYFS_IOC_ZERO_ALL:
		return myfs_ioctl_zero_all(sb);
	case MYFS_IOC_ERASE_FS:
		return myfs_ioctl_erase_fs(sb);
	case MYFS_IOC_GET_HASHES:
		return myfs_ioctl_get_hashes(sb,
		         (struct myfs_hashes __user *)arg);
	case MYFS_IOC_GET_MAPPING:
		return myfs_ioctl_get_mapping(sb,
		         (struct myfs_mapping __user *)arg);
	default:
		return -ENOTTY;
	}
}

/* ----------------- file operations table ------------- */

const struct inode_operations myfs_file_iops = {
	/* Никаких link/rename/unlink/setattr — спецификация запрещает. */
	.getattr = simple_getattr,
};

const struct file_operations myfs_file_fops = {
	.owner          = THIS_MODULE,
	.open           = myfs_open,
	.read_iter      = myfs_read_iter,
	.write_iter     = myfs_write_iter,
	.llseek         = generic_file_llseek,
	.unlocked_ioctl = myfs_ioctl_dispatch,
	.compat_ioctl   = myfs_ioctl_dispatch,
};
