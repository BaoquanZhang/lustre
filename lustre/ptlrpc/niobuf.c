/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (c) 2002, 2003 Cluster File Systems, Inc.
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
#endif
#include <linux/obd_support.h>
#include <linux/lustre_net.h>
#include <linux/lustre_lib.h>
#include <linux/obd.h>
#include "ptlrpc_internal.h"

static int ptl_send_buf (lnet_handle_md_t *mdh, void *base, int len,
                         lnet_ack_req_t ack, struct ptlrpc_cb_id *cbid,
                         struct ptlrpc_connection *conn, int portal, __u64 xid)
{
        int              rc;
        lnet_md_t         md;
        ENTRY;

        LASSERT (portal != 0);
        LASSERT (conn != NULL);
        CDEBUG (D_INFO, "conn=%p id %s\n", conn, libcfs_id2str(conn->c_peer));
        md.start     = base;
        md.length    = len;
        md.threshold = (ack == LNET_ACK_REQ) ? 2 : 1;
        md.options   = PTLRPC_MD_OPTIONS;
        md.user_ptr  = cbid;
        md.eq_handle = ptlrpc_eq_h;

        if (ack == LNET_ACK_REQ &&
            OBD_FAIL_CHECK(OBD_FAIL_PTLRPC_ACK | OBD_FAIL_ONCE)) {
                /* don't ask for the ack to simulate failing client */
                ack = LNET_NOACK_REQ;
                obd_fail_loc |= OBD_FAIL_ONCE | OBD_FAILED;
        }

        rc = LNetMDBind (md, LNET_UNLINK, mdh);
        if (rc != 0) {
                CERROR ("LNetMDBind failed: %d\n", rc);
                LASSERT (rc == -ENOMEM);
                RETURN (-ENOMEM);
        }

        CDEBUG(D_NET, "Sending %d bytes to portal %d, xid "LPD64"\n",
               len, portal, xid);

        rc = LNetPut (conn->c_self, *mdh, ack, 
                      conn->c_peer, portal, xid, 0, 0);
        if (rc != 0) {
                int rc2;
                /* We're going to get an UNLINK event when I unlink below,
                 * which will complete just like any other failed send, so
                 * I fall through and return success here! */
                CERROR("LNetPut(%s, %d, "LPD64") failed: %d\n",
                       libcfs_id2str(conn->c_peer), portal, xid, rc);
                rc2 = LNetMDUnlink(*mdh);
                LASSERTF(rc2 == 0, "rc2 = %d\n", rc2);
        }

        RETURN (0);
}

int ptlrpc_start_bulk_transfer (struct ptlrpc_bulk_desc *desc)
{
        struct ptlrpc_connection *conn = desc->bd_export->exp_connection;
        int                       rc;
        int                       rc2;
        lnet_md_t                 md;
        __u64                     xid;
        ENTRY;

        if (OBD_FAIL_CHECK_ONCE(OBD_FAIL_PTLRPC_BULK_PUT_NET)) 
                RETURN(0);

        /* NB no locking required until desc is on the network */
        LASSERT (!desc->bd_network_rw);
        LASSERT (desc->bd_type == BULK_PUT_SOURCE ||
                 desc->bd_type == BULK_GET_SINK);
        desc->bd_success = 0;

        md.user_ptr = &desc->bd_cbid;
        md.eq_handle = ptlrpc_eq_h;
        md.threshold = 2; /* SENT and ACK/REPLY */
        md.options = PTLRPC_MD_OPTIONS;
        ptlrpc_fill_bulk_md(&md, desc);

        LASSERT (desc->bd_cbid.cbid_fn == server_bulk_callback);
        LASSERT (desc->bd_cbid.cbid_arg == desc);

        /* NB total length may be 0 for a read past EOF, so we send a 0
         * length bulk, since the client expects a bulk event. */

        rc = LNetMDBind(md, LNET_UNLINK, &desc->bd_md_h);
        if (rc != 0) {
                CERROR("LNetMDBind failed: %d\n", rc);
                LASSERT (rc == -ENOMEM);
                RETURN(-ENOMEM);
        }

        /* Client's bulk and reply matchbits are the same */
        xid = desc->bd_req->rq_xid;
        CDEBUG(D_NET, "Transferring %u pages %u bytes via portal %d "
               "id %s xid "LPX64"\n", desc->bd_iov_count,
               desc->bd_nob, desc->bd_portal, 
               libcfs_id2str(conn->c_peer), xid);

        /* Network is about to get at the memory */
        desc->bd_network_rw = 1;

        if (desc->bd_type == BULK_PUT_SOURCE)
                rc = LNetPut (conn->c_self, desc->bd_md_h, LNET_ACK_REQ, 
                              conn->c_peer, desc->bd_portal, xid, 0, 0);
        else
                rc = LNetGet (conn->c_self, desc->bd_md_h, 
                              conn->c_peer, desc->bd_portal, xid, 0);

        if (rc != 0) {
                /* Can't send, so we unlink the MD bound above.  The UNLINK
                 * event this creates will signal completion with failure,
                 * so we return SUCCESS here! */
                CERROR("Transfer(%s, %d, "LPX64") failed: %d\n",
                       libcfs_id2str(conn->c_peer), desc->bd_portal, xid, rc);
                rc2 = LNetMDUnlink(desc->bd_md_h);
                LASSERT (rc2 == 0);
        }

        RETURN(0);
}

