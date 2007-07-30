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

#include <linux/ldiskfs_fs.h>
#include <lustre_mds.h>
#include <lustre/lustre_idl.h>

#include "mdd_internal.h"

int mdd_txn_start_cb(const struct lu_env *env, struct txn_param *param,
                     void *cookie)
{
        return 0;
}

int mdd_txn_stop_cb(const struct lu_env *env, struct thandle *txn,
                    void *cookie)
{
        struct mdd_device *mdd = cookie;
        struct obd_device *obd = mdd2obd_dev(mdd);

        LASSERT(obd);
        return mds_lov_write_objids(obd);
}

int mdd_txn_commit_cb(const struct lu_env *env, struct thandle *txn,
                      void *cookie)
{
        return 0;
}

static int dto_txn_credits[DTO_NR];
void mdd_txn_param_build(const struct lu_env *env, struct mdd_device *mdd,
                         enum mdd_txn_op op)
{
        LASSERT(0 <= op && op < MDD_TXN_LAST_OP);

        txn_param_init(&mdd_env_info(env)->mti_param,
                       mdd->mdd_tod[op].mod_credits);
}

int mdd_log_txn_param_build(const struct lu_env *env, struct md_object *obj,
                            struct md_attr *ma, enum mdd_txn_op op)
{
        struct mdd_device *mdd = mdo2mdd(&md2mdd_obj(obj)->mod_obj);
        int rc, log_credits, stripe;
        ENTRY;

        mdd_txn_param_build(env, mdd, op);

        if (S_ISDIR(lu_object_attr(&obj->mo_lu)))
                RETURN(0);

        LASSERT(op == MDD_TXN_UNLINK_OP || op == MDD_TXN_RENAME_OP);
        rc = mdd_lmm_get_locked(env, md2mdd_obj(obj), ma);
        if (rc || !(ma->ma_valid & MA_LOV))
                RETURN(rc);

        LASSERT(le32_to_cpu(ma->ma_lmm->lmm_magic) == LOV_MAGIC);
        if ((int)le32_to_cpu(ma->ma_lmm->lmm_stripe_count) < 0)
                stripe = mdd2obd_dev(mdd)->u.mds.mds_lov_desc.ld_tgt_count;
        else
                stripe = le32_to_cpu(ma->ma_lmm->lmm_stripe_count);

        log_credits = stripe * dto_txn_credits[DTO_LOG_REC];
        mdd_env_info(env)->mti_param.tp_credits += log_credits;
        RETURN(rc);
}

static void mdd_txn_init_dto_credits(const struct lu_env *env,
                                     struct mdd_device *mdd, int *dto_credits)
{
        int op, credits;
        for (op = 0; op < DTO_NR; op++) {
                credits = mdd_child_ops(mdd)->dt_credit_get(env, mdd->mdd_child,
                                                            op);
                LASSERT(credits > 0);
                dto_txn_credits[op] = credits;
        }
}

int mdd_txn_init_credits(const struct lu_env *env, struct mdd_device *mdd)
{
        int op;

        /* Init credits for each ops. */
        mdd_txn_init_dto_credits(env, mdd, dto_txn_credits);

        /* Calculate the mdd credits. */
        for (op = MDD_TXN_OBJECT_DESTROY_OP; op < MDD_TXN_LAST_OP; op++) {
                int *c = &mdd->mdd_tod[op].mod_credits;
                int *dt = dto_txn_credits;
                mdd->mdd_tod[op].mod_op = op;
                switch(op) {
                        case MDD_TXN_OBJECT_DESTROY_OP:
                                *c = dt[DTO_OBJECT_DELETE];
                                break;
                        case MDD_TXN_OBJECT_CREATE_OP:
                                /* OI_INSERT + CREATE OBJECT */
                                *c = dt[DTO_INDEX_INSERT] +
                                        dt[DTO_OBJECT_CREATE];
                                break;
                        case MDD_TXN_ATTR_SET_OP:
                                /* ATTR set + XATTR(lsm, lmv) set */
                                *c = dt[DTO_ATTR_SET] + dt[DTO_XATTR_SET];
                                break;
                        case MDD_TXN_XATTR_SET_OP:
                                *c = dt[DTO_XATTR_SET];
                                break;
                        case MDD_TXN_INDEX_INSERT_OP:
                                *c = dt[DTO_INDEX_INSERT];
                                break;
                        case MDD_TXN_INDEX_DELETE_OP:
                                *c = dt[DTO_INDEX_DELETE];
                                break;
                        case MDD_TXN_LINK_OP:
                                *c = dt[DTO_INDEX_INSERT];
                                break;
                        case MDD_TXN_UNLINK_OP:
                                /* delete index + Unlink log */
                                *c = dt[DTO_INDEX_DELETE];
                                break;
                        case MDD_TXN_RENAME_OP:
                                /* 2 delete index + 1 insert + Unlink log */
                                *c = 2 * dt[DTO_INDEX_DELETE] +
                                        dt[DTO_INDEX_INSERT];
                                break;
                        case MDD_TXN_RENAME_TGT_OP:
                                /* index insert + index delete */
                                *c = dt[DTO_INDEX_DELETE] +
                                     dt[DTO_INDEX_INSERT];
                                break;
                        case MDD_TXN_CREATE_DATA_OP:
                                /* same as set xattr(lsm) */
                                *c = dt[DTO_XATTR_SET];
                                break;
                        case MDD_TXN_MKDIR_OP:
                                /* INDEX INSERT + OI INSERT +
                                 * CREATE_OBJECT_CREDITS
                                 * SET_MD CREDITS is already counted in
                                 * CREATE_OBJECT CREDITS
                                 */
                                 *c = 2 * dt[DTO_INDEX_INSERT] +
                                         dt[DTO_OBJECT_CREATE];
                                break;
                        default:
                                CERROR("Invalid op %d init its credit\n", op);
                                LBUG();
                }
        }
        RETURN(0);
}

struct thandle* mdd_trans_start(const struct lu_env *env,
                                struct mdd_device *mdd)
{
        struct txn_param *p = &mdd_env_info(env)->mti_param;
        struct thandle *th;

        th = mdd_child_ops(mdd)->dt_trans_start(env, mdd->mdd_child, p);
        return th;
}

void mdd_trans_stop(const struct lu_env *env, struct mdd_device *mdd,
                    int result, struct thandle *handle)
{
        handle->th_result = result;
        mdd_child_ops(mdd)->dt_trans_stop(env, handle);
}
