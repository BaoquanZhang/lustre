/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (c) 2002, 2003 Cluster File Systems, Inc.
 *   Author: Zach Brown <zab@clusterfs.com>
 *
 *   This file is part of Lustre, http://www.lustre.org.
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

#include <linux/version.h>
#include <linux/module.h>
#include <linux/fs.h>

#define DEBUG_SUBSYSTEM S_PTLBD

#include <linux/obd_support.h>
#include <linux/obd_class.h>
#include <linux/lustre_debug.h>
#include <linux/lprocfs_status.h>
#include <linux/obd_ptlbd.h>

#define BACKING_FILE    "/tmp/ptlbd-backing-file-la-la-la"

static int ptlbd_sv_already_setup = 1;

static int ptlbd_sv_setup(struct obd_device *obddev, obd_count len, void *buf)
{
        struct ptlbd_obd *ptlbd = &obddev->u.ptlbd;
        int rc;
        ENTRY;

        ptlbd->filp = filp_open(BACKING_FILE,
                                        O_RDWR|O_CREAT|O_LARGEFILE, 0600);

        if ( IS_ERR(ptlbd->filp) )
                RETURN(PTR_ERR(ptlbd->filp));

        ptlbd->ptlbd_service =
                ptlrpc_init_svc(PTLBD_NBUFS, PTLBD_BUFSIZE, PTLBD_MAXREQSIZE,
                                PTLBD_REQUEST_PORTAL, PTLBD_REPLY_PORTAL,
                                ptlbd_handle, "ptlbd_sv",
                                obddev->obd_proc_entry);

        if (ptlbd->ptlbd_service == NULL) 
                GOTO(out_filp, rc = -ENOMEM);

        rc = ptlrpc_start_n_threads(obddev, ptlbd->ptlbd_service, 1, "ptldb");
        if (rc != 0) 
                GOTO(out_thread, rc);

        ptlbd_sv_already_setup = 1;

        RETURN(0);

out_thread:
        ptlrpc_unregister_service(ptlbd->ptlbd_service);
out_filp:
        filp_close(ptlbd->filp, NULL);

        RETURN(rc);
}

static int ptlbd_sv_cleanup(struct obd_device *obddev, int flags)
{
        struct ptlbd_obd *ptlbd = &obddev->u.ptlbd;
        ENTRY;

        /* XXX check for state */

        ptlrpc_stop_all_threads(ptlbd->ptlbd_service);
        ptlrpc_unregister_service(ptlbd->ptlbd_service);
        if ( ! IS_ERR(ptlbd->filp) )
                filp_close(ptlbd->filp, NULL);

        ptlbd_sv_already_setup = 0;
        RETURN(0);
}

static struct obd_ops ptlbd_sv_obd_ops = {
        o_owner:        THIS_MODULE,
        o_setup:        ptlbd_sv_setup,
        o_cleanup:      ptlbd_sv_cleanup,
        o_connect:      class_connect,
        o_disconnect:   class_disconnect,
};

static struct lprocfs_vars lprocfs_obd_vars[] = { {0} };
static struct lprocfs_vars lprocfs_module_vars[] = { {0} };
LPROCFS_INIT_VARS(ptlbd_sv, lprocfs_module_vars, lprocfs_obd_vars)

int ptlbd_sv_init(void)
{
        struct lprocfs_static_vars lvars;

        lprocfs_init_vars(ptlbd_sv,&lvars);
        return class_register_type(&ptlbd_sv_obd_ops, lvars.module_vars,
                                   OBD_PTLBD_SV_DEVICENAME);
}

void ptlbd_sv_exit(void)
{
        class_unregister_type(OBD_PTLBD_SV_DEVICENAME);
}
