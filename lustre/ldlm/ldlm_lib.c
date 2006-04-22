/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (c) 2003 Cluster File Systems, Inc.
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
 */

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_LDLM

#ifdef __KERNEL__
# include <libcfs/libcfs.h>
#else
# include <liblustre.h>
#endif
#include <obd.h>
#include <lustre_mds.h>
#include <lustre_dlm.h>
#include <lustre_net.h>
#include <lustre_ver.h>

/* @priority: if non-zero, move the selected to the list head
 * @create: if zero, only search in existed connections
 */
static int import_set_conn(struct obd_import *imp, struct obd_uuid *uuid,
                           int priority, int create)
{
        struct ptlrpc_connection *ptlrpc_conn;
        struct obd_import_conn *imp_conn = NULL, *item;
        int rc = 0;
        ENTRY;

        if (!create && !priority) {
                CDEBUG(D_HA, "Nothing to do\n");
                RETURN(-EINVAL);
        }

        ptlrpc_conn = ptlrpc_uuid_to_connection(uuid);
        if (!ptlrpc_conn) {
                CDEBUG(D_HA, "can't find connection %s\n", uuid->uuid);
                RETURN (-ENOENT);
        }

        if (create) {
                OBD_ALLOC(imp_conn, sizeof(*imp_conn));
                if (!imp_conn) {
                        GOTO(out_put, rc = -ENOMEM);
                }
        }

        spin_lock(&imp->imp_lock);
        list_for_each_entry(item, &imp->imp_conn_list, oic_item) {
                if (obd_uuid_equals(uuid, &item->oic_uuid)) {
                        if (priority) {
                                list_del(&item->oic_item);
                                list_add(&item->oic_item, &imp->imp_conn_list);
                        }
                        CDEBUG(D_HA, "imp %p@%s: found existing conn %s%s\n",
                               imp, imp->imp_obd->obd_name, uuid->uuid,
                               (priority ? ", moved to head" : ""));
                        spin_unlock(&imp->imp_lock);
                        GOTO(out_free, rc = 0);
                }
        }
        /* not found */
        if (create) {
                imp_conn->oic_conn = ptlrpc_conn;
                imp_conn->oic_uuid = *uuid;
                if (priority)
                        list_add(&imp_conn->oic_item, &imp->imp_conn_list);
                else
                        list_add_tail(&imp_conn->oic_item, &imp->imp_conn_list);
                CDEBUG(D_HA, "imp %p@%s: add connection %s at %s\n",
                       imp, imp->imp_obd->obd_name, uuid->uuid,
                       (priority ? "head" : "tail"));
        } else {
                spin_unlock(&imp->imp_lock);
                GOTO(out_free, rc = -ENOENT);

        }

        spin_unlock(&imp->imp_lock);
        RETURN(0);
out_free:
        if (imp_conn)
                OBD_FREE(imp_conn, sizeof(*imp_conn));
out_put:
        ptlrpc_put_connection(ptlrpc_conn);
        RETURN(rc);
}

int import_set_conn_priority(struct obd_import *imp, struct obd_uuid *uuid)
{
        return import_set_conn(imp, uuid, 1, 0);
}

int client_import_add_conn(struct obd_import *imp, struct obd_uuid *uuid,
                           int priority)
{
        return import_set_conn(imp, uuid, priority, 1);
}

int client_import_del_conn(struct obd_import *imp, struct obd_uuid *uuid)
{
        struct obd_import_conn *imp_conn;
        struct obd_import_conn *cur_conn;
        struct obd_export *dlmexp;
        int rc = -ENOENT;
        ENTRY;

        spin_lock(&imp->imp_lock);
        if (list_empty(&imp->imp_conn_list)) {
                LASSERT(!imp->imp_connection);
                GOTO(out, rc);
        }

        list_for_each_entry(imp_conn, &imp->imp_conn_list, oic_item) {
                if (!obd_uuid_equals(uuid, &imp_conn->oic_uuid))
                        continue;
                LASSERT(imp_conn->oic_conn);

                cur_conn = list_entry(imp->imp_conn_list.next,
                                      struct obd_import_conn,
                                      oic_item);

                /* is current conn? */
                if (imp_conn == cur_conn) {
                        LASSERT(imp_conn->oic_conn == imp->imp_connection);

                        if (imp->imp_state != LUSTRE_IMP_CLOSED &&
                            imp->imp_state != LUSTRE_IMP_DISCON) {
                                CERROR("can't remove current connection\n");
                                GOTO(out, rc = -EBUSY);
                        }

                        ptlrpc_put_connection(imp->imp_connection);
                        imp->imp_connection = NULL;

                        dlmexp = class_conn2export(&imp->imp_dlm_handle);
                        if (dlmexp && dlmexp->exp_connection) {
                                LASSERT(dlmexp->exp_connection ==
                                        imp_conn->oic_conn);
                                ptlrpc_put_connection(dlmexp->exp_connection);
                                dlmexp->exp_connection = NULL;
                        }
                }

                list_del(&imp_conn->oic_item);
                ptlrpc_put_connection(imp_conn->oic_conn);
                OBD_FREE(imp_conn, sizeof(*imp_conn));
                CDEBUG(D_HA, "imp %p@%s: remove connection %s\n",
                       imp, imp->imp_obd->obd_name, uuid->uuid);
                rc = 0;
                break;
        }
out:
        spin_unlock(&imp->imp_lock);
        if (rc == -ENOENT)
                CERROR("connection %s not found\n", uuid->uuid);
        RETURN(rc);
}