void ptlrpc_abort_bulk (struct ptlrpc_bulk_desc *desc)
{
        /* Server side bulk abort. Idempotent. Not thread-safe (i.e. only
         * serialises with completion callback) */
        struct l_wait_info lwi;
        int                rc;

        LASSERT (!in_interrupt ());             /* might sleep */

        if (!ptlrpc_bulk_active(desc))          /* completed or */
                return;                         /* never started */
        
        /* Do not send any meaningful data over the wire for evicted clients */
        if (desc->bd_export && desc->bd_export->exp_failed)
                ptl_rpc_wipe_bulk_pages(desc);

        /* The unlink ensures the callback happens ASAP and is the last
         * one.  If it fails, it must be because completion just happened,
         * but we must still l_wait_event() in this case, to give liblustre
         * a chance to run server_bulk_callback()*/

        LNetMDUnlink (desc->bd_md_h);

        for (;;) {
                /* Network access will complete in finite time but the HUGE
                 * timeout lets us CWARN for visibility of sluggish NALs */
                lwi = LWI_TIMEOUT (300 * HZ, NULL, NULL);
                rc = l_wait_event(desc->bd_waitq, 
                                  !ptlrpc_bulk_active(desc), &lwi);
                if (rc == 0)
                        return;

                LASSERT(rc == -ETIMEDOUT);
                CWARN("Unexpectedly long timeout: desc %p\n", desc);
        }
}

