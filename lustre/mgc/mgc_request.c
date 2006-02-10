/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  lustre/mgc/mgc_request.c
 *  Lustre Management Client config llog handling
 *
 *  Copyright (C) 2006 Cluster File Systems, Inc.
 *   Author Nathan Rutman <nathan@clusterfs.com>
 *
 *   This file is part of Lustre, http://www.lustre.org
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
 *  For testing and management it is treated as an obd_device,
 *  although * it does not export a full OBD method table (the
 *  requests are coming * in over the wire, so object target modules
 *  do not have a full * method table.)
 */
 
#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_MGC
#define D_MGC D_CONFIG|D_WARNING

#ifdef __KERNEL__
# include <linux/module.h>
# include <linux/pagemap.h>
# include <linux/miscdevice.h>
# include <linux/init.h>
#else
# include <liblustre.h>
#endif

#include <linux/obd_class.h>
#include <linux/lustre_dlm.h>
#include <linux/lustre_log.h>
#include <linux/lustre_fsfilt.h>
#include <linux/lustre_disk.h>
#include <linux/lustre_mgs.h>


int mgc_logname2resid(char *logname, struct ldlm_res_id *res_id)
{
        char *name_end;
        int len;
        __u64 resname = 0;
        
        /* fsname is at most 8 chars long at the beginning of the logname
           e.g. "lustre-MDT0001" or "lustre" */
        name_end = strchr(logname, '-');
        if (name_end)
                len = name_end - logname;
        else
                len = strlen(logname);
        LASSERT(len <= 8);
        memcpy(&resname, logname, len);

        memset(res_id, 0, sizeof(*res_id));
        /* FIXME are resid names swabbed across the wire? */
        res_id->name[0] = cpu_to_le64(resname);
        CDEBUG(D_MGC, "log %s to resid "LPX64"/"LPX64" (%.8s)\n", logname,
               res_id->name[0], res_id->name[1], (char *)&res_id->name[0]);
        return 0;
}
EXPORT_SYMBOL(mgc_logname2resid);

/********************** config llog list **********************/
DECLARE_MUTEX(config_llog_lock);
struct list_head config_llog_list = LIST_HEAD_INIT(config_llog_list);

/* Find log and take the global log sem.  I don't want mutliple processes
   running process_log at once -- sounds like badness.  It actually might be
   fine, as long as we're not trying to update from the same log
   simultaneously (in which case we should use a per-log sem.) */
static struct config_llog_data *config_log_get(char *logname, 
                                               struct config_llog_instance *cfg)
{
        struct list_head *tmp;
        struct config_llog_data *cld;
        int match_instance = 0;

        if (cfg) {
                CDEBUG(D_MGC, "get log %s:%s\n", logname ? logname : "-",
                       cfg->cfg_instance ? cfg->cfg_instance : "-");
                if (cfg->cfg_instance)
                        match_instance++;
        }

        down(&config_llog_lock);
        list_for_each(tmp, &config_llog_list) {
                cld = list_entry(tmp, struct config_llog_data, cld_list_chain);
                if (match_instance && 
                    strcmp(cfg->cfg_instance, cld->cld_cfg.cfg_instance) == 0) 
                        return(cld);
                
                if (!match_instance && 
                    strcmp(logname, cld->cld_logname) == 0) 
                        return(cld);
        }
        up(&config_llog_lock);
        CERROR("can't get log %s\n", logname);
        return(ERR_PTR(-ENOENT));
}

static void config_log_put(void)
{
        up(&config_llog_lock);
}

/* Add this log to our list of active logs. 
   We have one active log per "mount" - client instance or servername.
   Each instance may be at a different point in the log. */
static int config_log_add(char *logname, struct config_llog_instance *cfg,
                          struct super_block *sb)
{
        struct config_llog_data *cld;
        int rc;
        ENTRY;

        CDEBUG(D_MGC, "adding config log %s:%s\n", logname, cfg->cfg_instance);
        
        down(&config_llog_lock);
        OBD_ALLOC(cld, sizeof(*cld));
        if (!cld) 
                GOTO(out, rc = -ENOMEM);
        OBD_ALLOC(cld->cld_logname, strlen(logname) + 1);
        if (!cld->cld_logname) { 
                OBD_FREE(cld, sizeof(*cld));
                GOTO(out, rc = -ENOMEM);
        }
        strcpy(cld->cld_logname, logname);
        cld->cld_sb = sb;
        cld->cld_cfg = *cfg;
        cld->cld_cfg.cfg_last_idx = 0;
        if (cfg->cfg_instance != NULL) {
                OBD_ALLOC(cld->cld_cfg.cfg_instance, 
                          strlen(cfg->cfg_instance) + 1);
                strcpy(cld->cld_cfg.cfg_instance, cfg->cfg_instance);
        }
        mgc_logname2resid(logname, &cld->cld_resid);
        list_add(&cld->cld_list_chain, &config_llog_list);
out:
        up(&config_llog_lock);
        RETURN(rc);
}

