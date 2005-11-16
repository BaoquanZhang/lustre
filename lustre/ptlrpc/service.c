/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (C) 2002 Cluster File Systems, Inc.
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
 *
 */

#define DEBUG_SUBSYSTEM S_RPC
#ifndef __KERNEL__
#include <liblustre.h>
#include <libcfs/kp30.h>
#endif
#include <linux/obd_support.h>
#include <linux/obd_class.h>
#include <linux/lustre_net.h>
#include <lnet/types.h>
#include "ptlrpc_internal.h"

/* forward ref */
static int ptlrpc_server_post_idle_rqbds (struct ptlrpc_service *svc);

static LIST_HEAD (ptlrpc_all_services);
static spinlock_t ptlrpc_all_services_lock = SPIN_LOCK_UNLOCKED;

static char *
ptlrpc_alloc_request_buffer (int size)
{
        char *ptr;

        if (size > SVC_BUF_VMALLOC_THRESHOLD)
                OBD_VMALLOC(ptr, size);
        else
                OBD_ALLOC(ptr, size);

        return (ptr);
}

static void
ptlrpc_free_request_buffer (char *ptr, int size)
{
        if (size > SVC_BUF_VMALLOC_THRESHOLD)
                OBD_VFREE(ptr, size);
        else
                OBD_FREE(ptr, size);
}

struct ptlrpc_request_buffer_desc *
ptlrpc_alloc_rqbd (struct ptlrpc_service *svc)
{
        unsigned long                      flags;
        struct ptlrpc_request_buffer_desc *rqbd;

        OBD_ALLOC(rqbd, sizeof (*rqbd));
        if (rqbd == NULL)
                return (NULL);

        rqbd->rqbd_service = svc;
        rqbd->rqbd_refcount = 0;
        rqbd->rqbd_cbid.cbid_fn = request_in_callback;
        rqbd->rqbd_cbid.cbid_arg = rqbd;
        INIT_LIST_HEAD(&rqbd->rqbd_reqs);
        rqbd->rqbd_buffer = ptlrpc_alloc_request_buffer(svc->srv_buf_size);

        if (rqbd->rqbd_buffer == NULL) {
                OBD_FREE(rqbd, sizeof (*rqbd));
                return (NULL);
        }

        spin_lock_irqsave (&svc->srv_lock, flags);
        list_add(&rqbd->rqbd_list, &svc->srv_idle_rqbds);
        svc->srv_nbufs++;
        spin_unlock_irqrestore (&svc->srv_lock, flags);

        return (rqbd);
}

void
ptlrpc_free_rqbd (struct ptlrpc_request_buffer_desc *rqbd)
{
        struct ptlrpc_service *svc = rqbd->rqbd_service;
        unsigned long          flags;

        LASSERT (rqbd->rqbd_refcount == 0);
        LASSERT (list_empty(&rqbd->rqbd_reqs));

        spin_lock_irqsave(&svc->srv_lock, flags);
        list_del(&rqbd->rqbd_list);
        svc->srv_nbufs--;
        spin_unlock_irqrestore(&svc->srv_lock, flags);

        ptlrpc_free_request_buffer (rqbd->rqbd_buffer, svc->srv_buf_size);
        OBD_FREE (rqbd, sizeof (*rqbd));
}

int
ptlrpc_grow_req_bufs(struct ptlrpc_service *svc)
{
        struct ptlrpc_request_buffer_desc *rqbd;
        int                                i;

        CDEBUG(D_RPCTRACE, "%s: allocate %d new %d-byte reqbufs (%d/%d left)\n",
               svc->srv_name, svc->srv_nbuf_per_group, svc->srv_buf_size,
               svc->srv_nrqbd_receiving, svc->srv_nbufs);
        for (i = 0; i < svc->srv_nbuf_per_group; i++) {
                rqbd = ptlrpc_alloc_rqbd(svc);

                if (rqbd == NULL) {
                        CERROR ("%s: Can't allocate request buffer\n",
                                svc->srv_name);
                        return (-ENOMEM);
                }

                if (ptlrpc_server_post_idle_rqbds(svc) < 0)
                        return (-EAGAIN);
        }

        return (0);
}

void
ptlrpc_save_lock (struct ptlrpc_request *req,
                  struct lustre_handle *lock, int mode)
{
        struct ptlrpc_reply_state *rs = req->rq_reply_state;
        int                        idx;

        LASSERT (rs != NULL);
        LASSERT (rs->rs_nlocks < RS_MAX_LOCKS);

        idx = rs->rs_nlocks++;
        rs->rs_locks[idx] = *lock;
        rs->rs_modes[idx] = mode;
        rs->rs_difficult = 1;
}

void
ptlrpc_schedule_difficult_reply (struct ptlrpc_reply_state *rs)
{
        struct ptlrpc_service *svc = rs->rs_service;

#ifdef CONFIG_SMP
        LASSERT (spin_is_locked (&svc->srv_lock));
#endif
        LASSERT (rs->rs_difficult);
        rs->rs_scheduled_ever = 1;              /* flag any notification attempt */

        if (rs->rs_scheduled)                   /* being set up or already notified */
                return;

        rs->rs_scheduled = 1;
        list_del (&rs->rs_list);
        list_add (&rs->rs_list, &svc->srv_reply_queue);
        wake_up (&svc->srv_waitq);
}

