/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  lustre/mdd/mdd_lov.c
 *  Lustre Metadata Server (mds) handling of striped file data
 *
 *  Copyright (C) 2001-2006 Cluster File Systems, Inc.
 *   Author: Peter Braam <braam@clusterfs.com>
 *           wangdi <wangdi@clusterfs.com>
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
#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_MDS

#include <linux/module.h>
#include <obd.h>
#include <obd_class.h>
#include <lustre_ver.h>
#include <obd_support.h>
#include <obd_lov.h>
#include <lprocfs_status.h>
#include <lustre_mds.h>
#include <lustre_fid.h>
#include <lustre/lustre_idl.h>

#include "mdd_internal.h"

static int mdd_lov_update(struct obd_device *host,
                          struct obd_device *watched,
                          enum obd_notify_event ev, void *owner)
{
        struct mdd_device *mdd = owner;
        int rc = 0;
        ENTRY;

        LASSERT(owner != NULL);

        rc = md_do_upcall(NULL, &mdd->mdd_md_dev, MD_LOV_SYNC);

        RETURN(rc);
}

/* The obd is created for handling data stack for mdd */
int mdd_init_obd(const struct lu_env *env, struct mdd_device *mdd,
                 struct lustre_cfg *cfg)
{
        char                   *dev = lustre_cfg_string(cfg, 0);
        int                     rc, name_size, uuid_size;
        char                   *name, *uuid;
        __u32                   mds_id;
        struct lustre_cfg_bufs *bufs;
        struct lustre_cfg      *lcfg;
        struct obd_device      *obd;
        ENTRY;

        mds_id = mdd2lu_dev(mdd)->ld_site->ls_node_id;
        name_size = strlen(MDD_OBD_NAME) + 35;
        uuid_size = strlen(MDD_OBD_UUID) + 35;

        OBD_ALLOC(name, name_size);
        OBD_ALLOC(uuid, uuid_size);
        if (name == NULL || uuid == NULL)
                GOTO(cleanup_mem, rc = -ENOMEM);

        OBD_ALLOC_PTR(bufs);
        if (!bufs)
                GOTO(cleanup_mem, rc = -ENOMEM);

        snprintf(name, strlen(MDD_OBD_NAME) + 35, "%s-%s-%d",
                 MDD_OBD_NAME, dev, mds_id);

        snprintf(uuid, strlen(MDD_OBD_UUID) + 35, "%s-%s-%d",
                 MDD_OBD_UUID, dev, mds_id);

        lustre_cfg_bufs_reset(bufs, name);
        lustre_cfg_bufs_set_string(bufs, 1, MDD_OBD_TYPE);
        lustre_cfg_bufs_set_string(bufs, 2, uuid);
        lustre_cfg_bufs_set_string(bufs, 3, (char*)dev/* MDD_OBD_PROFILE */);
        lustre_cfg_bufs_set_string(bufs, 4, (char*)dev);

        lcfg = lustre_cfg_new(LCFG_ATTACH, bufs);
        OBD_FREE_PTR(bufs);
        if (!lcfg)
                GOTO(cleanup_mem, rc = -ENOMEM);

        rc = class_attach(lcfg);
        if (rc)
                GOTO(lcfg_cleanup, rc);

        obd = class_name2obd(name);
        if (!obd) {
                CERROR("Can not find obd %s\n", MDD_OBD_NAME);
                LBUG();
        }

        obd->obd_recovering = 1;
        obd->u.mds.mds_id = mds_id;
        rc = class_setup(obd, lcfg);
        if (rc)
                GOTO(class_detach, rc);

        /*
         * Add here for obd notify mechanism, when adding a new ost, the mds
         * will notify this mdd.
         */
        obd->obd_upcall.onu_upcall = mdd_lov_update;
        obd->obd_upcall.onu_owner = mdd;
        mdd->mdd_obd_dev = obd;

