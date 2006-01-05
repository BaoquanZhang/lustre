/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (c) 2002, 2003 Cluster File Systems, Inc.
 *   Author: Mike Shaver <shaver@clusterfs.com>
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

#define DEBUG_SUBSYSTEM S_RPC
#ifndef __KERNEL__
# include <liblustre.h>
#endif

#include <obd_support.h>
#include <lustre_ha.h>
#include <lustre_net.h>
#include <lustre_import.h>
#include <lustre_export.h>
#include <obd.h>
#include <obd_class.h>

#include "ptlrpc_internal.h"

struct ptlrpc_connect_async_args {
         __u64 pcaa_peer_committed;
        int pcaa_initial_connect;
};

/* A CLOSED import should remain so. */
#define IMPORT_SET_STATE_NOLOCK(imp, state)                                    \
do {                                                                           \
        if (imp->imp_state != LUSTRE_IMP_CLOSED) {                             \
               CDEBUG(D_HA, "%p %s: changing import state from %s to %s\n",    \
                      imp, imp->imp_target_uuid.uuid,                          \
                      ptlrpc_import_state_name(imp->imp_state),                \
                      ptlrpc_import_state_name(state));                        \
               imp->imp_state = state;                                         \
        }                                                                      \
} while(0)

#define IMPORT_SET_STATE(imp, state)                    \
do {                                                    \
        unsigned long flags;                            \
                                                        \
        spin_lock_irqsave(&imp->imp_lock, flags);       \
        IMPORT_SET_STATE_NOLOCK(imp, state);            \
        spin_unlock_irqrestore(&imp->imp_lock, flags);  \
} while(0)


static int ptlrpc_connect_interpret(struct ptlrpc_request *request,
                                    void * data, int rc);
int ptlrpc_import_recovery_state_machine(struct obd_import *imp);

/* Only this function is allowed to change the import state when it is
 * CLOSED. I would rather refcount the import and free it after
 * disconnection like we do with exports. To do that, the client_obd
 * will need to save the peer info somewhere other than in the import,
 * though. */
int ptlrpc_init_import(struct obd_import *imp)
{
        unsigned long flags;

        spin_lock_irqsave(&imp->imp_lock, flags);

        imp->imp_generation++;
        imp->imp_state =  LUSTRE_IMP_NEW;

        spin_unlock_irqrestore(&imp->imp_lock, flags);

        return 0;
}

#define UUID_STR "_UUID"
static void deuuidify(char *uuid, const char *prefix, char **uuid_start, int *uuid_len)
{
        *uuid_start = !prefix || strncmp(uuid, prefix, strlen(prefix))
                ? uuid : uuid + strlen(prefix);

        *uuid_len = strlen(*uuid_start);

        if (*uuid_len < strlen(UUID_STR))
                return;

        if (!strncmp(*uuid_start + *uuid_len - strlen(UUID_STR),
                    UUID_STR, strlen(UUID_STR)))
                *uuid_len -= strlen(UUID_STR);
}

/* Returns true if import was FULL, false if import was already not
 * connected.
 */
int ptlrpc_set_import_discon(struct obd_import *imp)
{
        unsigned long flags;
        int rc = 0;

        spin_lock_irqsave(&imp->imp_lock, flags);

        if (imp->imp_state == LUSTRE_IMP_FULL) {
                char *target_start;
                int   target_len;

                deuuidify(imp->imp_target_uuid.uuid, NULL,
                          &target_start, &target_len);

                LCONSOLE_ERROR("Connection to service %.*s via nid %s was "
                               "lost; in progress operations using this "
                               "service will %s.\n",
                               target_len, target_start,
                               libcfs_nid2str(imp->imp_connection->c_peer.nid),
                               imp->imp_replayable 
                               ? "wait for recovery to complete"
                               : "fail");

                if (obd_dump_on_timeout)
                        libcfs_debug_dumplog();

                CWARN("%s: connection lost to %s@%s\n",
                      imp->imp_obd->obd_name,
                      imp->imp_target_uuid.uuid,
                      imp->imp_connection->c_remote_uuid.uuid);
                IMPORT_SET_STATE_NOLOCK(imp, LUSTRE_IMP_DISCON);
                spin_unlock_irqrestore(&imp->imp_lock, flags);
                obd_import_event(imp->imp_obd, imp, IMP_EVENT_DISCON);
                rc = 1;
        } else {
                spin_unlock_irqrestore(&imp->imp_lock, flags);
                CDEBUG(D_HA, "%p %s: import already not connected: %s\n",
                       imp,imp->imp_client->cli_name,
                       ptlrpc_import_state_name(imp->imp_state));
        }

        return rc;
}

