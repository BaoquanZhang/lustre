/* -*- MODE: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  mdd/mdd_handler.c
 *  Lustre Metadata Server (mdd) routines
 *
 *  Copyright (C) 2006 Cluster File Systems, Inc.
 *   Author: Wang Di <wangdi@clusterfs.com>
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
#include <linux/jbd.h>
#include <obd.h>
#include <obd_class.h>
#include <lustre_ver.h>
#include <obd_support.h>
#include <lprocfs_status.h>
/* fid_be_cpu(), fid_cpu_to_be(). */
#include <lustre_fid.h>

#include <linux/ldiskfs_fs.h>
#include <lustre_mds.h>
#include <lustre/lustre_idl.h>

#include "mdd_internal.h"

static struct lu_object_operations mdd_lu_obj_ops;

int mdd_la_get(const struct lu_env *env, struct mdd_object *obj,
               struct lu_attr *la, struct lustre_capa *capa)
{
        struct dt_object *next = mdd_object_child(obj);
        LASSERTF(lu_object_exists(mdd2lu_obj(obj)), "FID is "DFID"\n",
                 PFID(lu_object_fid(mdd2lu_obj(obj))));
        return next->do_ops->do_attr_get(env, next, la, capa);
}

static void mdd_flags_xlate(struct mdd_object *obj, __u32 flags)
{
        obj->mod_flags &= ~(APPEND_OBJ|IMMUTE_OBJ);

        if (flags & LUSTRE_APPEND_FL)
                obj->mod_flags |= APPEND_OBJ;

        if (flags & LUSTRE_IMMUTABLE_FL)
                obj->mod_flags |= IMMUTE_OBJ;
}

struct lu_buf *mdd_buf_get(const struct lu_env *env, void *area, ssize_t len)
{
        struct lu_buf *buf;

        buf = &mdd_env_info(env)->mti_buf;
        buf->lb_buf = area;
        buf->lb_len = len;
        return buf;
}

const struct lu_buf *mdd_buf_get_const(const struct lu_env *env,
                                       const void *area, ssize_t len)
{
        struct lu_buf *buf;

        buf = &mdd_env_info(env)->mti_buf;
        buf->lb_buf = (void *)area;
        buf->lb_len = len;
        return buf;
}

struct mdd_thread_info *mdd_env_info(const struct lu_env *env)
{
        struct mdd_thread_info *info;

        info = lu_context_key_get(&env->le_ctx, &mdd_thread_key);
        LASSERT(info != NULL);
        return info;
}

struct lu_object *mdd_object_alloc(const struct lu_env *env,
                                   const struct lu_object_header *hdr,
                                   struct lu_device *d)
{
        struct mdd_object *mdd_obj;

        OBD_ALLOC_PTR(mdd_obj);
        if (mdd_obj != NULL) {
                struct lu_object *o;

                o = mdd2lu_obj(mdd_obj);
                lu_object_init(o, NULL, d);
                mdd_obj->mod_obj.mo_ops = &mdd_obj_ops;
                mdd_obj->mod_obj.mo_dir_ops = &mdd_dir_ops;
                mdd_obj->mod_count = 0;
                o->lo_ops = &mdd_lu_obj_ops;
                return o;
        } else {
                return NULL;
        }
}

static int mdd_object_init(const struct lu_env *env, struct lu_object *o)
{
	struct mdd_device *d = lu2mdd_dev(o->lo_dev);
	struct lu_object  *below;
        struct lu_device  *under;
        ENTRY;

	under = &d->mdd_child->dd_lu_dev;
	below = under->ld_ops->ldo_object_alloc(env, o->lo_header, under);
        mdd_pdlock_init(lu2mdd_obj(o));
        if (below == NULL)
		RETURN(-ENOMEM);

        lu_object_add(o, below);
        RETURN(0);
}

static int mdd_object_start(const struct lu_env *env, struct lu_object *o)
{
        if (lu_object_exists(o))
                return mdd_get_flags(env, lu2mdd_obj(o));
        else
                return 0;
}

static void mdd_object_free(const struct lu_env *env, struct lu_object *o)
{
        struct mdd_object *mdd = lu2mdd_obj(o);
	
        lu_object_fini(o);
        OBD_FREE_PTR(mdd);
}

static int mdd_object_print(const struct lu_env *env, void *cookie,
                            lu_printer_t p, const struct lu_object *o)
{
        return (*p)(env, cookie, LUSTRE_MDD_NAME"-object@%p", o);
}

/* orphan handling is here */
static void mdd_object_delete(const struct lu_env *env,
                               struct lu_object *o)
{
        struct mdd_object *mdd_obj = lu2mdd_obj(o);
        struct thandle *handle = NULL;
        ENTRY;

        if (lu2mdd_dev(o->lo_dev)->mdd_orphans == NULL)
                return;

        if (mdd_obj->mod_flags & ORPHAN_OBJ) {
                mdd_txn_param_build(env, lu2mdd_dev(o->lo_dev),
                                    MDD_TXN_INDEX_DELETE_OP);
                handle = mdd_trans_start(env, lu2mdd_dev(o->lo_dev));
                if (IS_ERR(handle))
                        CERROR("Cannot get thandle\n");
                else {
                        mdd_write_lock(env, mdd_obj);
                        /* let's remove obj from the orphan list */
                        __mdd_orphan_del(env, mdd_obj, handle);
                        mdd_write_unlock(env, mdd_obj);
                        mdd_trans_stop(env, lu2mdd_dev(o->lo_dev),
                                       0, handle);
                }
        }
}

static struct lu_object_operations mdd_lu_obj_ops = {
	.loo_object_init    = mdd_object_init,
	.loo_object_start   = mdd_object_start,
	.loo_object_free    = mdd_object_free,
	.loo_object_print   = mdd_object_print,
        .loo_object_delete  = mdd_object_delete
};

struct mdd_object *mdd_object_find(const struct lu_env *env,
                                   struct mdd_device *d,
                                   const struct lu_fid *f)
{
        struct lu_object *o, *lo;
        struct mdd_object *m;
        ENTRY;

        o = lu_object_find(env, mdd2lu_dev(d)->ld_site, f);
        if (IS_ERR(o))
                m = (struct mdd_object *)o;
        else {
                lo = lu_object_locate(o->lo_header, mdd2lu_dev(d)->ld_type);
                /* remote object can't be located and should be put then */
                if (lo == NULL)
                        lu_object_put(env, o);
                m = lu2mdd_obj(lo);
        }
        RETURN(m);
}

