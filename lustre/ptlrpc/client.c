/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (c) 2002, 2003 Cluster File Systems, Inc.
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
 *
 */

#define DEBUG_SUBSYSTEM S_RPC
#ifndef __KERNEL__
#include <errno.h>
#include <signal.h>
#include <liblustre.h>
#endif

#include <linux/obd_support.h>
#include <linux/obd_class.h>
#include <linux/lustre_lib.h>
#include <linux/lustre_ha.h>
#include <linux/lustre_import.h>

#include "ptlrpc_internal.h"

void ptlrpc_init_client(int req_portal, int rep_portal, char *name,
                        struct ptlrpc_client *cl)
{
        cl->cli_request_portal = req_portal;
        cl->cli_reply_portal   = rep_portal;
        cl->cli_name           = name;
}

struct ptlrpc_connection *ptlrpc_uuid_to_connection(struct obd_uuid *uuid)
{
        struct ptlrpc_connection *c;
        struct ptlrpc_peer peer;
        int err;

        err = ptlrpc_uuid_to_peer(uuid, &peer);
        if (err != 0) {
                CERROR("cannot find peer %s!\n", uuid->uuid);
                return NULL;
        }

        c = ptlrpc_get_connection(&peer, uuid);
        if (c) {
                memcpy(c->c_remote_uuid.uuid,
                       uuid->uuid, sizeof(c->c_remote_uuid.uuid));
        }

        CDEBUG(D_INFO, "%s -> %p\n", uuid->uuid, c);

        return c;
}

void ptlrpc_readdress_connection(struct ptlrpc_connection *conn,
                                 struct obd_uuid *uuid)
{
        struct ptlrpc_peer peer;
        int err;

        err = ptlrpc_uuid_to_peer(uuid, &peer);
        if (err != 0) {
                CERROR("cannot find peer %s!\n", uuid->uuid);
                return;
        }

        memcpy(&conn->c_peer, &peer, sizeof (peer));
        return;
}

static inline struct ptlrpc_bulk_desc *new_bulk(int npages, int type, int portal)
{
        struct ptlrpc_bulk_desc *desc;

        OBD_ALLOC(desc, offsetof (struct ptlrpc_bulk_desc, bd_iov[npages]));
        if (!desc)
                return NULL;

        spin_lock_init(&desc->bd_lock);
        init_waitqueue_head(&desc->bd_waitq);
        desc->bd_max_iov = npages;
        desc->bd_iov_count = 0;
        desc->bd_md_h = PTL_INVALID_HANDLE;
        desc->bd_portal = portal;
        desc->bd_type = type;
        
        return desc;
}

struct ptlrpc_bulk_desc *ptlrpc_prep_bulk_imp (struct ptlrpc_request *req,
                                               int npages, int type, int portal)
{
        struct obd_import *imp = req->rq_import;
        struct ptlrpc_bulk_desc *desc;

        LASSERT(type == BULK_PUT_SINK || type == BULK_GET_SOURCE);
        desc = new_bulk(npages, type, portal);
        if (desc == NULL)
                RETURN(NULL);

        desc->bd_import_generation = req->rq_import_generation;
        desc->bd_import = class_import_get(imp);
        desc->bd_req = req;

        desc->bd_cbid.cbid_fn  = client_bulk_callback;
        desc->bd_cbid.cbid_arg = desc;

        /* This makes req own desc, and free it when she frees herself */
        req->rq_bulk = desc;

        return desc;
}

struct ptlrpc_bulk_desc *ptlrpc_prep_bulk_exp (struct ptlrpc_request *req,
                                               int npages, int type, int portal)
{
        struct obd_export *exp = req->rq_export;
        struct ptlrpc_bulk_desc *desc;

        LASSERT(type == BULK_PUT_SOURCE || type == BULK_GET_SINK);

        desc = new_bulk(npages, type, portal);
        if (desc == NULL)
                RETURN(NULL);

        desc->bd_export = class_export_get(exp);
        desc->bd_req = req;

        desc->bd_cbid.cbid_fn  = server_bulk_callback;
        desc->bd_cbid.cbid_arg = desc;

        /* NB we don't assign rq_bulk here; server-side requests are
         * re-used, and the handler frees the bulk desc explicitly. */

        return desc;
}

void ptlrpc_prep_bulk_page(struct ptlrpc_bulk_desc *desc,
                           struct page *page, int pageoffset, int len)
{
        LASSERT(desc->bd_iov_count < desc->bd_max_iov);
        LASSERT(page != NULL);
        LASSERT(pageoffset >= 0);
        LASSERT(len > 0);
        LASSERT(pageoffset + len <= PAGE_SIZE);

        desc->bd_nob += len;

        ptlrpc_add_bulk_page(desc, page, pageoffset, len);
}

void ptlrpc_free_bulk(struct ptlrpc_bulk_desc *desc)
{
        ENTRY;

        LASSERT(desc != NULL);
        LASSERT(desc->bd_iov_count != 0x5a5a5a5a); /* not freed already */
        LASSERT(!desc->bd_network_rw);         /* network hands off or */
        LASSERT((desc->bd_export != NULL) ^ (desc->bd_import != NULL));
        if (desc->bd_export)
                class_export_put(desc->bd_export);
        else
                class_import_put(desc->bd_import);

        OBD_FREE(desc, offsetof(struct ptlrpc_bulk_desc, 
                                bd_iov[desc->bd_max_iov]));
        EXIT;
}

struct ptlrpc_request *ptlrpc_prep_req(struct obd_import *imp, int opcode,
                                       int count, int *lengths, char **bufs)
{
        struct ptlrpc_request *request;
        int rc;
        ENTRY;

        LASSERT((unsigned long)imp > 0x1000);

        OBD_ALLOC(request, sizeof(*request));
        if (!request) {
                CERROR("request allocation out of memory\n");
                RETURN(NULL);
        }

        rc = lustre_pack_request(request, count, lengths, bufs);
        if (rc) {
                CERROR("cannot pack request %d\n", rc);
                OBD_FREE(request, sizeof(*request));
                RETURN(NULL);
        }

        if (imp->imp_server_timeout)
                request->rq_timeout = obd_timeout / 2;
        else
                request->rq_timeout = obd_timeout;
        request->rq_send_state = LUSTRE_IMP_FULL;
        request->rq_type = PTL_RPC_MSG_REQUEST;
        request->rq_import = class_import_get(imp);

        request->rq_req_cbid.cbid_fn  = request_out_callback;
        request->rq_req_cbid.cbid_arg = request;

        request->rq_reply_cbid.cbid_fn  = reply_in_callback;
        request->rq_reply_cbid.cbid_arg = request;
        
        request->rq_phase = RQ_PHASE_NEW;

        /* XXX FIXME bug 249 */
        request->rq_request_portal = imp->imp_client->cli_request_portal;
        request->rq_reply_portal = imp->imp_client->cli_reply_portal;

        spin_lock_init(&request->rq_lock);
        INIT_LIST_HEAD(&request->rq_list);
        INIT_LIST_HEAD(&request->rq_replay_list);
        INIT_LIST_HEAD(&request->rq_set_chain);
        init_waitqueue_head(&request->rq_reply_waitq);
        request->rq_xid = ptlrpc_next_xid();
        atomic_set(&request->rq_refcount, 1);

        request->rq_reqmsg->opc = opcode;
        request->rq_reqmsg->flags = 0;

        RETURN(request);
}