/*
 * This acts as a barrier; all existing requests are rejected, and
 * no new requests will be accepted until the import is valid again.
 */
void ptlrpc_deactivate_import(struct obd_import *imp)
{
        unsigned long flags;
        ENTRY;

        spin_lock_irqsave(&imp->imp_lock, flags);
        CDEBUG(D_HA, "setting import %s INVALID\n", imp->imp_target_uuid.uuid);
        imp->imp_invalid = 1;
        imp->imp_generation++;
        spin_unlock_irqrestore(&imp->imp_lock, flags);

        ptlrpc_abort_inflight(imp);
        obd_import_event(imp->imp_obd, imp, IMP_EVENT_INACTIVE);
}

/*
 * This function will invalidate the import, if necessary, then block
 * for all the RPC completions, and finally notify the obd to
 * invalidate its state (ie cancel locks, clear pending requests,
 * etc).
 */
void ptlrpc_invalidate_import(struct obd_import *imp)
{
        struct l_wait_info lwi;
        int rc;

        if (!imp->imp_invalid)
                ptlrpc_deactivate_import(imp);

        LASSERT(imp->imp_invalid);

        /* wait for all requests to error out and call completion callbacks */
        lwi = LWI_TIMEOUT_INTR(cfs_timeout_cap(cfs_time_seconds(obd_timeout)), 
                               NULL, NULL, NULL);
        rc = l_wait_event(imp->imp_recovery_waitq,
                          (atomic_read(&imp->imp_inflight) == 0),
                          &lwi);

        if (rc)
                CERROR("%s: rc = %d waiting for callback (%d != 0)\n",
                       imp->imp_target_uuid.uuid, rc,
                       atomic_read(&imp->imp_inflight));

        obd_import_event(imp->imp_obd, imp, IMP_EVENT_INVALIDATE);
}

void ptlrpc_activate_import(struct obd_import *imp)
{
        struct obd_device *obd = imp->imp_obd;
        unsigned long flags;

        spin_lock_irqsave(&imp->imp_lock, flags);
        imp->imp_invalid = 0;
        spin_unlock_irqrestore(&imp->imp_lock, flags);

        obd_import_event(obd, imp, IMP_EVENT_ACTIVE);
}

void ptlrpc_fail_import(struct obd_import *imp, int generation)
{
        ENTRY;

        LASSERT (!imp->imp_dlm_fake);

        if (ptlrpc_set_import_discon(imp)) {
                unsigned long flags;

                if (!imp->imp_replayable) {
                        CDEBUG(D_HA, "import %s@%s for %s not replayable, "
                               "auto-deactivating\n",
                               imp->imp_target_uuid.uuid,
                               imp->imp_connection->c_remote_uuid.uuid,
                               imp->imp_obd->obd_name);
                        ptlrpc_deactivate_import(imp);
                }

                CDEBUG(D_HA, "%s: waking up pinger\n",
                       imp->imp_target_uuid.uuid);

                spin_lock_irqsave(&imp->imp_lock, flags);
                imp->imp_force_verify = 1;
                spin_unlock_irqrestore(&imp->imp_lock, flags);

                ptlrpc_pinger_wake_up();
        }
        EXIT;
}

