/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) 2003 Cluster File Systems, Inc.
 *
 * This code is issued under the GNU General Public License.
 * See the file COPYING in this distribution
 */

#ifndef LLITE_INTERNAL_H
#define LLITE_INTERNAL_H

/* default to about 40meg of readahead on a given system.  That much tied
 * up in 512k readahead requests serviced at 40ms each is about 1GB/s. */
#define SBI_DEFAULT_RA_MAX ((40 << 20) >> PAGE_CACHE_SHIFT)

enum ra_stat {
        RA_STAT_HIT = 0,
        RA_STAT_MISS,
        RA_STAT_DISTANT_READPAGE,
        RA_STAT_MISS_IN_WINDOW,
        RA_STAT_FAILED_MATCH,
        RA_STAT_DISCARDED,
        RA_STAT_ZERO_LEN,
        RA_STAT_ZERO_WINDOW,
        RA_STAT_EOF,
        RA_STAT_MAX_IN_FLIGHT,
        _NR_RA_STAT,
};

struct ll_ra_info {
        unsigned long             ra_cur_pages;
        unsigned long             ra_max_pages;
        unsigned long             ra_stats[_NR_RA_STAT];
};

struct ll_sb_info {
        /* this protects pglist and max_r_a_pages.  It isn't safe to grab from
         * interrupt contexts. */
        spinlock_t                ll_lock;
        
        struct obd_uuid           ll_sb_uuid;
        struct obd_export        *ll_lmv_exp;
        struct lmv_desc           ll_lmv_desc;
        struct obd_export        *ll_lov_exp;
        struct lov_desc           ll_lov_desc;
        struct proc_dir_entry    *ll_proc_root;
        struct lustre_id          ll_rootid;     /* root lustre id */

        struct lustre_mount_data *ll_lmd;
        char                     *ll_instance;

        int                       ll_flags;
        struct list_head          ll_conn_chain; /* per-conn chain of SBs */

        struct hlist_head         ll_orphan_dentry_list; /*please don't ask -p*/
        struct ll_close_queue    *ll_lcq;

        struct lprocfs_stats     *ll_stats;      /* lprocfs stats counter */

        unsigned long             ll_pglist_gen;
        struct list_head          ll_pglist;

        struct ll_ra_info         ll_ra_info;
                                                                                                                                                                                                     
        /* times spent waiting for locks in each call site.  These are
         * all protected by the ll_lock */
        struct obd_service_time   ll_read_stime;
        struct obd_service_time   ll_write_stime;
        struct obd_service_time   ll_grouplock_stime;
        struct obd_service_time   ll_seek_stime;
        struct obd_service_time   ll_setattr_stime;
        struct obd_service_time   ll_brw_stime;
//      struct obd_service_time   ll_done_stime;

        int                       ll_config_version; /* last-applied update */

        /* list of GNS mounts; protected by the dcache_lock */
        struct list_head          ll_mnt_list;

        struct semaphore          ll_gns_sem;
        wait_queue_head_t         ll_gns_waitq;
        struct completion         ll_gns_completion;
        int                       ll_gns_state;
        struct timer_list         ll_gns_timer;
        struct list_head          ll_gns_sbi_head;
};

#define LL_GNS_STATE_IDLE     1100
#define LL_GNS_STATE_MOUNTING 1101
#define LL_GNS_STATE_FINISHED 1102

struct ll_readahead_state {
        spinlock_t      ras_lock;
        unsigned long   ras_last_readpage, ras_consecutive;
        unsigned long   ras_window_start, ras_window_len;
        unsigned long   ras_next_readahead;

};

extern kmem_cache_t *ll_file_data_slab;
struct lustre_handle;
struct ll_file_data {
        struct obd_client_handle fd_mds_och;
        struct ll_readahead_state fd_ras;
        __u32 fd_flags;
        struct lustre_handle fd_cwlockh;
        unsigned long fd_gid;
};

struct lov_stripe_md;

extern spinlock_t inode_lock;

extern void lprocfs_unregister_mountpoint(struct ll_sb_info *sbi);
extern struct proc_dir_entry *proc_lustre_fs_root;

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,5,0))
# define hlist_del_init list_del_init
#endif

