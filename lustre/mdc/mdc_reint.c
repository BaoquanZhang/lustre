/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) 2001-2003 Cluster File Systems, Inc.
 *
 *   This file is part of Lustre, http://www.sf.net/projects/lustre/
 *
 *   Lustre is free software; you can redistribute it and/or
 *   modify it under the terms of version 2 of the GNU General Public
 *   License as published by the Free Software Foundation.
 *
 *   Lustre is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Lustre; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#define EXPORT_SYMTAB
#define DEBUG_SUBSYSTEM S_MDC

#ifdef __KERNEL__
# include <linux/config.h>
# include <linux/module.h>
# include <linux/kernel.h>
#else
# include <liblustre.h>
#endif

#include <linux/obd_class.h>
#include <linux/lustre_mds.h>
#include "mdc_internal.h"

/* mdc_setattr does its own semaphore handling */
static int mdc_reint(struct ptlrpc_request *request, int level)
{
        int rc;
        __u32 *opcodeptr;

        opcodeptr = lustre_msg_buf(request->rq_reqmsg, 0, sizeof (*opcodeptr));
        request->rq_level = level;

        if (!(*opcodeptr == REINT_SETATTR))
                mdc_get_rpc_lock(&mdc_rpc_lock, NULL);
        rc = ptlrpc_queue_wait(request);
        if (!(*opcodeptr == REINT_SETATTR))
                mdc_put_rpc_lock(&mdc_rpc_lock, NULL);

        if (rc)
                CDEBUG(D_INFO, "error in handling %d\n", rc);
        return rc;
}

/* If mdc_setattr is called with an 'iattr', then it is a normal RPC that
 * should take the normal semaphore and go to the normal portal.
 *
 * If it is called with iattr->ia_valid & ATTR_FROM_OPEN, then it is a
 * magic open-path setattr that should take the setattr semaphore and
 * go to the setattr portal. */
int mdc_setattr(struct lustre_handle *conn,
                struct mdc_op_data *data,
                struct iattr *iattr, void *ea, int ealen, void *ea2, int ea2len,
                struct ptlrpc_request **request)
{
        struct ptlrpc_request *req;
        struct mds_rec_setattr *rec;
        struct mdc_rpc_lock *rpc_lock;
        int rc, bufcount = 1, size[3] = {sizeof(*rec), ealen, ea2len};
        ENTRY;

        LASSERT(iattr != NULL);

        if (ealen > 0) {
                bufcount = 2;
                if (ea2len > 0)
                        bufcount = 3;
        }

        req = ptlrpc_prep_req(class_conn2cliimp(conn), MDS_REINT, bufcount,
                              size, NULL);
        if (!req)
                RETURN(-ENOMEM);

        if (iattr->ia_valid & ATTR_FROM_OPEN) {
                req->rq_request_portal = MDS_SETATTR_PORTAL; //XXX FIXME bug 249
                rpc_lock = &mdc_setattr_lock;
        } else
                rpc_lock = &mdc_rpc_lock;

        mdc_setattr_pack(req, data, iattr, ea, ealen, ea2, ea2len);

        size[0] = sizeof(struct mds_body);
        req->rq_replen = lustre_msg_size(1, size);

        mdc_get_rpc_lock(rpc_lock, NULL);
        rc = mdc_reint(req, LUSTRE_CONN_FULL);
        mdc_put_rpc_lock(rpc_lock, NULL);

        *request = req;
        if (rc == -ERESTARTSYS)
                rc = 0;

        RETURN(rc);
}

int mdc_create(struct lustre_handle *conn,
               struct mdc_op_data *op_data,
               const void *data, int datalen,
               int mode, __u32 uid, __u32 gid, __u64 time, __u64 rdev,
               struct ptlrpc_request **request)
{
        struct ptlrpc_request *req;
        int rc, size[3] = {sizeof(struct mds_rec_create),
                           op_data->namelen + 1, 0};
        int level, bufcount = 2;
//        ENTRY;