/* configure an RPC client OBD device
 *
 * lcfg parameters:
 * 1 - client UUID
 * 2 - server UUID
 * 3 - inactive-on-startup
 */
int client_obd_setup(struct obd_device *obddev, obd_count len, void *buf)
{
        struct lustre_cfg* lcfg = buf;
        struct client_obd *cli = &obddev->u.cli;
        struct obd_import *imp;
        struct obd_uuid server_uuid;
        int rq_portal, rp_portal, connect_op;
        char *name = obddev->obd_type->typ_name;
        int rc;
        ENTRY;

        /* In a more perfect world, we would hang a ptlrpc_client off of
         * obd_type and just use the values from there. */
        if (!strcmp(name, LUSTRE_OSC_NAME)) {
                rq_portal = OST_IO_PORTAL;
                rp_portal = OSC_REPLY_PORTAL;
                connect_op = OST_CONNECT;
        } else if (!strcmp(name, LUSTRE_MDC_NAME)) {
                rq_portal = MDS_REQUEST_PORTAL;
                rp_portal = MDC_REPLY_PORTAL;
                connect_op = MDS_CONNECT;
        } else {
                CERROR("unknown client OBD type \"%s\", can't setup\n",
                       name);
                RETURN(-EINVAL);
        }

        if (LUSTRE_CFG_BUFLEN(lcfg, 1) < 1) {
                CERROR("requires a TARGET UUID\n");
                RETURN(-EINVAL);
        }

        if (LUSTRE_CFG_BUFLEN(lcfg, 1) > 37) {
                CERROR("client UUID must be less than 38 characters\n");
                RETURN(-EINVAL);
        }

        if (LUSTRE_CFG_BUFLEN(lcfg, 2) < 1) {
                CERROR("setup requires a SERVER UUID\n");
                RETURN(-EINVAL);
        }

        if (LUSTRE_CFG_BUFLEN(lcfg, 2) > 37) {
                CERROR("target UUID must be less than 38 characters\n");
                RETURN(-EINVAL);
        }

        sema_init(&cli->cl_sem, 1);
        cli->cl_conn_count = 0;
        memcpy(server_uuid.uuid, lustre_cfg_buf(lcfg, 2),
               min_t(unsigned int, LUSTRE_CFG_BUFLEN(lcfg, 2),
                     sizeof(server_uuid)));

        cli->cl_dirty = 0;
        cli->cl_avail_grant = 0;
        /* FIXME: should limit this for the sum of all cl_dirty_max */
        cli->cl_dirty_max = OSC_MAX_DIRTY_DEFAULT * 1024 * 1024;
        if (cli->cl_dirty_max >> PAGE_SHIFT > num_physpages / 8)
                cli->cl_dirty_max = num_physpages << (PAGE_SHIFT - 3);
        CFS_INIT_LIST_HEAD(&cli->cl_cache_waiters);
        CFS_INIT_LIST_HEAD(&cli->cl_loi_ready_list);
        CFS_INIT_LIST_HEAD(&cli->cl_loi_write_list);
        CFS_INIT_LIST_HEAD(&cli->cl_loi_read_list);
        client_obd_list_lock_init(&cli->cl_loi_list_lock);
        cli->cl_r_in_flight = 0;
        cli->cl_w_in_flight = 0;
        spin_lock_init(&cli->cl_read_rpc_hist.oh_lock);
        spin_lock_init(&cli->cl_write_rpc_hist.oh_lock);
        spin_lock_init(&cli->cl_read_page_hist.oh_lock);
        spin_lock_init(&cli->cl_write_page_hist.oh_lock);
        spin_lock_init(&cli->cl_read_offset_hist.oh_lock);
        spin_lock_init(&cli->cl_write_offset_hist.oh_lock);
        cli->cl_max_pages_per_rpc = PTLRPC_MAX_BRW_PAGES;
        if (num_physpages >> (20 - PAGE_SHIFT) <= 128 /* MB */) {
                cli->cl_max_rpcs_in_flight = 2;
        } else if (num_physpages >> (20 - PAGE_SHIFT) <= 256 /* MB */) {
                cli->cl_max_rpcs_in_flight = 3;
        } else if (num_physpages >> (20 - PAGE_SHIFT) <= 512 /* MB */) {
                cli->cl_max_rpcs_in_flight = 4;
        } else {
                cli->cl_max_rpcs_in_flight = OSC_MAX_RIF_DEFAULT;
        }

        rc = ldlm_get_ref();
        if (rc) {
                CERROR("ldlm_get_ref failed: %d\n", rc);
                GOTO(err, rc);
        }

        ptlrpc_init_client(rq_portal, rp_portal, name,
                           &obddev->obd_ldlm_client);

        imp = class_new_import(obddev);
        if (imp == NULL)
                GOTO(err_ldlm, rc = -ENOENT);
        imp->imp_client = &obddev->obd_ldlm_client;
        imp->imp_connect_op = connect_op;
        imp->imp_initial_recov = 1;
        CFS_INIT_LIST_HEAD(&imp->imp_pinger_chain);
        memcpy(cli->cl_target_uuid.uuid, lustre_cfg_buf(lcfg, 1),
               LUSTRE_CFG_BUFLEN(lcfg, 1));
        class_import_put(imp);

        rc = client_import_add_conn(imp, &server_uuid, 1);
        if (rc) {
                CERROR("can't add initial connection\n");
                GOTO(err_import, rc);
        }

        cli->cl_import = imp;
        /* cli->cl_max_mds_{easize,cookiesize} updated by mdc_init_ea_size() */
        cli->cl_max_mds_easize = sizeof(struct lov_mds_md);
        cli->cl_max_mds_cookiesize = sizeof(struct llog_cookie);
        cli->cl_sandev = to_kdev_t(0);

        if (LUSTRE_CFG_BUFLEN(lcfg, 3) > 0) {
                if (!strcmp(lustre_cfg_string(lcfg, 3), "inactive")) {
                        CDEBUG(D_HA, "marking %s %s->%s as inactive\n",
                               name, obddev->obd_name,
                               cli->cl_target_uuid.uuid);
                        imp->imp_invalid = 1;
                }
        }

        cli->cl_qchk_stat = CL_NOT_QUOTACHECKED;

        RETURN(rc);

err_import:
        class_destroy_import(imp);
err_ldlm:
        ldlm_put_ref(0);
err:
        RETURN(rc);

}

