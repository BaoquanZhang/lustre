/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Lustre Light Super operations
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
 */

#define DEBUG_SUBSYSTEM S_LLITE

#include <linux/module.h>
#include <linux/types.h>
#include <linux/random.h>
#include <linux/version.h>

#include <linux/lustre_lite.h>
#include <linux/lustre_ha.h>
#include <linux/lustre_dlm.h>
#include <linux/lprocfs_status.h>
#include <linux/lustre_smfs.h>
#include <linux/lustre_snap.h>
#include "llite_internal.h"

kmem_cache_t *ll_file_data_slab;

extern struct address_space_operations ll_aops;
extern struct address_space_operations ll_dir_aops;

#ifndef log2
#define log2(n) ffz(~(n))
#endif

struct ll_sb_info *lustre_init_sbi(struct super_block *sb)
{
        struct ll_sb_info *sbi = NULL;
        class_uuid_t uuid;
        ENTRY;

        OBD_ALLOC(sbi, sizeof(*sbi));
        if (!sbi)
                RETURN(NULL);

        spin_lock_init(&sbi->ll_lock);
        INIT_LIST_HEAD(&sbi->ll_pglist);
        sbi->ll_pglist_gen = 0;
        sbi->ll_max_read_ahead_pages = SBI_DEFAULT_RA_MAX;
        INIT_LIST_HEAD(&sbi->ll_conn_chain);
        INIT_HLIST_HEAD(&sbi->ll_orphan_dentry_list);
        INIT_LIST_HEAD(&sbi->ll_mnt_list);
        sema_init(&sbi->ll_gns_sem, 1);
        init_completion(&sbi->ll_gns_completion);
        sbi->ll_gns_state = LL_GNS_STATE_IDLE;
        sbi->ll_gns_timer.data = (unsigned long)sbi;
        sbi->ll_gns_timer.function = ll_gns_timer_callback;
        init_timer(&sbi->ll_gns_timer);
        INIT_LIST_HEAD(&sbi->ll_gns_sbi_head);

        ll_set_sbi(sb, sbi);

        generate_random_uuid(uuid);
        class_uuid_unparse(uuid, &sbi->ll_sb_uuid);
        RETURN(sbi);
}

void lustre_free_sbi(struct super_block *sb)
{
        struct ll_sb_info *sbi = ll_s2sbi(sb);
        ENTRY;

        if (sbi != NULL) {
                list_del(&sbi->ll_gns_sbi_head);
                del_timer(&sbi->ll_gns_timer);
                OBD_FREE(sbi, sizeof(*sbi));
        }
        ll_set_sbi(sb, NULL);
        EXIT;
}

int lustre_init_ea_size(struct ll_sb_info *sbi)
{
        struct lov_desc desc;
        int rc, valsize;
        
        /* OSC may have larger ea size */
        valsize = sizeof(desc);
        rc = obd_get_info(sbi->ll_osc_exp, strlen("lovdesc") + 1, "lovdesc", 
                          &valsize, &desc);
        if (rc)
                RETURN(rc);
        obd_init_ea_size(sbi->ll_mdc_exp, obd_size_diskmd(sbi->ll_osc_exp, NULL),
                         desc.ld_tgt_count*sizeof(struct llog_cookie));

        RETURN(rc);
}

int lustre_common_fill_super(struct super_block *sb, char *mdc, char *osc)
{
        struct inode *root = 0;
        struct ll_sb_info *sbi = ll_s2sbi(sb);
        struct obd_device *obd;
        struct ll_fid rootfid;
        struct obd_statfs osfs;
        struct ptlrpc_request *request = NULL;
        struct lustre_handle osc_conn = {0, };
        struct lustre_handle mdc_conn = {0, };
        struct lustre_md md;
        kdev_t devno;
        int err;

        obd = class_name2obd(mdc);
        if (!obd) {
                CERROR("MDC %s: not setup or attached\n", mdc);
                RETURN(-EINVAL);
        }

        if (proc_lustre_fs_root) {
                err = lprocfs_register_mountpoint(proc_lustre_fs_root, sb,
                                                  osc, mdc);
                if (err < 0)
                        CERROR("could not register mount in /proc/lustre");
        }

        err = obd_connect(&mdc_conn, obd, &sbi->ll_sb_uuid, 0);
        if (err == -EBUSY) {
                CERROR("An MDS (mdc %s) is performing recovery, of which this"
                       " client is not a part.  Please wait for recovery to "
                       "complete, abort, or time out.\n", mdc);
                GOTO(out, err);
        } else if (err) {
                CERROR("cannot connect to %s: rc = %d\n", mdc, err);
                GOTO(out, err);
        }
        sbi->ll_mdc_exp = class_conn2export(&mdc_conn);

        err = obd_statfs(obd, &osfs, jiffies - HZ);
        if (err)
                GOTO(out_mdc, err);

        LASSERT(osfs.os_bsize);
        sb->s_blocksize = osfs.os_bsize;
        sb->s_blocksize_bits = log2(osfs.os_bsize);
        sb->s_magic = LL_SUPER_MAGIC;
        sb->s_maxbytes = PAGE_CACHE_MAXBYTES;
       
        devno = get_uuid2int(sbi->ll_mdc_exp->exp_obd->obd_uuid.uuid, 
                             strlen(sbi->ll_mdc_exp->exp_obd->obd_uuid.uuid));

        sb->s_dev = devno;

        obd = class_name2obd(osc);
        if (!obd) {
                CERROR("OSC %s: not setup or attached\n", osc);
                GOTO(out_mdc, err);
        }

        err = obd_connect(&osc_conn, obd, &sbi->ll_sb_uuid, 0);
        if (err == -EBUSY) {
                CERROR("An OST (osc %s) is performing recovery, of which this"
                       " client is not a part.  Please wait for recovery to "
                       "complete, abort, or time out.\n", osc);
                GOTO(out, err);
        } else if (err) {
                CERROR("cannot connect to %s: rc = %d\n", osc, err);
                GOTO(out_mdc, err);
        }
        sbi->ll_osc_exp = class_conn2export(&osc_conn);

        lustre_init_ea_size(sbi);
        err = md_getstatus(sbi->ll_mdc_exp, &rootfid);
        if (err) {
                CERROR("cannot mds_connect: rc = %d\n", err);
                GOTO(out_osc, err);
        }
        CDEBUG(D_SUPER, "rootfid "LPU64"\n", rootfid.id);
        sbi->ll_rootino = rootfid.id;

        sb->s_op = &lustre_super_operations;

        /* make root inode
         * XXX: move this to after cbd setup? */
        err = md_getattr(sbi->ll_mdc_exp, &rootfid,
                         OBD_MD_FLNOTOBD | OBD_MD_FLBLOCKS, 0, &request);
        if (err) {
                CERROR("md_getattr failed for root: rc = %d\n", err);
                GOTO(out_osc, err);
        }

        err = mdc_req2lustre_md(sbi->ll_mdc_exp, request, 0, 
                                sbi->ll_osc_exp, &md);
        if (err) {
                CERROR("failed to understand root inode md: rc = %d\n",err);
                ptlrpc_req_finished (request);
                GOTO(out_osc, err);
        }

        LASSERT(sbi->ll_rootino != 0);
        root = ll_iget(sb, sbi->ll_rootino, &md);

        ptlrpc_req_finished(request);

        if (root == NULL || is_bad_inode(root)) {
                /* XXX might need iput() for bad inode */
                CERROR("lustre_lite: bad iget4 for root\n");
                GOTO(out_root, err = -EBADF);
        }

        err = ll_close_thread_start(&sbi->ll_lcq);
        if (err) {
                CERROR("cannot start close thread: rc %d\n", err);
                GOTO(out_root, err);
        }

        ll_gns_add_timer(sbi);

        /* making vm readahead 0 for 2.4.x. In the case of 2.6.x,
           backing dev info assigned to inode mapping is used for
           determining maximal readahead. */
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0))
        /* bug 2805 - set VM readahead to zero */
        vm_max_readahead = vm_min_readahead = 0;