        if (data && datalen) {
                size[bufcount] = datalen;
                bufcount++;
        }

        req = ptlrpc_prep_req(class_conn2cliimp(conn), MDS_REINT, bufcount,
                              size, NULL);
        if (!req)
                return -ENOMEM;
//                RETURN(-ENOMEM);

        /* mdc_create_pack fills msg->bufs[1] with name
         * and msg->bufs[2] with tgt, for symlinks or lov MD data */
        mdc_create_pack(req, 0, op_data,
                        mode, rdev, uid, gid, time,
                        data, datalen);

        size[0] = sizeof(struct mds_body);
        req->rq_replen = lustre_msg_size(1, size);

        level = LUSTRE_CONN_FULL;
 resend:
        rc = mdc_reint(req, level);
        /* Resend if we were told to. */
        if (rc == -ERESTARTSYS) {
                level = LUSTRE_CONN_RECOVER;
                goto resend;
        }

        if (!rc)
                mdc_store_inode_generation(req, 0, 0);

        *request = req;
        return rc;
//        RETURN(rc);
}

int mdc_unlink(struct lustre_handle *conn,
               struct mdc_op_data *data,
               struct ptlrpc_request **request)
{
        struct obd_device *obddev = class_conn2obd(conn);
        struct ptlrpc_request *req = *request;
        int rc, size[2] = {sizeof(struct mds_rec_unlink), data->namelen + 1};
        ENTRY;

        LASSERT(req == NULL);

        req = ptlrpc_prep_req(class_conn2cliimp(conn), MDS_REINT, 2, size,
                              NULL);
        if (!req)
                RETURN(-ENOMEM);
        *request = req;

        size[0] = sizeof(struct mds_body);
        size[1] = obddev->u.cli.cl_max_mds_easize;
        size[2] = obddev->u.cli.cl_max_mds_cookiesize;
        req->rq_replen = lustre_msg_size(3, size);

        mdc_unlink_pack(req, 0, data);

        rc = mdc_reint(req, LUSTRE_CONN_FULL);
        if (rc == -ERESTARTSYS)
                rc = 0;
        RETURN(rc);
}

int mdc_link(struct lustre_handle *conn,
             struct mdc_op_data *data,
             struct ptlrpc_request **request)
{
        struct ptlrpc_request *req;
        int rc, size[2] = {sizeof(struct mds_rec_link), data->namelen + 1};
        ENTRY;

        req = ptlrpc_prep_req(class_conn2cliimp(conn), MDS_REINT, 2, size,
                              NULL);
        if (!req)
                RETURN(-ENOMEM);

        mdc_link_pack(req, 0, data);

        size[0] = sizeof(struct mds_body);
        req->rq_replen = lustre_msg_size(1, size);

        rc = mdc_reint(req, LUSTRE_CONN_FULL);
        *request = req;
        if (rc == -ERESTARTSYS)
                rc = 0;

        RETURN(rc);
}

int mdc_rename(struct lustre_handle *conn,
               struct mdc_op_data *data,
               const char *old, int oldlen,
               const char *new, int newlen,
               struct ptlrpc_request **request)
{
        struct ptlrpc_request *req;
        int rc, size[3] = {sizeof(struct mds_rec_rename), oldlen + 1,
                           newlen + 1};
        ENTRY;

        req = ptlrpc_prep_req(class_conn2cliimp(conn), MDS_REINT, 3, size,
                              NULL);
        if (!req)
                RETURN(-ENOMEM);

        mdc_rename_pack(req, 0, data, old, oldlen, new, newlen);

        size[0] = sizeof(struct mds_body);
        req->rq_replen = lustre_msg_size(1, size);

        rc = mdc_reint(req, LUSTRE_CONN_FULL);
        *request = req;
        if (rc == -ERESTARTSYS)
                rc = 0;

        RETURN(rc);
}