struct ptlrpc_request_set *ptlrpc_prep_set(void)
{
        struct ptlrpc_request_set *set;

        OBD_ALLOC(set, sizeof *set);
        if (!set)
                RETURN(NULL);
        INIT_LIST_HEAD(&set->set_requests);
        init_waitqueue_head(&set->set_waitq);
        set->set_remaining = 0;
        spin_lock_init(&set->set_new_req_lock);
        INIT_LIST_HEAD(&set->set_new_requests);

        RETURN(set);
}

/* Finish with this set; opposite of prep_set. */
void ptlrpc_set_destroy(struct ptlrpc_request_set *set)
{
        struct list_head *tmp;
        struct list_head *next;
        int               expected_phase;
        int               n = 0;
        ENTRY;

        /* Requests on the set should either all be completed, or all be new */
        expected_phase = (set->set_remaining == 0) ?
                         RQ_PHASE_COMPLETE : RQ_PHASE_NEW;
        list_for_each (tmp, &set->set_requests) {
                struct ptlrpc_request *req =
                        list_entry(tmp, struct ptlrpc_request, rq_set_chain);

                LASSERT(req->rq_phase == expected_phase);
                n++;
        }

        LASSERT(set->set_remaining == 0 || set->set_remaining == n);

        list_for_each_safe(tmp, next, &set->set_requests) {
                struct ptlrpc_request *req =
                        list_entry(tmp, struct ptlrpc_request, rq_set_chain);
                list_del_init(&req->rq_set_chain);

                LASSERT(req->rq_phase == expected_phase);

                if (req->rq_phase == RQ_PHASE_NEW) {

                        if (req->rq_interpret_reply != NULL) {
                                int (*interpreter)(struct ptlrpc_request *,
                                                   void *, int) =
                                        req->rq_interpret_reply;

                                /* higher level (i.e. LOV) failed;
                                 * let the sub reqs clean up */
                                req->rq_status = -EBADR;
                                interpreter(req, &req->rq_async_args,
                                            req->rq_status);
                        }
                        set->set_remaining--;
                }

                req->rq_set = NULL;
                ptlrpc_req_finished (req);
        }

        LASSERT(set->set_remaining == 0);

        OBD_FREE(set, sizeof(*set));
        EXIT;
}

void ptlrpc_set_add_req(struct ptlrpc_request_set *set,
                        struct ptlrpc_request *req)
{
        /* The set takes over the caller's request reference */
        list_add_tail(&req->rq_set_chain, &set->set_requests);
        req->rq_set = set;
        set->set_remaining++;
        atomic_inc(&req->rq_import->imp_inflight);
}

/* lock so many callers can add things, the context that owns the set
 * is supposed to notice these and move them into the set proper. */
void ptlrpc_set_add_new_req(struct ptlrpc_request_set *set,
                            struct ptlrpc_request *req)
{
        unsigned long flags;
        spin_lock_irqsave(&set->set_new_req_lock, flags);
        /* The set takes over the caller's request reference */
        list_add_tail(&req->rq_set_chain, &set->set_new_requests);
        req->rq_set = set;
        spin_unlock_irqrestore(&set->set_new_req_lock, flags);
}

/*
 * Based on the current state of the import, determine if the request
 * can be sent, is an error, or should be delayed.
 *
 * Returns true if this request should be delayed. If false, and
 * *status is set, then the request can not be sent and *status is the
 * error code.  If false and status is 0, then request can be sent.
 *
 * The imp->imp_lock must be held.
 */
static int ptlrpc_import_delay_req(struct obd_import *imp, 
                                   struct ptlrpc_request *req, int *status)
{
        int delay = 0;
        ENTRY;

        LASSERT (status != NULL);
        *status = 0;

        if (imp->imp_state == LUSTRE_IMP_NEW) {
                DEBUG_REQ(D_ERROR, req, "Uninitialized import.");
                *status = -EIO;
                LBUG();
        }
        else if (imp->imp_state == LUSTRE_IMP_CLOSED) {
                DEBUG_REQ(D_ERROR, req, "IMP_CLOSED ");
                *status = -EIO;
        }
        /* allow CONNECT even if import is invalid */
        else if (req->rq_send_state == LUSTRE_IMP_CONNECTING &&
                 imp->imp_state == LUSTRE_IMP_CONNECTING) {
                ;
        }
        /*
         * If the import has been invalidated (such as by an OST failure), the
         * request must fail with -EIO.  
         */
        else if (imp->imp_invalid) {
                DEBUG_REQ(D_ERROR, req, "IMP_INVALID");
                *status = -EIO;
        } 
        else if (req->rq_import_generation != imp->imp_generation) {
                DEBUG_REQ(D_ERROR, req, "req wrong generation:");
                *status = -EIO;
        } 
        else if (req->rq_send_state != imp->imp_state) {
                if (imp->imp_obd->obd_no_recov || imp->imp_dlm_fake 
                    || req->rq_no_delay) 
                        *status = -EWOULDBLOCK;
                else
                        delay = 1;
        }

        RETURN(delay);
}

static int ptlrpc_check_reply(struct ptlrpc_request *req)
{
        unsigned long flags;
        int rc = 0;
        ENTRY;

        /* serialise with network callback */
        spin_lock_irqsave (&req->rq_lock, flags);

        if (req->rq_replied) {
                DEBUG_REQ(D_NET, req, "REPLIED:");
                GOTO(out, rc = 1);
        }

        if (req->rq_err) {
                DEBUG_REQ(D_ERROR, req, "ABORTED:");
                GOTO(out, rc = 1);
        }

        if (req->rq_resend) {
                DEBUG_REQ(D_ERROR, req, "RESEND:");
                GOTO(out, rc = 1);
        }

        if (req->rq_restart) {
                DEBUG_REQ(D_ERROR, req, "RESTART:");
                GOTO(out, rc = 1);
        }
        EXIT;
 out:
        spin_unlock_irqrestore (&req->rq_lock, flags);
        DEBUG_REQ(D_NET, req, "rc = %d for", rc);
        return rc;
}

static int ptlrpc_check_status(struct ptlrpc_request *req)
{
        int err;
        ENTRY;

        err = req->rq_repmsg->status;
        if (req->rq_repmsg->type == PTL_RPC_MSG_ERR) {
                DEBUG_REQ(D_ERROR, req, "type == PTL_RPC_MSG_ERR, err == %d", 
                          err);
                RETURN(err < 0 ? err : -EINVAL);
        }

        if (err < 0) {
                DEBUG_REQ(D_INFO, req, "status is %d", err);
        } else if (err > 0) {
                /* XXX: translate this error from net to host */
                DEBUG_REQ(D_INFO, req, "status is %d", err);
        }

        RETURN(err);
}

