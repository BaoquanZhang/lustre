/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 */

#ifndef __OBD_H
#define __OBD_H

#define IOC_OSC_TYPE         'h'
#define IOC_OSC_MIN_NR       20
#define IOC_OSC_SET_ACTIVE   _IOWR(IOC_OSC_TYPE, 21, struct obd_device *)
#define IOC_OSC_MAX_NR       50

#define IOC_MDC_TYPE         'i'
#define IOC_MDC_MIN_NR       20
#define IOC_MDC_LOOKUP       _IOWR(IOC_MDC_TYPE, 20, struct obd_device *)
/* Moved to lustre_user.h
#define IOC_MDC_GETSTRIPE    _IOWR(IOC_MDC_TYPE, 21, struct lov_mds_md *) */
#define IOC_MDC_MAX_NR       50

#ifdef __KERNEL__
# include <linux/fs.h>
# include <linux/list.h>
# include <linux/sched.h> /* for struct task_struct, for current.h */
# include <asm/current.h> /* for smp_lock.h */
# include <linux/smp_lock.h>
# include <linux/proc_fs.h>
# include <linux/mount.h>
#endif

#include <linux/lustre_lib.h>
#include <linux/lustre_idl.h>
#include <linux/lustre_export.h>
#include <linux/lustre_quota.h>

/* this is really local to the OSC */
struct loi_oap_pages {
        struct list_head        lop_pending;
        int                     lop_num_pending;
        struct list_head        lop_urgent;
        struct list_head        lop_pending_group;
};

struct osc_async_rc {
        int     ar_rc;
        int     ar_force_sync;
        int     ar_min_xid;
};

struct lov_oinfo {                 /* per-stripe data structure */
        __u64 loi_id;              /* object ID on the target OST */
        __u64 loi_gr;              /* object group on the target OST */
        int loi_ost_idx;           /* OST stripe index in lov_tgt_desc->tgts */
        int loi_ost_gen;           /* generation of this loi_ost_idx */

        /* used by the osc to keep track of what objects to build into rpcs */
        struct loi_oap_pages loi_read_lop;
        struct loi_oap_pages loi_write_lop;
        /* _cli_ is poorly named, it should be _ready_ */
        struct list_head loi_cli_item;
        struct list_head loi_write_item;
        struct list_head loi_read_item;

        unsigned loi_kms_valid:1;
        __u64 loi_kms;             /* known minimum size */
        __u64 loi_rss;             /* recently seen size */
        __u64 loi_mtime;           /* recently seen mtime */
        __u64 loi_blocks;          /* recently seen blocks */

        struct osc_async_rc     loi_ar;
};

static inline void loi_init(struct lov_oinfo *loi)
{
        INIT_LIST_HEAD(&loi->loi_read_lop.lop_pending);
        INIT_LIST_HEAD(&loi->loi_read_lop.lop_urgent);
        INIT_LIST_HEAD(&loi->loi_read_lop.lop_pending_group);
        INIT_LIST_HEAD(&loi->loi_write_lop.lop_pending);
        INIT_LIST_HEAD(&loi->loi_write_lop.lop_urgent);
        INIT_LIST_HEAD(&loi->loi_write_lop.lop_pending_group);
        INIT_LIST_HEAD(&loi->loi_cli_item);
        INIT_LIST_HEAD(&loi->loi_write_item);
        INIT_LIST_HEAD(&loi->loi_read_item);
}

struct lov_stripe_md {
        spinlock_t       lsm_lock;
        void            *lsm_lock_owner; /* debugging */

        /* Public members. */
        __u64 lsm_object_id;        /* lov object id */
        __u64 lsm_object_gr;        /* lov object id */
        __u64 lsm_maxbytes;         /* maximum possible file size */
        unsigned long lsm_xfersize; /* optimal transfer size */

        /* LOV-private members start here -- only for use in lov/. */
        __u32 lsm_magic;
        __u32 lsm_stripe_size;      /* size of the stripe */
        __u32 lsm_pattern;          /* striping pattern (RAID0, RAID1) */
        unsigned lsm_stripe_count;  /* number of objects being striped over */
        struct lov_oinfo lsm_oinfo[0];
};