void
ptlrpc_commit_replies (struct obd_device *obd)
{
        struct list_head   *tmp;
        struct list_head   *nxt;
        unsigned long       flags;

        /* Find any replies that have been committed and get their service
         * to attend to complete them. */

        /* CAVEAT EMPTOR: spinlock ordering!!! */
        spin_lock_irqsave (&obd->obd_uncommitted_replies_lock, flags);

        list_for_each_safe (tmp, nxt, &obd->obd_uncommitted_replies) {
                struct ptlrpc_reply_state *rs =
                        list_entry(tmp, struct ptlrpc_reply_state, rs_obd_list);

                LASSERT (rs->rs_difficult);

                if (rs->rs_transno <= obd->obd_last_committed) {
                        struct ptlrpc_service *svc = rs->rs_service;

                        spin_lock (&svc->srv_lock);
                        list_del_init (&rs->rs_obd_list);
                        ptlrpc_schedule_difficult_reply (rs);
                        spin_unlock (&svc->srv_lock);
                }
        }

        spin_unlock_irqrestore (&obd->obd_uncommitted_replies_lock, flags);
}

static long
timeval_sub(struct timeval *large, struct timeval *small)
{
        return (large->tv_sec - small->tv_sec) * 1000000 +
                (large->tv_usec - small->tv_usec);
}

static int
ptlrpc_server_post_idle_rqbds (struct ptlrpc_service *svc)
{
        struct ptlrpc_request_buffer_desc *rqbd;
        unsigned long                      flags;
        int                                rc;
        int                                posted = 0;

        for (;;) {
                spin_lock_irqsave(&svc->srv_lock, flags);

                if (list_empty (&svc->srv_idle_rqbds)) {
                        spin_unlock_irqrestore(&svc->srv_lock, flags);
                        return (posted);
                }

                rqbd = list_entry(svc->srv_idle_rqbds.next,
                                  struct ptlrpc_request_buffer_desc,
                                  rqbd_list);
                list_del (&rqbd->rqbd_list);

                /* assume we will post successfully */
                svc->srv_nrqbd_receiving++;
                list_add (&rqbd->rqbd_list, &svc->srv_active_rqbds);

                spin_unlock_irqrestore(&svc->srv_lock, flags);

                rc = ptlrpc_register_rqbd(rqbd);
                if (rc != 0)
                        break;

                posted = 1;
        }

        spin_lock_irqsave(&svc->srv_lock, flags);

        svc->srv_nrqbd_receiving--;
        list_del(&rqbd->rqbd_list);
        list_add_tail(&rqbd->rqbd_list, &svc->srv_idle_rqbds);

        if (svc->srv_nrqbd_receiving == 0) {
                /* This service is off-air on this interface because all
                 * its request buffers are busy.  Portals will have started
                 * dropping incoming requests until more buffers get
                 * posted */
                CERROR("All %s request buffers busy\n", svc->srv_name);
        }

        spin_unlock_irqrestore (&svc->srv_lock, flags);

        return (-1);
}

struct ptlrpc_service *
ptlrpc_init_svc(int nbufs, int bufsize, int max_req_size, int max_reply_size,
                int req_portal, int rep_portal, int watchdog_timeout,
                svc_handler_t handler, char *name,
                struct proc_dir_entry *proc_entry,
                svcreq_printfn_t svcreq_printfn, int num_threads)
{
        int                    rc;
        struct ptlrpc_service *service;
        ENTRY;

        LASSERT (nbufs > 0);
        LASSERT (bufsize >= max_req_size);
        
        OBD_ALLOC(service, sizeof(*service));
        if (service == NULL)
                RETURN(NULL);

        /* First initialise enough for early teardown */

        service->srv_name = name;
        spin_lock_init(&service->srv_lock);
        INIT_LIST_HEAD(&service->srv_threads);
        init_waitqueue_head(&service->srv_waitq);

        service->srv_nbuf_per_group = nbufs;
        service->srv_max_req_size = max_req_size;
        service->srv_buf_size = bufsize;
        service->srv_rep_portal = rep_portal;
        service->srv_req_portal = req_portal;
        service->srv_watchdog_timeout = watchdog_timeout;
        service->srv_handler = handler;
        service->srv_request_history_print_fn = svcreq_printfn;
        service->srv_request_seq = 1;           /* valid seq #s start at 1 */
        service->srv_request_max_cull_seq = 0;
        service->srv_num_threads = num_threads;

        INIT_LIST_HEAD(&service->srv_request_queue);
        INIT_LIST_HEAD(&service->srv_idle_rqbds);
        INIT_LIST_HEAD(&service->srv_active_rqbds);
        INIT_LIST_HEAD(&service->srv_history_rqbds);
        INIT_LIST_HEAD(&service->srv_request_history);
        INIT_LIST_HEAD(&service->srv_active_replies);
        INIT_LIST_HEAD(&service->srv_reply_queue);
        INIT_LIST_HEAD(&service->srv_free_rs_list);
        init_waitqueue_head(&service->srv_free_rs_waitq);

        spin_lock (&ptlrpc_all_services_lock);
        list_add (&service->srv_list, &ptlrpc_all_services);
        spin_unlock (&ptlrpc_all_services_lock);
        
        /* Now allocate the request buffers */
        rc = ptlrpc_grow_req_bufs(service);
        /* We shouldn't be under memory pressure at startup, so
         * fail if we can't post all our buffers at this time. */
        if (rc != 0)
                GOTO(failed, NULL);

        /* Now allocate pool of reply buffers */
        /* Increase max reply size to next power of two */
        service->srv_max_reply_size = 1;
        while(service->srv_max_reply_size < max_reply_size)
                service->srv_max_reply_size <<= 1;

        if (proc_entry != NULL)
                ptlrpc_lprocfs_register_service(proc_entry, service);

        CDEBUG(D_NET, "%s: Started, listening on portal %d\n",
               service->srv_name, service->srv_req_portal);

        RETURN(service);
failed:
        ptlrpc_unregister_service(service);
        return NULL;
}

