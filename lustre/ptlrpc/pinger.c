/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Portal-RPC reconnection and replay operations, for use in recovery.
 *
 *  Copyright (c) 2003 Cluster File Systems, Inc.
 *   Authors: Phil Schwan <phil@clusterfs.com>
 *            Mike Shaver <shaver@clusterfs.com>
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

#ifndef __KERNEL__
#include <liblustre.h>
#else
#include <linux/version.h>
#include <asm/semaphore.h>
#define DEBUG_SUBSYSTEM S_RPC
#endif

#include <linux/obd_support.h>
#include <linux/obd_class.h>
#include "ptlrpc_internal.h"

static DECLARE_MUTEX(pinger_sem);
static struct list_head pinger_imports = LIST_HEAD_INIT(pinger_imports);

int ptlrpc_ping(struct obd_import *imp) 
{
        struct ptlrpc_request *req;
        int rc = 0;
        ENTRY;

        req = ptlrpc_prep_req(imp, OBD_PING, 0, NULL,
                              NULL);
        if (req) {
                DEBUG_REQ(D_HA, req, "pinging %s->%s",
                          imp->imp_obd->obd_uuid.uuid,
                          imp->imp_target_uuid.uuid);
                req->rq_no_resend = req->rq_no_delay = 1;
                req->rq_replen = lustre_msg_size(0, 
                                                 NULL);
                ptlrpcd_add_req(req);
        } else {
                CERROR("OOM trying to ping %s->%s\n",
                          imp->imp_obd->obd_uuid.uuid,
                          imp->imp_target_uuid.uuid);
                rc = -ENOMEM;
        }

        RETURN(rc);
}

#ifdef __KERNEL__
static int ptlrpc_pinger_main(void *arg)
{
        struct ptlrpc_svc_data *data = (struct ptlrpc_svc_data *)arg;
        struct ptlrpc_thread *thread = data->thread;
        unsigned long flags;
        ENTRY;

        lock_kernel();
        ptlrpc_daemonize();

        SIGNAL_MASK_LOCK(current, flags);
        sigfillset(&current->blocked);
        RECALC_SIGPENDING;
        SIGNAL_MASK_UNLOCK(current, flags);

        LASSERTF(strlen(data->name) < sizeof(current->comm),
                 "name %d > len %d\n",
                 (int)strlen(data->name), (int)sizeof(current->comm));
        THREAD_NAME(current->comm, sizeof(current->comm) - 1, "%s", data->name);
        unlock_kernel();

        /* Record that the thread is running */
        thread->t_flags = SVC_RUNNING;
        wake_up(&thread->t_ctl_waitq);

        /* And now, loop forever, pinging as needed. */
        while (1) {
                unsigned long this_ping = jiffies;
                long time_to_next_ping;
                struct l_wait_info lwi = LWI_TIMEOUT(obd_timeout * HZ,
                                                     NULL, NULL);
                struct list_head *iter;

                down(&pinger_sem);
                list_for_each(iter, &pinger_imports) {
                        struct obd_import *imp =
                                list_entry(iter, struct obd_import,
                                           imp_pinger_chain);
                        int force, level;
                        unsigned long flags;


                        spin_lock_irqsave(&imp->imp_lock, flags);
                        level = imp->imp_state;
                        force = imp->imp_force_verify;
                        if (force)
                                imp->imp_force_verify = 0;
                        spin_unlock_irqrestore(&imp->imp_lock, flags);

                        if (imp->imp_next_ping <= this_ping || force) {
                                if (level == LUSTRE_IMP_DISCON && 
                                    !imp->imp_deactive) {
                                        /* wait at least a timeout before 
                                           trying recovery again. */
                                        imp->imp_next_ping = jiffies + 
                                                (obd_timeout * HZ);
                                        ptlrpc_initiate_recovery(imp);
                                } 
                                else if (level != LUSTRE_IMP_FULL ||
                                         imp->imp_obd->obd_no_recov) {
                                        CDEBUG(D_HA, 
                                               "not pinging %s (in recovery "
                                               " or recovery disabled: %s)\n",
                                               imp->imp_target_uuid.uuid,
                                               ptlrpc_import_state_name(level));
                                } 
                                else if (imp->imp_pingable || force) {
                                        ptlrpc_ping(imp);
                                }

                        } else {
                                if (imp->imp_pingable)
                                        CDEBUG(D_HA, "don't need to ping %s "
                                               "(%lu > %lu)\n", 
                                               imp->imp_target_uuid.uuid,
                                               imp->imp_next_ping, this_ping);
                        }
                }
                up(&pinger_sem);

                /* Wait until the next ping time, or until we're stopped. */
                time_to_next_ping = this_ping + (obd_timeout * HZ) - jiffies;
                CDEBUG(D_HA, "next ping in %lu (%lu)\n", time_to_next_ping,
                       this_ping + (obd_timeout * HZ));
                if (time_to_next_ping > 0) {
                        lwi = LWI_TIMEOUT(time_to_next_ping, NULL, NULL);
                        l_wait_event(thread->t_ctl_waitq,
                                     thread->t_flags & (SVC_STOPPING|SVC_EVENT),
                                     &lwi);
                        if (thread->t_flags & SVC_STOPPING) {
                                thread->t_flags &= ~SVC_STOPPING;
                                EXIT;
                                break;
                        } else if (thread->t_flags & SVC_EVENT) {
                                /* woken after adding import to reset timer */
                                thread->t_flags &= ~SVC_EVENT;
                        }
                }
        }

        thread->t_flags = SVC_STOPPED;
        wake_up(&thread->t_ctl_waitq);

        CDEBUG(D_NET, "pinger thread exiting, process %d\n", current->pid);
        return 0;
}