int client_obd_cleanup(struct obd_device *obddev)
{
        ENTRY;
        ldlm_put_ref(obddev->obd_force);

        RETURN(0);
}

/* ->o_connect() method for client side (OSC and MDC) */
int client_connect_import(struct lustre_handle *dlm_handle,
                          struct obd_device *obd, struct obd_uuid *cluuid,
                          struct obd_connect_data *data)
{
        struct client_obd *cli = &obd->u.cli;
        struct obd_import *imp = cli->cl_import;
        struct obd_export *exp;
        struct obd_connect_data *ocd;
        int rc;
        ENTRY;

        mutex_down(&cli->cl_sem);
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

        ocd = &imp->imp_connect_data;
        if (data) {
                *ocd = *data;
                imp->imp_connect_flags_orig = data->ocd_connect_flags;
        }

        rc = ptlrpc_connect_import(imp, NULL);
        if (rc != 0) {
                LASSERT (imp->imp_state == LUSTRE_IMP_DISCON);
                GOTO(out_ldlm, rc);
        }
        LASSERT(exp->exp_connection);

        if (data) {
                LASSERT((ocd->ocd_connect_flags & data->ocd_connect_flags) ==
                        ocd->ocd_connect_flags);
                data->ocd_connect_flags = ocd->ocd_connect_flags;
        }

        ptlrpc_pinger_add_import(imp);
        EXIT;

        if (rc) {
out_ldlm:
                ldlm_namespace_free(obd->obd_namespace, 0);
                obd->obd_namespace = NULL;
out_disco:
                cli->cl_conn_count--;
                class_disconnect(exp);
        } else {
                class_export_put(exp);
        }
out_sem:
        mutex_up(&cli->cl_sem);
        return rc;
}

int client_disconnect_export(struct obd_export *exp)
{
        struct obd_device *obd = class_exp2obd(exp);
        struct client_obd *cli;
        struct obd_import *imp;
        int rc = 0, err;
        ENTRY;

        if (!obd) {
                CERROR("invalid export for disconnect: exp %p cookie "LPX64"\n",
                       exp, exp ? exp->exp_handle.h_cookie : -1);
                RETURN(-EINVAL);
        }

        cli = &obd->u.cli;
        imp = cli->cl_import;

        mutex_down(&cli->cl_sem);
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
        if (!obd->obd_no_recov)
                rc = ptlrpc_disconnect_import(imp);

        ptlrpc_invalidate_import(imp);
        imp->imp_deactive = 1;
        ptlrpc_free_rq_pool(imp->imp_rq_pool);
        class_destroy_import(imp);
        cli->cl_import = NULL;

        EXIT;
 out_no_disconnect:
        err = class_disconnect(exp);
        if (!rc && err)
                rc = err;
 out_sem:
        mutex_up(&cli->cl_sem);
        RETURN(rc);
}