static void __ptlrpc_server_free_request(struct ptlrpc_request *req)
{
        struct ptlrpc_request_buffer_desc *rqbd = req->rq_rqbd;

        list_del(&req->rq_list);

        if (req->rq_reply_state != NULL) {
                ptlrpc_rs_decref(req->rq_reply_state);
                req->rq_reply_state = NULL;
        }

        if (req != &rqbd->rqbd_req) {
                /* NB request buffers use an embedded
                 * req if the incoming req unlinked the
                 * MD; this isn't one of them! */
                OBD_FREE(req, sizeof(*req));
        }
}

static void
ptlrpc_server_free_request(struct ptlrpc_request *req)
{
        struct ptlrpc_request_buffer_desc *rqbd = req->rq_rqbd;
        struct ptlrpc_service             *svc = rqbd->rqbd_service;
        unsigned long                      flags;
        int                                refcount;
        struct list_head                  *tmp;
        struct list_head                  *nxt;

        spin_lock_irqsave(&svc->srv_lock, flags);

        svc->srv_n_active_reqs--;
        list_add(&req->rq_list, &rqbd->rqbd_reqs);

        refcount = --(rqbd->rqbd_refcount);
        if (refcount == 0) {
                /* request buffer is now idle: add to history */
                list_del(&rqbd->rqbd_list);
                list_add_tail(&rqbd->rqbd_list, &svc->srv_history_rqbds);
                svc->srv_n_history_rqbds++;

                /* cull some history?
                 * I expect only about 1 or 2 rqbds need to be recycled here */
                while (svc->srv_n_history_rqbds > svc->srv_max_history_rqbds) {
                        rqbd = list_entry(svc->srv_history_rqbds.next,
                                          struct ptlrpc_request_buffer_desc,
                                          rqbd_list);

                        list_del(&rqbd->rqbd_list);
                        svc->srv_n_history_rqbds--;

                        /* remove rqbd's reqs from svc's req history while
                         * I've got the service lock */
                        list_for_each(tmp, &rqbd->rqbd_reqs) {
                                req = list_entry(tmp, struct ptlrpc_request,
                                                 rq_list);
                                /* Track the highest culled req seq */
                                if (req->rq_history_seq >
                                    svc->srv_request_max_cull_seq)
                                        svc->srv_request_max_cull_seq =
                                                req->rq_history_seq;
                                list_del(&req->rq_history_list);
                        }

                        spin_unlock_irqrestore(&svc->srv_lock, flags);

                        list_for_each_safe(tmp, nxt, &rqbd->rqbd_reqs) {
                                req = list_entry(rqbd->rqbd_reqs.next,
                                                 struct ptlrpc_request,
                                                 rq_list);
                                __ptlrpc_server_free_request(req);
                        }

                        spin_lock_irqsave(&svc->srv_lock, flags);

                        /* schedule request buffer for re-use.
                         * NB I can only do this after I've disposed of their
                         * reqs; particularly the embedded req */
                        list_add_tail(&rqbd->rqbd_list, &svc->srv_idle_rqbds);
                }
        } else if (req->rq_reply_state && req->rq_reply_state->rs_prealloc) {
                 /* If we are low on memory, we are not interested in
                    history */
                list_del(&req->rq_history_list);
                __ptlrpc_server_free_request(req);
        }

        spin_unlock_irqrestore(&svc->srv_lock, flags);

}

static int
ptlrpc_server_handle_request(struct ptlrpc_service *svc,
                             struct ptlrpc_thread *thread)
{
        struct ptlrpc_request *request;
        unsigned long          flags;
        struct timeval         work_start;
        struct timeval         work_end;
        long                   timediff;
        int                    rc;
        ENTRY;

        LASSERT(svc);

        spin_lock_irqsave (&svc->srv_lock, flags);
        if (list_empty (&svc->srv_request_queue) ||
            (svc->srv_n_difficult_replies != 0 &&
             svc->srv_n_active_reqs >= (svc->srv_nthreads - 1))) {
                /* If all the other threads are handling requests, I must
                 * remain free to handle any 'difficult' reply that might
                 * block them */
                spin_unlock_irqrestore (&svc->srv_lock, flags);
                RETURN(0);
        }

