/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (c) 2002 Cluster File Systems, Inc.
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
#ifndef __KERNEL__
# include <liblustre.h>
#endif

#include <linux/lustre_dlm.h>
#include <linux/obd_support.h>
#include <linux/lustre_lib.h>

/* This function will be called to judge if one extent overlaps with another */
int ldlm_extent_compat(struct ldlm_lock *a, struct ldlm_lock *b)
{
        if ((a->l_extent.start <= b->l_extent.end) &&
            (a->l_extent.end >=  b->l_extent.start))
                RETURN(0);

        RETURN(1);
}

#include "ldlm_internal.h"

/* The purpose of this function is to return:
 * - the maximum extent
 * - containing the requested extent
 * - and not overlapping existing conflicting extents outside the requested one
 *
 * An alternative policy is to not shrink the new extent when conflicts exist.
 *
 * To reconstruct our formulas, take a deep breath. */
static void policy_internal(struct list_head *queue, struct ldlm_extent *req_ex,
                            struct ldlm_extent *new_ex, ldlm_mode_t mode)
{
        struct list_head *tmp;

        list_for_each(tmp, queue) {
                struct ldlm_lock *lock;
                lock = list_entry(tmp, struct ldlm_lock, l_res_link);

                /* if lock doesn't overlap new_ex, skip it. */
                if (lock->l_extent.end < new_ex->start ||
                    lock->l_extent.start > new_ex->end)
                        continue;

                /* Locks are compatible, overlap doesn't matter */
                if (lockmode_compat(lock->l_req_mode, mode))
                        continue;

                if (lock->l_extent.start < req_ex->start) {
                        if (lock->l_extent.end == ~0) {
                                new_ex->start = req_ex->start;
                                new_ex->end = req_ex->end;
                                return;
                        }
                        new_ex->start = MIN(lock->l_extent.end + 1,
                                            req_ex->start);
                }

                if (lock->l_extent.end > req_ex->end) {
                        if (lock->l_extent.start == 0) {
                                new_ex->start = req_ex->start;
                                new_ex->end = req_ex->end;
                                return;
                        }
                        new_ex->end = MAX(lock->l_extent.start - 1,
                                          req_ex->end);
                }
        }
}

/* apply the internal policy by walking all the lists */
int ldlm_extent_policy(struct ldlm_namespace *ns, struct ldlm_lock **lockp,
                       void *req_cookie, ldlm_mode_t mode, int flags,
                       void *data)
{
        struct ldlm_lock *lock = *lockp;
        struct ldlm_resource *res = lock->l_resource;
        struct ldlm_extent *req_ex = req_cookie;
        struct ldlm_extent new_ex;
        new_ex.start = 0;
        new_ex.end = ~0;

        if (!res)
                LBUG();

        l_lock(&ns->ns_lock);
        policy_internal(&res->lr_granted, req_ex, &new_ex, mode);
        policy_internal(&res->lr_converting, req_ex, &new_ex, mode);
        policy_internal(&res->lr_waiting, req_ex, &new_ex, mode);
        l_unlock(&ns->ns_lock);

        memcpy(&lock->l_extent, &new_ex, sizeof(new_ex));

        LDLM_DEBUG(lock, "requested extent ["LPU64"->"LPU64"], new extent ["
                   LPU64"->"LPU64"]",
                   req_ex->start, req_ex->end, new_ex.start, new_ex.end);

        if (new_ex.end != req_ex->end || new_ex.start != req_ex->start)
                return ELDLM_LOCK_CHANGED;
        else 
                return 0;
}
