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
#include <linux/obd_class.h>
#include <linux/lustre_lib.h>
#include <portals/list.h>
#else
#include <liblustre.h>
#endif

#include "ldlm_internal.h"

#define l_flock_waitq   l_lru

static struct list_head ldlm_flock_waitq = LIST_HEAD_INIT(ldlm_flock_waitq);

int ldlm_flock_blocking_ast(struct ldlm_lock *lock, struct ldlm_lock_desc *desc,
                            void *data, int flag);

/**
 * list_for_remaining_safe - iterate over the remaining entries in a list
 *              and safeguard against removal of a list entry.
 * @pos:        the &struct list_head to use as a loop counter. pos MUST
 *              have been initialized prior to using it in this macro.
 * @n:          another &struct list_head to use as temporary storage
 * @head:       the head for your list.
 */
#define list_for_remaining_safe(pos, n, head) \
        for (n = pos->next; pos != (head); pos = n, n = pos->next)

static inline int
ldlm_same_flock_owner(struct ldlm_lock *lock, struct ldlm_lock *new)
{
        return((new->l_policy_data.l_flock.pid ==
                lock->l_policy_data.l_flock.pid) &&
               (new->l_export == lock->l_export));
}

static inline int
ldlm_flocks_overlap(struct ldlm_lock *lock, struct ldlm_lock *new)
{
        return((new->l_policy_data.l_flock.start <=
                lock->l_policy_data.l_flock.end) &&
               (new->l_policy_data.l_flock.end >=
                lock->l_policy_data.l_flock.start));
}

static inline void
ldlm_flock_destroy(struct ldlm_lock *lock, ldlm_mode_t mode, int flags)
{
        ENTRY;

        LDLM_DEBUG(lock, "ldlm_flock_destroy(mode: %d, flags: 0x%x)",
                   mode, flags);

        LASSERT(list_empty(&lock->l_flock_waitq));

        list_del_init(&lock->l_res_link);
        if (flags == LDLM_FL_WAIT_NOREPROC) {
                /* client side - set a flag to prevent sending a CANCEL */
                lock->l_flags |= LDLM_FL_LOCAL_ONLY | LDLM_FL_CBPENDING;
                ldlm_lock_decref_internal(lock, mode);
        }

        ldlm_lock_destroy(lock);
        EXIT;
}

static int
ldlm_flock_deadlock(struct ldlm_lock *req, struct ldlm_lock *blocking_lock)
{
        struct obd_export *req_export = req->l_export;
        struct obd_export *blocking_export = blocking_lock->l_export;
        pid_t req_pid = req->l_policy_data.l_flock.pid;
        pid_t blocking_pid = blocking_lock->l_policy_data.l_flock.pid;
        struct ldlm_lock *lock;

restart:
        list_for_each_entry(lock, &ldlm_flock_waitq, l_flock_waitq) {
                if ((lock->l_policy_data.l_flock.pid != blocking_pid) ||
                    (lock->l_export != blocking_export))
                        continue;

                blocking_pid = lock->l_policy_data.l_flock.blocking_pid;
                blocking_export = (struct obd_export *)(long)
                        lock->l_policy_data.l_flock.blocking_export;
                if (blocking_pid == req_pid && blocking_export == req_export)
                        return 1;

                goto restart;
        }

        return 0;
}