        request = list_entry (svc->srv_request_queue.next,
                              struct ptlrpc_request, rq_list);
        list_del_init (&request->rq_list);
        svc->srv_n_queued_reqs--;
        svc->srv_n_active_reqs++;

        spin_unlock_irqrestore (&svc->srv_lock, flags);

        do_gettimeofday(&work_start);
        timediff = timeval_sub(&work_start, &request->rq_arrival_time);
        if (svc->srv_stats != NULL) {
                lprocfs_counter_add(svc->srv_stats, PTLRPC_REQWAIT_CNTR,
                                    timediff);
                lprocfs_counter_add(svc->srv_stats, PTLRPC_REQQDEPTH_CNTR,
                                    svc->srv_n_queued_reqs);
                lprocfs_counter_add(svc->srv_stats, PTLRPC_REQACTIVE_CNTR,
                                    svc->srv_n_active_reqs);
        }

#if SWAB_PARANOIA
        /* Clear request swab mask; this is a new request */
        request->rq_req_swab_mask = 0;
#endif
        rc = lustre_unpack_msg (request->rq_reqmsg, request->rq_reqlen);
        if (rc != 0) {
                CERROR ("error unpacking request: ptl %d from %s"
                        " xid "LPU64"\n", svc->srv_req_portal,
                        libcfs_id2str(request->rq_peer), request->rq_xid);
                goto out;
        }

        rc = -EINVAL;
        if (request->rq_reqmsg->type != PTL_RPC_MSG_REQUEST) {
                CERROR("wrong packet type received (type=%u) from %s\n",
                       request->rq_reqmsg->type,
                       libcfs_id2str(request->rq_peer));
                goto out;
        }

        CDEBUG(D_NET, "got req "LPD64"\n", request->rq_xid);

        request->rq_svc_thread = thread;
        request->rq_export = class_conn2export(&request->rq_reqmsg->handle);

        if (request->rq_export) {
                if (request->rq_reqmsg->conn_cnt <
                    request->rq_export->exp_conn_cnt) {
                        DEBUG_REQ(D_ERROR, request,
                                  "DROPPING req from old connection %d < %d",
                                  request->rq_reqmsg->conn_cnt,
                                  request->rq_export->exp_conn_cnt);
                        goto put_conn;
                }
                if (request->rq_export->exp_obd &&
                    request->rq_export->exp_obd->obd_fail) {
                        /* Failing over, don't handle any more reqs, send
                           error response instead. */
                        CDEBUG(D_HA, "Dropping req %p for failed obd %s\n",
                               request, request->rq_export->exp_obd->obd_name);
                        request->rq_status = -ENODEV;
                        ptlrpc_error(request);
                        goto put_conn;
                }

                class_update_export_timer(request->rq_export,
                                          (time_t)(timediff / 500000));
        }

        /* Discard requests queued for longer than my timeout.  If the
         * client's timeout is similar to mine, she'll be timing out this
         * REQ anyway (bug 1502) */
        if (timediff / 1000000 > (long)obd_timeout) {
                CERROR("Dropping timed-out opc %d request from %s"
                       ": %ld seconds old\n", request->rq_reqmsg->opc,
                       libcfs_id2str(request->rq_peer),
                       timediff / 1000000);
                goto put_conn;
        }

        request->rq_phase = RQ_PHASE_INTERPRET;

        CDEBUG(D_RPCTRACE, "Handling RPC pname:cluuid+ref:pid:xid:nid:opc "
               "%s:%s+%d:%d:"LPU64":%s:%d\n", current->comm,
               (request->rq_export ?
                (char *)request->rq_export->exp_client_uuid.uuid : "0"),
               (request->rq_export ?
                atomic_read(&request->rq_export->exp_refcount) : -99),
               request->rq_reqmsg->status, request->rq_xid,
               libcfs_id2str(request->rq_peer),
               request->rq_reqmsg->opc);

        rc = svc->srv_handler(request);

        request->rq_phase = RQ_PHASE_COMPLETE;

        CDEBUG(D_RPCTRACE, "Handled RPC pname:cluuid+ref:pid:xid:nid:opc "
               "%s:%s+%d:%d:"LPU64":%s:%d\n", current->comm,
               (request->rq_export ?
                (char *)request->rq_export->exp_client_uuid.uuid : "0"),
               (request->rq_export ?
                atomic_read(&request->rq_export->exp_refcount) : -99),
               request->rq_reqmsg->status, request->rq_xid,
               libcfs_id2str(request->rq_peer),
               request->rq_reqmsg->opc);

put_conn:
        if (request->rq_export != NULL)
                class_export_put(request->rq_export);

 out:
        do_gettimeofday(&work_end);

        timediff = timeval_sub(&work_end, &work_start);

        if (timediff / 1000000 > (long)obd_timeout)
                CERROR("request "LPU64" opc %u from %s processed in %lds\n",
                       request->rq_xid, request->rq_reqmsg->opc,
                       libcfs_id2str(request->rq_peer),
                       timeval_sub(&work_end,
                                   &request->rq_arrival_time) / 1000000);
        else
                CDEBUG(D_HA,"request "LPU64" opc %u from %s processed in %ldus"
                       " (%ldus total)\n", request->rq_xid,
                       request->rq_reqmsg->opc,
                       libcfs_id2str(request->rq_peer), timediff,
                       timeval_sub(&work_end, &request->rq_arrival_time));

