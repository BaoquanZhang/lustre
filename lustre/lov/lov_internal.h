/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) 2003 Cluster File Systems, Inc.
 *
 * This code is issued under the GNU General Public License.
 * See the file COPYING in this distribution
 */

#ifndef LOV_INTERNAL_H
#define LOV_INTERNAL_H

#include <lustre/lustre_user.h>

struct lov_lock_handles {
        struct portals_handle   llh_handle;
        atomic_t                llh_refcount;
        int                     llh_stripe_count;
        struct lustre_handle    llh_handles[0];
};

struct lov_request {
        struct list_head         rq_link;
        struct ldlm_extent       rq_extent;
        int                      rq_idx;        /* index in lov->tgts array */
        int                      rq_gen;        /* lov target generation # */
        int                      rq_stripe;     /* stripe number */
        int                      rq_complete;
        int                      rq_rc;
        int                      rq_buflen;     /* length of sub_md */
        struct obdo             *rq_oa;
        struct lov_stripe_md    *rq_md;
        obd_count                rq_oabufs;
        obd_count                rq_pgaidx;
};

struct lov_request_set {
        atomic_t                 set_refcount;
        struct obd_export       *set_exp;
        int                      set_count;
        int                      set_completes;
        int                      set_success;
        struct llog_cookie      *set_cookies;
        int                      set_cookie_sent;
        struct lov_stripe_md    *set_md;
        struct obdo             *set_oa;
        struct obd_trans_info   *set_oti;
        obd_count                set_oabufs;
        struct brw_page         *set_pga;
        struct lov_lock_handles *set_lockh;
        struct list_head         set_list;
};

#define LAP_MAGIC 8200

#define LOV_MAX_TGT_COUNT 1024

#define lov_tgts_lock(lov)      spin_lock(&lov->lov_lock);
#define lov_tgts_unlock(lov)    spin_unlock(&lov->lov_lock);

static inline void
lov_tgt_set_flags(struct lov_obd *lov, struct lov_tgt_desc *tgt, int flags)
{
        lov_tgts_lock(lov);
        if ((flags & LTD_ACTIVE) && ((tgt->ltd_flags & LTD_ACTIVE) == 0))
                lov->desc.ld_active_tgt_count++;
        tgt->ltd_flags |= flags;
        lov_tgts_unlock(lov);
}

static inline void
lov_tgt_clear_flags(struct lov_obd *lov, struct lov_tgt_desc *tgt, int flags)
{
        ENTRY;

        lov_tgts_lock(lov);
        if ((flags & LTD_ACTIVE) && (tgt->ltd_flags & LTD_ACTIVE))
                lov->desc.ld_active_tgt_count--;
        tgt->ltd_flags &= ~flags;
        lov_tgts_unlock(lov);
        EXIT;
}

static inline int
lov_tgt_changed(struct lov_obd *lov, struct lov_oinfo *loi)
{
        int changed;

        lov_tgts_lock(lov);
        changed = lov->tgts[loi->loi_ost_idx].ltd_gen != loi->loi_ost_gen;
        lov_tgts_unlock(lov);

        return changed;
}

static inline int
lov_tgt_active(struct lov_obd *lov, struct lov_tgt_desc *tgt, int gen)
{
        int rc = 0;
        lov_tgts_lock(lov);

        if (((gen == 0) || (gen == tgt->ltd_gen)) &&
            ((tgt->ltd_flags &(LTD_ACTIVE|LTD_DEL_PENDING)) == LTD_ACTIVE)) {
                tgt->ltd_refcount++;
                rc = 1;
        }

        lov_tgts_unlock(lov);
        return rc;
}

static inline int
lov_tgt_ready(struct lov_obd *lov, struct lov_tgt_desc *tgt, int gen)
{
        int rc = 0;

        lov_tgts_lock(lov);

        if (((gen == 0) || (gen == tgt->ltd_gen)) &&
            (tgt->ltd_flags & LTD_ACTIVE)) {
                tgt->ltd_refcount++;
                rc = 1;
        }

        lov_tgts_unlock(lov);
        return rc;
}

static inline void
lov_tgt_decref(struct lov_obd *lov, struct lov_tgt_desc *tgt)
{
        int do_wakeup = 0;

        lov_tgts_lock(lov);

        if ((--tgt->ltd_refcount == 0) && (tgt->ltd_flags & LTD_DEL_PENDING)) {
                do_wakeup = 1;
        } 

        lov_tgts_unlock(lov);
        if (do_wakeup)
                wake_up(&lov->lov_tgt_waitq);
}