static int import_select_connection(struct obd_import *imp)
{
        struct obd_import_conn *imp_conn;
        struct obd_export *dlmexp;
        ENTRY;

        spin_lock(&imp->imp_lock);

        if (list_empty(&imp->imp_conn_list)) {
                CERROR("%s: no connections available\n",
                        imp->imp_obd->obd_name);
                spin_unlock(&imp->imp_lock);
                RETURN(-EINVAL);
        }

        if (imp->imp_conn_current &&
            imp->imp_conn_current->oic_item.next != &imp->imp_conn_list) {
                imp_conn = list_entry(imp->imp_conn_current->oic_item.next,
                                      struct obd_import_conn, oic_item);
        } else {
                imp_conn = list_entry(imp->imp_conn_list.next,
                                      struct obd_import_conn, oic_item);
        }

        /* switch connection, don't mind if it's same as the current one */
        if (imp->imp_connection)
                ptlrpc_put_connection(imp->imp_connection);
        imp->imp_connection = ptlrpc_connection_addref(imp_conn->oic_conn);

        dlmexp =  class_conn2export(&imp->imp_dlm_handle);
        LASSERT(dlmexp != NULL);
        if (dlmexp->exp_connection)
                ptlrpc_put_connection(dlmexp->exp_connection);
        dlmexp->exp_connection = ptlrpc_connection_addref(imp_conn->oic_conn);
        class_export_put(dlmexp);

        if (imp->imp_conn_current && (imp->imp_conn_current != imp_conn)) {
                LCONSOLE_WARN("Changing connection for %s to %s\n",
                              imp->imp_obd->obd_name, imp_conn->oic_uuid.uuid);
        }
        imp->imp_conn_current = imp_conn;
        CDEBUG(D_HA, "%s: import %p using connection %s\n",
               imp->imp_obd->obd_name, imp, imp_conn->oic_uuid.uuid);
        spin_unlock(&imp->imp_lock);

        RETURN(0);
}

int ptlrpc_connect_import(struct obd_import *imp, char * new_uuid)
{
        struct obd_device *obd = imp->imp_obd;
        int initial_connect = 0;
        int rc;
        __u64 committed_before_reconnect = 0;
        struct ptlrpc_request *request;
        int size[] = {sizeof(imp->imp_target_uuid),
                      sizeof(obd->obd_uuid),
                      sizeof(imp->imp_dlm_handle),
                      sizeof(imp->imp_connect_data)};
        char *tmp[] = {imp->imp_target_uuid.uuid,
                       obd->obd_uuid.uuid,
                       (char *)&imp->imp_dlm_handle,
                       (char *)&imp->imp_connect_data};
        struct ptlrpc_connect_async_args *aa;
        unsigned long flags;

        ENTRY;
        spin_lock_irqsave(&imp->imp_lock, flags);
        if (imp->imp_state == LUSTRE_IMP_CLOSED) {
                spin_unlock_irqrestore(&imp->imp_lock, flags);
                CERROR("can't connect to a closed import\n");
                RETURN(-EINVAL);
        } else if (imp->imp_state == LUSTRE_IMP_FULL) {
                spin_unlock_irqrestore(&imp->imp_lock, flags);
                CERROR("already connected\n");
                RETURN(0);
        } else if (imp->imp_state == LUSTRE_IMP_CONNECTING) {
                spin_unlock_irqrestore(&imp->imp_lock, flags);
                CERROR("already connecting\n");
                RETURN(-EALREADY);
        }

        IMPORT_SET_STATE_NOLOCK(imp, LUSTRE_IMP_CONNECTING);

        imp->imp_conn_cnt++;
        imp->imp_resend_replay = 0;

        if (imp->imp_remote_handle.cookie == 0) {
                initial_connect = 1;
        } else {
                committed_before_reconnect = imp->imp_peer_committed_transno;
        }

        spin_unlock_irqrestore(&imp->imp_lock, flags);

        if (new_uuid) {
                struct obd_uuid uuid;

                obd_str2uuid(&uuid, new_uuid);
                rc = import_set_conn_priority(imp, &uuid);
                if (rc)
                        GOTO(out, rc);
        }

        rc = import_select_connection(imp);
        if (rc)
                GOTO(out, rc);

        request = ptlrpc_prep_req(imp, imp->imp_connect_op, 4, size, tmp);
        if (!request)
                GOTO(out, rc = -ENOMEM);

#ifndef __KERNEL__
        lustre_msg_add_op_flags(request->rq_reqmsg, MSG_CONNECT_LIBCLIENT);
#endif

        request->rq_send_state = LUSTRE_IMP_CONNECTING;
        /* Allow a slightly larger reply for future growth compatibility */
        size[0] = sizeof(struct obd_connect_data) + 16 * sizeof(__u64);
        request->rq_replen = lustre_msg_size(1, size);
        request->rq_interpret_reply = ptlrpc_connect_interpret;

        LASSERT (sizeof (*aa) <= sizeof (request->rq_async_args));
        aa = (struct ptlrpc_connect_async_args *)&request->rq_async_args;
        memset(aa, 0, sizeof *aa);

        aa->pcaa_peer_committed = committed_before_reconnect;
        aa->pcaa_initial_connect = initial_connect;

        if (aa->pcaa_initial_connect) {
                imp->imp_replayable = 1;
                /* On an initial connect, we don't know which one of a 
                   failover server pair is up.  Don't wait long. */
                request->rq_timeout = max((int)(obd_timeout / 20), 5);
        }

        DEBUG_REQ(D_RPCTRACE, request, "(re)connect request");
        ptlrpcd_add_req(request);
        rc = 0;
out:
        if (rc != 0) {
                IMPORT_SET_STATE(imp, LUSTRE_IMP_DISCON);
        }

        RETURN(rc);
}