#endif

        sb->s_root = d_alloc_root(root);

#ifdef S_PDIROPS
        CWARN("Enabling PDIROPS\n");
        sb->s_flags |= S_PDIROPS;
#endif

        RETURN(err);

out_root:
        if (root)
                iput(root);
out_osc:
        obd_disconnect(sbi->ll_osc_exp, 0);
out_mdc:
        obd_disconnect(sbi->ll_mdc_exp, 0);
out:
        lprocfs_unregister_mountpoint(sbi);
        RETURN(err);
}

void lustre_common_put_super(struct super_block *sb)
{
        struct ll_sb_info *sbi = ll_s2sbi(sb);
        struct hlist_node *tmp, *next;
        ENTRY;

        ll_gns_del_timer(sbi);

        ll_close_thread_shutdown(sbi->ll_lcq);

        list_del(&sbi->ll_conn_chain);
        obd_disconnect(sbi->ll_osc_exp, 0);

        lprocfs_unregister_mountpoint(sbi);
        if (sbi->ll_proc_root) {
                lprocfs_remove(sbi->ll_proc_root);
                sbi->ll_proc_root = NULL;
        }

        obd_disconnect(sbi->ll_mdc_exp, 0);

        // We do this to get rid of orphaned dentries. That is not really trw.
        spin_lock(&dcache_lock);
        hlist_for_each_safe(tmp, next, &sbi->ll_orphan_dentry_list) {
                struct dentry *dentry = hlist_entry(tmp, struct dentry, d_hash);
                CWARN("orphan dentry %*s (%p) at unmount\n",
                      dentry->d_name.len, dentry->d_name.name, dentry);
                shrink_dcache_parent(dentry);
        }
        spin_unlock(&dcache_lock);
        EXIT;
}


char *ll_read_opt(const char *opt, char *data)
{
        char *value;
        char *retval;
        ENTRY;

        CDEBUG(D_SUPER, "option: %s, data %s\n", opt, data);
        if (strncmp(opt, data, strlen(opt)))
                RETURN(NULL);
        if ((value = strchr(data, '=')) == NULL)
                RETURN(NULL);

        value++;
        OBD_ALLOC(retval, strlen(value) + 1);
        if (!retval) {
                CERROR("out of memory!\n");
                RETURN(NULL);
        }

        memcpy(retval, value, strlen(value)+1);
        CDEBUG(D_SUPER, "Assigned option: %s, value %s\n", opt, retval);
        RETURN(retval);
}

int ll_set_opt(const char *opt, char *data, int fl)
{
        ENTRY;

        CDEBUG(D_SUPER, "option: %s, data %s\n", opt, data);
        if (strncmp(opt, data, strlen(opt)))
                RETURN(0);
        else
                RETURN(fl);
}

void ll_options(char *options, char **ost, char **mdc, int *flags, 
                char **clone_opts)
{
        char *this_char;
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,5,0))
        char *opt_ptr = options;
#endif
        ENTRY;

        if (!options) {
                EXIT;
                return;
        }

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,5,0))
        for (this_char = strtok (options, ",");
             this_char != NULL;
             this_char = strtok (NULL, ",")) {
#else
        while ((this_char = strsep (&opt_ptr, ",")) != NULL) {
#endif
                CDEBUG(D_SUPER, "this_char %s\n", this_char);
                if (!*ost && (*ost = ll_read_opt("osc", this_char)))
                        continue;
                if (!*mdc && (*mdc = ll_read_opt("mdc", this_char)))
                        continue;
                if (!*clone_opts && (*clone_opts = ll_read_opt("clone", 
                                                                this_char))) 
                        continue; 
                if (!(*flags & LL_SBI_NOLCK) &&
                    ((*flags) = (*flags) |
                                ll_set_opt("nolock", this_char,
                                           LL_SBI_NOLCK)))
                        continue;
        }
        
        EXIT;
}

void ll_lli_init(struct ll_inode_info *lli)
{
        sema_init(&lli->lli_open_sem, 1);
        lli->lli_flags = 0;
        lli->lli_maxbytes = PAGE_CACHE_MAXBYTES;
        spin_lock_init(&lli->lli_lock);
        INIT_LIST_HEAD(&lli->lli_pending_write_llaps);
        lli->lli_inode_magic = LLI_INODE_MAGIC;
}

int ll_fill_super(struct super_block *sb, void *data, int silent)
{
        struct ll_sb_info *sbi;
        char *osc = NULL;
        char *mdc = NULL;
        char *clone_opts = NULL;
        int err;
        ENTRY;

        CDEBUG(D_VFSTRACE, "VFS Op: sb %p\n", sb);

        sbi = lustre_init_sbi(sb);
        if (!sbi)
                RETURN(-ENOMEM);

        sbi->ll_flags |= LL_SBI_READAHEAD;
        ll_options(data, &osc, &mdc, &sbi->ll_flags, &clone_opts);

        if (!osc) {
                CERROR("no osc\n");
                GOTO(out, err = -EINVAL);
        }

        if (!mdc) {
                CERROR("no mdc\n");
                GOTO(out, err = -EINVAL);
        }

        err = lustre_common_fill_super(sb, mdc, osc);
out:
        if (err)
                lustre_free_sbi(sb);

        if (mdc)
                OBD_FREE(mdc, strlen(mdc) + 1);
        if (osc)
                OBD_FREE(osc, strlen(osc) + 1);
        if (clone_opts)
                OBD_FREE(clone_opts, strlen(clone_opts) + 1);

        RETURN(err);
} /* ll_read_super */

static int lustre_process_log(struct lustre_mount_data *lmd, char *profile,
                              struct config_llog_instance *cfg, int allow_recov)
{
        struct lustre_cfg lcfg;
        struct portals_cfg pcfg;
        char * peer = "MDS_PEER_UUID";
        struct obd_device *obd;
        struct lustre_handle mdc_conn = {0, };
        struct obd_export *exp;
        char * name = "mdc_dev";
        class_uuid_t uuid;
        struct obd_uuid mdc_uuid;
        struct llog_ctxt *ctxt;
        int rc = 0;
        int err;
        ENTRY;

        if (lmd_bad_magic(lmd))
                RETURN(-EINVAL);

        generate_random_uuid(uuid);
        class_uuid_unparse(uuid, &mdc_uuid);

