/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  lustre/lib/fsfilt_ext3.c
 *  Lustre filesystem abstraction routines
 *
 *  Copyright (C) 2002, 2003 Cluster File Systems, Inc.
 *   Author: Andreas Dilger <adilger@clusterfs.com>
 *
 *   This file is part of Lustre, http://www.lustre.org.
 *
 *   Lustre is free software; you can redistribute it and/or
 *   modify it under the terms of version 2 of the GNU General Public
 *   License as published by the Free Software Foundation.
 *
 *   Lustre is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Lustre; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif

#define DEBUG_SUBSYSTEM S_FILTER

#include <linux/version.h>
#include <linux/fs.h>
#include <asm/unistd.h>
#include <linux/jbd.h>
#include <linux/slab.h>
#include <linux/pagemap.h>
#include <linux/quotaops.h>
#include <linux/ext3_fs.h>
#include <linux/ext3_jbd.h>
#include <linux/version.h>
/* XXX ugh */
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,5,0))
 #include <linux/ext3_xattr.h>
#else 
 #include <linux/../../fs/ext3/xattr.h>
#endif
#include <linux/kp30.h>
#include <linux/lustre_fsfilt.h>
#include <linux/obd.h>
#include <linux/obd_class.h>
#include <linux/module.h>
#include <linux/lustre_compat25.h>
#include <linux/lvfs.h>
#include "lvfs_internal.h"

#include <linux/obd.h>
#include <linux/lustre_lib.h>

/* Debugging check only needed during development */
#ifdef OBD_CTXT_DEBUG
# define ASSERT_CTXT_MAGIC(magic) LASSERT((magic) == OBD_RUN_CTXT_MAGIC)
# define ASSERT_NOT_KERNEL_CTXT(msg) LASSERT(!segment_eq(get_fs(), get_ds()))
# define ASSERT_KERNEL_CTXT(msg) LASSERT(segment_eq(get_fs(), get_ds()))
#else
# define ASSERT_CTXT_MAGIC(magic) do {} while(0)
# define ASSERT_NOT_KERNEL_CTXT(msg) do {} while(0)
# define ASSERT_KERNEL_CTXT(msg) do {} while(0)
#endif

/* push / pop to root of obd store */
void push_ctxt(struct obd_run_ctxt *save, struct obd_run_ctxt *new_ctx,
               struct obd_ucred *uc)
{
        //ASSERT_NOT_KERNEL_CTXT("already in kernel context!\n");
        ASSERT_CTXT_MAGIC(new_ctx->magic);
        OBD_SET_CTXT_MAGIC(save);

        /*
        CDEBUG(D_INFO,
               "= push %p->%p = cur fs %p pwd %p:d%d:i%d (%*s), pwdmnt %p:%d\n",
               save, current, current->fs, current->fs->pwd,
               atomic_read(&current->fs->pwd->d_count),
               atomic_read(&current->fs->pwd->d_inode->i_count),
               current->fs->pwd->d_name.len, current->fs->pwd->d_name.name,
               current->fs->pwdmnt,
               atomic_read(&current->fs->pwdmnt->mnt_count));
        */

        save->fs = get_fs();
        LASSERT(atomic_read(&current->fs->pwd->d_count));
        LASSERT(atomic_read(&new_ctx->pwd->d_count));
        save->pwd = dget(current->fs->pwd);
        save->pwdmnt = mntget(current->fs->pwdmnt);
        save->ngroups = current->ngroups;

        LASSERT(save->pwd);
        LASSERT(save->pwdmnt);
        LASSERT(new_ctx->pwd);
        LASSERT(new_ctx->pwdmnt);

