/*
 * myfs.h — общие определения для модуля ядра и пользовательской программы.
 *
 * Дисковый формат:
 *   block sb1_offset       : суперблок (копия 1)
 *   block sb2_offset       : суперблок (копия 2)
 *   blocks [data_start..]  : файлы по M блоков каждый
 *
 * Единица обмена с устройством — блок 4096 байт ("сектор" в терминах задания).
 */
#ifndef _MYFS_H
#define _MYFS_H

#ifdef __KERNEL__
#  include <linux/types.h>
#  include <linux/ioctl.h>
#else
#  include <stdint.h>
#  include <sys/ioctl.h>
   typedef uint8_t  u8;
   typedef uint32_t u32;
   typedef uint64_t u64;
#  ifndef __le32
#    define __le32 uint32_t
#  endif
#endif

#define MYFS_MAGIC          0x4D594653U  /* 'MYFS' */
#define MYFS_VERSION        1
#define MYFS_BLOCK_SIZE     4096         /* размер «сектора» нашей ФС */
#define MYFS_FS_NAME        "myfs"
#define MYFS_NAME_MAX       64           /* верхний предел длины имени файла  */
#define MYFS_MAX_HASHES     1024         /* максимум записей в IOCTL hashes   */

/*
 * Структура суперблока на диске.
 * Поле hash — CRC32 от всех предшествующих полей (магия включена,
 * само поле hash и padding не входят).
 */
struct myfs_disk_superblock {
	__le32 magic;
	__le32 version;
	__le32 block_size;
	__le32 file_size_blocks;   /* M — размер одного файла в блоках  */
	__le32 max_filename_len;
	__le32 num_files;
	__le32 sb1_offset;         /* номер блока с копией 1 */
	__le32 sb2_offset;         /* номер блока с копией 2 */
	__le32 data_start_block;   /* блок, с которого начинаются файлы */
	__le32 total_blocks;       /* всего блоков на устройстве */
	__le32 hash;               /* CRC32 целостности */
	u8     padding[4096 - 11 * sizeof(__le32)];
};

/* ----------------------------- IOCTL ----------------------------- */

#define MYFS_IOC_MAGIC      'M'

struct myfs_hash_entry {
	char name[MYFS_NAME_MAX];
	u32  hash;
};

struct myfs_hashes {
	u32 count;                              /* in/out: фактически заполнено */
	u32 capacity;                           /* in: размер массива entries   */
	struct myfs_hash_entry entries[MYFS_MAX_HASHES];
};

struct myfs_mapping {
	char name[MYFS_NAME_MAX];               /* in:  имя файла  */
	u32  file_index;                        /* out: индекс    */
	u32  start_block;                       /* out: первый блок */
	u32  num_blocks;                        /* out: число блоков */
};

#define MYFS_IOC_ZERO_ALL    _IO  (MYFS_IOC_MAGIC, 1)
#define MYFS_IOC_ERASE_FS    _IO  (MYFS_IOC_MAGIC, 2)
#define MYFS_IOC_GET_HASHES  _IOWR(MYFS_IOC_MAGIC, 3, struct myfs_hashes *)
#define MYFS_IOC_GET_MAPPING _IOWR(MYFS_IOC_MAGIC, 4, struct myfs_mapping)

/* ---------------------- Только для ядра ------------------------- */
#ifdef __KERNEL__

#include <linux/fs.h>

/* In-core superblock info — храним то, что нужно во время работы.       */
struct myfs_sb_info {
	u32  magic;
	u32  block_size;
	u32  file_size_blocks;
	u32  max_filename_len;
	u32  num_files;
	u32  sb1_offset;
	u32  sb2_offset;
	u32  data_start_block;
	u32  total_blocks;
};

/* Кастомный inode info с обёрткой над VFS inode.                       */
struct myfs_inode_info {
	u32 file_index;        /* (u32)-1 для корня */
	u32 start_block;
	u32 num_blocks;
	struct inode vfs_inode;
};

static inline struct myfs_sb_info *MYFS_SB(struct super_block *sb)
{
	return sb->s_fs_info;
}

static inline struct myfs_inode_info *MYFS_I(struct inode *inode)
{
	return container_of(inode, struct myfs_inode_info, vfs_inode);
}

/* Глобальные параметры модуля (myfs_main.c) */
extern char         *myfs_param_device;
extern unsigned int  myfs_param_sb1_offset;
extern unsigned int  myfs_param_sb2_offset;
extern unsigned int  myfs_param_max_name_len;
extern unsigned int  myfs_param_max_file_blocks;

/* Файловая система и кэш inode'ов */
extern struct file_system_type   myfs_fs_type;
extern const struct super_operations myfs_sops;
extern const struct inode_operations myfs_dir_iops;
extern const struct file_operations  myfs_dir_fops;
extern const struct inode_operations myfs_file_iops;
extern const struct file_operations  myfs_file_fops;
extern struct kmem_cache            *myfs_inode_cachep;

/* Прототипы (myfs_super.c)  */
int      myfs_fill_super(struct super_block *sb, struct fs_context *fc);
u32      myfs_sb_compute_hash(const struct myfs_disk_superblock *dsb);
int      myfs_write_superblock(struct super_block *sb, u32 block);
int      myfs_init_inodecache(void);
void     myfs_destroy_inodecache(void);

/* Прототипы (myfs_inode.c) */
struct inode *myfs_iget_root(struct super_block *sb);
struct inode *myfs_iget_file(struct super_block *sb, u32 file_index);

/* Прототипы (myfs_file.c)  */
long     myfs_ioctl_dispatch(struct file *filp, unsigned int cmd, unsigned long arg);

#endif /* __KERNEL__ */
#endif /* _MYFS_H */