int ptlrpc_register_bulk (struct ptlrpc_request *req)
{
        struct ptlrpc_bulk_desc *desc = req->rq_bulk;
        lnet_process_id_t peer;
        int rc;
        int rc2;
        lnet_handle_me_t  me_h;
        lnet_md_t         md;
        ENTRY;

        if (OBD_FAIL_CHECK_ONCE(OBD_FAIL_PTLRPC_BULK_GET_NET)) 
                RETURN(0);

        /* NB no locking required until desc is on the network */
        LASSERT (desc->bd_nob > 0);
        LASSERT (!desc->bd_network_rw);
        LASSERT (desc->bd_iov_count <= PTLRPC_MAX_BRW_PAGES);
        LASSERT (desc->bd_req != NULL);
        LASSERT (desc->bd_type == BULK_PUT_SINK ||
                 desc->bd_type == BULK_GET_SOURCE);

        desc->bd_success = 0;

        peer = desc->bd_import->imp_connection->c_peer;

        md.user_ptr = &desc->bd_cbid;
        md.eq_handle = ptlrpc_eq_h;
        md.threshold = 1;                       /* PUT or GET */
        md.options = PTLRPC_MD_OPTIONS | 
                     ((desc->bd_type == BULK_GET_SOURCE) ? 
                      LNET_MD_OP_GET : LNET_MD_OP_PUT);
        ptlrpc_fill_bulk_md(&md, desc);

        LASSERT (desc->bd_cbid.cbid_fn == client_bulk_callback);
        LASSERT (desc->bd_cbid.cbid_arg == desc);

        /* XXX Registering the same xid on retried bulk makes my head
         * explode trying to understand how the original request's bulk
         * might interfere with the retried request -eeb */
        LASSERTF (!desc->bd_registered || req->rq_xid != desc->bd_last_xid,
                  "registered: %d  rq_xid: "LPU64" bd_last_xid: "LPU64"\n",
                  desc->bd_registered, req->rq_xid, desc->bd_last_xid);
        desc->bd_registered = 1;
        desc->bd_last_xid = req->rq_xid;

        rc = LNetMEAttach(desc->bd_portal, peer,
                         req->rq_xid, 0, LNET_UNLINK, LNET_INS_AFTER, &me_h);
        if (rc != 0) {
                CERROR("LNetMEAttach failed: %d\n", rc);
                LASSERT (rc == -ENOMEM);
                RETURN (-ENOMEM);
        }

        /* About to let the network at it... */
        desc->bd_network_rw = 1;
        rc = LNetMDAttach(me_h, md, LNET_UNLINK, &desc->bd_md_h);
        if (rc != 0) {
                CERROR("LNetMDAttach failed: %d\n", rc);
                LASSERT (rc == -ENOMEM);
                desc->bd_network_rw = 0;
                rc2 = LNetMEUnlink (me_h);
                LASSERT (rc2 == 0);
                RETURN (-ENOMEM);
        }

        CDEBUG(D_NET, "Setup bulk %s buffers: %u pages %u bytes, xid "LPX64", "
               "portal %u\n",
               desc->bd_type == BULK_GET_SOURCE ? "get-source" : "put-sink",
               desc->bd_iov_count, desc->bd_nob,
               req->rq_xid, desc->bd_portal);
        RETURN(0);
}

void ptlrpc_unregister_bulk (struct ptlrpc_request *req)
{
        /* Disconnect a bulk desc from the network. Idempotent. Not
         * thread-safe (i.e. only interlocks with completion callback). */
        struct ptlrpc_bulk_desc *desc = req->rq_bulk;
        wait_queue_head_t       *wq;
        struct l_wait_info       lwi;
        int                      rc;

        LASSERT (!in_interrupt ());     /* might sleep */

        if (!ptlrpc_bulk_active(desc))  /* completed or */
                return;                 /* never registered */

        LASSERT (desc->bd_req == req);  /* bd_req NULL until registered */

        /* the unlink ensures the callback happens ASAP and is the last
         * one.  If it fails, it must be because completion just happened,
         * but we must still l_wait_event() in this case to give liblustre
         * a chance to run client_bulk_callback() */

        LNetMDUnlink (desc->bd_md_h);
        
        if (req->rq_set != NULL)
                wq = &req->rq_set->set_waitq;
        else
                wq = &req->rq_reply_waitq;

        for (;;) {
                /* Network access will complete in finite time but the HUGE
                 * timeout lets us CWARN for visibility of sluggish NALs */
                lwi = LWI_TIMEOUT (300 * HZ, NULL, NULL);
                rc = l_wait_event(*wq, !ptlrpc_bulk_active(desc), &lwi);
                if (rc == 0)
                        return;

                LASSERT (rc == -ETIMEDOUT);
                DEBUG_REQ(D_WARNING,req,"Unexpectedly long timeout: desc %p\n",
                          desc);
        }
}