int
ldlm_process_flock_lock(struct ldlm_lock *req, int *flags, int first_enq,
                        ldlm_error_t *err)
{
        struct ldlm_resource *res = req->l_resource;
        struct ldlm_namespace *ns = res->lr_namespace;
        struct list_head *tmp;
        struct list_head *ownlocks = NULL;
        struct ldlm_lock *lock = NULL;
        struct ldlm_lock *new = req;
        struct ldlm_lock *new2 = NULL;
        ldlm_mode_t mode = req->l_req_mode;
        int local = ns->ns_client;
        int added = (mode == LCK_NL);
        int overlaps = 0;
        ENTRY;

        CDEBUG(D_DLMTRACE, "flags %#x pid "LPU64" mode %u start "LPU64" end "
               LPU64"\n", *flags, new->l_policy_data.l_flock.pid, mode,
               req->l_policy_data.l_flock.start,
               req->l_policy_data.l_flock.end);

        *err = ELDLM_OK;

        if (local) {
                /* No blocking ASTs are sent to the clients for
                 * Posix file & record locks */
                req->l_blocking_ast = NULL;
        } else {
                /* Called on the server for lock cancels. */
                req->l_blocking_ast = ldlm_flock_blocking_ast;
        }

        if ((*flags == LDLM_FL_WAIT_NOREPROC) || (mode == LCK_NL)) {
                /* This loop determines where this processes locks start
                 * in the resource lr_granted list. */
                list_for_each(tmp, &res->lr_granted) {
                        lock = list_entry(tmp, struct ldlm_lock, l_res_link);
                        if (ldlm_same_flock_owner(lock, req)) {
                                ownlocks = tmp;
                                break;
                        }
                }
        } else {
                lockmode_verify(mode);

                /* This loop determines if there are existing locks
                 * that conflict with the new lock request. */
                list_for_each(tmp, &res->lr_granted) {
                        lock = list_entry(tmp, struct ldlm_lock, l_res_link);

                        if (ldlm_same_flock_owner(lock, req)) {
                                if (!ownlocks)
                                        ownlocks = tmp;
                                continue;
                        }

                        /* locks are compatible, overlap doesn't matter */
                        if (lockmode_compat(lock->l_granted_mode, mode))
                                continue;

                        if (!ldlm_flocks_overlap(lock, req))
                                continue;

                        if (!first_enq)
                                RETURN(LDLM_ITER_CONTINUE);

                        if (*flags & LDLM_FL_BLOCK_NOWAIT) {
                                ldlm_flock_destroy(req, mode, *flags);
                                *err = -EAGAIN;
                                RETURN(LDLM_ITER_STOP);
                        }

                        if (*flags & LDLM_FL_TEST_LOCK) {
                                ldlm_flock_destroy(req, mode, *flags);
                                req->l_req_mode = lock->l_granted_mode;
                                req->l_policy_data.l_flock.pid =
                                        lock->l_policy_data.l_flock.pid;
                                req->l_policy_data.l_flock.start =
                                        lock->l_policy_data.l_flock.start;
                                req->l_policy_data.l_flock.end =
                                        lock->l_policy_data.l_flock.end;
                                *flags |= LDLM_FL_LOCK_CHANGED;
                                RETURN(LDLM_ITER_STOP);
                        }

                        if (ldlm_flock_deadlock(req, lock)) {
                                ldlm_flock_destroy(req, mode, *flags);
                                *err = -EDEADLK;
                                RETURN(LDLM_ITER_STOP);
                        }

                        req->l_policy_data.l_flock.blocking_pid =
                                lock->l_policy_data.l_flock.pid;
                        req->l_policy_data.l_flock.blocking_export =
                                (long)(void *)lock->l_export;

                        LASSERT(list_empty(&req->l_flock_waitq));
                        list_add_tail(&req->l_flock_waitq, &ldlm_flock_waitq);

                        ldlm_resource_add_lock(res, &res->lr_waiting, req);
                        *flags |= LDLM_FL_BLOCK_GRANTED;
                        RETURN(LDLM_ITER_STOP);
                }
        }

        if (*flags & LDLM_FL_TEST_LOCK) {
                ldlm_flock_destroy(req, mode, *flags);
                req->l_req_mode = LCK_NL;
                *flags |= LDLM_FL_LOCK_CHANGED;
                RETURN(LDLM_ITER_STOP);
        }

        /* In case we had slept on this lock request take it off of the
         * deadlock detection waitq. */
        list_del_init(&req->l_flock_waitq);

        /* Scan the locks owned by this process that overlap this request.
         * We may have to merge or split existing locks. */

        if (!ownlocks)
                ownlocks = &res->lr_granted;

        list_for_remaining_safe(ownlocks, tmp, &res->lr_granted) {
                lock = list_entry(ownlocks, struct ldlm_lock, l_res_link);

                if (!ldlm_same_flock_owner(lock, new))
                        break;

                if (lock->l_granted_mode == mode) {
                        /* If the modes are the same then we need to process
                         * locks that overlap OR adjoin the new lock. The extra
                         * logic condition is necessary to deal with arithmetic
                         * overflow and underflow. */
                        if ((new->l_policy_data.l_flock.start >
                             (lock->l_policy_data.l_flock.end + 1))
                            && (lock->l_policy_data.l_flock.end != ~0))
                                continue;

                        if ((new->l_policy_data.l_flock.end <
                             (lock->l_policy_data.l_flock.start - 1))
                            && (lock->l_policy_data.l_flock.start != 0))
                                break;

                        if (new->l_policy_data.l_flock.start <
                            lock->l_policy_data.l_flock.start) {
                                lock->l_policy_data.l_flock.start =
                                        new->l_policy_data.l_flock.start;
                        } else {
                                new->l_policy_data.l_flock.start =
                                        lock->l_policy_data.l_flock.start;
                        }

                        if (new->l_policy_data.l_flock.end >
                            lock->l_policy_data.l_flock.end) {
                                lock->l_policy_data.l_flock.end =
                                        new->l_policy_data.l_flock.end;
                        } else {
                                new->l_policy_data.l_flock.end =
                                        lock->l_policy_data.l_flock.end;
                        }

                        if (added) {
                                ldlm_flock_destroy(lock, mode, *flags);
                        } else {
                                new = lock;
                                added = 1;
                        }
                        continue;
                }

                if (new->l_policy_data.l_flock.start >
                    lock->l_policy_data.l_flock.end)
                        continue;

                if (new->l_policy_data.l_flock.end <
                    lock->l_policy_data.l_flock.start)
                        break;

                ++overlaps;

                if (new->l_policy_data.l_flock.start <=
                    lock->l_policy_data.l_flock.start) {
                        if (new->l_policy_data.l_flock.end <
                            lock->l_policy_data.l_flock.end) {
                                lock->l_policy_data.l_flock.start =
                                        new->l_policy_data.l_flock.end + 1;
                                break;
                        }
                        ldlm_flock_destroy(lock, lock->l_req_mode, *flags);
                        continue;
                }
                if (new->l_policy_data.l_flock.end >=
                    lock->l_policy_data.l_flock.end) {
                        lock->l_policy_data.l_flock.end =
                                new->l_policy_data.l_flock.start - 1;
                        continue;
                }

                /* split the existing lock into two locks */

                /* if this is an F_UNLCK operation then we could avoid
                 * allocating a new lock and use the req lock passed in
                 * with the request but this would complicate the reply
                 * processing since updates to req get reflected in the
                 * reply. The client side replays the lock request so
                 * it must see the original lock data in the reply. */

                /* XXX - if ldlm_lock_new() can sleep we should
                 * release the ns_lock, allocate the new lock,
                 * and restart processing this lock. */
                new2 = ldlm_lock_create(ns, NULL, res->lr_name, LDLM_FLOCK,
                                        lock->l_granted_mode, NULL, NULL, NULL,
                                        NULL, 0);
                if (!new2) {
                        ldlm_flock_destroy(req, lock->l_granted_mode, *flags);
                        *err = -ENOLCK;
                        RETURN(LDLM_ITER_STOP);
                }

                new2->l_granted_mode = lock->l_granted_mode;
                new2->l_policy_data.l_flock.pid =
                        new->l_policy_data.l_flock.pid;
                new2->l_policy_data.l_flock.start =
                        lock->l_policy_data.l_flock.start;
                new2->l_policy_data.l_flock.end =
                        new->l_policy_data.l_flock.start - 1;
                lock->l_policy_data.l_flock.start =
                        new->l_policy_data.l_flock.end + 1;
                new2->l_conn_export = lock->l_conn_export;
                if (lock->l_export != NULL) {
                        new2->l_export = class_export_get(lock->l_export);
                        list_add(&new2->l_export_chain,
                                 &new2->l_export->exp_ldlm_data.led_held_locks);
                }
                if (*flags == LDLM_FL_WAIT_NOREPROC)
                        ldlm_lock_addref_internal(new2, lock->l_granted_mode);

                /* insert new2 at lock */
                ldlm_resource_add_lock(res, ownlocks, new2);
                LDLM_LOCK_PUT(new2);
                break;
        }

        /* At this point we're granting the lock request. */
        req->l_granted_mode = req->l_req_mode;

        /* Add req to the granted queue before calling ldlm_reprocess_all(). */
        if (!added) {
                list_del_init(&req->l_res_link);
                /* insert new lock before ownlocks in list. */
                ldlm_resource_add_lock(res, ownlocks, req);
        }

        if (*flags != LDLM_FL_WAIT_NOREPROC) {
                if (first_enq) {
                        /* If this is an unlock, reprocess the waitq and
                         * send completions ASTs for locks that can now be 
                         * granted. The only problem with doing this
                         * reprocessing here is that the completion ASTs for
                         * newly granted locks will be sent before the unlock
                         * completion is sent. It shouldn't be an issue. Also
                         * note that ldlm_process_flock_lock() will recurse,
                         * but only once because first_enq will be false from
                         * ldlm_reprocess_queue. */
                        if ((mode == LCK_NL) && overlaps) {
                                struct list_head rpc_list
                                                    = LIST_HEAD_INIT(rpc_list);
                                int rc;
restart:
                                res->lr_tmp = &rpc_list;
                                ldlm_reprocess_queue(res, &res->lr_waiting);
                                res->lr_tmp = NULL;

                                l_unlock(&ns->ns_lock);
                                rc = ldlm_run_ast_work(res->lr_namespace,
                                                       &rpc_list);
                                l_lock(&ns->ns_lock);
                                if (rc == -ERESTART)
                                        GOTO(restart, -ERESTART);
                       }
                } else {
                        LASSERT(req->l_completion_ast);
                        ldlm_add_ast_work_item(req, NULL, NULL, 0);
                }
        }

        /* In case we're reprocessing the requested lock we can't destroy
         * it until after calling ldlm_ast_work_item() above so that lawi()
         * can bump the reference count on req. Otherwise req could be freed
         * before the completion AST can be sent.  */
        if (added)
                ldlm_flock_destroy(req, mode, *flags);

        ldlm_resource_dump(res);
        RETURN(LDLM_ITER_CONTINUE);
}