int mdd_get_flags(const struct lu_env *env, struct mdd_object *obj)
{
        struct lu_attr *la = &mdd_env_info(env)->mti_la;
        int rc;

        ENTRY;
        rc = mdd_la_get(env, obj, la, BYPASS_CAPA);
        if (rc == 0)
                mdd_flags_xlate(obj, la->la_flags);
        RETURN(rc);
}

void mdd_ref_add_internal(const struct lu_env *env, struct mdd_object *obj,
                          struct thandle *handle)
{
        struct dt_object *next;

        LASSERT(lu_object_exists(mdd2lu_obj(obj)));
        next = mdd_object_child(obj);
        next->do_ops->do_ref_add(env, next, handle);
}

void mdd_ref_del_internal(const struct lu_env *env, struct mdd_object *obj,
                          struct thandle *handle)
{
        struct dt_object *next = mdd_object_child(obj);
        ENTRY;

        LASSERT(lu_object_exists(mdd2lu_obj(obj)));

        next->do_ops->do_ref_del(env, next, handle);
        EXIT;
}

/* get only inode attributes */
int mdd_iattr_get(const struct lu_env *env, struct mdd_object *mdd_obj,
                  struct md_attr *ma)
{
        int rc = 0;
        ENTRY;

        if (ma->ma_valid & MA_INODE)
                RETURN(0);

        rc = mdd_la_get(env, mdd_obj, &ma->ma_attr,
                          mdd_object_capa(env, mdd_obj));
        if (rc == 0)
                ma->ma_valid |= MA_INODE;
        RETURN(rc);
}

/* get lov EA only */
static int __mdd_lmm_get(const struct lu_env *env,
                         struct mdd_object *mdd_obj, struct md_attr *ma)
{
        int rc, lmm_size;
        ENTRY;

        if (ma->ma_valid & MA_LOV)
                RETURN(0);

        lmm_size = ma->ma_lmm_size;
        rc = mdd_get_md(env, mdd_obj, ma->ma_lmm, &lmm_size,
                        MDS_LOV_MD_NAME);
        if (rc > 0) {
                ma->ma_valid |= MA_LOV;
                ma->ma_lmm_size = lmm_size;
                rc = 0;
        }
        RETURN(rc);
}

int mdd_lmm_get_locked(const struct lu_env *env, struct mdd_object *mdd_obj,
                       struct md_attr *ma)
{
        int rc;
        ENTRY;

        mdd_read_lock(env, mdd_obj);
        rc = __mdd_lmm_get(env, mdd_obj, ma);
        mdd_read_unlock(env, mdd_obj);
        RETURN(rc);
}

/* get lmv EA only*/
static int __mdd_lmv_get(const struct lu_env *env,
                         struct mdd_object *mdd_obj, struct md_attr *ma)
{
        int rc;

        if (ma->ma_valid & MA_LMV)
                RETURN(0);

        rc = mdd_get_md(env, mdd_obj, ma->ma_lmv, &ma->ma_lmv_size,
                        MDS_LMV_MD_NAME);
        if (rc > 0) {
                ma->ma_valid |= MA_LMV;
                rc = 0;
        }
        RETURN(rc);
}

static int mdd_attr_get_internal(const struct lu_env *env,
                                 struct mdd_object *mdd_obj,
                                 struct md_attr *ma)
{
        struct mdd_device *mdd = mdo2mdd(&mdd_obj->mod_obj);
        struct timeval  start;
        int rc = 0;
        ENTRY;

        mdd_lprocfs_time_start(mdd, &start, LPROC_MDD_ATTR_GET);
        if (ma->ma_need & MA_INODE)
                rc = mdd_iattr_get(env, mdd_obj, ma);

        if (rc == 0 && ma->ma_need & MA_LOV) {
                if (S_ISREG(mdd_object_type(mdd_obj)) ||
                    S_ISDIR(mdd_object_type(mdd_obj)))
                        rc = __mdd_lmm_get(env, mdd_obj, ma);
        }
        if (rc == 0 && ma->ma_need & MA_LMV) {
                if (S_ISDIR(mdd_object_type(mdd_obj)))
                        rc = __mdd_lmv_get(env, mdd_obj, ma);
        }
#ifdef CONFIG_FS_POSIX_ACL
        if (rc == 0 && ma->ma_need & MA_ACL_DEF) {
                if (S_ISDIR(mdd_object_type(mdd_obj)))
                        rc = mdd_acl_def_get(env, mdd_obj, ma);
        }
#endif
        CDEBUG(D_INODE, "after getattr rc = %d, ma_valid = "LPX64"\n",
               rc, ma->ma_valid);
        mdd_lprocfs_time_end(mdd, &start, LPROC_MDD_ATTR_GET);
        RETURN(rc);
}

int mdd_attr_get_internal_locked(const struct lu_env *env,
                                 struct mdd_object *mdd_obj, struct md_attr *ma)
{
        int rc;
        int needlock = ma->ma_need & (MA_LOV | MA_LMV | MA_ACL_DEF);

        if (needlock)
                mdd_read_lock(env, mdd_obj);
        rc = mdd_attr_get_internal(env, mdd_obj, ma);
        if (needlock)
                mdd_read_unlock(env, mdd_obj);
        return rc;
}

/*
 * No permission check is needed.
 */
static int mdd_attr_get(const struct lu_env *env, struct md_object *obj,
                        struct md_attr *ma)
{
        struct mdd_object *mdd_obj = md2mdd_obj(obj);
        int                rc;

        ENTRY;
        rc = mdd_attr_get_internal_locked(env, mdd_obj, ma);
        RETURN(rc);
}

/*
 * No permission check is needed.
 */
static int mdd_xattr_get(const struct lu_env *env,
                         struct md_object *obj, struct lu_buf *buf,
                         const char *name)
{
        struct mdd_object *mdd_obj = md2mdd_obj(obj);
        struct dt_object  *next;
        int rc;

        ENTRY;

        LASSERT(lu_object_exists(&obj->mo_lu));

        next = mdd_object_child(mdd_obj);
        mdd_read_lock(env, mdd_obj);
        rc = next->do_ops->do_xattr_get(env, next, buf, name,
                                        mdd_object_capa(env, mdd_obj));
        mdd_read_unlock(env, mdd_obj);

        RETURN(rc);
}

/*
 * Permission check is done when open,
 * no need check again.
 */