        EXIT;
class_detach:
        if (rc)
                class_detach(obd, lcfg);
lcfg_cleanup:
        lustre_cfg_free(lcfg);
cleanup_mem:
        if (name)
                OBD_FREE(name, name_size);
        if (uuid)
                OBD_FREE(uuid, uuid_size);
        return rc;
}

int mdd_fini_obd(const struct lu_env *env, struct mdd_device *mdd,
                 struct lustre_cfg *lcfg)
{
        struct obd_device      *obd;
        int rc;
        ENTRY;

        obd = mdd2obd_dev(mdd);
        LASSERT(obd);

        rc = class_cleanup(obd, lcfg);
        if (rc)
                GOTO(lcfg_cleanup, rc);

        obd->obd_upcall.onu_upcall = NULL;
        obd->obd_upcall.onu_owner = NULL;
        rc = class_detach(obd, lcfg);
        if (rc)
                GOTO(lcfg_cleanup, rc);
        mdd->mdd_obd_dev = NULL;
        
        EXIT;
lcfg_cleanup:
        return rc;
}

int mdd_get_md(const struct lu_env *env, struct mdd_object *obj,
               void *md, int *md_size, const char *name)
{
        int rc;
        ENTRY;

        rc = mdo_xattr_get(env, obj, mdd_buf_get(env, md, *md_size), name,
                           mdd_object_capa(env, obj));
        /*
         * XXX: Handling of -ENODATA, the right way is to have ->do_md_get()
         * exported by dt layer.
         */
        if (rc == 0 || rc == -ENODATA) {
                *md_size = 0;
                rc = 0;
        } else if (rc < 0) {
                CERROR("Error %d reading eadata \n", rc);
        } else {
                /* XXX: Convert lov EA but fixed after verification test. */
                *md_size = rc;
        }

        RETURN(rc);
}

int mdd_get_md_locked(const struct lu_env *env, struct mdd_object *obj,
                      void *md, int *md_size, const char *name)
{
        int rc = 0;
        mdd_read_lock(env, obj);
        rc = mdd_get_md(env, obj, md, md_size, name);
        mdd_read_unlock(env, obj);
        return rc;
}

static int mdd_lov_set_stripe_md(const struct lu_env *env,
                                 struct mdd_object *obj, struct lu_buf *buf,
                                 struct thandle *handle)
{
        struct mdd_device       *mdd = mdo2mdd(&obj->mod_obj);
        struct obd_device       *obd = mdd2obd_dev(mdd);
        struct obd_export       *lov_exp = obd->u.mds.mds_osc_exp;
        struct lov_stripe_md    *lsm = NULL;
        int rc;
        ENTRY;

        LASSERT(S_ISDIR(mdd_object_type(obj)) || S_ISREG(mdd_object_type(obj)));
        rc = obd_iocontrol(OBD_IOC_LOV_SETSTRIPE, lov_exp, 0,
                           &lsm, buf->lb_buf);
        if (rc)
                RETURN(rc);
        obd_free_memmd(lov_exp, &lsm);

        rc = mdd_xattr_set_txn(env, obj, buf, MDS_LOV_MD_NAME, 0, handle);

        CDEBUG(D_INFO, "set lov ea of "DFID" rc %d \n", PFID(mdo2fid(obj)), rc);
        RETURN(rc);
}

/*
 * Permission check is done before call it,
 * no need check again.
 */
static int mdd_lov_set_dir_md(const struct lu_env *env,
                              struct mdd_object *obj, struct lu_buf *buf,
                              struct thandle *handle)
{
        struct lov_user_md *lum = NULL;
        int rc = 0;
        ENTRY;

        LASSERT(S_ISDIR(mdd_object_type(obj)));
        lum = (struct lov_user_md*)buf->lb_buf;

