/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *   This file is part of Lustre, http://www.lustre.org
 *
 * MDS data structures.
 * See also lustre_idl.h for wire formats of requests.
 */

#ifndef _LUSTRE_MDS_H
#define _LUSTRE_MDS_H

#ifdef __KERNEL__
# include <linux/fs.h>
# include <linux/dcache.h>
#endif
#include <linux/lustre_handles.h>
#include <linux/kp30.h>
#include <linux/lustre_idl.h>
#include <linux/lustre_lib.h>
#include <linux/lustre_dlm.h>
#include <linux/lustre_log.h>
#include <linux/lustre_export.h>

struct ldlm_lock_desc;
struct mds_obd;
struct ptlrpc_connection;
struct ptlrpc_client;
struct obd_export;
struct ptlrpc_request;
struct obd_device;
struct ll_file_data;

#define LUSTRE_MDS_NAME "mds"
#define LUSTRE_MDT_NAME "mdt"
#define LUSTRE_MDC_NAME "mdc"

struct lustre_md {
        struct mds_body *body;
        struct lov_stripe_md *lsm;
};

struct ll_uctxt {
        __u32 gid1;
        __u32 gid2;
};

struct mdc_op_data {
        struct ll_fid fid1;
        struct ll_fid fid2;
        struct ll_uctxt ctxt;
        __u64 mod_time;
        const char *name;
        int namelen;
        __u32 create_mode;
};

struct mds_update_record {
        __u32 ur_opcode;
        struct ll_fid *ur_fid1;
        struct ll_fid *ur_fid2;
        int ur_namelen;
        char *ur_name;
        int ur_tgtlen;
        char *ur_tgt;
        int ur_eadatalen;
        void *ur_eadata;
        int ur_cookielen;
        struct llog_cookie *ur_logcookies;
        struct iattr ur_iattr;
        struct obd_ucred ur_uc;
        __u64 ur_rdev;
        __u32 ur_mode;
        __u64 ur_time;
        __u32 ur_flags;
};

#define ur_fsuid    ur_uc.ouc_fsuid
#define ur_fsgid    ur_uc.ouc_fsgid
#define ur_cap      ur_uc.ouc_cap
#define ur_suppgid1 ur_uc.ouc_suppgid1
#define ur_suppgid2 ur_uc.ouc_suppgid2
#define ur_umask    ur_uc.ouc_umask

#define MDS_LR_SERVER_SIZE    512

#define MDS_LR_CLIENT_START  8192
#define MDS_LR_CLIENT_SIZE    128
#if MDS_LR_CLIENT_START < MDS_LR_SERVER_SIZE
#error "Can't have MDS_LR_CLIENT_START < MDS_LR_SERVER_SIZE"
#endif

#define MDS_CLIENT_SLOTS 17

#define MDS_ROCOMPAT_LOVOBJID   0x00000001
#define MDS_ROCOMPAT_SUPP       (MDS_ROCOMPAT_LOVOBJID)

#define MDS_INCOMPAT_SUPP       (0)

/* Data stored per server at the head of the last_rcvd file.  In le32 order.
 * Try to keep this the same as fsd_server_data so we might one day merge. */
struct mds_server_data {
        __u8  msd_uuid[40];        /* server UUID */
        __u64 msd_last_transno;    /* last completed transaction ID */
        __u64 msd_mount_count;     /* MDS incarnation number */
        __u64 msd_unused;
        __u32 msd_feature_compat;  /* compatible feature flags */
        __u32 msd_feature_rocompat;/* read-only compatible feature flags */
        __u32 msd_feature_incompat;/* incompatible feature flags */
        __u32 msd_server_size;     /* size of server data area */
        __u32 msd_client_start;    /* start of per-client data area */
        __u16 msd_client_size;     /* size of per-client data area */
        __u16 msd_subdir_count;    /* number of subdirectories for objects */
        __u64 msd_catalog_oid;     /* recovery catalog object id */
        __u32 msd_catalog_ogen;    /* recovery catalog inode generation */
        __u8  msd_peeruuid[40];    /* UUID of LOV/OSC associated with MDS */
        __u8  msd_padding[MDS_LR_SERVER_SIZE - 140];
};

/* Data stored per client in the last_rcvd file.  In le32 order. */
struct mds_client_data {
        __u8 mcd_uuid[40];      /* client UUID */
        __u64 mcd_last_transno; /* last completed transaction ID */
        __u64 mcd_last_xid;     /* xid for the last transaction */
        __u32 mcd_last_result;  /* result from last RPC */
        __u32 mcd_last_data;    /* per-op data (disposition for open &c.) */
        __u8 mcd_padding[MDS_LR_CLIENT_SIZE - 64];
};

/* file data for open files on MDS */
struct mds_file_data {
        struct portals_handle mfd_handle; /* must be first */
        atomic_t              mfd_refcount;
        struct list_head      mfd_list;
        __u64                 mfd_xid;
        int                   mfd_mode;
        struct dentry        *mfd_dentry;
};

/* mds/mds_reint.c  */
int mds_reint_rec(struct mds_update_record *r, int offset,
                  struct ptlrpc_request *req, struct lustre_handle *);

/* mds/handler.c */
#ifdef __KERNEL__
struct dentry *mds_fid2locked_dentry(struct obd_device *obd, struct ll_fid *fid,
                                     struct vfsmount **mnt, int lock_mode,
                                     struct lustre_handle *lockh,
                                     char *name, int namelen);
