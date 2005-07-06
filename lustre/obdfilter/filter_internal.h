/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 */

#ifndef _FILTER_INTERNAL_H
#define _FILTER_INTERNAL_H

#ifdef __KERNEL__
# include <linux/spinlock.h>
#endif
#include <linux/lustre_handles.h>
#include <linux/lustre_debug.h>
#include <linux/obd.h>

#define FILTER_LAYOUT_VERSION "2"

#ifndef OBD_FILTER_DEVICENAME
# define OBD_FILTER_DEVICENAME "obdfilter"
#endif

#ifndef OBD_FILTER_SAN_DEVICENAME
# define OBD_FILTER_SAN_DEVICENAME "sanobdfilter"
#endif

#define LAST_RCVD "last_rcvd"
#define FILTER_INIT_OBJID 0

#define FILTER_LR_SERVER_SIZE    512

#define FILTER_LR_CLIENT_START   8192
#define FILTER_LR_CLIENT_SIZE    128

/* This limit is arbitrary, but for now we fit it in 1 page (32k clients) */
#define FILTER_LR_MAX_CLIENTS (PAGE_SIZE * 8)

#define FILTER_SUBDIR_COUNT      32            /* set to zero for no subdirs */
#define FILTER_GROUPS 3 /* must be at least 3; not dynamic yet */

#define FILTER_RECOVERY_TIMEOUT (obd_timeout * 5 * HZ / 2) /* *waves hands* */

#define FILTER_ROCOMPAT_SUPP   (0)

#define FILTER_INCOMPAT_GROUPS 0x00000001
#define FILTER_INCOMPAT_SUPP   (FILTER_INCOMPAT_GROUPS)

#define FILTER_GRANT_CHUNK (2ULL*1024*1024)

/* Data stored per server at the head of the last_rcvd file.  In le32 order.
 * Try to keep this the same as mds_server_data so we might one day merge. */
struct filter_server_data {
        __u8  fsd_uuid[40];        /* server UUID */
        __u64 fsd_unused;          /* was fsd_last_objid - don't use for now */
        __u64 fsd_last_transno;    /* last completed transaction ID */
        __u64 fsd_mount_count;     /* FILTER incarnation number */
        __u32 fsd_feature_compat;  /* compatible feature flags */
        __u32 fsd_feature_rocompat;/* read-only compatible feature flags */
        __u32 fsd_feature_incompat;/* incompatible feature flags */
        __u32 fsd_server_size;     /* size of server data area */
        __u32 fsd_client_start;    /* start of per-client data area */
        __u16 fsd_client_size;     /* size of per-client data area */
        __u16 fsd_subdir_count;    /* number of subdirectories for objects */
        __u64 fsd_catalog_oid;     /* recovery catalog object id */
        __u32 fsd_catalog_ogen;    /* recovery catalog inode generation */
        __u8  fsd_peeruuid[40];    /* UUID of MDS associated with this OST */
        __u8  fsd_padding[FILTER_LR_SERVER_SIZE - 140];
};

/* Data stored per client in the last_rcvd file.  In le32 order. */
struct filter_client_data {
        __u8  fcd_uuid[40];        /* client UUID */
        __u64 fcd_last_rcvd;       /* last completed transaction ID */
        __u64 fcd_last_xid;        /* client RPC xid for the last transaction */
        __u8  fcd_padding[FILTER_LR_CLIENT_SIZE - 56];
};

#define FILTER_DENTRY_MAGIC 0x9efba101
#define FILTER_FLAG_DESTROY 0x0001      /* destroy dentry on last file close */

/* Limit the returned fields marked valid to those that we actually might set */
#define FILTER_VALID_FLAGS (OBD_MD_FLTYPE | OBD_MD_FLMODE | OBD_MD_FLGENER  |\
                            OBD_MD_FLSIZE | OBD_MD_FLBLOCKS | OBD_MD_FLBLKSZ|\
                            OBD_MD_FLATIME | OBD_MD_FLMTIME | OBD_MD_FLCTIME)

enum {
        LPROC_FILTER_READ_BYTES = 0,
        LPROC_FILTER_WRITE_BYTES = 1,
        LPROC_FILTER_LAST,
};

