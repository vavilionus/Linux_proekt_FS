// SPDX-License-Identifier: GPL-2.0
/*
 * myfs_super.c — операции суперблока, fill_super(), форматирование.
 */
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/buffer_head.h>
#include <linux/blkdev.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/crc32.h>
#include <linux/statfs.h>

#include "myfs.h"

struct kmem_cache *myfs_inode_cachep;

/* ------------- кэш inode'ов ------------- */

static struct inode *myfs_alloc_inode(struct super_block *sb)
{
	struct myfs_inode_info *mi;

	mi = alloc_inode_sb(sb, myfs_inode_cachep, GFP_KERNEL);
	if (!mi)
		return NULL;
	mi->file_index  = (u32)-1;
	mi->start_block = 0;
	mi->num_blocks  = 0;
	return &mi->vfs_inode;
}

static void myfs_free_inode(struct inode *inode)
{
	kmem_cache_free(myfs_inode_cachep, MYFS_I(inode));
}

static void myfs_inode_init_once(void *p)
{
	struct myfs_inode_info *mi = p;
	inode_init_once(&mi->vfs_inode);
}

int myfs_init_inodecache(void)
{
	myfs_inode_cachep = kmem_cache_create("myfs_inode_cache",
		sizeof(struct myfs_inode_info), 0,
		SLAB_RECLAIM_ACCOUNT | SLAB_ACCOUNT,
		myfs_inode_init_once);
	return myfs_inode_cachep ? 0 : -ENOMEM;
}

void myfs_destroy_inodecache(void)
{
	rcu_barrier();
	if (myfs_inode_cachep) {
		kmem_cache_destroy(myfs_inode_cachep);
		myfs_inode_cachep = NULL;
	}
}

/* ------------- хэш суперблока ----------- */

u32 myfs_sb_compute_hash(const struct myfs_disk_superblock *dsb)
{
	/* CRC32 от полей с magic по total_blocks включительно (10 × u32). */
	return crc32_le(0, (const u8 *)dsb, offsetof(struct myfs_disk_superblock, hash));
}

/* ----------- запись/чтение суперблока с диска -------- */

static int myfs_read_dsb(struct super_block *sb, u32 block,
                         struct myfs_disk_superblock *out)
{
	struct buffer_head *bh = sb_bread(sb, block);
	if (!bh) {
		pr_err("myfs: sb_bread(block=%u) failed\n", block);
		return -EIO;
	}
	memcpy(out, bh->b_data, sizeof(*out));
	brelse(bh);
	return 0;
}

static int myfs_write_dsb(struct super_block *sb, u32 block,
                          const struct myfs_disk_superblock *in)
{
	struct buffer_head *bh = sb_bread(sb, block);
	if (!bh)
		return -EIO;
	lock_buffer(bh);
	memcpy(bh->b_data, in, sizeof(*in));
	/* остальное место в блоке зануляем */
	if (sb->s_blocksize > sizeof(*in))
		memset(bh->b_data + sizeof(*in), 0,
		       sb->s_blocksize - sizeof(*in));
	set_buffer_uptodate(bh);
	mark_buffer_dirty(bh);
	unlock_buffer(bh);
	sync_dirty_buffer(bh);
	brelse(bh);
	return 0;
}

int myfs_write_superblock(struct super_block *sb, u32 block)
{
	struct myfs_sb_info *sbi = MYFS_SB(sb);
	struct myfs_disk_superblock *dsb = kzalloc(sizeof(*dsb), GFP_KERNEL);
	int err;

	if (!dsb)
		return -ENOMEM;

	dsb->magic            = cpu_to_le32(MYFS_MAGIC);
	dsb->version          = cpu_to_le32(MYFS_VERSION);
	dsb->block_size       = cpu_to_le32(sbi->block_size);
	dsb->file_size_blocks = cpu_to_le32(sbi->file_size_blocks);
	dsb->max_filename_len = cpu_to_le32(sbi->max_filename_len);
	dsb->num_files        = cpu_to_le32(sbi->num_files);
	dsb->sb1_offset       = cpu_to_le32(sbi->sb1_offset);
	dsb->sb2_offset       = cpu_to_le32(sbi->sb2_offset);
	dsb->data_start_block = cpu_to_le32(sbi->data_start_block);
	dsb->total_blocks     = cpu_to_le32(sbi->total_blocks);
	dsb->hash             = cpu_to_le32(myfs_sb_compute_hash(dsb));

	err = myfs_write_dsb(sb, block, dsb);
	kfree(dsb);
	return err;
}