/* Stop watching for updates on this log. 2 clients on the same node
   may be at different gens, so we need different log info (eg. 
   already mounted client is at gen 10, but must start a new client
   from gen 0.)*/
static int config_log_end(char *logname, struct config_llog_instance *cfg)
{       
        struct config_llog_data *cld;
        int rc = 0;
        ENTRY;
                                       
        cld = config_log_get(logname, cfg);
        if (IS_ERR(cld)) 
                RETURN(PTR_ERR(cld));

        OBD_FREE(cld->cld_logname, strlen(cld->cld_logname) + 1);
        if (cld->cld_cfg.cfg_instance != NULL)
                OBD_FREE(cld->cld_cfg.cfg_instance, 
                         strlen(cfg->cfg_instance) + 1);

        list_del(&cld->cld_list_chain);
        OBD_FREE(cld, sizeof(*cld));
        config_log_put();
        CDEBUG(D_MGC, "dropped config log %s (%d)\n", logname, rc);
        RETURN(rc);
}

static void config_log_end_all(void)
{
        struct list_head *tmp, *n;
        struct config_llog_data *cld;
        ENTRY;
        
        down(&config_llog_lock);
        list_for_each_safe(tmp, n, &config_llog_list) {
                cld = list_entry(tmp, struct config_llog_data, cld_list_chain);
                CERROR("conflog failsafe %s\n", cld->cld_logname);
                OBD_FREE(cld->cld_logname, strlen(cld->cld_logname) + 1);
                if (cld->cld_cfg.cfg_instance != NULL)
                        OBD_FREE(cld->cld_cfg.cfg_instance, 
                                 strlen(cld->cld_cfg.cfg_instance) + 1);
                list_del(&cld->cld_list_chain);
                OBD_FREE(cld, sizeof(*cld));
        }
        up(&config_llog_lock);
        EXIT;
}


/********************** class fns **********************/

static int mgc_fs_setup(struct obd_device *obd, struct super_block *sb, 
                        struct vfsmount *mnt)
{
        struct lvfs_run_ctxt saved;
        struct lustre_sb_info *lsi = s2lsi(sb);
        struct client_obd *cli = &obd->u.cli;
        struct dentry *dentry;
        int err = 0;
        ENTRY;

        LASSERT(lsi);
        LASSERT(lsi->lsi_srv_mnt == mnt);

        /* The mgc fs exclusion sem. Only one fs can be setup at a time.
           Maybe just overload the cl_sem? */
        down(&cli->cl_mgc_sem);

        obd->obd_fsops = fsfilt_get_ops(MT_STR(lsi->lsi_ldd));
        if (IS_ERR(obd->obd_fsops)) {
                up(&cli->cl_mgc_sem);
                CERROR("No fstype %s rc=%ld\n", MT_STR(lsi->lsi_ldd), 
                       PTR_ERR(obd->obd_fsops));
                RETURN(PTR_ERR(obd->obd_fsops));
        }

        cli->cl_mgc_vfsmnt = mnt;
        // FIXME which is the right SB? - filter_common_setup also 
        CDEBUG(D_MGC, "SB's: fill=%p mnt=%p root=%p\n", sb, mnt->mnt_sb,
               mnt->mnt_root->d_inode->i_sb);
        fsfilt_setup(obd, mnt->mnt_sb);

        OBD_SET_CTXT_MAGIC(&obd->obd_lvfs_ctxt);
        obd->obd_lvfs_ctxt.pwdmnt = mnt;
        obd->obd_lvfs_ctxt.pwd = mnt->mnt_root;
        obd->obd_lvfs_ctxt.fs = get_ds();

        push_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
        dentry = lookup_one_len(MOUNT_CONFIGS_DIR, current->fs->pwd,
                                strlen(MOUNT_CONFIGS_DIR));
        pop_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
        if (IS_ERR(dentry)) {
                err = PTR_ERR(dentry);
                CERROR("cannot lookup %s directory: rc = %d\n", 
                       MOUNT_CONFIGS_DIR, err);
                GOTO(err_ops, err);
        }
        cli->cl_mgc_configs_dir = dentry;