        if (svc->srv_stats != NULL) {
                int opc = opcode_offset(request->rq_reqmsg->opc);
                if (opc > 0) {
                        LASSERT(opc < LUSTRE_MAX_OPCODES);
                        lprocfs_counter_add(svc->srv_stats,
                                            opc + PTLRPC_LAST_CNTR,
                                            timediff);
                }
        }

        ptlrpc_server_free_request(request);

        RETURN(1);
}

static int
ptlrpc_server_handle_reply (struct ptlrpc_service *svc)
{
        struct ptlrpc_reply_state *rs;
        unsigned long              flags;
        struct obd_export         *exp;
        struct obd_device         *obd;
        int                        nlocks;
        int                        been_handled;
        ENTRY;

        spin_lock_irqsave (&svc->srv_lock, flags);
        if (list_empty (&svc->srv_reply_queue)) {
                spin_unlock_irqrestore (&svc->srv_lock, flags);
                RETURN(0);
        }

        rs = list_entry (svc->srv_reply_queue.next,
                         struct ptlrpc_reply_state, rs_list);

        exp = rs->rs_export;
        obd = exp->exp_obd;

        LASSERT (rs->rs_difficult);
        LASSERT (rs->rs_scheduled);

        list_del_init (&rs->rs_list);

        /* Disengage from notifiers carefully (lock ordering!) */
        spin_unlock(&svc->srv_lock);

        spin_lock (&obd->obd_uncommitted_replies_lock);
        /* Noop if removed already */
        list_del_init (&rs->rs_obd_list);
        spin_unlock (&obd->obd_uncommitted_replies_lock);

        spin_lock (&exp->exp_lock);
        /* Noop if removed already */
        list_del_init (&rs->rs_exp_list);
        spin_unlock (&exp->exp_lock);

        spin_lock(&svc->srv_lock);

        been_handled = rs->rs_handled;
        rs->rs_handled = 1;

        nlocks = rs->rs_nlocks;                 /* atomic "steal", but */
        rs->rs_nlocks = 0;                      /* locks still on rs_locks! */

        if (nlocks == 0 && !been_handled) {
                /* If we see this, we should already have seen the warning
                 * in mds_steal_ack_locks()  */
                CWARN("All locks stolen from rs %p x"LPD64".t"LPD64
                      " o%d NID %s\n",
                      rs,
                      rs->rs_xid, rs->rs_transno,
                      rs->rs_msg.opc,
                      libcfs_nid2str(exp->exp_connection->c_peer.nid));
        }

        if ((!been_handled && rs->rs_on_net) ||
            nlocks > 0) {
                spin_unlock_irqrestore(&svc->srv_lock, flags);

                if (!been_handled && rs->rs_on_net) {
                        LNetMDUnlink(rs->rs_md_h);
                        /* Ignore return code; we're racing with
                         * completion... */
                }

                while (nlocks-- > 0)
                        ldlm_lock_decref(&rs->rs_locks[nlocks],
                                         rs->rs_modes[nlocks]);

                spin_lock_irqsave(&svc->srv_lock, flags);
        }

        rs->rs_scheduled = 0;

        if (!rs->rs_on_net) {
                /* Off the net */
                svc->srv_n_difficult_replies--;
                spin_unlock_irqrestore(&svc->srv_lock, flags);

                class_export_put (exp);
                rs->rs_export = NULL;
                ptlrpc_rs_decref (rs);
                atomic_dec (&svc->srv_outstanding_replies);
                RETURN(1);
        }

        /* still on the net; callback will schedule */
        spin_unlock_irqrestore (&svc->srv_lock, flags);
        RETURN(1);
}

#ifndef __KERNEL__
/* FIXME make use of timeout later */
int
liblustre_check_services (void *arg)
{
        int  did_something = 0;
        int  rc;
        struct list_head *tmp, *nxt;
        ENTRY;

        /* I'm relying on being single threaded, not to have to lock
         * ptlrpc_all_services etc */
        list_for_each_safe (tmp, nxt, &ptlrpc_all_services) {
                struct ptlrpc_service *svc =
                        list_entry (tmp, struct ptlrpc_service, srv_list);

                if (svc->srv_nthreads != 0)     /* I've recursed */
                        continue;

                /* service threads can block for bulk, so this limits us
                 * (arbitrarily) to recursing 1 stack frame per service.
                 * Note that the problem with recursion is that we have to
                 * unwind completely before our caller can resume. */

                svc->srv_nthreads++;

                do {
                        rc = ptlrpc_server_handle_reply(svc);
                        rc |= ptlrpc_server_handle_request(svc, NULL);
                        rc |= (ptlrpc_server_post_idle_rqbds(svc) > 0);
                        did_something |= rc;
                } while (rc);

                svc->srv_nthreads--;
        }

        RETURN(did_something);
}
#define ptlrpc_stop_all_threads(s) do {} while (0)

#else /* __KERNEL__ */

/* Don't use daemonize, it removes fs struct from new thread (bug 418) */
void ptlrpc_daemonize(void)
{
        exit_mm(current);
        lustre_daemonize_helper();
        set_fs_pwd(current->fs, init_task.fs->pwdmnt, init_task.fs->pwd);
        exit_files(current);
        reparent_to_init();
}