struct dentry *mds_fid2dentry(struct mds_obd *mds, struct ll_fid *fid,
                              struct vfsmount **mnt);
int mds_update_server_data(struct obd_device *, int force_sync);

/* mds/mds_fs.c */
int mds_fs_setup(struct obd_device *obddev, struct vfsmount *mnt);
int mds_fs_cleanup(struct obd_device *obddev, int failover);
#endif

/* mds/mds_lov.c */

/* mdc/mdc_locks.c */
int it_disposition(struct lookup_intent *it, int flag);
void it_set_disposition(struct lookup_intent *it, int flag);
int it_open_error(int phase, struct lookup_intent *it);
void mdc_set_lock_data(__u64 *lockh, void *data);
int mdc_change_cbdata(struct obd_export *exp, struct ll_fid *fid, 
                      ldlm_iterator_t it, void *data);
int mdc_intent_lock(struct obd_export *exp, struct ll_uctxt *, 
                    struct ll_fid *parent, 
                    const char *name, int len, void *lmm, int lmmsize,
                    struct ll_fid *child,
                    struct lookup_intent *, int, 
                    struct ptlrpc_request **reqp,
                    ldlm_blocking_callback cb_blocking);
int mdc_enqueue(struct obd_export *exp,
                int lock_type,
                struct lookup_intent *it,
                int lock_mode,
                struct mdc_op_data *data,
                struct lustre_handle *lockh,
                void *lmm,
                int lmmlen,
                ldlm_completion_callback cb_completion,
                ldlm_blocking_callback cb_blocking,
                void *cb_data);

/* mdc/mdc_request.c */
int mdc_init_ea_size(struct obd_device *obd, char *lov_name);
int mdc_req2lustre_md(struct ptlrpc_request *req, int offset,
                      struct obd_export *exp,
                      struct lustre_md *md);
int mdc_getstatus(struct obd_export *exp, struct ll_fid *rootfid);
int mdc_getattr(struct obd_export *exp, struct ll_fid *fid,
                unsigned long valid, unsigned int ea_size,
                struct ptlrpc_request **request);
int mdc_getattr_name(struct obd_export *exp, struct ll_fid *fid,
                     char *filename, int namelen, unsigned long valid,
                     unsigned int ea_size, struct ptlrpc_request **request);
int mdc_setattr(struct obd_export *exp, struct mdc_op_data *data,
                struct iattr *iattr, void *ea, int ealen, void *ea2, int ea2len,
                struct ptlrpc_request **request);
int mdc_open(struct obd_export *exp, obd_id ino, int type, int flags,
             struct lov_mds_md *lmm, int lmm_size, struct lustre_handle *fh,
             struct ptlrpc_request **);
struct obd_client_handle;
void mdc_set_open_replay_data(struct obd_client_handle *och,
                              struct ptlrpc_request *open_req);
void mdc_clear_open_replay_data(struct obd_client_handle *och);
int mdc_close(struct obd_export *, struct obdo *, struct obd_client_handle *,
              struct ptlrpc_request **);
int mdc_readpage(struct obd_export *exp, struct ll_fid *mdc_fid, __u64 offset,
                 struct page *, struct ptlrpc_request **);
int mdc_create(struct obd_export *exp, struct mdc_op_data *op_data,
               const void *data, int datalen, int mode, __u32 uid, __u32 gid,
               __u64 rdev, struct ptlrpc_request **request);
int mdc_unlink(struct obd_export *exp, struct mdc_op_data *data,
               struct ptlrpc_request **request);
int mdc_link(struct obd_export *exp, struct mdc_op_data *data,
             struct ptlrpc_request **);
int mdc_rename(struct obd_export *exp, struct mdc_op_data *data,
               const char *old, int oldlen, const char *new, int newlen,
               struct ptlrpc_request **request);
int mdc_sync(struct obd_export *exp, struct ll_fid *fid,
             struct ptlrpc_request **);
int mdc_create_client(struct obd_uuid uuid, struct ptlrpc_client *cl);

/* Store the generation of a newly-created inode in |req| for replay. */
void mdc_store_inode_generation(struct ptlrpc_request *req, int reqoff,
                                int repoff);
int mdc_llog_process(struct obd_export *, char *logname, llog_cb_t, void *data);
int mdc_done_writing(struct obd_export *exp, struct obdo *);

static inline void mdc_pack_fid(struct ll_fid *fid, obd_id ino, __u32 gen,
                                int type)
{
        fid->id = ino;
        fid->generation = gen;
        fid->f_type = type;
}

/* ioctls for trying requests */
#define IOC_REQUEST_TYPE                   'f'
#define IOC_REQUEST_MIN_NR                 30

#define IOC_REQUEST_GETATTR             _IOWR('f', 30, long)
#define IOC_REQUEST_READPAGE            _IOWR('f', 31, long)
#define IOC_REQUEST_SETATTR             _IOWR('f', 32, long)
#define IOC_REQUEST_CREATE              _IOWR('f', 33, long)
#define IOC_REQUEST_OPEN                _IOWR('f', 34, long)
#define IOC_REQUEST_CLOSE               _IOWR('f', 35, long)
#define IOC_REQUEST_MAX_NR               35

#endif