        /* We keep the cl_mgc_sem until mgc_fs_cleanup */
        RETURN(0);

err_ops:        
        fsfilt_put_ops(obd->obd_fsops);
        obd->obd_fsops = NULL;
        cli->cl_mgc_vfsmnt = NULL;
        up(&cli->cl_mgc_sem);
        RETURN(err);
}

static int mgc_fs_cleanup(struct obd_device *obd)
{
        struct client_obd *cli = &obd->u.cli;
        int rc = 0;
        ENTRY;

        LASSERT(cli->cl_mgc_vfsmnt != NULL);

        if (cli->cl_mgc_configs_dir != NULL) {
                struct lvfs_run_ctxt saved;
                push_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
                l_dput(cli->cl_mgc_configs_dir);
                cli->cl_mgc_configs_dir = NULL; 
                pop_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
        }

        cli->cl_mgc_vfsmnt = NULL;
        if (obd->obd_fsops) 
                fsfilt_put_ops(obd->obd_fsops);
        
        up(&cli->cl_mgc_sem);
        RETURN(rc);
}

static int mgc_cleanup(struct obd_device *obd)
{
        struct client_obd *cli = &obd->u.cli;
        int rc;

        /* FIXME calls to mgc_fs_setup must take an obd ref to insure there's
           no fs by the time we get here. */
        LASSERT(cli->cl_mgc_vfsmnt == NULL);
        
        rc = obd_llog_finish(obd, 0);
        if (rc != 0)
                CERROR("failed to cleanup llogging subsystems\n");

        ptlrpcd_decref();

        config_log_end_all();

        return client_obd_cleanup(obd);
}

static struct obd_device *the_mgc;

static int mgc_setup(struct obd_device *obd, obd_count len, void *buf)
{
        int rc;
        ENTRY;

        ptlrpcd_addref();

        rc = client_obd_setup(obd, len, buf);
        if (rc)
                GOTO(err_decref, rc);

        rc = obd_llog_init(obd, obd, 0, NULL);
        if (rc) {
                CERROR("failed to setup llogging subsystems\n");
                GOTO(err_cleanup, rc);
        }

        the_mgc = obd;
        RETURN(rc);

err_cleanup:
        client_obd_cleanup(obd);
err_decref:
        ptlrpcd_decref();
        RETURN(rc);
}

static int mgc_process_log(struct obd_device *mgc, 
                           struct config_llog_data *cld);

/* FIXME I don't want a thread for every cld; make a list of cld's to requeue
   and use only 1 thread. */
/* reenqueue the lock, reparse the log */
static int mgc_async_requeue(void *data)
{
        struct config_llog_data *cld = (struct config_llog_data *)data;
        unsigned long flags;
        int rc;
        ENTRY;

        lock_kernel();
        ptlrpc_daemonize();
        SIGNAL_MASK_LOCK(current, flags);
        sigfillset(&current->blocked);
        RECALC_SIGPENDING;
        SIGNAL_MASK_UNLOCK(current, flags);
        THREAD_NAME(current->comm, sizeof(current->comm) - 1, "reQ %s", 
                    cld->cld_logname);
        unlock_kernel();

        CDEBUG(D_MGC, "requeue "LPX64" %s:%s\n", 
               cld->cld_resid.name[0], cld->cld_logname, 
               cld->cld_cfg.cfg_instance);
        
        LASSERT(the_mgc);
        class_export_get(the_mgc->obd_self_export);
        /* FIXME sleep a few seconds here to allow the server who caused
           the lock revocation to finish its setup */
        
        /* re-send server info every time, in case MGS needs to regen its
           logs */
        server_register_target(cld->cld_sb);
        rc = mgc_process_log(the_mgc, cld);
        class_export_put(the_mgc->obd_self_export);
        
        RETURN(rc);
}