static void
ptlrpc_check_rqbd_pool(struct ptlrpc_service *svc)
{
        int avail = svc->srv_nrqbd_receiving;
        int low_water = svc->srv_nbuf_per_group/2;

        /* NB I'm not locking; just looking. */

        /* CAVEAT EMPTOR: We might be allocating buffers here because we've
         * allowed the request history to grow out of control.  We could put a
         * sanity check on that here and cull some history if we need the
         * space. */

        if (avail <= low_water)
                ptlrpc_grow_req_bufs(svc);

        lprocfs_counter_add(svc->srv_stats, PTLRPC_REQBUF_AVAIL_CNTR, avail);
}

static int
ptlrpc_retry_rqbds(void *arg)
{
        struct ptlrpc_service *svc = (struct ptlrpc_service *)arg;

        svc->srv_rqbd_timeout = 0;
        return (-ETIMEDOUT);
}

static int ptlrpc_main(void *arg)
{
        struct ptlrpc_svc_data *data = (struct ptlrpc_svc_data *)arg;
        struct ptlrpc_service  *svc = data->svc;
        struct ptlrpc_thread   *thread = data->thread;
        struct ptlrpc_reply_state *rs;
        struct lc_watchdog     *watchdog;
        unsigned long           flags;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,4)
        struct group_info *ginfo = NULL;
#endif
        int rc = 0;
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

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,9) && CONFIG_NUMA
        /* we need to do this before any per-thread allocation is done so that
         * we get the per-thread allocations on local node.  bug 7342 */
        if (svc->srv_cpu_affinity) {
                int cpu, num_cpu;

                for (cpu = 0, num_cpu = 0; cpu < NR_CPUS; cpu++) {
                        if (!cpu_online(cpu))
                                continue;
                        if (num_cpu == thread->t_id % num_online_cpus())
                                break;
                        num_cpu++;
                }
                set_cpus_allowed(current, node_to_cpumask(cpu_to_node(cpu)));
        }
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,4)
        ginfo = groups_alloc(0);
        if (!ginfo) {
                rc = -ENOMEM;
                goto out;
        }

        set_current_groups(ginfo);
        put_group_info(ginfo);
#endif

        if (svc->srv_init != NULL) {
                rc = svc->srv_init(thread);
                if (rc)
                        goto out;
        }

        /* Alloc reply state structure for this one */
        OBD_ALLOC_GFP(rs, svc->srv_max_reply_size, GFP_KERNEL);
        if (!rs) {
                rc = -ENOMEM;
                goto out_srv_init;
        }

        /* Record that the thread is running */
        thread->t_flags = SVC_RUNNING;
        /*
         * wake up our creator. Note: @data is invalid after this point,
         * because it's allocated on ptlrpc_start_thread() stack.
         */
        wake_up(&thread->t_ctl_waitq);

        watchdog = lc_watchdog_add(svc->srv_watchdog_timeout,
                                   LC_WATCHDOG_DEFAULT_CB, NULL);

        spin_lock_irqsave(&svc->srv_lock, flags);
        svc->srv_nthreads++;
        list_add(&rs->rs_list, &svc->srv_free_rs_list);
        spin_unlock_irqrestore(&svc->srv_lock, flags);
        wake_up(&svc->srv_free_rs_waitq);

        CDEBUG(D_NET, "service thread %d started\n", thread->t_id);

        /* XXX maintain a list of all managed devices: insert here */

        while ((thread->t_flags & SVC_STOPPING) == 0 ||
               svc->srv_n_difficult_replies != 0) {
                /* Don't exit while there are replies to be handled */
                struct l_wait_info lwi = LWI_TIMEOUT(svc->srv_rqbd_timeout,
                                                     ptlrpc_retry_rqbds, svc);

                lc_watchdog_disable(watchdog);

                l_wait_event_exclusive (svc->srv_waitq,
                              ((thread->t_flags & SVC_STOPPING) != 0 &&
                               svc->srv_n_difficult_replies == 0) ||
                              (!list_empty(&svc->srv_idle_rqbds) &&
                               svc->srv_rqbd_timeout == 0) ||
                              !list_empty (&svc->srv_reply_queue) ||
                              (!list_empty (&svc->srv_request_queue) &&
                               (svc->srv_n_difficult_replies == 0 ||
                                svc->srv_n_active_reqs <
                                (svc->srv_nthreads - 1))),
                              &lwi);

                lc_watchdog_touch(watchdog);

                ptlrpc_check_rqbd_pool(svc);

                if (!list_empty (&svc->srv_reply_queue))
                        ptlrpc_server_handle_reply (svc);

                /* only handle requests if there are no difficult replies
                 * outstanding, or I'm not the last thread handling
                 * requests */
                if (!list_empty (&svc->srv_request_queue) &&
                    (svc->srv_n_difficult_replies == 0 ||
                     svc->srv_n_active_reqs < (svc->srv_nthreads - 1)))
                        ptlrpc_server_handle_request(svc, thread);

                if (!list_empty(&svc->srv_idle_rqbds) &&
                    ptlrpc_server_post_idle_rqbds(svc) < 0) {
                        /* I just failed to repost request buffers.  Wait
                         * for a timeout (unless something else happens)
                         * before I try again */
                        svc->srv_rqbd_timeout = HZ/10;
                }
        }

        lc_watchdog_delete(watchdog);