        if (lmd->lmd_local_nid) {
                PCFG_INIT(pcfg, NAL_CMD_REGISTER_MYNID);
                pcfg.pcfg_nal = lmd->lmd_nal;
                pcfg.pcfg_nid = lmd->lmd_local_nid;
                err = libcfs_nal_cmd(&pcfg);
                if (err <0)
                        GOTO(out, err);
        }

        if (lmd->lmd_nal == SOCKNAL) {
                PCFG_INIT(pcfg, NAL_CMD_ADD_AUTOCONN);
                pcfg.pcfg_nal     = lmd->lmd_nal;
                pcfg.pcfg_nid     = lmd->lmd_server_nid;
                pcfg.pcfg_id      = lmd->lmd_server_ipaddr;
                pcfg.pcfg_misc    = lmd->lmd_port;
                pcfg.pcfg_size    = 8388608;
                pcfg.pcfg_flags   = 0x4; /*share*/
                err = libcfs_nal_cmd(&pcfg);
                if (err <0)
                        GOTO(out, err);
        }

        LCFG_INIT(lcfg, LCFG_ADD_UUID, name);
        lcfg.lcfg_nid = lmd->lmd_server_nid;
        lcfg.lcfg_inllen1 = strlen(peer) + 1;
        lcfg.lcfg_inlbuf1 = peer;
        lcfg.lcfg_nal = lmd->lmd_nal;
        err = class_process_config(&lcfg);
        if (err < 0)
                GOTO(out_del_conn, err);

        LCFG_INIT(lcfg, LCFG_ATTACH, name);
        lcfg.lcfg_inlbuf1 = "mdc";
        lcfg.lcfg_inllen1 = strlen(lcfg.lcfg_inlbuf1) + 1;
        lcfg.lcfg_inlbuf2 = mdc_uuid.uuid;
        lcfg.lcfg_inllen2 = strlen(lcfg.lcfg_inlbuf2) + 1;
        err = class_process_config(&lcfg);
        if (err < 0)
                GOTO(out_del_uuid, err);

        LCFG_INIT(lcfg, LCFG_SETUP, name);
        lcfg.lcfg_inlbuf1 = lmd->lmd_mds;
        lcfg.lcfg_inllen1 = strlen(lcfg.lcfg_inlbuf1) + 1;
        lcfg.lcfg_inlbuf2 = peer;
        lcfg.lcfg_inllen2 = strlen(lcfg.lcfg_inlbuf2) + 1;
        err = class_process_config(&lcfg);
        if (err < 0)
                GOTO(out_detach, err);

        obd = class_name2obd(name);
        if (obd == NULL)
                GOTO(out_cleanup, err = -EINVAL);

        /* Disable initial recovery on this import */
        err = obd_set_info(obd->obd_self_export,
                           strlen("initial_recov"), "initial_recov",
                           sizeof(allow_recov), &allow_recov);
        if (err)
                GOTO(out_cleanup, err);

        err = obd_connect(&mdc_conn, obd, &mdc_uuid, 0);
        if (err) {
                CERROR("cannot connect to %s: rc = %d\n", lmd->lmd_mds, err);
                GOTO(out_cleanup, err);
        }

        exp = class_conn2export(&mdc_conn);

        ctxt = llog_get_context(&exp->exp_obd->obd_llogs,LLOG_CONFIG_REPL_CTXT);
        rc = class_config_process_llog(ctxt, profile, cfg);
        if (rc)
                CERROR("class_config_process_llog failed: rc = %d\n", rc);

        err = obd_disconnect(exp, 0);

out_cleanup:
        LCFG_INIT(lcfg, LCFG_CLEANUP, name);
        err = class_process_config(&lcfg);
        if (err < 0)
                GOTO(out, err);

out_detach:
        LCFG_INIT(lcfg, LCFG_DETACH, name);
        err = class_process_config(&lcfg);
        if (err < 0)
                GOTO(out, err);

out_del_uuid:
        LCFG_INIT(lcfg, LCFG_DEL_UUID, name);
        lcfg.lcfg_inllen1 = strlen(peer) + 1;
        lcfg.lcfg_inlbuf1 = peer;
        err = class_process_config(&lcfg);

out_del_conn:
        if (lmd->lmd_nal == SOCKNAL) {
                PCFG_INIT(pcfg, NAL_CMD_DEL_AUTOCONN);
                pcfg.pcfg_nal     = lmd->lmd_nal;
                pcfg.pcfg_nid     = lmd->lmd_server_nid;
                pcfg.pcfg_id      = lmd->lmd_server_ipaddr;
                pcfg.pcfg_flags   = 1; /*share*/
                err = libcfs_nal_cmd(&pcfg);
                if (err <0)
                        GOTO(out, err);
        }
out:
        if (rc == 0)
                rc = err;

        RETURN(rc);
}

int lustre_fill_super(struct super_block *sb, void *data, int silent)
{
        struct lustre_mount_data * lmd = data;
        struct ll_sb_info *sbi;
        char *osc = NULL;
        char *mdc = NULL;
        int err;
        ENTRY;

        CDEBUG(D_VFSTRACE, "VFS Op: sb %p\n", sb);
        if (lmd_bad_magic(lmd))
                RETURN(-EINVAL);

        sbi = lustre_init_sbi(sb);
        if (!sbi)
                RETURN(-ENOMEM);

        sbi->ll_flags |= LL_SBI_READAHEAD;

        if (lmd->lmd_profile) {
                struct lustre_profile *lprof;
                struct config_llog_instance cfg;
                int len;

                if (lmd->lmd_mds[0] == '\0') {
                        CERROR("no mds name\n");
                        GOTO(out_free, err = -EINVAL);
                }

                OBD_ALLOC(sbi->ll_lmd, sizeof(*sbi->ll_lmd));
                if (sbi->ll_lmd == NULL)
                        GOTO(out_free, err = -ENOMEM);
                memcpy(sbi->ll_lmd, lmd, sizeof(*lmd));

                /* generate a string unique to this super, let's try
                 the address of the super itself.*/
                len = (sizeof(sb) * 2) + 1;
                OBD_ALLOC(sbi->ll_instance, len);
                if (sbi->ll_instance == NULL)
                        GOTO(out_free, err = -ENOMEM);
                sprintf(sbi->ll_instance, "%p", sb);

                cfg.cfg_instance = sbi->ll_instance;
                cfg.cfg_uuid = sbi->ll_sb_uuid;
                cfg.cfg_local_nid = lmd->lmd_local_nid;
                err = lustre_process_log(lmd, lmd->lmd_profile, &cfg, 0);
                if (err < 0) {
                        CERROR("Unable to process log: %s\n", lmd->lmd_profile);
                        GOTO(out_free, err);
                }

                lprof = class_get_profile(lmd->lmd_profile);
                if (lprof == NULL) {
                        CERROR("No profile found: %s\n", lmd->lmd_profile);
                        GOTO(out_free, err = -EINVAL);
                }
                if (osc)
                        OBD_FREE(osc, strlen(osc) + 1);
                OBD_ALLOC(osc, strlen(lprof->lp_osc) +
                          strlen(sbi->ll_instance) + 2);
                sprintf(osc, "%s-%s", lprof->lp_osc, sbi->ll_instance);

                if (mdc)
                        OBD_FREE(mdc, strlen(mdc) + 1);
                OBD_ALLOC(mdc, strlen(lprof->lp_mdc) +
                          strlen(sbi->ll_instance) + 2);
                sprintf(mdc, "%s-%s", lprof->lp_mdc, sbi->ll_instance);
        }

        if (!osc) {
                CERROR("no osc\n");
                GOTO(out_free, err = -EINVAL);
        }

        if (!mdc) {
                CERROR("no mdc\n");
                GOTO(out_free, err = -EINVAL);
        }

        err = lustre_common_fill_super(sb, mdc, osc);

        if (err)
                GOTO(out_free, err);
        
out_dev:
        if (mdc)
                OBD_FREE(mdc, strlen(mdc) + 1);
        if (osc)
                OBD_FREE(osc, strlen(osc) + 1);

        RETURN(err);

out_free:
        if (sbi->ll_lmd) {
                int len = strlen(sbi->ll_lmd->lmd_profile) + sizeof("-clean")+1;
                int err;

                if (sbi->ll_instance != NULL) {
                        struct lustre_mount_data *lmd = sbi->ll_lmd;
                        char * cln_prof;
                        struct config_llog_instance cfg;

                        cfg.cfg_instance = sbi->ll_instance;
                        cfg.cfg_uuid = sbi->ll_sb_uuid;

                        OBD_ALLOC(cln_prof, len);
                        sprintf(cln_prof, "%s-clean", lmd->lmd_profile);

                        err = lustre_process_log(lmd, cln_prof, &cfg, 0);
                        if (err < 0)
                                CERROR("Unable to process log: %s\n", cln_prof);
                        OBD_FREE(cln_prof, len);
                        OBD_FREE(sbi->ll_instance, strlen(sbi->ll_instance)+ 1);
                }
                OBD_FREE(sbi->ll_lmd, sizeof(*sbi->ll_lmd));
        }
        lustre_free_sbi(sb);

        goto out_dev;
} /* lustre_fill_super */