static int after_reply(struct ptlrpc_request *req)
{
        unsigned long flags;
        struct obd_import *imp = req->rq_import;
        int rc;
        ENTRY;

        LASSERT(!req->rq_receiving_reply);

        /* NB Until this point, the whole of the incoming message,
         * including buflens, status etc is in the sender's byte order. */

#if SWAB_PARANOIA
        /* Clear reply swab mask; this is a new reply in sender's byte order */
        req->rq_rep_swab_mask = 0;
#endif
        LASSERT (req->rq_nob_received <= req->rq_replen);
        rc = lustre_unpack_msg(req->rq_repmsg, req->rq_nob_received);
        if (rc) {
                CERROR("unpack_rep failed: %d\n", rc);
                RETURN(-EPROTO);
        }

        if (req->rq_repmsg->type != PTL_RPC_MSG_REPLY &&
            req->rq_repmsg->type != PTL_RPC_MSG_ERR) {
                CERROR("invalid packet type received (type=%u)\n",
                       req->rq_repmsg->type);
                RETURN(-EPROTO);
        }

        /* Store transno in reqmsg for replay. */
        req->rq_reqmsg->transno = req->rq_transno = req->rq_repmsg->transno;

        rc = ptlrpc_check_status(req);

        /* Either we've been evicted, or the server has failed for
         * some reason. Try to reconnect, and if that fails, punt to the
         * upcall. */
        if (rc == -ENOTCONN) {
                if (req->rq_send_state != LUSTRE_IMP_FULL ||
                    imp->imp_obd->obd_no_recov || imp->imp_dlm_fake) {
                        RETURN(-ENOTCONN);
                }

                ptlrpc_request_handle_notconn(req);

                RETURN(rc);
        }

        if (req->rq_import->imp_replayable) {
                spin_lock_irqsave(&imp->imp_lock, flags);
                if (req->rq_replay || req->rq_transno != 0)
                        ptlrpc_retain_replayable_request(req, imp);
                else if (req->rq_commit_cb != NULL)
                        req->rq_commit_cb(req);

                if (req->rq_transno > imp->imp_max_transno)
                        imp->imp_max_transno = req->rq_transno;

                /* Replay-enabled imports return commit-status information. */
                if (req->rq_repmsg->last_committed)
                        imp->imp_peer_committed_transno =
                                req->rq_repmsg->last_committed;
                ptlrpc_free_committed(imp);
                spin_unlock_irqrestore(&imp->imp_lock, flags);
        }

        RETURN(rc);
}

static int ptlrpc_send_new_req(struct ptlrpc_request *req)
{
        struct obd_import     *imp;
        unsigned long          flags;
        int rc;
        ENTRY;

        LASSERT(req->rq_phase == RQ_PHASE_NEW);
        req->rq_phase = RQ_PHASE_RPC;

        imp = req->rq_import;
        spin_lock_irqsave(&imp->imp_lock, flags);

        req->rq_import_generation = imp->imp_generation;

        if (ptlrpc_import_delay_req(imp, req, &rc)) {
                spin_lock (&req->rq_lock);
                req->rq_waiting = 1;
                spin_unlock (&req->rq_lock);

                DEBUG_REQ(D_HA, req, "req from PID %d waiting for recovery: "
                          "(%s != %s)",
                          req->rq_reqmsg->status, 
                          ptlrpc_import_state_name(req->rq_send_state),
                          ptlrpc_import_state_name(imp->imp_state));
                LASSERT(list_empty (&req->rq_list));

                list_add_tail(&req->rq_list, &imp->imp_delayed_list);
                spin_unlock_irqrestore(&imp->imp_lock, flags);
                RETURN(0);
        }

        if (rc != 0) {
                spin_unlock_irqrestore(&imp->imp_lock, flags);
                req->rq_status = rc;
                req->rq_phase = RQ_PHASE_INTERPRET;
                RETURN(rc);
        }

        /* XXX this is the same as ptlrpc_queue_wait */
        LASSERT(list_empty(&req->rq_list));
        list_add_tail(&req->rq_list, &imp->imp_sending_list);
        spin_unlock_irqrestore(&imp->imp_lock, flags);

        req->rq_reqmsg->status = current->pid;
        CDEBUG(D_RPCTRACE, "Sending RPC pname:cluuid:pid:xid:ni:nid:opc"
               " %s:%s:%d:"LPU64":%s:"LPX64":%d\n", current->comm,
               imp->imp_obd->obd_uuid.uuid, req->rq_reqmsg->status,
               req->rq_xid,
               imp->imp_connection->c_peer.peer_ni->pni_name,
               imp->imp_connection->c_peer.peer_nid,
               req->rq_reqmsg->opc);

        rc = ptl_send_rpc(req);
        if (rc) {
                DEBUG_REQ(D_HA, req, "send failed (%d); expect timeout", rc);
                req->rq_timeout = 1;
                RETURN(rc);
        }
        RETURN(0);
}

