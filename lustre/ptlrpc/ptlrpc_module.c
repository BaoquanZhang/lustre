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

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_RPC

#ifdef __KERNEL__
# include <linux/module.h>
# include <linux/init.h>
#else
# include <liblustre.h>
#endif

#include <linux/obd_support.h>
#include <linux/obd_class.h>
#include <linux/lustre_net.h>

#include "ptlrpc_internal.h"

extern int ptlrpc_init_portals(void);
extern void ptlrpc_exit_portals(void);

__init int ptlrpc_init(void)
{
        int rc;
        ENTRY;

        lustre_assert_wire_constants();

        rc = ptlrpc_init_portals();
        if (rc)
                RETURN(rc);

        ptlrpc_init_connection();
        llog_init_commit_master();

        ptlrpc_put_connection_superhack = ptlrpc_put_connection;
        ptlrpc_abort_inflight_superhack = ptlrpc_abort_inflight;

        ptlrpc_start_pinger();
        ldlm_init();
        RETURN(0);
}

#ifdef __KERNEL__
static void __exit ptlrpc_exit(void)
{
        ldlm_exit();
        ptlrpc_stop_pinger();
        ptlrpc_exit_portals();
        ptlrpc_cleanup_connection();
        llog_cleanup_commit_master(0);
}

/* connection.c */
EXPORT_SYMBOL(ptlrpc_dump_connections);
EXPORT_SYMBOL(ptlrpc_readdress_connection);
EXPORT_SYMBOL(ptlrpc_get_connection);
EXPORT_SYMBOL(ptlrpc_put_connection);
EXPORT_SYMBOL(ptlrpc_connection_addref);
EXPORT_SYMBOL(ptlrpc_init_connection);
EXPORT_SYMBOL(ptlrpc_cleanup_connection);

/* niobuf.c */
EXPORT_SYMBOL(ptlrpc_start_bulk_transfer);
EXPORT_SYMBOL(ptlrpc_abort_bulk);
EXPORT_SYMBOL(ptlrpc_register_bulk);
EXPORT_SYMBOL(ptlrpc_unregister_bulk);
EXPORT_SYMBOL(ptlrpc_send_reply);
EXPORT_SYMBOL(ptlrpc_reply);
EXPORT_SYMBOL(ptlrpc_error);
EXPORT_SYMBOL(ptlrpc_resend_req);
EXPORT_SYMBOL(ptl_send_rpc);

/* client.c */
EXPORT_SYMBOL(ptlrpc_init_client);
EXPORT_SYMBOL(ptlrpc_cleanup_client);
EXPORT_SYMBOL(ptlrpc_uuid_to_connection);
EXPORT_SYMBOL(ptlrpc_queue_wait);
EXPORT_SYMBOL(ptlrpc_replay_req);
EXPORT_SYMBOL(ptlrpc_restart_req);
EXPORT_SYMBOL(ptlrpc_prep_req);
EXPORT_SYMBOL(ptlrpc_free_req);
EXPORT_SYMBOL(ptlrpc_unregister_reply);
EXPORT_SYMBOL(ptlrpc_req_finished);
EXPORT_SYMBOL(ptlrpc_req_finished_with_imp_lock);
EXPORT_SYMBOL(ptlrpc_request_addref);
EXPORT_SYMBOL(ptlrpc_prep_bulk_imp);
EXPORT_SYMBOL(ptlrpc_prep_bulk_exp);
EXPORT_SYMBOL(ptlrpc_free_bulk);
EXPORT_SYMBOL(ptlrpc_prep_bulk_page);
EXPORT_SYMBOL(ptlrpc_abort_inflight);
EXPORT_SYMBOL(ptlrpc_retain_replayable_request);
EXPORT_SYMBOL(ptlrpc_next_xid);

EXPORT_SYMBOL(ptlrpc_prep_set);
EXPORT_SYMBOL(ptlrpc_set_add_req);
EXPORT_SYMBOL(ptlrpc_set_add_new_req);
EXPORT_SYMBOL(ptlrpc_set_destroy);
EXPORT_SYMBOL(ptlrpc_set_next_timeout);
EXPORT_SYMBOL(ptlrpc_check_set);
EXPORT_SYMBOL(ptlrpc_set_wait);
EXPORT_SYMBOL(ptlrpc_expired_set);
EXPORT_SYMBOL(ptlrpc_interrupted_set);
EXPORT_SYMBOL(ptlrpc_mark_interrupted);

/* service.c */
EXPORT_SYMBOL(ptlrpc_save_lock);
EXPORT_SYMBOL(ptlrpc_schedule_difficult_reply);
EXPORT_SYMBOL(ptlrpc_commit_replies);
EXPORT_SYMBOL(ptlrpc_init_svc);
EXPORT_SYMBOL(ptlrpc_stop_all_threads);
EXPORT_SYMBOL(ptlrpc_start_n_threads);
EXPORT_SYMBOL(ptlrpc_start_thread);
EXPORT_SYMBOL(ptlrpc_unregister_service);
EXPORT_SYMBOL(ptlrpc_daemonize);

