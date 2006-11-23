/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  lustre/mdt/mdt_internal.h
 *  Lustre Metadata Target (mdt) request handler
 *
 *  Copyright (c) 2006 Cluster File Systems, Inc.
 *   Author: Peter Braam <braam@clusterfs.com>
 *   Author: Andreas Dilger <adilger@clusterfs.com>
 *   Author: Phil Schwan <phil@clusterfs.com>
 *   Author: Mike Shaver <shaver@clusterfs.com>
 *   Author: Nikita Danilov <nikita@clusterfs.com>
 *   Author: Huang Hua <huanghua@clusterfs.com>
 *
 *   This file is part of the Lustre file system, http://www.lustre.org
 *   Lustre is a trademark of Cluster File Systems, Inc.
 *
 *   You may have signed or agreed to another license before downloading
 *   this software.  If so, you are bound by the terms and conditions
 *   of that agreement, and the following does not apply to you.  See the
 *   LICENSE file included with this distribution for more information.
 *
 *   If you did not agree to a different license, then this copy of Lustre
 *   is open source software; you can redistribute it and/or modify it
 *   under the terms of version 2 of the GNU General Public License as
 *   published by the Free Software Foundation.
 *
 *   In either case, Lustre is distributed in the hope that it will be
 *   useful, but WITHOUT ANY WARRANTY; without even the implied warranty
 *   of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   license text for more details.
 */

#ifndef _MDT_INTERNAL_H
#define _MDT_INTERNAL_H

#if defined(__KERNEL__)

/*
 * struct ptlrpc_client
 */
#include <lustre_net.h>
#include <obd.h>
/*
 * struct obd_connect_data
 * struct lustre_handle
 */
#include <lustre/lustre_idl.h>
#include <md_object.h>
#include <dt_object.h>
#include <lustre_fid.h>
#include <lustre_fld.h>
#include <lustre_req_layout.h>
/* LR_CLIENT_SIZE, etc. */
#include <lustre_disk.h>
#include <lustre_sec.h>
#include <lvfs.h>


/* Data stored per client in the last_rcvd file.  In le32 order. */
struct mdt_client_data {
        __u8  mcd_uuid[40];     /* client UUID */
        __u64 mcd_last_transno; /* last completed transaction ID */
        __u64 mcd_last_xid;     /* xid for the last transaction */
        __u32 mcd_last_result;  /* result from last RPC */
        __u32 mcd_last_data;    /* per-op data (disposition for open &c.) */
        /* for MDS_CLOSE requests */
        __u64 mcd_last_close_transno; /* last completed transaction ID */
        __u64 mcd_last_close_xid;     /* xid for the last transaction */
        __u32 mcd_last_close_result;  /* result from last RPC */
        __u8 mcd_padding[LR_CLIENT_SIZE - 84];
};

static inline __u64 mcd_last_transno(struct mdt_client_data *mcd)
{
        return max(mcd->mcd_last_transno, mcd->mcd_last_close_transno);
}

static inline __u64 mcd_last_xid(struct mdt_client_data *mcd)
{
        return max(mcd->mcd_last_xid, mcd->mcd_last_close_xid);
}

/* check if request's xid is equal to last one or not*/
static inline int req_xid_is_last(struct ptlrpc_request *req)
{
        struct mdt_client_data *mcd = req->rq_export->exp_mdt_data.med_mcd;
        return (req->rq_xid == mcd->mcd_last_xid ||
                req->rq_xid == mcd->mcd_last_close_xid);
}

/* copied from lr_server_data.
 * mds data stored at the head of last_rcvd file. In le32 order. */