static inline int
lov_tgt_pending(struct lov_obd *lov, struct lov_tgt_desc *tgt, int gen)
{
        int rc = 0;
        lov_tgts_lock(lov);

        if (((gen == 0) || (gen == tgt->ltd_gen)) &&
            (tgt->ltd_flags &(LTD_ACTIVE|LTD_DEL_PENDING)) == LTD_DEL_PENDING) {
                tgt->ltd_refcount++;
                rc = 1;
        }

        lov_tgts_unlock(lov);
        return rc;
}

struct lov_async_page {
        int                             lap_magic;
        int                             lap_stripe;
        obd_off                         lap_sub_offset;
        void                            *lap_sub_cookie;
        struct obd_async_page_ops       *lap_caller_ops;
        void                            *lap_caller_data;
        obd_id                          lap_loi_id;
};

#define LAP_FROM_COOKIE(c)                                                      \
        (LASSERT(((struct lov_async_page *)(c))->lap_magic == LAP_MAGIC),       \
         (struct lov_async_page *)(c))

static inline void lov_llh_addref(void *llhp)
{
        struct lov_lock_handles *llh = llhp;
        atomic_inc(&llh->llh_refcount);
        CDEBUG(D_INFO, "GETting llh %p : new refcount %d\n", llh,
               atomic_read(&llh->llh_refcount));
}

static inline struct lov_lock_handles *lov_llh_new(struct lov_stripe_md *lsm)
{
        struct lov_lock_handles *llh;

        OBD_ALLOC(llh, sizeof *llh +
                  sizeof(*llh->llh_handles) * lsm->lsm_stripe_count);
        if (llh == NULL) 
                return NULL;
        atomic_set(&llh->llh_refcount, 2);
        llh->llh_stripe_count = lsm->lsm_stripe_count;
        INIT_LIST_HEAD(&llh->llh_handle.h_link);
        class_handle_hash(&llh->llh_handle, lov_llh_addref);
        return llh;
}

static inline struct lov_lock_handles *
lov_handle2llh(struct lustre_handle *handle)
{
        LASSERT(handle != NULL);
        return(class_handle2object(handle->cookie));
}

static inline void lov_llh_put(struct lov_lock_handles *llh)
{
        CDEBUG(D_INFO, "PUTting llh %p : new refcount %d\n", llh,
               atomic_read(&llh->llh_refcount) - 1);
        LASSERT(atomic_read(&llh->llh_refcount) > 0 &&
                atomic_read(&llh->llh_refcount) < 0x5a5a);
        if (atomic_dec_and_test(&llh->llh_refcount)) {
                class_handle_unhash(&llh->llh_handle);
                LASSERT(list_empty(&llh->llh_handle.h_link));
                OBD_FREE(llh, sizeof *llh +
                         sizeof(*llh->llh_handles) * llh->llh_stripe_count);
        }
}

/* lov_merge.c */
void lov_merge_attrs(struct obdo *tgt, struct obdo *src, obd_flags valid,
                     struct lov_stripe_md *lsm, int stripeno, int *set);

int lov_adjust_kms(struct obd_export *exp, struct lov_stripe_md *lsm,
                   obd_off size, int shrink);
/* lov_offset.c */
obd_size lov_stripe_size(struct lov_stripe_md *lsm, obd_size ost_size, 
                         int stripeno);
int lov_stripe_offset(struct lov_stripe_md *lsm, obd_off lov_off,
                      int stripeno, obd_off *obd_off);
obd_off lov_size_to_stripe(struct lov_stripe_md *lsm, obd_off file_size,
                           int stripeno);
int lov_stripe_intersects(struct lov_stripe_md *lsm, int stripeno,
                          obd_off start, obd_off end,
                          obd_off *obd_start, obd_off *obd_end);
int lov_stripe_number(struct lov_stripe_md *lsm, obd_off lov_off);

/* lov_qos.c */
void qos_shrink_lsm(struct lov_request_set *set);
int qos_prep_create(struct lov_obd *lov, struct lov_request_set *set, 
                    int newea);

/* lov_request.c */
void lov_set_add_req(struct lov_request *req, struct lov_request_set *set);
int lov_update_common_set(struct lov_request_set *set, 
                          struct lov_request *req, int rc);
int lov_prep_create_set(struct obd_export *exp, struct lov_stripe_md **ea, 
                        struct obdo *src_oa, struct obd_trans_info *oti,
                        struct lov_request_set **reqset);
