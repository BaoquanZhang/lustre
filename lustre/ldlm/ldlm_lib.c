/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (c) 2003 Cluster File Systems, Inc.
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

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_LDLM

#ifdef __KERNEL__
# include <linux/module.h>
#else
# include <liblustre.h>
#endif
#include <linux/obd.h>
#include <linux/obd_ost.h> /* for LUSTRE_OSC_NAME */
#include <linux/lustre_mds.h> /* for LUSTRE_MDC_NAME */
#include <linux/lustre_mgmt.h>
#include <linux/lustre_dlm.h>
#include <linux/lustre_net.h>

int client_obd_setup(struct obd_device *obddev, obd_count len, void *buf)
{
        struct ptlrpc_connection *conn;
        struct lustre_cfg* lcfg = buf;
        struct client_obd *cli = &obddev->u.cli;
        struct obd_import *imp;
        struct obd_uuid server_uuid;
        int rq_portal, rp_portal, connect_op;
        char *name = obddev->obd_type->typ_name;
        char *mgmt_name = NULL;
        int rc;
        struct obd_device *mgmt_obd;
        mgmtcli_register_for_events_t register_f;
        ENTRY;

        /* In a more perfect world, we would hang a ptlrpc_client off of
         * obd_type and just use the values from there. */
        if (!strcmp(name, LUSTRE_OSC_NAME)) {
                rq_portal = OST_REQUEST_PORTAL;
                rp_portal = OSC_REPLY_PORTAL;
                connect_op = OST_CONNECT;
        } else if (!strcmp(name, LUSTRE_MDC_NAME)) {
                rq_portal = MDS_REQUEST_PORTAL;
                rp_portal = MDC_REPLY_PORTAL;
                connect_op = MDS_CONNECT;
        } else if (!strcmp(name, LUSTRE_MGMTCLI_NAME)) {
                rq_portal = MGMT_REQUEST_PORTAL;
                rp_portal = MGMT_REPLY_PORTAL;
                connect_op = MGMT_CONNECT;
        } else {
                CERROR("unknown client OBD type \"%s\", can't setup\n",
                       name);
                RETURN(-EINVAL);
        }

        if (lcfg->lcfg_inllen1 < 1) {
                CERROR("requires a TARGET UUID\n");
                RETURN(-EINVAL);
        }

        if (lcfg->lcfg_inllen1 > 37) {
                CERROR("client UUID must be less than 38 characters\n");
                RETURN(-EINVAL);
        }

        if (lcfg->lcfg_inllen2 < 1) {
                CERROR("setup requires a SERVER UUID\n");
                RETURN(-EINVAL);
        }

        if (lcfg->lcfg_inllen2 > 37) {
                CERROR("target UUID must be less than 38 characters\n");
                RETURN(-EINVAL);
        }

        sema_init(&cli->cl_sem, 1);
        cli->cl_conn_count = 0;
        memcpy(server_uuid.uuid, lcfg->lcfg_inlbuf2,
               min_t(unsigned int, lcfg->lcfg_inllen2, sizeof(server_uuid)));

        cli->cl_dirty = 0;
        cli->cl_avail_grant = 0;
        cli->cl_dirty_max = OSC_MAX_DIRTY_DEFAULT * 1024 * 1024;
        INIT_LIST_HEAD(&cli->cl_cache_waiters);
        INIT_LIST_HEAD(&cli->cl_loi_ready_list);
        INIT_LIST_HEAD(&cli->cl_loi_write_list);
        INIT_LIST_HEAD(&cli->cl_loi_read_list);
        spin_lock_init(&cli->cl_loi_list_lock);
        cli->cl_r_in_flight = 0;
        cli->cl_w_in_flight = 0;
        spin_lock_init(&cli->cl_read_rpc_hist.oh_lock);
        spin_lock_init(&cli->cl_write_rpc_hist.oh_lock);
        spin_lock_init(&cli->cl_read_page_hist.oh_lock);
        spin_lock_init(&cli->cl_write_page_hist.oh_lock);
        cli->cl_max_pages_per_rpc = PTLRPC_MAX_BRW_PAGES;
        cli->cl_max_rpcs_in_flight = OSC_MAX_RIF_DEFAULT;

        rc = ldlm_get_ref();
        if (rc) {
                CERROR("ldlm_get_ref failed: %d\n", rc);
                GOTO(err, rc);
        }

        conn = ptlrpc_uuid_to_connection(&server_uuid);
        if (conn == NULL)
                GOTO(err_ldlm, rc = -ENOENT);

        ptlrpc_init_client(rq_portal, rp_portal, name,
                           &obddev->obd_ldlm_client);

        imp = class_new_import();
        if (imp == NULL) {
                ptlrpc_put_connection(conn);
                GOTO(err_ldlm, rc = -ENOENT);
        }
        imp->imp_connection = conn;
        imp->imp_client = &obddev->obd_ldlm_client;
        imp->imp_obd = obddev;
        imp->imp_connect_op = connect_op;
        imp->imp_generation = 0;
        imp->imp_initial_recov = 1;
        INIT_LIST_HEAD(&imp->imp_pinger_chain);
        memcpy(imp->imp_target_uuid.uuid, lcfg->lcfg_inlbuf1,
              lcfg->lcfg_inllen1);
        class_import_put(imp);

        cli->cl_import = imp;
        cli->cl_max_mds_easize = sizeof(struct lov_mds_md);
        cli->cl_max_mds_cookiesize = sizeof(struct llog_cookie);
        cli->cl_sandev = to_kdev_t(0);

        if (lcfg->lcfg_inllen3 != 0) {
                if (!strcmp(lcfg->lcfg_inlbuf3, "inactive")) {
                        CDEBUG(D_HA, "marking %s %s->%s as inactive\n",
                               name, obddev->obd_name,
                               imp->imp_target_uuid.uuid);
                        imp->imp_invalid = 1;

                        if (lcfg->lcfg_inllen4 != 0)
                                mgmt_name = lcfg->lcfg_inlbuf4;
                } else {
                        mgmt_name = lcfg->lcfg_inlbuf3;
                }
        }

        if (mgmt_name != NULL) {
                /* Register with management client if we need to. */
                CDEBUG(D_HA, "%s registering with %s for events about %s\n",
                       obddev->obd_name, mgmt_name, server_uuid.uuid);

                mgmt_obd = class_name2obd(mgmt_name);
                if (!mgmt_obd) {
                        CERROR("can't find mgmtcli %s to register\n",
                               mgmt_name);
                        GOTO(err_import, rc = -ENOSYS);
                }

                register_f = inter_module_get("mgmtcli_register_for_events");
                if (!register_f) {
                        CERROR("can't i_m_g mgmtcli_register_for_events\n");
                        GOTO(err_import, rc = -ENOSYS);
                }

                rc = register_f(mgmt_obd, obddev, &imp->imp_target_uuid);
                inter_module_put("mgmtcli_register_for_events");

                if (!rc)
                        cli->cl_mgmtcli_obd = mgmt_obd;
        }

        RETURN(rc);

err_import:
        class_destroy_import(imp);
err_ldlm:
        ldlm_put_ref(0);
err:
        RETURN(rc);

}