        if (uc) {
                save->ouc.ouc_fsuid = current->fsuid;
                save->ouc.ouc_fsgid = current->fsgid;
                save->ouc.ouc_cap = current->cap_effective;
                save->ouc.ouc_suppgid1 = current->groups[0];
                save->ouc.ouc_suppgid2 = current->groups[1];

                current->fsuid = uc->ouc_fsuid;
                current->fsgid = uc->ouc_fsgid;
                current->cap_effective = uc->ouc_cap;
                current->ngroups = 0;

                if (uc->ouc_suppgid1 != -1)
                        current->groups[current->ngroups++] = uc->ouc_suppgid1;
                if (uc->ouc_suppgid2 != -1)
                        current->groups[current->ngroups++] = uc->ouc_suppgid2;
        }
        set_fs(new_ctx->fs);
        set_fs_pwd(current->fs, new_ctx->pwdmnt, new_ctx->pwd);

        /*
        CDEBUG(D_INFO,
               "= push %p->%p = cur fs %p pwd %p:d%d:i%d (%*s), pwdmnt %p:%d\n",
               new_ctx, current, current->fs, current->fs->pwd,
               atomic_read(&current->fs->pwd->d_count),
               atomic_read(&current->fs->pwd->d_inode->i_count),
               current->fs->pwd->d_name.len, current->fs->pwd->d_name.name,
               current->fs->pwdmnt,
               atomic_read(&current->fs->pwdmnt->mnt_count));
        */
}
EXPORT_SYMBOL(push_ctxt);

void pop_ctxt(struct obd_run_ctxt *saved, struct obd_run_ctxt *new_ctx,
              struct obd_ucred *uc)
{
        //printk("pc0");
        ASSERT_CTXT_MAGIC(saved->magic);
        //printk("pc1");
        ASSERT_KERNEL_CTXT("popping non-kernel context!\n");

        /*
        CDEBUG(D_INFO,
               " = pop  %p==%p = cur %p pwd %p:d%d:i%d (%*s), pwdmnt %p:%d\n",
               new_ctx, current, current->fs, current->fs->pwd,
               atomic_read(&current->fs->pwd->d_count),
               atomic_read(&current->fs->pwd->d_inode->i_count),
               current->fs->pwd->d_name.len, current->fs->pwd->d_name.name,
               current->fs->pwdmnt,
               atomic_read(&current->fs->pwdmnt->mnt_count));
        */

        LASSERT(current->fs->pwd == new_ctx->pwd);
        LASSERT(current->fs->pwdmnt == new_ctx->pwdmnt);

        set_fs(saved->fs);
        set_fs_pwd(current->fs, saved->pwdmnt, saved->pwd);

        dput(saved->pwd);
        mntput(saved->pwdmnt);
        if (uc) {
                current->fsuid = saved->ouc.ouc_fsuid;
                current->fsgid = saved->ouc.ouc_fsgid;
                current->cap_effective = saved->ouc.ouc_cap;
                current->ngroups = saved->ngroups;
                current->groups[0] = saved->ouc.ouc_suppgid1;
                current->groups[1] = saved->ouc.ouc_suppgid2;
        }

        /*
        CDEBUG(D_INFO,
               "= pop  %p->%p = cur fs %p pwd %p:d%d:i%d (%*s), pwdmnt %p:%d\n",
               saved, current, current->fs, current->fs->pwd,
               atomic_read(&current->fs->pwd->d_count),
               atomic_read(&current->fs->pwd->d_inode->i_count),
               current->fs->pwd->d_name.len, current->fs->pwd->d_name.name,
               current->fs->pwdmnt,
               atomic_read(&current->fs->pwdmnt->mnt_count));
        */
}
EXPORT_SYMBOL(pop_ctxt);

/* utility to make a file */
struct dentry *simple_mknod(struct dentry *dir, char *name, int mode)
{
        struct dentry *dchild;
        int err = 0;
        ENTRY;

        ASSERT_KERNEL_CTXT("kernel doing mknod outside kernel context\n");
        CDEBUG(D_INODE, "creating file %*s\n", (int)strlen(name), name);

        dchild = ll_lookup_one_len(name, dir, strlen(name));
        if (IS_ERR(dchild))
                GOTO(out_up, dchild);