static void lustre_manual_cleanup(struct ll_sb_info *sbi)
{
        struct lustre_cfg lcfg;
        struct obd_device *obd;
        int next = 0;

        while ((obd = class_devices_in_group(&sbi->ll_sb_uuid, &next)) != NULL)
        {
                int err;

                LCFG_INIT(lcfg, LCFG_CLEANUP, obd->obd_name);
                err = class_process_config(&lcfg);
                if (err) {
                        CERROR("cleanup failed: %s\n", obd->obd_name);
                        //continue;
                }

                LCFG_INIT(lcfg, LCFG_DETACH, obd->obd_name);
                err = class_process_config(&lcfg);
                if (err) {
                        CERROR("detach failed: %s\n", obd->obd_name);
                        //continue;
                }
        }

        if (sbi->ll_lmd != NULL)
                class_del_profile(sbi->ll_lmd->lmd_profile);
}

void lustre_put_super(struct super_block *sb)
{
        struct obd_device *obd;
        struct ll_sb_info *sbi = ll_s2sbi(sb);
        int force_umount = 0;
        ENTRY;

        CDEBUG(D_VFSTRACE, "VFS Op: sb %p\n", sb);
        obd = class_exp2obd(sbi->ll_mdc_exp);
        if (obd)
                force_umount = obd->obd_no_recov;
        obd = NULL;

        lustre_common_put_super(sb);
        if (sbi->ll_lmd != NULL) {
                char * cln_prof;
                int len = strlen(sbi->ll_lmd->lmd_profile) + sizeof("-clean")+1;
                int err;
                struct config_llog_instance cfg;

                if (force_umount) {
                        CERROR("force umount, doing manual cleanup\n");
                        lustre_manual_cleanup(sbi);
                        GOTO(free_lmd, 0);
                }

                cfg.cfg_instance = sbi->ll_instance;
                cfg.cfg_uuid = sbi->ll_sb_uuid;

                OBD_ALLOC(cln_prof, len);
                sprintf(cln_prof, "%s-clean", sbi->ll_lmd->lmd_profile);

                err = lustre_process_log(sbi->ll_lmd, cln_prof, &cfg, 0);
                if (err < 0) {
                        CERROR("Unable to process log: %s, doing manual cleanup"
                               "\n", cln_prof);
                        lustre_manual_cleanup(sbi);
                }

                OBD_FREE(cln_prof, len);
        free_lmd:
                OBD_FREE(sbi->ll_lmd, sizeof(*sbi->ll_lmd));
                OBD_FREE(sbi->ll_instance, strlen(sbi->ll_instance) + 1);
        }

        lustre_free_sbi(sb);

        EXIT;
} /* lustre_put_super */

int ll_process_config_update(struct ll_sb_info *sbi, int clean)
{
        struct obd_export *mdc_exp = sbi->ll_mdc_exp;
        struct lustre_mount_data *lmd = sbi->ll_lmd;
        struct llog_ctxt *ctxt;
        struct config_llog_instance cfg;
        char *profile = lmd->lmd_profile, *name = NULL;
        int rc, namelen =  0, version;
        ENTRY;

        if (profile == NULL)
                RETURN(0);
        if (lmd == NULL) {
                CERROR("Client not mounted with zero-conf; cannot process "
                       "update log.\n");
                RETURN(0);
        }

        rc = ldlm_cli_cancel_unused(mdc_exp->exp_obd->obd_namespace, NULL,
                                    LDLM_FL_CONFIG_CHANGE, NULL);
        if (rc != 0)
                CWARN("ldlm_cli_cancel_unused(mdc): %d\n", rc);

        rc = obd_cancel_unused(sbi->ll_osc_exp, NULL, LDLM_FL_CONFIG_CHANGE,
                               NULL);
        if (rc != 0)
                CWARN("obd_cancel_unused(lov): %d\n", rc);

        cfg.cfg_instance = sbi->ll_instance;
        cfg.cfg_uuid = sbi->ll_sb_uuid;
        cfg.cfg_local_nid = lmd->lmd_local_nid;

        namelen = strlen(profile) + 20; /* -clean-######### */
        OBD_ALLOC(name, namelen);
        if (name == NULL)
                RETURN(-ENOMEM);

        if (clean) {
                version = sbi->ll_config_version - 1;
                sprintf(name, "%s-clean-%d", profile, version);
        } else {
                version = sbi->ll_config_version + 1;
                sprintf(name, "%s-%d", profile, version);
        }

        CWARN("Applying configuration log %s\n", name);

        ctxt = llog_get_context(&mdc_exp->exp_obd->obd_llogs,
                                LLOG_CONFIG_REPL_CTXT);
        rc = class_config_process_llog(ctxt, name, &cfg);
        if (rc == 0)
                sbi->ll_config_version = version;
        CWARN("Finished applying configuration log %s: %d\n", name, rc);

        if (rc == 0 && clean == 0) {
                struct lov_desc desc;
                int rc, valsize;
                valsize = sizeof(desc);
                rc = obd_get_info(sbi->ll_osc_exp, strlen("lovdesc") + 1,
                                  "lovdesc", &valsize, &desc);

                rc = obd_init_ea_size(mdc_exp,
                                      obd_size_diskmd(sbi->ll_osc_exp, NULL),
                                      (desc.ld_tgt_count *
                                       sizeof(struct llog_cookie)));
        }
        OBD_FREE(name, namelen);
        RETURN(rc);
}