/* based on ll_mdc_blocking_ast */
static int mgc_blocking_ast(struct ldlm_lock *lock, struct ldlm_lock_desc *desc,
                            void *data, int flag)
{
        struct lustre_handle lockh;
        int rc = 0;
        ENTRY;

        switch (flag) {
        case LDLM_CB_BLOCKING:
                /* mgs wants the lock, give it up... */
                LDLM_DEBUG(lock, "MGC blocking CB");
                ldlm_lock2handle(lock, &lockh);
                rc = ldlm_cli_cancel(&lockh);
                break;
        case LDLM_CB_CANCELING: {
                /* We've given up the lock, prepare ourselves to update. */
                LDLM_DEBUG(lock, "MGC cancel CB");
                
                CDEBUG(D_MGC, "Lock res "LPX64" (%.8s)\n",
                       lock->l_resource->lr_name.name[0], 
                       (char *)&lock->l_resource->lr_name.name[0]);

                /* Make sure not to re-enqueue when the mgc is stopping
                   (we get called from client_disconnect_export) */
                if (!lock->l_conn_export ||
                    !lock->l_conn_export->exp_obd->u.cli.cl_conn_count) {
                        CDEBUG(D_MGC, "Disconnecting, don't requeue\n");
                        break;
                }
                if (lock->l_req_mode != lock->l_granted_mode) {
                        CERROR("original grant failed, won't requeue\n");
                        break;
                }

                /* Reenque the lock in a separate thread, because we must
                   return from this fn before that lock can be taken. */
                rc = kernel_thread(mgc_async_requeue, data,
                                   CLONE_VM | CLONE_FS);
                if (rc < 0) 
                        CERROR("Cannot re-enqueue thread: %d\n", rc);
                else 
                        rc = 0;
                break;
        }
        default:
                LBUG();
        }

        if (rc) {
                CERROR("%s CB failed %d:\n", flag == LDLM_CB_BLOCKING ? 
                       "blocking" : "cancel", rc);
                LDLM_ERROR(lock, "MGC ast");
        }
        RETURN(rc);
}

/* based on ll_get_dir_page and osc_enqueue. */
static int mgc_enqueue(struct obd_export *exp, struct lov_stripe_md *lsm,
                       __u32 type, ldlm_policy_data_t *policy, __u32 mode,
                       int *flags, void *bl_cb, void *cp_cb, void *gl_cb,
                       void *data, __u32 lvb_len, void *lvb_swabber,
                       struct lustre_handle *lockh)
{                       
        struct config_llog_data *cld = (struct config_llog_data *)data;
        struct obd_device *obd = class_exp2obd(exp);
        int rc;
        ENTRY;

        CDEBUG(D_MGC, "Enqueue for %s (res "LPX64")\n", cld->cld_logname,
               cld->cld_resid.name[0]);

        /* Search for already existing locks.*/
        rc = ldlm_lock_match(obd->obd_namespace, 0, &cld->cld_resid, type, 
                             NULL, mode, lockh);
        if (rc == 1) 
                RETURN(ELDLM_OK);


        rc = ldlm_cli_enqueue(exp, NULL, obd->obd_namespace, cld->cld_resid,
                              type, NULL, mode, flags, 
                              mgc_blocking_ast, ldlm_completion_ast, NULL,
                              data, NULL, 0, NULL, lockh);
        if (rc == 0) {
                /* Allow matches for other clients mounted on this host */
                struct ldlm_lock *lock = ldlm_handle2lock(lockh);
                LASSERT(lock);
                ldlm_lock_allow_match(lock);
                LDLM_LOCK_PUT(lock);
        }

        RETURN(rc);
}

static int mgc_cancel(struct obd_export *exp, struct lov_stripe_md *md,
                      __u32 mode, struct lustre_handle *lockh)
{
        ENTRY;

        ldlm_lock_decref(lockh, mode);

        RETURN(0);
}

#if 0
static int mgc_iocontrol(unsigned int cmd, struct obd_export *exp, int len,
                         void *karg, void *uarg)
{
        struct obd_device *obd = exp->exp_obd;
        struct obd_ioctl_data *data = karg;
        struct llog_ctxt *ctxt;
        struct lvfs_run_ctxt saved;
        int rc;
        ENTRY;

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,5,0))
        MOD_INC_USE_COUNT;
#else
        if (!try_module_get(THIS_MODULE)) {
                CERROR("Can't get module. Is it alive?");
                return -EINVAL;
        }