int client_obd_cleanup(struct obd_device *obddev, int flags)
{
        struct client_obd *cli = &obddev->u.cli;

        if (!cli->cl_import)
                RETURN(-EINVAL);
        if (cli->cl_mgmtcli_obd) {
                mgmtcli_deregister_for_events_t dereg_f;

                dereg_f = inter_module_get("mgmtcli_deregister_for_events");
                dereg_f(cli->cl_mgmtcli_obd, obddev);
                inter_module_put("mgmtcli_deregister_for_events");
        }
        class_destroy_import(cli->cl_import);
        cli->cl_import = NULL;

        ldlm_put_ref(flags & OBD_OPT_FORCE);

        RETURN(0);
}

int client_connect_import(struct lustre_handle *dlm_handle,
                          struct obd_device *obd,
                          struct obd_uuid *cluuid)
{
        struct client_obd *cli = &obd->u.cli;
        struct obd_import *imp = cli->cl_import;
        struct obd_export *exp;
        int rc;
        ENTRY;

        down(&cli->cl_sem);
        rc = class_connect(dlm_handle, obd, cluuid);
        if (rc)
                GOTO(out_sem, rc);

        cli->cl_conn_count++;
        if (cli->cl_conn_count > 1)
                GOTO(out_sem, rc);
        exp = class_conn2export(dlm_handle);

        if (obd->obd_namespace != NULL)
                CERROR("already have namespace!\n");
        obd->obd_namespace = ldlm_namespace_new(obd->obd_name,
                                                LDLM_NAMESPACE_CLIENT);
        if (obd->obd_namespace == NULL)
                GOTO(out_disco, rc = -ENOMEM);

        imp->imp_dlm_handle = *dlm_handle;
        rc = ptlrpc_init_import(imp);
        if (rc != 0) 
                GOTO(out_ldlm, rc);

        exp->exp_connection = ptlrpc_connection_addref(imp->imp_connection);
        rc = ptlrpc_connect_import(imp, NULL);
        if (rc != 0) {
                LASSERT (imp->imp_state == LUSTRE_IMP_DISCON);
                GOTO(out_ldlm, rc);
        }

        ptlrpc_pinger_add_import(imp);
        EXIT;

        if (rc) {
out_ldlm:
                ldlm_namespace_free(obd->obd_namespace, 0);
                obd->obd_namespace = NULL;
out_disco:
                cli->cl_conn_count--;
                class_disconnect(exp, 0);
        } else {
                class_export_put(exp);
        }
out_sem:
        up(&cli->cl_sem);
        return rc;
}