out_srv_init:
        /*
         * deconstruct service specific state created by ptlrpc_start_thread()
         */
        if (svc->srv_done != NULL)
                svc->srv_done(thread);

out:
        spin_lock_irqsave(&svc->srv_lock, flags);

        svc->srv_nthreads--;                    /* must know immediately */
        thread->t_flags = SVC_STOPPED;
        wake_up(&thread->t_ctl_waitq);

        spin_unlock_irqrestore(&svc->srv_lock, flags);

        CDEBUG(D_NET, "service thread %d exiting: rc %d\n", thread->t_id, rc);
        thread->t_id = rc;

        return rc;
}

static void ptlrpc_stop_thread(struct ptlrpc_service *svc,
                               struct ptlrpc_thread *thread)
{
        struct l_wait_info lwi = { 0 };
        unsigned long      flags;

        spin_lock_irqsave(&svc->srv_lock, flags);
        thread->t_flags = SVC_STOPPING;
        spin_unlock_irqrestore(&svc->srv_lock, flags);

        wake_up_all(&svc->srv_waitq);
        l_wait_event(thread->t_ctl_waitq, (thread->t_flags & SVC_STOPPED),
                     &lwi);

        spin_lock_irqsave(&svc->srv_lock, flags);
        list_del(&thread->t_link);
        spin_unlock_irqrestore(&svc->srv_lock, flags);

        OBD_FREE(thread, sizeof(*thread));
}

void ptlrpc_stop_all_threads(struct ptlrpc_service *svc)
{
        unsigned long flags;
        struct ptlrpc_thread *thread;

        spin_lock_irqsave(&svc->srv_lock, flags);
        while (!list_empty(&svc->srv_threads)) {
                thread = list_entry(svc->srv_threads.next,
                                    struct ptlrpc_thread, t_link);

                spin_unlock_irqrestore(&svc->srv_lock, flags);
                ptlrpc_stop_thread(svc, thread);
                spin_lock_irqsave(&svc->srv_lock, flags);
        }

        spin_unlock_irqrestore(&svc->srv_lock, flags);
}

/* @base_name should be 12 characters or less - 3 will be added on */
int ptlrpc_start_threads(struct obd_device *dev, struct ptlrpc_service *svc,
                         char *base_name)
{
        int i, rc = 0;
        ENTRY;

        for (i = 0; i < svc->srv_num_threads; i++) {
                char name[32];
                sprintf(name, "%s_%02d", base_name, i);
                rc = ptlrpc_start_thread(dev, svc, name, i);
                if (rc) {
                        CERROR("cannot start %s thread #%d: rc %d\n", base_name,
                               i, rc);
                        ptlrpc_stop_all_threads(svc);
                }
        }
        RETURN(rc);
}

int ptlrpc_start_thread(struct obd_device *dev, struct ptlrpc_service *svc,
                        char *name, int id)
{
        struct l_wait_info lwi = { 0 };
        struct ptlrpc_svc_data d;
        struct ptlrpc_thread *thread;
        unsigned long flags;
        int rc;
        ENTRY;

        OBD_ALLOC(thread, sizeof(*thread));
        if (thread == NULL)
                RETURN(-ENOMEM);
        init_waitqueue_head(&thread->t_ctl_waitq);
        thread->t_id = id;

        spin_lock_irqsave(&svc->srv_lock, flags);
        list_add(&thread->t_link, &svc->srv_threads);
        spin_unlock_irqrestore(&svc->srv_lock, flags);

        d.dev = dev;
        d.svc = svc;
        d.name = name;
        d.thread = thread;

        /* CLONE_VM and CLONE_FILES just avoid a needless copy, because we
         * just drop the VM and FILES in ptlrpc_daemonize() right away.
         */
        rc = kernel_thread(ptlrpc_main, &d, CLONE_VM | CLONE_FILES);
        if (rc < 0) {
                CERROR("cannot start thread '%s': rc %d\n", name, rc);

                spin_lock_irqsave(&svc->srv_lock, flags);
                list_del(&thread->t_link);
                spin_unlock_irqrestore(&svc->srv_lock, flags);

                OBD_FREE(thread, sizeof(*thread));
                RETURN(rc);
        }
        l_wait_event(thread->t_ctl_waitq,
                     thread->t_flags & (SVC_RUNNING | SVC_STOPPED), &lwi);

        rc = (thread->t_flags & SVC_STOPPED) ? thread->t_id : 0;
        RETURN(rc);
}
#endif