//#define FILTER_MAX_CACHE_SIZE (32 * 1024 * 1024) /* was OBD_OBJECT_EOF */
#define FILTER_MAX_CACHE_SIZE OBD_OBJECT_EOF

/* We have to pass a 'created' array to fsfilt_map_inode_pages() which we
 * then ignore.  So we pre-allocate one that everyone can use... */
#define OBDFILTER_CREATED_SCRATCHPAD_ENTRIES 1024
extern int *obdfilter_created_scratchpad;

/* filter.c */
void f_dput(struct dentry *);
struct dentry *filter_fid2dentry(struct obd_device *, struct dentry *dir,
                                 obd_gr group, obd_id id);
struct dentry *__filter_oa2dentry(struct obd_device *obd, struct obdo *oa,
                                  const char *what, int quiet);
#define filter_oa2dentry(obd, oa) __filter_oa2dentry(obd, oa, __FUNCTION__, 0)

int filter_finish_transno(struct obd_export *, struct obd_trans_info *, int rc);
__u64 filter_next_id(struct filter_obd *, struct obdo *);
__u64 filter_last_id(struct filter_obd *, struct obdo *);
int filter_update_server_data(struct obd_device *, struct file *,
                              struct filter_server_data *, int force_sync);
int filter_update_last_objid(struct obd_device *, obd_gr, int force_sync);
int filter_common_setup(struct obd_device *, obd_count len, void *buf,
                        void *option);
int filter_destroy(struct obd_export *exp, struct obdo *oa,
                   struct lov_stripe_md *md, struct obd_trans_info *);
int filter_setattr(struct obd_export *exp, struct obdo *oa,
                   struct lov_stripe_md *md, struct obd_trans_info *oti);

/* filter_lvb.c */
extern struct ldlm_valblock_ops filter_lvbo;


/* filter_io.c */
int filter_preprw(int cmd, struct obd_export *, struct obdo *, int objcount,
                  struct obd_ioobj *, int niocount, struct niobuf_remote *,
                  struct niobuf_local *, struct obd_trans_info *);
int filter_commitrw(int cmd, struct obd_export *, struct obdo *, int objcount,
                    struct obd_ioobj *, int niocount, struct niobuf_local *,
                    struct obd_trans_info *, int rc);
int filter_brw(int cmd, struct obd_export *, struct obdo *,
	       struct lov_stripe_md *, obd_count oa_bufs, struct brw_page *,
	       struct obd_trans_info *);
void flip_into_page_cache(struct inode *inode, struct page *new_page);
void filter_free_dio_pages(int objcount, struct obd_ioobj *obj,
                           int niocount, struct niobuf_local *res);

/* filter_io_*.c */
int filter_commitrw_write(struct obd_export *exp, struct obdo *oa, int objcount,
                          struct obd_ioobj *obj, int niocount,
                          struct niobuf_local *res, struct obd_trans_info *oti,
                          int rc);
obd_size filter_grant_space_left(struct obd_export *exp);
long filter_grant(struct obd_export *exp, obd_size current_grant,
                  obd_size want, obd_size fs_space_left);
void filter_grant_commit(struct obd_export *exp, int niocount,
                         struct niobuf_local *res);
int filter_alloc_iobuf(struct filter_obd *, int rw, int num_pages, void **ret);
void filter_free_iobuf(void *iobuf);
int filter_iobuf_add_page(struct obd_device *obd, void *iobuf,
                          struct inode *inode, struct page *page);
int filter_direct_io(int rw, struct dentry *dchild, void *iobuf,
                     struct obd_export *exp, struct iattr *attr,
                     struct obd_trans_info *oti, void **wait_handle);

/* filter_log.c */
struct ost_filterdata {
        __u32  ofd_epoch;
};
int filter_log_sz_change(struct llog_handle *cathandle,
                         struct ll_fid *mds_fid,
                         __u32 io_epoch,
                         struct llog_cookie *logcookie,
                         struct inode *inode);
//int filter_get_catalog(struct obd_device *);
void filter_cancel_cookies_cb(struct obd_device *obd, __u64 transno,
                              void *cb_data, int error);
