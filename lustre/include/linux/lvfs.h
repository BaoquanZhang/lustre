#ifndef __LVFS_H__
#define __LVFS_H__

#include <linux/kp30.h>

#define LL_FID_NAMELEN	(16 + 1 + 8 + 1)

#if defined __KERNEL__
#include <linux/lvfs_linux.h>
#endif 

#ifdef LIBLUSTRE
#include <lvfs_user_fs.h>
#endif

/* simple.c */
struct lvfs_ucred {
        __u32 luc_fsuid;
        __u32 luc_fsgid;
        __u32 luc_cap;
        __u32 luc_suppgid1;
        __u32 luc_suppgid2;
        __u32 luc_umask;
};

struct lvfs_callback_ops {
        struct dentry *(*l_fid2dentry)(__u64 id_ino, __u32 gen, __u64 gr, void *data);
};

#define OBD_RUN_CTXT_MAGIC      0xC0FFEEAA
#define OBD_CTXT_DEBUG          /* development-only debugging */
struct lvfs_run_ctxt {
        struct vfsmount *pwdmnt;
        struct dentry   *pwd;
        mm_segment_t     fs;
        struct lvfs_ucred luc;
        int              ngroups;
        struct lvfs_callback_ops cb_ops;
#ifdef OBD_CTXT_DEBUG
        __u32            magic;
#endif
};

#ifdef OBD_CTXT_DEBUG
#define OBD_SET_CTXT_MAGIC(ctxt) (ctxt)->magic = OBD_RUN_CTXT_MAGIC
#else
#define OBD_SET_CTXT_MAGIC(ctxt) do {} while(0)
#endif

/* lvfs_common.c */
struct dentry *lvfs_fid2dentry(struct lvfs_run_ctxt *, __u64, __u32, __u64 ,void *data);

void push_ctxt(struct lvfs_run_ctxt *save, struct lvfs_run_ctxt *new_ctx,
               struct lvfs_ucred *cred);
void pop_ctxt(struct lvfs_run_ctxt *saved, struct lvfs_run_ctxt *new_ctx,
              struct lvfs_ucred *cred);

#ifdef __KERNEL__
int lvfs_reint(struct super_block *sb, void *r_rec);
int lvfs_undo(struct super_block *sb, void *r_rec);
struct dentry *simple_mkdir(struct dentry *dir, char *name, int mode, int fix);
struct dentry *simple_mknod(struct dentry *dir, char *name, int mode, int fix);
int lustre_fread(struct file *file, void *buf, int len, loff_t *off);
int lustre_fwrite(struct file *file, const void *buf, int len, loff_t *off);
int lustre_fsync(struct file *file);
long l_readdir(struct file * file, struct list_head *dentry_list);

static inline void l_dput(struct dentry *de)
{
        if (!de || IS_ERR(de))
                return;
        //shrink_dcache_parent(de);
        LASSERT(atomic_read(&de->d_count) > 0);
        dput(de);
}

/* We need to hold the inode semaphore over the dcache lookup itself, or we
 * run the risk of entering the filesystem lookup path concurrently on SMP
 * systems, and instantiating two inodes for the same entry.  We still
 * protect against concurrent addition/removal races with the DLM locking.
 */
static inline struct dentry *ll_lookup_one_len(const char *fid_name,
                                               struct dentry *dparent,
                                               int fid_namelen)
{
        struct dentry *dchild;

        down(&dparent->d_inode->i_sem);
        dchild = lookup_one_len(fid_name, dparent, fid_namelen);
        up(&dparent->d_inode->i_sem);

        return dchild;
}

static inline void ll_sleep(int t)
{
        set_current_state(TASK_INTERRUPTIBLE);
        schedule_timeout(t * HZ);
        set_current_state(TASK_RUNNING);
}
#endif

static inline int ll_fid2str(char *str, __u64 id, __u32 generation)
{
        return sprintf(str, "%llx:%08x", (unsigned long long)id, generation);
}

#endif