struct inode *ll_inode_from_lock(struct ldlm_lock *lock)
{
        struct inode *inode = NULL;
        l_lock(&lock->l_resource->lr_namespace->ns_lock);
        if (lock->l_ast_data) {
                struct ll_inode_info *lli = ll_i2info(lock->l_ast_data);
                if (lli->lli_inode_magic == LLI_INODE_MAGIC) {
                        inode = igrab(lock->l_ast_data);
                } else {
                        CERROR("DEBUG: l_ast_data %p is bogus: magic %x\n",
                               lock->l_ast_data, lli->lli_inode_magic);
                }
        }
        l_unlock(&lock->l_resource->lr_namespace->ns_lock);
        return inode;
}

int null_if_equal(struct ldlm_lock *lock, void *data)
{
        if (data == lock->l_ast_data) {
                lock->l_ast_data = NULL;

                if (lock->l_req_mode != lock->l_granted_mode)
                        LDLM_ERROR(lock,"clearing inode with ungranted lock\n");
        }

        return LDLM_ITER_CONTINUE;
}

void ll_clear_inode(struct inode *inode)
{
        struct ll_fid fid;
        struct ll_inode_info *lli = ll_i2info(inode);
        struct ll_sb_info *sbi = ll_i2sbi(inode);
        ENTRY;

        CDEBUG(D_VFSTRACE, "VFS Op:inode=%lu/%u(%p)\n", inode->i_ino,
               inode->i_generation, inode);

        lli->lli_inode_magic = LLI_INODE_DEAD;
        ll_inode2fid(&fid, inode);
        clear_bit(LLI_F_HAVE_MDS_SIZE_LOCK, &(ll_i2info(inode)->lli_flags));
        md_change_cbdata(sbi->ll_mdc_exp, &fid, null_if_equal, inode);

        if (lli->lli_smd)
                obd_change_cbdata(sbi->ll_osc_exp, lli->lli_smd,
                                  null_if_equal, inode);

        if (lli->lli_smd) {
                obd_free_memmd(sbi->ll_osc_exp, &lli->lli_smd);
                lli->lli_smd = NULL;
        }

        if (lli->lli_mea) {
                /* FIXME: change cbdata for mea here */
                obd_free_memmd(sbi->ll_mdc_exp,
                               (struct lov_stripe_md **) &lli->lli_mea);
                lli->lli_mea = NULL;
        }

        if (lli->lli_symlink_name) {
                OBD_FREE(lli->lli_symlink_name,
                         strlen(lli->lli_symlink_name) + 1);
                lli->lli_symlink_name = NULL;
        }

        EXIT;
}

/* If this inode has objects allocated to it (lsm != NULL), then the OST
 * object(s) determine the file size and mtime.  Otherwise, the MDS will
 * keep these values until such a time that objects are allocated for it.
 * We do the MDS operations first, as it is checking permissions for us.
 * We don't to the MDS RPC if there is nothing that we want to store there,
 * otherwise there is no harm in updating mtime/atime on the MDS if we are
 * going to do an RPC anyways.
 *
 * If we are doing a truncate, we will send the mtime and ctime updates
 * to the OST with the punch RPC, otherwise we do an explicit setattr RPC.
 * I don't believe it is possible to get e.g. ATTR_MTIME_SET and ATTR_SIZE
 * at the same time.
 */
int ll_setattr_raw(struct inode *inode, struct iattr *attr)
{
        struct lov_stripe_md *lsm = ll_i2info(inode)->lli_smd;
        struct ll_sb_info *sbi = ll_i2sbi(inode);
        struct ptlrpc_request *request = NULL;
        struct mdc_op_data op_data;
        int ia_valid = attr->ia_valid;
        int rc = 0;
        ENTRY;

        CDEBUG(D_VFSTRACE, "VFS Op:inode=%lu\n", inode->i_ino);
        lprocfs_counter_incr(ll_i2sbi(inode)->ll_stats, LPROC_LL_SETATTR);

        if (ia_valid & ATTR_SIZE) {
                if (attr->ia_size > ll_file_maxbytes(inode)) {
                        CDEBUG(D_INODE, "file too large %llu > "LPU64"\n",
                               attr->ia_size, ll_file_maxbytes(inode));
                        RETURN(-EFBIG);
                }

                attr->ia_valid |= ATTR_MTIME | ATTR_CTIME;
        }

        /* POSIX: check before ATTR_*TIME_SET set (from inode_change_ok) */
        if (ia_valid & (ATTR_MTIME_SET | ATTR_ATIME_SET)) {
                if (current->fsuid != inode->i_uid && !capable(CAP_FOWNER))
                        RETURN(-EPERM);
        }

        /* We mark all of the fields "set" so MDS/OST does not re-set them */
        if (attr->ia_valid & ATTR_CTIME) {
                attr->ia_ctime = CURRENT_TIME;
                attr->ia_valid |= ATTR_CTIME_SET;
        }
        if (!(ia_valid & ATTR_ATIME_SET) && (attr->ia_valid & ATTR_ATIME)) {
                attr->ia_atime = CURRENT_TIME;
                attr->ia_valid |= ATTR_ATIME_SET;
        }
        if (!(ia_valid & ATTR_MTIME_SET) && (attr->ia_valid & ATTR_MTIME)) {
                attr->ia_mtime = CURRENT_TIME;
                attr->ia_valid |= ATTR_MTIME_SET;
        }

        if (attr->ia_valid & (ATTR_MTIME | ATTR_CTIME))
                CDEBUG(D_INODE, "setting mtime %lu, ctime %lu, now = %lu\n",
                       LTIME_S(attr->ia_mtime), LTIME_S(attr->ia_ctime),
                       LTIME_S(CURRENT_TIME));
        if (lsm)
                attr->ia_valid &= ~ATTR_SIZE;

        /* If only OST attributes being set on objects, don't do MDS RPC.
         * In that case, we need to check permissions and update the local
         * inode ourselves so we can call obdo_from_inode() always. */
        if (ia_valid & (lsm ? ~(ATTR_SIZE | ATTR_FROM_OPEN | ATTR_RAW) : ~0)) {
                struct lustre_md md;
                ll_prepare_mdc_op_data(&op_data, inode, NULL, NULL, 0, 0);

                rc = md_setattr(sbi->ll_mdc_exp, &op_data,
                                attr, NULL, 0, NULL, 0, &request);
                if (rc) {
                        ptlrpc_req_finished(request);
                        if (rc != -EPERM && rc != -EACCES)
                                CERROR("md_setattr fails: rc = %d\n", rc);
                        RETURN(rc);
                }

                rc = mdc_req2lustre_md(sbi->ll_mdc_exp, request, 0, 
                                       sbi->ll_osc_exp, &md);
                if (rc) {
                        ptlrpc_req_finished(request);
                        RETURN(rc);
                }

                /* Won't invoke vmtruncate as we already cleared ATTR_SIZE,
                 * but needed to set timestamps backwards on utime. */
                inode_setattr(inode, attr);
                ll_update_inode(inode, &md);
                ptlrpc_req_finished(request);

                if (!lsm || !S_ISREG(inode->i_mode)) {
                        CDEBUG(D_INODE, "no lsm: not setting attrs on OST\n");
                        RETURN(0);
                }
        } else {
                /* The OST doesn't check permissions, but the alternative is
                 * a gratuitous RPC to the MDS.  We already rely on the client
                 * to do read/write/truncate permission checks, so is mtime OK?
                 */
                if (ia_valid & (ATTR_MTIME | ATTR_ATIME)) {
                        /* from sys_utime() */
                        if (!(ia_valid & (ATTR_MTIME_SET | ATTR_ATIME_SET))) {
                                if (current->fsuid != inode->i_uid &&
                                    (rc=ll_permission(inode,MAY_WRITE,NULL))!=0)
                                        RETURN(rc);
                        } else {
                                /* from inode_change_ok() */
                                if (current->fsuid != inode->i_uid &&
                                    !capable(CAP_FOWNER))
                                        RETURN(-EPERM);
                        }
                }

                /* Won't invoke vmtruncate, as we already cleared ATTR_SIZE */
                inode_setattr(inode, attr);
        }

        /* We really need to get our PW lock before we change inode->i_size.
         * If we don't we can race with other i_size updaters on our node, like
         * ll_file_read.  We can also race with i_size propogation to other
         * nodes through dirtying and writeback of final cached pages.  This
         * last one is especially bad for racing o_append users on other
         * nodes. */
        if (ia_valid & ATTR_SIZE) {
                ldlm_policy_data_t policy = { .l_extent = {attr->ia_size,
                                                           OBD_OBJECT_EOF } };
                struct lustre_handle lockh = { 0 };
                int err, ast_flags = 0;
                /* XXX when we fix the AST intents to pass the discard-range
                 * XXX extent, make ast_flags always LDLM_AST_DISCARD_DATA
                 * XXX here. */
                if (attr->ia_size == 0)
                        ast_flags = LDLM_AST_DISCARD_DATA;

                /* bug 1639: avoid write/truncate i_sem/DLM deadlock */
                LASSERT(atomic_read(&inode->i_sem.count) <= 0);
                up(&inode->i_sem);
                rc = ll_extent_lock(NULL, inode, lsm, LCK_PW, &policy, &lockh,
                                    ast_flags);
                down(&inode->i_sem);
                if (rc != 0)
                        RETURN(rc);

                rc = vmtruncate(inode, attr->ia_size);

                /* We need to drop the semaphore here, because this unlock may
                 * result in a cancellation, which will need the i_sem */
                up(&inode->i_sem);
                /* unlock now as we don't mind others file lockers racing with
                 * the mds updates below? */
                err = ll_extent_unlock(NULL, inode, lsm, LCK_PW, &lockh);
                down(&inode->i_sem);
                if (err) {
                        CERROR("ll_extent_unlock failed: %d\n", err);
                        if (!rc)
                                rc = err;
                }
        } else if (ia_valid & (ATTR_MTIME | ATTR_MTIME_SET)) {
                struct obdo oa;

                CDEBUG(D_INODE, "set mtime on OST inode %lu to %lu\n",
                       inode->i_ino, LTIME_S(attr->ia_mtime));
                oa.o_id = lsm->lsm_object_id;
                oa.o_gr = lsm->lsm_object_gr;
                oa.o_valid = OBD_MD_FLID | OBD_MD_FLGROUP;
                obdo_from_inode(&oa, inode, OBD_MD_FLTYPE | OBD_MD_FLATIME |
                                            OBD_MD_FLMTIME | OBD_MD_FLCTIME);
                rc = obd_setattr(sbi->ll_osc_exp, &oa, lsm, NULL);
                if (rc)
                        CERROR("obd_setattr fails: rc=%d\n", rc);
        }
        RETURN(rc);
}