        if (dchild->d_inode) {
                if (!S_ISREG(dchild->d_inode->i_mode))
                        GOTO(out_err, err = -EEXIST);

                GOTO(out_up, dchild);
        }

        err = ll_vfs_create(dir->d_inode, dchild, (mode & ~S_IFMT) | S_IFREG, NULL);
        if (err)
                GOTO(out_err, err);

        RETURN(dchild);

out_err:
        dput(dchild);
        dchild = ERR_PTR(err);
out_up:
        return dchild;
}
EXPORT_SYMBOL(simple_mknod);

/* utility to make a directory */
struct dentry *simple_mkdir(struct dentry *dir, char *name, int mode)
{
        struct dentry *dchild;
        int err = 0;
        ENTRY;

        ASSERT_KERNEL_CTXT("kernel doing mkdir outside kernel context\n");
        CDEBUG(D_INODE, "creating directory %*s\n", (int)strlen(name), name);
        dchild = ll_lookup_one_len(name, dir, strlen(name));
        if (IS_ERR(dchild))
                GOTO(out_up, dchild);

        if (dchild->d_inode) {
                if (!S_ISDIR(dchild->d_inode->i_mode))
                        GOTO(out_err, err = -ENOTDIR);

                GOTO(out_up, dchild);
        }

        err = vfs_mkdir(dir->d_inode, dchild, mode);
        if (err)
                GOTO(out_err, err);

        RETURN(dchild);

out_err:
        dput(dchild);
        dchild = ERR_PTR(err);
out_up:
        return dchild;
}
EXPORT_SYMBOL(simple_mkdir);

/*
 * Read a file from within kernel context.  Prior to calling this
 * function we should already have done a push_ctxt().
 */
int lustre_fread(struct file *file, void *buf, int len, loff_t *off)
{
        ASSERT_KERNEL_CTXT("kernel doing read outside kernel context\n");
        if (!file || !file->f_op || !file->f_op->read || !off)
                RETURN(-ENOSYS);

        return file->f_op->read(file, buf, len, off);
}
EXPORT_SYMBOL(lustre_fread);

/*
 * Write a file from within kernel context.  Prior to calling this
 * function we should already have done a push_ctxt().
 */
int lustre_fwrite(struct file *file, const void *buf, int len, loff_t *off)
{
        ENTRY;
        ASSERT_KERNEL_CTXT("kernel doing write outside kernel context\n");
        if (!file)
                RETURN(-ENOENT);
        if (!file->f_op)
                RETURN(-ENOSYS);
        if (!off)
                RETURN(-EINVAL);

        if (!file->f_op->write)
                RETURN(-EROFS);

        RETURN(file->f_op->write(file, buf, len, off));
}
EXPORT_SYMBOL(lustre_fwrite);

/*
 * Sync a file from within kernel context.  Prior to calling this
 * function we should already have done a push_ctxt().
 */
int lustre_fsync(struct file *file)
{
        ENTRY;
        ASSERT_KERNEL_CTXT("kernel doing sync outside kernel context\n");
        if (!file || !file->f_op || !file->f_op->fsync)
                RETURN(-ENOSYS);

        RETURN(file->f_op->fsync(file, file->f_dentry, 0));
}
EXPORT_SYMBOL(lustre_fsync);







#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,5,0))

static int __init lvfs_linux_init(void)
{
        RETURN(0);
}

static void __exit lvfs_linux_exit(void)
{

        return;
}

MODULE_AUTHOR("Cluster File Systems, Inc. <info@clusterfs.com>");
MODULE_DESCRIPTION("Lustre VFS Filesystem Helper v0.1");
MODULE_LICENSE("GPL");

module_init(lvfs_linux_init);
module_exit(lvfs_linux_exit);

#else

#warning "lvfs_linux_init() and fsfilt_ext3_exit() aren't called on 2.6. MUST be fixed"


#endif