/* --------------------------------------------------------------------------
 * from old lib/target.c
 * -------------------------------------------------------------------------- */

int target_handle_reconnect(struct lustre_handle *conn, struct obd_export *exp,
                            struct obd_uuid *cluuid)
{
        ENTRY;
        if (exp->exp_connection && exp->exp_imp_reverse) {
                struct lustre_handle *hdl;
                hdl = &exp->exp_imp_reverse->imp_remote_handle;
                /* Might be a re-connect after a partition. */
                if (!memcmp(&conn->cookie, &hdl->cookie, sizeof conn->cookie)) {
                        CWARN("%s: %s reconnecting\n", exp->exp_obd->obd_name,
                              cluuid->uuid);
                        conn->cookie = exp->exp_handle.h_cookie;
                        /* target_handle_connect() treats EALREADY and
                         * -EALREADY differently.  EALREADY means we are
                         * doing a valid reconnect from the same client. */
                        RETURN(EALREADY);
                } else {
                        CERROR("%s reconnecting from %s, "
                               "handle mismatch (ours "LPX64", theirs "
                               LPX64")\n", cluuid->uuid,
                               exp->exp_connection->c_remote_uuid.uuid,
                               hdl->cookie, conn->cookie);
                        memset(conn, 0, sizeof *conn);
                        /* target_handle_connect() treats EALREADY and
                         * -EALREADY differently.  -EALREADY is an error
                         * (same UUID, different handle). */
                        RETURN(-EALREADY);
                }
        }

        conn->cookie = exp->exp_handle.h_cookie;
        CDEBUG(D_HA, "connect export for UUID '%s' at %p, cookie "LPX64"\n",
               cluuid->uuid, exp, conn->cookie);
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
        struct obd_connect_data *data;
        int size = sizeof(*data);
        ENTRY;

        OBD_RACE(OBD_FAIL_TGT_CONN_RACE);

        LASSERT_REQSWAB (req, 0);
        str = lustre_msg_string(req->rq_reqmsg, 0, sizeof(tgtuuid) - 1);
        if (str == NULL) {
                DEBUG_REQ(D_ERROR, req, "bad target UUID for connect");
                GOTO(out, rc = -EINVAL);
        }

        obd_str2uuid (&tgtuuid, str);
        target = class_uuid2obd(&tgtuuid);
        if (!target)
                target = class_name2obd(str);

        if (!target || target->obd_stopping || !target->obd_set_up) {
                DEBUG_REQ(D_ERROR, req, "UUID '%s' is not available "
                          " for connect (%s)", str,
                          !target ? "no target" :
                          (target->obd_stopping ? "stopping" : "not set up"));
                GOTO(out, rc = -ENODEV);
        }

        LASSERT_REQSWAB (req, 1);
        str = lustre_msg_string(req->rq_reqmsg, 1, sizeof(cluuid) - 1);
        if (str == NULL) {
                DEBUG_REQ(D_ERROR, req, "bad client UUID for connect");
                GOTO(out, rc = -EINVAL);
        }

        obd_str2uuid (&cluuid, str);

        /* XXX extract a nettype and format accordingly */
        switch (sizeof(lnet_nid_t)) {
                /* NB the casts only avoid compiler warnings */
        case 8:
                snprintf(remote_uuid.uuid, sizeof remote_uuid,
                         "NET_"LPX64"_UUID", (__u64)req->rq_peer.nid);
                break;
        case 4:
                snprintf(remote_uuid.uuid, sizeof remote_uuid,
                         "NET_%x_UUID", (__u32)req->rq_peer.nid);
                break;
        default:
                LBUG();
        }

        spin_lock_bh(&target->obd_processing_task_lock);
        abort_recovery = target->obd_abort_recovery;
        spin_unlock_bh(&target->obd_processing_task_lock);
        if (abort_recovery)
                target_abort_recovery(target);

        tmp = lustre_msg_buf(req->rq_reqmsg, 2, sizeof conn);
        if (tmp == NULL)
                GOTO(out, rc = -EPROTO);

        memcpy(&conn, tmp, sizeof conn);

        data = lustre_swab_reqbuf(req, 3, sizeof(*data), lustre_swab_connect);
        rc = lustre_pack_reply(req, 1, &size, NULL);
        if (rc)
                GOTO(out, rc);