static int mdd_readlink(const struct lu_env *env, struct md_object *obj,
                        struct lu_buf *buf)
{
        struct mdd_object *mdd_obj = md2mdd_obj(obj);
        struct dt_object  *next;
        loff_t             pos = 0;
        int                rc;
        ENTRY;

        LASSERT(lu_object_exists(&obj->mo_lu));

        next = mdd_object_child(mdd_obj);
        mdd_read_lock(env, mdd_obj);
        rc = next->do_body_ops->dbo_read(env, next, buf, &pos,
                                         mdd_object_capa(env, mdd_obj));
        mdd_read_unlock(env, mdd_obj);
        RETURN(rc);
}

static int mdd_xattr_list(const struct lu_env *env, struct md_object *obj,
                          struct lu_buf *buf)
{
        struct mdd_object *mdd_obj = md2mdd_obj(obj);
        struct dt_object  *next;
        int rc;

        ENTRY;
        LASSERT(lu_object_exists(&obj->mo_lu));

        next = mdd_object_child(mdd_obj);
        mdd_read_lock(env, mdd_obj);
        rc = next->do_ops->do_xattr_list(env, next, buf,
                                         mdd_object_capa(env, mdd_obj));
        mdd_read_unlock(env, mdd_obj);

        RETURN(rc);
}

int mdd_object_create_internal(const struct lu_env *env,
                               struct mdd_object *obj, struct md_attr *ma,
                               struct thandle *handle)
{
        struct mdd_device *mdd = mdo2mdd(&obj->mod_obj);
        struct dt_object *next;
        struct lu_attr *attr = &ma->ma_attr;
        struct timeval  start;
        int rc;
        ENTRY;

        mdd_lprocfs_time_start(mdd, &start, LPROC_MDD_CREATE_OBJ);

        if (!lu_object_exists(mdd2lu_obj(obj))) {
                next = mdd_object_child(obj);
                rc = next->do_ops->do_create(env, next, attr, handle);
                LASSERT(ergo(rc == 0, lu_object_exists(mdd2lu_obj(obj))));
        } else
                rc = -EEXIST;

        mdd_lprocfs_time_end(mdd, &start, LPROC_MDD_CREATE_OBJ);
        RETURN(rc);
}


int mdd_attr_set_internal(const struct lu_env *env, struct mdd_object *o,
                          const struct lu_attr *attr, struct thandle *handle,
                          const int needacl)
{
        struct mdd_device *mdd = mdo2mdd(&o->mod_obj);
        struct dt_object *next;
        struct timeval  start;
        int rc;
        ENTRY;

        mdd_lprocfs_time_start(mdd, &start, LPROC_MDD_ATTR_SET);
        LASSERT(lu_object_exists(mdd2lu_obj(o)));
        next = mdd_object_child(o);
        rc = next->do_ops->do_attr_set(env, next, attr, handle,
                                       mdd_object_capa(env, o));
#ifdef CONFIG_FS_POSIX_ACL
        if (!rc && (attr->la_valid & LA_MODE) && needacl)
                rc = mdd_acl_chmod(env, o, attr->la_mode, handle);
#endif
        mdd_lprocfs_time_end(mdd, &start, LPROC_MDD_ATTR_SET);
        RETURN(rc);
}

int mdd_attr_set_internal_locked(const struct lu_env *env,
                                 struct mdd_object *o,
                                 const struct lu_attr *attr,
                                 struct thandle *handle, int needacl)
{
        int rc;
        ENTRY;

        needacl = needacl && (attr->la_valid & LA_MODE);

        if (needacl)
                mdd_write_lock(env, o);

        rc = mdd_attr_set_internal(env, o, attr, handle, needacl);

        if (needacl)
                mdd_write_unlock(env, o);
        RETURN(rc);
}

static int __mdd_xattr_set(const struct lu_env *env, struct mdd_object *o,
                           const struct lu_buf *buf, const char *name,
                           int fl, struct thandle *handle)
{
        struct dt_object *next;
        struct lustre_capa *capa = mdd_object_capa(env, o);
        int rc = 0;
        ENTRY;

        LASSERT(lu_object_exists(mdd2lu_obj(o)));
        next = mdd_object_child(o);
        if (buf->lb_buf && buf->lb_len > 0) {
                rc = next->do_ops->do_xattr_set(env, next, buf, name, 0, handle,
                                                capa);
        } else if (buf->lb_buf == NULL && buf->lb_len == 0) {
                rc = next->do_ops->do_xattr_del(env, next, name, handle, capa);
        }
        RETURN(rc);
}

/*
 * This gives the same functionality as the code between
 * sys_chmod and inode_setattr
 * chown_common and inode_setattr
 * utimes and inode_setattr
 * This API is ported from mds_fix_attr but remove some unnecesssary stuff.
 */
static int mdd_fix_attr(const struct lu_env *env, struct mdd_object *obj,
                        struct lu_attr *la)
{
        struct lu_attr   *tmp_la     = &mdd_env_info(env)->mti_la;
        struct md_ucred  *uc         = md_ucred(env);
        time_t            now        = CURRENT_SECONDS;
        int               rc;
        ENTRY;

        if (!la->la_valid)
                RETURN(0);

        /* Do not permit change file type */
        if (la->la_valid & LA_TYPE)
                RETURN(-EPERM);

        /* They should not be processed by setattr */
        if (la->la_valid & (LA_NLINK | LA_RDEV | LA_BLKSIZE))
                RETURN(-EPERM);

        rc = mdd_la_get(env, obj, tmp_la, BYPASS_CAPA);
        if (rc)
                RETURN(rc);

        if (mdd_is_immutable(obj) || mdd_is_append(obj)) {

                /*
                 * If only change flags of the object, we should
                 * let it pass, but also need capability check
                 * here if (!capable(CAP_LINUX_IMMUTABLE)),
                 * fix it, when implement capable in mds
                 */
                if (la->la_valid & ~LA_FLAGS)
                        RETURN(-EPERM);

                if (!mdd_capable(uc, CAP_LINUX_IMMUTABLE))
                        RETURN(-EPERM);

                if ((uc->mu_fsuid != tmp_la->la_uid) &&
                    !mdd_capable(uc, CAP_FOWNER))
                        RETURN(-EPERM);

                /*
                 * According to Ext3 implementation on this,
                 * the ctime will be changed, but not clear why?
                 */
                la->la_ctime = now;
                la->la_valid |= LA_CTIME;
                RETURN(0);
        }

