/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 */
#ifndef __LVFS_LINUX_H__
#define __LVFS_LINUX_H__

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,5,0))
#include <linux/namei.h>
#endif
#include <linux/sched.h>

#include <lvfs.h>

#define l_file file
#define l_dentry dentry
#define l_inode inode

#define l_filp_open filp_open

struct lvfs_run_ctxt;
struct l_file *l_dentry_open(struct lvfs_run_ctxt *, struct l_dentry *,
                             int flags);

struct l_linux_dirent {
        struct list_head lld_list;
        ino_t           lld_ino;
        unsigned long   lld_off;
        char            lld_name[LL_FID_NAMELEN];
};
struct l_readdir_callback {
        struct l_linux_dirent *lrc_dirent;
        struct list_head      *lrc_list;
};

#define LVFS_DENTRY_PARAM_MAGIC         20070216UL
struct lvfs_dentry_params
{
        unsigned long    ldp_inum;
        void            *ldp_ptr;
        __u32            ldp_magic;
};
#define LVFS_DENTRY_PARAMS_INIT         { .ldp_magic = LVFS_DENTRY_PARAM_MAGIC }

# if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,5,0))
#  define BDEVNAME_DECLARE_STORAGE(foo) char foo[BDEVNAME_SIZE]
#  define ll_bdevname(SB, STORAGE) __bdevname(kdev_t_to_nr(SB->s_dev), STORAGE)
#  define lvfs_sbdev(SB)       ((SB)->s_bdev)
#  define lvfs_sbdev_type      struct block_device *
   int fsync_bdev(struct block_device *);
#  define lvfs_sbdev_sync      fsync_bdev
# else
#  define BDEVNAME_DECLARE_STORAGE(foo) char __unused_##foo
#  define ll_bdevname(SB,STORAGE) ((void)__unused_##STORAGE,bdevname(lvfs_sbdev(SB)))
#  define lvfs_sbdev(SB)       (kdev_t_to_nr((SB)->s_dev))
#  define lvfs_sbdev_type      kdev_t
#  define lvfs_sbdev_sync      fsync_dev
# endif

/* Instead of calling within lvfs (a layering violation) */
#define lvfs_set_rdonly(obd, sb) \
        __lvfs_set_rdonly(lvfs_sbdev(sb), fsfilt_journal_sbdev(obd, sb))

void __lvfs_set_rdonly(lvfs_sbdev_type dev, lvfs_sbdev_type jdev);

int lvfs_check_rdonly(lvfs_sbdev_type dev);
void lvfs_clear_rdonly(lvfs_sbdev_type dev);

#endif /*  __LVFS_LINUX_H__ */