int client_disconnect_export(struct obd_export *exp, int failover)
{
        struct obd_device *obd = class_exp2obd(exp);
        struct client_obd *cli = &obd->u.cli;
        struct obd_import *imp = cli->cl_import;
        int rc = 0, err;
        ENTRY;

        if (!obd) {
                CERROR("invalid export for disconnect: exp %p cookie "LPX64"\n",
                       exp, exp ? exp->exp_handle.h_cookie : -1);
                RETURN(-EINVAL);
        }

        down(&cli->cl_sem);
        if (!cli->cl_conn_count) {
                CERROR("disconnecting disconnected device (%s)\n",
                       obd->obd_name);
                GOTO(out_sem, rc = -EINVAL);
        }

        cli->cl_conn_count--;
        if (cli->cl_conn_count)
                GOTO(out_no_disconnect, rc = 0);

        /* Some non-replayable imports (MDS's OSCs) are pinged, so just
         * delete it regardless.  (It's safe to delete an import that was
         * never added.) */
        (void)ptlrpc_pinger_del_import(imp);

        if (obd->obd_namespace != NULL) {
                /* obd_no_recov == local only */
                ldlm_cli_cancel_unused(obd->obd_namespace, NULL,
                                       obd->obd_no_recov, NULL);
                ldlm_namespace_free(obd->obd_namespace, obd->obd_no_recov);
                obd->obd_namespace = NULL;
        }

        /* Yeah, obd_no_recov also (mainly) means "forced shutdown". */
        if (obd->obd_no_recov)
                ptlrpc_invalidate_import(imp, 0);
        else
                rc = ptlrpc_disconnect_import(imp);

        EXIT;
 out_no_disconnect:
        err = class_disconnect(exp, 0);
        if (!rc && err)
                rc = err;
 out_sem:
        up(&cli->cl_sem);
        RETURN(rc);
}

/* --------------------------------------------------------------------------
 * from old lib/target.c
 * -------------------------------------------------------------------------- */

int target_handle_reconnect(struct lustre_handle *conn, struct obd_export *exp,
                            struct obd_uuid *cluuid)
{
        if (exp->exp_connection) {
                struct lustre_handle *hdl;
                hdl = &exp->exp_imp_reverse->imp_remote_handle;
                /* Might be a re-connect after a partition. */
                if (!memcmp(&conn->cookie, &hdl->cookie, sizeof conn->cookie)) {
                        CERROR("%s reconnecting\n", cluuid->uuid);
                        conn->cookie = exp->exp_handle.h_cookie;
                        RETURN(EALREADY);
                } else {
                        CERROR("%s reconnecting from %s, "
                               "handle mismatch (ours "LPX64", theirs "
                               LPX64")\n", cluuid->uuid,
                               exp->exp_connection->c_remote_uuid.uuid,
                               hdl->cookie, conn->cookie);
                        memset(conn, 0, sizeof *conn);
                        RETURN(-EALREADY);
                }
        }

        conn->cookie = exp->exp_handle.h_cookie;
        CDEBUG(D_INFO, "existing export for UUID '%s' at %p\n",
               cluuid->uuid, exp);
        CDEBUG(D_IOCTL,"connect: cookie "LPX64"\n", conn->cookie);
        RETURN(0);
}

