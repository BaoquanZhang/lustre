/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 */

#ifndef _MDS_INTERNAL_H
#define _MDS_INTERNAL_H

#include <lustre_disk.h>
#include <lustre_mds.h>

#define MDT_ROCOMPAT_SUPP       (OBD_ROCOMPAT_LOVOBJID)
#define MDT_INCOMPAT_SUPP       (OBD_INCOMPAT_MDT | OBD_INCOMPAT_COMMON_LR)

/* Data stored per client in the last_rcvd file.  In le32 order. */
struct mds_client_data {
        __u8 mcd_uuid[40];      /* client UUID */
        __u64 mcd_last_transno; /* last completed transaction ID */
        __u64 mcd_last_xid;     /* xid for the last transaction */
        __u32 mcd_last_result;  /* result from last RPC */
        __u32 mcd_last_data;    /* per-op data (disposition for open &c.) */
        __u8 mcd_padding[LR_CLIENT_SIZE - 64];
};

#define MDS_SERVICE_WATCHDOG_TIMEOUT (obd_timeout * 1000)

#define MAX_ATIME_DIFF 60

struct mds_filter_data {
        __u64 io_epoch;
};

#define MDS_FILTERDATA(inode) ((struct mds_filter_data *)(inode)->i_filterdata)

static inline struct mds_obd *mds_req2mds(struct ptlrpc_request *req)
{
        return &req->rq_export->exp_obd->u.mds;
}

#ifdef __KERNEL__
/* Open counts for files.  No longer atomic, must hold inode->i_sem */
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,5,0))
# define mds_inode_oatomic(inode)    ((inode)->i_cindex)
#else
# define mds_inode_oatomic(inode)    ((inode)->i_attr_flags)
#endif

#ifdef HAVE_I_ALLOC_SEM
#define MDS_UP_READ_ORPHAN_SEM(i)          UP_READ_I_ALLOC_SEM(i)
#define MDS_DOWN_READ_ORPHAN_SEM(i)        DOWN_READ_I_ALLOC_SEM(i)
#define LASSERT_MDS_ORPHAN_READ_LOCKED(i)  LASSERT_I_ALLOC_SEM_READ_LOCKED(i)

#define MDS_UP_WRITE_ORPHAN_SEM(i)         UP_WRITE_I_ALLOC_SEM(i)
#define MDS_DOWN_WRITE_ORPHAN_SEM(i)       DOWN_WRITE_I_ALLOC_SEM(i)
#define LASSERT_MDS_ORPHAN_WRITE_LOCKED(i) LASSERT_I_ALLOC_SEM_WRITE_LOCKED(i)
#define MDS_PACK_MD_LOCK 1
#else
#define MDS_UP_READ_ORPHAN_SEM(i)          do { up(&(i)->i_sem); } while (0)
#define MDS_DOWN_READ_ORPHAN_SEM(i)        do { down(&(i)->i_sem); } while (0)
#define LASSERT_MDS_ORPHAN_READ_LOCKED(i)  LASSERT(down_trylock(&(i)->i_sem)!=0)

#define MDS_UP_WRITE_ORPHAN_SEM(i)         do { up(&(i)->i_sem); } while (0)
#define MDS_DOWN_WRITE_ORPHAN_SEM(i)       do { down(&(i)->i_sem); } while (0)
#define LASSERT_MDS_ORPHAN_WRITE_LOCKED(i) LASSERT(down_trylock(&(i)->i_sem)!=0)
#define MDS_PACK_MD_LOCK 0
#endif

static inline int mds_orphan_open_count(struct inode *inode)
{
        LASSERT_MDS_ORPHAN_READ_LOCKED(inode);
        return mds_inode_oatomic(inode);
}

static inline int mds_orphan_open_inc(struct inode *inode)
{
        LASSERT_MDS_ORPHAN_WRITE_LOCKED(inode);
        return ++mds_inode_oatomic(inode);
}

static inline int mds_orphan_open_dec_test(struct inode *inode)
{
        LASSERT_MDS_ORPHAN_WRITE_LOCKED(inode);
        return --mds_inode_oatomic(inode) == 0;
}

#define mds_inode_is_orphan(inode)  ((inode)->i_flags & 0x4000000)