        if (lustre_msg_get_op_flags(req->rq_reqmsg) & MSG_CONNECT_LIBCLIENT) {
                if (!data) {
                        DEBUG_REQ(D_INFO, req, "Refusing old (unversioned) "
                                  "libclient connection attempt\n");
                        GOTO(out, rc = -EPROTO);
                } else if (data->ocd_version < LUSTRE_VERSION_CODE -
                                               LUSTRE_VERSION_ALLOWED_OFFSET) {
                        DEBUG_REQ(D_INFO, req, "Refusing old (%d.%d.%d.%d) "
                                  "libclient connection attempt\n",
                                  OBD_OCD_VERSION_MAJOR(data->ocd_version),
                                  OBD_OCD_VERSION_MINOR(data->ocd_version),
                                  OBD_OCD_VERSION_PATCH(data->ocd_version),
                                  OBD_OCD_VERSION_FIX(data->ocd_version));
                        data = lustre_msg_buf(req->rq_repmsg, 0,
                                              offsetof(typeof(*data),
                                                       ocd_version) +
                                              sizeof(data->ocd_version));
                        if (data) {
                                data->ocd_connect_flags = OBD_CONNECT_VERSION;
                                data->ocd_version = LUSTRE_VERSION_CODE;
                        }
                        GOTO(out, rc = -EPROTO);
                }
        }

        /* lctl gets a backstage, all-access pass. */
        if (obd_uuid_equals(&cluuid, &target->obd_uuid))
                goto dont_check_exports;

        spin_lock(&target->obd_dev_lock);
        list_for_each(p, &target->obd_exports) {
                export = list_entry(p, struct obd_export, exp_obd_chain);
                if (obd_uuid_equals(&cluuid, &export->exp_client_uuid)) {
                        if (export->exp_connecting) { /* bug 9635, et. al. */
                                CWARN("%s: exp %p already connecting\n",
                                      export->exp_obd->obd_name, export);
                                export = NULL;
                                rc = -EALREADY;
                                break;
                        }
                        export->exp_connecting = 1;
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
                OBD_FAIL_TIMEOUT(OBD_FAIL_TGT_DELAY_CONNECT, 2 * obd_timeout);
        } else if (req->rq_reqmsg->conn_cnt == 1) {
                CERROR("%s: NID %s (%s) reconnected with 1 conn_cnt; "
                       "cookies not random?\n", target->obd_name,
                       libcfs_nid2str(req->rq_peer.nid), cluuid.uuid);
                GOTO(out, rc = -EALREADY);
        } else {
                OBD_FAIL_TIMEOUT(OBD_FAIL_TGT_DELAY_RECONNECT, 2 * obd_timeout);
        }

        /* We want to handle EALREADY but *not* -EALREADY from
         * target_handle_reconnect(), return reconnection state in a flag */
        if (rc == EALREADY) {
                lustre_msg_add_op_flags(req->rq_repmsg, MSG_CONNECT_RECONNECT);
                rc = 0;
        } else if (rc) {
                GOTO(out, rc);
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
                        CERROR("%s: denying connection for new client %s (%s): "
                               "%d clients in recovery for %lds\n",
                               target->obd_name,
                               libcfs_nid2str(req->rq_peer.nid), cluuid.uuid,
                               target->obd_recoverable_clients,
                               cfs_duration_sec(cfs_time_sub(cfs_timer_deadline(&target->obd_recovery_timer),
                                                             cfs_time_current())));
                        rc = -EBUSY;
                } else {
 dont_check_exports:
                        rc = obd_connect(&conn, target, &cluuid, data);
                }
        } else {
                rc = obd_reconnect(export, target, &cluuid, data);
        }

        if (rc)
                GOTO(out, rc);

        /* Return only the parts of obd_connect_data that we understand, so the
         * client knows that we don't understand the rest. */
        if (data)
                memcpy(lustre_msg_buf(req->rq_repmsg, 0, sizeof(*data)), data,
                       sizeof(*data));

        /* If all else goes well, this is our RPC return code. */
        req->rq_status = 0;

        req->rq_repmsg->handle = conn;

        /* ownership of this export ref transfers to the request AFTER we
         * drop any previous reference the request had, but we don't want
         * that to go to zero before we get our new export reference. */
        export = class_conn2export(&conn);
        LASSERT(export != NULL);

        /* If the client and the server are the same node, we will already
         * have an export that really points to the client's DLM export,
         * because we have a shared handles table.
         *
         * XXX this will go away when shaver stops sending the "connect" handle
         * in the real "remote handle" field of the request --phik 24 Apr 2003
         */
        if (req->rq_export != NULL)
                class_export_put(req->rq_export);

        req->rq_export = export;