static struct ptlrpc_thread *pinger_thread = NULL;

int ptlrpc_start_pinger(void)
{
        struct l_wait_info lwi = { 0 };
        struct ptlrpc_svc_data d;
        int rc;
#ifndef ENABLE_PINGER
        return 0;
#endif
        ENTRY;

        if (pinger_thread != NULL)
                RETURN(-EALREADY);

        OBD_ALLOC(pinger_thread, sizeof(*pinger_thread));
        if (pinger_thread == NULL)
                RETURN(-ENOMEM);
        init_waitqueue_head(&pinger_thread->t_ctl_waitq);

        d.name = "ll_ping";
        d.thread = pinger_thread;

        /* CLONE_VM and CLONE_FILES just avoid a needless copy, because we
         * just drop the VM and FILES in ptlrpc_daemonize() right away. */
        rc = kernel_thread(ptlrpc_pinger_main, &d, CLONE_VM | CLONE_FILES);
        if (rc < 0) {
                CERROR("cannot start thread: %d\n", rc);
                OBD_FREE(pinger_thread, sizeof(*pinger_thread));
                RETURN(rc);
        }
        l_wait_event(pinger_thread->t_ctl_waitq,
                     pinger_thread->t_flags & SVC_RUNNING, &lwi);

        RETURN(rc);
}

int ptlrpc_stop_pinger(void)
{
        struct l_wait_info lwi = { 0 };
        int rc = 0;
#ifndef ENABLE_PINGER
        return 0;
#endif
        ENTRY;

        if (pinger_thread == NULL)
                RETURN(-EALREADY);
        down(&pinger_sem);
        pinger_thread->t_flags = SVC_STOPPING;
        wake_up(&pinger_thread->t_ctl_waitq);
        up(&pinger_sem);

        l_wait_event(pinger_thread->t_ctl_waitq,
                     (pinger_thread->t_flags & SVC_STOPPED), &lwi);

        OBD_FREE(pinger_thread, sizeof(*pinger_thread));
        pinger_thread = NULL;
        RETURN(rc);
}

void ptlrpc_pinger_sending_on_import(struct obd_import *imp)
{
        down(&pinger_sem);
        imp->imp_next_ping = jiffies + (obd_timeout * HZ);
        up(&pinger_sem);
}