        /* if { size, offset, count } = { 0, -1, 0 } (i.e. all default
         * values specified) then delete default striping from dir. */
        if ((lum->lmm_stripe_size == 0 && lum->lmm_stripe_count == 0 &&
             lum->lmm_stripe_offset == (typeof(lum->lmm_stripe_offset))(-1)) ||
             /* lmm_stripe_size == -1 is deprecated in 1.4.6 */
             lum->lmm_stripe_size == (typeof(lum->lmm_stripe_size))(-1)){
                rc = mdd_xattr_set_txn(env, obj, &LU_BUF_NULL,
                                       MDS_LOV_MD_NAME, 0, handle);
                if (rc == -ENODATA)
                        rc = 0;
                CDEBUG(D_INFO, "delete lov ea of "DFID" rc %d \n",
                                PFID(mdo2fid(obj)), rc);
        } else {
                rc = mdd_lov_set_stripe_md(env, obj, buf, handle);
        }
        RETURN(rc);
}

int mdd_lsm_sanity_check(const struct lu_env *env,  struct mdd_object *obj)
{
        struct lu_attr   *tmp_la = &mdd_env_info(env)->mti_la;
        struct md_ucred  *uc     = md_ucred(env);
        int rc;
        ENTRY;

        rc = mdd_la_get(env, obj, tmp_la, BYPASS_CAPA);
        if (rc)
                RETURN(rc);

        if ((uc->mu_fsuid != tmp_la->la_uid) && !mdd_capable(uc, CAP_FOWNER))
                rc = mdd_permission_internal_locked(env, obj, tmp_la,
                                                    MAY_WRITE);

        RETURN(rc);
}

int mdd_lov_set_md(const struct lu_env *env, struct mdd_object *pobj,
                   struct mdd_object *child, struct lov_mds_md *lmmp,
                   int lmm_size, struct thandle *handle, int set_stripe)
{
        struct lu_buf *buf;
        umode_t mode;
        int rc = 0;
        ENTRY;

        buf = mdd_buf_get(env, lmmp, lmm_size);
        mode = mdd_object_type(child);
        if (S_ISREG(mode) && lmm_size > 0) {
                if (set_stripe) {
                        rc = mdd_lov_set_stripe_md(env, child, buf, handle);
                } else {
                        rc = mdd_xattr_set_txn(env, child, buf,
                                               MDS_LOV_MD_NAME, 0, handle);
                }
        } else if (S_ISDIR(mode)) {
                if (lmmp == NULL && lmm_size == 0) {
                        struct mdd_device *mdd = mdd_obj2mdd_dev(child);
                        struct lov_mds_md *lmm = mdd_max_lmm_get(env, mdd);
                        int size = sizeof(*lmm);

                        /* Get parent dir stripe and set */
                        if (pobj != NULL)
                                rc = mdd_get_md_locked(env, pobj, lmm, &size,
                                                       MDS_LOV_MD_NAME);
                        if (rc > 0) {
                                buf = mdd_buf_get(env, lmm, size);
                                rc = mdd_xattr_set_txn(env, child, buf,
                                               MDS_LOV_MD_NAME, 0, handle);
                                if (rc)
                                        CERROR("error on copy stripe info: rc "
                                                "= %d\n", rc);
                        }
                } else {
                        LASSERT(lmmp != NULL && lmm_size > 0);
                        rc = mdd_lov_set_dir_md(env, child, buf, handle);
                }
        }
        CDEBUG(D_INFO, "Set lov md %p size %d for fid "DFID" rc %d\n",
                        lmmp, lmm_size, PFID(mdo2fid(child)), rc);
        RETURN(rc);
}

/*
 * XXX: this is for create lsm object id, which should identify the lsm object
 * unique in the whole mds, as I see. But it seems, we still not need it
 * now. Right? So just borrow the ll_fid_build_ino().
 */
static obd_id mdd_lov_create_id(const struct lu_fid *fid)
{
        return fid_flatten(fid);
}

static int mdd_lov_objid_alloc(const struct lu_env *env,
                               struct mdd_device *mdd)
{
        struct mdd_thread_info *info = mdd_env_info(env);
        struct mds_obd *mds = &mdd->mdd_obd_dev->u.mds;