        /* Check for setting the obj time. */
        if ((la->la_valid & (LA_MTIME | LA_ATIME | LA_CTIME)) &&
            !(la->la_valid & ~(LA_MTIME | LA_ATIME | LA_CTIME))) {
                if ((uc->mu_fsuid != tmp_la->la_uid) &&
                    !mdd_capable(uc, CAP_FOWNER)) {
                        rc = mdd_permission_internal_locked(env, obj, tmp_la,
                                                            MAY_WRITE);
                        if (rc)
                                RETURN(rc);
                }
        }

        /* Make sure a caller can chmod. */
        if (la->la_valid & LA_MODE) {
                /*
                 * Bypass la_vaild == LA_MODE,
                 * this is for changing file with SUID or SGID.
                 */
                if ((la->la_valid & ~LA_MODE) &&
                    (uc->mu_fsuid != tmp_la->la_uid) &&
                    !mdd_capable(uc, CAP_FOWNER))
                        RETURN(-EPERM);

                if (la->la_mode == (umode_t) -1)
                        la->la_mode = tmp_la->la_mode;
                else
                        la->la_mode = (la->la_mode & S_IALLUGO) |
                                      (tmp_la->la_mode & ~S_IALLUGO);

                /* Also check the setgid bit! */
                if (!mdd_in_group_p(uc, (la->la_valid & LA_GID) ? la->la_gid :
                                tmp_la->la_gid) && !mdd_capable(uc, CAP_FSETID))
                        la->la_mode &= ~S_ISGID;
        } else {
               la->la_mode = tmp_la->la_mode;
        }

        /* Make sure a caller can chown. */
        if (la->la_valid & LA_UID) {
                if (la->la_uid == (uid_t) -1)
                        la->la_uid = tmp_la->la_uid;
                if (((uc->mu_fsuid != tmp_la->la_uid) ||
                    (la->la_uid != tmp_la->la_uid)) &&
                    !mdd_capable(uc, CAP_CHOWN))
                        RETURN(-EPERM);

                /*
                 * If the user or group of a non-directory has been
                 * changed by a non-root user, remove the setuid bit.
                 * 19981026 David C Niemi <niemi@tux.org>
                 *
                 * Changed this to apply to all users, including root,
                 * to avoid some races. This is the behavior we had in
                 * 2.0. The check for non-root was definitely wrong
                 * for 2.2 anyway, as it should have been using
                 * CAP_FSETID rather than fsuid -- 19990830 SD.
                 */
                if (((tmp_la->la_mode & S_ISUID) == S_ISUID) &&
                    !S_ISDIR(tmp_la->la_mode)) {
                        la->la_mode &= ~S_ISUID;
                        la->la_valid |= LA_MODE;
                }
        }

        /* Make sure caller can chgrp. */
        if (la->la_valid & LA_GID) {
                if (la->la_gid == (gid_t) -1)
                        la->la_gid = tmp_la->la_gid;
                if (((uc->mu_fsuid != tmp_la->la_uid) ||
                    ((la->la_gid != tmp_la->la_gid) &&
                    !mdd_in_group_p(uc, la->la_gid))) &&
                    !mdd_capable(uc, CAP_CHOWN))
                        RETURN(-EPERM);

                /*
                 * Likewise, if the user or group of a non-directory
                 * has been changed by a non-root user, remove the
                 * setgid bit UNLESS there is no group execute bit
                 * (this would be a file marked for mandatory
                 * locking).  19981026 David C Niemi <niemi@tux.org>
                 *
                 * Removed the fsuid check (see the comment above) --
                 * 19990830 SD.
                 */
                if (((tmp_la->la_mode & (S_ISGID | S_IXGRP)) ==
                     (S_ISGID | S_IXGRP)) && !S_ISDIR(tmp_la->la_mode)) {
                        la->la_mode &= ~S_ISGID;
                        la->la_valid |= LA_MODE;
                }
        }

        /* For tuncate (or setsize), we should have MAY_WRITE perm */
        if (la->la_valid & (LA_SIZE | LA_BLOCKS)) {
                rc = mdd_permission_internal_locked(env, obj, tmp_la, MAY_WRITE);
                if (rc)
                        RETURN(rc);

                /*
                 * For the "Size-on-MDS" setattr update, merge coming
                 * attributes with the set in the inode. BUG 10641
                 */
                if ((la->la_valid & LA_ATIME) &&
                    (la->la_atime < tmp_la->la_atime))
                        la->la_valid &= ~LA_ATIME;

                if ((la->la_valid & LA_CTIME) &&
                    (la->la_ctime < tmp_la->la_ctime))
                        la->la_valid &= ~(LA_MTIME | LA_CTIME);

                if (!(la->la_valid & LA_MTIME) && (now > tmp_la->la_mtime)) {
                        la->la_mtime = now;
                        la->la_valid |= LA_MTIME;
                }
        }

        /* For last, ctime must be fixed */
        if (!(la->la_valid & LA_CTIME) && (now > tmp_la->la_ctime)) {
                la->la_ctime = now;
                la->la_valid |= LA_CTIME;
        }

        RETURN(0);
}

