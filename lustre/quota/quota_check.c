/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  lustre/quota/quota_check.c
 *
 *  Copyright (c) 2005 Cluster File Systems, Inc.
 *
 *   This file is part of Lustre, http://www.lustre.org.
 *
 *   No redistribution or use is permitted outside of Cluster File Systems, Inc.
 *
 */
#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_MDS

#ifdef __KERNEL__
# include <linux/version.h>
# include <linux/module.h>
# include <linux/init.h>
# include <linux/fs.h>
# include <linux/jbd.h>
# include <linux/ext3_fs.h>
# if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,5,0))
#  include <linux/smp_lock.h>
#  include <linux/buffer_head.h>
#  include <linux/workqueue.h>
#  include <linux/mount.h>
# else
#  include <linux/locks.h>
# endif
#else /* __KERNEL__ */
# include <liblustre.h>
#endif

#include <obd_class.h>
#include <lustre_mds.h>
#include <lustre_dlm.h>
#include <lustre_cfg.h>
#include <obd_ost.h>
#include <lustre_fsfilt.h>
#include <lustre_quota.h>
#include "quota_internal.h"

#ifdef __KERNEL__
static int target_quotacheck_callback(struct obd_export *exp,
                                      struct obd_quotactl *oqctl)
{
        struct ptlrpc_request *req;
        struct obd_quotactl *body;
        int rc, size = sizeof(*oqctl);
        ENTRY;

        req = ptlrpc_prep_req(exp->exp_imp_reverse, LUSTRE_OBD_VERSION,
                              OBD_QC_CALLBACK, 1, &size, NULL);
        if (!req)
                RETURN(-ENOMEM);

        body = lustre_msg_buf(req->rq_reqmsg, 0, sizeof(*body));
        *body = *oqctl;

        req->rq_replen = lustre_msg_size(0, NULL);

        rc = ptlrpc_queue_wait(req);
        ptlrpc_req_finished(req);

        RETURN(rc);
}

static int target_quotacheck_thread(void *data)
{
        struct quotacheck_thread_args *qta = data;
        struct obd_export *exp;
        struct obd_device *obd;
        struct obd_quotactl *oqctl;
        struct lvfs_run_ctxt saved;
        int rc;

        ptlrpc_daemonize("quotacheck");

        exp = qta->qta_exp;
        obd = exp->exp_obd;
        oqctl = &qta->qta_oqctl;

        push_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);

        rc = fsfilt_quotacheck(obd, qta->qta_sb, oqctl);
        if (rc)
                CERROR("%s: fsfilt_quotacheck: %d\n", obd->obd_name, rc);

        pop_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);

        rc = target_quotacheck_callback(exp, oqctl);

        atomic_inc(qta->qta_sem);

        OBD_FREE_PTR(qta);
        return rc;
}

int target_quota_check(struct obd_export *exp, struct obd_quotactl *oqctl)
{
        struct obd_device *obd = exp->exp_obd;
        struct obd_device_target *obt = &obd->u.obt;
        struct quotacheck_thread_args *qta;
        int rc = 0;
        ENTRY;

        if (!atomic_dec_and_test(&obt->obt_quotachecking)) {
                CDEBUG(D_INFO, "other people are doing quotacheck\n");
                GOTO(out, rc = -EBUSY);
        }

        OBD_ALLOC_PTR(qta);
        if (!qta)
                GOTO(out, rc = -ENOMEM);

        qta->qta_exp = exp;
        qta->qta_oqctl = *oqctl;
        qta->qta_sb = obt->obt_sb;
        qta->qta_sem = &obt->obt_quotachecking;

        if (!strcmp(obd->obd_type->typ_name, LUSTRE_MDS_NAME)) {
                /* quota master */
                rc = init_admin_quotafiles(obd, &qta->qta_oqctl);
                if (rc) {
                        CERROR("init_admin_quotafiles failed: %d\n", rc);
                        OBD_FREE_PTR(qta);
                        GOTO(out, rc);
                }
        }

        rc = kernel_thread(target_quotacheck_thread, qta, CLONE_VM|CLONE_FILES);
        if (rc >= 0) {
                CDEBUG(D_INFO, "%s: target_quotacheck_thread: %d\n",
                       obd->obd_name, rc);
                RETURN(0);
        }

        CERROR("%s: error starting quotacheck_thread: %d\n",
               obd->obd_name, rc);
        OBD_FREE_PTR(qta);
out:
        atomic_inc(&obt->obt_quotachecking);
        RETURN(rc);
}