        OBD_ALLOC(info->mti_oti.oti_objid,
                  mds->mds_lov_desc.ld_tgt_count * sizeof(obd_id));
        return (info->mti_oti.oti_objid == NULL ? -ENOMEM : 0);
}

void mdd_lov_objid_update(const struct lu_env *env, struct mdd_device *mdd)
{
        struct mdd_thread_info *info = mdd_env_info(env);
        if (info->mti_oti.oti_objid != NULL)
                mds_lov_update_objids(mdd->mdd_obd_dev,
                                      info->mti_oti.oti_objid);
}

static void mdd_lov_objid_from_lmm(const struct lu_env *env,
                                   struct mdd_device *mdd,
                                   struct lov_mds_md *lmm)
{
        struct mds_obd *mds = &mdd->mdd_obd_dev->u.mds;
        struct mdd_thread_info *info = mdd_env_info(env);
        mds_objids_from_lmm(info->mti_oti.oti_objid, lmm, &mds->mds_lov_desc);
}

static void mdd_lov_objid_free(const struct lu_env *env,
                               struct mdd_device *mdd)
{
        struct mdd_thread_info *info = mdd_env_info(env);
        struct mds_obd *mds = &mdd->mdd_obd_dev->u.mds;

        OBD_FREE(info->mti_oti.oti_objid,
                 mds->mds_lov_desc.ld_tgt_count * sizeof(obd_id));
        info->mti_oti.oti_objid = NULL;
}

void mdd_lov_create_finish(const struct lu_env *env, struct mdd_device *mdd,
                           struct lov_mds_md *lmm, int lmm_size,
                           const struct md_op_spec *spec)
{
        struct mdd_thread_info *info = mdd_env_info(env);

        if (lmm && !spec->u.sp_ea.no_lov_create)
                OBD_FREE(lmm, lmm_size);

        if (info->mti_oti.oti_objid != NULL)
                mdd_lov_objid_free(env, mdd);
}