/* set attr and LOV EA at once, return updated attr */
static int mdd_attr_set(const struct lu_env *env, struct md_object *obj,
                        const struct md_attr *ma)
{
        struct mdd_object *mdd_obj = md2mdd_obj(obj);
        struct mdd_device *mdd = mdo2mdd(obj);
        struct thandle *handle;
        struct lov_mds_md *lmm = NULL;
        int  rc, lmm_size = 0, max_size = 0;
        struct lu_attr *la_copy = &mdd_env_info(env)->mti_la_for_fix;
        ENTRY;

        mdd_txn_param_build(env, mdd, MDD_TXN_ATTR_SET_OP);
        handle = mdd_trans_start(env, mdd);
        if (IS_ERR(handle))
                RETURN(PTR_ERR(handle));
        /*TODO: add lock here*/
        /* start a log jounal handle if needed */
        if (S_ISREG(mdd_object_type(mdd_obj)) &&
            ma->ma_attr.la_valid & (LA_UID | LA_GID)) {
                max_size = mdd_lov_mdsize(env, mdd);
                OBD_ALLOC(lmm, max_size);
                lmm_size = max_size;
                if (lmm == NULL)
                        GOTO(cleanup, rc = -ENOMEM);

                rc = mdd_get_md_locked(env, mdd_obj, lmm, &lmm_size,
                                MDS_LOV_MD_NAME);

                if (rc < 0)
                        GOTO(cleanup, rc);
        }

        if (ma->ma_attr.la_valid & (ATTR_MTIME | ATTR_CTIME))
                CDEBUG(D_INODE, "setting mtime "LPU64", ctime "LPU64"\n",
                       ma->ma_attr.la_mtime, ma->ma_attr.la_ctime);

        *la_copy = ma->ma_attr;
        rc = mdd_fix_attr(env, mdd_obj, la_copy);
        if (rc)
                GOTO(cleanup, rc);

        if (la_copy->la_valid & LA_FLAGS) {
                rc = mdd_attr_set_internal_locked(env, mdd_obj, la_copy,
                                                  handle, 1);
                if (rc == 0)
                        mdd_flags_xlate(mdd_obj, la_copy->la_flags);
        } else if (la_copy->la_valid) {            /* setattr */
                rc = mdd_attr_set_internal_locked(env, mdd_obj, la_copy,
                                                  handle, 1);
                /* journal chown/chgrp in llog, just like unlink */
                if (rc == 0 && lmm_size){
                        /*TODO set_attr llog */
                }
        }

        if (rc == 0 && ma->ma_valid & MA_LOV) {
                umode_t mode;

                mode = mdd_object_type(mdd_obj);
                if (S_ISREG(mode) || S_ISDIR(mode)) {
                        /*TODO check permission*/
                        rc = mdd_lov_set_md(env, NULL, mdd_obj, ma->ma_lmm,
                                            ma->ma_lmm_size, handle, 1);
                }

        }
cleanup:
        mdd_trans_stop(env, mdd, rc, handle);
        if (rc == 0 && (lmm != NULL && lmm_size > 0 )) {
                /*set obd attr, if needed*/
                rc = mdd_lov_setattr_async(env, mdd_obj, lmm, lmm_size);
        }
        if (lmm != NULL) {
                OBD_FREE(lmm, max_size);
        }

        RETURN(rc);
}

int mdd_xattr_set_txn(const struct lu_env *env, struct mdd_object *obj,
                      const struct lu_buf *buf, const char *name, int fl,
                      struct thandle *handle)
{
        int  rc;
        ENTRY;

        mdd_write_lock(env, obj);
        rc = __mdd_xattr_set(env, obj, buf, name, fl, handle);
        mdd_write_unlock(env, obj);

        RETURN(rc);
}

static int mdd_xattr_sanity_check(const struct lu_env *env,
                                  struct mdd_object *obj)
{
        struct lu_attr  *tmp_la = &mdd_env_info(env)->mti_la;
        struct md_ucred *uc     = md_ucred(env);
        int rc;
        ENTRY;

        if (mdd_is_immutable(obj) || mdd_is_append(obj))
                RETURN(-EPERM);

        rc = mdd_la_get(env, obj, tmp_la, BYPASS_CAPA);
        if (rc)
                RETURN(rc);

        if ((uc->mu_fsuid != tmp_la->la_uid) && !mdd_capable(uc, CAP_FOWNER))
                RETURN(-EPERM);

        RETURN(rc);
}

static int mdd_xattr_set(const struct lu_env *env, struct md_object *obj,
                         const struct lu_buf *buf, const char *name, int fl)
{
        struct lu_attr *la_copy = &mdd_env_info(env)->mti_la_for_fix;
        struct mdd_object *mdd_obj = md2mdd_obj(obj);
        struct mdd_device *mdd = mdo2mdd(obj);
        struct thandle *handle;
        int  rc;
        ENTRY;

        rc = mdd_xattr_sanity_check(env, mdd_obj);
        if (rc)
                RETURN(rc);

        mdd_txn_param_build(env, mdd, MDD_TXN_XATTR_SET_OP);
        handle = mdd_trans_start(env, mdd);
        if (IS_ERR(handle))
                RETURN(PTR_ERR(handle));

        rc = mdd_xattr_set_txn(env, md2mdd_obj(obj), buf, name,
                               fl, handle);
        if (rc == 0) {
                la_copy->la_ctime = CURRENT_SECONDS;
                la_copy->la_valid = LA_CTIME;
                rc = mdd_attr_set_internal_locked(env, mdd_obj, la_copy, handle, 0);
        }
        mdd_trans_stop(env, mdd, rc, handle);

        RETURN(rc);
}

static int __mdd_xattr_del(const struct lu_env *env,struct mdd_device *mdd,
                           struct mdd_object *obj,
                           const char *name, struct thandle *handle)
{
        struct dt_object *next;

        LASSERT(lu_object_exists(mdd2lu_obj(obj)));
        next = mdd_object_child(obj);
        return next->do_ops->do_xattr_del(env, next, name, handle,
                                          mdd_object_capa(env, obj));
}

int mdd_xattr_del(const struct lu_env *env, struct md_object *obj,
                  const char *name)
{
        struct lu_attr *la_copy = &mdd_env_info(env)->mti_la_for_fix;
        struct mdd_object *mdd_obj = md2mdd_obj(obj);
        struct mdd_device *mdd = mdo2mdd(obj);
        struct thandle *handle;
        int  rc;
        ENTRY;

        rc = mdd_xattr_sanity_check(env, mdd_obj);
        if (rc)
                RETURN(rc);

        mdd_txn_param_build(env, mdd, MDD_TXN_XATTR_SET_OP);
        handle = mdd_trans_start(env, mdd);
        if (IS_ERR(handle))
                RETURN(PTR_ERR(handle));

        mdd_write_lock(env, mdd_obj);
        rc = __mdd_xattr_del(env, mdd, md2mdd_obj(obj), name, handle);
        mdd_write_unlock(env, mdd_obj);
        if (rc == 0) {
                la_copy->la_ctime = CURRENT_SECONDS;
                la_copy->la_valid = LA_CTIME;
                rc = mdd_attr_set_internal(env, mdd_obj, la_copy, handle, 0);
        }

        mdd_trans_stop(env, mdd, rc, handle);

        RETURN(rc);
}

/* partial unlink */
static int mdd_ref_del(const struct lu_env *env, struct md_object *obj,
                       struct md_attr *ma)
{
        struct lu_attr *la_copy = &mdd_env_info(env)->mti_la_for_fix;
        struct mdd_object *mdd_obj = md2mdd_obj(obj);
        struct mdd_device *mdd = mdo2mdd(obj);
        struct thandle *handle;
        int rc;
        ENTRY;