int ptlrpc_send_reply (struct ptlrpc_request *req, int may_be_difficult)
{
        struct ptlrpc_service     *svc = req->rq_rqbd->rqbd_service;
        struct ptlrpc_reply_state *rs = req->rq_reply_state;
        struct ptlrpc_connection  *conn;
        int                        rc;

        /* We must already have a reply buffer (only ptlrpc_error() may be
         * called without one).  We must also have a request buffer which
         * is either the actual (swabbed) incoming request, or a saved copy
         * if this is a req saved in target_queue_final_reply(). */
        LASSERT (req->rq_reqmsg != NULL);
        LASSERT (rs != NULL);
        LASSERT (req->rq_repmsg != NULL);
        LASSERT (may_be_difficult || !rs->rs_difficult);
        LASSERT (req->rq_repmsg == &rs->rs_msg);
        LASSERT (rs->rs_cb_id.cbid_fn == reply_out_callback);
        LASSERT (rs->rs_cb_id.cbid_arg == rs);
        LASSERT (req->rq_repmsg != NULL);

        if (req->rq_export && req->rq_export->exp_obd &&
            req->rq_export->exp_obd->obd_fail) {
                /* Failed obd's only send ENODEV */
                req->rq_type = PTL_RPC_MSG_ERR;
                req->rq_status = -ENODEV;
                CDEBUG(D_HA, "sending ENODEV from failed obd %d\n",
                       req->rq_export->exp_obd->obd_minor);
        }

        if (req->rq_type != PTL_RPC_MSG_ERR)
                req->rq_type = PTL_RPC_MSG_REPLY;

        req->rq_repmsg->type   = req->rq_type;
        req->rq_repmsg->status = req->rq_status;
        req->rq_repmsg->opc    = req->rq_reqmsg->opc;

        if (req->rq_export == NULL) 
                conn = ptlrpc_get_connection(req->rq_peer, req->rq_self, NULL);
        else
                conn = ptlrpc_connection_addref(req->rq_export->exp_connection);

        atomic_inc (&svc->srv_outstanding_replies);
        ptlrpc_rs_addref(rs);                   /* +1 ref for the network */

        rc = ptl_send_buf (&rs->rs_md_h, req->rq_repmsg, req->rq_replen,
                           rs->rs_difficult ? LNET_ACK_REQ : LNET_NOACK_REQ,
                           &rs->rs_cb_id, conn,
                           svc->srv_rep_portal, req->rq_xid);
        if (rc != 0) {
                atomic_dec (&svc->srv_outstanding_replies);
                ptlrpc_rs_decref(rs);
        }
        ptlrpc_put_connection(conn);
        return rc;
}

int ptlrpc_reply (struct ptlrpc_request *req)
{
        return (ptlrpc_send_reply (req, 0));
}

int ptlrpc_error(struct ptlrpc_request *req)
{
        int rc;
        ENTRY;

        if (!req->rq_repmsg) {
                rc = lustre_pack_reply(req, 0, NULL, NULL);
                if (rc)
                        RETURN(rc);
        }

        req->rq_type = PTL_RPC_MSG_ERR;

        rc = ptlrpc_send_reply (req, 0);
        RETURN(rc);
}

int ptl_send_rpc_nowait(struct ptlrpc_request *request)
{
        int rc;
        struct ptlrpc_connection *connection;
        unsigned long flags;
        ENTRY;

        LASSERT (request->rq_type == PTL_RPC_MSG_REQUEST);

        if (request->rq_import->imp_obd &&
            request->rq_import->imp_obd->obd_fail) {
                CDEBUG(D_HA, "muting rpc for failed imp obd %s\n",
                       request->rq_import->imp_obd->obd_name);
                /* this prevents us from waiting in ptlrpc_queue_wait */
                request->rq_err = 1;
                RETURN(-ENODEV);
        }
        
        connection = request->rq_import->imp_connection;

        request->rq_reqmsg->handle = request->rq_import->imp_remote_handle;
        request->rq_reqmsg->type = PTL_RPC_MSG_REQUEST;
        request->rq_reqmsg->conn_cnt = request->rq_import->imp_conn_cnt;

        spin_lock_irqsave (&request->rq_lock, flags);
        /* If the MD attach succeeds, there _will_ be a reply_in callback */
        request->rq_receiving_reply = 0;
        /* Clear any flags that may be present from previous sends. */
        request->rq_replied = 0;
        request->rq_err = 0;
        request->rq_timedout = 0;
        request->rq_net_err = 0;
        request->rq_resend = 0;
        request->rq_restart = 0;
        spin_unlock_irqrestore (&request->rq_lock, flags);

        ptlrpc_request_addref(request);       /* +1 ref for the SENT callback */

        request->rq_sent = CURRENT_SECONDS;
        ptlrpc_pinger_sending_on_import(request->rq_import);
        rc = ptl_send_buf(&request->rq_req_md_h, 
                          request->rq_reqmsg, request->rq_reqlen,
                          LNET_NOACK_REQ, &request->rq_req_cbid, 
                          connection,
                          request->rq_request_portal,
                          request->rq_xid);
        if (rc == 0) {
                ptlrpc_lprocfs_rpc_sent(request);
        } else {
                ptlrpc_req_finished (request);          /* drop callback ref */
        }

        return rc;
}