static inline void mds_inode_set_orphan(struct inode *inode)
{
        inode->i_flags |= 0x4000000;
        CDEBUG(D_VFSTRACE, "setting orphan flag on inode %p\n", inode);
}

static inline void mds_inode_unset_orphan(struct inode *inode)
{
        inode->i_flags &= ~(0x4000000);
        CDEBUG(D_VFSTRACE, "removing orphan flag from inode %p\n", inode);
}

#endif /* __KERNEL__ */

#define MDS_CHECK_RESENT(req, reconstruct)                                    \
{                                                                             \
        if (lustre_msg_get_flags(req->rq_reqmsg) & MSG_RESENT) {              \
                struct mds_client_data *mcd =                                 \
                        req->rq_export->exp_mds_data.med_mcd;                 \
                if (mcd->mcd_last_xid == req->rq_xid) {                       \
                        reconstruct;                                          \
                        RETURN(req->rq_repmsg->status);                       \
                }                                                             \
                DEBUG_REQ(D_HA, req, "no reply for RESENT req (have "LPD64")",\
                          mcd->mcd_last_xid);                                 \
        }                                                                     \
}

/* mds/mds_reint.c */
int res_gt(struct ldlm_res_id *res1, struct ldlm_res_id *res2,
           ldlm_policy_data_t *p1, ldlm_policy_data_t *p2);
int enqueue_ordered_locks(struct obd_device *obd, struct ldlm_res_id *p1_res_id,
                          struct lustre_handle *p1_lockh, int p1_lock_mode,
                          ldlm_policy_data_t *p1_policy,
                          struct ldlm_res_id *p2_res_id,
                          struct lustre_handle *p2_lockh, int p2_lock_mode,
                          ldlm_policy_data_t *p2_policy);
void mds_commit_cb(struct obd_device *, __u64 last_rcvd, void *data, int error);
int mds_finish_transno(struct mds_obd *mds, struct inode *inode, void *handle,
                       struct ptlrpc_request *req, int rc, __u32 op_data);
void mds_reconstruct_generic(struct ptlrpc_request *req);
void mds_req_from_mcd(struct ptlrpc_request *req, struct mds_client_data *mcd);
int mds_get_parent_child_locked(struct obd_device *obd, struct mds_obd *mds,
                                struct ll_fid *fid,
                                struct lustre_handle *parent_lockh,
                                struct dentry **dparentp, int parent_mode,
                                __u64 parent_lockpart,
                                char *name, int namelen,
                                struct lustre_handle *child_lockh,
                                struct dentry **dchildp, int child_mode,
                                __u64 child_lockpart);
int mds_lock_new_child(struct obd_device *obd, struct inode *inode,
                       struct lustre_handle *child_lockh);
int mds_osc_setattr_async(struct obd_device *obd, struct inode *inode,
                          struct lov_mds_md *lmm, int lmm_size,
                          struct llog_cookie *logcookies, struct ll_fid *fid);

int mds_get_parents_children_locked(struct obd_device *obd,
                                    struct mds_obd *mds,
                                    struct ll_fid *p1_fid,
                                    struct dentry **de_srcdirp,
                                    struct ll_fid *p2_fid,
                                    struct dentry **de_tgtdirp,
                                    int parent_mode,
                                    const char *old_name, int old_len,
                                    struct dentry **de_oldp,
                                    const char *new_name, int new_len,
                                    struct dentry **de_newp,
                                    struct lustre_handle *dlm_handles,
                                    int child_mode);

void mds_shrink_reply(struct obd_device *obd, struct ptlrpc_request *req,
                      struct mds_body *body);
int mds_get_cookie_size(struct obd_device *obd, struct lov_mds_md *lmm);
/* mds/mds_lib.c */
int mds_update_unpack(struct ptlrpc_request *, int offset,
                      struct mds_update_record *);
int mds_init_ucred(struct lvfs_ucred *ucred, struct ptlrpc_request *req,
                   int offset);
void mds_exit_ucred(struct lvfs_ucred *ucred, struct mds_obd *obd);

/* mds/mds_unlink_open.c */
int mds_cleanup_pending(struct obd_device *obd);