int target_handle_connect(struct ptlrpc_request *req, svc_handler_t handler)
{
        struct obd_device *target;
        struct obd_export *export = NULL;
        struct obd_import *revimp;
        struct lustre_handle conn;
        struct obd_uuid tgtuuid;
        struct obd_uuid cluuid;
        struct obd_uuid remote_uuid;
        struct list_head *p;
        char *str, *tmp;
        int rc = 0, abort_recovery;
        unsigned long flags;
        ENTRY;

        OBD_RACE(OBD_FAIL_TGT_CONN_RACE); 

        LASSERT_REQSWAB (req, 0);
        str = lustre_msg_string(req->rq_reqmsg, 0, sizeof(tgtuuid) - 1);
        if (str == NULL) {
                CERROR("bad target UUID for connect\n");
                GOTO(out, rc = -EINVAL);
        }

        obd_str2uuid (&tgtuuid, str);
        target = class_uuid2obd(&tgtuuid);
        if (!target) {
                target = class_name2obd(str);
        }
        
        if (!target || target->obd_stopping || !target->obd_set_up) {
                CERROR("UUID '%s' is not available for connect\n", str);
                GOTO(out, rc = -ENODEV);
        }

        LASSERT_REQSWAB (req, 1);
        str = lustre_msg_string(req->rq_reqmsg, 1, sizeof(cluuid) - 1);
        if (str == NULL) {
                CERROR("bad client UUID for connect\n");
                GOTO(out, rc = -EINVAL);
        }

        obd_str2uuid (&cluuid, str);

        /* XXX extract a nettype and format accordingly */
        snprintf(remote_uuid.uuid, sizeof remote_uuid,
                 "NET_"LPX64"_UUID", req->rq_peer.peer_nid);

        spin_lock_bh(&target->obd_processing_task_lock);
        abort_recovery = target->obd_abort_recovery;
        spin_unlock_bh(&target->obd_processing_task_lock);
        if (abort_recovery)
                target_abort_recovery(target);

        tmp = lustre_msg_buf(req->rq_reqmsg, 2, sizeof conn);
        if (tmp == NULL)
                GOTO(out, rc = -EPROTO);

        memcpy(&conn, tmp, sizeof conn);

        rc = lustre_pack_reply(req, 0, NULL, NULL);
        if (rc)
                GOTO(out, rc);

        /* lctl gets a backstage, all-access pass. */
        if (obd_uuid_equals(&cluuid, &target->obd_uuid))
                goto dont_check_exports;

        spin_lock(&target->obd_dev_lock);
        list_for_each(p, &target->obd_exports) {
                export = list_entry(p, struct obd_export, exp_obd_chain);
                if (obd_uuid_equals(&cluuid, &export->exp_client_uuid)) {
                        spin_unlock(&target->obd_dev_lock);
                        LASSERT(export->exp_obd == target);

                        rc = target_handle_reconnect(&conn, export, &cluuid);
                        break;
                }
                export = NULL;
        }
        /* If we found an export, we already unlocked. */
        if (!export) {
                spin_unlock(&target->obd_dev_lock);
        } else if (req->rq_reqmsg->conn_cnt == 1) {
                CERROR("%s reconnected with 1 conn_cnt; cookies not random?\n",
                       cluuid.uuid);
                GOTO(out, rc = -EALREADY);
        }

        /* Tell the client if we're in recovery. */
        /* If this is the first client, start the recovery timer */
        if (target->obd_recovering) {
                lustre_msg_add_op_flags(req->rq_repmsg, MSG_CONNECT_RECOVERING);
                target_start_recovery_timer(target, handler);
        }

        /* Tell the client if we support replayable requests */
        if (target->obd_replayable)
                lustre_msg_add_op_flags(req->rq_repmsg, MSG_CONNECT_REPLAYABLE);

        if (export == NULL) {
                if (target->obd_recovering) {
                        CERROR("denying connection for new client %s: "
                               "%d clients in recovery for %lds\n", cluuid.uuid,
                               target->obd_recoverable_clients,
                               (target->obd_recovery_timer.expires-jiffies)/HZ);
                        rc = -EBUSY;
                } else {
 dont_check_exports:
                        rc = obd_connect(&conn, target, &cluuid);
                }
        }


        /* If all else goes well, this is our RPC return code. */
        req->rq_status = 0;

        if (rc && rc != EALREADY)
                GOTO(out, rc);

        req->rq_repmsg->handle = conn;

        /* If the client and the server are the same node, we will already
         * have an export that really points to the client's DLM export,
         * because we have a shared handles table.
         *
         * XXX this will go away when shaver stops sending the "connect" handle
         * in the real "remote handle" field of the request --phik 24 Apr 2003
         */
        if (req->rq_export != NULL)
                class_export_put(req->rq_export);

        /* ownership of this export ref transfers to the request */
        export = req->rq_export = class_conn2export(&conn);
        LASSERT(export != NULL);

        spin_lock_irqsave(&export->exp_lock, flags);
        if (export->exp_conn_cnt >= req->rq_reqmsg->conn_cnt) {
                CERROR("%s: already connected at a higher conn_cnt: %d > %d\n",
                       cluuid.uuid, export->exp_conn_cnt, 
                       req->rq_reqmsg->conn_cnt);
                spin_unlock_irqrestore(&export->exp_lock, flags);
                GOTO(out, rc = -EALREADY);
        }
        export->exp_conn_cnt = req->rq_reqmsg->conn_cnt;
        spin_unlock_irqrestore(&export->exp_lock, flags);

        /* request from liblustre? */
        if (lustre_msg_get_op_flags(req->rq_reqmsg) & MSG_CONNECT_LIBCLIENT)
                export->exp_libclient = 1;

        if (export->exp_connection != NULL)
                ptlrpc_put_connection(export->exp_connection);
        export->exp_connection = ptlrpc_get_connection(&req->rq_peer,
                                                       &remote_uuid);

        if (rc == EALREADY) {
                /* We indicate the reconnection in a flag, not an error code. */
                lustre_msg_add_op_flags(req->rq_repmsg, MSG_CONNECT_RECONNECT);
                GOTO(out, rc = 0);
        }

        if (target->obd_recovering)
                target->obd_connected_clients++;

        memcpy(&conn, lustre_msg_buf(req->rq_reqmsg, 2, sizeof conn),
               sizeof conn);

        if (export->exp_imp_reverse != NULL)
                class_destroy_import(export->exp_imp_reverse);
        revimp = export->exp_imp_reverse = class_new_import();
        revimp->imp_connection = ptlrpc_connection_addref(export->exp_connection);
        revimp->imp_client = &export->exp_obd->obd_ldlm_client;
        revimp->imp_remote_handle = conn;
        revimp->imp_obd = target;
        revimp->imp_dlm_fake = 1;
        revimp->imp_state = LUSTRE_IMP_FULL;
        class_import_put(revimp);
out:
        if (rc)
                req->rq_status = rc;
        RETURN(rc);
}

int target_handle_disconnect(struct ptlrpc_request *req)
{
        struct obd_export *exp;
        int rc;
        ENTRY;

        rc = lustre_pack_reply(req, 0, NULL, NULL);
        if (rc)
                RETURN(rc);

        /* keep the rq_export around so we can send the reply */
        exp = class_export_get(req->rq_export);
        req->rq_status = obd_disconnect(exp, 0);
        RETURN(0);
}

void target_destroy_export(struct obd_export *exp)
{
        /* exports created from last_rcvd data, and "fake"
           exports created by lctl don't have an import */
        if (exp->exp_imp_reverse != NULL)
                class_destroy_import(exp->exp_imp_reverse);

        /* We cancel locks at disconnect time, but this will catch any locks
         * granted in a race with recovery-induced disconnect. */
        if (exp->exp_obd->obd_namespace != NULL)
                ldlm_cancel_locks_for_export(exp);
}

/*
 * Recovery functions
 */


static void target_release_saved_req(struct ptlrpc_request *req) 
{
        class_export_put(req->rq_export);
        OBD_FREE(req->rq_reqmsg, req->rq_reqlen);
        OBD_FREE(req, sizeof *req);
}