static void ptlrpc_maybe_ping_import_soon(struct obd_import *imp)
{
        struct obd_import_conn *imp_conn;
        unsigned long flags;
        int wake_pinger = 0;

        ENTRY;

        spin_lock_irqsave(&imp->imp_lock, flags);
        if (list_empty(&imp->imp_conn_list))
                GOTO(unlock, 0);

        imp_conn = list_entry(imp->imp_conn_list.prev,
                              struct obd_import_conn,
                              oic_item);

        if (imp->imp_conn_current != imp_conn) {
                ptlrpc_ping_import_soon(imp);
                wake_pinger = 1;
        }

 unlock:
        spin_unlock_irqrestore(&imp->imp_lock, flags);

        if (wake_pinger)
                ptlrpc_pinger_wake_up();

        EXIT;
}

static int ptlrpc_connect_interpret(struct ptlrpc_request *request,
                                    void * data, int rc)
{
        struct ptlrpc_connect_async_args *aa = data;
        struct obd_import *imp = request->rq_import;
        struct lustre_handle old_hdl;
        unsigned long flags;
        int msg_flags;
        ENTRY;

        spin_lock_irqsave(&imp->imp_lock, flags);
        if (imp->imp_state == LUSTRE_IMP_CLOSED) {
                spin_unlock_irqrestore(&imp->imp_lock, flags);
                RETURN(0);
        }
        spin_unlock_irqrestore(&imp->imp_lock, flags);

        if (rc)
                GOTO(out, rc);

        LASSERT(imp->imp_conn_current);

        msg_flags = lustre_msg_get_op_flags(request->rq_repmsg);

        /* All imports are pingable */
        imp->imp_pingable = 1;

        if (aa->pcaa_initial_connect) {
                if (msg_flags & MSG_CONNECT_REPLAYABLE) {
                        CDEBUG(D_HA, "connected to replayable target: %s\n",
                               imp->imp_target_uuid.uuid);
                        imp->imp_replayable = 1;
                } else {
                        imp->imp_replayable = 0;
                }
                imp->imp_remote_handle = request->rq_repmsg->handle;

                IMPORT_SET_STATE(imp, LUSTRE_IMP_FULL);
                GOTO(finish, rc = 0);
        }

        /* Determine what recovery state to move the import to. */
        if (MSG_CONNECT_RECONNECT & msg_flags) {
                memset(&old_hdl, 0, sizeof(old_hdl));
                if (!memcmp(&old_hdl, &request->rq_repmsg->handle,
                            sizeof (old_hdl))) {
                        CERROR("%s@%s didn't like our handle "LPX64
                               ", failed\n", imp->imp_target_uuid.uuid,
                               imp->imp_connection->c_remote_uuid.uuid,
                               imp->imp_dlm_handle.cookie);
                        GOTO(out, rc = -ENOTCONN);
                }

                if (memcmp(&imp->imp_remote_handle, &request->rq_repmsg->handle,
                           sizeof(imp->imp_remote_handle))) {
                        CERROR("%s@%s changed handle from "LPX64" to "LPX64
                               "; copying, but this may foreshadow disaster\n",
                               imp->imp_target_uuid.uuid,
                               imp->imp_connection->c_remote_uuid.uuid,
                               imp->imp_remote_handle.cookie,
                               request->rq_repmsg->handle.cookie);
                        imp->imp_remote_handle = request->rq_repmsg->handle;
                } else {
                        CDEBUG(D_HA, "reconnected to %s@%s after partition\n",
                               imp->imp_target_uuid.uuid,
                               imp->imp_connection->c_remote_uuid.uuid);
                }

                if (imp->imp_invalid) {
                        IMPORT_SET_STATE(imp, LUSTRE_IMP_EVICTED);
                } else if (MSG_CONNECT_RECOVERING & msg_flags) {
                        CDEBUG(D_HA, "%s: reconnected to %s during replay\n",
                               imp->imp_obd->obd_name,
                               imp->imp_target_uuid.uuid);
                        imp->imp_resend_replay = 1;
                        IMPORT_SET_STATE(imp, LUSTRE_IMP_REPLAY);
                } else {
                        IMPORT_SET_STATE(imp, LUSTRE_IMP_RECOVER);
                }
        } else if ((MSG_CONNECT_RECOVERING & msg_flags) && !imp->imp_invalid) {
                LASSERT(imp->imp_replayable);
                imp->imp_remote_handle = request->rq_repmsg->handle;
                imp->imp_last_replay_transno = 0;
                IMPORT_SET_STATE(imp, LUSTRE_IMP_REPLAY);
        } else {
                imp->imp_remote_handle = request->rq_repmsg->handle;
                IMPORT_SET_STATE(imp, LUSTRE_IMP_EVICTED);
        }

        /* Sanity checks for a reconnected import. */
        if (!(imp->imp_replayable) != !(msg_flags & MSG_CONNECT_REPLAYABLE)) {
                CERROR("imp_replayable flag does not match server "
                       "after reconnect. We should LBUG right here.\n");
        }

        if (request->rq_repmsg->last_committed < aa->pcaa_peer_committed) {
                CERROR("%s went back in time (transno "LPD64
                       " was previously committed, server now claims "LPD64
                       ")!  See https://bugzilla.clusterfs.com/"
                       "long_list.cgi?buglist=9646\n",
                       imp->imp_target_uuid.uuid, aa->pcaa_peer_committed,
                       request->rq_repmsg->last_committed);
        }

finish:
        rc = ptlrpc_import_recovery_state_machine(imp);
        if (rc != 0) {
                if (rc == -ENOTCONN) {
                        CDEBUG(D_HA, "evicted/aborted by %s@%s during recovery;"
                               "invalidating and reconnecting\n",
                               imp->imp_target_uuid.uuid,
                               imp->imp_connection->c_remote_uuid.uuid);
                        ptlrpc_connect_import(imp, NULL);
                        RETURN(0);
                }
        } else {
                struct obd_connect_data *ocd;

                ocd = lustre_swab_repbuf(request, 0,
                                         sizeof *ocd, lustre_swab_connect);
                if (ocd == NULL) {
                        CERROR("Wrong connect data from server\n");
                        rc = -EPROTO;
                        GOTO(out, rc);
                }
                spin_lock_irqsave(&imp->imp_lock, flags);
                
                /*
                 * check that server granted subset of flags we asked for.
                 */
                LASSERT((ocd->ocd_connect_flags &
                         imp->imp_connect_data.ocd_connect_flags) ==
                        ocd->ocd_connect_flags);

                imp->imp_connect_data = *ocd;
                
                if (IMP_CROW_ABLE(imp)) {
                        CDEBUG(D_HA, "connected to CROW capable target: %s\n",
                               imp->imp_target_uuid.uuid);
                }
                if (imp->imp_conn_current != NULL) {
                        list_del(&imp->imp_conn_current->oic_item);
                        list_add(&imp->imp_conn_current->oic_item,
                                 &imp->imp_conn_list);
                        imp->imp_conn_current = NULL;
                        spin_unlock_irqrestore(&imp->imp_lock, flags);
                } else {
                        static int bug7269_dump = 0;
                        spin_unlock_irqrestore(&imp->imp_lock, flags);
                        CERROR("this is bug 7269 - please attach log there\n");
                        if (bug7269_dump == 0)
                                libcfs_debug_dumplog();
                        bug7269_dump = 1;
                }
        }

 out:
        if (rc != 0) {
                IMPORT_SET_STATE(imp, LUSTRE_IMP_DISCON);
                if (aa->pcaa_initial_connect && !imp->imp_initial_recov) {
                        ptlrpc_deactivate_import(imp);
                }

                ptlrpc_maybe_ping_import_soon(imp);

                CDEBUG(D_HA, "recovery of %s on %s failed (%d)\n",
                       imp->imp_target_uuid.uuid,
                       (char *)imp->imp_connection->c_remote_uuid.uuid, rc);
        }

        cfs_waitq_signal(&imp->imp_recovery_waitq);
        RETURN(rc);
}