struct ldlm_flock_wait_data {
        struct ldlm_lock *fwd_lock;
        int               fwd_generation;
};

static void
ldlm_flock_interrupted_wait(void *data)
{
        struct ldlm_lock *lock;
        struct lustre_handle lockh;
        int rc;
        ENTRY;

        lock = ((struct ldlm_flock_wait_data *)data)->fwd_lock;

        /* take lock off the deadlock detection waitq. */
        list_del_init(&lock->l_flock_waitq);

        ldlm_lock_decref_internal(lock, lock->l_req_mode);
        ldlm_lock2handle(lock, &lockh);
        rc = ldlm_cli_cancel(&lockh);
        EXIT;
}

int
ldlm_flock_completion_ast(struct ldlm_lock *lock, int flags, void *data)
{
        struct ldlm_namespace *ns;
        struct file_lock *getlk = lock->l_ast_data;
        struct ldlm_flock_wait_data fwd;
        unsigned long irqflags;
        struct obd_device *obd;
        struct obd_import *imp = NULL;
        ldlm_error_t err;
        int rc = 0;
        struct l_wait_info lwi;
        ENTRY;

        CDEBUG(D_DLMTRACE, "flags: 0x%x data: %p getlk: %p\n",
               flags, data, getlk);

        LASSERT(flags != LDLM_FL_WAIT_NOREPROC);

        if (flags == 0) {
                wake_up(&lock->l_waitq);
                RETURN(0);
        }

        if (!(flags & (LDLM_FL_BLOCK_WAIT | LDLM_FL_BLOCK_GRANTED |
                       LDLM_FL_BLOCK_CONV)))
                goto  granted;

        LDLM_DEBUG(lock, "client-side enqueue returned a blocked lock, "
                   "sleeping");

        ldlm_lock_dump(D_DLMTRACE, lock, 0);

        fwd.fwd_lock = lock;
        obd = class_exp2obd(lock->l_conn_export);

        /* if this is a local lock, then there is no import */
        if (obd != NULL)
                imp = obd->u.cli.cl_import;

        if (imp != NULL) {
                spin_lock_irqsave(&imp->imp_lock, irqflags);
                fwd.fwd_generation = imp->imp_generation;
                spin_unlock_irqrestore(&imp->imp_lock, irqflags);
        }

        lwi = LWI_TIMEOUT_INTR(0, NULL, ldlm_flock_interrupted_wait, &fwd);

        /* Go to sleep until the lock is granted. */
        rc = l_wait_event(lock->l_waitq,
                          ((lock->l_req_mode == lock->l_granted_mode) ||
                           lock->l_destroyed), &lwi);

        if (rc) {
                LDLM_DEBUG(lock, "client-side enqueue waking up: failed (%d)",
                           rc);
                RETURN(rc);
        }

        LASSERT(!(lock->l_destroyed));

granted:

        LDLM_DEBUG(lock, "client-side enqueue waking up");
        ns = lock->l_resource->lr_namespace;
        l_lock(&ns->ns_lock);

        /* take lock off the deadlock detection waitq. */
        list_del_init(&lock->l_flock_waitq);

        /* ldlm_lock_enqueue() has already placed lock on the granted list. */
        list_del_init(&lock->l_res_link);

        if (flags & LDLM_FL_TEST_LOCK) {
                /* fcntl(F_GETLK) request */
                /* The old mode was saved in getlk->fl_type so that if the mode
                 * in the lock changes we can decref the approprate refcount. */
                ldlm_flock_destroy(lock, getlk->fl_type, LDLM_FL_WAIT_NOREPROC);
                switch (lock->l_granted_mode) {
                case LCK_PR:
                        getlk->fl_type = F_RDLCK;
                        break;
                case LCK_PW:
                        getlk->fl_type = F_WRLCK;
                        break;
                default:
                        getlk->fl_type = F_UNLCK;
                }
                getlk->fl_pid = lock->l_policy_data.l_flock.pid;
                getlk->fl_start = lock->l_policy_data.l_flock.start;
                getlk->fl_end = lock->l_policy_data.l_flock.end;
        } else {
                /* We need to reprocess the lock to do merges or splits
                 * with existing locks owned by this process. */
                flags = LDLM_FL_WAIT_NOREPROC;
                ldlm_process_flock_lock(lock, &flags, 1, &err);
        }
        l_unlock(&ns->ns_lock);
        RETURN(0);
}

int ldlm_flock_blocking_ast(struct ldlm_lock *lock, struct ldlm_lock_desc *desc,
                            void *data, int flag)
{
        struct ldlm_namespace *ns;
        ENTRY;

        LASSERT(lock);
        LASSERT(flag == LDLM_CB_CANCELING);

        ns = lock->l_resource->lr_namespace;
        
        /* take lock off the deadlock detection waitq. */
        l_lock(&ns->ns_lock);
        list_del_init(&lock->l_flock_waitq);
        l_unlock(&ns->ns_lock);
        RETURN(0);
}