struct mdt_server_data {
        __u8  msd_uuid[40];        /* server UUID */
        __u64 msd_last_transno;    /* last completed transaction ID */
        __u64 msd_mount_count;     /* incarnation number */
        __u32 msd_feature_compat;  /* compatible feature flags */
        __u32 msd_feature_rocompat;/* read-only compatible feature flags */
        __u32 msd_feature_incompat;/* incompatible feature flags */
        __u32 msd_server_size;     /* size of server data area */
        __u32 msd_client_start;    /* start of per-client data area */
        __u16 msd_client_size;     /* size of per-client data area */
        //__u16 msd_subdir_count;    /* number of subdirectories for objects */
        //__u64 msd_catalog_oid;     /* recovery catalog object id */
        //__u32 msd_catalog_ogen;    /* recovery catalog inode generation */
        //__u8  msd_peeruuid[40];    /* UUID of MDS associated with this OST */
        //__u32 msd_ost_index;       /* index number of OST in LOV */
        //__u32 msd_mdt_index;       /* index number of MDT in LMV */
        __u8  msd_padding[LR_SERVER_SIZE - 78];
};

struct mdt_object;
/* file data for open files on MDS */
struct mdt_file_data {
        struct portals_handle mfd_handle; /* must be first */
        struct list_head      mfd_list;   /* protected by med_open_lock */
        __u64                 mfd_xid;    /* xid of the open request */
        int                   mfd_mode;   /* open mode provided by client */
        struct mdt_object    *mfd_object; /* point to opened object */
};

struct mdt_device {
        /* super-class */
        struct md_device           mdt_md_dev;
        struct ptlrpc_service     *mdt_regular_service;
        struct ptlrpc_service     *mdt_readpage_service;
        struct ptlrpc_service     *mdt_setattr_service;
        struct ptlrpc_service     *mdt_mdsc_service;
        struct ptlrpc_service     *mdt_mdss_service;
        struct ptlrpc_service     *mdt_dtss_service;
        struct ptlrpc_service     *mdt_fld_service;
        /* DLM name-space for meta-data locks maintained by this server */
        struct ldlm_namespace     *mdt_namespace;
        /* ptlrpc handle for MDS->client connections (for lock ASTs). */
        struct ptlrpc_client      *mdt_ldlm_client;
        /* underlying device */
        struct md_device          *mdt_child;
        struct dt_device          *mdt_bottom;
        /*
         * Options bit-fields.
         */
        struct {
                signed int         mo_user_xattr :1,
                                   mo_acl        :1,
                                   mo_compat_resname:1,
                                   mo_mds_capa   :1,
                                   mo_oss_capa   :1;
        } mdt_opts;

        /* lock to pretect epoch and write count */
        spinlock_t                 mdt_ioepoch_lock;
        __u64                      mdt_ioepoch;

        /* Transaction related stuff here */
        spinlock_t                 mdt_transno_lock;
        __u64                      mdt_last_transno;

        /* transaction callbacks */
        struct dt_txn_callback     mdt_txn_cb;
        /* last_rcvd file */
        struct dt_object          *mdt_last_rcvd;

        /* these values should be updated from lov if necessary.
         * or should be placed somewhere else. */
        int                        mdt_max_mdsize;
        int                        mdt_max_cookiesize;
        __u64                      mdt_mount_count;

        /* last_rcvd data */
        struct mdt_server_data     mdt_msd;
        spinlock_t                 mdt_client_bitmap_lock;
        unsigned long              mdt_client_bitmap[(LR_MAX_CLIENTS >> 3) / sizeof(long)];

        struct upcall_cache        *mdt_identity_cache;
        struct upcall_cache        *mdt_rmtacl_cache;

        /* root squash */
        struct rootsquash_info     *mdt_rootsquash_info;

        /* capability keys */
        unsigned long              mdt_capa_timeout;
        __u32                      mdt_capa_alg;
        struct dt_object          *mdt_ck_obj;
        unsigned long              mdt_ck_timeout;
        unsigned long              mdt_ck_expiry;
        struct timer_list          mdt_ck_timer;
        struct ptlrpc_thread       mdt_ck_thread;
        struct lustre_capa_key     mdt_capa_keys[2];
        unsigned int               mdt_capa_conf:1;

        cfs_proc_dir_entry_t      *mdt_proc_entry;
        struct lprocfs_stats      *mdt_stats;
};

/*XXX copied from mds_internal.h */
#define MDT_SERVICE_WATCHDOG_TIMEOUT (obd_timeout * 1000)
#define MDT_ROCOMPAT_SUPP       (OBD_ROCOMPAT_LOVOBJID)
#define MDT_INCOMPAT_SUPP       (OBD_INCOMPAT_MDT | OBD_INCOMPAT_COMMON_LR)