static int completed_replay_interpret(struct ptlrpc_request *req,
                                    void * data, int rc)
{
        ENTRY;
        atomic_dec(&req->rq_import->imp_replay_inflight);
        if (req->rq_status == 0) {
                ptlrpc_import_recovery_state_machine(req->rq_import);
        } else {
                CDEBUG(D_HA, "%s: LAST_REPLAY message error: %d, "
                       "reconnecting\n",
                       req->rq_import->imp_obd->obd_name, req->rq_status);
                ptlrpc_connect_import(req->rq_import, NULL);
        }

        RETURN(0);
}

static int signal_completed_replay(struct obd_import *imp)
{
        struct ptlrpc_request *req;
        ENTRY;

        LASSERT(atomic_read(&imp->imp_replay_inflight) == 0);
        atomic_inc(&imp->imp_replay_inflight);

        req = ptlrpc_prep_req(imp, OBD_PING, 0, NULL, NULL);
        if (!req) {
                atomic_dec(&imp->imp_replay_inflight);
                RETURN(-ENOMEM);
        }

        req->rq_replen = lustre_msg_size(0, NULL);
        req->rq_send_state = LUSTRE_IMP_REPLAY_WAIT;
        req->rq_reqmsg->flags |= MSG_LAST_REPLAY;
        req->rq_timeout *= 3;
        req->rq_interpret_reply = completed_replay_interpret;

        ptlrpcd_add_req(req);
        RETURN(0);
}