/* ------------- проверка валидности dsb -------------- */

static bool myfs_sb_is_valid(const struct myfs_disk_superblock *dsb)
{
	u32 stored, computed;

	if (le32_to_cpu(dsb->magic) != MYFS_MAGIC)
		return false;
	if (le32_to_cpu(dsb->version) != MYFS_VERSION)
		return false;
	if (le32_to_cpu(dsb->block_size) != MYFS_BLOCK_SIZE)
		return false;

	stored = le32_to_cpu(dsb->hash);
	computed = myfs_sb_compute_hash(dsb);

	if (stored != computed) {
		pr_warn("myfs: superblock hash mismatch (stored=0x%08x, computed=0x%08x)\n",
			stored, computed);
		return false;
	}

	return true;
}

static void myfs_dsb_to_sbi(const struct myfs_disk_superblock *dsb,
			    struct myfs_sb_info *sbi)
{
	sbi->magic            = le32_to_cpu(dsb->magic);
	sbi->block_size       = le32_to_cpu(dsb->block_size);
	sbi->file_size_blocks = le32_to_cpu(dsb->file_size_blocks);
	sbi->max_filename_len = le32_to_cpu(dsb->max_filename_len);
	sbi->num_files        = le32_to_cpu(dsb->num_files);
	sbi->sb1_offset       = le32_to_cpu(dsb->sb1_offset);
	sbi->sb2_offset       = le32_to_cpu(dsb->sb2_offset);
	sbi->data_start_block = le32_to_cpu(dsb->data_start_block);
	sbi->total_blocks     = le32_to_cpu(dsb->total_blocks);
}

/* ------------- форматирование чистого устройства ---- */

static int myfs_format_device(struct super_block *sb)
{
	struct myfs_sb_info *sbi = MYFS_SB(sb);
	u32 data_start;
	u32 total_blocks;
	int err;

	total_blocks = (u32)(bdev_nr_bytes(sb->s_bdev) / MYFS_BLOCK_SIZE);
	if (total_blocks == 0)
		return -ENOSPC;

	data_start = max(myfs_param_sb1_offset, myfs_param_sb2_offset) + 1;
	if (data_start >= total_blocks)
		return -ENOSPC;

	sbi->block_size       = MYFS_BLOCK_SIZE;
	sbi->file_size_blocks = myfs_param_max_file_blocks;
	sbi->max_filename_len = myfs_param_max_name_len;
	sbi->sb1_offset       = myfs_param_sb1_offset;
	sbi->sb2_offset       = myfs_param_sb2_offset;
	sbi->total_blocks     = total_blocks;
	sbi->data_start_block = data_start;
	sbi->num_files        = (total_blocks - data_start) / sbi->file_size_blocks;

	if (sbi->num_files == 0) {
		pr_err("myfs: device too small — no room for files\n");
		return -ENOSPC;
	}

	pr_info("myfs: formatting device (%u blocks, %u files of %u blocks each)\n",
		total_blocks, sbi->num_files, sbi->file_size_blocks);

	err = myfs_write_superblock(sb, sbi->sb1_offset);
	if (err)
		return err;
	err = myfs_write_superblock(sb, sbi->sb2_offset);
	if (err)
		return err;

	pr_info("myfs: format complete\n");
	return 0;
}

/* ----------------- super_operations ----------------- */