/* compare all fields except for semaphore */
static inline int lov_stripe_md_cmp(struct lov_stripe_md *m1,
                                    struct lov_stripe_md *m2)
{
        return memcmp(&m1->lsm_object_id, &m2->lsm_object_id,
                      (char *)&m2->lsm_oinfo[0] - (char *)&m2->lsm_object_id);
}

void lov_stripe_lock(struct lov_stripe_md *md);
void lov_stripe_unlock(struct lov_stripe_md *md);

struct obd_type {
        struct list_head typ_chain;
        struct obd_ops *typ_ops;
        struct proc_dir_entry *typ_procroot;
        char *typ_name;
        int  typ_refcnt;
};

struct brw_page {
        obd_off  off;
        struct page *pg;
        int count;
        obd_flag flag;
};

enum async_flags {
        ASYNC_READY = 0x1, /* ap_make_ready will not be called before this
                              page is added to an rpc */
        ASYNC_URGENT = 0x2,
        ASYNC_COUNT_STABLE = 0x4, /* ap_refresh_count will not be called
                                     to give the caller a chance to update
                                     or cancel the size of the io */
        ASYNC_GROUP_SYNC = 0x8,  /* ap_completion will not be called, instead
                                    the page is accounted for in the
                                    obd_io_group given to 
                                    obd_queue_group_io */
};

struct obd_async_page_ops {
        int  (*ap_make_ready)(void *data, int cmd);
        int  (*ap_refresh_count)(void *data, int cmd);
        void (*ap_fill_obdo)(void *data, int cmd, struct obdo *oa);
        void (*ap_completion)(void *data, int cmd, struct obdo *oa, int rc);
};

/* the `oig' is passed down from a caller of obd rw methods.  the callee
 * records enough state such that the caller can sleep on the oig and
 * be woken when all the callees have finished their work */
struct obd_io_group {
        spinlock_t      oig_lock;
        atomic_t        oig_refcount;
        int             oig_pending;
        int             oig_rc;
        struct list_head oig_occ_list;
        wait_queue_head_t oig_waitq;
};

/* the oig callback context lets the callee of obd rw methods register
 * for callbacks from the caller. */
struct oig_callback_context {
        struct list_head occ_oig_item;
        /* called when the caller has received a signal while sleeping.
         * callees of this method are encouraged to abort their state 
         * in the oig.  This may be called multiple times. */
        void (*occ_interrupted)(struct oig_callback_context *occ);
        unsigned int interrupted:1;
};

/* if we find more consumers this could be generalized */
#define OBD_HIST_MAX 32
struct obd_histogram {
        spinlock_t      oh_lock;
        unsigned long   oh_buckets[OBD_HIST_MAX];
};

/* Individual type definitions */

struct ost_server_data;

struct filter_obd {
        const char          *fo_fstype;
        struct super_block  *fo_sb;
        struct vfsmount     *fo_vfsmnt;
        struct dentry       *fo_dentry_O;
        struct dentry      **fo_dentry_O_groups;
        struct dentry      **fo_dentry_O_sub;
        spinlock_t           fo_objidlock; /* protect fo_lastobjid increment */
        spinlock_t           fo_translock; /* protect fsd_last_rcvd increment */
        struct file         *fo_rcvd_filp;
        struct filter_server_data *fo_fsd;
        unsigned long       *fo_last_rcvd_slots;
        __u64                fo_mount_count;

        unsigned int         fo_destroy_in_progress:1;
        struct semaphore     fo_create_lock;

        struct file_operations *fo_fop;
        struct inode_operations *fo_iop;
        struct address_space_operations *fo_aops;

        struct list_head     fo_export_list;
        int                  fo_subdir_count;

        obd_size             fo_tot_dirty;      /* protected by obd_osfs_lock */
        obd_size             fo_tot_granted;    /* all values in bytes */
        obd_size             fo_tot_pending;

        obd_size             fo_readcache_max_filesize;