int ptlrpc_check_set(struct ptlrpc_request_set *set)
{
        unsigned long flags;
        struct list_head *tmp;
        int force_timer_recalc = 0;
        ENTRY;

        if (set->set_remaining == 0)
                RETURN(1);

        list_for_each(tmp, &set->set_requests) {
                struct ptlrpc_request *req =
                        list_entry(tmp, struct ptlrpc_request, rq_set_chain);
                struct obd_import *imp = req->rq_import;
                int rc = 0;

                if (req->rq_phase == RQ_PHASE_NEW &&
                    ptlrpc_send_new_req(req)) {
                        force_timer_recalc = 1;
                }

                if (!(req->rq_phase == RQ_PHASE_RPC ||
                      req->rq_phase == RQ_PHASE_BULK ||
                      req->rq_phase == RQ_PHASE_INTERPRET ||
                      req->rq_phase == RQ_PHASE_COMPLETE)) {
                        DEBUG_REQ(D_ERROR, req, "bad phase %x", req->rq_phase);
                        LBUG();
                }

                if (req->rq_phase == RQ_PHASE_COMPLETE)
                        continue;

                if (req->rq_phase == RQ_PHASE_INTERPRET)
                        GOTO(interpret, req->rq_status);

                if (req->rq_err) {
                        ptlrpc_unregister_reply(req);
                        if (req->rq_status == 0)
                                req->rq_status = -EIO;
                        req->rq_phase = RQ_PHASE_INTERPRET;

                        spin_lock_irqsave(&imp->imp_lock, flags);
                        list_del_init(&req->rq_list);
                        spin_unlock_irqrestore(&imp->imp_lock, flags);

                        GOTO(interpret, req->rq_status);
                }

                /* ptlrpc_queue_wait->l_wait_event guarantees that rq_intr
                 * will only be set after rq_timedout, but the oig waiting
                 * path sets rq_intr irrespective of whether ptlrpcd has
                 * seen a timeout.  our policy is to only interpret 
                 * interrupted rpcs after they have timed out */
                if (req->rq_intr && (req->rq_timedout || req->rq_waiting)) {
                        /* NB could be on delayed list */
                        ptlrpc_unregister_reply(req);
                        req->rq_status = -EINTR;
                        req->rq_phase = RQ_PHASE_INTERPRET;

                        spin_lock_irqsave(&imp->imp_lock, flags);
                        list_del_init(&req->rq_list);
                        spin_unlock_irqrestore(&imp->imp_lock, flags);

                        GOTO(interpret, req->rq_status);
                }

                if (req->rq_phase == RQ_PHASE_RPC) {
                        if (req->rq_waiting || req->rq_resend) {
                                int status;

                                LASSERT (!ptlrpc_client_receiving_reply(req));
                                LASSERT (req->rq_bulk == NULL ||
                                         !ptlrpc_bulk_active(req->rq_bulk));

                                spin_lock_irqsave(&imp->imp_lock, flags);

                                if (ptlrpc_import_delay_req(imp, req, &status)) {
                                        spin_unlock_irqrestore(&imp->imp_lock,
                                                               flags);
                                        continue;
                                } 

                                list_del_init(&req->rq_list);
                                if (status != 0)  {
                                        req->rq_status = status;
                                        req->rq_phase = RQ_PHASE_INTERPRET;
                                        spin_unlock_irqrestore(&imp->imp_lock,
                                                               flags);
                                        GOTO(interpret, req->rq_status);
                                }
                                if (req->rq_no_resend) {
                                        req->rq_status = -ENOTCONN;
                                        req->rq_phase = RQ_PHASE_INTERPRET;
                                        spin_unlock_irqrestore(&imp->imp_lock,
                                                               flags);
                                        GOTO(interpret, req->rq_status);
                                }
                                list_add_tail(&req->rq_list,
                                              &imp->imp_sending_list);

                                spin_unlock_irqrestore(&imp->imp_lock, flags);

                                req->rq_waiting = 0;
                                if (req->rq_resend) {
                                        lustre_msg_add_flags(req->rq_reqmsg,
                                                             MSG_RESENT);
                                        ptlrpc_unregister_reply(req);
                                        if (req->rq_bulk) {
                                                __u64 old_xid = req->rq_xid;

                                                /* ensure previous bulk fails */
                                                req->rq_xid = ptlrpc_next_xid();
                                                CDEBUG(D_HA, "resend bulk "
                                                       "old x"LPU64
                                                       " new x"LPU64"\n",
                                                       old_xid, req->rq_xid);
                                        }
                                }

                                rc = ptl_send_rpc(req);
                                if (rc) {
                                        DEBUG_REQ(D_HA, req, "send failed (%d)",
                                                  rc);
                                        force_timer_recalc = 1;
                                        req->rq_timeout = 0;
                                }
                                /* need to reset the timeout */
                                force_timer_recalc = 1;
                        }

                        /* Still waiting for a reply? */
                        if (ptlrpc_client_receiving_reply(req))
                                continue;

                        /* Did we actually receive a reply? */
                        if (!ptlrpc_client_replied(req))
                                continue;

                        spin_lock_irqsave(&imp->imp_lock, flags);
                        list_del_init(&req->rq_list);
                        spin_unlock_irqrestore(&imp->imp_lock, flags);

                        req->rq_status = after_reply(req);
                        if (req->rq_resend) {
                                /* Add this req to the delayed list so
                                   it can be errored if the import is
                                   evicted after recovery. */
                                spin_lock_irqsave (&req->rq_lock, flags);
                                list_add_tail(&req->rq_list, 
                                              &imp->imp_delayed_list);
                                spin_unlock_irqrestore(&req->rq_lock, flags);
                                continue;
                        }

                        /* If there is no bulk associated with this request,
                         * then we're done and should let the interpreter
                         * process the reply.  Similarly if the RPC returned
                         * an error, and therefore the bulk will never arrive.
                         */
                        if (req->rq_bulk == NULL || req->rq_status != 0) {
                                req->rq_phase = RQ_PHASE_INTERPRET;
                                GOTO(interpret, req->rq_status);
                        }

                        req->rq_phase = RQ_PHASE_BULK;
                }

                LASSERT(req->rq_phase == RQ_PHASE_BULK);
                if (ptlrpc_bulk_active(req->rq_bulk))
                        continue;

                if (!req->rq_bulk->bd_success) {
                        /* The RPC reply arrived OK, but the bulk screwed
                         * up!  Dead wierd since the server told us the RPC
                         * was good after getting the REPLY for her GET or
                         * the ACK for her PUT. */
                        DEBUG_REQ(D_ERROR, req, "bulk transfer failed");
                        LBUG();
                }

                req->rq_phase = RQ_PHASE_INTERPRET;

        interpret:
                LASSERT(req->rq_phase == RQ_PHASE_INTERPRET);
                LASSERT(!req->rq_receiving_reply);

                ptlrpc_unregister_reply(req);
                if (req->rq_bulk != NULL)
                        ptlrpc_unregister_bulk (req);

                req->rq_phase = RQ_PHASE_COMPLETE;

                if (req->rq_interpret_reply != NULL) {
                        int (*interpreter)(struct ptlrpc_request *,void *,int) =
                                req->rq_interpret_reply;
                        req->rq_status = interpreter(req, &req->rq_async_args,
                                                     req->rq_status);
                }

                CDEBUG(D_RPCTRACE, "Completed RPC pname:cluuid:pid:xid:ni:nid:"
                       "opc %s:%s:%d:"LPU64":%s:"LPX64":%d\n", current->comm,
                       imp->imp_obd->obd_uuid.uuid, req->rq_reqmsg->status,
                       req->rq_xid,
                       imp->imp_connection->c_peer.peer_ni->pni_name,
                       imp->imp_connection->c_peer.peer_nid,
                       req->rq_reqmsg->opc);

                set->set_remaining--;

                atomic_dec(&imp->imp_inflight);
                wake_up(&imp->imp_recovery_waitq);
        }

        /* If we hit an error, we want to recover promptly. */
        RETURN(set->set_remaining == 0 || force_timer_recalc);
}

int ptlrpc_expire_one_request(struct ptlrpc_request *req)
{
        unsigned long      flags;
        struct obd_import *imp = req->rq_import;
        ENTRY;

        DEBUG_REQ(D_ERROR, req, "timeout");

        spin_lock_irqsave (&req->rq_lock, flags);
        req->rq_timedout = 1;
        spin_unlock_irqrestore (&req->rq_lock, flags);

        ptlrpc_unregister_reply (req);

        if (req->rq_bulk != NULL)
                ptlrpc_unregister_bulk (req);

        if (imp == NULL) {
                DEBUG_REQ(D_HA, req, "NULL import: already cleaned up?");
                RETURN(1);
        }

        /* The DLM server doesn't want recovery run on its imports. */
        if (imp->imp_dlm_fake)
                RETURN(1);

        /* If this request is for recovery or other primordial tasks,
         * then error it out here. */
        if (req->rq_send_state != LUSTRE_IMP_FULL || 
            imp->imp_obd->obd_no_recov) {
                spin_lock_irqsave (&req->rq_lock, flags);
                req->rq_status = -ETIMEDOUT;
                req->rq_err = 1;
                spin_unlock_irqrestore (&req->rq_lock, flags);
                RETURN(1);
        }

        ptlrpc_fail_import(imp, req->rq_import_generation);

        RETURN(0);
}