static void target_finish_recovery(struct obd_device *obd)
{
        struct list_head *tmp, *n;
        int rc;

        CWARN("%s: sending delayed replies to recovered clients\n",
              obd->obd_name);

        ldlm_reprocess_all_ns(obd->obd_namespace);

        /* when recovery finished, cleanup orphans on mds and ost */
        if (OBT(obd) && OBP(obd, postrecov)) {
                rc = OBP(obd, postrecov)(obd);
                if (rc >= 0)
                        CWARN("%s: all clients recovered, %d MDS "
                              "orphans deleted\n", obd->obd_name, rc);
                else
                        CERROR("postrecov failed %d\n", rc);
        }

        list_for_each_safe(tmp, n, &obd->obd_delayed_reply_queue) {
                struct ptlrpc_request *req;
                req = list_entry(tmp, struct ptlrpc_request, rq_list);
                list_del(&req->rq_list);
                DEBUG_REQ(D_ERROR, req, "delayed:");
                ptlrpc_reply(req);
                target_release_saved_req(req);
        }
        obd->obd_recovery_end = LTIME_S(CURRENT_TIME);
        return;
}

static void abort_recovery_queue(struct obd_device *obd)
{
        struct ptlrpc_request *req;
        struct list_head *tmp, *n;
        int rc;

        list_for_each_safe(tmp, n, &obd->obd_recovery_queue) {
                req = list_entry(tmp, struct ptlrpc_request, rq_list);
                list_del(&req->rq_list);
                DEBUG_REQ(D_ERROR, req, "aborted:");
                req->rq_status = -ENOTCONN;
                req->rq_type = PTL_RPC_MSG_ERR;
                rc = lustre_pack_reply(req, 0, NULL, NULL);
                if (rc == 0) {
                        ptlrpc_reply(req);
                } else {
                        DEBUG_REQ(D_ERROR, req,
                                  "packing failed for abort-reply; skipping");
                }
                target_release_saved_req(req);
        }
}

/* Called from a cleanup function if the device is being cleaned up 
   forcefully.  The exports should all have been disconnected already, 
   the only thing left to do is 
     - clear the recovery flags
     - cancel the timer
     - free queued requests and replies, but don't send replies
   Because the obd_stopping flag is set, no new requests should be received.
     
*/
void target_cleanup_recovery(struct obd_device *obd)
{
        struct list_head *tmp, *n;
        struct ptlrpc_request *req;

        spin_lock_bh(&obd->obd_processing_task_lock);
        if (!obd->obd_recovering) {
                spin_unlock_bh(&obd->obd_processing_task_lock);
                EXIT;
                return;
        }
        obd->obd_recovering = obd->obd_abort_recovery = 0;
        target_cancel_recovery_timer(obd);
        spin_unlock_bh(&obd->obd_processing_task_lock);


        list_for_each_safe(tmp, n, &obd->obd_delayed_reply_queue) {
                req = list_entry(tmp, struct ptlrpc_request, rq_list);
                list_del(&req->rq_list);
                LASSERT (req->rq_reply_state);
                lustre_free_reply_state(req->rq_reply_state);
                target_release_saved_req(req);
        }

        list_for_each_safe(tmp, n, &obd->obd_recovery_queue) {
                req = list_entry(tmp, struct ptlrpc_request, rq_list);
                list_del(&req->rq_list);
                LASSERT (req->rq_reply_state == 0);
                target_release_saved_req(req);
        }
}

void target_abort_recovery(void *data)
{
        struct obd_device *obd = data;

        spin_lock_bh(&obd->obd_processing_task_lock);
        if (!obd->obd_recovering) {
                spin_unlock_bh(&obd->obd_processing_task_lock);
                EXIT;
                return;
        }
        obd->obd_recovering = obd->obd_abort_recovery = 0;
        target_cancel_recovery_timer(obd);
        spin_unlock_bh(&obd->obd_processing_task_lock);

        CERROR("%s: recovery period over; disconnecting unfinished clients.\n",
               obd->obd_name);
        class_disconnect_stale_exports(obd, 0);
        abort_recovery_queue(obd);

        target_finish_recovery(obd);

        ptlrpc_run_recovery_over_upcall(obd);
}

static void target_recovery_expired(unsigned long castmeharder)
{
        struct obd_device *obd = (struct obd_device *)castmeharder;
        CERROR("recovery timed out, aborting\n");
        spin_lock_bh(&obd->obd_processing_task_lock);
        if (obd->obd_recovering)
                obd->obd_abort_recovery = 1;
        wake_up(&obd->obd_next_transno_waitq);
        spin_unlock_bh(&obd->obd_processing_task_lock);
}


/* obd_processing_task_lock should be held */
void target_cancel_recovery_timer(struct obd_device *obd)
{
        CDEBUG(D_HA, "%s: cancel recovery timer\n", obd->obd_name);
        del_timer(&obd->obd_recovery_timer);
}

static void reset_recovery_timer(struct obd_device *obd)
{
        spin_lock_bh(&obd->obd_processing_task_lock);
        if (!obd->obd_recovering) {
                spin_unlock_bh(&obd->obd_processing_task_lock);
                return;
        }                
        CDEBUG(D_HA, "timer will expire in %u seconds\n",
               OBD_RECOVERY_TIMEOUT / HZ);
        mod_timer(&obd->obd_recovery_timer, jiffies + OBD_RECOVERY_TIMEOUT);
        spin_unlock_bh(&obd->obd_processing_task_lock);
}