struct mdt_object {
        struct lu_object_header mot_header;
        struct md_object        mot_obj;
        __u64                   mot_ioepoch;
        __u64                   mot_flags;
        int                     mot_epochcount;
        int                     mot_writecount;
};

struct mdt_lock_handle {
        /* Lock type, reg for cross-ref use or pdo lock. */
        mdl_type_t              mlh_type;

        /* Regular lock */
        struct lustre_handle    mlh_reg_lh;
        ldlm_mode_t             mlh_reg_mode;

        /* Pdirops lock */
        struct lustre_handle    mlh_pdo_lh;
        ldlm_mode_t             mlh_pdo_mode;
        unsigned int            mlh_pdo_hash;
};

enum {
        MDT_LH_PARENT, /* parent lockh */
        MDT_LH_CHILD,  /* child lockh */
        MDT_LH_OLD,    /* old lockh for rename */
        MDT_LH_NEW,    /* new lockh for rename */
        MDT_LH_RMT,    /* used for return lh to caller */
        MDT_LH_NR
};

enum {
        MDT_LOCAL_LOCK,
        MDT_CROSS_LOCK
};

struct mdt_reint_record {
        mdt_reint_t             rr_opcode;
        const struct lu_fid    *rr_fid1;
        const struct lu_fid    *rr_fid2;
        const char             *rr_name;
        int                     rr_namelen;
        const char             *rr_tgt;
        int                     rr_tgtlen;
        const void             *rr_eadata;
        int                     rr_eadatalen;
        int                     rr_logcookielen;
        const struct llog_cookie  *rr_logcookies;
        __u32                   rr_flags;
};

enum mdt_reint_flag {
        MRF_SETATTR_LOCKED = 1 << 0,
};

enum {
        MDT_NONEED_TRANSNO = (1 << 0) /*Do not need transno for this req*/
};

/*
 * Common data shared by mdt-level handlers. This is allocated per-thread to
 * reduce stack consumption.
 */
struct mdt_thread_info {
        /*
         * XXX: Part One:
         * The following members will be filled expilictly
         * with specific data in mdt_thread_info_init().
         */

        /*
         * for req-layout interface. This field should be first to be compatible
         * with "struct com_thread_info" in seq and fld.
         */
        struct req_capsule         mti_pill;
        /*
         * number of buffers in reply message.
         */
        int                        mti_rep_buf_nr;
        /*
         * sizes of reply buffers.
         */
        int                        mti_rep_buf_size[REQ_MAX_FIELD_NR];
        /*
         * A couple of lock handles.
         */
        struct mdt_lock_handle     mti_lh[MDT_LH_NR];

        struct mdt_device         *mti_mdt;
        const struct lu_env       *mti_env;

        /*
         * Additional fail id that can be set by handler. Passed to
         * target_send_reply().
         */
        int                        mti_fail_id;

        /* transaction number of current request */
        __u64                      mti_transno;


        /*
         * XXX: Part Two:
         * The following members will be filled expilictly
         * with zero in mdt_thread_info_init(). These members may be used
         * by all requests.
         */

        /*
         * Object attributes.
         */
        struct md_attr             mti_attr;
        /*
         * Body for "habeo corpus" operations.
         */
        const struct mdt_body     *mti_body;
        /*
         * Host object. This is released at the end of mdt_handler().
         */
        struct mdt_object         *mti_object;
        /*
         * Lock request for "habeo clavis" operations.
         */
        const struct ldlm_request *mti_dlm_req;

        __u32                      mti_has_trans:1, /* has txn already? */
                                   mti_no_need_trans:1,
                                   mti_cross_ref:1;

        /* opdata for mdt_reint_open(), has the same as
         * ldlm_reply:lock_policy_res1.  mdt_update_last_rcvd() stores this
         * value onto disk for recovery when mdt_trans_stop_cb() is called.
         */
        __u64                      mti_opdata;

        /*
         * XXX: Part Three:
         * The following members will be filled expilictly
         * with zero in mdt_reint_unpack(), because they are only used
         * by reint requests (including mdt_reint_open()).
         */

