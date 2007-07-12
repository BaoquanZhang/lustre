/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 * 
 * Author: Liang Zhen <liangzhen@clusterfs.com>
 *
 * This file is part of Lustre, http://www.lustre.org
 */
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <errno.h>
#include <pwd.h>
#include <lnet/lnetctl.h>
#include <lnet/lnetst.h>
#include "../selftest/rpc.h"
#include "../selftest/selftest.h"

static int lstjn_stopping = 0;
static int lstjn_intialized = 0;

unsigned int libcfs_subsystem_debug = ~0 - (S_LNET | S_LND);
unsigned int libcfs_debug = 0;

static struct option lstjn_options[] =
{
        {"sesid",   required_argument,  0, 's' },
        {"group",   required_argument,  0, 'g' },
        {0,         0,                  0,  0  }
};

void
lstjn_stop (int sig)
{
        lstjn_stopping = 1;
}

void
lstjn_rpc_done(srpc_client_rpc_t *rpc)
{
        if (!lstjn_intialized)
                lstjn_intialized = 1;
}

int
lstjn_join_session(char *ses, char *grp)
{
        lnet_process_id_t  sesid;
        srpc_client_rpc_t *rpc;
        srpc_join_reqst_t *req;
        srpc_join_reply_t *rep;
        srpc_mksn_reqst_t *sreq;
        srpc_mksn_reply_t *srep;
        int                rc;

        sesid.pid = LUSTRE_LNET_PID;
        sesid.nid = libcfs_str2nid(ses);
        if (sesid.nid == LNET_NID_ANY) {
                fprintf(stderr, "Invalid session NID: %s\n", ses);
                return -1;
        }

        rpc = sfw_create_rpc(sesid, SRPC_SERVICE_JOIN, 0,
                             0, lstjn_rpc_done, NULL);
        if (rpc == NULL) {
                fprintf(stderr, "Out of memory\n");
                return -1;
        }

        req = &rpc->crpc_reqstmsg.msg_body.join_reqst;

        req->join_sid = LST_INVALID_SID;
        strncpy(req->join_group, grp, LST_NAME_SIZE);

        sfw_post_rpc(rpc);

        for (;;) {
                rc = selftest_wait_events();

                if (lstjn_intialized)
                        break;
        }

        if (rpc->crpc_status != 0) {
                fprintf(stderr, "Failed to send RPC to console: %s\n",
                        strerror(rpc->crpc_status));
                srpc_client_rpc_decref(rpc);
                return -1;
        }

        sfw_unpack_message(&rpc->crpc_replymsg);

        rep = &rpc->crpc_replymsg.msg_body.join_reply;
        if (rep->join_status != 0) {
                fprintf(stderr, "Can't join session %s group %s: %s\n",
                        ses, grp, strerror(rep->join_status));
                srpc_client_rpc_decref(rpc);
                return -1;
        }

        sreq = &rpc->crpc_reqstmsg.msg_body.mksn_reqst;
        sreq->mksn_sid     = rep->join_sid;
        sreq->mksn_force   = 0;
        strcpy(sreq->mksn_name, rep->join_session);

        srep = &rpc->crpc_replymsg.msg_body.mksn_reply;

        rc = sfw_make_session(sreq, srep);
        if (rc != 0 || srep->mksn_status != 0) {
                fprintf(stderr, "Can't create session: %d, %s\n",
                        rc, strerror(srep->mksn_status));
                srpc_client_rpc_decref(rpc);
                return -1;
        }

        fprintf(stdout, "Session %s, ID: %s, %Lu\n",
                ses, libcfs_nid2str(rep->join_sid.ses_nid),
                rep->join_sid.ses_stamp);

        srpc_client_rpc_decref(rpc);

        return 0;
}

int
main(int argc, char **argv)
{
        char   *ses = NULL;
        char   *grp = NULL;
        int     optidx;
        int     c;
        int     rc;

        while (1) {
                c = getopt_long(argc, argv, "s:g:",
                                lstjn_options, &optidx);

                if (c == -1)
                        break;

                switch (c) {
                case 's':
                        ses = optarg;
                        break;
                case 'g':
                        grp = optarg;
                        break;
                default:
                        fprintf(stderr,
                                "Usage: lstclient --sesid ID --group GROUP\n");
                        return -1;
                }
        }

        if (optind != argc || grp == NULL || ses == NULL) {
                fprintf(stderr, "Usage: lstclient --sesid ID --group GROUP\n");
                return -1;
        }

        rc = libcfs_debug_init(5 * 1024 * 1024);
        if (rc != 0) {
                CERROR("libcfs_debug_init() failed: %d\n", rc);
                return -1;
        }

        rc = LNetInit();
        if (rc != 0) {
                CERROR("LNetInit() failed: %d\n", rc);
                libcfs_debug_cleanup();
                return -1;
        }

        rc = lnet_selftest_init();
        if (rc != 0) {
                fprintf(stderr, "Can't startup selftest\n");
                LNetFini();
                libcfs_debug_cleanup();

                return -1;
        }
	
        rc = lstjn_join_session(ses, grp);
        if (rc != 0)
                goto out;

        signal(SIGINT, lstjn_stop);

        fprintf(stdout, "Start handling selftest requests, Ctl-C to stop\n");

        while (!lstjn_stopping) {
                selftest_wait_events();

                if (!sfw_session_removed())
                        continue;

                fprintf(stdout, "Session ended\n");
                break;
        }

out:
        lnet_selftest_fini();

        LNetFini();

        libcfs_debug_cleanup();

        return rc;
}