#endif
        switch (cmd) {
        /* REPLicator context */
        case OBD_IOC_PARSE: {
                CERROR("MGC parsing llog %s\n", data->ioc_inlbuf1);
                ctxt = llog_get_context(exp->exp_obd, LLOG_CONFIG_REPL_CTXT);
                rc = class_config_parse_llog(ctxt, data->ioc_inlbuf1, NULL);
                GOTO(out, rc);
        }
#ifdef __KERNEL__
        case OBD_IOC_LLOG_INFO:
        case OBD_IOC_LLOG_PRINT: {
                ctxt = llog_get_context(obd, LLOG_CONFIG_REPL_CTXT);
                rc = llog_ioctl(ctxt, cmd, data);

                GOTO(out, rc);
        }
#endif
        /* ORIGinator context */
        case OBD_IOC_DUMP_LOG: {
                ctxt = llog_get_context(obd, LLOG_CONFIG_ORIG_CTXT);
                push_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
                rc = class_config_dump_llog(ctxt, data->ioc_inlbuf1, NULL);
                pop_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
                if (rc)
                        RETURN(rc);

                GOTO(out, rc);
        }
        case OBD_IOC_START: {
                char *name = data->ioc_inlbuf1;
                CERROR("getting config log %s\n", name);
                /* FIXME Get llog from MGS */

                push_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
                ctxt = llog_get_context(obd, LLOG_CONFIG_ORIG_CTXT);
                rc = class_config_parse_llog(ctxt, name, NULL);
                if (rc < 0)
                        CERROR("Unable to process log: %s\n", name);
                pop_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);

                GOTO(out, rc);
        }
        default:
                CERROR("mgc_ioctl(): unrecognised ioctl %#x\n", cmd);
                GOTO(out, rc = -ENOTTY);
        }
out:
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,5,0))
        MOD_DEC_USE_COUNT;
#else
        module_put(THIS_MODULE);
#endif

        return rc;
}
#endif

/* Send target_add message to MGS */
static int mgc_target_add(struct obd_export *exp, struct mgs_target_info *mti)
{
        struct ptlrpc_request *req;
        struct mgs_target_info *req_mti, *rep_mti;
        int size = sizeof(*req_mti);
        int rep_size = sizeof(*mti);
        int rc;
        ENTRY;

        req = ptlrpc_prep_req(class_exp2cliimp(exp), LUSTRE_MGS_VERSION,
                              MGS_TARGET_ADD, 1, &size, NULL);
        if (!req)
                RETURN(rc = -ENOMEM);

        req_mti = lustre_msg_buf(req->rq_reqmsg, 0, sizeof(*req_mti));
        memcpy(req_mti, mti, sizeof(*req_mti));

        req->rq_replen = lustre_msg_size(1, &rep_size);

        CDEBUG(D_MGC, "requesting add for %s\n", mti->mti_svname);
        
        rc = ptlrpc_queue_wait(req);
        if (!rc) {
                rep_mti = lustre_swab_repbuf(req, 0, sizeof(*rep_mti),
                                             lustre_swab_mgs_target_info);
                memcpy(mti, rep_mti, sizeof(*rep_mti));
                CDEBUG(D_MGC, "target_add %s got index = %d\n",
                       mti->mti_svname, mti->mti_stripe_index);
        } else {
                CERROR("target_add failed. rc=%d\n", rc);
        }
        ptlrpc_req_finished(req);

        RETURN(rc);
}

int mgc_set_info(struct obd_export *exp, obd_count keylen,
                 void *key, obd_count vallen, void *val)
{
        struct obd_import *imp = class_exp2cliimp(exp);
        int rc = -EINVAL;
        ENTRY;

        /* Try to "recover" the initial connection; i.e. retry */
        if (KEY_IS(KEY_INIT_RECOV)) {
                if (vallen != sizeof(int))
                        RETURN(-EINVAL);
                imp->imp_initial_recov = *(int *)val;
                CDEBUG(D_HA, "%s: set imp_initial_recov = %d\n",
                       exp->exp_obd->obd_name, imp->imp_initial_recov);
                RETURN(0);
        }
        /* Turn off initial_recov after we try all backup servers once */
        if (KEY_IS(KEY_INIT_RECOV_BACKUP)) {
                if (vallen != sizeof(int))
                        RETURN(-EINVAL);
                imp->imp_initial_recov_bk = *(int *)val;
                CDEBUG(D_HA, "%s: set imp_initial_recov_bk = %d\n",
                       exp->exp_obd->obd_name, imp->imp_initial_recov_bk);
                if (imp->imp_invalid) {
                        /* Resurrect if we previously died */
                        CDEBUG(D_MGC, "Reactivate %s %d:%d:%d\n", 
                               imp->imp_obd->obd_name,
                               imp->imp_deactive, imp->imp_invalid, 
                               imp->imp_state);
                        /* can't put this in obdclass, module loop with ptlrpc*/
                        /* remove 'invalid' flag */
                        ptlrpc_activate_import(imp);
                        /* reconnect */
                        ptlrpc_set_import_active(imp, 1);
                        //ptlrpc_recover_import(imp);
                }
                RETURN(0);
        }
        /* Hack alert */
        if (KEY_IS("add_target")) {
                struct mgs_target_info *mti;
                if (vallen != sizeof(struct mgs_target_info))
                        RETURN(-EINVAL);
                mti = (struct mgs_target_info *)val;
                CDEBUG(D_MGC, "add_target %s %#x\n",
                       mti->mti_svname, mti->mti_flags);
                rc =  mgc_target_add(exp, mti);
                RETURN(rc);
        }
        if (KEY_IS("set_fs")) {
                struct super_block *sb = (struct super_block *)val;
                struct lustre_sb_info *lsi;
                if (vallen != sizeof(struct super_block))
                        RETURN(-EINVAL);
                lsi = s2lsi(sb);
                rc = mgc_fs_setup(exp->exp_obd, sb, lsi->lsi_srv_mnt);
                if (rc) {
                        CERROR("set_fs got %d\n", rc);
                }
                RETURN(rc);
        }
        if (KEY_IS("clear_fs")) {
                if (vallen != 0)
                        RETURN(-EINVAL);
                rc = mgc_fs_cleanup(exp->exp_obd);
                if (rc) {
                        CERROR("clear_fs got %d\n", rc);
                }
                RETURN(rc);
        }

        RETURN(rc);
}               