        /*
         * reint record. contains information for reint operations.
         */
        struct mdt_reint_record    mti_rr;
        
        /*
         * Operation specification (currently create and lookup)
         */
        struct md_op_spec          mti_spec;

        /*
         * XXX: Part Four:
         * The following members will _NOT_ be initialized at all.
         * DO NOT expect them to contain any valid value.
         * They should be initialized explicitly by the user themselves.
         */

         /* XXX: If something is in a union, make sure they do not conflict */

        struct lu_fid              mti_tmp_fid1;
        struct lu_fid              mti_tmp_fid2;
        ldlm_policy_data_t         mti_policy;    /* for mdt_object_lock() and
                                                   * mdt_rename_lock() */
        struct ldlm_res_id         mti_res_id;    /* for mdt_object_lock() and
                                                     mdt_rename_lock()   */
        union {
                struct obd_uuid    uuid[2];       /* for mdt_seq_init_cli()  */
                char               ns_name[48];   /* for mdt_init0()         */
                struct lustre_cfg_bufs bufs;      /* for mdt_stack_fini()    */
                struct kstatfs     ksfs;          /* for mdt_statfs()        */
                struct {
                        /* for mdt_readpage()      */
                        struct lu_rdpg     mti_rdpg;
                        /* for mdt_sendpage()      */
                        struct l_wait_info mti_wait_info;
                } rdpg;
        } mti_u;

        /* IO epoch related stuff. */
        struct mdt_epoch          *mti_epoch;
        __u64                      mti_replayepoch;

        /* server and client data buffers */
        struct mdt_server_data     mti_msd;
        struct mdt_client_data     mti_mcd;
        loff_t                     mti_off;
        struct txn_param           mti_txn_param;
        struct lu_buf              mti_buf;
        struct lustre_capa_key     mti_capa_key;

        /* Time for stats */
        struct timeval             mti_time;

        /* Ops object filename */
        struct lu_name             mti_name;
};
/*
 * Info allocated per-transaction.
 */
struct mdt_txn_info {
        __u64  txi_transno;
};

static inline struct md_device_operations *mdt_child_ops(struct mdt_device * m)
{
        LASSERT(m->mdt_child);
        return m->mdt_child->md_ops;
}

static inline struct md_object *mdt_object_child(struct mdt_object *o)
{
        return lu2md(lu_object_next(&o->mot_obj.mo_lu));
}

static inline struct ptlrpc_request *mdt_info_req(struct mdt_thread_info *info)
{
         return info->mti_pill.rc_req;
}

static inline void mdt_object_get(const struct lu_env *env,
                                  struct mdt_object *o)
{
        ENTRY;
        lu_object_get(&o->mot_obj.mo_lu);
        EXIT;
}

static inline void mdt_object_put(const struct lu_env *env,
                                  struct mdt_object *o)
{
        ENTRY;
        lu_object_put(env, &o->mot_obj.mo_lu);
        EXIT;
}

static inline int mdt_object_exists(const struct mdt_object *o)
{
        return lu_object_exists(&o->mot_obj.mo_lu);
}

static inline const struct lu_fid *mdt_object_fid(struct mdt_object *o)
{
        return lu_object_fid(&o->mot_obj.mo_lu);
}

int mdt_get_disposition(struct ldlm_reply *rep, int flag);
void mdt_set_disposition(struct mdt_thread_info *info,
                        struct ldlm_reply *rep, int flag);
void mdt_clear_disposition(struct mdt_thread_info *info,
                        struct ldlm_reply *rep, int flag);

void mdt_lock_pdo_init(struct mdt_lock_handle *lh,
                       ldlm_mode_t lm, const char *name,
                       int namelen);

void mdt_lock_reg_init(struct mdt_lock_handle *lh,
                       ldlm_mode_t lm);

int mdt_lock_setup(struct mdt_thread_info *info,
                   struct mdt_object *o,
                   struct mdt_lock_handle *lh);

int mdt_object_lock(struct mdt_thread_info *,
                    struct mdt_object *,
                    struct mdt_lock_handle *,
                    __u64, int);