/* Only start it the first time called */
void target_start_recovery_timer(struct obd_device *obd, svc_handler_t handler)
{
        spin_lock_bh(&obd->obd_processing_task_lock);
        if (obd->obd_recovery_handler) {
                spin_unlock_bh(&obd->obd_processing_task_lock);
                return;
        }
        CWARN("%s: starting recovery timer (%us)\n", obd->obd_name,
               OBD_RECOVERY_TIMEOUT / HZ);
        obd->obd_recovery_handler = handler;
        obd->obd_recovery_timer.function = target_recovery_expired;
        obd->obd_recovery_timer.data = (unsigned long)obd;
        spin_unlock_bh(&obd->obd_processing_task_lock);

        reset_recovery_timer(obd);
}

static int check_for_next_transno(struct obd_device *obd)
{
        struct ptlrpc_request *req;
        int wake_up = 0, connected, completed, queue_len, max;
        __u64 next_transno, req_transno;

        spin_lock_bh(&obd->obd_processing_task_lock);
        req = list_entry(obd->obd_recovery_queue.next,
                         struct ptlrpc_request, rq_list);
        max = obd->obd_max_recoverable_clients;
        req_transno = req->rq_reqmsg->transno;
        connected = obd->obd_connected_clients;
        completed = max - obd->obd_recoverable_clients;
        queue_len = obd->obd_requests_queued_for_recovery;
        next_transno = obd->obd_next_recovery_transno;

        CDEBUG(D_HA,"max: %d, connected: %d, completed: %d, queue_len: %d, "
               "req_transno: "LPU64", next_transno: "LPU64"\n",
               max, connected, completed, queue_len, req_transno, next_transno);
        if (obd->obd_abort_recovery) {
                CDEBUG(D_HA, "waking for aborted recovery\n");
                wake_up = 1;
        } else if (!obd->obd_recovering) {
                CDEBUG(D_HA, "waking for completed recovery (?)\n");
                wake_up = 1;
        } else if (req_transno == next_transno) {
                CDEBUG(D_HA, "waking for next ("LPD64")\n", next_transno);
                wake_up = 1;
        } else if (queue_len + completed == max) {
                CDEBUG(D_ERROR,
                       "waking for skipped transno (skip: "LPD64
                       ", ql: %d, comp: %d, conn: %d, next: "LPD64")\n",
                       next_transno, queue_len, completed, max, req_transno);
                obd->obd_next_recovery_transno = req_transno;
                wake_up = 1;
        }
        spin_unlock_bh(&obd->obd_processing_task_lock);
        LASSERT(req->rq_reqmsg->transno >= next_transno);
        return wake_up;
}

static void process_recovery_queue(struct obd_device *obd)
{
        struct ptlrpc_request *req;
        int abort_recovery = 0;
        struct l_wait_info lwi = { 0 };
        ENTRY;

        for (;;) {
                spin_lock_bh(&obd->obd_processing_task_lock);
                LASSERT(obd->obd_processing_task == current->pid);
                req = list_entry(obd->obd_recovery_queue.next,
                                 struct ptlrpc_request, rq_list);

                if (req->rq_reqmsg->transno != obd->obd_next_recovery_transno) {
                        spin_unlock_bh(&obd->obd_processing_task_lock);
                        CDEBUG(D_HA, "Waiting for transno "LPD64" (1st is "
                               LPD64")\n",
                               obd->obd_next_recovery_transno,
                               req->rq_reqmsg->transno);
                        l_wait_event(obd->obd_next_transno_waitq,
                                     check_for_next_transno(obd), &lwi);
                        spin_lock_bh(&obd->obd_processing_task_lock);
                        abort_recovery = obd->obd_abort_recovery;
                        spin_unlock_bh(&obd->obd_processing_task_lock);
                        if (abort_recovery) {
                                target_abort_recovery(obd);
                                return;
                        }
                        continue;
                }
                list_del_init(&req->rq_list);
                obd->obd_requests_queued_for_recovery--;
                spin_unlock_bh(&obd->obd_processing_task_lock);

                DEBUG_REQ(D_HA, req, "processing: ");
                (void)obd->obd_recovery_handler(req);
                obd->obd_replayed_requests++;
                reset_recovery_timer(obd);
                /* bug 1580: decide how to properly sync() in recovery */
                //mds_fsync_super(mds->mds_sb);
                class_export_put(req->rq_export);
                OBD_FREE(req->rq_reqmsg, req->rq_reqlen);
                OBD_FREE(req, sizeof *req);
                spin_lock_bh(&obd->obd_processing_task_lock);
                obd->obd_next_recovery_transno++;
                if (list_empty(&obd->obd_recovery_queue)) {
                        obd->obd_processing_task = 0;
                        spin_unlock_bh(&obd->obd_processing_task_lock);
                        break;
                }
                spin_unlock_bh(&obd->obd_processing_task_lock);
        }
        EXIT;
}

int target_queue_recovery_request(struct ptlrpc_request *req,
                                  struct obd_device *obd)
{
        struct list_head *tmp;
        int inserted = 0;
        __u64 transno = req->rq_reqmsg->transno;
        struct ptlrpc_request *saved_req;
        struct lustre_msg *reqmsg;