int ll_setattr(struct dentry *de, struct iattr *attr)
{
        LBUG(); /* code is unused, but leave this in case of VFS changes */
        RETURN(-ENOSYS);
}

int ll_statfs_internal(struct super_block *sb, struct obd_statfs *osfs,
                       unsigned long max_age)
{
        struct ll_sb_info *sbi = ll_s2sbi(sb);
        struct obd_statfs obd_osfs;
        int rc;
        ENTRY;

        rc = obd_statfs(class_exp2obd(sbi->ll_mdc_exp), osfs, max_age);
        if (rc) {
                CERROR("mdc_statfs fails: rc = %d\n", rc);
                RETURN(rc);
        }

        osfs->os_type = sb->s_magic;

        CDEBUG(D_SUPER, "MDC blocks "LPU64"/"LPU64" objects "LPU64"/"LPU64"\n",
               osfs->os_bavail, osfs->os_blocks, osfs->os_ffree,osfs->os_files);

        rc = obd_statfs(class_exp2obd(sbi->ll_osc_exp), &obd_osfs, max_age);
        if (rc) {
                CERROR("obd_statfs fails: rc = %d\n", rc);
                RETURN(rc);
        }

        CDEBUG(D_SUPER, "OSC blocks "LPU64"/"LPU64" objects "LPU64"/"LPU64"\n",
               obd_osfs.os_bavail, obd_osfs.os_blocks, obd_osfs.os_ffree,
               obd_osfs.os_files);

        osfs->os_blocks = obd_osfs.os_blocks;
        osfs->os_bfree = obd_osfs.os_bfree;
        osfs->os_bavail = obd_osfs.os_bavail;

        /* If we don't have as many objects free on the OST as inodes
         * on the MDS, we reduce the total number of inodes to
         * compensate, so that the "inodes in use" number is correct.
         */
        if (obd_osfs.os_ffree < osfs->os_ffree) {
                osfs->os_files = (osfs->os_files - osfs->os_ffree) +
                        obd_osfs.os_ffree;
                osfs->os_ffree = obd_osfs.os_ffree;
        }

        RETURN(rc);
}

int ll_statfs(struct super_block *sb, struct kstatfs *sfs)
{
        struct obd_statfs osfs;
        int rc;

        CDEBUG(D_VFSTRACE, "VFS Op:\n");
        lprocfs_counter_incr(ll_s2sbi(sb)->ll_stats, LPROC_LL_STAFS);

        /* For now we will always get up-to-date statfs values, but in the
         * future we may allow some amount of caching on the client (e.g.
         * from QOS or lprocfs updates). */
        rc = ll_statfs_internal(sb, &osfs, jiffies - 1);
        if (rc)
                return rc;

        statfs_unpack(sfs, &osfs);

        if (sizeof(sfs->f_blocks) == 4) {
                while (osfs.os_blocks > ~0UL) {
                        sfs->f_bsize <<= 1;

                        osfs.os_blocks >>= 1;
                        osfs.os_bfree >>= 1;
                        osfs.os_bavail >>= 1;
                }
        }

        sfs->f_blocks = osfs.os_blocks;
        sfs->f_bfree = osfs.os_bfree;
        sfs->f_bavail = osfs.os_bavail;

        return 0;
}