void mdt_object_unlock(struct mdt_thread_info *,
                       struct mdt_object *,
                       struct mdt_lock_handle *,
                       int decref);

struct mdt_object *mdt_object_find(const struct lu_env *,
                                   struct mdt_device *,
                                   const struct lu_fid *);
struct mdt_object *mdt_object_find_lock(struct mdt_thread_info *,
                                        const struct lu_fid *,
                                        struct mdt_lock_handle *,
                                        __u64);
void mdt_object_unlock_put(struct mdt_thread_info *,
                           struct mdt_object *,
                           struct mdt_lock_handle *,
                           int decref);

int mdt_close_unpack(struct mdt_thread_info *info);
int mdt_reint_unpack(struct mdt_thread_info *info, __u32 op);
int mdt_reint_rec(struct mdt_thread_info *, struct mdt_lock_handle *);
void mdt_pack_size2body(struct mdt_body *b, const struct lu_attr *attr,
                        struct mdt_object *o);
void mdt_pack_attr2body(struct mdt_thread_info *info, struct mdt_body *b,
                        const struct lu_attr *attr, const struct lu_fid *fid);

int mdt_getxattr(struct mdt_thread_info *info);
int mdt_setxattr(struct mdt_thread_info *info);

void mdt_lock_handle_init(struct mdt_lock_handle *lh);
void mdt_lock_handle_fini(struct mdt_lock_handle *lh);

void mdt_reconstruct(struct mdt_thread_info *, struct mdt_lock_handle *);

int mdt_fs_setup(const struct lu_env *, struct mdt_device *,
                 struct obd_device *);
void mdt_fs_cleanup(const struct lu_env *, struct mdt_device *);

int mdt_client_del(const struct lu_env *env,
                    struct mdt_device *mdt,
                    struct mdt_export_data *med);
int mdt_client_add(const struct lu_env *env,
                   struct mdt_device *mdt,
                   struct mdt_export_data *med,
                   int cl_idx);
int mdt_client_new(const struct lu_env *env,
                   struct mdt_device *mdt,
                   struct mdt_export_data *med);

int mdt_recovery_handle(struct ptlrpc_request *);

int mdt_pin(struct mdt_thread_info* info);

int mdt_lock_new_child(struct mdt_thread_info *info,
                       struct mdt_object *o,
                       struct mdt_lock_handle *child_lockh);

int mdt_reint_open(struct mdt_thread_info *info,
                   struct mdt_lock_handle *lhc);

struct mdt_file_data *mdt_handle2mfd(const struct lustre_handle *handle);
int mdt_epoch_open(struct mdt_thread_info *info, struct mdt_object *o);
void mdt_sizeonmds_enable(struct mdt_thread_info *info, struct mdt_object *mo);
int mdt_sizeonmds_enabled(struct mdt_object *mo);
int mdt_write_get(struct mdt_device *mdt, struct mdt_object *o);
struct mdt_file_data *mdt_mfd_new(void);
int mdt_mfd_close(struct mdt_thread_info *info, struct mdt_file_data *mfd);
void mdt_mfd_free(struct mdt_file_data *mfd);
int mdt_close(struct mdt_thread_info *info);
int mdt_attr_set(struct mdt_thread_info *info, struct mdt_object *mo,
                 int flags);
int mdt_done_writing(struct mdt_thread_info *info);
void mdt_shrink_reply(struct mdt_thread_info *info, int offset,
                      int mdscapa, int osscapa);
int mdt_handle_last_unlink(struct mdt_thread_info *, struct mdt_object *,
                           const struct md_attr *);
void mdt_reconstruct_open(struct mdt_thread_info *, struct mdt_lock_handle *);
struct thandle* mdt_trans_start(const struct lu_env *env,
                                struct mdt_device *mdt, int credits);
void mdt_trans_stop(const struct lu_env *env,
                    struct mdt_device *mdt, struct thandle *th);
int mdt_record_write(const struct lu_env *env,
                     struct dt_object *dt, const struct lu_buf *buf,
                     loff_t *pos, struct thandle *th);
int mdt_record_read(const struct lu_env *env,
                    struct dt_object *dt, struct lu_buf *buf, loff_t *pos);