        /*
         * Check -ENOENT early here because we need to get object type
         * to calculate credits before transaction start
         */
        if (!lu_object_exists(&obj->mo_lu))
                RETURN(-ENOENT);
        LASSERT(lu_object_exists(&obj->mo_lu) > 0);

        rc = mdd_log_txn_param_build(env, obj, ma, MDD_TXN_UNLINK_OP);
        if (rc)
                RETURN(rc);

        handle = mdd_trans_start(env, mdd);
        if (IS_ERR(handle))
                RETURN(-ENOMEM);

        mdd_write_lock(env, mdd_obj);

        rc = mdd_unlink_sanity_check(env, NULL, mdd_obj, ma);
        if (rc)
                GOTO(cleanup, rc);

        mdd_ref_del_internal(env, mdd_obj, handle);

        if (S_ISDIR(lu_object_attr(&obj->mo_lu))) {
                /* unlink dot */
                mdd_ref_del_internal(env, mdd_obj, handle);
        }

        la_copy->la_ctime = CURRENT_SECONDS;
        la_copy->la_valid = LA_CTIME;
        rc = mdd_attr_set_internal(env, mdd_obj, la_copy, handle, 0);
        if (rc)
                GOTO(cleanup, rc);

        rc = mdd_finish_unlink(env, mdd_obj, ma, handle);

        EXIT;
cleanup:
        mdd_write_unlock(env, mdd_obj);
        mdd_trans_stop(env, mdd, rc, handle);
        return rc;
}

/* partial operation */
static int mdd_oc_sanity_check(const struct lu_env *env,
                               struct mdd_object *obj,
                               struct md_attr *ma)
{
        int rc;
        ENTRY;

        switch (ma->ma_attr.la_mode & S_IFMT) {
        case S_IFREG:
        case S_IFDIR:
        case S_IFLNK:
        case S_IFCHR:
        case S_IFBLK:
        case S_IFIFO:
        case S_IFSOCK:
                rc = 0;
                break;
        default:
                rc = -EINVAL;
                break;
        }
        RETURN(rc);
}

static int mdd_object_create(const struct lu_env *env,
                             struct md_object *obj,
                             const struct md_op_spec *spec,
                             struct md_attr *ma)
{

        struct mdd_device *mdd = mdo2mdd(obj);
        struct mdd_object *mdd_obj = md2mdd_obj(obj);
        const struct lu_fid *pfid = spec->u.sp_pfid;
        struct thandle *handle;
        int rc;
        ENTRY;

        mdd_txn_param_build(env, mdd, MDD_TXN_OBJECT_CREATE_OP);
        handle = mdd_trans_start(env, mdd);
        if (IS_ERR(handle))
                RETURN(PTR_ERR(handle));

        mdd_write_lock(env, mdd_obj);
        rc = mdd_oc_sanity_check(env, mdd_obj, ma);
        if (rc)
                GOTO(unlock, rc);

        rc = mdd_object_create_internal(env, mdd_obj, ma, handle);
        if (rc)
                GOTO(unlock, rc);

        if (spec->sp_cr_flags & MDS_CREATE_SLAVE_OBJ) {
                /* If creating the slave object, set slave EA here. */
                int lmv_size = spec->u.sp_ea.eadatalen;
                struct lmv_stripe_md *lmv;

                lmv = (struct lmv_stripe_md *)spec->u.sp_ea.eadata;
                LASSERT(lmv != NULL && lmv_size > 0);

                rc = __mdd_xattr_set(env, mdd_obj,
                                     mdd_buf_get_const(env, lmv, lmv_size),
                                     MDS_LMV_MD_NAME, 0, handle);
                if (rc)
                        GOTO(unlock, rc);
                pfid = spec->u.sp_ea.fid;

                CDEBUG(D_INFO, "Set slave ea "DFID", eadatalen %d, rc %d\n",
                       PFID(mdo2fid(mdd_obj)), spec->u.sp_ea.eadatalen, rc);

                rc = mdd_attr_set_internal(env, mdd_obj, &ma->ma_attr, handle, 0);
        } else {
#ifdef CONFIG_FS_POSIX_ACL
                if (spec->sp_cr_flags & MDS_CREATE_RMT_ACL) {
                        struct lu_buf *buf = &mdd_env_info(env)->mti_buf;

                        buf->lb_buf = (void *)spec->u.sp_ea.eadata;
                        buf->lb_len = spec->u.sp_ea.eadatalen;
                        if ((buf->lb_len > 0) && (buf->lb_buf != NULL)) {
                                rc = __mdd_acl_init(env, mdd_obj, buf,
                                                    &ma->ma_attr.la_mode,
                                                    handle);
                                if (rc)
                                        GOTO(unlock, rc);
                                else
                                        ma->ma_attr.la_valid |= LA_MODE;
                        }

                        pfid = spec->u.sp_ea.fid;
                }
#endif
                rc = mdd_object_initialize(env, pfid, mdd_obj, ma, handle);
        }
        EXIT;
unlock:
        mdd_write_unlock(env, mdd_obj);
        if (rc == 0)
                rc = mdd_attr_get_internal_locked(env, mdd_obj, ma);

        mdd_trans_stop(env, mdd, rc, handle);
        return rc;
}

/* partial link */
static int mdd_ref_add(const struct lu_env *env,
                       struct md_object *obj)
{
        struct lu_attr *la_copy = &mdd_env_info(env)->mti_la_for_fix;
        struct mdd_object *mdd_obj = md2mdd_obj(obj);
        struct mdd_device *mdd = mdo2mdd(obj);
        struct thandle *handle;
        int rc;
        ENTRY;

        mdd_txn_param_build(env, mdd, MDD_TXN_XATTR_SET_OP);
        handle = mdd_trans_start(env, mdd);
        if (IS_ERR(handle))
                RETURN(-ENOMEM);

        mdd_write_lock(env, mdd_obj);
        rc = mdd_link_sanity_check(env, NULL, NULL, mdd_obj);
        if (rc == 0)
                mdd_ref_add_internal(env, mdd_obj, handle);
        mdd_write_unlock(env, mdd_obj);
        if (rc == 0) {
                la_copy->la_ctime = CURRENT_SECONDS;
                la_copy->la_valid = LA_CTIME;
                rc = mdd_attr_set_internal(env, mdd_obj, la_copy, handle, 0);
        }
        mdd_trans_stop(env, mdd, 0, handle);