static int mgc_import_event(struct obd_device *obd,
                            struct obd_import *imp,
                            enum obd_import_event event)
{
        int rc = 0;

        LASSERT(imp->imp_obd == obd);

        switch (event) {
        case IMP_EVENT_INVALIDATE: {
                struct ldlm_namespace *ns = obd->obd_namespace;

                ldlm_namespace_cleanup(ns, LDLM_FL_LOCAL_ONLY);

                break;
        }
        case IMP_EVENT_DISCON: 
        case IMP_EVENT_INACTIVE: 
        case IMP_EVENT_ACTIVE: 
        case IMP_EVENT_OCD:
                break;
        default:
                CERROR("Unknown import event %#x\n", event);
                LBUG();
        }
        RETURN(rc);
}

static int mgc_llog_init(struct obd_device *obd, struct obd_device *tgt,
                         int count, struct llog_catid *logid)
{
        struct llog_ctxt *ctxt;
        int rc;
        ENTRY;

        rc = llog_setup(obd, LLOG_CONFIG_ORIG_CTXT, tgt, 0, NULL,
                        &llog_lvfs_ops);
        if (rc)
                RETURN(rc);

        rc = llog_setup(obd, LLOG_CONFIG_REPL_CTXT, tgt, 0, NULL,
                        &llog_client_ops);
        if (rc == 0) {
                ctxt = llog_get_context(obd, LLOG_CONFIG_REPL_CTXT);
                ctxt->loc_imp = obd->u.cli.cl_import;
        }

        RETURN(rc);
}

static int mgc_llog_finish(struct obd_device *obd, int count)
{
        int rc;
        ENTRY;

        rc = llog_cleanup(llog_get_context(obd, LLOG_CONFIG_REPL_CTXT));
        rc = llog_cleanup(llog_get_context(obd, LLOG_CONFIG_ORIG_CTXT));

        RETURN(rc);
}

/* identical to mgs_log_is_empty */
static int mgc_llog_is_empty(struct obd_device *obd, struct llog_ctxt *ctxt,
                            char *name)
{
        struct lvfs_run_ctxt saved;
        struct llog_handle *llh;
        int rc = 0;

        push_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
        rc = llog_create(ctxt, &llh, NULL, name);
        if (rc == 0) {
                llog_init_handle(llh, LLOG_F_IS_PLAIN, NULL);
                rc = llog_get_size(llh);
                llog_close(llh);
        }
        pop_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
        /* header is record 1 */
        return(rc <= 1);
}

static int mgc_copy_handler(struct llog_handle *llh, struct llog_rec_hdr *rec, 
                            void *data)
{
        struct llog_rec_hdr local_rec = *rec;
        struct llog_handle *local_llh = (struct llog_handle *)data;
        char *cfg_buf = (char*) (rec + 1);
        struct lustre_cfg *lcfg;
        int rc = 0;
        ENTRY;

        lcfg = (struct lustre_cfg *)cfg_buf;

        /* FIXME we should always write to an empty log, so remove this check.*/
        /* append new records */
        if (rec->lrh_index >= llog_get_size(local_llh)) { 
                rc = llog_write_rec(local_llh, &local_rec, NULL, 0, 
                                    (void *)cfg_buf, -1);

                CDEBUG(D_INFO, "idx=%d, rc=%d, len=%d, cmd %x %s %s\n", 
                       rec->lrh_index, rc, rec->lrh_len, lcfg->lcfg_command, 
                       lustre_cfg_string(lcfg, 0), lustre_cfg_string(lcfg, 1));
        } else {
                CDEBUG(D_INFO, "skip idx=%d\n",  rec->lrh_index);
        }

        RETURN(rc);
}