struct lu_buf *mdt_buf(const struct lu_env *env, void *area, ssize_t len);
const struct lu_buf *mdt_buf_const(const struct lu_env *env,
                                   const void *area, ssize_t len);

void mdt_dump_lmm(int level, const struct lov_mds_md *lmm);

int mdt_check_ucred(struct mdt_thread_info *);

int mdt_init_ucred(struct mdt_thread_info *, struct mdt_body *);

int mdt_init_ucred_reint(struct mdt_thread_info *);

void mdt_exit_ucred(struct mdt_thread_info *);

int groups_from_list(struct group_info *, gid_t *);

void groups_sort(struct group_info *);

/* mdt_idmap.c */
int mdt_init_idmap(struct mdt_thread_info *);

void mdt_cleanup_idmap(struct mdt_export_data *);

int mdt_handle_idmap(struct mdt_thread_info *);

int ptlrpc_user_desc_do_idmap(struct ptlrpc_request *,
                              struct ptlrpc_user_desc *);

void mdt_body_reverse_idmap(struct mdt_thread_info *,
                            struct mdt_body *);

int mdt_remote_perm_reverse_idmap(struct ptlrpc_request *,
                                  struct mdt_remote_perm *);

int mdt_fix_attr_ucred(struct mdt_thread_info *, __u32);

static inline struct mdt_device *mdt_dev(struct lu_device *d)
{
//        LASSERT(lu_device_is_mdt(d));
        return container_of0(d, struct mdt_device, mdt_md_dev.md_lu_dev);
}

/* mdt/mdt_identity.c */
#define MDT_IDENTITY_UPCALL_PATH        "/usr/sbin/l_getidentity"

extern struct upcall_cache_ops mdt_identity_upcall_cache_ops;

struct mdt_identity *mdt_identity_get(struct upcall_cache *, __u32);

void mdt_identity_put(struct upcall_cache *, struct mdt_identity *);

void mdt_flush_identity(struct upcall_cache *, int);

__u32 mdt_identity_get_setxid_perm(struct mdt_identity *, __u32, lnet_nid_t);

int mdt_pack_remote_perm(struct mdt_thread_info *, struct mdt_object *, void *);

/* mdt/mdt_rmtacl.c */
#define MDT_RMTACL_UPCALL_PATH          "/usr/sbin/l_facl"

extern struct upcall_cache_ops mdt_rmtacl_upcall_cache_ops;

int mdt_rmtacl_upcall(struct mdt_thread_info *, char *, struct lu_buf *);

extern struct lu_context_key       mdt_thread_key;
/* debug issues helper starts here*/
static inline void mdt_fail_write(const struct lu_env *env,
                                  struct dt_device *dd, int id)
{
        if (OBD_FAIL_CHECK(id)) {
                CERROR(LUSTRE_MDT_NAME": obd_fail_loc=%x, fail write ops\n",
                       id);
                dd->dd_ops->dt_ro(env, dd);
                /* We set FAIL_ONCE because we never "un-fail" a device */
                obd_fail_loc |= OBD_FAILED | OBD_FAIL_ONCE;
        }
}

static inline struct mdt_export_data *mdt_req2med(struct ptlrpc_request *req)
{
        return &req->rq_export->exp_mdt_data;
}