        /* CAVEAT EMPTOR: The incoming request message has been swabbed
         * (i.e. buflens etc are in my own byte order), but type-dependent
         * buffers (eg mds_body, ost_body etc) have NOT been swabbed. */

        if (!transno) {
                INIT_LIST_HEAD(&req->rq_list);
                DEBUG_REQ(D_HA, req, "not queueing");
                return 1;
        }

        /* XXX If I were a real man, these LBUGs would be sane cleanups. */
        /* XXX just like the request-dup code in queue_final_reply */
        OBD_ALLOC(saved_req, sizeof *saved_req);
        if (!saved_req)
                LBUG();
        OBD_ALLOC(reqmsg, req->rq_reqlen);
        if (!reqmsg)
                LBUG();

        spin_lock_bh(&obd->obd_processing_task_lock);

        /* If we're processing the queue, we want don't want to queue this
         * message.
         *
         * Also, if this request has a transno less than the one we're waiting
         * for, we should process it now.  It could (and currently always will)
         * be an open request for a descriptor that was opened some time ago.
         *
         * Also, a resent, replayed request that has already been
         * handled will pass through here and be processed immediately.
         */
        if (obd->obd_processing_task == current->pid ||
            transno < obd->obd_next_recovery_transno) {
                /* Processing the queue right now, don't re-add. */
                LASSERT(list_empty(&req->rq_list));
                spin_unlock_bh(&obd->obd_processing_task_lock);
                OBD_FREE(reqmsg, req->rq_reqlen);
                OBD_FREE(saved_req, sizeof *saved_req);
                return 1;
        }

        /* A resent, replayed request that is still on the queue; just drop it.
           The queued request will handle this. */
        if ((lustre_msg_get_flags(req->rq_reqmsg) & (MSG_RESENT | MSG_REPLAY)) ==
            (MSG_RESENT | MSG_REPLAY)) {
                DEBUG_REQ(D_ERROR, req, "dropping resent queued req");
                spin_unlock_bh(&obd->obd_processing_task_lock);
                OBD_FREE(reqmsg, req->rq_reqlen);
                OBD_FREE(saved_req, sizeof *saved_req);
                return 0;
        }

        memcpy(saved_req, req, sizeof *req);
        memcpy(reqmsg, req->rq_reqmsg, req->rq_reqlen);
        req = saved_req;
        req->rq_reqmsg = reqmsg;
        class_export_get(req->rq_export);
        INIT_LIST_HEAD(&req->rq_list);

        /* XXX O(n^2) */
        list_for_each(tmp, &obd->obd_recovery_queue) {
                struct ptlrpc_request *reqiter =
                        list_entry(tmp, struct ptlrpc_request, rq_list);

                if (reqiter->rq_reqmsg->transno > transno) {
                        list_add_tail(&req->rq_list, &reqiter->rq_list);
                        inserted = 1;
                        break;
                }
        }

        if (!inserted) {
                list_add_tail(&req->rq_list, &obd->obd_recovery_queue);
        }

        obd->obd_requests_queued_for_recovery++;

        if (obd->obd_processing_task != 0) {
                /* Someone else is processing this queue, we'll leave it to
                 * them.
                 */
                wake_up(&obd->obd_next_transno_waitq);
                spin_unlock_bh(&obd->obd_processing_task_lock);
                return 0;
        }

        /* Nobody is processing, and we know there's (at least) one to process
         * now, so we'll do the honours.
         */
        obd->obd_processing_task = current->pid;
        spin_unlock_bh(&obd->obd_processing_task_lock);

        process_recovery_queue(obd);
        return 0;
}

struct obd_device * target_req2obd(struct ptlrpc_request *req)
{
        return req->rq_export->exp_obd;
}

int target_queue_final_reply(struct ptlrpc_request *req, int rc)
{
        struct obd_device *obd = target_req2obd(req);
        struct ptlrpc_request *saved_req;
        struct lustre_msg *reqmsg;
        int recovery_done = 0;

        LASSERT ((rc == 0) == (req->rq_reply_state != NULL));

        if (rc) {
                /* Just like ptlrpc_error, but without the sending. */
                rc = lustre_pack_reply(req, 0, NULL, NULL);
                LASSERT(rc == 0); /* XXX handle this */
                req->rq_type = PTL_RPC_MSG_ERR;
        }

        LASSERT (!req->rq_reply_state->rs_difficult);
        LASSERT(list_empty(&req->rq_list));
        /* XXX a bit like the request-dup code in queue_recovery_request */
        OBD_ALLOC(saved_req, sizeof *saved_req);
        if (!saved_req)
                LBUG();
        OBD_ALLOC(reqmsg, req->rq_reqlen);
        if (!reqmsg)
                LBUG();
        memcpy(saved_req, req, sizeof *saved_req);
        memcpy(reqmsg, req->rq_reqmsg, req->rq_reqlen);
        /* the copied req takes over the reply state */
        req->rq_reply_state = NULL;
        req = saved_req;
        req->rq_reqmsg = reqmsg;
        class_export_get(req->rq_export);
        list_add(&req->rq_list, &obd->obd_delayed_reply_queue);

        spin_lock_bh(&obd->obd_processing_task_lock);
        /* only count the first "replay over" request from each
           export */
        if (req->rq_export->exp_replay_needed) {
                --obd->obd_recoverable_clients;
                req->rq_export->exp_replay_needed = 0;
        }
        recovery_done = (obd->obd_recoverable_clients == 0);
        spin_unlock_bh(&obd->obd_processing_task_lock);

        if (recovery_done) {
                spin_lock_bh(&obd->obd_processing_task_lock);
                obd->obd_recovering = obd->obd_abort_recovery = 0;
                target_cancel_recovery_timer(obd);
                spin_unlock_bh(&obd->obd_processing_task_lock);

                target_finish_recovery(obd);
                ptlrpc_run_recovery_over_upcall(obd);
        } else {
                CWARN("%s: %d recoverable clients remain\n",
                       obd->obd_name, obd->obd_recoverable_clients);
                wake_up(&obd->obd_next_transno_waitq);
        }

        return 1;
}