void ll_update_inode(struct inode *inode, struct lustre_md *md)
{
        struct ll_inode_info *lli = ll_i2info(inode);
        struct mds_body *body = md->body;
        struct lov_stripe_md *lsm = md->lsm;
        struct mea *mea = md->mea;
        ENTRY;

        LASSERT((lsm != NULL) == ((body->valid & OBD_MD_FLEASIZE) != 0));
        LASSERT((mea != NULL) == ((body->valid & OBD_MD_FLDIREA) != 0));
        if (lsm != NULL) {
                LASSERT(lsm->lsm_object_gr > 0);
                if (lli->lli_smd == NULL) {
                        lli->lli_smd = lsm;
                        lli->lli_maxbytes = lsm->lsm_maxbytes;
                        if (lli->lli_maxbytes > PAGE_CACHE_MAXBYTES)
                                lli->lli_maxbytes = PAGE_CACHE_MAXBYTES;
                } else {
                        int i;
                        if (memcmp(lli->lli_smd, lsm, sizeof(*lsm))) {
                                CERROR("lsm mismatch for inode %ld\n",
                                       inode->i_ino);
                                CERROR("lli_smd:\n");
                                dump_lsm(D_ERROR, lli->lli_smd);
                                CERROR("lsm:\n");
                                dump_lsm(D_ERROR, lsm);
                                LBUG();
                        }
                        /* XXX FIXME -- We should decide on a safer (atomic) and
                         * more elegant way to update the lsm */
                        for (i = 0; i < lsm->lsm_stripe_count; i++) {
                                lli->lli_smd->lsm_oinfo[i].loi_id =
                                        lsm->lsm_oinfo[i].loi_id;
                                lli->lli_smd->lsm_oinfo[i].loi_gr =
                                        lsm->lsm_oinfo[i].loi_gr;
                                lli->lli_smd->lsm_oinfo[i].loi_ost_idx =
                                        lsm->lsm_oinfo[i].loi_ost_idx;
                                lli->lli_smd->lsm_oinfo[i].loi_ost_gen =
                                        lsm->lsm_oinfo[i].loi_ost_gen;
                        }
                }
                /* bug 2844 - limit i_blksize for broken user-space apps */
                LASSERTF(lsm->lsm_xfersize != 0, "%lu\n", lsm->lsm_xfersize);
                inode->i_blksize = min(lsm->lsm_xfersize, LL_MAX_BLKSIZE);
                if (lli->lli_smd != lsm)
                        obd_free_memmd(ll_i2obdexp(inode), &lsm);
        }

        if (mea != NULL) {
                if (lli->lli_mea == NULL) {
                        lli->lli_mea = mea;
                } else {
                        if (memcmp(lli->lli_mea, mea, body->eadatasize)) {
                                CERROR("mea mismatch for inode %lu\n",
                                        inode->i_ino);
                                LBUG();
                        }
                }
                if (lli->lli_mea != mea)
                        obd_free_memmd(ll_i2mdcexp(inode),
                                        (struct lov_stripe_md **) &mea);
                        
        }

        if (body->valid & OBD_MD_FLID)
                inode->i_ino = body->ino;
        if (body->valid & OBD_MD_FLATIME)
                LTIME_S(inode->i_atime) = body->atime;
        if (body->valid & OBD_MD_FLMTIME &&
            body->mtime > LTIME_S(inode->i_mtime)) {
                CDEBUG(D_INODE, "setting ino %lu mtime from %lu to %u\n",
                       inode->i_ino, LTIME_S(inode->i_mtime), body->mtime);
                LTIME_S(inode->i_mtime) = body->mtime;
        }
        if (body->valid & OBD_MD_FLCTIME &&
            body->ctime > LTIME_S(inode->i_ctime))
                LTIME_S(inode->i_ctime) = body->ctime;
        if (body->valid & OBD_MD_FLMODE)
                inode->i_mode = (inode->i_mode & S_IFMT)|(body->mode & ~S_IFMT);
        if (body->valid & OBD_MD_FLTYPE)
                inode->i_mode = (inode->i_mode & ~S_IFMT)|(body->mode & S_IFMT);
        if (body->valid & OBD_MD_FLUID)
                inode->i_uid = body->uid;
        if (body->valid & OBD_MD_FLGID)
                inode->i_gid = body->gid;
        if (body->valid & OBD_MD_FLFLAGS)
                inode->i_flags = body->flags;
        if (body->valid & OBD_MD_FLNLINK)
                inode->i_nlink = body->nlink;
        if (body->valid & OBD_MD_FLGENER)
                inode->i_generation = body->generation;
        if (body->valid & OBD_MD_FLRDEV)
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,5,0))
                inode->i_rdev = body->rdev;
#else
                inode->i_rdev = old_decode_dev(body->rdev);
#endif
        if (body->valid & OBD_MD_FLSIZE)
                inode->i_size = body->size;
        if (body->valid & OBD_MD_FLBLOCKS)
                inode->i_blocks = body->blocks;

        if (body->valid & OBD_MD_FLSIZE)
                set_bit(LLI_F_HAVE_MDS_SIZE_LOCK, &lli->lli_flags);
        
        lli->lli_mds = body->mds;
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,5,0))
        inode->i_dev = (kdev_t) body->mds;
#endif
        LASSERT(body->mds < 1000);
}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,5,0))
static struct backing_dev_info ll_backing_dev_info = {
        .ra_pages       = 0,    /* No readahead */
        .memory_backed  = 0,    /* Does contribute to dirty memory */
};
#endif

void ll_read_inode2(struct inode *inode, void *opaque)
{
        struct lustre_md *md = opaque;
        struct ll_inode_info *lli = ll_i2info(inode);
        ENTRY;

        CDEBUG(D_VFSTRACE, "VFS Op:inode=%lu/%u(%p)\n", inode->i_ino,
               inode->i_generation, inode);

        ll_lli_init(lli);

        LASSERT(!lli->lli_smd);

        /* Core attributes from the MDS first.  This is a new inode, and
         * the VFS doesn't zero times in the core inode so we have to do
         * it ourselves.  They will be overwritten by either MDS or OST
         * attributes - we just need to make sure they aren't newer. */
        LTIME_S(inode->i_mtime) = 0;
        LTIME_S(inode->i_atime) = 0;
        LTIME_S(inode->i_ctime) = 0;

        inode->i_rdev = 0;
        ll_update_inode(inode, md);

        /* OIDEBUG(inode); */

        if (S_ISREG(inode->i_mode)) {
                inode->i_op = &ll_file_inode_operations;
                inode->i_fop = &ll_file_operations;
                inode->i_mapping->a_ops = &ll_aops;
                EXIT;
        } else if (S_ISDIR(inode->i_mode)) {
                inode->i_op = &ll_dir_inode_operations;
                inode->i_fop = &ll_dir_operations;
                inode->i_mapping->a_ops = &ll_dir_aops;
                EXIT;
        } else if (S_ISLNK(inode->i_mode)) {
                inode->i_op = &ll_fast_symlink_inode_operations;
                EXIT;
        } else {
                inode->i_op = &ll_special_inode_operations;

#if (LINUX_VERSION_CODE > KERNEL_VERSION(2,5,0))
                init_special_inode(inode, inode->i_mode,
                                   kdev_t_to_nr(inode->i_rdev));

                /* initializing backing dev info. */
                inode->i_mapping->backing_dev_info = &ll_backing_dev_info;
#else
                init_special_inode(inode, inode->i_mode, inode->i_rdev);
#endif
                lli->ll_save_ifop = inode->i_fop;

                if (S_ISCHR(inode->i_mode))
                        inode->i_fop = &ll_special_chr_inode_fops;
                else if (S_ISBLK(inode->i_mode))
                        inode->i_fop = &ll_special_blk_inode_fops;
                else if (S_ISFIFO(inode->i_mode))
                        inode->i_fop = &ll_special_fifo_inode_fops;
                else if (S_ISSOCK(inode->i_mode))
                        inode->i_fop = &ll_special_sock_inode_fops;

                CWARN("saved %p, replaced with %p\n", lli->ll_save_ifop,
                      inode->i_fop);

                if (lli->ll_save_ifop->owner) {
                        CWARN("%p has owner %p\n", lli->ll_save_ifop,
                              lli->ll_save_ifop->owner);
                }
                EXIT;
        }
}