int mdd_lov_create(const struct lu_env *env, struct mdd_device *mdd,
                   struct mdd_object *parent, struct mdd_object *child,
                   struct lov_mds_md **lmm, int *lmm_size,
                   const struct md_op_spec *spec, struct lu_attr *la)
{
        struct obd_device     *obd = mdd2obd_dev(mdd);
        struct obd_export     *lov_exp = obd->u.mds.mds_osc_exp;
        struct obdo           *oa;
        struct lov_stripe_md  *lsm = NULL;
        const void            *eadata = spec->u.sp_ea.eadata;
        __u32                  create_flags = spec->sp_cr_flags;
        struct obd_trans_info *oti = &mdd_env_info(env)->mti_oti;
        int                    rc = 0;
        ENTRY;

        if (create_flags & MDS_OPEN_DELAY_CREATE ||
            !(create_flags & FMODE_WRITE))
                RETURN(0);

        oti_init(oti, NULL);
        rc = mdd_lov_objid_alloc(env, mdd);
        if (rc != 0)
                RETURN(rc);

        /* replay case, has objects already, only get lov from eadata */
        if (spec->u.sp_ea.no_lov_create != 0) {
                *lmm = (struct lov_mds_md *)spec->u.sp_ea.eadata;
                *lmm_size = spec->u.sp_ea.eadatalen;
                mdd_lov_objid_from_lmm(env, mdd, *lmm);
                RETURN(0);
        }

        if (OBD_FAIL_CHECK_ONCE(OBD_FAIL_MDS_ALLOC_OBDO))
                GOTO(out_ids, rc = -ENOMEM);

        LASSERT(lov_exp != NULL);
        oa = &mdd_env_info(env)->mti_oa;

        oa->o_uid = 0; /* must have 0 uid / gid on OST */
        oa->o_gid = 0;
        oa->o_gr = FILTER_GROUP_MDS0 + mdd2lu_dev(mdd)->ld_site->ls_node_id;
        oa->o_mode = S_IFREG | 0600;
        oa->o_id = mdd_lov_create_id(mdd_object_fid(child));
        oa->o_valid = OBD_MD_FLID | OBD_MD_FLTYPE | OBD_MD_FLFLAGS |
                OBD_MD_FLMODE | OBD_MD_FLUID | OBD_MD_FLGID | OBD_MD_FLGROUP;
        oa->o_size = 0;

        if (!(create_flags & MDS_OPEN_HAS_OBJS)) {
                if (create_flags & MDS_OPEN_HAS_EA) {
                        LASSERT(eadata != NULL);
                        rc = obd_iocontrol(OBD_IOC_LOV_SETSTRIPE, lov_exp,
                                           0, &lsm, (void*)eadata);
                        if (rc)
                                GOTO(out_oti, rc);
                        lsm->lsm_object_id = oa->o_id;
                        lsm->lsm_object_gr = oa->o_gr;
                } else if (parent != NULL) {
                        /* get lov ea from parent and set to lov */
                        struct lov_mds_md *_lmm;
                        int _lmm_size;

                        _lmm_size = mdd_lov_mdsize(env, mdd);
                        _lmm = mdd_max_lmm_get(env, mdd);

                        if (_lmm == NULL)
                                GOTO(out_oti, rc = -ENOMEM);

                        rc = mdd_get_md_locked(env, parent, _lmm,
                                               &_lmm_size,
                                               MDS_LOV_MD_NAME);
                        if (rc > 0)
                                rc = obd_iocontrol(OBD_IOC_LOV_SETSTRIPE,
                                                   lov_exp, 0, &lsm, _lmm);
                        if (rc)
                                GOTO(out_oti, rc);
                }

                rc = obd_create(lov_exp, oa, &lsm, oti);
                if (rc) {
                        if (rc > 0) {
                                CERROR("Create error for "DFID": %d\n",
                                       PFID(mdo2fid(child)), rc);
                                rc = -EIO;
                        }
                        GOTO(out_oti, rc);
                }
                LASSERT(lsm->lsm_object_gr >= FILTER_GROUP_MDS0);
        } else {
                LASSERT(eadata != NULL);
                rc = obd_iocontrol(OBD_IOC_LOV_SETEA, lov_exp, 0, &lsm,
                                   (void*)eadata);
                if (rc)
                        GOTO(out_oti, rc);
                lsm->lsm_object_id = oa->o_id;
                lsm->lsm_object_gr = oa->o_gr;
        }

        /*
         * Sometimes, we may truncate some object(without lsm) then open it
         * (with write flags), so creating lsm above.  The Nonzero(truncated)
         * size should tell ost, since size attr is in charge by OST.
         */
        if (la->la_size && la->la_valid & LA_SIZE) {
                struct obd_info *oinfo = &mdd_env_info(env)->mti_oi;

                memset(oinfo, 0, sizeof(*oinfo));

                oa->o_size = la->la_size;

                /* When setting attr to ost, FLBKSZ is not needed. */
                oa->o_valid &= ~OBD_MD_FLBLKSZ;
                obdo_from_la(oa, la, OBD_MD_FLTYPE | OBD_MD_FLATIME |
                             OBD_MD_FLMTIME | OBD_MD_FLCTIME | OBD_MD_FLSIZE);

                /*
                 * XXX: Pack lustre id to OST, in OST, it will be packed by
                 * filter_fid, but can not see what is the usages. So just pack
                 * o_seq o_ver here, maybe fix it after this cycle.
                 */
                oa->o_fid = fid_seq(mdd_object_fid(child));
                oa->o_generation = fid_oid(mdd_object_fid(child));
                oa->o_valid |= OBD_MD_FLFID | OBD_MD_FLGENER;
                oinfo->oi_oa = oa;
                oinfo->oi_md = lsm;
                oinfo->oi_capa = mdo_capa_get(env, child, NULL, 0,
                                              CAPA_OPC_MDS_DEFAULT);
                if (IS_ERR(oinfo->oi_capa))
                        oinfo->oi_capa = NULL;

                rc = obd_setattr(lov_exp, oinfo, oti);
                capa_put(oinfo->oi_capa);
                if (rc) {
                        CERROR("Error setting attrs for "DFID": rc %d\n",
                               PFID(mdo2fid(child)), rc);
                        if (rc > 0) {
                                CERROR("obd_setattr for "DFID" rc %d\n",
                                        PFID(mdo2fid(child)), rc);
                                rc = -EIO;
                        }
                        GOTO(out_oti, rc);
                }
        }

        /* blksize should be changed after create data object */
        la->la_valid |= LA_BLKSIZE;
        la->la_blksize = oa->o_blksize;
        *lmm = NULL;
        rc = obd_packmd(lov_exp, lmm, lsm);
        if (rc < 0) {
                CERROR("Cannot pack lsm, err = %d\n", rc);
                GOTO(out_oti, rc);
        }
        *lmm_size = rc;
        rc = 0;
        EXIT;
out_oti:
        oti_free_cookies(oti);
out_ids:
        if (lsm)
                obd_free_memmd(lov_exp, &lsm);
        if (rc != 0)
                mdd_lov_objid_free(env, mdd);

        return rc;
}