int ptlrpc_unregister_service(struct ptlrpc_service *service)
{
        int                   rc;
        unsigned long         flags;
        struct l_wait_info    lwi;
        struct list_head     *tmp;
        struct ptlrpc_reply_state *rs, *t;

        ptlrpc_stop_all_threads(service);
        LASSERT(list_empty(&service->srv_threads));

        spin_lock (&ptlrpc_all_services_lock);
        list_del_init (&service->srv_list);
        spin_unlock (&ptlrpc_all_services_lock);

        ptlrpc_lprocfs_unregister_service(service);

        /* All history will be culled when the next request buffer is
         * freed */
        service->srv_max_history_rqbds = 0;

        CDEBUG(D_NET, "%s: tearing down\n", service->srv_name);

        /* Unlink all the request buffers.  This forces a 'final' event with
         * its 'unlink' flag set for each posted rqbd */
        list_for_each(tmp, &service->srv_active_rqbds) {
                struct ptlrpc_request_buffer_desc *rqbd =
                        list_entry(tmp, struct ptlrpc_request_buffer_desc, 
                                   rqbd_list);

                rc = LNetMDUnlink(rqbd->rqbd_md_h);
                LASSERT (rc == 0 || rc == -ENOENT);
        }

        /* Wait for the network to release any buffers it's currently
         * filling */
        for (;;) {
                spin_lock_irqsave(&service->srv_lock, flags);
                rc = service->srv_nrqbd_receiving;
                spin_unlock_irqrestore(&service->srv_lock, flags);

                if (rc == 0)
                        break;

                /* Network access will complete in finite time but the HUGE
                 * timeout lets us CWARN for visibility of sluggish NALs */
                lwi = LWI_TIMEOUT(300 * HZ, NULL, NULL);
                rc = l_wait_event(service->srv_waitq,
                                  service->srv_nrqbd_receiving == 0,
                                  &lwi);
                if (rc == -ETIMEDOUT)
                        CWARN("Service %s waiting for request buffers\n",
                              service->srv_name);
        }

        /* schedule all outstanding replies to terminate them */
        spin_lock_irqsave(&service->srv_lock, flags);
        while (!list_empty(&service->srv_active_replies)) {
                struct ptlrpc_reply_state *rs =
                        list_entry(service->srv_active_replies.next,
                                   struct ptlrpc_reply_state, rs_list);
                ptlrpc_schedule_difficult_reply(rs);
        }
        spin_unlock_irqrestore(&service->srv_lock, flags);

        /* purge the request queue.  NB No new replies (rqbds all unlinked)
         * and no service threads, so I'm the only thread noodling the
         * request queue now */
        while (!list_empty(&service->srv_request_queue)) {
                struct ptlrpc_request *req =
                        list_entry(service->srv_request_queue.next,
                                   struct ptlrpc_request,
                                   rq_list);

                list_del(&req->rq_list);
                service->srv_n_queued_reqs--;
                service->srv_n_active_reqs++;

                ptlrpc_server_free_request(req);
        }
        LASSERT(service->srv_n_queued_reqs == 0);
        LASSERT(service->srv_n_active_reqs == 0);
        LASSERT(service->srv_n_history_rqbds == 0);
        LASSERT(list_empty(&service->srv_active_rqbds));

        /* Now free all the request buffers since nothing references them
         * any more... */
        while (!list_empty(&service->srv_idle_rqbds)) {
                struct ptlrpc_request_buffer_desc *rqbd =
                        list_entry(service->srv_idle_rqbds.next,
                                   struct ptlrpc_request_buffer_desc,
                                   rqbd_list);

                ptlrpc_free_rqbd(rqbd);
        }

        /* wait for all outstanding replies to complete (they were
         * scheduled having been flagged to abort above) */
        while (atomic_read(&service->srv_outstanding_replies) != 0) {
                struct l_wait_info lwi = LWI_TIMEOUT(10 * HZ, NULL, NULL);

                rc = l_wait_event(service->srv_waitq,
                                  !list_empty(&service->srv_reply_queue), &lwi);
                LASSERT(rc == 0 || rc == -ETIMEDOUT);

                if (rc == 0) {
                        ptlrpc_server_handle_reply(service);
                        continue;
                }
                CWARN("Unexpectedly long timeout %p\n", service);
        }

        list_for_each_entry_safe(rs, t, &service->srv_free_rs_list, rs_list) {
                list_del(&rs->rs_list);
                OBD_FREE(rs, service->srv_max_reply_size);
        }

        OBD_FREE(service, sizeof(*service));
        return 0;
}

/* Returns 0 if the service is healthy.
 *
 * Right now, it just checks to make sure that requests aren't languishing
 * in the queue.  We'll use this health check to govern whether a node needs
 * to be shot, so it's intentionally non-aggressive. */
int ptlrpc_service_health_check(struct ptlrpc_service *svc)
{
        struct ptlrpc_request *request;
        struct timeval         right_now;
        long                   timediff, cutoff;
        unsigned long          flags;
        int                    rc = 0;

        if (svc == NULL)
                return 0;

        spin_lock_irqsave(&svc->srv_lock, flags);

        if (list_empty(&svc->srv_request_queue))
                goto out;

        request = list_entry(svc->srv_request_queue.next,
                             struct ptlrpc_request, rq_list);

        do_gettimeofday(&right_now);
        timediff = timeval_sub(&right_now, &request->rq_arrival_time);

        cutoff = obd_health_check_timeout;

        if (timediff / 1000000 > cutoff) {
                rc = -1;
                goto out;
        }

 out:
        spin_unlock_irqrestore(&svc->srv_lock, flags);
        return rc;
}