int ptlrpc_expired_set(void *data)
{
        struct ptlrpc_request_set *set = data;
        struct list_head          *tmp;
        time_t                     now = LTIME_S (CURRENT_TIME);
        ENTRY;

        LASSERT(set != NULL);

        /* A timeout expired; see which reqs it applies to... */
        list_for_each (tmp, &set->set_requests) {
                struct ptlrpc_request *req =
                        list_entry(tmp, struct ptlrpc_request, rq_set_chain);

                /* request in-flight? */
                if (!((req->rq_phase == RQ_PHASE_RPC && !req->rq_waiting 
                       && !req->rq_resend) ||
                      (req->rq_phase == RQ_PHASE_BULK)))
                        continue;

                if (req->rq_timedout ||           /* already dealt with */
                    req->rq_sent + req->rq_timeout > now) /* not expired */
                        continue;

                /* deal with this guy */
                ptlrpc_expire_one_request (req);
        }

        /* When waiting for a whole set, we always to break out of the
         * sleep so we can recalculate the timeout, or enable interrupts
         * iff everyone's timed out.
         */
        RETURN(1);
}

void ptlrpc_mark_interrupted(struct ptlrpc_request *req)
{
        unsigned long flags;
        spin_lock_irqsave(&req->rq_lock, flags);
        req->rq_intr = 1;
        spin_unlock_irqrestore(&req->rq_lock, flags);
}

void ptlrpc_interrupted_set(void *data)
{
        struct ptlrpc_request_set *set = data;
        struct list_head *tmp;

        LASSERT(set != NULL);
        CERROR("INTERRUPTED SET %p\n", set);

        list_for_each(tmp, &set->set_requests) {
                struct ptlrpc_request *req =
                        list_entry(tmp, struct ptlrpc_request, rq_set_chain);

                if (req->rq_phase != RQ_PHASE_RPC)
                        continue;

                ptlrpc_mark_interrupted(req);
        }
}

int ptlrpc_set_next_timeout(struct ptlrpc_request_set *set)
{
        struct list_head      *tmp;
        time_t                 now = LTIME_S(CURRENT_TIME);
        time_t                 deadline;
        int                    timeout = 0;
        struct ptlrpc_request *req;
        ENTRY;

        SIGNAL_MASK_ASSERT(); /* XXX BUG 1511 */

        list_for_each(tmp, &set->set_requests) {
                req = list_entry(tmp, struct ptlrpc_request, rq_set_chain);

                /* request in-flight? */
                if (!((req->rq_phase == RQ_PHASE_RPC && !req->rq_waiting) ||
                      (req->rq_phase == RQ_PHASE_BULK)))
                        continue;

                if (req->rq_timedout)   /* already timed out */
                        continue;

                deadline = req->rq_sent + req->rq_timeout;
                if (deadline <= now)    /* actually expired already */
                        timeout = 1;    /* ASAP */
                else if (timeout == 0 || timeout > deadline - now)
                        timeout = deadline - now;
        }
        RETURN(timeout);
}
                

int ptlrpc_set_wait(struct ptlrpc_request_set *set)
{
        struct list_head      *tmp;
        struct ptlrpc_request *req;
        struct l_wait_info     lwi;
        int                    rc, timeout;
        ENTRY;

        LASSERT(!list_empty(&set->set_requests));
        list_for_each(tmp, &set->set_requests) {
                req = list_entry(tmp, struct ptlrpc_request, rq_set_chain);
                if (req->rq_phase == RQ_PHASE_NEW)
                        (void)ptlrpc_send_new_req(req);
        }

        do {
                timeout = ptlrpc_set_next_timeout(set);

                /* wait until all complete, interrupted, or an in-flight
                 * req times out */
                CDEBUG(D_HA, "set %p going to sleep for %d seconds\n",
                       set, timeout);
                lwi = LWI_TIMEOUT_INTR((timeout ? timeout : 1) * HZ,
                                       ptlrpc_expired_set, 
                                       ptlrpc_interrupted_set, set);
                rc = l_wait_event(set->set_waitq, ptlrpc_check_set(set), &lwi);

                LASSERT(rc == 0 || rc == -EINTR || rc == -ETIMEDOUT);

                /* -EINTR => all requests have been flagged rq_intr so next
                 * check completes.
                 * -ETIMEOUTD => someone timed out.  When all reqs have
                 * timed out, signals are enabled allowing completion with
                 * EINTR.
                 * I don't really care if we go once more round the loop in
                 * the error cases -eeb. */
        } while (rc != 0);

        LASSERT(set->set_remaining == 0);

        rc = 0;
        list_for_each(tmp, &set->set_requests) {
                req = list_entry(tmp, struct ptlrpc_request, rq_set_chain);

                LASSERT(req->rq_phase == RQ_PHASE_COMPLETE);
                if (req->rq_status != 0)
                        rc = req->rq_status;
        }

        if (set->set_interpret != NULL) {
                int (*interpreter)(struct ptlrpc_request_set *set,void *,int) =
                        set->set_interpret;
                rc = interpreter (set, &set->set_args, rc);
        }

        RETURN(rc);
}

static void __ptlrpc_free_req(struct ptlrpc_request *request, int locked)
{
        ENTRY;
        if (request == NULL) {
                EXIT;
                return;
        }

        LASSERTF(!request->rq_receiving_reply, "req %p\n", request);
        LASSERTF(request->rq_rqbd == NULL, "req %p\n",request);/* client-side */
        LASSERTF(list_empty(&request->rq_list), "req %p\n", request);
        LASSERTF(list_empty(&request->rq_set_chain), "req %p\n", request);

        /* We must take it off the imp_replay_list first.  Otherwise, we'll set
         * request->rq_reqmsg to NULL while osc_close is dereferencing it. */
        if (request->rq_import != NULL) {
                unsigned long flags = 0;
                if (!locked)
                        spin_lock_irqsave(&request->rq_import->imp_lock, flags);
                list_del_init(&request->rq_replay_list);
                if (!locked)
                        spin_unlock_irqrestore(&request->rq_import->imp_lock,
                                               flags);
        }
        LASSERTF(list_empty(&request->rq_replay_list), "req %p\n", request);

        if (atomic_read(&request->rq_refcount) != 0) {
                DEBUG_REQ(D_ERROR, request,
                          "freeing request with nonzero refcount");
                LBUG();
        }

        if (request->rq_repmsg != NULL) {
                OBD_FREE(request->rq_repmsg, request->rq_replen);
                request->rq_repmsg = NULL;
        }
        if (request->rq_reqmsg != NULL) {
                OBD_FREE(request->rq_reqmsg, request->rq_reqlen);
                request->rq_reqmsg = NULL;
        }
        if (request->rq_export != NULL) {
                class_export_put(request->rq_export);
                request->rq_export = NULL;
        }
        if (request->rq_import != NULL) {
                class_import_put(request->rq_import);
                request->rq_import = NULL;
        }
        if (request->rq_bulk != NULL)
                ptlrpc_free_bulk(request->rq_bulk);

        OBD_FREE(request, sizeof(*request));
        EXIT;
}

void ptlrpc_free_req(struct ptlrpc_request *request)
{
        __ptlrpc_free_req(request, 0);
}