int lov_update_create_set(struct lov_request_set *set,
                          struct lov_request *req, int rc);
int lov_fini_create_set(struct lov_request_set *set, struct lov_stripe_md **ea);
int lov_prep_brw_set(struct obd_export *exp, struct obdo *src_oa, 
                     struct lov_stripe_md *lsm, obd_count oa_bufs,
                     struct brw_page *pga, struct obd_trans_info *oti,
                     struct lov_request_set **reqset);
int lov_fini_brw_set(struct lov_request_set *set);
int lov_prep_getattr_set(struct obd_export *exp, struct obdo *src_oa, 
                         struct lov_stripe_md *lsm, 
                         struct lov_request_set **reqset);
int lov_fini_getattr_set(struct lov_request_set *set);
int lov_prep_destroy_set(struct obd_export *exp, struct obdo *src_oa,
                         struct lov_stripe_md *lsm, 
                         struct obd_trans_info *oti, 
                         struct lov_request_set **reqset);
int lov_update_destroy_set(struct lov_request_set *set,
                           struct lov_request *req, int rc);
int lov_fini_destroy_set(struct lov_request_set *set);
int lov_prep_setattr_set(struct obd_export *exp, struct obdo *src_oa,
                         struct lov_stripe_md *lsm, struct obd_trans_info *oti,
                         struct lov_request_set **reqset);
int lov_fini_setattr_set(struct lov_request_set *set);
int lov_prep_punch_set(struct obd_export *exp, struct obdo *src_oa,
                       struct lov_stripe_md *lsm, obd_off start,
                       obd_off end, struct obd_trans_info *oti,
                       struct lov_request_set **reqset);
int lov_update_punch_set(struct lov_request_set *set, struct lov_request *req,
                         int rc);
int lov_fini_punch_set(struct lov_request_set *set);
int lov_prep_sync_set(struct obd_export *exp, struct obdo *src_oa,
                      struct lov_stripe_md *lsm, obd_off start,
                      obd_off end, struct lov_request_set **reqset);
int lov_fini_sync_set(struct lov_request_set *set);
int lov_prep_enqueue_set(struct obd_export *exp, struct lov_stripe_md *lsm, 
                         ldlm_policy_data_t *policy, __u32 mode,
                         struct lustre_handle *lockh,
                         struct lov_request_set **reqset);
int lov_update_enqueue_set(struct lov_request_set *set, 
                           struct lov_request *req, int rc, int flags);
int lov_fini_enqueue_set(struct lov_request_set *set, __u32 mode);
int lov_prep_match_set(struct obd_export *exp, struct lov_stripe_md *lsm,
                       ldlm_policy_data_t *policy, __u32 mode,
                       struct lustre_handle *lockh,
                       struct lov_request_set **reqset);
int lov_update_match_set(struct lov_request_set *set, struct lov_request *req,
                         int rc);
int lov_fini_match_set(struct lov_request_set *set, __u32 mode, int flags); 
int lov_prep_cancel_set(struct obd_export *exp, struct lov_stripe_md *lsm,
                        __u32 mode, struct lustre_handle *lockh,
                        struct lov_request_set **reqset);
int lov_fini_cancel_set(struct lov_request_set *set);

/* lov_obd.c */
int lov_get_stripecnt(struct lov_obd *lov, int stripe_count);
int lov_alloc_memmd(struct lov_stripe_md **lsmp, int stripe_count, int pattern);
void lov_free_memmd(struct lov_stripe_md **lsmp);

/* lov_log.c */
int lov_llog_init(struct obd_device *, struct obd_llogs *,
                  struct obd_device *, int, struct llog_catid *);
int lov_llog_finish(struct obd_device *, struct obd_llogs *, int);

/* lov_pack.c */
int lov_packmd(struct obd_export *exp, struct lov_mds_md **lmm,
               struct lov_stripe_md *lsm);
int lov_unpackmd(struct obd_export *exp, struct lov_stripe_md **lsmp,
                 struct lov_mds_md *lmm, int lmm_bytes);
int lov_setstripe(struct obd_export *exp,
                  struct lov_stripe_md **lsmp, struct lov_user_md *lump);
int lov_setea(struct obd_export *exp, struct lov_stripe_md **lsmp,
              struct lov_user_md *lump);
int lov_getstripe(struct obd_export *exp,
                  struct lov_stripe_md *lsm, struct lov_user_md *lump);

/* lproc_lov.c */
extern struct file_operations lov_proc_target_fops;

#endif
