/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) 2001-2003 Cluster File Systems, Inc.
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
#define DEBUG_SUBSYSTEM S_MDC

#ifdef __KERNEL__
# include <linux/config.h>
# include <linux/module.h>
# include <linux/kernel.h>
#else
# include <liblustre.h>
#endif

#include <linux/obd_class.h>
#include <linux/lustre_mdc.h>
#include "mdc_internal.h"

/* mdc_setattr does its own semaphore handling */
static int mdc_reint(struct ptlrpc_request *request,
                     struct mdc_rpc_lock *rpc_lock, int level)
{
        int rc;

        request->rq_send_state = level;

        mdc_get_rpc_lock(rpc_lock, NULL);
        rc = ptlrpc_queue_wait(request);
        mdc_put_rpc_lock(rpc_lock, NULL);
        if (rc)
                CDEBUG(D_INFO, "error in handling %d\n", rc);
        else if (!lustre_swab_repbuf(request, 0, sizeof(struct mdt_body),
                                     lustre_swab_mdt_body)) {
                CERROR ("Can't unpack mdt_body\n");
                rc = -EPROTO;
        }
        return rc;
}

/* If mdc_setattr is called with an 'iattr', then it is a normal RPC that
 * should take the normal semaphore and go to the normal portal.
 *
 * If it is called with iattr->ia_valid & ATTR_FROM_OPEN, then it is a
 * magic open-path setattr that should take the setattr semaphore and
 * go to the setattr portal. */
int mdc_setattr(struct obd_export *exp, struct mdc_op_data *data,
                struct iattr *iattr, void *ea, int ealen, void *ea2, int ea2len,
                struct ptlrpc_request **request)
{
        struct ptlrpc_request *req;
        struct mdt_rec_setattr *rec;
        struct mdc_rpc_lock *rpc_lock;
        struct obd_device *obd = exp->exp_obd;
        int size[] = { sizeof(*rec), ealen, ea2len};
        int rc, bufcount = 1;
        ENTRY;

        LASSERT(iattr != NULL);

        if (ealen > 0) {
                bufcount++;
                if (ea2len > 0)
                        bufcount++;
        }

        req = ptlrpc_prep_req(class_exp2cliimp(exp), LUSTRE_MDS_VERSION,
                              MDS_REINT, bufcount, size, NULL);
        if (req == NULL)
                RETURN(-ENOMEM);

        if (iattr->ia_valid & ATTR_FROM_OPEN) {
                req->rq_request_portal = MDS_SETATTR_PORTAL; //XXX FIXME bug 249
                rpc_lock = obd->u.cli.cl_setattr_lock;
        } else {
                rpc_lock = obd->u.cli.cl_rpc_lock;
        }

        if (iattr->ia_valid & (ATTR_MTIME | ATTR_CTIME))
                CDEBUG(D_INODE, "setting mtime %lu, ctime %lu\n",
                       LTIME_S(iattr->ia_mtime), LTIME_S(iattr->ia_ctime));
        mdc_setattr_pack(req, MDS_REQ_REC_OFF, data, iattr, ea, ealen, ea2, ea2len);

        size[0] = sizeof(struct mdt_body);
        req->rq_replen = lustre_msg_size(1, size);

        rc = mdc_reint(req, rpc_lock, LUSTRE_IMP_FULL);
        *request = req;
        if (rc == -ERESTARTSYS)
                rc = 0;

        RETURN(rc);
}

int mdc_create(struct obd_export *exp, struct mdc_op_data *op_data,
               const void *data, int datalen, int mode, __u32 uid, __u32 gid,
               __u32 cap_effective, __u64 rdev, struct ptlrpc_request **request)
{
        struct obd_device *obd = exp->exp_obd;
        struct ptlrpc_request *req;
        int size[] = { sizeof(struct mdt_rec_create), op_data->namelen + 1, 0};
        int rc, level, bufcount = 2;
        ENTRY;

        if (data && datalen) {
                size[bufcount] = datalen;
                bufcount++;
        }

        req = ptlrpc_prep_req(class_exp2cliimp(exp), LUSTRE_MDS_VERSION,
                              MDS_REINT, bufcount, size, NULL);
        if (req == NULL)
                RETURN(-ENOMEM);

        /* mdc_create_pack fills msg->bufs[1] with name
         * and msg->bufs[2] with tgt, for symlinks or lov MD data */
        mdc_create_pack(req, MDS_REQ_REC_OFF, op_data, data, datalen, mode,
                        uid, gid, cap_effective, rdev);

        size[0] = sizeof(struct mdt_body);
        req->rq_replen = lustre_msg_size(1, size);

        level = LUSTRE_IMP_FULL;
 resend:
        rc = mdc_reint(req, obd->u.cli.cl_rpc_lock, level);
        /* Resend if we were told to. */
        if (rc == -ERESTARTSYS) {
                level = LUSTRE_IMP_RECOVER;
                goto resend;
        }

        *request = req;
        RETURN(rc);
}