        struct obd_import   *fo_mdc_imp;
        struct obd_uuid      fo_mdc_uuid;
        struct lustre_handle fo_mdc_conn;
#if 0
        struct ptlrpc_client fo_mdc_client;
#endif
        struct file        **fo_last_objid_files;
        __u64               *fo_last_objids; /* last created objid for groups */

        struct semaphore     fo_alloc_lock;

        spinlock_t fo_stats_lock;
        int fo_r_in_flight; /* protected by fo_stats_lock */
        int fo_w_in_flight; /* protected by fo_stats_lock */

        /*
         * per-filter pool of kiobuf's allocated by filter_common_setup() and
         * torn down by filter_cleanup(). Contains OST_NUM_THREADS elements of
         * which ->fo_iobuf_count were allocated.
         *
         * This pool contains kiobuf used by
         * filter_{prep,commit}rw_{read,write}() and is shared by all OST
         * threads.
         *
         * Locking: none, each OST thread uses only one element, determined by
         * its "ordinal number", ->t_id.
         *
         * This is (void *) array, because 2.4 and 2.6 use different iobuf
         * structures.
         */
        void                   **fo_iobuf_pool;
        int                      fo_iobuf_count;

        struct obd_histogram     fo_r_pages;
        struct obd_histogram     fo_w_pages;
        struct obd_histogram     fo_read_rpc_hist;
        struct obd_histogram     fo_write_rpc_hist;
        struct obd_histogram     fo_r_io_time;
        struct obd_histogram     fo_w_io_time;
        struct obd_histogram     fo_r_discont_pages;
        struct obd_histogram     fo_w_discont_pages;
        struct obd_histogram     fo_r_discont_blocks;
        struct obd_histogram     fo_w_discont_blocks;
        struct obd_histogram     fo_r_disk_iosize;
        struct obd_histogram     fo_w_disk_iosize;

        struct lustre_quota_ctxt fo_quota_ctxt;
        spinlock_t               fo_quotacheck_lock;
        atomic_t                 fo_quotachecking;
};

struct mds_server_data;

#define OSC_MAX_RIF_DEFAULT       8
#define OSC_MAX_RIF_MAX          64
#define OSC_MAX_DIRTY_DEFAULT    32
#define OSC_MAX_DIRTY_MB_MAX    512     /* totally arbitrary */

enum {
        CL_QUOTACHECKING = 1,
        CL_NO_QUOTACHECK
};

struct mdc_rpc_lock;
struct client_obd {
        struct obd_import       *cl_import;
        struct semaphore         cl_sem;
        int                      cl_conn_count;
        /* max_mds_easize is purely a performance thing so we don't have to
         * call obd_size_diskmd() all the time. */
        int                      cl_default_mds_easize;
        int                      cl_max_mds_easize;
        int                      cl_max_mds_cookiesize;
        kdev_t                   cl_sandev;

        //struct llog_canceld_ctxt *cl_llcd; /* it's included by obd_llog_ctxt */
        void                    *cl_llcd_offset;

        /* the grant values are protected by loi_list_lock below */
        long                     cl_dirty;         /* all _dirty_ in bytes */
        long                     cl_dirty_max;     /* allowed w/o rpc */
        long                     cl_avail_grant;   /* bytes of credit for ost */
        long                     cl_lost_grant;    /* lost credits (trunc) */
        struct list_head         cl_cache_waiters; /* waiting for cache/grant */

        /* keep track of objects that have lois that contain pages which
         * have been queued for async brw.  this lock also protects the
         * lists of osc_client_pages that hang off of the loi */
        spinlock_t               cl_loi_list_lock;
        struct list_head         cl_loi_ready_list;
        struct list_head         cl_loi_write_list;
        struct list_head         cl_loi_read_list;
        int                      cl_r_in_flight;
        int                      cl_w_in_flight;
        /* just a sum of the loi/lop pending numbers to be exported by /proc */
        int                      cl_pending_w_pages;
        int                      cl_pending_r_pages;
        int                      cl_max_pages_per_rpc;
        int                      cl_max_rpcs_in_flight;
        struct obd_histogram     cl_read_rpc_hist;
        struct obd_histogram     cl_write_rpc_hist;
        struct obd_histogram     cl_read_page_hist;
        struct obd_histogram     cl_write_page_hist;
        struct obd_histogram     cl_read_offset_hist;
        struct obd_histogram     cl_write_offset_hist;