#ifdef __KERNEL__
static int ptlrpc_invalidate_import_thread(void *data)
{
        struct obd_import *imp = data;

        ENTRY;

        lock_kernel();
        ptlrpc_daemonize();

        cfs_block_allsigs();
        THREAD_NAME(cfs_curproc_comm(), CFS_CURPROC_COMM_MAX - 1, "ll_imp_inval");
        unlock_kernel();

        CDEBUG(D_HA, "thread invalidate import %s to %s@%s\n",
               imp->imp_obd->obd_name, imp->imp_target_uuid.uuid,
               imp->imp_connection->c_remote_uuid.uuid);

        ptlrpc_invalidate_import(imp);

        IMPORT_SET_STATE(imp, LUSTRE_IMP_RECOVER);
        ptlrpc_import_recovery_state_machine(imp);

        RETURN(0);
}
#endif

int ptlrpc_import_recovery_state_machine(struct obd_import *imp)
{
        int rc = 0;
        int inflight;
        char *target_start;
        int target_len;

        ENTRY;
        if (imp->imp_state == LUSTRE_IMP_EVICTED) {
                deuuidify(imp->imp_target_uuid.uuid, NULL,
                          &target_start, &target_len);
                LCONSOLE_ERROR("This client was evicted by %.*s; in progress "
                               "operations using this service will fail.\n",
                               target_len, target_start);
                CDEBUG(D_HA, "evicted from %s@%s; invalidating\n",
                       imp->imp_target_uuid.uuid,
                       imp->imp_connection->c_remote_uuid.uuid);

#ifdef __KERNEL__
                rc = cfs_kernel_thread(ptlrpc_invalidate_import_thread, imp,
                                   CLONE_VM | CLONE_FILES);
                if (rc < 0)
                        CERROR("error starting invalidate thread: %d\n", rc);
                else
                        rc = 0;
                RETURN(rc);
#else
                ptlrpc_invalidate_import(imp);

                IMPORT_SET_STATE(imp, LUSTRE_IMP_RECOVER);
#endif
        }

        if (imp->imp_state == LUSTRE_IMP_REPLAY) {
                CDEBUG(D_HA, "replay requested by %s\n",
                       imp->imp_target_uuid.uuid);
                rc = ptlrpc_replay_next(imp, &inflight);
                if (inflight == 0 &&
                    atomic_read(&imp->imp_replay_inflight) == 0) {
                        IMPORT_SET_STATE(imp, LUSTRE_IMP_REPLAY_LOCKS);
                        rc = ldlm_replay_locks(imp);
                        if (rc)
                                GOTO(out, rc);
                }
                rc = 0;
        }

        if (imp->imp_state == LUSTRE_IMP_REPLAY_LOCKS) {
                if (atomic_read(&imp->imp_replay_inflight) == 0) {
                        IMPORT_SET_STATE(imp, LUSTRE_IMP_REPLAY_WAIT);
                        rc = signal_completed_replay(imp);
                        if (rc)
                                GOTO(out, rc);
                }

        }

        if (imp->imp_state == LUSTRE_IMP_REPLAY_WAIT) {
                if (atomic_read(&imp->imp_replay_inflight) == 0) {
                        IMPORT_SET_STATE(imp, LUSTRE_IMP_RECOVER);
                }
        }

        if (imp->imp_state == LUSTRE_IMP_RECOVER) {
                char   *nidstr;

                CDEBUG(D_HA, "reconnected to %s@%s\n",
                       imp->imp_target_uuid.uuid,
                       imp->imp_connection->c_remote_uuid.uuid);

                rc = ptlrpc_resend(imp);
                if (rc)
                        GOTO(out, rc);
                IMPORT_SET_STATE(imp, LUSTRE_IMP_FULL);
                ptlrpc_activate_import(imp);

                deuuidify(imp->imp_target_uuid.uuid, NULL,
                          &target_start, &target_len);
                nidstr = libcfs_nid2str(imp->imp_connection->c_peer.nid);

                LCONSOLE_INFO("Connection restored to service %.*s using nid "
                              "%s.\n", target_len, target_start, nidstr);

                CWARN("%s: connection restored to %s@%s\n",
                      imp->imp_obd->obd_name,
                      imp->imp_target_uuid.uuid,
                      imp->imp_connection->c_remote_uuid.uuid);
        }

        if (imp->imp_state == LUSTRE_IMP_FULL) {
                cfs_waitq_signal(&imp->imp_recovery_waitq);
                ptlrpc_wake_delayed(imp);
        }

 out:
        RETURN(rc);
}