int ptl_send_rpc(struct ptlrpc_request *request)
{
        int rc;
        int rc2;
        struct ptlrpc_connection *connection;
        unsigned long flags;
        lnet_handle_me_t  reply_me_h;
        lnet_md_t         reply_md;
        ENTRY;

        OBD_FAIL_RETURN(OBD_FAIL_PTLRPC_DROP_RPC, 0); 

        LASSERT (request->rq_type == PTL_RPC_MSG_REQUEST);

        /* If this is a re-transmit, we're required to have disengaged
         * cleanly from the previous attempt */
        LASSERT (!request->rq_receiving_reply);

        if (request->rq_import->imp_obd &&
            request->rq_import->imp_obd->obd_fail) {
                CDEBUG(D_HA, "muting rpc for failed imp obd %s\n",
                       request->rq_import->imp_obd->obd_name);
                /* this prevents us from waiting in ptlrpc_queue_wait */
                request->rq_err = 1;
                RETURN(-ENODEV);
        }
        
        connection = request->rq_import->imp_connection;

        if (request->rq_bulk != NULL) {
                rc = ptlrpc_register_bulk (request);
                if (rc != 0)
                        RETURN(rc);
        }

        request->rq_reqmsg->handle = request->rq_import->imp_remote_handle;
        request->rq_reqmsg->type = PTL_RPC_MSG_REQUEST;
        request->rq_reqmsg->conn_cnt = request->rq_import->imp_conn_cnt;

        LASSERT (request->rq_replen != 0);
        if (request->rq_repmsg == NULL)
                OBD_ALLOC(request->rq_repmsg, request->rq_replen);
        if (request->rq_repmsg == NULL)
                GOTO(cleanup_bulk, rc = -ENOMEM);

        rc = LNetMEAttach(request->rq_reply_portal, /* XXX FIXME bug 249 */
                          connection->c_peer, request->rq_xid, 0,
                          LNET_UNLINK, LNET_INS_AFTER, &reply_me_h);
        if (rc != 0) {
                CERROR("LNetMEAttach failed: %d\n", rc);
                LASSERT (rc == -ENOMEM);
                GOTO(cleanup_repmsg, rc = -ENOMEM);
        }

        spin_lock_irqsave (&request->rq_lock, flags);
        /* If the MD attach succeeds, there _will_ be a reply_in callback */
        request->rq_receiving_reply = 1;
        /* Clear any flags that may be present from previous sends. */
        request->rq_replied = 0;
        request->rq_err = 0;
        request->rq_timedout = 0;
        request->rq_net_err = 0;
        request->rq_resend = 0;
        request->rq_restart = 0;
        spin_unlock_irqrestore (&request->rq_lock, flags);

        reply_md.start     = request->rq_repmsg;
        reply_md.length    = request->rq_replen;
        reply_md.threshold = 1;
        reply_md.options   = PTLRPC_MD_OPTIONS | LNET_MD_OP_PUT;
        reply_md.user_ptr  = &request->rq_reply_cbid;
        reply_md.eq_handle = ptlrpc_eq_h;

        rc = LNetMDAttach(reply_me_h, reply_md, LNET_UNLINK, 
                         &request->rq_reply_md_h);
        if (rc != 0) {
                CERROR("LNetMDAttach failed: %d\n", rc);
                LASSERT (rc == -ENOMEM);
                spin_lock_irqsave (&request->rq_lock, flags);
                /* ...but the MD attach didn't succeed... */
                request->rq_receiving_reply = 0;
                spin_unlock_irqrestore (&request->rq_lock, flags);
                GOTO(cleanup_me, rc -ENOMEM);
        }

        CDEBUG(D_NET, "Setup reply buffer: %u bytes, xid "LPU64
               ", portal %u\n",
               request->rq_replen, request->rq_xid,
               request->rq_reply_portal);

        ptlrpc_request_addref(request);       /* +1 ref for the SENT callback */

        request->rq_sent = CURRENT_SECONDS;
        ptlrpc_pinger_sending_on_import(request->rq_import);
        rc = ptl_send_buf(&request->rq_req_md_h, 
                          request->rq_reqmsg, request->rq_reqlen,
                          LNET_NOACK_REQ, &request->rq_req_cbid, 
                          connection,
                          request->rq_request_portal,
                          request->rq_xid);
        if (rc == 0) {
                ptlrpc_lprocfs_rpc_sent(request);
                RETURN(rc);
        }

        ptlrpc_req_finished (request);          /* drop callback ref */

 cleanup_me:
        /* MEUnlink is safe; the PUT didn't even get off the ground, and
         * nobody apart from the PUT's target has the right nid+XID to
         * access the reply buffer. */
        rc2 = LNetMEUnlink(reply_me_h);
        LASSERT (rc2 == 0);
        /* UNLINKED callback called synchronously */
        LASSERT (!request->rq_receiving_reply);

 cleanup_repmsg:
        OBD_FREE(request->rq_repmsg, request->rq_replen);
        request->rq_repmsg = NULL;

 cleanup_bulk:
        if (request->rq_bulk != NULL)
                ptlrpc_unregister_bulk(request);

        return rc;
}