        struct mdc_rpc_lock     *cl_rpc_lock;
        struct mdc_rpc_lock     *cl_setattr_lock;
        struct osc_creator       cl_oscc;

        /* Flags section */
        unsigned int             cl_checksum:1; /* debug checksums */

        /* also protected by the poorly named _loi_list_lock lock above */
        struct osc_async_rc      cl_ar;

        /* used by quotacheck */
        spinlock_t               cl_qchk_lock;
        int                      cl_qchk_stat; /* quotacheck stat of the peer */
        struct ptlrpc_request_pool *cl_rq_pool; /* emergency pool of requests */
};

struct mds_obd {
        struct ptlrpc_service           *mds_service;
        struct ptlrpc_service           *mds_setattr_service;
        struct ptlrpc_service           *mds_readpage_service;
        struct super_block              *mds_sb;
        struct vfsmount                 *mds_vfsmnt;
        struct dentry                   *mds_fid_de;
        int                              mds_max_mdsize;
        int                              mds_max_cookiesize;
        struct file                     *mds_rcvd_filp;
        spinlock_t                       mds_transno_lock;
        __u64                            mds_last_transno;
        __u64                            mds_mount_count;
        __u64                            mds_io_epoch;
        struct semaphore                 mds_epoch_sem;
        struct ll_fid                    mds_rootfid;
        struct mds_server_data          *mds_server_data;
        struct dentry                   *mds_pending_dir;
        struct dentry                   *mds_logs_dir;
        struct dentry                   *mds_objects_dir;
        struct llog_handle              *mds_cfg_llh;
//        struct llog_handle              *mds_catalog;
        struct obd_device               *mds_osc_obd; /* XXX lov_obd */
        struct obd_uuid                  mds_lov_uuid;
        char                            *mds_profile;
        struct obd_export               *mds_osc_exp; /* XXX lov_exp */
        int                              mds_has_lov_desc;
        struct lov_desc                  mds_lov_desc;
        obd_id                          *mds_lov_objids;
        int                              mds_lov_nextid_set;
        struct file                     *mds_lov_objid_filp;
        unsigned long                   *mds_client_bitmap;
        struct semaphore                 mds_orphan_recovery_sem;
        struct upcall_cache             *mds_group_hash;
        struct lustre_quota_info         mds_quota_info;
        struct lustre_quota_ctxt         mds_quota_ctxt;
        atomic_t                         mds_quotachecking;
        struct semaphore                 mds_health_sem;
        unsigned long                    mds_lov_objids_valid:1,
                                         mds_fl_user_xattr:1,
                                         mds_fl_acl:1;
};

struct echo_obd {
        struct obdo          eo_oa;
        spinlock_t           eo_lock;
        __u64                eo_lastino;
        struct lustre_handle eo_nl_lock;
        atomic_t             eo_prep;
};

/*
 * this struct does double-duty acting as either a client or
 * server instance .. maybe not wise.
 */
struct ptlbd_obd {
        /* server's */
        struct ptlrpc_service *ptlbd_service;
        struct file *filp;
        /* client's */
        struct ptlrpc_client    bd_client;
        struct obd_import       *bd_import;
        struct obd_uuid         bd_server_uuid;
        struct obd_export       *bd_exp;
        int refcount; /* XXX sigh */
};

struct recovd_obd {
        spinlock_t            recovd_lock;
        struct list_head      recovd_managed_items; /* items managed  */
        struct list_head      recovd_troubled_items; /* items in recovery */

        wait_queue_head_t     recovd_recovery_waitq;
        wait_queue_head_t     recovd_ctl_waitq;
        wait_queue_head_t     recovd_waitq;
        struct task_struct   *recovd_thread;
        __u32                 recovd_state;
};

struct ost_obd {
        struct ptlrpc_service *ost_service;
        struct ptlrpc_service *ost_create_service;
        struct semaphore       ost_health_sem;
};