/*
 * used when destroying orphans and from mds_reint_unlink() when MDS wants to
 * destroy objects on OSS.
 */
static
int mdd_lovobj_unlink(const struct lu_env *env, struct mdd_device *mdd,
                      struct mdd_object *obj, struct lu_attr *la,
                      struct lov_mds_md *lmm, int lmm_size,
                      struct llog_cookie *logcookies,
                      int log_unlink)
{
        struct obd_device     *obd = mdd2obd_dev(mdd);
        struct obd_export     *lov_exp = obd->u.mds.mds_osc_exp;
        struct lov_stripe_md  *lsm = NULL;
        struct obd_trans_info *oti = &mdd_env_info(env)->mti_oti;
        struct obdo           *oa = &mdd_env_info(env)->mti_oa;
        int rc;
        ENTRY;

        if (lmm_size == 0)
                RETURN(0);

        rc = obd_unpackmd(lov_exp, &lsm, lmm, lmm_size);
        if (rc < 0) {
                CERROR("Error unpack md %p\n", lmm);
                RETURN(rc);
        } else {
                LASSERT(rc >= sizeof(*lsm));
                rc = 0;
        }

        oa->o_id = lsm->lsm_object_id;
        oa->o_gr = FILTER_GROUP_MDS0 + mdd2lu_dev(mdd)->ld_site->ls_node_id;
        oa->o_mode = la->la_mode & S_IFMT;
        oa->o_valid = OBD_MD_FLID | OBD_MD_FLTYPE | OBD_MD_FLGROUP;

        oti_init(oti, NULL);
        if (log_unlink && logcookies) {
                oa->o_valid |= OBD_MD_FLCOOKIE;
                oti->oti_logcookies = logcookies;
        }

        CDEBUG(D_INFO, "destroying OSS object %d/%d\n",
                        (int)oa->o_id, (int)oa->o_gr);

        rc = obd_destroy(lov_exp, oa, lsm, oti, NULL);

        obd_free_memmd(lov_exp, &lsm);
        RETURN(rc);
}


/*
 * called with obj not locked. 
 */
int mdd_lov_destroy(const struct lu_env *env, struct mdd_device *mdd,
                    struct mdd_object *obj, struct lu_attr *la)
{
        struct md_attr    *ma = &mdd_env_info(env)->mti_ma;
        int                rc;
        ENTRY;

        if (unlikely(la->la_nlink != 0)) {
                CWARN("Attempt to destroy OSS object when nlink == %d\n",
                      la->la_nlink);
                RETURN(0);
        }

