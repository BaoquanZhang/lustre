/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (c) 2002, 2003 Cluster File Systems, Inc.
 *   Author: Peter Braam <braam@clusterfs.com>
 *   Author: Phil Schwan <phil@clusterfs.com>
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

#define DEBUG_SUBSYSTEM S_LDLM

#ifdef __KERNEL__
#include <linux/lustre_dlm.h>
#include <linux/obd_support.h>
#include <linux/lustre_lib.h>
#else
#include <liblustre.h>
#endif

#include "ldlm_internal.h"

static inline int
ldlm_plain_compat_queue(struct list_head *queue, struct ldlm_lock *req,
                        int send_cbs)
{
        struct list_head *tmp;
        struct ldlm_lock *lock;
        ldlm_mode_t req_mode = req->l_req_mode;
        int compat = 1;
        ENTRY;

        list_for_each(tmp, queue) {
                lock = list_entry(tmp, struct ldlm_lock, l_res_link);

                if (req == lock)
                        RETURN(compat);

                if (lockmode_compat(lock->l_req_mode, req_mode))
                        continue;

                if (!send_cbs)
                        RETURN(0);

                compat = 0;
                if (lock->l_blocking_ast)
                        ldlm_add_ast_work_item(lock, req, NULL, 0);
        }

        RETURN(compat);
}

/* If first_enq is 0 (ie, called from ldlm_reprocess_queue):
 *   - blocking ASTs have already been sent
 *   - the caller has already initialized req->lr_tmp
 *   - must call this function with the ns lock held
 *
 * If first_enq is 1 (ie, called from ldlm_lock_enqueue):
 *   - blocking ASTs have not been sent
 *   - the caller has NOT initialized req->lr_tmp, so we must
 *   - must call this function with the ns lock held once */
int ldlm_process_plain_lock(struct ldlm_lock *lock, int *flags, int first_enq,
                            ldlm_error_t *err)
{
        struct ldlm_resource *res = lock->l_resource;
        struct list_head rpc_list = LIST_HEAD_INIT(rpc_list);
        int rc;
        ENTRY;

        LASSERT(list_empty(&res->lr_converting));

        if (!first_enq) {
                LASSERT(res->lr_tmp != NULL);
                rc = ldlm_plain_compat_queue(&res->lr_granted, lock, 0);
                if (!rc)
                        RETURN(LDLM_ITER_STOP);
                rc = ldlm_plain_compat_queue(&res->lr_waiting, lock, 0);
                if (!rc)
                        RETURN(LDLM_ITER_STOP);

                ldlm_resource_unlink_lock(lock);
                ldlm_grant_lock(lock, NULL, 0, 1);
                RETURN(LDLM_ITER_CONTINUE);
        }

 restart:
        LASSERT(res->lr_tmp == NULL);
        res->lr_tmp = &rpc_list;
        rc = ldlm_plain_compat_queue(&res->lr_granted, lock, 1);
        rc += ldlm_plain_compat_queue(&res->lr_waiting, lock, 1);
        res->lr_tmp = NULL;

        if (rc != 2) {
                /* If either of the compat_queue()s returned 0, then we
                 * have ASTs to send and must go onto the waiting list. */
                ldlm_resource_unlink_lock(lock);
                ldlm_resource_add_lock(res, &res->lr_waiting, lock);
                l_unlock(&res->lr_namespace->ns_lock);
                rc = ldlm_run_ast_work(res->lr_namespace, &rpc_list);
                l_lock(&res->lr_namespace->ns_lock);
                if (rc == -ERESTART)
                        GOTO(restart, -ERESTART);
                *flags |= LDLM_FL_BLOCK_GRANTED;
        } else {
                ldlm_resource_unlink_lock(lock);
                ldlm_grant_lock(lock, NULL, 0, 0);
        }
        RETURN(0);
}