static int mgc_copy_llog(struct obd_device *obd, struct llog_ctxt *rctxt,
                         struct llog_ctxt *lctxt, char *logname)
{
        struct llog_handle *local_llh, *remote_llh;
        struct obd_uuid *uuid;
        int rc, rc2;
        ENTRY;

        /* open local log */
        rc = llog_create(lctxt, &local_llh, NULL, logname);
        if (rc)
                RETURN(rc);
        /* set the log header uuid for fun */
        OBD_ALLOC_PTR(uuid);
        obd_str2uuid(uuid, logname);
        rc = llog_init_handle(local_llh, LLOG_F_IS_PLAIN, uuid);
        OBD_FREE_PTR(uuid);
        if (rc)
                GOTO(out_closel, rc);

        /* FIXME write new log to a temp name, then vfs_rename over logname
           upon successful completion. */

        /* open remote log */
        rc = llog_create(rctxt, &remote_llh, NULL, logname);
        if (rc)
                GOTO(out_closel, rc);
        rc = llog_init_handle(remote_llh, LLOG_F_IS_PLAIN, NULL);
        if (rc)
                GOTO(out_closer, rc);

        rc = llog_process(remote_llh, mgc_copy_handler,(void *)local_llh, NULL);

out_closer:
        rc2 = llog_close(remote_llh);
        if (!rc)
                rc = rc2;
out_closel:
        rc2 = llog_close(local_llh);
        if (!rc)
                rc = rc2;

        CDEBUG(D_MGC, "Copied remote log %s (%d)\n", logname, rc);
        RETURN(rc);
}

/* Get a config log from the MGS and process it.
   This func is called for both clients and servers. */
static int mgc_process_log(struct obd_device *mgc, 
                           struct config_llog_data *cld)
{
        struct llog_ctxt *ctxt, *lctxt;
        struct lustre_handle lockh;
        struct client_obd *cli = &mgc->u.cli;
        struct lvfs_run_ctxt saved;
        struct lustre_sb_info *lsi = s2lsi(cld->cld_sb);
        int rc, rcl, flags = 0, must_pop = 0;
        ENTRY;

        CDEBUG(D_MGC, "Process log %s:%s from %d\n", cld->cld_logname, 
               cld->cld_cfg.cfg_instance, cld->cld_cfg.cfg_last_idx + 1);

        ctxt = llog_get_context(mgc, LLOG_CONFIG_REPL_CTXT);
        if (!ctxt) {
                CERROR("missing llog context\n");
                RETURN(-EINVAL);
        }

        /* Get the cfg lock on the llog */
        rcl = mgc_enqueue(mgc->u.cli.cl_mgc_mgsexp, NULL, LDLM_PLAIN, NULL, 
                          LCK_CR, &flags, NULL, NULL, NULL, 
                          cld, 0, NULL, &lockh);
        if (rcl) 
                CERROR("Can't get cfg lock: %d\n", rcl);
        
        lctxt = llog_get_context(mgc, LLOG_CONFIG_ORIG_CTXT);

        /* Copy the setup log locally if we can. Don't mess around if we're 
           running an MGS though (logs are already local). */
        if (lctxt && lsi && (lsi->lsi_flags & LSI_SERVER) && 
            (lsi->lsi_srv_mnt == cli->cl_mgc_vfsmnt) &&
            !IS_MGS(lsi->lsi_ldd)) {
                push_ctxt(&saved, &mgc->obd_lvfs_ctxt, NULL);
                must_pop++;
                if (rcl == 0) 
                        /* Only try to copy log if we have the lock. */
                        rc = mgc_copy_llog(mgc, ctxt, lctxt, cld->cld_logname);
                if (rcl || rc) {
                        if (mgc_llog_is_empty(mgc, lctxt, cld->cld_logname)) {
                                LCONSOLE_ERROR("Failed to get MGS log %s "
                                               "and no local copy.\n",
                                               cld->cld_logname);
                                GOTO(out_pop, rc = -ENOTCONN);
                        }
                        LCONSOLE_WARN("Failed to get MGS log %s, using "
                                      "local copy.\n", cld->cld_logname);
                }
                /* Now, whether we copied or not, start using the local llog.
                   If we failed to copy, we'll start using whatever the old 
                   log has. */
                ctxt = lctxt;
        }