/* pack_generic.c */
EXPORT_SYMBOL(lustre_msg_swabbed);
EXPORT_SYMBOL(lustre_pack_request);
EXPORT_SYMBOL(lustre_pack_reply);
EXPORT_SYMBOL(lustre_free_reply_state);
EXPORT_SYMBOL(lustre_msg_size);
EXPORT_SYMBOL(lustre_unpack_msg);
EXPORT_SYMBOL(lustre_msg_buf);
EXPORT_SYMBOL(lustre_msg_string);
EXPORT_SYMBOL(lustre_swab_buf);
EXPORT_SYMBOL(lustre_swab_reqbuf);
EXPORT_SYMBOL(lustre_swab_repbuf);
EXPORT_SYMBOL(lustre_swab_obdo);
EXPORT_SYMBOL(lustre_swab_obd_statfs);
EXPORT_SYMBOL(lustre_swab_obd_ioobj);
EXPORT_SYMBOL(lustre_swab_niobuf_remote);
EXPORT_SYMBOL(lustre_swab_ost_body);
EXPORT_SYMBOL(lustre_swab_ost_last_id);
EXPORT_SYMBOL(lustre_swab_ost_lvb);
EXPORT_SYMBOL(lustre_swab_ll_fid);
EXPORT_SYMBOL(lustre_swab_mds_status_req);
EXPORT_SYMBOL(lustre_swab_mds_body);
EXPORT_SYMBOL(lustre_swab_mds_rec_setattr);
EXPORT_SYMBOL(lustre_swab_mds_rec_create);
EXPORT_SYMBOL(lustre_swab_mds_rec_link);
EXPORT_SYMBOL(lustre_swab_mds_rec_unlink);
EXPORT_SYMBOL(lustre_swab_mds_rec_rename);
EXPORT_SYMBOL(lustre_swab_lov_desc);
EXPORT_SYMBOL(lustre_swab_ldlm_res_id);
EXPORT_SYMBOL(lustre_swab_ldlm_policy_data);
EXPORT_SYMBOL(lustre_swab_ldlm_intent);
EXPORT_SYMBOL(lustre_swab_ldlm_resource_desc);
EXPORT_SYMBOL(lustre_swab_ldlm_lock_desc);
EXPORT_SYMBOL(lustre_swab_ldlm_request);
EXPORT_SYMBOL(lustre_swab_ldlm_reply);
EXPORT_SYMBOL(lustre_swab_ptlbd_op);
EXPORT_SYMBOL(lustre_swab_ptlbd_niob);
EXPORT_SYMBOL(lustre_swab_ptlbd_rsp);

/* recover.c */
EXPORT_SYMBOL(ptlrpc_run_recovery_over_upcall);
EXPORT_SYMBOL(ptlrpc_run_failed_import_upcall);
EXPORT_SYMBOL(ptlrpc_disconnect_import);
EXPORT_SYMBOL(ptlrpc_resend);
EXPORT_SYMBOL(ptlrpc_wake_delayed);
EXPORT_SYMBOL(ptlrpc_set_import_active);
EXPORT_SYMBOL(ptlrpc_deactivate_import);
EXPORT_SYMBOL(ptlrpc_invalidate_import);
EXPORT_SYMBOL(ptlrpc_fail_import);
EXPORT_SYMBOL(ptlrpc_fail_export);
EXPORT_SYMBOL(ptlrpc_recover_import);

/* pinger.c */
EXPORT_SYMBOL(ptlrpc_pinger_add_import);
EXPORT_SYMBOL(ptlrpc_pinger_del_import);
EXPORT_SYMBOL(ptlrpc_pinger_sending_on_import);

/* ptlrpcd.c */
EXPORT_SYMBOL(ptlrpcd_addref);
EXPORT_SYMBOL(ptlrpcd_decref);
EXPORT_SYMBOL(ptlrpcd_add_req);
EXPORT_SYMBOL(ptlrpcd_wake);

/* lproc_ptlrpc.c */
EXPORT_SYMBOL(ptlrpc_lprocfs_register_obd);
EXPORT_SYMBOL(ptlrpc_lprocfs_unregister_obd);

/* llogd.c */
EXPORT_SYMBOL(llog_origin_handle_create);
EXPORT_SYMBOL(llog_origin_handle_next_block);
EXPORT_SYMBOL(llog_origin_handle_read_header);
EXPORT_SYMBOL(llog_origin_handle_close);
EXPORT_SYMBOL(llog_client_ops);
EXPORT_SYMBOL(llog_catinfo);

MODULE_AUTHOR("Cluster File Systems, Inc. <info@clusterfs.com>");
MODULE_DESCRIPTION("Lustre Request Processor and Lock Management");
MODULE_LICENSE("GPL");

module_init(ptlrpc_init);
module_exit(ptlrpc_exit);
#endif