void ll_delete_inode(struct inode *inode)
{
        int rc;
        struct ll_fid fid;
        struct ll_sb_info *sbi = ll_i2sbi(inode);
        ENTRY;
        
        ll_inode2fid(&fid, inode);

        rc = md_delete_object(sbi->ll_mdc_exp, &fid);
        if (rc) {
                CERROR("md_delete_object() failed, error %d.\n",
                       rc);
        }

        clear_inode(inode);
        EXIT;
}

int ll_iocontrol(struct inode *inode, struct file *file,
                 unsigned int cmd, unsigned long arg)
{
        struct ll_sb_info *sbi = ll_i2sbi(inode);
        struct ptlrpc_request *req = NULL;
        int rc, flags = 0;
        ENTRY;

        switch(cmd) {
        case EXT3_IOC_GETFLAGS: {
                struct ll_fid fid;
                unsigned long valid = OBD_MD_FLFLAGS;
                struct mds_body *body;

                ll_inode2fid(&fid, inode);
                rc = md_getattr(sbi->ll_mdc_exp, &fid, valid, 0, &req);
                if (rc) {
                        CERROR("failure %d inode %lu\n", rc, inode->i_ino);
                        RETURN(-abs(rc));
                }

                body = lustre_msg_buf(req->rq_repmsg, 0, sizeof(*body));

                if (body->flags & S_APPEND)
                        flags |= EXT3_APPEND_FL;
                if (body->flags & S_IMMUTABLE)
                        flags |= EXT3_IMMUTABLE_FL;
                if (body->flags & S_NOATIME)
                        flags |= EXT3_NOATIME_FL;

                ptlrpc_req_finished (req);

                RETURN(put_user(flags, (int *)arg));
        }
        case EXT3_IOC_SETFLAGS: {
                struct mdc_op_data op_data;
                struct iattr attr;
                struct obdo *oa;
                struct lov_stripe_md *lsm = ll_i2info(inode)->lli_smd;

                if (get_user(flags, (int *)arg))
                        RETURN(-EFAULT);

                oa = obdo_alloc();
                if (!oa)
                        RETURN(-ENOMEM);

                ll_prepare_mdc_op_data(&op_data, inode, NULL, NULL, 0, 0);

                memset(&attr, 0x0, sizeof(attr));
                attr.ia_attr_flags = flags;
                attr.ia_valid |= ATTR_ATTR_FLAG;

                rc = md_setattr(sbi->ll_mdc_exp, &op_data,
                                &attr, NULL, 0, NULL, 0, &req);
                if (rc) {
                        ptlrpc_req_finished(req);
                        if (rc != -EPERM && rc != -EACCES)
                                CERROR("md_setattr fails: rc = %d\n", rc);
                        obdo_free(oa);
                        RETURN(rc);
                }
                ptlrpc_req_finished(req);

                oa->o_id = lsm->lsm_object_id;
                oa->o_gr = lsm->lsm_object_gr;
                oa->o_flags = flags;
                oa->o_valid = OBD_MD_FLID | OBD_MD_FLFLAGS | OBD_MD_FLGROUP;

                rc = obd_setattr(sbi->ll_osc_exp, oa, lsm, NULL);
                obdo_free(oa);
                if (rc) {
                        if (rc != -EPERM && rc != -EACCES)
                                CERROR("md_setattr fails: rc = %d\n", rc);
                        RETURN(rc);
                }

                if (flags & EXT3_APPEND_FL)
                        inode->i_flags |= S_APPEND;
                else
                        inode->i_flags &= ~S_APPEND;
                if (flags & EXT3_IMMUTABLE_FL)
                        inode->i_flags |= S_IMMUTABLE;
                else
                        inode->i_flags &= ~S_IMMUTABLE;
                if (flags & EXT3_NOATIME_FL)
                        inode->i_flags |= S_NOATIME;
                else
                        inode->i_flags &= ~S_NOATIME;

                RETURN(0);
        }
        default:
                RETURN(-ENOSYS);
        }

        RETURN(0);
}

void ll_umount_begin(struct super_block *sb)
{
        struct ll_sb_info *sbi = ll_s2sbi(sb);
        struct obd_device *obd;
        struct obd_ioctl_data ioc_data = { 0 };
        ENTRY;
        CDEBUG(D_VFSTRACE, "VFS Op:\n");

        obd = class_exp2obd(sbi->ll_mdc_exp);
        if (obd == NULL) {
                CERROR("Invalid MDC connection handle "LPX64"\n",
                       sbi->ll_mdc_exp->exp_handle.h_cookie);
                EXIT;
                return;
        }
        obd->obd_no_recov = 1;
        obd_iocontrol(IOC_OSC_SET_ACTIVE, sbi->ll_mdc_exp, sizeof ioc_data,
                      &ioc_data, NULL);

        obd = class_exp2obd(sbi->ll_osc_exp);
        if (obd == NULL) {
                CERROR("Invalid LOV connection handle "LPX64"\n",
                       sbi->ll_osc_exp->exp_handle.h_cookie);
                EXIT;
                return;
        }

        obd->obd_no_recov = 1;
        obd_iocontrol(IOC_OSC_SET_ACTIVE, sbi->ll_osc_exp, sizeof ioc_data,
                      &ioc_data, NULL);

        /* Really, we'd like to wait until there are no requests outstanding,
         * and then continue.  For now, we just invalidate the requests,
         * schedule, and hope.
         */
        schedule();

        EXIT;
}

int ll_prep_inode(struct obd_export *osc_exp, struct obd_export *mdc_exp,
                  struct inode **inode, struct ptlrpc_request *req,
                  int offset, struct super_block *sb)
{
        struct lustre_md md;
        int rc = 0;

        rc = mdc_req2lustre_md(mdc_exp, req, offset, osc_exp, &md);
        if (rc)
                RETURN(rc);

        if (*inode) {
                ll_update_inode(*inode, &md);
        } else {
                LASSERT(sb);
                *inode = ll_iget(sb, md.body->ino, &md);
                if (*inode == NULL || is_bad_inode(*inode)) {
                        /* free the lsm if we allocated one above */
                        if (md.lsm != NULL)
                                obd_free_memmd(osc_exp, &md.lsm);
                        if (md.mea != NULL)
                                obd_free_memmd(mdc_exp,
                                               (struct lov_stripe_md**)&md.mea);
                        rc = -ENOMEM;
                        CERROR("new_inode -fatal: rc %d\n", rc);
                }
        }

        RETURN(rc);
}