#define MDT_FAIL_CHECK(id)                                              \
({                                                                      \
        if (OBD_FAIL_CHECK(id))                                         \
                CERROR(LUSTRE_MDT_NAME": " #id " test failed\n");      \
        OBD_FAIL_CHECK(id);                                             \
})

#define MDT_FAIL_CHECK_ONCE(id)                                              \
({      int _ret_ = 0;                                                       \
        if (OBD_FAIL_CHECK(id)) {                                            \
                CERROR(LUSTRE_MDT_NAME": *** obd_fail_loc=%x ***\n", id);   \
                obd_fail_loc |= OBD_FAILED;                                  \
                if ((id) & OBD_FAIL_ONCE)                                    \
                        obd_fail_loc |= OBD_FAIL_ONCE;                       \
                _ret_ = 1;                                                   \
        }                                                                    \
        _ret_;                                                               \
})

#define MDT_FAIL_RETURN(id, ret)                                             \
do {                                                                         \
        if (MDT_FAIL_CHECK_ONCE(id)) {                                       \
                RETURN(ret);                                                 \
        }                                                                    \
} while(0)

struct md_ucred *mdt_ucred(const struct mdt_thread_info *info);

static inline int is_identity_get_disabled(struct upcall_cache *cache)
{
        return cache ? (strcmp(cache->uc_upcall, "NONE") == 0) : 1;
}

/* Issues dlm lock on passed @ns, @f stores it lock handle into @lh. */
static inline int mdt_fid_lock(struct ldlm_namespace *ns,
                               struct lustre_handle *lh,
                               ldlm_mode_t mode,
                               ldlm_policy_data_t *policy,
                               const struct ldlm_res_id *res_id,
                               int flags)
{
        int rc;

        LASSERT(ns != NULL);
        LASSERT(lh != NULL);

        rc = ldlm_cli_enqueue_local(ns, res_id, LDLM_IBITS, policy,
                                    mode, &flags, ldlm_blocking_ast,
                                    ldlm_completion_ast, NULL, NULL,
                                    0, NULL, lh);
        return rc == ELDLM_OK ? 0 : -EIO;
}

static inline void mdt_fid_unlock(struct lustre_handle *lh,
                                  ldlm_mode_t mode)
{
        ldlm_lock_decref(lh, mode);
}

extern mdl_mode_t mdt_mdl_lock_modes[];
extern ldlm_mode_t mdt_dlm_lock_modes[];

static inline mdl_mode_t mdt_dlm_mode2mdl_mode(ldlm_mode_t mode)
{
        LASSERT(IS_PO2(mode));
        return mdt_mdl_lock_modes[mode];
}

static inline ldlm_mode_t mdt_mdl_mode2dlm_mode(mdl_mode_t mode)
{
        LASSERT(IS_PO2(mode));
        return mdt_dlm_lock_modes[mode];
}

static inline struct lu_name *mdt_name(const struct lu_env *env,
                                       char *name, int namelen)
{
        struct lu_name *lname;
        struct mdt_thread_info *mti;

        mti = lu_context_key_get(&env->le_ctx, &mdt_thread_key);
        lname = &mti->mti_name;
        lname->ln_name = name;
        lname->ln_namelen = namelen;
        return lname;
}

/* lprocfs stuff */
int mdt_procfs_init(struct mdt_device *mdt, const char *name);
int mdt_procfs_fini(struct mdt_device *mdt);

void mdt_lprocfs_time_start(struct mdt_device *mdt,
			    struct timeval *start, int op);

void mdt_lprocfs_time_end(struct mdt_device *mdt,
			  struct timeval *start, int op);

enum {
        LPROC_MDT_REINT_CREATE = 0,
        LPROC_MDT_REINT_OPEN,
        LPROC_MDT_REINT_LINK,
        LPROC_MDT_REINT_UNLINK,
        LPROC_MDT_REINT_RENAME,
        LPROC_MDT_REINT_SETATTR,
        LPROC_MDT_GETATTR,
        LPROC_MDT_GETATTR_NAME,
        LPROC_MDT_INTENT_GETATTR,
        LPROC_MDT_INTENT_REINT,
        LPROC_MDT_LAST
};

/* Capability */
int mdt_ck_thread_start(struct mdt_device *mdt);
void mdt_ck_thread_stop(struct mdt_device *mdt);
void mdt_ck_timer_callback(unsigned long castmeharder);
int mdt_capa_keys_init(const struct lu_env *env, struct mdt_device *mdt);

static inline void mdt_set_capainfo(struct mdt_thread_info *info, int offset,
                                    const struct lu_fid *fid,
                                    struct lustre_capa *capa)
{
        struct mdt_device *dev = info->mti_mdt;
        struct md_capainfo *ci;

        if (!dev->mdt_opts.mo_mds_capa)
                return;

        ci = md_capainfo(info->mti_env);
        LASSERT(ci);
        ci->mc_fid[offset]  = fid;
        ci->mc_capa[offset] = capa;
}

#endif /* __KERNEL__ */
#endif /* _MDT_H */