static int __ptlrpc_req_finished(struct ptlrpc_request *request, int locked);
void ptlrpc_req_finished_with_imp_lock(struct ptlrpc_request *request)
{
        LASSERT_SPIN_LOCKED(&request->rq_import->imp_lock);
        (void)__ptlrpc_req_finished(request, 1);
}

static int __ptlrpc_req_finished(struct ptlrpc_request *request, int locked)
{
        ENTRY;
        if (request == NULL)
                RETURN(1);

        if (request == LP_POISON ||
            request->rq_reqmsg == LP_POISON) {
                CERROR("dereferencing freed request (bug 575)\n");
                LBUG();
                RETURN(1);
        }

        DEBUG_REQ(D_INFO, request, "refcount now %u",
                  atomic_read(&request->rq_refcount) - 1);

        if (atomic_dec_and_test(&request->rq_refcount)) {
                __ptlrpc_free_req(request, locked);
                RETURN(1);
        }

        RETURN(0);
}

void ptlrpc_req_finished(struct ptlrpc_request *request)
{
        __ptlrpc_req_finished(request, 0);
}

/* Disengage the client's reply buffer from the network
 * NB does _NOT_ unregister any client-side bulk.
 * IDEMPOTENT, but _not_ safe against concurrent callers.
 * The request owner (i.e. the thread doing the I/O) must call...
 */
void ptlrpc_unregister_reply (struct ptlrpc_request *request)
{
        int                rc;
        wait_queue_head_t *wq;
        struct l_wait_info lwi;

        LASSERT(!in_interrupt ());             /* might sleep */

        if (!ptlrpc_client_receiving_reply(request))
                return;

        PtlMDUnlink (request->rq_reply_md_h);

        /* We have to l_wait_event() whatever the result, to give liblustre
         * a chance to run reply_in_callback() */

        if (request->rq_set == NULL)
                wq = &request->rq_set->set_waitq;
        else
                wq = &request->rq_reply_waitq;

        for (;;) {
                /* Network access will complete in finite time but the HUGE
                 * timeout lets us CWARN for visibility of sluggish NALs */
                lwi = LWI_TIMEOUT(300 * HZ, NULL, NULL);
                rc = l_wait_event (*wq, !ptlrpc_client_receiving_reply(request), &lwi);
                if (rc == 0)
                        return;

                LASSERT (rc == -ETIMEDOUT);
                DEBUG_REQ(D_WARNING, request, "Unexpectedly long timeout");
        }
}

/* caller must hold imp->imp_lock */
void ptlrpc_free_committed(struct obd_import *imp)
{
        struct list_head *tmp, *saved;
        struct ptlrpc_request *req;
        struct ptlrpc_request *last_req = NULL; /* temporary fire escape */
        ENTRY;

        LASSERT(imp != NULL);

        LASSERT_SPIN_LOCKED(&imp->imp_lock);

        CDEBUG(D_HA, "%s: committing for last_committed "LPU64"\n",
               imp->imp_obd->obd_name, imp->imp_peer_committed_transno);

        list_for_each_safe(tmp, saved, &imp->imp_replay_list) {
                req = list_entry(tmp, struct ptlrpc_request, rq_replay_list);

                /* XXX ok to remove when 1357 resolved - rread 05/29/03  */
                LASSERT(req != last_req);
                last_req = req;

                if (req->rq_import_generation < imp->imp_generation) {
                        DEBUG_REQ(D_HA, req, "freeing request with old gen");
                        GOTO(free_req, 0);
                }

                if (req->rq_replay) {
                        DEBUG_REQ(D_HA, req, "keeping (FL_REPLAY)");
                        continue;
                }

                /* not yet committed */
                if (req->rq_transno > imp->imp_peer_committed_transno) {
                        DEBUG_REQ(D_HA, req, "stopping search");
                        break;
                }

                DEBUG_REQ(D_HA, req, "committing (last_committed "LPU64")",
                          imp->imp_peer_committed_transno);
free_req:
                if (req->rq_commit_cb != NULL)
                        req->rq_commit_cb(req);
                list_del_init(&req->rq_replay_list);
                __ptlrpc_req_finished(req, 1);
        }

        EXIT;
        return;
}

void ptlrpc_cleanup_client(struct obd_import *imp)
{
        ENTRY;
        EXIT;
        return;
}

void ptlrpc_resend_req(struct ptlrpc_request *req)
{
        unsigned long flags;

        DEBUG_REQ(D_HA, req, "going to resend");
        req->rq_reqmsg->handle.cookie = 0;
        req->rq_status = -EAGAIN;

        spin_lock_irqsave (&req->rq_lock, flags);
        req->rq_resend = 1;
        req->rq_timedout = 0;
        if (req->rq_bulk) {
                __u64 old_xid = req->rq_xid;
                
                /* ensure previous bulk fails */
                req->rq_xid = ptlrpc_next_xid();
                CDEBUG(D_HA, "resend bulk old x"LPU64" new x"LPU64"\n",
                       old_xid, req->rq_xid);
        }
        ptlrpc_wake_client_req(req);
        spin_unlock_irqrestore (&req->rq_lock, flags);
}

/* XXX: this function and rq_status are currently unused */
void ptlrpc_restart_req(struct ptlrpc_request *req)
{
        unsigned long flags;

        DEBUG_REQ(D_HA, req, "restarting (possibly-)completed request");
        req->rq_status = -ERESTARTSYS;

        spin_lock_irqsave (&req->rq_lock, flags);
        req->rq_restart = 1;
        req->rq_timedout = 0;
        ptlrpc_wake_client_req(req);
        spin_unlock_irqrestore (&req->rq_lock, flags);
}

static int expired_request(void *data)
{
        struct ptlrpc_request *req = data;
        ENTRY;

        RETURN(ptlrpc_expire_one_request(req));
}

static void interrupted_request(void *data)
{
        unsigned long flags;

        struct ptlrpc_request *req = data;
        DEBUG_REQ(D_HA, req, "request interrupted");
        spin_lock_irqsave (&req->rq_lock, flags);
        req->rq_intr = 1;
        spin_unlock_irqrestore (&req->rq_lock, flags);
}

struct ptlrpc_request *ptlrpc_request_addref(struct ptlrpc_request *req)
{
        ENTRY;
        atomic_inc(&req->rq_refcount);
        RETURN(req);
}

void ptlrpc_retain_replayable_request(struct ptlrpc_request *req,
                                      struct obd_import *imp)
{
        struct list_head *tmp;

        LASSERT_SPIN_LOCKED(&imp->imp_lock);

        /* don't re-add requests that have been replayed */
        if (!list_empty(&req->rq_replay_list))
                return;

        lustre_msg_add_flags(req->rq_reqmsg,
                             MSG_REPLAY);