        /* logname and instance info should be the same, so use our 
           copy of the instance for the update.  The cfg_last_idx will
           be updated here. */
        rc = class_config_parse_llog(ctxt, cld->cld_logname, &cld->cld_cfg);
        
 out_pop:
        if (must_pop) 
                pop_ctxt(&saved, &mgc->obd_lvfs_ctxt, NULL);

        /* Now drop the lock so MGS can revoke it */ 
        if (!rcl) {
                rcl = mgc_cancel(mgc->u.cli.cl_mgc_mgsexp, NULL, 
                                 LCK_CR, &lockh);
                if (rcl) 
                        CERROR("Can't drop cfg lock: %d\n", rcl);
        }
        
        if (rc) {
                CERROR("%s: the configuration '%s' could not be read "
                       "(%d) from the MGS.\n",
                       mgc->obd_name, cld->cld_logname, rc);
        }
        
        RETURN(rc);
}

static int mgc_process_config(struct obd_device *obd, obd_count len, void *buf)
{
        struct lustre_cfg *lcfg = buf;
        int cmd;
        int rc = 0;
        ENTRY;

        switch(cmd = lcfg->lcfg_command) {
        case LCFG_LOV_ADD_OBD: {
                struct mgs_target_info *mti;

                if (LUSTRE_CFG_BUFLEN(lcfg, 1) != 
                    sizeof(struct mgs_target_info))
                        GOTO(out, rc = -EINVAL);

                mti = (struct mgs_target_info *)lustre_cfg_buf(lcfg, 1);
                CDEBUG(D_MGC, "add_target %s %#x\n",    
                       mti->mti_svname, mti->mti_flags);
                rc = mgc_target_add(obd->u.cli.cl_mgc_mgsexp, mti);
                break;
        }
        case LCFG_LOV_DEL_OBD: 
                /* FIXME */
                CERROR("lov_del_obd unimplemented\n");
                rc = -ENOSYS;
                break;
        case LCFG_LOG_START: {
                struct config_llog_data *cld;
                struct config_llog_instance *cfg;
                struct super_block *sb;
                char *logname = lustre_cfg_string(lcfg, 1);
                cfg = (struct config_llog_instance *)lustre_cfg_buf(lcfg, 2);
                sb = *(struct super_block **)lustre_cfg_buf(lcfg, 3);
                
                CDEBUG(D_MGC, "parse_log %s from %d\n", logname, 
                       cfg->cfg_last_idx);

                /* We're only called through here on the initial mount */
                config_log_add(logname, cfg, sb);

                cld = config_log_get(logname, cfg);
                if (IS_ERR(cld)) 
                        rc = PTR_ERR(cld);
                else
                        rc = mgc_process_log(obd, cld);
                config_log_put();
                break;       
        }
        case LCFG_LOG_END: {
                struct config_llog_instance *cfg = NULL;
                char *logname = lustre_cfg_string(lcfg, 1);
                if (lcfg->lcfg_bufcount >= 2)
                        cfg = (struct config_llog_instance *)lustre_cfg_buf(
                                lcfg, 2);
                rc = config_log_end(logname, cfg);
                break;
        }
        default: {
                CERROR("Unknown command: %d\n", lcfg->lcfg_command);
                GOTO(out, rc = -EINVAL);

        }
        }
out:
        RETURN(rc);
}

struct obd_ops mgc_obd_ops = {
        .o_owner        = THIS_MODULE,
        .o_setup        = mgc_setup,
        .o_cleanup      = mgc_cleanup,
        .o_add_conn     = client_import_add_conn,
        .o_del_conn     = client_import_del_conn,
        .o_connect      = client_connect_import,
        .o_disconnect   = client_disconnect_export,
        //.o_enqueue      = mgc_enqueue,
        .o_cancel       = mgc_cancel,
        //.o_iocontrol    = mgc_iocontrol,
        .o_set_info     = mgc_set_info,
        .o_import_event = mgc_import_event,
        .o_llog_init    = mgc_llog_init,
        .o_llog_finish  = mgc_llog_finish,
        .o_process_config = mgc_process_config,
};

int __init mgc_init(void)
{
        return class_register_type(&mgc_obd_ops, NULL, LUSTRE_MGC_NAME);
}

#ifdef __KERNEL__
static void /*__exit*/ mgc_exit(void)
{
        class_unregister_type(LUSTRE_MGC_NAME);
}

MODULE_AUTHOR("Cluster File Systems, Inc. <info@clusterfs.com>");
MODULE_DESCRIPTION("Lustre Management Client");
MODULE_LICENSE("GPL");

module_init(mgc_init);
module_exit(mgc_exit);
#endif