int ptlrpc_pinger_add_import(struct obd_import *imp)
{
        ENTRY;
        if (!list_empty(&imp->imp_pinger_chain))
                RETURN(-EALREADY);

        down(&pinger_sem);
        CDEBUG(D_HA, "adding pingable import %s->%s\n",
               imp->imp_obd->obd_uuid.uuid, imp->imp_target_uuid.uuid);
        imp->imp_next_ping = jiffies + (obd_timeout * HZ);
        /* XXX sort, blah blah */
        list_add_tail(&imp->imp_pinger_chain, &pinger_imports);
        class_import_get(imp);

        ptlrpc_pinger_wake_up();
        up(&pinger_sem);

        RETURN(0);
}

int ptlrpc_pinger_del_import(struct obd_import *imp)
{
        ENTRY;
        if (list_empty(&imp->imp_pinger_chain))
                RETURN(-ENOENT);

        down(&pinger_sem);
        list_del_init(&imp->imp_pinger_chain);
        CDEBUG(D_HA, "removing pingable import %s->%s\n",
               imp->imp_obd->obd_uuid.uuid, imp->imp_target_uuid.uuid);
        class_import_put(imp);
        up(&pinger_sem);
        RETURN(0);
}

void ptlrpc_pinger_wake_up()
{
#ifdef ENABLE_PINGER
        pinger_thread->t_flags |= SVC_EVENT;
        wake_up(&pinger_thread->t_ctl_waitq);
#endif
}

#else /* !__KERNEL__ */

/* XXX
 * the current implementation of pinger in liblustre is not optimized
 */

static struct pinger_data {
        int             pd_recursion;
        unsigned long   pd_this_ping;
        unsigned long   pd_next_ping;
        struct ptlrpc_request_set *pd_set;
} pinger_args;

static int pinger_check_rpcs(void *arg)
{
        unsigned long curtime = time(NULL);
        struct ptlrpc_request *req;
        struct ptlrpc_request_set *set;
        struct list_head *iter;
        struct pinger_data *pd = &pinger_args;
        int rc;

        /* prevent recursion */
        if (pd->pd_recursion++) {
                CDEBUG(D_HA, "pinger: recursion! quit\n");
                LASSERT(pd->pd_set);
                pd->pd_recursion--;
                return 0;
        }

        /* have we reached ping point? */
        if (!pd->pd_set && pd->pd_next_ping > curtime) {
                pd->pd_recursion--;
                return 0;
        }

        /* if we have rpc_set already, continue processing it */
        if (pd->pd_set) {
                LASSERT(pd->pd_this_ping);
                set = pd->pd_set;
                goto do_check_set;
        }

        pd->pd_this_ping = curtime;
        pd->pd_set = ptlrpc_prep_set();
        set = pd->pd_set;

        /* add rpcs into set */
        down(&pinger_sem);
        list_for_each(iter, &pinger_imports) {
                struct obd_import *imp =
                        list_entry(iter, struct obd_import,
                                   imp_pinger_chain);
                int generation, level;
                unsigned long flags;

                if (imp->imp_next_ping <= pd->pd_this_ping) {
                        /* Add a ping. */
                        spin_lock_irqsave(&imp->imp_lock, flags);
                        generation = imp->imp_generation;
                        level = imp->imp_state;
                        spin_unlock_irqrestore(&imp->imp_lock, flags);

                        if (level != LUSTRE_IMP_FULL) {
                                CDEBUG(D_HA,
                                       "not pinging %s (in recovery)\n",
                                       imp->imp_target_uuid.uuid);
                                continue;
                        }

                        req = ptlrpc_prep_req(imp, OBD_PING, 0, NULL,
                                              NULL);
                        if (!req) {
                                CERROR("out of memory\n");
                                break;
                        }
                        req->rq_no_resend = 1;
                        req->rq_replen = lustre_msg_size(0, NULL);
                        req->rq_send_state = LUSTRE_IMP_FULL;
                        req->rq_phase = RQ_PHASE_RPC;
                        req->rq_import_generation = generation;
                        ptlrpc_set_add_req(set, req);
                } else {
                        CDEBUG(D_HA, "don't need to ping %s (%lu > "
                               "%lu)\n", imp->imp_target_uuid.uuid,
                               imp->imp_next_ping, pd->pd_this_ping);
                }
        }
        pd->pd_this_ping = curtime;
        up(&pinger_sem);

        /* Might be empty, that's OK. */
        if (set->set_remaining == 0)
                CDEBUG(D_HA, "nothing to ping\n");

        list_for_each(iter, &set->set_requests) {
                struct ptlrpc_request *req =
                        list_entry(iter, struct ptlrpc_request,
                                   rq_set_chain);
                DEBUG_REQ(D_HA, req, "pinging %s->%s",
                          req->rq_import->imp_obd->obd_uuid.uuid,
                          req->rq_import->imp_target_uuid.uuid);
                (void)ptl_send_rpc(req);
        }

do_check_set:
        rc = ptlrpc_check_set(set);

        /* not finished, and we are not expired, simply return */
        if (!rc && curtime < pd->pd_this_ping + obd_timeout) {
                CDEBUG(D_HA, "not finished, but also not expired\n");
                pd->pd_recursion--;
                return 0;
        }

        /* Expire all the requests that didn't come back. */
        down(&pinger_sem);
        list_for_each(iter, &set->set_requests) {
                req = list_entry(iter, struct ptlrpc_request,
                                 rq_set_chain);

                if (req->rq_replied)
                        continue;

                req->rq_phase = RQ_PHASE_COMPLETE;
                set->set_remaining--;
                /* If it was disconnected, don't sweat it. */
                if (list_empty(&req->rq_import->imp_pinger_chain)) {
                        ptlrpc_unregister_reply(req);
                        continue;
                }

                CDEBUG(D_HA, "pinger initiate expire_one_request\n");
                ptlrpc_expire_one_request(req);
        }
        up(&pinger_sem);

        ptlrpc_set_destroy(set);
        pd->pd_set = NULL;

        pd->pd_next_ping = pd->pd_this_ping + obd_timeout;
        pd->pd_this_ping = 0; /* XXX for debug */

        CDEBUG(D_HA, "finished a round ping\n");
        pd->pd_recursion--;
        return 0;
}