int ptlrpc_register_rqbd (struct ptlrpc_request_buffer_desc *rqbd)
{
        struct ptlrpc_service   *service = rqbd->rqbd_service;
        static lnet_process_id_t  match_id = {LNET_NID_ANY, LNET_PID_ANY};
        int                      rc;
        lnet_md_t                 md;
        lnet_handle_me_t          me_h;

        CDEBUG(D_NET, "LNetMEAttach: portal %d\n",
               service->srv_req_portal);

        if (OBD_FAIL_CHECK_ONCE(OBD_FAIL_PTLRPC_RQBD))
                return (-ENOMEM);

        rc = LNetMEAttach(service->srv_req_portal,
                          match_id, 0, ~0, LNET_UNLINK, LNET_INS_AFTER, &me_h);
        if (rc != 0) {
                CERROR("LNetMEAttach failed: %d\n", rc);
                return (-ENOMEM);
        }

        LASSERT(rqbd->rqbd_refcount == 0);
        rqbd->rqbd_refcount = 1;

        md.start     = rqbd->rqbd_buffer;
        md.length    = service->srv_buf_size;
        md.max_size  = service->srv_max_req_size;
        md.threshold = LNET_MD_THRESH_INF;
        md.options   = PTLRPC_MD_OPTIONS | LNET_MD_OP_PUT | LNET_MD_MAX_SIZE;
        md.user_ptr  = &rqbd->rqbd_cbid;
        md.eq_handle = ptlrpc_eq_h;
        
        rc = LNetMDAttach(me_h, md, LNET_UNLINK, &rqbd->rqbd_md_h);
        if (rc == 0)
                return (0);

        CERROR("LNetMDAttach failed: %d; \n", rc);
        LASSERT (rc == -ENOMEM);
        rc = LNetMEUnlink (me_h);
        LASSERT (rc == 0);
        rqbd->rqbd_refcount = 0;
        
        return (-ENOMEM);
}