        RETURN(rc);
}

/*
 * do NOT or the MAY_*'s, you'll get the weakest
 * XXX: Can NOT understand.
 */
static int accmode(struct mdd_object *mdd_obj, int flags)
{
        int res = 0;

#if 0
        /* Sadly, NFSD reopens a file repeatedly during operation, so the
         * "acc_mode = 0" allowance for newly-created files isn't honoured.
         * NFSD uses the MDS_OPEN_OWNEROVERRIDE flag to say that a file
         * owner can write to a file even if it is marked readonly to hide
         * its brokenness. (bug 5781) */
        if (flags & MDS_OPEN_OWNEROVERRIDE && inode->i_uid == current->fsuid)
                return 0;
#endif
        if (flags & FMODE_READ)
                res |= MAY_READ;
        if (flags & (FMODE_WRITE | MDS_OPEN_TRUNC | MDS_OPEN_APPEND))
                res |= MAY_WRITE;
        if (flags & MDS_FMODE_EXEC)
                res |= MAY_EXEC;
        return res;
}

static int mdd_open_sanity_check(const struct lu_env *env,
                                 struct mdd_object *obj, int flag)
{
        struct lu_attr *tmp_la = &mdd_env_info(env)->mti_la;
        int mode = accmode(obj, flag);
        int rc;
        ENTRY;

        /* EEXIST check */
        if (mdd_is_dead_obj(obj))
                RETURN(-ENOENT);

        rc = mdd_la_get(env, obj, tmp_la, BYPASS_CAPA);
        if (rc)
               RETURN(rc);

        if (S_ISLNK(tmp_la->la_mode))
                RETURN(-ELOOP);

        if (S_ISDIR(tmp_la->la_mode) && (mode & MAY_WRITE))
                RETURN(-EISDIR);

        if (!(flag & MDS_OPEN_CREATED)) {
                rc = mdd_permission_internal(env, obj, tmp_la, mode);
                if (rc)
                        RETURN(rc);
        }

        if (S_ISFIFO(tmp_la->la_mode) || S_ISSOCK(tmp_la->la_mode) ||
            S_ISBLK(tmp_la->la_mode) || S_ISCHR(tmp_la->la_mode))
                flag &= ~MDS_OPEN_TRUNC;

        /* For writing append-only file must open it with append mode. */
        if (mdd_is_append(obj)) {
                if ((flag & FMODE_WRITE) && !(flag & MDS_OPEN_APPEND))
                        RETURN(-EPERM);
                if (flag & MDS_OPEN_TRUNC)
                        RETURN(-EPERM);
        }

#if 0
        /*
         * Now, flag -- O_NOATIME does not be packed by client.
         */
        if (flag & O_NOATIME) {
                struct md_ucred *uc = md_ucred(env);

                if (uc->mu_fsuid != tmp_la->la_uid &&
                    !mdd_capable(uc, CAP_FOWNER))
                        RETURN(-EPERM);
        }
#endif

        RETURN(0);
}

static int mdd_open(const struct lu_env *env, struct md_object *obj,
                    int flags)
{
        struct mdd_object *mdd_obj = md2mdd_obj(obj);
        int rc = 0;

        mdd_write_lock(env, mdd_obj);

        rc = mdd_open_sanity_check(env, mdd_obj, flags);
        if (rc == 0)
                mdd_obj->mod_count++;

        mdd_write_unlock(env, mdd_obj);
        return rc;
}

/* return md_attr back,
 * if it is last unlink then return lov ea + llog cookie*/
int mdd_object_kill(const struct lu_env *env, struct mdd_object *obj,
                    struct md_attr *ma)
{
        int rc = 0;
        ENTRY;

        if (S_ISREG(mdd_object_type(obj))) {
                /* Return LOV & COOKIES unconditionally here. We clean evth up.
                 * Caller must be ready for that. */
                rc = __mdd_lmm_get(env, obj, ma);
                if ((ma->ma_valid & MA_LOV))
                        rc = mdd_unlink_log(env, mdo2mdd(&obj->mod_obj),
                                            obj, ma);
        }
        RETURN(rc);
}

/*
 * No permission check is needed.
 */
static int mdd_close(const struct lu_env *env, struct md_object *obj,
                     struct md_attr *ma)
{
        int rc;
        struct mdd_object *mdd_obj = md2mdd_obj(obj);
        ENTRY;

        mdd_write_lock(env, mdd_obj);
        /* release open count */
        mdd_obj->mod_count --;

        rc = mdd_iattr_get(env, mdd_obj, ma);
        if (rc == 0 && mdd_obj->mod_count == 0) {
                if (ma->ma_attr.la_nlink == 0)
                        rc = mdd_object_kill(env, mdd_obj, ma);
        }
        mdd_write_unlock(env, mdd_obj);
        RETURN(rc);
}

/*
 * Permission check is done when open,
 * no need check again.
 */
static int mdd_readpage_sanity_check(const struct lu_env *env,
                                     struct mdd_object *obj)
{
        struct dt_object *next = mdd_object_child(obj);
        int rc;
        ENTRY;

        if (S_ISDIR(mdd_object_type(obj)) && dt_try_as_dir(env, next))
                rc = 0;
        else
                rc = -ENOTDIR;

        RETURN(rc);
}

static int mdd_dir_page_build(const struct lu_env *env, int first,
                              void *area, int nob, struct dt_it_ops *iops,
                              struct dt_it *it, __u32 *start, __u32 *end,
                              struct lu_dirent **last)
{
        struct lu_fid          *fid  = &mdd_env_info(env)->mti_fid2;
        struct mdd_thread_info *info = mdd_env_info(env);
        struct lu_fid_pack     *pack = &info->mti_pack;
        int                     result;
        struct lu_dirent       *ent;

        if (first) {
                memset(area, 0, sizeof (struct lu_dirpage));
                area += sizeof (struct lu_dirpage);
                nob  -= sizeof (struct lu_dirpage);
        }

        LASSERT(nob > sizeof *ent);