        LASSERT(imp->imp_replayable);
        /* Balanced in ptlrpc_free_committed, usually. */
        ptlrpc_request_addref(req);
        list_for_each_prev(tmp, &imp->imp_replay_list) {
                struct ptlrpc_request *iter =
                        list_entry(tmp, struct ptlrpc_request, rq_replay_list);

                /* We may have duplicate transnos if we create and then
                 * open a file, or for closes retained if to match creating
                 * opens, so use req->rq_xid as a secondary key.
                 * (See bugs 684, 685, and 428.)
                 * XXX no longer needed, but all opens need transnos!
                 */
                if (iter->rq_transno > req->rq_transno)
                        continue;

                if (iter->rq_transno == req->rq_transno) {
                        LASSERT(iter->rq_xid != req->rq_xid);
                        if (iter->rq_xid > req->rq_xid)
                                continue;
                }

                list_add(&req->rq_replay_list, &iter->rq_replay_list);
                return;
        }

        list_add_tail(&req->rq_replay_list, &imp->imp_replay_list);
}

int ptlrpc_queue_wait(struct ptlrpc_request *req)
{
        int rc = 0;
        int brc;
        struct l_wait_info lwi;
        struct obd_import *imp = req->rq_import;
        unsigned long flags;
        int timeout = 0;
        ENTRY;

        LASSERT(req->rq_set == NULL);
        LASSERT(!req->rq_receiving_reply);
        atomic_inc(&imp->imp_inflight);

        /* for distributed debugging */
        req->rq_reqmsg->status = current->pid;
        LASSERT(imp->imp_obd != NULL);
        CDEBUG(D_RPCTRACE, "Sending RPC pname:cluuid:pid:xid:ni:nid:opc "
               "%s:%s:%d:"LPU64":%s:"LPX64":%d\n", current->comm,
               imp->imp_obd->obd_uuid.uuid,
               req->rq_reqmsg->status, req->rq_xid,
               imp->imp_connection->c_peer.peer_ni->pni_name,
               imp->imp_connection->c_peer.peer_nid,
               req->rq_reqmsg->opc);

        /* Mark phase here for a little debug help */
        req->rq_phase = RQ_PHASE_RPC;

        spin_lock_irqsave(&imp->imp_lock, flags);
        req->rq_import_generation = imp->imp_generation;
restart:
        if (ptlrpc_import_delay_req(imp, req, &rc)) {
                list_del(&req->rq_list);

                list_add_tail(&req->rq_list, &imp->imp_delayed_list);
                spin_unlock_irqrestore(&imp->imp_lock, flags);

                DEBUG_REQ(D_HA, req, "\"%s\" waiting for recovery: (%s != %s)",
                          current->comm, 
                          ptlrpc_import_state_name(req->rq_send_state), 
                          ptlrpc_import_state_name(imp->imp_state));
                lwi = LWI_INTR(interrupted_request, req);
                rc = l_wait_event(req->rq_reply_waitq,
                                  (req->rq_send_state == imp->imp_state ||
                                   req->rq_err),
                                  &lwi);
                DEBUG_REQ(D_HA, req, "\"%s\" awake: (%s == %s or %d == 1)",
                          current->comm, 
                          ptlrpc_import_state_name(imp->imp_state), 
                          ptlrpc_import_state_name(req->rq_send_state),
                          req->rq_err);

                spin_lock_irqsave(&imp->imp_lock, flags);
                list_del_init(&req->rq_list);

                if (req->rq_err) {
                        rc = -EIO;
                } 
                else if (req->rq_intr) {
                        rc = -EINTR;
                }
                else if (req->rq_no_resend) {
                        spin_unlock_irqrestore(&imp->imp_lock, flags);
                        GOTO(out, rc = -ETIMEDOUT);
                }
                else {
                        GOTO(restart, rc);
                }
        } 

        if (rc != 0) {
                list_del_init(&req->rq_list);
                spin_unlock_irqrestore(&imp->imp_lock, flags);
                req->rq_status = rc; // XXX this ok?
                GOTO(out, rc);
        }

        if (req->rq_resend) {
                lustre_msg_add_flags(req->rq_reqmsg, MSG_RESENT);

                if (req->rq_bulk != NULL)
                        ptlrpc_unregister_bulk (req);

                DEBUG_REQ(D_HA, req, "resending: ");
        }

        /* XXX this is the same as ptlrpc_set_wait */
        LASSERT(list_empty(&req->rq_list));
        list_add_tail(&req->rq_list, &imp->imp_sending_list);
        spin_unlock_irqrestore(&imp->imp_lock, flags);

        rc = ptl_send_rpc(req);
        if (rc) {
                DEBUG_REQ(D_HA, req, "send failed (%d); recovering", rc);
                timeout = 1;
        } else {
                timeout = MAX(req->rq_timeout * HZ, 1);
                DEBUG_REQ(D_NET, req, "-- sleeping");
        }
        lwi = LWI_TIMEOUT_INTR(timeout, expired_request, interrupted_request,
                               req);
        l_wait_event(req->rq_reply_waitq, ptlrpc_check_reply(req), &lwi);
        DEBUG_REQ(D_NET, req, "-- done sleeping");

        CDEBUG(D_RPCTRACE, "Completed RPC pname:cluuid:pid:xid:ni:nid:opc "
               "%s:%s:%d:"LPU64":%s:"LPX64":%d\n", current->comm,
               imp->imp_obd->obd_uuid.uuid,
               req->rq_reqmsg->status, req->rq_xid,
               imp->imp_connection->c_peer.peer_ni->pni_name,
               imp->imp_connection->c_peer.peer_nid,
               req->rq_reqmsg->opc);

        spin_lock_irqsave(&imp->imp_lock, flags);
        list_del_init(&req->rq_list);
        spin_unlock_irqrestore(&imp->imp_lock, flags);

        /* If the reply was received normally, this just grabs the spinlock
         * (ensuring the reply callback has returned), sees that
         * req->rq_receiving_reply is clear and returns. */
        ptlrpc_unregister_reply (req);

        if (req->rq_err)
                GOTO(out, rc = -EIO);

        /* Resend if we need to, unless we were interrupted. */
        if (req->rq_resend && !req->rq_intr) {
                /* ...unless we were specifically told otherwise. */
                if (req->rq_no_resend)
                        GOTO(out, rc = -ETIMEDOUT);
                spin_lock_irqsave(&imp->imp_lock, flags);
                goto restart;
        }

        if (req->rq_intr) {
                /* Should only be interrupted if we timed out. */
                if (!req->rq_timedout)
                        DEBUG_REQ(D_ERROR, req,
                                  "rq_intr set but rq_timedout not");
                GOTO(out, rc = -EINTR);
        }

        if (req->rq_timedout) {                 /* non-recoverable timeout */
                GOTO(out, rc = -ETIMEDOUT);
        }

        if (!req->rq_replied) {
                /* How can this be? -eeb */
                DEBUG_REQ(D_ERROR, req, "!rq_replied: ");
                LBUG();
                GOTO(out, rc = req->rq_status);
        }

        rc = after_reply (req);
        /* NB may return +ve success rc */
        if (req->rq_resend) {
                spin_lock_irqsave(&imp->imp_lock, flags);
                goto restart;
        }

 out:
        if (req->rq_bulk != NULL) {
                if (rc >= 0) {                  
                        /* success so far.  Note that anything going wrong
                         * with bulk now, is EXTREMELY strange, since the
                         * server must have believed that the bulk
                         * tranferred OK before she replied with success to
                         * me. */
                        lwi = LWI_TIMEOUT(timeout, NULL, NULL);
                        brc = l_wait_event(req->rq_reply_waitq,
                                           !ptlrpc_bulk_active(req->rq_bulk),
                                           &lwi);
                        LASSERT(brc == 0 || brc == -ETIMEDOUT);
                        if (brc != 0) {
                                LASSERT(brc == -ETIMEDOUT);
                                DEBUG_REQ(D_ERROR, req, "bulk timed out");
                                rc = brc;
                        } else if (!req->rq_bulk->bd_success) {
                                DEBUG_REQ(D_ERROR, req, "bulk transfer failed");
                                rc = -EIO;
                        }
                }
                if (rc < 0)
                        ptlrpc_unregister_bulk (req);
        }

        LASSERT(!req->rq_receiving_reply);
        req->rq_phase = RQ_PHASE_INTERPRET;

        atomic_dec(&imp->imp_inflight);
        wake_up(&imp->imp_recovery_waitq);
        RETURN(rc);
}