struct echo_client_obd {
        struct obd_export   *ec_exp;   /* the local connection to osc/lov */
        spinlock_t           ec_lock;
        struct list_head     ec_objects;
        int                  ec_nstripes;
        __u64                ec_unique;
};

struct cache_obd {
        struct obd_export *cobd_target_exp;/* local connection to target obd */
        struct obd_export *cobd_cache_exp; /* local connection to cache obd */
};

struct lov_tgt_desc {
        struct obd_uuid          uuid;
        __u32                    ltd_gen;
        struct obd_export       *ltd_exp;
        int                      active; /* is this target up for requests */
};

struct lov_obd {
        spinlock_t lov_lock;
        struct lov_desc desc;
        int bufsize;
        int refcount;
        unsigned int lo_catalog_loaded:1;
        struct lov_tgt_desc *tgts;
};

struct niobuf_local {
        __u64 offset;
        __u32 len;
        __u32 flags;
        struct page *page;
        struct dentry *dentry;
        int lnb_grant_used;
        int rc;
};


/* Don't conflict with on-wire flags OBD_BRW_WRITE, etc */
#define N_LOCAL_TEMP_PAGE 0x10000000

struct obd_trans_info {
        __u64                    oti_transno;
        __u64                   *oti_objid;
        /* Only used on the server side for tracking acks. */
        struct oti_req_ack_lock {
                struct lustre_handle lock;
                __u32                mode;
        }                        oti_ack_locks[4];
        void                    *oti_handle;
        struct llog_cookie       oti_onecookie;
        struct llog_cookie      *oti_logcookies;
        int                      oti_numcookies;

        /* initial thread handling transaction */
        struct ptlrpc_thread    *oti_thread; 
};

static inline void oti_alloc_cookies(struct obd_trans_info *oti,int num_cookies)
{
        if (!oti)
                return;

        if (num_cookies == 1)
                oti->oti_logcookies = &oti->oti_onecookie;
        else
                OBD_ALLOC(oti->oti_logcookies,
                          num_cookies * sizeof(oti->oti_onecookie));

        oti->oti_numcookies = num_cookies;
}

static inline void oti_free_cookies(struct obd_trans_info *oti)
{
        if (!oti || !oti->oti_logcookies)
                return;

        if (oti->oti_logcookies == &oti->oti_onecookie)
                LASSERT(oti->oti_numcookies == 1);
        else
                OBD_FREE(oti->oti_logcookies,
                         oti->oti_numcookies * sizeof(oti->oti_onecookie));
        oti->oti_logcookies = NULL;
        oti->oti_numcookies = 0;
}

/* llog contexts */
enum llog_ctxt_id {
        LLOG_CONFIG_ORIG_CTXT  =  0,
        LLOG_CONFIG_REPL_CTXT  =  1,
        LLOG_MDS_OST_ORIG_CTXT =  2,
        LLOG_MDS_OST_REPL_CTXT =  3,
        LLOG_SIZE_ORIG_CTXT    =  4,
        LLOG_SIZE_REPL_CTXT    =  5,
        LLOG_MD_ORIG_CTXT      =  6,
        LLOG_MD_REPL_CTXT      =  7,
        LLOG_RD1_ORIG_CTXT     =  8,
        LLOG_RD1_REPL_CTXT     =  9,
        LLOG_TEST_ORIG_CTXT    = 10,
        LLOG_TEST_REPL_CTXT    = 11,
        LLOG_MAX_CTXTS
};


/* corresponds to one of the obd's */
struct obd_device {
        struct obd_type *obd_type;

        /* common and UUID name of this device */
        char *obd_name;
        struct obd_uuid obd_uuid;