        spin_lock_irqsave(&export->exp_lock, flags);
        if (export->exp_conn_cnt >= req->rq_reqmsg->conn_cnt) {
                CERROR("%s: %s already connected at higher conn_cnt: %d > %d\n",
                       cluuid.uuid, libcfs_nid2str(req->rq_peer.nid),
                       export->exp_conn_cnt, req->rq_reqmsg->conn_cnt);
                spin_unlock_irqrestore(&export->exp_lock, flags);
                GOTO(out, rc = -EALREADY);
        }
        export->exp_conn_cnt = req->rq_reqmsg->conn_cnt;
        spin_unlock_irqrestore(&export->exp_lock, flags);

        /* request from liblustre?  Don't evict it for not pinging. */
        if (lustre_msg_get_op_flags(req->rq_reqmsg) & MSG_CONNECT_LIBCLIENT) {
                export->exp_libclient = 1;
                spin_lock(&target->obd_dev_lock);
                list_del_init(&export->exp_obd_chain_timed);
                spin_unlock(&target->obd_dev_lock);
        }

        if (export->exp_connection != NULL)
                ptlrpc_put_connection(export->exp_connection);
        export->exp_connection = ptlrpc_get_connection(req->rq_peer,
                                                       req->rq_self,
                                                       &remote_uuid);

        if (lustre_msg_get_op_flags(req->rq_repmsg) & MSG_CONNECT_RECONNECT)
                GOTO(out, rc = 0);

        if (target->obd_recovering)
                target->obd_connected_clients++;

        memcpy(&conn, lustre_msg_buf(req->rq_reqmsg, 2, sizeof conn),
               sizeof conn);

        if (export->exp_imp_reverse != NULL)
                class_destroy_import(export->exp_imp_reverse);
        revimp = export->exp_imp_reverse = class_new_import(target);
        revimp->imp_connection = ptlrpc_connection_addref(export->exp_connection);
        revimp->imp_client = &export->exp_obd->obd_ldlm_client;
        revimp->imp_remote_handle = conn;
        revimp->imp_dlm_fake = 1;
        revimp->imp_state = LUSTRE_IMP_FULL;
        class_import_put(revimp);
out:
        if (export)
                export->exp_connecting = 0;
        if (rc)
                req->rq_status = rc;
        RETURN(rc);
}

int target_handle_disconnect(struct ptlrpc_request *req)
{
        int rc;
        ENTRY;

        rc = lustre_pack_reply(req, 0, NULL, NULL);
        if (rc)
                RETURN(rc);

        /* keep the rq_export around so we can send the reply */
        req->rq_status = obd_disconnect(class_export_get(req->rq_export));
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
        if (req->rq_reply_state != NULL) {
                ptlrpc_rs_decref(req->rq_reply_state);
                /* req->rq_reply_state = NULL; */
        }

        class_export_put(req->rq_export);
        OBD_FREE(req->rq_reqmsg, req->rq_reqlen);
        OBD_FREE(req, sizeof *req);
}

static void target_finish_recovery(struct obd_device *obd)
{
        struct list_head *tmp, *n;

        CWARN("%s: sending delayed replies to recovered clients\n",
              obd->obd_name);

        ldlm_reprocess_all_ns(obd->obd_namespace);

        /* when recovery finished, cleanup orphans on mds and ost */
        if (OBT(obd) && OBP(obd, postrecov)) {
                int rc = OBP(obd, postrecov)(obd);
                CWARN("%s: recovery %s: rc %d\n", obd->obd_name,
                      rc < 0 ? "failed" : "complete", rc);
        }

        list_for_each_safe(tmp, n, &obd->obd_delayed_reply_queue) {
                struct ptlrpc_request *req;
                req = list_entry(tmp, struct ptlrpc_request, rq_list);
                list_del(&req->rq_list);
                DEBUG_REQ(D_WARNING, req, "delayed:");
                ptlrpc_reply(req);
                target_release_saved_req(req);
        }
        obd->obd_recovery_end = CURRENT_SECONDS;
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
        ENTRY;

        LASSERT(obd->obd_stopping);

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
                target_release_saved_req(req);
        }

        list_for_each_safe(tmp, n, &obd->obd_recovery_queue) {
                req = list_entry(tmp, struct ptlrpc_request, rq_list);
                list_del(&req->rq_list);
                target_release_saved_req(req);
        }
        EXIT;
}

void target_abort_recovery(void *data)
{
        struct obd_device *obd = data;

        ENTRY;
        spin_lock_bh(&obd->obd_processing_task_lock);
        if (!obd->obd_recovering) {
                spin_unlock_bh(&obd->obd_processing_task_lock);
                EXIT;
                return;
        }
        obd->obd_recovering = obd->obd_abort_recovery = 0;
        obd->obd_recoverable_clients = 0;
        target_cancel_recovery_timer(obd);
        spin_unlock_bh(&obd->obd_processing_task_lock);

        CERROR("%s: recovery period over; disconnecting unfinished clients.\n",
               obd->obd_name);
        class_disconnect_stale_exports(obd);
        abort_recovery_queue(obd);

        target_finish_recovery(obd);

        ptlrpc_run_recovery_over_upcall(obd);
        EXIT;
}