/* mds/mds_log.c */
int mds_log_op_unlink(struct obd_device *obd, struct inode *inode,
                      struct lov_mds_md *lmm, int lmm_size,
                      struct llog_cookie *logcookies, int cookies_size);
int mds_log_op_setattr(struct obd_device *obd, struct inode *inode,
                      struct lov_mds_md *lmm, int lmm_size,
                      struct llog_cookie *logcookies, int cookies_size);
int mds_llog_init(struct obd_device *obd, struct obd_device *tgt, int count,
                  struct llog_catid *logid);
int mds_llog_finish(struct obd_device *obd, int count);

/* mds/mds_lov.c */
int mds_lov_connect(struct obd_device *obd, char * lov_name);
int mds_lov_disconnect(struct obd_device *obd);
int mds_lov_write_objids(struct obd_device *obd);
void mds_lov_update_objids(struct obd_device *obd, obd_id *ids);
int mds_lov_clear_orphans(struct mds_obd *mds, struct obd_uuid *ost_uuid);
int mds_lov_set_nextid(struct obd_device *obd);
int mds_lov_start_synchronize(struct obd_device *obd, 
                              struct obd_device *watched,
                              void *data, int nonblock);
int mds_post_mds_lovconf(struct obd_device *obd);
int mds_notify(struct obd_device *obd, struct obd_device *watched,
               enum obd_notify_event ev, void *data);
int mds_convert_lov_ea(struct obd_device *obd, struct inode *inode,
                       struct lov_mds_md *lmm, int lmm_size);
void mds_objids_from_lmm(obd_id *ids, struct lov_mds_md *lmm,
                         struct lov_desc *desc);
int mds_init_lov_desc(struct obd_device *obd, struct obd_export *osc_exp);

/* mds/mds_open.c */
int mds_query_write_access(struct inode *inode);
int mds_open(struct mds_update_record *rec, int offset,
             struct ptlrpc_request *req, struct lustre_handle *);
int mds_pin(struct ptlrpc_request *req, int offset);
void mds_mfd_unlink(struct mds_file_data *mfd, int decref);
int mds_mfd_close(struct ptlrpc_request *req, int offset, struct obd_device *obd,
                  struct mds_file_data *mfd, int unlink_orphan);
int mds_close(struct ptlrpc_request *req, int offset);
int mds_done_writing(struct ptlrpc_request *req, int offset);

/*mds/mds_join.c*/
int mds_join_file(struct mds_update_record *rec, struct ptlrpc_request *req, 
                  struct dentry *dchild, struct lustre_handle *lockh);

/* mds/mds_fs.c */
int mds_client_add(struct obd_device *obd, struct mds_obd *mds,
                   struct mds_export_data *med, int cl_off);
int mds_client_free(struct obd_export *exp);
int mds_obd_create(struct obd_export *exp, struct obdo *oa,
                   struct lov_stripe_md **ea, struct obd_trans_info *oti);
int mds_obd_destroy(struct obd_export *exp, struct obdo *oa,
                    struct lov_stripe_md *ea, struct obd_trans_info *oti,
                    struct obd_export *md_exp);

/* mds/handler.c */
extern struct lvfs_callback_ops mds_lvfs_ops;
extern int mds_iocontrol(unsigned int cmd, struct obd_export *exp,
                         int len, void *karg, void *uarg);
int mds_postrecov(struct obd_device *obd);
int mds_init_export(struct obd_export *exp);
#ifdef __KERNEL__
int mds_get_md(struct obd_device *, struct inode *, void *md, int *size,
               int lock);
int mds_pack_md(struct obd_device *, struct lustre_msg *, int offset,
                struct mds_body *, struct inode *, int lock);
void mds_pack_inode2fid(struct ll_fid *fid, struct inode *inode);
void mds_pack_inode2body(struct mds_body *body, struct inode *inode);
#endif
int mds_pack_acl(struct mds_export_data *med, struct inode *inode,
                 struct lustre_msg *repmsg, struct mds_body *repbody,
                 int repoff);

/* quota stuff */
extern quota_interface_t mds_quota_interface;
extern quota_interface_t *quota_interface;

/* mds/mds_xattr.c */
int mds_setxattr(struct ptlrpc_request *req);
int mds_getxattr(struct ptlrpc_request *req);

#endif /* _MDS_INTERNAL_H */