#endif /* __KERNEL__ */

int client_quota_check(struct obd_export *exp, struct obd_quotactl *oqctl)
{
        struct client_obd *cli = &exp->exp_obd->u.cli;
        struct ptlrpc_request *req;
        struct obd_quotactl *body;
        int size = sizeof(*body), opc, version;
        int rc;
        ENTRY;

        if (!strcmp(exp->exp_obd->obd_type->typ_name, LUSTRE_MDC_NAME)) {
                version = LUSTRE_MDS_VERSION;
                opc = MDS_QUOTACHECK;
        } else if (!strcmp(exp->exp_obd->obd_type->typ_name, LUSTRE_OSC_NAME)) {
                version = LUSTRE_OST_VERSION;
                opc = OST_QUOTACHECK;
        } else {
                RETURN(-EINVAL);
        }

        req = ptlrpc_prep_req(class_exp2cliimp(exp), version, opc, 1, &size,
                              NULL);
        if (!req)
                GOTO(out, rc = -ENOMEM);

        body = lustre_msg_buf(req->rq_reqmsg, 0, sizeof(*body));
        *body = *oqctl;

        req->rq_replen = lustre_msg_size(0, NULL);

        /* the next poll will find -ENODATA, that means quotacheck is
         * going on */
        cli->cl_qchk_stat = -ENODATA;
        rc = ptlrpc_queue_wait(req);
        if (rc)
                cli->cl_qchk_stat = rc;
out:
        ptlrpc_req_finished(req);
        RETURN(rc);
}

int client_quota_poll_check(struct obd_export *exp, struct if_quotacheck *qchk)
{
        struct client_obd *cli = &exp->exp_obd->u.cli;
        int rc;
        ENTRY;

        rc = cli->cl_qchk_stat;

        /* the client is not the previous one */
        if (rc == CL_NOT_QUOTACHECKED)
                rc = -EINTR;

        qchk->obd_uuid = cli->cl_target_uuid;
        /* FIXME change strncmp to strcmp and save the strlen op */
        if (strncmp(exp->exp_obd->obd_type->typ_name, LUSTRE_OSC_NAME,
            strlen(LUSTRE_OSC_NAME)))
                memcpy(qchk->obd_type, LUSTRE_OST_NAME,
                       strlen(LUSTRE_OST_NAME));
        else if (strncmp(exp->exp_obd->obd_type->typ_name, LUSTRE_MDC_NAME,
                 strlen(LUSTRE_MDC_NAME)))
                memcpy(qchk->obd_type, LUSTRE_MDS_NAME,
                       strlen(LUSTRE_MDS_NAME));

        RETURN(rc);
}

int lov_quota_check(struct obd_export *exp, struct obd_quotactl *oqctl)
{
        struct obd_device *obd = class_exp2obd(exp);
        struct lov_obd *lov = &obd->u.lov;
        int i, rc = 0;
        ENTRY;

        for (i = 0; i < lov->desc.ld_tgt_count; i++) {
                int err;

                if (!lov->tgts[i].active) {
                        CERROR("lov idx %d inactive\n", i);
                        RETURN(-EIO);
                }

                err = obd_quotacheck(lov->tgts[i].ltd_exp, oqctl);
                if (err && lov->tgts[i].active && !rc)
                        rc = err;
        }

        RETURN(rc);
}