        ma->ma_lmm_size = mdd_lov_mdsize(env, mdd);
        ma->ma_lmm = mdd_max_lmm_get(env, mdd);
        ma->ma_cookie_size = mdd_lov_cookiesize(env, mdd);
        ma->ma_cookie = mdd_max_cookie_get(env, mdd);
        if (ma->ma_lmm == NULL || ma->ma_cookie == NULL)
                RETURN(rc = -ENOMEM);

        /* get lov ea */
        rc = mdd_get_md_locked(env, obj, ma->ma_lmm, &ma->ma_lmm_size,
                               MDS_LOV_MD_NAME);
        if (rc) {
                CWARN("Get lov ea failed for "DFID"\n", PFID(mdo2fid(obj)));
                RETURN(rc);
        }
        ma->ma_valid = MA_LOV;
        
        rc = mdd_unlink_log(env, mdd, obj, ma);
        if (rc) {
                CWARN("mds unlink log for "DFID" failed: %d\n",
                       PFID(mdo2fid(obj)), rc);
                RETURN(rc);
        }
        if (ma->ma_valid | MA_COOKIE)
                rc = mdd_lovobj_unlink(env, mdd, obj, la, 
                                       ma->ma_lmm, ma->ma_lmm_size,
                                       ma->ma_cookie, 1);
        RETURN(rc);
}


int mdd_unlink_log(const struct lu_env *env, struct mdd_device *mdd,
                   struct mdd_object *mdd_cobj, struct md_attr *ma)
{
        struct obd_device *obd = mdd2obd_dev(mdd);

        LASSERT(ma->ma_valid & MA_LOV);

        if ((ma->ma_cookie_size > 0) &&
            (mds_log_op_unlink(obd, ma->ma_lmm, ma->ma_lmm_size,
                               ma->ma_cookie, ma->ma_cookie_size) > 0)) {
                ma->ma_valid |= MA_COOKIE;
        }
        return 0;
}

int mdd_setattr_log(const struct lu_env *env, struct mdd_device *mdd,
                    const struct md_attr *ma,
                    struct lov_mds_md *lmm, int lmm_size,
                    struct llog_cookie *logcookies, int cookies_size)
{
        struct obd_device *obd = mdd2obd_dev(mdd);

        /* journal chown/chgrp in llog, just like unlink */
        if (lmm_size > 0) {
                CDEBUG(D_INFO, "setattr llog for uid/gid=%lu/%lu\n",
                        (unsigned long)ma->ma_attr.la_uid, 
                        (unsigned long)ma->ma_attr.la_gid);
                return mds_log_op_setattr(obd, ma->ma_attr.la_uid, 
                                          ma->ma_attr.la_gid, lmm, 
                                          lmm_size, logcookies,
                                          cookies_size);
        } else
                return 0;
}

int mdd_lov_setattr_async(const struct lu_env *env, struct mdd_object *obj,
                          struct lov_mds_md *lmm, int lmm_size, 
                          struct llog_cookie *logcookies)
{
        struct mdd_device   *mdd = mdo2mdd(&obj->mod_obj);
        struct obd_device   *obd = mdd2obd_dev(mdd);
        struct lu_attr      *tmp_la = &mdd_env_info(env)->mti_la;
        const struct lu_fid *fid = mdd_object_fid(obj);
        struct obd_capa     *oc;
        int rc = 0;
        ENTRY;

        mdd_read_lock(env, obj);
        rc = mdo_attr_get(env, obj, tmp_la, mdd_object_capa(env, obj));
        mdd_read_unlock(env, obj);
        if (rc)
                RETURN(rc);

        oc = mdo_capa_get(env, obj, NULL, 0, CAPA_OPC_MDS_DEFAULT);
        if (IS_ERR(oc))
                oc = NULL;

        rc = mds_osc_setattr_async(obd, tmp_la->la_uid, tmp_la->la_gid, lmm,
                                   lmm_size, logcookies, fid_seq(fid),
                                   fid_oid(fid), oc);

        capa_put(oc);

        RETURN(rc);
}