static int back_to_sleep(void *unused)
{
        return 0;
}

int ptlrpc_disconnect_import(struct obd_import *imp)
{
        struct ptlrpc_request *request;
        int rq_opc;
        int rc = 0;
        unsigned long flags;
        ENTRY;

        switch (imp->imp_connect_op) {
        case OST_CONNECT: rq_opc = OST_DISCONNECT; break;
        case MDS_CONNECT: rq_opc = MDS_DISCONNECT; break;
        default:
                CERROR("don't know how to disconnect from %s (connect_op %d)\n",
                       imp->imp_target_uuid.uuid, imp->imp_connect_op);
                RETURN(-EINVAL);
        }

        if (ptlrpc_import_in_recovery(imp)) {
                struct l_wait_info lwi;
                lwi = LWI_TIMEOUT_INTR(cfs_timeout_cap(cfs_time_seconds(obd_timeout)), 
                                       back_to_sleep, NULL, NULL);
                rc = l_wait_event(imp->imp_recovery_waitq,
                                  !ptlrpc_import_in_recovery(imp), &lwi);

        }

        spin_lock_irqsave(&imp->imp_lock, flags);
        if (imp->imp_state != LUSTRE_IMP_FULL)
                GOTO(out, 0);

        spin_unlock_irqrestore(&imp->imp_lock, flags);

        request = ptlrpc_prep_req(imp, rq_opc, 0, NULL, NULL);
        if (request) {
                /* We are disconnecting, do not retry a failed DISCONNECT rpc if
                 * it fails.  We can get through the above with a down server
                 * if the client doesn't know the server is gone yet. */
                request->rq_no_resend = 1;
                request->rq_timeout = 5;
                IMPORT_SET_STATE(imp, LUSTRE_IMP_CONNECTING);
                request->rq_send_state =  LUSTRE_IMP_CONNECTING;
                request->rq_replen = lustre_msg_size(0, NULL);
                rc = ptlrpc_queue_wait(request);
                ptlrpc_req_finished(request);
        }

        spin_lock_irqsave(&imp->imp_lock, flags);
out:
        IMPORT_SET_STATE_NOLOCK(imp, LUSTRE_IMP_CLOSED);
        memset(&imp->imp_remote_handle, 0, sizeof(imp->imp_remote_handle));
        spin_unlock_irqrestore(&imp->imp_lock, flags);

        RETURN(rc);
}