        int obd_minor;
        unsigned int obd_attached:1, obd_set_up:1, obd_recovering:1,
                obd_abort_recovery:1, obd_replayable:1, obd_no_transno:1,
                obd_no_recov:1, obd_stopping:1, obd_starting:1,
                obd_force:1, obd_fail:1, obd_async_recov:1;
        atomic_t obd_refcount;
        wait_queue_head_t obd_refcount_waitq;
        struct proc_dir_entry  *obd_proc_entry;
        struct list_head        obd_exports;
        int                     obd_num_exports;
        struct ldlm_namespace  *obd_namespace;
        struct ptlrpc_client    obd_ldlm_client; /* XXX OST/MDS only */
        /* a spinlock is OK for what we do now, may need a semaphore later */
        spinlock_t              obd_dev_lock;
        __u64                   obd_last_committed;
        struct fsfilt_operations *obd_fsops;
        spinlock_t              obd_osfs_lock;
        struct obd_statfs       obd_osfs;
        unsigned long           obd_osfs_age;   /* jiffies */
        struct lvfs_run_ctxt    obd_lvfs_ctxt;
        struct llog_ctxt        *obd_llog_ctxt[LLOG_MAX_CTXTS];
        struct obd_device       *obd_observer;
        struct obd_export       *obd_self_export;
        /* list of exports in LRU order, for ping evictor, with obd_dev_lock */
        struct list_head        obd_exports_timed;
        time_t                  obd_eviction_timer; /* for ping evictor */

        /* XXX encapsulate all this recovery data into one struct */
        svc_handler_t                    obd_recovery_handler;
        int                              obd_max_recoverable_clients;
        int                              obd_connected_clients;
        int                              obd_recoverable_clients;
        spinlock_t                       obd_processing_task_lock;
        pid_t                            obd_processing_task;
        __u64                            obd_next_recovery_transno;
        int                              obd_replayed_requests;
        int                              obd_requests_queued_for_recovery;
        wait_queue_head_t                obd_next_transno_waitq;
        struct list_head                 obd_uncommitted_replies;
        spinlock_t                       obd_uncommitted_replies_lock;
        struct timer_list                obd_recovery_timer;
        struct list_head                 obd_recovery_queue;
        struct list_head                 obd_delayed_reply_queue;
        time_t                           obd_recovery_start;
        time_t                           obd_recovery_end;

        union {
                struct filter_obd filter;
                struct mds_obd mds;
                struct client_obd cli;
                struct ost_obd ost;
                struct echo_client_obd echo_client;
                struct echo_obd echo;
                struct recovd_obd recovd;
                struct lov_obd lov;
                struct cache_obd cobd;
                struct ptlbd_obd ptlbd;
        } u;
       /* Fields used by LProcFS */
        unsigned int           obd_cntr_base;
        struct lprocfs_stats  *obd_stats;
        struct proc_dir_entry *obd_svc_procroot;
        struct lprocfs_stats  *obd_svc_stats;
};

#define OBD_OPT_FORCE           0x0001
#define OBD_OPT_FAILOVER        0x0002

#define OBD_LLOG_FL_SENDNOW     0x0001

struct obd_ops {
        struct module *o_owner;
        int (*o_iocontrol)(unsigned int cmd, struct obd_export *exp, int len,
                           void *karg, void *uarg);
        int (*o_get_info)(struct obd_export *, __u32 keylen, void *key,
                          __u32 *vallen, void *val);
        int (*o_set_info)(struct obd_export *, __u32 keylen, void *key,
                          __u32 vallen, void *val);
        int (*o_attach)(struct obd_device *dev, obd_count len, void *data);
        int (*o_detach)(struct obd_device *dev);
        int (*o_setup) (struct obd_device *dev, obd_count len, void *data);
        int (*o_precleanup)(struct obd_device *dev, int cleanup_stage);
        int (*o_cleanup)(struct obd_device *dev);
        int (*o_process_config)(struct obd_device *dev, obd_count len,
                                void *data);
        int (*o_postrecov)(struct obd_device *dev);
        int (*o_add_conn)(struct obd_import *imp, struct obd_uuid *uuid,
                          int priority);
        int (*o_del_conn)(struct obd_import *imp, struct obd_uuid *uuid);
        /* connect to the target device with given connection
         * data. @ocd->ocd_connect_flags is modified to reflect flags actually
         * granted by the target, which are guaranteed to be a subset of flags
         * asked for. If @ocd == NULL, use default parameters. */
        int (*o_connect)(struct lustre_handle *conn, struct obd_device *src,
                         struct obd_uuid *cluuid, struct obd_connect_data *ocd);
        int (*o_disconnect)(struct obd_export *exp);