static void target_recovery_expired(unsigned long castmeharder)
{
        struct obd_device *obd = (struct obd_device *)castmeharder;
        CERROR("%s: recovery timed out, aborting\n", obd->obd_name);
        spin_lock_bh(&obd->obd_processing_task_lock);
        if (obd->obd_recovering)
                obd->obd_abort_recovery = 1;
        cfs_waitq_signal(&obd->obd_next_transno_waitq);
        spin_unlock_bh(&obd->obd_processing_task_lock);
}


/* obd_processing_task_lock should be held */
void target_cancel_recovery_timer(struct obd_device *obd)
{
        CDEBUG(D_HA, "%s: cancel recovery timer\n", obd->obd_name);
        cfs_timer_disarm(&obd->obd_recovery_timer);
}

static void reset_recovery_timer(struct obd_device *obd)
{
        spin_lock_bh(&obd->obd_processing_task_lock);
        if (!obd->obd_recovering) {
                spin_unlock_bh(&obd->obd_processing_task_lock);
                return;
        }
        cfs_timer_arm(&obd->obd_recovery_timer, 
                      cfs_time_shift(OBD_RECOVERY_TIMEOUT));
        spin_unlock_bh(&obd->obd_processing_task_lock);
        CDEBUG(D_HA, "%s: timer will expire in %u seconds\n", obd->obd_name,
               OBD_RECOVERY_TIMEOUT);
        /* Only used for lprocfs_status */
        obd->obd_recovery_end = CURRENT_SECONDS + OBD_RECOVERY_TIMEOUT;
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
              OBD_RECOVERY_TIMEOUT);
        obd->obd_recovery_handler = handler;
        cfs_timer_init(&obd->obd_recovery_timer, target_recovery_expired, obd);
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
                LASSERT(obd->obd_processing_task == cfs_curproc_pid());
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
                //mds_fsync_super(obd->u.obt.obt_sb);
                class_export_put(req->rq_export);
                if (req->rq_reply_state != NULL) {
                        ptlrpc_rs_decref(req->rq_reply_state);
                        /* req->rq_reply_state = NULL; */
                }
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
                CFS_INIT_LIST_HEAD(&req->rq_list);
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
        if (obd->obd_processing_task == cfs_curproc_pid() ||
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
        if ((lustre_msg_get_flags(req->rq_reqmsg) & (MSG_RESENT|MSG_REPLAY)) ==
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
        CFS_INIT_LIST_HEAD(&req->rq_list);

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
                cfs_waitq_signal(&obd->obd_next_transno_waitq);
                spin_unlock_bh(&obd->obd_processing_task_lock);
                return 0;
        }

        /* Nobody is processing, and we know there's (at least) one to process
         * now, so we'll do the honours.
         */
        obd->obd_processing_task = cfs_curproc_pid();
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
        *saved_req = *req;
        memcpy(reqmsg, req->rq_reqmsg, req->rq_reqlen);

        /* Don't race cleanup */
        spin_lock_bh(&obd->obd_processing_task_lock);
        if (obd->obd_stopping) {
                spin_unlock_bh(&obd->obd_processing_task_lock);
                OBD_FREE(reqmsg, req->rq_reqlen);
                OBD_FREE(saved_req, sizeof *req);
                req->rq_status = -ENOTCONN;
                /* rv is ignored anyhow */
                return -ENOTCONN;
        }
        ptlrpc_rs_addref(req->rq_reply_state);  /* +1 ref for saved reply */
        req = saved_req;
        req->rq_reqmsg = reqmsg;
        class_export_get(req->rq_export);
        list_add(&req->rq_list, &obd->obd_delayed_reply_queue);

        /* only count the first "replay over" request from each
           export */
        if (req->rq_export->exp_replay_needed) {
                --obd->obd_recoverable_clients;
                req->rq_export->exp_replay_needed = 0;
        }
        recovery_done = (obd->obd_recoverable_clients == 0);
        spin_unlock_bh(&obd->obd_processing_task_lock);

        OBD_RACE(OBD_FAIL_LDLM_RECOV_CLIENTS);
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
                cfs_waitq_signal(&obd->obd_next_transno_waitq);
        }

        return 1;
}