#ifdef FLD_PROTO_TYPE
int mdc_fld_req(struct obd_export *exp, __u64 mds_num, __u64 seq_num, int op)
{
        struct obd_device *obd = exp->exp_obd;
        struct ptlrpc_request *req;
        int size[] = { sizeof(struct mdt_body)};
        int rc, level, bufcount = 1;
        ENTRY;

        req = ptlrpc_prep_req(class_exp2cliimp(exp), LUSTRE_MDS_VERSION,
                              MDS_FLD_OP, bufcount, size, NULL);
        if (req == NULL)
                RETURN(-ENOMEM);

        /* mdc_create_pack fills msg->bufs[1] with name
         * and msg->bufs[2] with tgt, for symlinks or lov MD data */
        mdc_fld_pack(req, MDS_REQ_REC_OFF, mds_num, seq_num, op);

        size[0] = sizeof(struct mdt_body);
        req->rq_replen = lustre_msg_size(1, size);

        level = LUSTRE_IMP_FULL;
        rc = mdc_reint(req, obd->u.cli.cl_rpc_lock, level);

        ptlrpc_req_finished(req);
        RETURN(rc);
}
#endif

int mdc_unlink(struct obd_export *exp, struct mdc_op_data *data,
               struct ptlrpc_request **request)
{
        struct obd_device *obd = class_exp2obd(exp);
        struct ptlrpc_request *req = *request;
        int rc, size[] = { sizeof(struct mdt_rec_unlink), data->namelen + 1};
        ENTRY;

        LASSERT(req == NULL);
        req = ptlrpc_prep_req(class_exp2cliimp(exp), LUSTRE_MDS_VERSION,
                              MDS_REINT, 2, size, NULL);
        if (req == NULL)
                RETURN(-ENOMEM);
        *request = req;

        size[0] = sizeof(struct mdt_body);
        size[1] = obd->u.cli.cl_max_mds_easize;
        size[2] = obd->u.cli.cl_max_mds_cookiesize;
        req->rq_replen = lustre_msg_size(3, size);

        mdc_unlink_pack(req, MDS_REQ_REC_OFF, data);

        rc = mdc_reint(req, obd->u.cli.cl_rpc_lock, LUSTRE_IMP_FULL);
        if (rc == -ERESTARTSYS)
                rc = 0;
        RETURN(rc);
}

int mdc_link(struct obd_export *exp, struct mdc_op_data *data,
             struct ptlrpc_request **request)
{
        struct obd_device *obd = exp->exp_obd;
        struct ptlrpc_request *req;
        int rc, size[] = { sizeof(struct mdt_rec_link), data->namelen + 1};
        ENTRY;

        req = ptlrpc_prep_req(class_exp2cliimp(exp), LUSTRE_MDS_VERSION,
                              MDS_REINT, 2, size, NULL);
        if (req == NULL)
                RETURN(-ENOMEM);

        mdc_link_pack(req, MDS_REQ_REC_OFF, data);

        size[0] = sizeof(struct mdt_body);
        req->rq_replen = lustre_msg_size(1, size);

        rc = mdc_reint(req, obd->u.cli.cl_rpc_lock, LUSTRE_IMP_FULL);
        *request = req;
        if (rc == -ERESTARTSYS)
                rc = 0;

        RETURN(rc);
}

int mdc_rename(struct obd_export *exp, struct mdc_op_data *data,
               const char *old, int oldlen, const char *new, int newlen,
               struct ptlrpc_request **request)
{
        struct obd_device *obd = exp->exp_obd;
        struct ptlrpc_request *req;
        int rc, size[] = { sizeof(struct mdt_rec_rename), oldlen +1, newlen +1};
        ENTRY;

        req = ptlrpc_prep_req(class_exp2cliimp(exp), LUSTRE_MDS_VERSION,
                              MDS_REINT, 3, size, NULL);
        if (req == NULL)
                RETURN(-ENOMEM);

        mdc_rename_pack(req, MDS_REQ_REC_OFF, data, old, oldlen, new, newlen);

        size[0] = sizeof(struct mdt_body);
        size[1] = obd->u.cli.cl_max_mds_easize;
        size[2] = obd->u.cli.cl_max_mds_cookiesize;
        req->rq_replen = lustre_msg_size(3, size);

        rc = mdc_reint(req, obd->u.cli.cl_rpc_lock, LUSTRE_IMP_FULL);
        *request = req;
        if (rc == -ERESTARTSYS)
                rc = 0;

        RETURN(rc);
}