static inline struct inode *ll_info2i(struct ll_inode_info *lli)
{
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,5,0))
        return &lli->lli_vfs_inode;
#else
        return list_entry(lli, struct inode, u.generic_ip);
#endif
}


struct it_cb_data {
        struct inode *icbd_parent;
        struct dentry **icbd_childp;
        obd_id hash;
};

#define LLAP_MAGIC 98764321

struct ll_async_page {
        int             llap_magic;
        void            *llap_cookie;
        struct page     *llap_page;
        struct list_head llap_pending_write;
         /* only trust these if the page lock is providing exclusion */
        unsigned         llap_write_queued:1,
                         llap_defer_uptodate:1,
                         llap_ra_used:1;

        struct list_head llap_proc_item;
};

#define LL_CDEBUG_PAGE(mask, page, fmt, arg...)                         \
        CDEBUG(mask, "page %p map %p ind %lu priv %0lx: " fmt,          \
               page, page->mapping, page->index, page->private, ## arg)

/* llite/lproc_llite.c */
int lprocfs_register_mountpoint(struct proc_dir_entry *parent,
                                struct super_block *sb, char *lov,
                                char *lmv);
void lprocfs_unregister_mountpoint(struct ll_sb_info *sbi);

/* llite/dir.c */
extern struct file_operations ll_dir_operations;
extern struct inode_operations ll_dir_inode_operations;

/* llite/namei.c */
int ll_objects_destroy(struct ptlrpc_request *request, 
                       struct inode *dir, int offset);
struct inode *ll_iget(struct super_block *sb, ino_t hash,
                      struct lustre_md *lic);
struct dentry *ll_find_alias(struct inode *, struct dentry *);
int ll_mdc_cancel_unused(struct lustre_handle *, struct inode *, int flags,
                         void *opaque);
int ll_mdc_blocking_ast(struct ldlm_lock *, struct ldlm_lock_desc *,
                        void *data, int flag);
/* llite/rw.c */
int ll_prepare_write(struct file *, struct page *, unsigned from, unsigned to);
int ll_commit_write(struct file *, struct page *, unsigned from, unsigned to);
int ll_writepage(struct page *page);
void ll_inode_fill_obdo(struct inode *inode, int cmd, struct obdo *oa);
void ll_ap_completion(void *data, int cmd, struct obdo *oa, int rc);
void ll_removepage(struct page *page);
int ll_readpage(struct file *file, struct page *page);
struct ll_async_page *llap_from_cookie(void *cookie);
struct ll_async_page *llap_from_page(struct page *page);
struct ll_async_page *llap_cast_private(struct page *page);
void ll_readahead_init(struct inode *inode, struct ll_readahead_state *ras);

void ll_ra_accounting(struct page *page, struct address_space *mapping);
void ll_truncate(struct inode *inode);

/* llite/file.c */
extern struct file_operations ll_file_operations;
extern struct inode_operations ll_file_inode_operations;
extern int ll_inode_revalidate_it(struct dentry *, struct lookup_intent *);
int ll_refresh_lsm(struct inode *inode, struct lov_stripe_md *lsm);
int ll_extent_lock(struct ll_file_data *, struct inode *,
                   struct lov_stripe_md *, int mode, ldlm_policy_data_t *,
                   struct lustre_handle *, int ast_flags,
                   struct obd_service_time *);
int ll_extent_unlock(struct ll_file_data *, struct inode *,
                     struct lov_stripe_md *, int mode, struct lustre_handle *);
int ll_file_open(struct inode *inode, struct file *file);
int ll_file_release(struct inode *inode, struct file *file);
int ll_lsm_getattr(struct obd_export *, struct lov_stripe_md *, struct obdo *);
int ll_glimpse_size(struct inode *inode);
int ll_local_open(struct file *file, struct lookup_intent *it);
int ll_mdc_close(struct obd_export *lmv_exp, struct inode *inode,
                 struct file *file);
#if (LINUX_VERSION_CODE > KERNEL_VERSION(2,5,0))
int ll_getattr(struct vfsmount *mnt, struct dentry *de,
               struct lookup_intent *it, struct kstat *stat);
#endif
void ll_stime_record(struct ll_sb_info *sbi, struct timeval *start,
                     struct obd_service_time *stime);

/* llite/dcache.c */
void ll_intent_drop_lock(struct lookup_intent *);
void ll_intent_release(struct lookup_intent *);
extern void ll_set_dd(struct dentry *de);
void ll_unhash_aliases(struct inode *);
void ll_frob_intent(struct lookup_intent **itp, struct lookup_intent *deft);
void ll_lookup_finish_locks(struct lookup_intent *it, struct dentry *dentry);
int revalidate_it_finish(struct ptlrpc_request *request, int offset,
                         struct lookup_intent *it, struct dentry *de);


/* llite/llite_gns.c */
int ll_finish_gns(struct ll_sb_info *sbi);
int fill_page_with_path(struct dentry *, struct vfsmount *, char **pagep);
int ll_dir_process_mount_object(struct dentry *, struct vfsmount *);
int ll_gns_umount_all(struct ll_sb_info *sbi, int timeout);
void ll_gns_timer_callback(unsigned long data);
void ll_gns_add_timer(struct ll_sb_info *sbi);
void ll_gns_del_timer(struct ll_sb_info *sbi);
int ll_gns_start_thread(void);
void ll_gns_stop_thread(void);

/* llite/llite_lib.c */
extern struct super_operations lustre_super_operations;

char *ll_read_opt(const char *opt, char *data);
int ll_set_opt(const char *opt, char *data, int fl);
void ll_options(char *options, char **ost, char **mds, int *flags);
void ll_lli_init(struct ll_inode_info *lli);
int ll_fill_super(struct super_block *sb, void *data, int silent);
int lustre_fill_super(struct super_block *sb, void *data, int silent);
void lustre_put_super(struct super_block *sb);
struct inode *ll_inode_from_lock(struct ldlm_lock *lock);
void ll_clear_inode(struct inode *inode);
int ll_attr2inode(struct inode *inode, struct iattr *attr, int trunc);
int ll_setattr_raw(struct inode *inode, struct iattr *attr);
int ll_setattr(struct dentry *de, struct iattr *attr);
int ll_statfs(struct super_block *sb, struct kstatfs *sfs);
int ll_statfs_internal(struct super_block *sb, struct obd_statfs *osfs,
                       unsigned long maxage);
void ll_update_inode(struct inode *inode, struct lustre_md *);
int it_disposition(struct lookup_intent *it, int flag);
void it_set_disposition(struct lookup_intent *it, int flag);
void ll_read_inode2(struct inode *inode, void *opaque);
void ll_delete_inode(struct inode *inode);
int ll_iocontrol(struct inode *inode, struct file *file,
                 unsigned int cmd, unsigned long arg);
void ll_umount_begin(struct super_block *sb);
int ll_prep_inode(struct obd_export *, struct obd_export *, struct inode **inode,
                  struct ptlrpc_request *req, int offset, struct super_block *);
__u32 get_uuid2int(const char *name, int len);
struct dentry *ll_fh_to_dentry(struct super_block *sb, __u32 *data, int len,
                               int fhtype, int parent);
int ll_dentry_to_fh(struct dentry *, __u32 *datap, int *lenp, int need_parent);
int null_if_equal(struct ldlm_lock *lock, void *data);
int ll_process_config_update(struct ll_sb_info *sbi, int clean);

/* llite/special.c */
extern struct inode_operations ll_special_inode_operations;
extern struct file_operations ll_special_chr_inode_fops;
extern struct file_operations ll_special_chr_file_fops;
extern struct file_operations ll_special_blk_inode_fops;
extern struct file_operations ll_special_fifo_inode_fops;
extern struct file_operations ll_special_fifo_file_fops;
extern struct file_operations ll_special_sock_inode_fops;

/* llite/symlink.c */
extern struct inode_operations ll_fast_symlink_inode_operations;

/* llite/llite_close.c */
struct ll_close_queue {
        spinlock_t              lcq_lock;
        struct list_head        lcq_list;
        wait_queue_head_t       lcq_waitq;
        struct completion       lcq_comp;
};

void llap_write_pending(struct inode *inode, struct ll_async_page *llap);
void llap_write_complete(struct inode *inode, struct ll_async_page *llap);
void ll_open_complete(struct inode *inode);
int ll_is_inode_dirty(struct inode *inode);
void ll_try_done_writing(struct inode *inode);
void ll_queue_done_writing(struct inode *inode);
void ll_close_thread_shutdown(struct ll_close_queue *lcq);
int ll_close_thread_start(struct ll_close_queue **lcq_ret);


/* llite/llite_mmap.c */
#if  (LINUX_VERSION_CODE >= KERNEL_VERSION(2,5,0))
typedef struct rb_root  rb_root_t;
typedef struct rb_node  rb_node_t;
#endif

struct ll_lock_tree_node;
struct ll_lock_tree {
        rb_root_t                       lt_root;
        struct list_head                lt_locked_list;
        struct ll_file_data             *lt_fd;
};
int ll_teardown_mmaps(struct address_space *mapping, __u64 first,
                      __u64 last);
int ll_file_mmap(struct file * file, struct vm_area_struct * vma);
struct ll_lock_tree_node * ll_node_from_inode(struct inode *inode, __u64 start,
                                              __u64 end, ldlm_mode_t mode);
int ll_tree_lock(struct ll_lock_tree *tree,
                 struct ll_lock_tree_node *first_node, struct inode *inode,
                 const char *buf, size_t count, int ast_flags);
int ll_tree_unlock(struct ll_lock_tree *tree, struct inode *inode);

int ll_get_fid(struct obd_export *exp, struct lustre_id *idp,
               char *filename, struct lustre_id *ret);

/* generic */
#define LL_SBI_NOLCK           0x1
#define LL_SBI_READAHEAD       0x2
#define LL_SUPER_MAGIC         0x0BD00BD0
#define LL_MAX_BLKSIZE         (4UL * 1024 * 1024)

#if  (LINUX_VERSION_CODE >= KERNEL_VERSION(2,5,0))
#define    ll_s2sbi(sb)        ((struct ll_sb_info *)((sb)->s_fs_info))
#define    ll_set_sbi(sb, sbi) ((sb)->s_fs_info = sbi)
void __d_rehash(struct dentry * entry, int lock);
static inline __u64 ll_ts2u64(struct timespec *time)
{
        __u64 t = time->tv_sec;
        return t;
}
#else  /* 2.4 here */
#define    ll_s2sbi(sb)        ((struct ll_sb_info *)((sb)->u.generic_sbp))
#define    ll_set_sbi(sb, sbi) ((sb)->u.generic_sbp = sbi)
static inline __u64 ll_ts2u64(time_t *time)
{
        return *time;
}
#endif

/* don't need an addref as the sb_info should be holding one */
static inline struct obd_export *ll_s2obdexp(struct super_block *sb)
{
        return ll_s2sbi(sb)->ll_lov_exp;
}

/* don't need an addref as the sb_info should be holding one */
static inline struct obd_export *ll_s2lmvexp(struct super_block *sb)
{
        return ll_s2sbi(sb)->ll_lmv_exp;
}

static inline struct client_obd *sbi2lmv(struct ll_sb_info *sbi)
{
        struct obd_device *obd = sbi->ll_lmv_exp->exp_obd;
        if (obd == NULL)
                LBUG();
        return &obd->u.cli;
}

// FIXME: replace the name of this with LL_SB to conform to kernel stuff
static inline struct ll_sb_info *ll_i2sbi(struct inode *inode)
{
        return ll_s2sbi(inode->i_sb);
}

static inline struct obd_export *ll_i2obdexp(struct inode *inode)
{
        return ll_s2obdexp(inode->i_sb);
}

static inline struct obd_export *ll_i2lmvexp(struct inode *inode)
{
        return ll_s2lmvexp(inode->i_sb);
}

static inline int ll_mds_max_easize(struct super_block *sb)
{
        return sbi2lmv(ll_s2sbi(sb))->cl_max_mds_easize;
}

static inline __u64 ll_file_maxbytes(struct inode *inode)
{
        return ll_i2info(inode)->lli_maxbytes;
}

#endif /* LLITE_INTERNAL_H */