static void *pinger_callback = NULL;

int ptlrpc_start_pinger(void)
{
        memset(&pinger_args, 0, sizeof(pinger_args));
#ifdef ENABLE_PINGER
        pinger_callback =
                liblustre_register_wait_callback(&pinger_check_rpcs, &pinger_args);
#endif
        return 0;
}

int ptlrpc_stop_pinger(void)
{
#ifdef ENABLE_PINGER
        if (pinger_callback)
                liblustre_deregister_wait_callback(pinger_callback);
#endif
        return 0;
}

void ptlrpc_pinger_sending_on_import(struct obd_import *imp)
{
        down(&pinger_sem);
        imp->imp_next_ping = time(NULL) + obd_timeout;
        if (pinger_args.pd_set == NULL &&
            pinger_args.pd_next_ping > imp->imp_next_ping) {
                CDEBUG(D_HA, "set next ping to %ld(cur %ld)\n",
                        imp->imp_next_ping, time(NULL));
                pinger_args.pd_next_ping = imp->imp_next_ping;
        }
        up(&pinger_sem);
}

int ptlrpc_pinger_add_import(struct obd_import *imp)
{
        ENTRY;
        if (!list_empty(&imp->imp_pinger_chain))
                RETURN(-EALREADY);

        CDEBUG(D_HA, "adding pingable import %s->%s\n",
               imp->imp_obd->obd_uuid.uuid, imp->imp_target_uuid.uuid);
        ptlrpc_pinger_sending_on_import(imp);

        down(&pinger_sem);
        list_add_tail(&imp->imp_pinger_chain, &pinger_imports);
        class_import_get(imp);
        up(&pinger_sem);

        RETURN(0);
}

int ptlrpc_pinger_del_import(struct obd_import *imp)
{
        ENTRY;
        if (list_empty(&imp->imp_pinger_chain))
                RETURN(-ENOENT);

        down(&pinger_sem);
        list_del_init(&imp->imp_pinger_chain);
        CDEBUG(D_HA, "removing pingable import %s->%s\n",
               imp->imp_obd->obd_uuid.uuid, imp->imp_target_uuid.uuid);
        class_import_put(imp);
        up(&pinger_sem);
        RETURN(0);
}

void ptlrpc_pinger_wake_up()
{
#ifdef ENABLE_PINGER
        /* XXX force pinger to run, if needed */
#endif
}
#endif /* !__KERNEL__ */