        ent  = area;
        result = 0;
        do {
                char  *name;
                int    len;
                int    recsize;
                __u32  hash;

                name = (char *)iops->key(env, it);
                len  = iops->key_size(env, it);

                pack = (struct lu_fid_pack *)iops->rec(env, it);
                fid_unpack(pack, fid);

                recsize = (sizeof(*ent) + len + 3) & ~3;
                hash = iops->store(env, it);
                *end = hash;

                CDEBUG(D_INFO, "%p %p %d "DFID": %#8.8x (%d) \"%*.*s\"\n",
                       name, ent, nob, PFID(fid), hash, len, len, len, name);

                if (nob >= recsize) {
                        ent->lde_fid = *fid;
                        fid_cpu_to_le(&ent->lde_fid, &ent->lde_fid);
                        ent->lde_hash = hash;
                        ent->lde_namelen = cpu_to_le16(len);
                        ent->lde_reclen  = cpu_to_le16(recsize);
                        memcpy(ent->lde_name, name, len);
                        if (first && ent == area)
                                *start = hash;
                        *last = ent;
                        ent = (void *)ent + recsize;
                        nob -= recsize;
                        result = iops->next(env, it);
                } else {
                        /*
                         * record doesn't fit into page, enlarge previous one.
                         */
                        LASSERT(*last != NULL);
                        (*last)->lde_reclen =
                                cpu_to_le16(le16_to_cpu((*last)->lde_reclen) +
                                            nob);
                        break;
                }
        } while (result == 0);

        return result;
}

static int __mdd_readpage(const struct lu_env *env, struct mdd_object *obj,
                          const struct lu_rdpg *rdpg)
{
        struct dt_it      *it;
        struct dt_object  *next = mdd_object_child(obj);
        struct dt_it_ops  *iops;
        struct page       *pg;
        struct lu_dirent  *last;
        int i;
        int rc;
        int nob;
        __u32 hash_start;
        __u32 hash_end;

        LASSERT(rdpg->rp_pages != NULL);
        LASSERT(next->do_index_ops != NULL);

        if (rdpg->rp_count <= 0)
                return -EFAULT;

        /*
         * iterate through directory and fill pages from @rdpg
         */
        iops = &next->do_index_ops->dio_it;
        it = iops->init(env, next, 0, mdd_object_capa(env, obj));
        if (it == NULL)
                return -ENOMEM;

        rc = iops->load(env, it, rdpg->rp_hash);

        if (rc == 0)
                /*
                 * Iterator didn't find record with exactly the key requested.
                 *
                 * It is currently either
                 *
                 *     - positioned above record with key less than
                 *     requested---skip it.
                 *
                 *     - or not positioned at all (is in IAM_IT_SKEWED
                 *     state)---position it on the next item.
                 */
                rc = iops->next(env, it);
        else if (rc > 0)
                rc = 0;

        /*
         * At this point and across for-loop:
         *
         *  rc == 0 -> ok, proceed.
         *  rc >  0 -> end of directory.
         *  rc <  0 -> error.
         */
        for (i = 0, nob = rdpg->rp_count; rc == 0 && nob > 0;
             i++, nob -= CFS_PAGE_SIZE) {
                LASSERT(i < rdpg->rp_npages);
                pg = rdpg->rp_pages[i];
                rc = mdd_dir_page_build(env, !i, kmap(pg),
                                        min_t(int, nob, CFS_PAGE_SIZE), iops,
                                        it, &hash_start, &hash_end, &last);
                if (rc != 0 || i == rdpg->rp_npages - 1)
                        last->lde_reclen = 0;
                kunmap(pg);
        }
        if (rc > 0) {
                /*
                 * end of directory.
                 */
                hash_end = DIR_END_OFF;
                rc = 0;
        }
        if (rc == 0) {
                struct lu_dirpage *dp;

                dp = kmap(rdpg->rp_pages[0]);
                dp->ldp_hash_start = rdpg->rp_hash;
                dp->ldp_hash_end   = hash_end;
                if (i == 0)
                        /*
                         * No pages were processed, mark this.
                         */
                        dp->ldp_flags |= LDF_EMPTY;
                dp->ldp_flags = cpu_to_le16(dp->ldp_flags);
                kunmap(rdpg->rp_pages[0]);
        }
        iops->put(env, it);
        iops->fini(env, it);

        return rc;
}

static int mdd_readpage(const struct lu_env *env, struct md_object *obj,
                        const struct lu_rdpg *rdpg)
{
        struct mdd_object *mdd_obj = md2mdd_obj(obj);
        int rc;
        ENTRY;

        LASSERT(lu_object_exists(mdd2lu_obj(mdd_obj)));

        mdd_read_lock(env, mdd_obj);
        rc = mdd_readpage_sanity_check(env, mdd_obj);
        if (rc)
                GOTO(out_unlock, rc);

        if (mdd_is_dead_obj(mdd_obj)) {
                struct page *pg;
                struct lu_dirpage *dp;

                /*
                 * According to POSIX, please do not return any entry to client:
                 * even dot and dotdot should not be returned.
                 */
                CWARN("readdir from dead object: "DFID"\n",
                        PFID(lu_object_fid(mdd2lu_obj(mdd_obj))));

                if (rdpg->rp_count <= 0)
                        GOTO(out_unlock, rc = -EFAULT);
                LASSERT(rdpg->rp_pages != NULL);

                pg = rdpg->rp_pages[0];
                dp = (struct lu_dirpage*)kmap(pg);
                memset(dp, 0 , sizeof(struct lu_dirpage));
                dp->ldp_hash_start = rdpg->rp_hash;
                dp->ldp_hash_end   = DIR_END_OFF;
                dp->ldp_flags |= LDF_EMPTY;
                dp->ldp_flags = cpu_to_le16(dp->ldp_flags);
                kunmap(pg);
                GOTO(out_unlock, rc = 0);
        }

        rc = __mdd_readpage(env, mdd_obj, rdpg);

        EXIT;
out_unlock:
        mdd_read_unlock(env, mdd_obj);
        return rc;
}

struct md_object_operations mdd_obj_ops = {
        .moo_permission    = mdd_permission,
        .moo_attr_get      = mdd_attr_get,
        .moo_attr_set      = mdd_attr_set,
        .moo_xattr_get     = mdd_xattr_get,
        .moo_xattr_set     = mdd_xattr_set,
        .moo_xattr_list    = mdd_xattr_list,
        .moo_xattr_del     = mdd_xattr_del,
        .moo_object_create = mdd_object_create,
        .moo_ref_add       = mdd_ref_add,
        .moo_ref_del       = mdd_ref_del,
        .moo_open          = mdd_open,
        .moo_close         = mdd_close,
        .moo_readpage      = mdd_readpage,
        .moo_readlink      = mdd_readlink,
        .moo_capa_get      = mdd_capa_get
};