struct ptlrpc_replay_async_args {
        int praa_old_state;
        int praa_old_status;
};

static int ptlrpc_replay_interpret(struct ptlrpc_request *req,
                                    void * data, int rc)
{
        struct ptlrpc_replay_async_args *aa = data;
        struct obd_import *imp = req->rq_import;
        unsigned long flags;

        atomic_dec(&imp->imp_replay_inflight);
        
        if (!req->rq_replied) {
                CERROR("request replay timed out, restarting recovery\n");
                GOTO(out, rc = -ETIMEDOUT);
        }

#if SWAB_PARANOIA
        /* Clear reply swab mask; this is a new reply in sender's byte order */
        req->rq_rep_swab_mask = 0;
#endif
        LASSERT (req->rq_nob_received <= req->rq_replen);
        rc = lustre_unpack_msg(req->rq_repmsg, req->rq_nob_received);
        if (rc) {
                CERROR("unpack_rep failed: %d\n", rc);
                GOTO(out, rc = -EPROTO);
        }

        if (req->rq_repmsg->type == PTL_RPC_MSG_ERR && 
            req->rq_repmsg->status == -ENOTCONN) 
                GOTO(out, rc = req->rq_repmsg->status);

        /* The transno had better not change over replay. */
        LASSERT(req->rq_reqmsg->transno == req->rq_repmsg->transno);

        DEBUG_REQ(D_HA, req, "got rep");

        /* let the callback do fixups, possibly including in the request */
        if (req->rq_replay_cb)
                req->rq_replay_cb(req);

        if (req->rq_replied && req->rq_repmsg->status != aa->praa_old_status) {
                DEBUG_REQ(D_ERROR, req, "status %d, old was %d",
                          req->rq_repmsg->status, aa->praa_old_status);
        } else {
                /* Put it back for re-replay. */
                req->rq_repmsg->status = aa->praa_old_status;
        }

        spin_lock_irqsave(&imp->imp_lock, flags);
        imp->imp_last_replay_transno = req->rq_transno;
        spin_unlock_irqrestore(&imp->imp_lock, flags);

        /* continue with recovery */
        rc = ptlrpc_import_recovery_state_machine(imp);
 out:
        req->rq_send_state = aa->praa_old_state;
        
        if (rc != 0)
                /* this replay failed, so restart recovery */
                ptlrpc_connect_import(imp, NULL);

        RETURN(rc);
}


int ptlrpc_replay_req(struct ptlrpc_request *req)
{
        struct ptlrpc_replay_async_args *aa;
        ENTRY;

        LASSERT(req->rq_import->imp_state == LUSTRE_IMP_REPLAY);

        /* Not handling automatic bulk replay yet (or ever?) */
        LASSERT(req->rq_bulk == NULL);

        DEBUG_REQ(D_HA, req, "REPLAY");

        LASSERT (sizeof (*aa) <= sizeof (req->rq_async_args));
        aa = (struct ptlrpc_replay_async_args *)&req->rq_async_args;
        memset(aa, 0, sizeof *aa);

        /* Prepare request to be resent with ptlrpcd */
        aa->praa_old_state = req->rq_send_state;
        req->rq_send_state = LUSTRE_IMP_REPLAY;
        req->rq_phase = RQ_PHASE_NEW;
        /*
         * Q: "How can a req get on the replay list if it wasn't replied?"
         * A: "If we failed during the replay of this request, it will still
         *     be on the list, but rq_replied will have been reset to 0."
         */
        if (req->rq_replied) {
                aa->praa_old_status = req->rq_repmsg->status;
                req->rq_status = 0;
                req->rq_replied = 0;
        }

        req->rq_interpret_reply = ptlrpc_replay_interpret;
        atomic_inc(&req->rq_import->imp_replay_inflight);
        ptlrpc_request_addref(req); /* ptlrpcd needs a ref */

        ptlrpcd_add_req(req);
        RETURN(0);
}

void ptlrpc_abort_inflight(struct obd_import *imp)
{
        unsigned long flags;
        struct list_head *tmp, *n;
        ENTRY;

        /* Make sure that no new requests get processed for this import.
         * ptlrpc_{queue,set}_wait must (and does) hold imp_lock while testing
         * this flag and then putting requests on sending_list or delayed_list.
         */
        spin_lock_irqsave(&imp->imp_lock, flags);

        /* XXX locking?  Maybe we should remove each request with the list
         * locked?  Also, how do we know if the requests on the list are
         * being freed at this time?
         */
        list_for_each_safe(tmp, n, &imp->imp_sending_list) {
                struct ptlrpc_request *req =
                        list_entry(tmp, struct ptlrpc_request, rq_list);

                DEBUG_REQ(D_HA, req, "inflight");

                spin_lock (&req->rq_lock);
                if (req->rq_import_generation < imp->imp_generation) {
                        req->rq_err = 1;
                        ptlrpc_wake_client_req(req);
                }
                spin_unlock (&req->rq_lock);
        }

        list_for_each_safe(tmp, n, &imp->imp_delayed_list) {
                struct ptlrpc_request *req =
                        list_entry(tmp, struct ptlrpc_request, rq_list);

                DEBUG_REQ(D_HA, req, "aborting waiting req");

                spin_lock (&req->rq_lock);
                if (req->rq_import_generation < imp->imp_generation) {
                        req->rq_err = 1;
                        ptlrpc_wake_client_req(req);
                }
                spin_unlock (&req->rq_lock);
        }

        /* Last chance to free reqs left on the replay list, but we
         * will still leak reqs that haven't comitted.  */
        if (imp->imp_replayable)
                ptlrpc_free_committed(imp);

        spin_unlock_irqrestore(&imp->imp_lock, flags);

        EXIT;
}

static __u64 ptlrpc_last_xid = 0;
static spinlock_t ptlrpc_last_xid_lock = SPIN_LOCK_UNLOCKED;

__u64 ptlrpc_next_xid(void)
{
        __u64 tmp;
        spin_lock(&ptlrpc_last_xid_lock);
        tmp = ++ptlrpc_last_xid;
        spin_unlock(&ptlrpc_last_xid_lock);
        return tmp;
}