int
target_send_reply_msg (struct ptlrpc_request *req, int rc, int fail_id)
{
        if (OBD_FAIL_CHECK(fail_id | OBD_FAIL_ONCE)) {
                obd_fail_loc |= OBD_FAIL_ONCE | OBD_FAILED;
                DEBUG_REQ(D_ERROR, req, "dropping reply");
                /* NB this does _not_ send with ACK disabled, to simulate
                 * sending OK, but timing out for the ACK */
                if (req->rq_reply_state != NULL) {
                        if (!req->rq_reply_state->rs_difficult) {
                                lustre_free_reply_state (req->rq_reply_state);
                                req->rq_reply_state = NULL;
                        } else {
                                struct ptlrpc_service *svc =
                                        req->rq_rqbd->rqbd_srv_ni->sni_service;
                                atomic_inc(&svc->srv_outstanding_replies);
                        }
                }
                return (-ECOMM);
        }

        if (rc) {
                DEBUG_REQ(D_ERROR, req, "processing error (%d)", rc);
                req->rq_status = rc;
                return (ptlrpc_error(req));
        } else {
                DEBUG_REQ(D_NET, req, "sending reply");
        }
        
        return (ptlrpc_send_reply(req, 1));
}

void 
target_send_reply(struct ptlrpc_request *req, int rc, int fail_id)
{
        int                        netrc;
        unsigned long              flags;
        struct ptlrpc_reply_state *rs;
        struct obd_device         *obd;
        struct obd_export         *exp;
        struct ptlrpc_srv_ni      *sni;
        struct ptlrpc_service     *svc;

        sni = req->rq_rqbd->rqbd_srv_ni;
        svc = sni->sni_service;
        
        rs = req->rq_reply_state;
        if (rs == NULL || !rs->rs_difficult) {
                /* The easy case; no notifiers and reply_out_callback()
                 * cleans up (i.e. we can't look inside rs after a
                 * successful send) */
                netrc = target_send_reply_msg (req, rc, fail_id);

                LASSERT (netrc == 0 || req->rq_reply_state == NULL);
                return;
        }

        /* must be an export if locks saved */
        LASSERT (req->rq_export != NULL);
        /* req/reply consistent */
        LASSERT (rs->rs_srv_ni == sni);

        /* "fresh" reply */
        LASSERT (!rs->rs_scheduled);
        LASSERT (!rs->rs_scheduled_ever);
        LASSERT (!rs->rs_handled);
        LASSERT (!rs->rs_on_net);
        LASSERT (rs->rs_export == NULL);
        LASSERT (list_empty(&rs->rs_obd_list));
        LASSERT (list_empty(&rs->rs_exp_list));

        exp = class_export_get (req->rq_export);
        obd = exp->exp_obd;

        /* disable reply scheduling onto srv_reply_queue while I'm setting up */
        rs->rs_scheduled = 1;
        rs->rs_on_net    = 1;
        rs->rs_xid       = req->rq_xid;
        rs->rs_transno   = req->rq_transno;
        rs->rs_export    = exp;
        
        spin_lock_irqsave (&obd->obd_uncommitted_replies_lock, flags);

        if (rs->rs_transno > obd->obd_last_committed) {
                /* not committed already */ 
                list_add_tail (&rs->rs_obd_list, 
                               &obd->obd_uncommitted_replies);
        }

        spin_unlock (&obd->obd_uncommitted_replies_lock);
        spin_lock (&exp->exp_lock);

        list_add_tail (&rs->rs_exp_list, &exp->exp_outstanding_replies);

        spin_unlock_irqrestore (&exp->exp_lock, flags);

        netrc = target_send_reply_msg (req, rc, fail_id);

        spin_lock_irqsave (&svc->srv_lock, flags);

        svc->srv_n_difficult_replies++;

        if (netrc != 0) /* error sending: reply is off the net */
                rs->rs_on_net = 0;

        if (!rs->rs_on_net ||                   /* some notifier */
            list_empty(&rs->rs_exp_list) ||     /* completed already */
            list_empty(&rs->rs_obd_list)) {
                list_add_tail (&rs->rs_list, &svc->srv_reply_queue);
                wake_up (&svc->srv_waitq);
        } else {
                list_add (&rs->rs_list, &sni->sni_active_replies);
                rs->rs_scheduled = 0;           /* allow notifier to schedule */
        }

        spin_unlock_irqrestore (&svc->srv_lock, flags);
}

int target_handle_ping(struct ptlrpc_request *req)
{
        return lustre_pack_reply(req, 0, NULL, NULL);
}