        int (*o_statfs)(struct obd_device *obd, struct obd_statfs *osfs,
                        unsigned long max_age);
        int (*o_packmd)(struct obd_export *exp, struct lov_mds_md **disk_tgt,
                        struct lov_stripe_md *mem_src);
        int (*o_unpackmd)(struct obd_export *exp,struct lov_stripe_md **mem_tgt,
                          struct lov_mds_md *disk_src, int disk_len);
        int (*o_preallocate)(struct lustre_handle *, obd_count *req,
                             obd_id *ids);
        int (*o_create)(struct obd_export *exp,  struct obdo *oa,
                        struct lov_stripe_md **ea, struct obd_trans_info *oti);
        int (*o_destroy)(struct obd_export *exp, struct obdo *oa,
                         struct lov_stripe_md *ea, struct obd_trans_info *oti);
        int (*o_setattr)(struct obd_export *exp, struct obdo *oa,
                         struct lov_stripe_md *ea, struct obd_trans_info *oti);
        int (*o_setattr_async)(struct obd_export *exp, struct obdo *oa,
                         struct lov_stripe_md *ea, struct obd_trans_info *oti);
        int (*o_getattr)(struct obd_export *exp, struct obdo *oa,
                         struct lov_stripe_md *ea);
        int (*o_getattr_async)(struct obd_export *exp, struct obdo *oa,
                               struct lov_stripe_md *ea,
                               struct ptlrpc_request_set *set);
        int (*o_brw)(int rw, struct obd_export *exp, struct obdo *oa,
                     struct lov_stripe_md *ea, obd_count oa_bufs,
                     struct brw_page *pgarr, struct obd_trans_info *oti);
        int (*o_brw_async)(int rw, struct obd_export *exp, struct obdo *oa,
                           struct lov_stripe_md *ea, obd_count oa_bufs,
                           struct brw_page *pgarr, struct ptlrpc_request_set *,
                           struct obd_trans_info *oti);
        int (*o_prep_async_page)(struct obd_export *exp, 
                                 struct lov_stripe_md *lsm,
                                 struct lov_oinfo *loi, 
                                 struct page *page, obd_off offset, 
                                 struct obd_async_page_ops *ops, void *data,
                                 void **res);
        int (*o_queue_async_io)(struct obd_export *exp, 
                                struct lov_stripe_md *lsm, 
                                struct lov_oinfo *loi, void *cookie, 
                                int cmd, obd_off off, int count, 
                                obd_flag brw_flags, obd_flag async_flags);
        int (*o_queue_group_io)(struct obd_export *exp, 
                                struct lov_stripe_md *lsm, 
                                struct lov_oinfo *loi, 
                                struct obd_io_group *oig, 
                                void *cookie, int cmd, obd_off off, int count, 
                                obd_flag brw_flags, obd_flag async_flags);
        int (*o_trigger_group_io)(struct obd_export *exp, 
                                  struct lov_stripe_md *lsm, 
                                  struct lov_oinfo *loi, 
                                  struct obd_io_group *oig);
        int (*o_set_async_flags)(struct obd_export *exp,
                                struct lov_stripe_md *lsm,
                                struct lov_oinfo *loi, void *cookie,
                                obd_flag async_flags);
        int (*o_teardown_async_page)(struct obd_export *exp,
                                     struct lov_stripe_md *lsm,
                                     struct lov_oinfo *loi, void *cookie);
        int (*o_adjust_kms)(struct obd_export *exp, struct lov_stripe_md *lsm,
                            obd_off size, int shrink);
        int (*o_punch)(struct obd_export *exp, struct obdo *oa,
                       struct lov_stripe_md *ea, obd_size start,
                       obd_size end, struct obd_trans_info *oti);
        int (*o_sync)(struct obd_export *exp, struct obdo *oa,
                      struct lov_stripe_md *ea, obd_size start, obd_size end);
        int (*o_migrate)(struct lustre_handle *conn, struct lov_stripe_md *dst,
                         struct lov_stripe_md *src, obd_size start,
                         obd_size end, struct obd_trans_info *oti);
        int (*o_copy)(struct lustre_handle *dstconn, struct lov_stripe_md *dst,
                      struct lustre_handle *srconn, struct lov_stripe_md *src,
                      obd_size start, obd_size end, struct obd_trans_info *);
        int (*o_iterate)(struct lustre_handle *conn,
                         int (*)(obd_id, obd_gr, void *),
                         obd_id *startid, obd_gr group, void *data);
        int (*o_preprw)(int cmd, struct obd_export *exp, struct obdo *oa,
                        int objcount, struct obd_ioobj *obj,
                        int niocount, struct niobuf_remote *remote,
                        struct niobuf_local *local, struct obd_trans_info *oti);
        int (*o_commitrw)(int cmd, struct obd_export *exp, struct obdo *oa,
                          int objcount, struct obd_ioobj *obj,
                          int niocount, struct niobuf_local *local,
                          struct obd_trans_info *oti, int rc);
        int (*o_enqueue)(struct obd_export *, struct lov_stripe_md *,
                         __u32 type, ldlm_policy_data_t *, __u32 mode,
                         int *flags, void *bl_cb, void *cp_cb, void *gl_cb,
                         void *data, __u32 lvb_len, void *lvb_swabber,
                         struct lustre_handle *lockh);
        int (*o_match)(struct obd_export *, struct lov_stripe_md *, __u32 type,
                       ldlm_policy_data_t *, __u32 mode, int *flags, void *data,
                       struct lustre_handle *lockh);
        int (*o_change_cbdata)(struct obd_export *, struct lov_stripe_md *,
                               ldlm_iterator_t it, void *data);
        int (*o_cancel)(struct obd_export *, struct lov_stripe_md *md,
                        __u32 mode, struct lustre_handle *);
        int (*o_cancel_unused)(struct obd_export *, struct lov_stripe_md *,
                               int flags, void *opaque);
        int (*o_join_lru)(struct obd_export *, struct lov_stripe_md *, 
                         int join);
        int (*o_san_preprw)(int cmd, struct obd_export *exp,
                            struct obdo *oa, int objcount,
                            struct obd_ioobj *obj, int niocount,
                            struct niobuf_remote *remote);
        int (*o_init_export)(struct obd_export *exp);
        int (*o_destroy_export)(struct obd_export *exp);