int
target_send_reply_msg (struct ptlrpc_request *req, int rc, int fail_id)
{
        if (OBD_FAIL_CHECK(fail_id | OBD_FAIL_ONCE)) {
                obd_fail_loc |= OBD_FAIL_ONCE | OBD_FAILED;
                DEBUG_REQ(D_ERROR, req, "dropping reply");
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
        struct ptlrpc_service     *svc;

        svc = req->rq_rqbd->rqbd_service;
        rs = req->rq_reply_state;
        if (rs == NULL || !rs->rs_difficult) {
                /* no notifiers */
                target_send_reply_msg (req, rc, fail_id);
                return;
        }

        /* must be an export if locks saved */
        LASSERT (req->rq_export != NULL);
        /* req/reply consistent */
        LASSERT (rs->rs_service == svc);

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

        if (netrc != 0) {
                /* error sending: reply is off the net.  Also we need +1
                 * reply ref until ptlrpc_server_handle_reply() is done
                 * with the reply state (if the send was successful, there
                 * would have been +1 ref for the net, which
                 * reply_out_callback leaves alone) */
                rs->rs_on_net = 0;
                ptlrpc_rs_addref(rs);
                atomic_inc (&svc->srv_outstanding_replies);
        }

        if (!rs->rs_on_net ||                   /* some notifier */
            list_empty(&rs->rs_exp_list) ||     /* completed already */
            list_empty(&rs->rs_obd_list)) {
                list_add_tail (&rs->rs_list, &svc->srv_reply_queue);
                cfs_waitq_signal (&svc->srv_waitq);
        } else {
                list_add (&rs->rs_list, &svc->srv_active_replies);
                rs->rs_scheduled = 0;           /* allow notifier to schedule */
        }

        spin_unlock_irqrestore (&svc->srv_lock, flags);
}

int target_handle_ping(struct ptlrpc_request *req)
{
        return lustre_pack_reply(req, 0, NULL, NULL);
}

void target_committed_to_req(struct ptlrpc_request *req)
{
        struct obd_device *obd = req->rq_export->exp_obd;

        if (!obd->obd_no_transno && req->rq_repmsg != NULL)
                req->rq_repmsg->last_committed = obd->obd_last_committed;
        else
                DEBUG_REQ(D_IOCTL, req, "not sending last_committed update");

        CDEBUG(D_INFO, "last_committed "LPU64", xid "LPU64"\n",
               obd->obd_last_committed, req->rq_xid);
}

EXPORT_SYMBOL(target_committed_to_req);

#ifdef HAVE_QUOTA_SUPPORT
int target_handle_qc_callback(struct ptlrpc_request *req)
{
        struct obd_quotactl *oqctl;
        struct client_obd *cli = &req->rq_export->exp_obd->u.cli;

        oqctl = lustre_swab_reqbuf(req, 0, sizeof(*oqctl),
                                   lustre_swab_obd_quotactl);

        cli->cl_qchk_stat = oqctl->qc_stat;

        return 0;
}

int target_handle_dqacq_callback(struct ptlrpc_request *req)
{
#ifdef __KERNEL__
        struct obd_device *obd = req->rq_export->exp_obd;
        struct obd_device *master_obd;
        struct lustre_quota_ctxt *qctxt;
        struct qunit_data *qdata, *rep;
        int rc = 0, repsize = sizeof(struct qunit_data);
        ENTRY;
        
        rc = lustre_pack_reply(req, 1, &repsize, NULL);
        if (rc) {
                CERROR("packing reply failed!: rc = %d\n", rc);
                RETURN(rc);
        }
        rep = lustre_msg_buf(req->rq_repmsg, 0, sizeof(*rep));
        LASSERT(rep);
        
        qdata = lustre_swab_reqbuf(req, 0, sizeof(*qdata), lustre_swab_qdata);
        if (qdata == NULL) {
                CERROR("unpacking request buffer failed!");
                RETURN(-EPROTO);
        }

        /* we use the observer */
        LASSERT(obd->obd_observer && obd->obd_observer->obd_observer);
        master_obd = obd->obd_observer->obd_observer;
        qctxt = &master_obd->u.obt.obt_qctxt;
        
        LASSERT(qctxt->lqc_handler);
        rc = qctxt->lqc_handler(master_obd, qdata, req->rq_reqmsg->opc);
        if (rc && rc != -EDQUOT)
                CDEBUG(rc == -EBUSY  ? D_QUOTA : D_ERROR, 
                       "dqacq failed! (rc:%d)\n", rc);
        
        /* the qd_count might be changed in lqc_handler */
        memcpy(rep, qdata, sizeof(*rep));
        req->rq_status = rc;
        rc = ptlrpc_reply(req);
        
        RETURN(rc);     
#else
        return 0;
#endif /* !__KERNEL__ */
}
#endif /* HAVE_QUOTA_SUPPORT */

ldlm_mode_t lck_compat_array[] = {
        [LCK_EX] LCK_COMPAT_EX,
        [LCK_PW] LCK_COMPAT_PW,
        [LCK_PR] LCK_COMPAT_PR,
        [LCK_CW] LCK_COMPAT_CW,
        [LCK_CR] LCK_COMPAT_CR,
        [LCK_NL] LCK_COMPAT_NL,
        [LCK_GROUP] LCK_COMPAT_GROUP
};