int filter_recov_log_mds_ost_cb(struct llog_handle *llh,
                               struct llog_rec_hdr *rec, void *data);

/* filter_san.c */
int filter_san_setup(struct obd_device *obd, obd_count len, void *buf);
int filter_san_preprw(int cmd, struct obd_export *, struct obdo *, int objcount,
                      struct obd_ioobj *, int niocount, struct niobuf_remote *);

#ifdef LPROCFS
void filter_tally_write(struct filter_obd *filter, struct page **pages,
                        int nr_pages, unsigned long *blocks,
                        int blocks_per_page);
void filter_tally_read(struct filter_obd *filter, struct niobuf_local *lnb,
                       int niocount);
int lproc_filter_attach_seqstat(struct obd_device *dev);
#else
static inline void filter_tally_write(struct filter_obd *filter,
                                 struct page **pages, int nr_pages,
                                 unsigned long *blocks, int blocks_per_page) {}
static inline void filter_tally_read(struct filter_obd *filter,
                                     struct niobuf_local *lnb, int niocount) {}
static inline int lproc_filter_attach_seqstat(struct obd_device *dev) {}
#endif

/* Quota stuff */
#ifdef HAVE_QUOTA_SUPPORT
int filter_quota_setup(struct filter_obd *filter);
void filter_quota_cleanup(struct filter_obd *filter);
void filter_quota_set_info(struct obd_export *exp, struct obd_device *obd);
int filter_quotacheck(struct obd_export *exp, struct obd_quotactl *oqctl);
int filter_quotactl(struct obd_export *exp, struct obd_quotactl *oqctl);
int filter_quota_enforcement(struct obd_device *obd,
                                    unsigned int fsuid, unsigned int fsgid,
                                    struct lvfs_ucred **ret_uc);
int filter_get_quota_flag(struct obd_device *obd, struct obdo *oa);
int filter_quota_check_master(struct obd_device *obd, struct inode *inode);

#ifdef LPROCFS
int lprocfs_filter_rd_bunit(char *page, char **start, off_t off,
                                   int count, int *eof, void *data);
int lprocfs_filter_rd_iunit(char *page, char **start, off_t off,
                                   int count, int *eof, void *data);
int lprocfs_filter_wr_bunit(struct file *file, const char *buffer,
                                   unsigned long count, void *data);
int lprocfs_filter_wr_iunit(struct file *file, const char *buffer,
                                   unsigned long count, void *data);
int lprocfs_filter_rd_btune(char *page, char **start, off_t off,
                                   int count, int *eof, void *data);
int lprocfs_filter_rd_itune(char *page, char **start, off_t off,
                                   int count, int *eof, void *data);
int lprocfs_filter_wr_btune(struct file *file, const char *buffer,
                                   unsigned long count, void *data);
int lprocfs_filter_wr_itune(struct file *file, const char *buffer,
                                   unsigned long count, void *data);
#endif /* LPROCFS */
#else /* !HAVE_QUOTA_SUPPORT */
static inline int filter_quota_setup(struct filter_obd *filter)
{
        return 0;
}
static inline void filter_quota_cleanup(struct filter_obd *filter) {}
static inline void filter_quota_set_info(struct obd_export *exp,
                                         struct obd_device *obd) {}
static inline int filter_quotacheck(struct obd_export *exp,
                                    struct obd_quotactl *oqctl)
{
        return -ENOTSUPP;
}
static inline int filter_quotactl(struct obd_export *exp,
                                  struct obd_quotactl *oqctl)
{
        return -ENOTSUPP;
}
static inline int filter_quota_enforcement(struct obd_device *obd,
                                           unsigned int fsuid,
                                           unsigned int fsgid,
                                           struct lvfs_ucred **ret_uc)
{
        return 0;
}
static inline int filter_get_quota_flag(struct obd_device *obd,
                                        struct obdo *oa)
{
        return 0;
}
static inline int filter_quota_check_master(struct obd_device *obd,
                                            struct inode *inode)
{
        return 0;
}
#endif /* HAVE_QUOTA_SUPPORT */

#endif /* _FILTER_INTERNAL_H */