        /* llog related obd_methods */
        int (*o_llog_init)(struct obd_device *obd, struct obd_device *disk_obd,
                           int count, struct llog_catid *logid);
        int (*o_llog_finish)(struct obd_device *obd, int count);

        /* metadata-only methods */
        int (*o_pin)(struct obd_export *, obd_id ino, __u32 gen, int type,
                     struct obd_client_handle *, int flag);
        int (*o_unpin)(struct obd_export *, struct obd_client_handle *, int);

        int (*o_import_event)(struct obd_device *, struct obd_import *,
                              enum obd_import_event);

        int (*o_notify)(struct obd_device *obd, struct obd_device *watched,
                        int active);

        int (*o_health_check)(struct obd_device *);

        /* quota methods */
        int (*o_quotacheck)(struct obd_export *, struct obd_quotactl *);
        int (*o_quotactl)(struct obd_export *, struct obd_quotactl *);

        /* 
         * NOTE: If adding ops, add another LPROCFS_OBD_OP_INIT() line
         * to lprocfs_alloc_obd_stats() in obdclass/lprocfs_status.c.
         * Also, add a wrapper function in include/linux/obd_class.h.
         *
         * Also note that if you add it to the END, you also have to change
         * the num_stats calculation.
         *
         */
};


static inline void obd_transno_commit_cb(struct obd_device *obd, __u64 transno,
                                         int error)
{
        if (error) {
                CERROR("%s: transno "LPD64" commit error: %d\n",
                       obd->obd_name, transno, error);
                return;
        }
        CDEBUG(D_HA, "%s: transno "LPD64" committed\n",
               obd->obd_name, transno);
        if (transno > obd->obd_last_committed) {
                obd->obd_last_committed = transno;
                ptlrpc_commit_replies (obd);
        }
}

#endif /* __OBD_H */