static int myfs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct super_block *sb = dentry->d_sb;
	struct myfs_sb_info *sbi = MYFS_SB(sb);

	buf->f_type    = MYFS_MAGIC;
	buf->f_bsize   = sbi->block_size;
	buf->f_blocks  = sbi->total_blocks;
	buf->f_bfree   = 0;
	buf->f_bavail  = 0;
	buf->f_files   = sbi->num_files;
	buf->f_ffree   = 0;
	buf->f_namelen = sbi->max_filename_len;
	return 0;
}

static void myfs_put_super(struct super_block *sb)
{
	struct myfs_sb_info *sbi = sb->s_fs_info;
	pr_info("myfs: unmounting\n");
	kfree(sbi);
	sb->s_fs_info = NULL;
}

const struct super_operations myfs_sops = {
	.alloc_inode    = myfs_alloc_inode,
	.free_inode     = myfs_free_inode,
	.statfs         = myfs_statfs,
	.put_super      = myfs_put_super,
	.drop_inode     = generic_delete_inode,
};

/* ------------------ fill_super ---------------------- */

int myfs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct myfs_sb_info *sbi;
	struct myfs_disk_superblock *dsb1, *dsb2;
	struct inode *root;
	int err = 0;
	bool need_format = false;

	if (!sb_set_blocksize(sb, MYFS_BLOCK_SIZE)) {
		pr_err("myfs: cannot set blocksize %d\n", MYFS_BLOCK_SIZE);
		return -EIO;
	}

	sb->s_magic    = MYFS_MAGIC;
	sb->s_op       = &myfs_sops;
	sb->s_maxbytes = MAX_LFS_FILESIZE;
	sb->s_time_gran = 1;

	sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
	if (!sbi)
		return -ENOMEM;
	sb->s_fs_info = sbi;

	dsb1 = kzalloc(sizeof(*dsb1), GFP_KERNEL);
	dsb2 = kzalloc(sizeof(*dsb2), GFP_KERNEL);
	if (!dsb1 || !dsb2) {
		err = -ENOMEM;
		goto out_free;
	}

	err = myfs_read_dsb(sb, myfs_param_sb1_offset, dsb1);
	if (err)
		goto out_free;
	err = myfs_read_dsb(sb, myfs_param_sb2_offset, dsb2);
	if (err)
		goto out_free;

	if (myfs_sb_is_valid(dsb1)) {
		pr_info("myfs: primary superblock OK\n");
		myfs_dsb_to_sbi(dsb1, sbi);
		/* если резерв повреждён — восстановим */
		if (!myfs_sb_is_valid(dsb2)) {
			pr_warn("myfs: secondary superblock corrupted, restoring\n");
			(void)myfs_write_superblock(sb, sbi->sb2_offset);
		}
	} else if (myfs_sb_is_valid(dsb2)) {
		pr_warn("myfs: primary corrupted, recovering from secondary\n");
		myfs_dsb_to_sbi(dsb2, sbi);
		(void)myfs_write_superblock(sb, sbi->sb1_offset);
	} else {
		pr_info("myfs: no valid superblock found, formatting\n");
		need_format = true;
	}

	if (need_format) {
		err = myfs_format_device(sb);
		if (err)
			goto out_free;
	}

	pr_info("myfs: mounted: %u files, %u blocks each, data starts at block %u\n",
		sbi->num_files, sbi->file_size_blocks, sbi->data_start_block);

	root = myfs_iget_root(sb);
	if (IS_ERR(root)) {
		err = PTR_ERR(root);
		goto out_free;
	}
	sb->s_root = d_make_root(root);
	if (!sb->s_root) {
		err = -ENOMEM;
		goto out_free;
	}

	kfree(dsb1);
	kfree(dsb2);
	return 0;

out_free:
	kfree(dsb1);
	kfree(dsb2);
	kfree(sbi);
	sb->s_fs_info = NULL;
	return err;
}
