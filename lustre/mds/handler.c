/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  lustre/mds/handler.c
 *  Lustre Metadata Server (mds) request handler
 *
 *  Copyright (c) 2001-2003 Cluster File Systems, Inc.
 *   Author: Peter Braam <braam@clusterfs.com>
 *   Author: Andreas Dilger <adilger@clusterfs.com>
 *   Author: Phil Schwan <phil@clusterfs.com>
 *   Author: Mike Shaver <shaver@clusterfs.com>
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
#define DEBUG_SUBSYSTEM S_MDS

#include <linux/module.h>
#include <linux/lustre_mds.h>
#include <linux/lustre_dlm.h>
#include <linux/init.h>
#include <linux/obd_class.h>
#include <linux/random.h>
#include <linux/fs.h>
#include <linux/jbd.h>
#include <linux/ext3_fs.h>
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,5,0))
# include <linux/smp_lock.h>
# include <linux/buffer_head.h>
# include <linux/workqueue.h>
# include <linux/mount.h>
#else
# include <linux/locks.h>
#endif
#include <linux/obd_lov.h>
#include <linux/obd_ost.h>
#include <linux/lustre_mds.h>
#include <linux/lustre_fsfilt.h>
#include <linux/lustre_snap.h>
#include <linux/lprocfs_status.h>
#include <linux/lustre_commit_confd.h>

#include "mds_internal.h"

static int mds_intent_policy(struct ldlm_namespace *ns,
                             struct ldlm_lock **lockp, void *req_cookie,
                             ldlm_mode_t mode, int flags, void *data);
static int mds_postsetup(struct obd_device *obd);
static int mds_cleanup(struct obd_device *obd, int flags);

/* Assumes caller has already pushed into the kernel filesystem context */
static int mds_sendpage(struct ptlrpc_request *req, struct file *file,
                        loff_t offset, int count)
{
        struct ptlrpc_bulk_desc *desc;
        struct l_wait_info lwi;
        struct page **pages;
        int rc = 0, npages, i, tmpcount, tmpsize = 0;
        ENTRY;

        LASSERT((offset & (PAGE_SIZE - 1)) == 0); /* I'm dubious about this */

        npages = (count + PAGE_SIZE - 1) >> PAGE_SHIFT;
        OBD_ALLOC(pages, sizeof(*pages) * npages);
        if (!pages)
                GOTO(out, rc = -ENOMEM);

        desc = ptlrpc_prep_bulk_exp(req, npages, BULK_PUT_SOURCE,
                                    MDS_BULK_PORTAL);
        if (desc == NULL)
                GOTO(out_free, rc = -ENOMEM);

        for (i = 0, tmpcount = count; i < npages; i++, tmpcount -= tmpsize) {
                tmpsize = tmpcount > PAGE_SIZE ? PAGE_SIZE : tmpcount;

                pages[i] = alloc_pages(GFP_KERNEL, 0);
                if (pages[i] == NULL)
                        GOTO(cleanup_buf, rc = -ENOMEM);

                ptlrpc_prep_bulk_page(desc, pages[i], 0, tmpsize);
        }

        for (i = 0, tmpcount = count; i < npages; i++, tmpcount -= tmpsize) {
                tmpsize = tmpcount > PAGE_SIZE ? PAGE_SIZE : tmpcount;
                CDEBUG(D_EXT2, "reading %u@%llu from dir %lu (size %llu)\n",
                       tmpsize, offset, file->f_dentry->d_inode->i_ino,
                       file->f_dentry->d_inode->i_size);

                rc = fsfilt_readpage(req->rq_export->exp_obd, file,
                                     kmap(pages[i]), tmpsize, &offset);
                kunmap(pages[i]);

                if (rc != tmpsize)
                        GOTO(cleanup_buf, rc = -EIO);
        }

        LASSERT(desc->bd_nob == count);

        rc = ptlrpc_start_bulk_transfer(desc);
        if (rc)
                GOTO(cleanup_buf, rc);

        if (OBD_FAIL_CHECK(OBD_FAIL_MDS_SENDPAGE)) {
                CERROR("obd_fail_loc=%x, fail operation rc=%d\n",
                       OBD_FAIL_MDS_SENDPAGE, rc);
                GOTO(abort_bulk, rc);
        }

        lwi = LWI_TIMEOUT(obd_timeout * HZ / 4, NULL, NULL);
        rc = l_wait_event(desc->bd_waitq, !ptlrpc_bulk_active(desc), &lwi);
        LASSERT (rc == 0 || rc == -ETIMEDOUT);

        if (rc == 0) {
                if (desc->bd_success &&
                    desc->bd_nob_transferred == count)
                        GOTO(cleanup_buf, rc);

                rc = -ETIMEDOUT; /* XXX should this be a different errno? */
        }

        DEBUG_REQ(D_ERROR, req, "bulk failed: %s %d(%d), evicting %s@%s\n",
                  (rc == -ETIMEDOUT) ? "timeout" : "network error",
                  desc->bd_nob_transferred, count,
                  req->rq_export->exp_client_uuid.uuid,
                  req->rq_export->exp_connection->c_remote_uuid.uuid);

        ptlrpc_fail_export(req->rq_export);

        EXIT;
 abort_bulk:
        ptlrpc_abort_bulk (desc);
 cleanup_buf:
        for (i = 0; i < npages; i++)
                if (pages[i])
                        __free_pages(pages[i], 0);

        ptlrpc_free_bulk(desc);
 out_free:
        OBD_FREE(pages, sizeof(*pages) * npages);
 out:
        return rc;
}

int mds_lock_mode_for_dir(struct obd_device *obd,
                              struct dentry *dentry, int mode)
{
        int ret_mode, split;

        /* any dir access needs couple locks:
         * 1) on part of dir we gonna lookup/modify in
         * 2) on a whole dir to protect it from concurrent splitting
         *    and to flush client's cache for readdir()
         * so, for a given mode and dentry this routine decides what
         * lock mode to use for lock #2:
         * 1) if caller's gonna lookup in dir then we need to protect
         *    dir from being splitted only - LCK_CR
         * 2) if caller's gonna modify dir then we need to protect
         *    dir from being splitted and to flush cache - LCK_CW
         * 3) if caller's gonna modify dir and that dir seems ready
         *    for splitting then we need to protect it from any
         *    type of access (lookup/modify/split) - LCK_EX -bzzz */

        split = mds_splitting_expected(obd, dentry);
        if (split == MDS_NO_SPLITTABLE) {
                /* this inode won't be splitted. so we need not to protect from
                 * just flush client's cache on modification */
                ret_mode = 0;
                if (mode == LCK_PW)
                        ret_mode = LCK_CW;
        } else {
                if (mode == LCK_PR) {
                        ret_mode = LCK_CR;
                } else if (mode == LCK_PW) {
                        /* caller gonna modify directory.we use concurrent
                           write lock here to retract client's cache for readdir */
                        ret_mode = LCK_CW;
                        if (split == MDS_EXPECT_SPLIT) {
                                /* splitting possible. serialize any access
                                 * the idea is that first one seen dir is
                                 * splittable is given exclusive lock and
                                 * split directory. caller passes lock mode
                                 * to mds_try_to_split_dir() and splitting
                                 * would be done with exclusive lock only -bzzz */
                                CDEBUG(D_OTHER, "%s: gonna split %u/%u\n",
                                       obd->obd_name,
                                       (unsigned) dentry->d_inode->i_ino,
                                       (unsigned) dentry->d_inode->i_generation);
                                ret_mode = LCK_EX;
                        }
                } else {
                        CWARN("unexpected lock mode %d\n", mode);
                        ret_mode = LCK_EX;
                }
        }
        return ret_mode;
}

/* only valid locked dentries or errors should be returned */
struct dentry *mds_fid2locked_dentry(struct obd_device *obd, struct ll_fid *fid,
                                     struct vfsmount **mnt, int lock_mode,
                                     struct lustre_handle *lockh, int *mode,
                                     char *name, int namelen, __u64 lockpart)
{
        struct mds_obd *mds = &obd->u.mds;
        struct dentry *de = mds_fid2dentry(mds, fid, mnt), *retval = de;
        struct ldlm_res_id res_id = { .name = {0} };
        int flags = 0, rc;
        ldlm_policy_data_t policy = { .l_inodebits = { lockpart } };

        ENTRY;

        if (IS_ERR(de))
                RETURN(de);

        res_id.name[0] = de->d_inode->i_ino;
        res_id.name[1] = de->d_inode->i_generation;
        lockh[1].cookie = 0;
#ifdef S_PDIROPS
        if (name && IS_PDIROPS(de->d_inode)) {
                ldlm_policy_data_t cpolicy =
                        { .l_inodebits = { MDS_INODELOCK_UPDATE } };
                LASSERT(mode != NULL);
                *mode = mds_lock_mode_for_dir(obd, de, lock_mode);
                if (*mode) {
                        rc = ldlm_cli_enqueue(NULL, NULL, obd->obd_namespace,
                                              res_id, LDLM_IBITS,
                                              &cpolicy, *mode, &flags,
                                              mds_blocking_ast,
                                              ldlm_completion_ast, NULL, NULL,
                                              NULL, 0, NULL, lockh + 1);
                        if (rc != ELDLM_OK) {
                                l_dput(de);
                                RETURN(ERR_PTR(-ENOLCK));
                        }
                }
                flags = 0;

                res_id.name[2] = full_name_hash(name, namelen);

                CDEBUG(D_INFO, "take lock on %lu:%u:"LPX64"\n",
                       de->d_inode->i_ino, de->d_inode->i_generation,
                       res_id.name[2]);
        }
#else
#warning "No PDIROPS support in the kernel"
#endif
        rc = ldlm_cli_enqueue(NULL, NULL, obd->obd_namespace, res_id,
                              LDLM_IBITS, &policy, lock_mode, &flags,
                              mds_blocking_ast, ldlm_completion_ast, NULL, NULL,
                              NULL, 0, NULL, lockh);
        if (rc != ELDLM_OK) {
                l_dput(de);
                retval = ERR_PTR(-EIO); /* XXX translate ldlm code */
#ifdef S_PDIROPS
                if (lockh[1].cookie)
                        ldlm_lock_decref(lockh + 1, LCK_CW);
#endif
        }

        RETURN(retval);
}

#ifndef DCACHE_DISCONNECTED
#define DCACHE_DISCONNECTED DCACHE_NFSD_DISCONNECTED
#endif


/* Look up an entry by inode number. */
/* this function ONLY returns valid dget'd dentries with an initialized inode
   or errors */
struct dentry *mds_fid2dentry(struct mds_obd *mds, struct ll_fid *fid,
                              struct vfsmount **mnt)
{
        char fid_name[32];
        unsigned long ino = fid->id;
        __u32 generation = fid->generation;
        struct inode *inode;
        struct dentry *result;

        if (ino == 0)
                RETURN(ERR_PTR(-ESTALE));

        snprintf(fid_name, sizeof(fid_name), "0x%lx", ino);

        CDEBUG(D_DENTRY, "--> mds_fid2dentry: ino/gen %lu/%u, sb %p\n",
               ino, generation, mds->mds_sb);

        /* under ext3 this is neither supposed to return bad inodes
           nor NULL inodes. */
        result = ll_lookup_one_len(fid_name, mds->mds_fid_de, strlen(fid_name));
        if (IS_ERR(result))
                RETURN(result);

        inode = result->d_inode;
        if (!inode)
                RETURN(ERR_PTR(-ENOENT));

        /* here we disabled generation check, as root inode i_generation
         * of cache mds and real mds are different. */
        if (inode->i_ino != mds->mds_rootfid.id && generation &&
                        inode->i_generation != generation) {
                /* we didn't find the right inode.. */
                CERROR("bad inode %lu, link: %lu ct: %d or generation %u/%u\n",
                       inode->i_ino, (unsigned long)inode->i_nlink,
                       atomic_read(&inode->i_count), inode->i_generation,
                       generation);
                dput(result);
                RETURN(ERR_PTR(-ENOENT));
        }

        if (mnt) {
                *mnt = mds->mds_vfsmnt;
                mntget(*mnt);
        }

        RETURN(result);
}


/* Establish a connection to the MDS.
 *
 * This will set up an export structure for the client to hold state data
 * about that client, like open files, the last operation number it did
 * on the server, etc.
 */
static int mds_connect(struct lustre_handle *conn, struct obd_device *obd,
                       struct obd_uuid *cluuid)
{
        struct obd_export *exp;
        struct mds_export_data *med; /*  */
        struct mds_client_data *mcd;
        int rc;
        ENTRY;

        if (!conn || !obd || !cluuid)
                RETURN(-EINVAL);

        /* XXX There is a small race between checking the list and adding a
         * new connection for the same UUID, but the real threat (list
         * corruption when multiple different clients connect) is solved.
         *
         * There is a second race between adding the export to the list,
         * and filling in the client data below.  Hence skipping the case
         * of NULL mcd above.  We should already be controlling multiple
         * connects at the client, and we can't hold the spinlock over
         * memory allocations without risk of deadlocking.
         */
        rc = class_connect(conn, obd, cluuid);
        if (rc)
                RETURN(rc);
        exp = class_conn2export(conn);
        LASSERT(exp);
        med = &exp->exp_mds_data;

        OBD_ALLOC(mcd, sizeof(*mcd));
        if (!mcd) {
                CERROR("mds: out of memory for client data\n");
                GOTO(out, rc = -ENOMEM);
        }

        memcpy(mcd->mcd_uuid, cluuid, sizeof(mcd->mcd_uuid));
        med->med_mcd = mcd;

        rc = mds_client_add(obd, &obd->u.mds, med, -1);
        if (rc == 0)
                EXIT;
out:
        if (rc) {
                OBD_FREE(mcd, sizeof(*mcd));
                class_disconnect(exp, 0);
        }
        class_export_put(exp);

        return rc;
}

static int mds_init_export(struct obd_export *exp)
{
        struct mds_export_data *med = &exp->exp_mds_data;

        INIT_LIST_HEAD(&med->med_open_head);
        spin_lock_init(&med->med_open_lock);
        RETURN(0);
}

static int mds_destroy_export(struct obd_export *export)
{
        struct mds_export_data *med;
        struct obd_device *obd = export->exp_obd;
        struct lvfs_run_ctxt saved;
        int rc = 0;
        ENTRY;

        med = &export->exp_mds_data;
        target_destroy_export(export);

        if (obd_uuid_equals(&export->exp_client_uuid, &obd->obd_uuid))
                GOTO(out, 0);

        push_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);

        /* Close any open files (which may also cause orphan unlinking). */
        spin_lock(&med->med_open_lock);
        while (!list_empty(&med->med_open_head)) {
                struct list_head *tmp = med->med_open_head.next;
                struct mds_file_data *mfd =
                        list_entry(tmp, struct mds_file_data, mfd_list);
                BDEVNAME_DECLARE_STORAGE(btmp);

                /* bug 1579: fix force-closing for 2.5 */
                struct dentry *dentry = mfd->mfd_dentry;

                list_del(&mfd->mfd_list);
                spin_unlock(&med->med_open_lock);

                /* If you change this message, be sure to update
                 * replay_single:test_46 */
                CERROR("force closing client file handle for %*s (%s:%lu)\n",
                       dentry->d_name.len, dentry->d_name.name,
                       ll_bdevname(dentry->d_inode->i_sb, btmp),
                       dentry->d_inode->i_ino);
                rc = mds_mfd_close(NULL, obd, mfd,
                                   !(export->exp_flags & OBD_OPT_FAILOVER));

                if (rc)
                        CDEBUG(D_INODE, "Error closing file: %d\n", rc);
                spin_lock(&med->med_open_lock);
        }
        spin_unlock(&med->med_open_lock);
        pop_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);

out:
        mds_client_free(export, !(export->exp_flags & OBD_OPT_FAILOVER));

        RETURN(rc);
}

static int mds_disconnect(struct obd_export *exp, int flags)
{
        struct obd_device *obd;
        struct mds_obd *mds;
        unsigned long irqflags;
        int rc;
        ENTRY;

        LASSERT(exp);
        class_export_get(exp);

        obd = class_exp2obd(exp);
        if (obd == NULL) {
                CDEBUG(D_IOCTL, "invalid client cookie "LPX64"\n",
                       exp->exp_handle.h_cookie);
                RETURN(-EINVAL);
        }
        mds = &obd->u.mds;

        if (!(exp->exp_flags & OBD_OPT_REAL_CLIENT)
                        && !atomic_read(&mds->mds_real_clients)) {
                /* there was no client at all */
                mds_lmv_disconnect(obd, flags);
        }

        if ((exp->exp_flags & OBD_OPT_REAL_CLIENT)
                        && atomic_dec_and_test(&mds->mds_real_clients)) {
                /* time to drop LMV connections */
                CDEBUG(D_OTHER, "%s: last real client %s disconnected.  "
                       "Disconnnect from LMV now\n",
                       obd->obd_name, exp->exp_client_uuid.uuid);
                mds_lmv_disconnect(obd, flags);
        }

        spin_lock_irqsave(&exp->exp_lock, irqflags);
        exp->exp_flags = flags;
        spin_unlock_irqrestore(&exp->exp_lock, irqflags);

        /* Disconnect early so that clients can't keep using export */
        rc = class_disconnect(exp, flags);
        ldlm_cancel_locks_for_export(exp);

        /* complete all outstanding replies */
        spin_lock_irqsave(&exp->exp_lock, irqflags);
        while (!list_empty(&exp->exp_outstanding_replies)) {
                struct ptlrpc_reply_state *rs =
                        list_entry(exp->exp_outstanding_replies.next,
                                   struct ptlrpc_reply_state, rs_exp_list);
                struct ptlrpc_service *svc = rs->rs_srv_ni->sni_service;

                spin_lock(&svc->srv_lock);
                list_del_init(&rs->rs_exp_list);
                ptlrpc_schedule_difficult_reply(rs);
                spin_unlock(&svc->srv_lock);
        }
        spin_unlock_irqrestore(&exp->exp_lock, irqflags);

        class_export_put(exp);
        RETURN(rc);
}

static int mds_getstatus(struct ptlrpc_request *req)
{
        struct mds_obd *mds = mds_req2mds(req);
        struct mds_body *body;
        int rc, size = sizeof(*body);
        ENTRY;

        rc = lustre_pack_reply(req, 1, &size, NULL);
        if (rc || OBD_FAIL_CHECK(OBD_FAIL_MDS_GETSTATUS_PACK)) {
                CERROR("mds: out of memory for message: size=%d\n", size);
                req->rq_status = -ENOMEM;       /* superfluous? */
                RETURN(-ENOMEM);
        }

        body = lustre_msg_buf(req->rq_repmsg, 0, sizeof (*body));
        memcpy(&body->fid1, &mds->mds_rootfid, sizeof(body->fid1));

        /* the last_committed and last_xid fields are filled in for all
         * replies already - no need to do so here also.
         */
        RETURN(0);
}

int mds_blocking_ast(struct ldlm_lock *lock, struct ldlm_lock_desc *desc,
                     void *data, int flag)
{
        int do_ast;
        ENTRY;

        if (flag == LDLM_CB_CANCELING) {
                /* Don't need to do anything here. */
                RETURN(0);
        }

        /* XXX layering violation!  -phil */
        l_lock(&lock->l_resource->lr_namespace->ns_lock);
        /* Get this: if mds_blocking_ast is racing with mds_intent_policy,
         * such that mds_blocking_ast is called just before l_i_p takes the
         * ns_lock, then by the time we get the lock, we might not be the
         * correct blocking function anymore.  So check, and return early, if
         * so. */
        if (lock->l_blocking_ast != mds_blocking_ast) {
                l_unlock(&lock->l_resource->lr_namespace->ns_lock);
                RETURN(0);
        }

        lock->l_flags |= LDLM_FL_CBPENDING;
        do_ast = (!lock->l_readers && !lock->l_writers);
        l_unlock(&lock->l_resource->lr_namespace->ns_lock);

        if (do_ast) {
                struct lustre_handle lockh;
                int rc;

                LDLM_DEBUG(lock, "already unused, calling ldlm_cli_cancel");
                ldlm_lock2handle(lock, &lockh);
                rc = ldlm_cli_cancel(&lockh);
                if (rc < 0)
                        CERROR("ldlm_cli_cancel: %d\n", rc);
        } else {
                LDLM_DEBUG(lock, "Lock still has references, will be "
                           "cancelled later");
        }
        RETURN(0);
}

int mds_get_md(struct obd_device *obd, struct inode *inode, void *md,
               int *size, int lock)
{
        int rc = 0;
        int lmm_size;

        if (lock)
                down(&inode->i_sem);
        rc = fsfilt_get_md(obd, inode, md, *size);
        if (lock)
                up(&inode->i_sem);

        if (rc < 0) {
                CERROR("Error %d reading eadata for ino %lu\n",
                       rc, inode->i_ino);
        } else if (rc > 0) {
                lmm_size = rc;
                
                if (S_ISREG(inode->i_mode))
                        rc = mds_convert_lov_ea(obd, inode, md, lmm_size);
                if (S_ISDIR(inode->i_mode))
                        rc = mds_convert_mea_ea(obd, inode, md, lmm_size);

                if (rc == 0) {
                        *size = lmm_size;
                        rc = lmm_size;
                } else if (rc > 0) {
                        *size = rc;
                }
        }

        RETURN (rc);
}


/* Call with lock=1 if you want mds_pack_md to take the i_sem.
 * Call with lock=0 if the caller has already taken the i_sem. */
int mds_pack_md(struct obd_device *obd, struct lustre_msg *msg, int offset,
                struct mds_body *body, struct inode *inode, int lock)
{
        struct mds_obd *mds = &obd->u.mds;
        void *lmm;
        int lmm_size;
        int rc;
        ENTRY;

        lmm = lustre_msg_buf(msg, offset, 0);
        if (lmm == NULL) {
                /* Some problem with getting eadata when I sized the reply
                 * buffer... */
                CDEBUG(D_INFO, "no space reserved for inode %lu MD\n",
                       inode->i_ino);
                RETURN(0);
        }
        lmm_size = msg->buflens[offset];

        /* I don't really like this, but it is a sanity check on the client
         * MD request.  However, if the client doesn't know how much space
         * to reserve for the MD, it shouldn't be bad to have too much space.
         */
        if (lmm_size > mds->mds_max_mdsize) {
                CWARN("Reading MD for inode %lu of %d bytes > max %d\n",
                       inode->i_ino, lmm_size, mds->mds_max_mdsize);
                // RETURN(-EINVAL);
        }

        rc = mds_get_md(obd, inode, lmm, &lmm_size, lock);
        if (rc > 0) {
                if (S_ISDIR(inode->i_mode))
                        body->valid |= OBD_MD_FLDIREA;
                else
                        body->valid |= OBD_MD_FLEASIZE;
                body->eadatasize = lmm_size;
                rc = 0;
        }

        RETURN(rc);
}

static int mds_getattr_internal(struct obd_device *obd, struct dentry *dentry,
                                struct ptlrpc_request *req,
                                struct mds_body *reqbody, int reply_off)
{
        struct mds_body *body;
        struct inode *inode = dentry->d_inode;
        int rc = 0;
        ENTRY;

        if (inode == NULL && !(dentry->d_flags & DCACHE_CROSS_REF))
                RETURN(-ENOENT);

        body = lustre_msg_buf(req->rq_repmsg, reply_off, sizeof(*body));
        LASSERT(body != NULL);                 /* caller prepped reply */

        if (dentry->d_flags & DCACHE_CROSS_REF) {
                CDEBUG(D_OTHER, "cross reference: %lu/%lu/%lu\n",
                       (unsigned long) dentry->d_mdsnum,
                       (unsigned long) dentry->d_inum,
                       (unsigned long) dentry->d_generation);
                body->valid |= OBD_MD_FLID | OBD_MD_MDS;
                body->fid1.id = dentry->d_inum;
                body->fid1.mds = dentry->d_mdsnum;
                body->fid1.generation = dentry->d_generation;
                RETURN(0);
        }
        mds_pack_inode2fid(obd, &body->fid1, inode);
        mds_pack_inode2body(obd, body, inode);

        if ((S_ISREG(inode->i_mode) && (reqbody->valid & OBD_MD_FLEASIZE)) ||
            (S_ISDIR(inode->i_mode) && (reqbody->valid & OBD_MD_FLDIREA))) {
                rc = mds_pack_md(obd, req->rq_repmsg, reply_off + 1, body,
                                 inode, 1);

                /* If we have LOV EA data, the OST holds size, atime, mtime */
                if (!(body->valid & OBD_MD_FLEASIZE) &&
                    !(body->valid & OBD_MD_FLDIREA))
                        body->valid |= (OBD_MD_FLSIZE | OBD_MD_FLBLOCKS |
                                        OBD_MD_FLATIME | OBD_MD_FLMTIME);
        } else if (S_ISLNK(inode->i_mode) &&
                   (reqbody->valid & OBD_MD_LINKNAME) != 0) {
                char *symname = lustre_msg_buf(req->rq_repmsg, reply_off + 1,0);
                int len;

                LASSERT (symname != NULL);       /* caller prepped reply */
                len = req->rq_repmsg->buflens[reply_off + 1];

                rc = inode->i_op->readlink(dentry, symname, len);
                if (rc < 0) {
                        CERROR("readlink failed: %d\n", rc);
                } else if (rc != len - 1) {
                        CERROR ("Unexpected readlink rc %d: expecting %d\n",
                                rc, len - 1);
                        rc = -EINVAL;
                } else {
                        CDEBUG(D_INODE, "read symlink dest %s\n", symname);
                        body->valid |= OBD_MD_LINKNAME;
                        body->eadatasize = rc + 1;
                        symname[rc] = 0;        /* NULL terminate */
                        rc = 0;
                }
        }

        RETURN(rc);
}

static int mds_getattr_pack_msg_cf(struct ptlrpc_request *req,
                                        struct dentry *dentry,
                                        int offset)
{
        int rc = 0, size[1] = {sizeof(struct mds_body)};
        ENTRY;

        if (OBD_FAIL_CHECK(OBD_FAIL_MDS_GETATTR_PACK)) {
                CERROR("failed MDS_GETATTR_PACK test\n");
                req->rq_status = -ENOMEM;
                GOTO(out, rc = -ENOMEM);
        }

        rc = lustre_pack_reply(req, 1, size, NULL);
        if (rc) {
                CERROR("out of memory\n");
                GOTO(out, req->rq_status = rc);
        }

        EXIT;
 out:
        return(rc);
}

static int mds_getattr_pack_msg(struct ptlrpc_request *req, struct inode *inode,
                                int offset)
{
        struct mds_obd *mds = mds_req2mds(req);
        struct mds_body *body;
        int rc = 0, size[2] = {sizeof(*body)}, bufcount = 1;
        ENTRY;

        body = lustre_msg_buf(req->rq_reqmsg, offset, sizeof (*body));
        LASSERT(body != NULL);                 /* checked by caller */
        LASSERT_REQSWABBED(req, offset);       /* swabbed by caller */

        if ((S_ISREG(inode->i_mode) && (body->valid & OBD_MD_FLEASIZE)) ||
            (S_ISDIR(inode->i_mode) && (body->valid & OBD_MD_FLDIREA))) {
                int rc;
                down(&inode->i_sem);
                rc = fsfilt_get_md(req->rq_export->exp_obd, inode, NULL, 0);
                up(&inode->i_sem);
                CDEBUG(D_INODE, "got %d bytes MD data for inode %lu\n",
                       rc, inode->i_ino);
                if (rc < 0) {
                        if (rc != -ENODATA)
                                CERROR("error getting inode %lu MD: rc = %d\n",
                                       inode->i_ino, rc);
                        size[bufcount] = 0;
                } else if (rc > mds->mds_max_mdsize) {
                        size[bufcount] = 0;
                        CERROR("MD size %d larger than maximum possible %u\n",
                               rc, mds->mds_max_mdsize);
                } else {
                        size[bufcount] = rc;
                }
                bufcount++;
        } else if (S_ISLNK(inode->i_mode) && (body->valid & OBD_MD_LINKNAME)) {
                if (inode->i_size + 1 != body->eadatasize)
                        CERROR("symlink size: %Lu, reply space: %d\n",
                               inode->i_size + 1, body->eadatasize);
                size[bufcount] = min_t(int, inode->i_size+1, body->eadatasize);
                bufcount++;
                CDEBUG(D_INODE, "symlink size: %Lu, reply space: %d\n",
                       inode->i_size + 1, body->eadatasize);
        }

        if (OBD_FAIL_CHECK(OBD_FAIL_MDS_GETATTR_PACK)) {
                CERROR("failed MDS_GETATTR_PACK test\n");
                req->rq_status = -ENOMEM;
                GOTO(out, rc = -ENOMEM);
        }

        rc = lustre_pack_reply(req, bufcount, size, NULL);
        if (rc) {
                CERROR("out of memory\n");
                GOTO(out, req->rq_status = rc);
        }

        EXIT;
 out:
        return(rc);
}

int mds_check_mds_num(struct obd_device *obd, struct inode* inode,
                      char *name, int namelen)
{
        struct mea *mea = NULL;
        int mea_size, rc = 0;
        ENTRY;
                                                                                                                                                                                                     
        rc = mds_get_lmv_attr(obd, inode, &mea, &mea_size);
        if (rc)
                RETURN(rc);
        if (mea != NULL) {
                /* dir is already splitted, check if requested filename
                 * should live at this MDS or at another one */
                int i;
                i = mea_name2idx(mea, name, namelen - 1);
                if (mea->mea_master != mea->mea_fids[i].mds) {
                        CDEBUG(D_OTHER,
                               "inapropriate MDS(%d) for %s. should be %d(%d)\n",
                               mea->mea_master, name, mea->mea_fids[i].mds, i);
                        rc = -ERESTART;
                }
        }
                                                                                                                                                                                                     
        if (mea)
                OBD_FREE(mea, mea_size);
        RETURN(rc);
}

static int mds_getattr_name(int offset, struct ptlrpc_request *req,
                            struct lustre_handle *child_lockh, int child_part)
{
        struct obd_device *obd = req->rq_export->exp_obd;
        struct ldlm_reply *rep = NULL;
        struct lvfs_run_ctxt saved;
        struct mds_body *body;
        struct dentry *dparent = NULL, *dchild = NULL;
        struct lvfs_ucred uc;
        struct lustre_handle parent_lockh[2];
        int namesize, update_mode;
        int rc = 0, cleanup_phase = 0, resent_req = 0;
        struct clonefs_info *clone_info = NULL;
        char *name;
        ENTRY;

        LASSERT(!strcmp(obd->obd_type->typ_name, LUSTRE_MDS_NAME));

        /* Swab now, before anyone looks inside the request */

        body = lustre_swab_reqbuf(req, offset, sizeof(*body),
                                  lustre_swab_mds_body);
        if (body == NULL) {
                CERROR("Can't swab mds_body\n");
                GOTO(cleanup, rc = -EFAULT);
        }

        LASSERT_REQSWAB(req, offset + 1);
        name = lustre_msg_string(req->rq_reqmsg, offset + 1, 0);
        if (name == NULL) {
                CERROR("Can't unpack name\n");
                GOTO(cleanup, rc = -EFAULT);
        }
#if CONFIG_SNAPFS
        clone_info = lustre_swab_reqbuf(req, offset + 2, sizeof(*clone_info), 
                                        lustre_swab_clonefs_info);
        if (clone_info) {
                CDEBUG(D_INFO,"getattr name %s clone_info index %d \n", name,
                       clone_info->clone_index);
        }
#endif
        namesize = req->rq_reqmsg->buflens[offset + 1];

        LASSERT (offset == 0 || offset == 2);
        /* if requests were at offset 2, the getattr reply goes back at 1 */
        if (offset) {
                rep = lustre_msg_buf(req->rq_repmsg, 0, sizeof (*rep));
                offset = 1;
        }

        uc.luc_fsuid = body->fsuid;
        uc.luc_fsgid = body->fsgid;
        uc.luc_cap = body->capability;
        uc.luc_suppgid1 = body->suppgid;
        uc.luc_suppgid2 = -1;
        push_ctxt(&saved, &obd->obd_lvfs_ctxt, &uc);
        cleanup_phase = 1; /* kernel context */
        intent_set_disposition(rep, DISP_LOOKUP_EXECD);

        LASSERT(namesize > 0);
        if (namesize == 1) {
                /* we have no dentry here, drop LOOKUP bit */
                child_part &= ~MDS_INODELOCK_LOOKUP;
                CDEBUG(D_OTHER, "%s: request to retrieve attrs for %lu/%lu\n",
                       obd->obd_name, (unsigned long) body->fid1.id,
                       (unsigned long) body->fid1.generation);
                dchild = mds_fid2locked_dentry(obd, &body->fid1, NULL, LCK_PR,
                                               parent_lockh, &update_mode, 
                                               NULL, 0, child_part);
                if (IS_ERR(dchild)) {
                        CERROR("can't find inode: %d\n", (int) PTR_ERR(dchild));
                        GOTO(cleanup, rc = PTR_ERR(dchild));
                }
                memcpy(child_lockh, parent_lockh, sizeof(parent_lockh[0]));
#ifdef S_PDIROPS
                if (parent_lockh[1].cookie)
                        ldlm_lock_decref(parent_lockh + 1, update_mode);
#endif
                cleanup_phase = 2;
                goto fill_inode;
        }
        
#if HAVE_LOOKUP_RAW
        /* FIXME: handle raw lookup */
        if (body->valid == OBD_MD_FLID) {
                struct mds_obd *mds = &obd->u.mds;
                struct mds_body *mds_reply;
                int size = sizeof(*mds_reply);
                struct inode *dir;
                ino_t inum;
                dparent = mds_fid2dentry(mds, &body->fid1, NULL);
                if (IS_ERR(dparent)) {
                        rc = PTR_ERR(dparent);
                        GOTO(cleanup, rc);
                }
                // The user requested ONLY the inode number, so do a raw lookup
                rc = lustre_pack_reply(req, 1, &size, NULL);
                if (rc) {
                        CERROR("out of memory\n");
                        l_dput(dparent);
                        GOTO(cleanup, rc);
                }
                dir  = dparent->d_inode;
                LASSERT(dir->i_op->lookup_raw != NULL);
                rc = dir->i_op->lookup_raw(dir, name, namesize - 1, &inum);
                l_dput(dparent);
                mds_reply = lustre_msg_buf(req->rq_repmsg, offset,
                                           sizeof(*mds_reply));
                mds_reply->fid1.id = inum;
                mds_reply->valid = OBD_MD_FLID;
                GOTO(cleanup, rc);
        }
#endif

        if (child_lockh->cookie != 0) {
                LASSERT(lustre_msg_get_flags(req->rq_reqmsg) & MSG_RESENT);
                resent_req = 1;
        }

        if (resent_req == 0) {
                rc = mds_get_parent_child_locked(obd, &obd->u.mds, &body->fid1,
                                                 parent_lockh, &dparent,
                                                 LCK_PR, MDS_INODELOCK_LOOKUP,
                                                 &update_mode, name, namesize,
                                                 child_lockh, &dchild, LCK_PR,
                                                 child_part, clone_info);
                if (rc)
                        GOTO(cleanup, rc);
        } else {
                struct ldlm_lock *granted_lock;
                struct ll_fid child_fid;
                struct ldlm_resource *res;
                DEBUG_REQ(D_DLMTRACE, req, "resent, not enqueuing new locks");
                granted_lock = ldlm_handle2lock(child_lockh);
                LASSERT(granted_lock);

                res = granted_lock->l_resource;
                child_fid.id = res->lr_name.name[0];
                child_fid.generation = res->lr_name.name[1];
                dchild = mds_fid2dentry(&obd->u.mds, &child_fid, NULL);
                LASSERT(dchild);
                LDLM_LOCK_PUT(granted_lock);
        }

        cleanup_phase = 2; /* dchild, dparent, locks */

        /* let's make sure this name should leave on this mds node */
        rc = mds_check_mds_num(obd, dparent->d_inode, name, namesize);
        if (rc)
                GOTO(cleanup, rc);

fill_inode:

        if (!DENTRY_VALID(dchild)) {
                intent_set_disposition(rep, DISP_LOOKUP_NEG);
                /* in the intent case, the policy clears this error:
                   the disposition is enough */
                rc = -ENOENT;
                GOTO(cleanup, rc);
        } else {
                intent_set_disposition(rep, DISP_LOOKUP_POS);
        }

        if (req->rq_repmsg == NULL) {
                if (dchild->d_flags & DCACHE_CROSS_REF)
                        rc = mds_getattr_pack_msg_cf(req, dchild, offset);
                else
                        rc = mds_getattr_pack_msg(req, dchild->d_inode, offset);
                if (rc != 0) {
                        CERROR ("mds_getattr_pack_msg: %d\n", rc);
                        GOTO (cleanup, rc);
                }
        }

        rc = mds_getattr_internal(obd, dchild, req, body, offset);
        GOTO(cleanup, rc); /* returns the lock to the client */

 cleanup:
        switch (cleanup_phase) {
        case 2:
                if (resent_req == 0) {
                        if (rc && DENTRY_VALID(dchild))
                                ldlm_lock_decref(child_lockh, LCK_PR);
                        if (dparent) {
                                ldlm_lock_decref(parent_lockh, LCK_PR);
#ifdef S_PDIROPS
                                if (parent_lockh[1].cookie != 0)
                                        ldlm_lock_decref(parent_lockh + 1,
                                                         update_mode);
#endif
                        }
                        if (dparent)
                                l_dput(dparent);
                }
                l_dput(dchild);
        case 1:
                pop_ctxt(&saved, &obd->obd_lvfs_ctxt, &uc);
        default: ;
        }
        return rc;
}

static int mds_getattr(int offset, struct ptlrpc_request *req)
{
        struct mds_obd *mds = mds_req2mds(req);
        struct obd_device *obd = req->rq_export->exp_obd;
        struct lvfs_run_ctxt saved;
        struct dentry *de;
        struct mds_body *body;
        struct lvfs_ucred uc;
        int rc = 0;
        ENTRY;

        body = lustre_swab_reqbuf (req, offset, sizeof (*body),
                                   lustre_swab_mds_body);
        if (body == NULL) {
                CERROR ("Can't unpack body\n");
                RETURN (-EFAULT);
        }

        uc.luc_fsuid = body->fsuid;
        uc.luc_fsgid = body->fsgid;
        uc.luc_cap = body->capability;
        push_ctxt(&saved, &obd->obd_lvfs_ctxt, &uc);
        de = mds_fid2dentry(mds, &body->fid1, NULL);
        if (IS_ERR(de)) {
                rc = req->rq_status = -ENOENT;
                GOTO(out_pop, PTR_ERR(de));
        }

        rc = mds_getattr_pack_msg(req, de->d_inode, offset);
        if (rc != 0) {
                CERROR ("mds_getattr_pack_msg: %d\n", rc);
                GOTO (out_pop, rc);
        }

        req->rq_status = mds_getattr_internal(obd, de, req, body, 0);

        l_dput(de);
        GOTO(out_pop, rc);
out_pop:
        pop_ctxt(&saved, &obd->obd_lvfs_ctxt, &uc);
        return rc;
}


static int mds_obd_statfs(struct obd_device *obd, struct obd_statfs *osfs,
                          unsigned long max_age)
{
        int rc;

        spin_lock(&obd->obd_osfs_lock);
        rc = fsfilt_statfs(obd, obd->u.mds.mds_sb, max_age);
        if (rc == 0)
                memcpy(osfs, &obd->obd_osfs, sizeof(*osfs));
        spin_unlock(&obd->obd_osfs_lock);

        return rc;
}

static int mds_statfs(struct ptlrpc_request *req)
{
        struct obd_device *obd = req->rq_export->exp_obd;
        int rc, size = sizeof(struct obd_statfs);
        ENTRY;

        rc = lustre_pack_reply(req, 1, &size, NULL);
        if (rc || OBD_FAIL_CHECK(OBD_FAIL_MDS_STATFS_PACK)) {
                CERROR("mds: statfs lustre_pack_reply failed: rc = %d\n", rc);
                GOTO(out, rc);
        }

        /* We call this so that we can cache a bit - 1 jiffie worth */
        rc = mds_obd_statfs(obd, lustre_msg_buf(req->rq_repmsg, 0, size),
                            jiffies - HZ);
        if (rc) {
                CERROR("mds_obd_statfs failed: rc %d\n", rc);
                GOTO(out, rc);
        }

        EXIT;
out:
        req->rq_status = rc;
        return 0;
}

static int mds_sync(struct ptlrpc_request *req)
{
        struct obd_device *obd = req->rq_export->exp_obd;
        struct mds_obd *mds = &obd->u.mds;
        struct mds_body *body;
        int rc, size = sizeof(*body);
        ENTRY;

        body = lustre_msg_buf(req->rq_reqmsg, 0, sizeof(*body));
        if (body == NULL)
                GOTO(out, rc = -EPROTO);

        rc = lustre_pack_reply(req, 1, &size, NULL);
        if (rc || OBD_FAIL_CHECK(OBD_FAIL_MDS_SYNC_PACK)) {
                CERROR("fsync lustre_pack_reply failed: rc = %d\n", rc);
                GOTO(out, rc);
        }

        if (body->fid1.id == 0) {
                /* a fid of zero is taken to mean "sync whole filesystem" */
                rc = fsfilt_sync(obd, mds->mds_sb);
                if (rc)
                        GOTO(out, rc);
        } else {
                /* just any file to grab fsync method - "file" arg unused */
                struct file *file = mds->mds_rcvd_filp;
                struct dentry *de;

                de = mds_fid2dentry(mds, &body->fid1, NULL);
                if (IS_ERR(de))
                        GOTO(out, rc = PTR_ERR(de));

                rc = file->f_op->fsync(NULL, de, 1);
                l_dput(de);
                if (rc)
                        GOTO(out, rc);

                body = lustre_msg_buf(req->rq_repmsg, 0, sizeof(*body));
                mds_pack_inode2fid(obd, &body->fid1, de->d_inode);
                mds_pack_inode2body(obd, body, de->d_inode);
        }
out:
        req->rq_status = rc;
        return 0;
}

/* mds_readpage does not take a DLM lock on the inode, because the client must
 * already have a PR lock.
 *
 * If we were to take another one here, a deadlock will result, if another
 * thread is already waiting for a PW lock. */
static int mds_readpage(struct ptlrpc_request *req)
{
        struct obd_device *obd = req->rq_export->exp_obd;
        struct vfsmount *mnt;
        struct dentry *de;
        struct file *file;
        struct mds_body *body, *repbody;
        struct lvfs_run_ctxt saved;
        int rc, size = sizeof(*repbody);
        struct lvfs_ucred uc;
        ENTRY;

        rc = lustre_pack_reply(req, 1, &size, NULL);
        if (rc || OBD_FAIL_CHECK(OBD_FAIL_MDS_READPAGE_PACK)) {
                CERROR("mds: out of memory\n");
                GOTO(out, rc = -ENOMEM);
        }

        body = lustre_swab_reqbuf(req, 0, sizeof(*body), lustre_swab_mds_body);
        if (body == NULL)
                GOTO (out, rc = -EFAULT);

        uc.luc_fsuid = body->fsuid;
        uc.luc_fsgid = body->fsgid;
        uc.luc_cap = body->capability;
        push_ctxt(&saved, &obd->obd_lvfs_ctxt, &uc);
        de = mds_fid2dentry(&obd->u.mds, &body->fid1, &mnt);
        if (IS_ERR(de))
                GOTO(out_pop, rc = PTR_ERR(de));

        CDEBUG(D_INODE, "ino %lu\n", de->d_inode->i_ino);

        file = dentry_open(de, mnt, O_RDONLY | O_LARGEFILE);
        /* note: in case of an error, dentry_open puts dentry */
        if (IS_ERR(file))
                GOTO(out_pop, rc = PTR_ERR(file));

        /* body->size is actually the offset -eeb */
        if ((body->size & (de->d_inode->i_blksize - 1)) != 0) {
                CERROR("offset "LPU64" not on a block boundary of %lu\n",
                       body->size, de->d_inode->i_blksize);
                GOTO(out_file, rc = -EFAULT);
        }

        /* body->nlink is actually the #bytes to read -eeb */
        if (body->nlink & (de->d_inode->i_blksize - 1)) {
                CERROR("size %u is not multiple of blocksize %lu\n",
                       body->nlink, de->d_inode->i_blksize);
                GOTO(out_file, rc = -EFAULT);
        }

        repbody = lustre_msg_buf(req->rq_repmsg, 0, sizeof (*repbody));
        repbody->size = file->f_dentry->d_inode->i_size;
        repbody->valid = OBD_MD_FLSIZE;

        /* to make this asynchronous make sure that the handling function
           doesn't send a reply when this function completes. Instead a
           callback function would send the reply */
        /* body->size is actually the offset -eeb */
        rc = mds_sendpage(req, file, body->size, body->nlink);

out_file:
        filp_close(file, 0);
out_pop:
        pop_ctxt(&saved, &obd->obd_lvfs_ctxt, &uc);
out:
        req->rq_status = rc;
        RETURN(0);
}

int mds_reint(struct ptlrpc_request *req, int offset,
              struct lustre_handle *lockh)
{
        struct mds_update_record *rec; /* 116 bytes on the stack?  no sir! */
        int rc;
        ENTRY;

        OBD_ALLOC(rec, sizeof(*rec));
        if (rec == NULL)
                RETURN(-ENOMEM);

        rc = mds_update_unpack(req, offset, rec);
        if (rc || OBD_FAIL_CHECK(OBD_FAIL_MDS_REINT_UNPACK)) {
                CERROR("invalid record\n");
                GOTO(out, req->rq_status = -EINVAL);
        }
        /* rc will be used to interrupt a for loop over multiple records */
        rc = mds_reint_rec(rec, offset, req, lockh);
 out:
        OBD_FREE(rec, sizeof(*rec));
        RETURN(rc);
}

static int mds_filter_recovery_request(struct ptlrpc_request *req,
                                       struct obd_device *obd, int *process)
{
        switch (req->rq_reqmsg->opc) {
        case MDS_CONNECT: /* This will never get here, but for completeness. */
        case OST_CONNECT: /* This will never get here, but for completeness. */
        case MDS_DISCONNECT:
        case OST_DISCONNECT:
               *process = 1;
               RETURN(0);

        case MDS_CLOSE:
        case MDS_SYNC: /* used in unmounting */
        case OBD_PING:
        case MDS_REINT:
        case LDLM_ENQUEUE:
        case OST_CREATE:
                *process = target_queue_recovery_request(req, obd);
                RETURN(0);

        default:
                DEBUG_REQ(D_ERROR, req, "not permitted during recovery");
                *process = 0;
                /* XXX what should we set rq_status to here? */
                req->rq_status = -EAGAIN;
                RETURN(ptlrpc_error(req));
        }
}

static char *reint_names[] = {
        [REINT_SETATTR] "setattr",
        [REINT_CREATE]  "create",
        [REINT_LINK]    "link",
        [REINT_UNLINK]  "unlink",
        [REINT_RENAME]  "rename",
        [REINT_OPEN]    "open",
};

#define FILTER_VALID_FLAGS (OBD_MD_FLTYPE | OBD_MD_FLMODE | OBD_MD_FLGENER  |\
                            OBD_MD_FLSIZE | OBD_MD_FLBLOCKS | OBD_MD_FLBLKSZ|\
                            OBD_MD_FLATIME | OBD_MD_FLMTIME | OBD_MD_FLCTIME|\
                            OBD_MD_FLID) 

static void reconstruct_create(struct ptlrpc_request *req)
{
        struct mds_export_data *med = &req->rq_export->exp_mds_data;
        struct mds_client_data *mcd = med->med_mcd;
        struct ost_body *body;

        body = lustre_msg_buf(req->rq_repmsg, 0, sizeof(*body));

        /* copy rc, transno and disp; steal locks */
        mds_req_from_mcd(req, mcd);
        CERROR("reconstruct reply for x"LPU64"\n", req->rq_xid);
}

static int mdt_obj_create(struct ptlrpc_request *req)
{
        struct obd_device *obd = req->rq_export->exp_obd;
        struct ldlm_res_id res_id = { .name = {0} };
        struct mds_obd *mds = &obd->u.mds;
        struct ost_body *body, *repbody;
        char fidname[LL_FID_NAMELEN];
        struct inode *parent_inode;
        struct lustre_handle lockh;
        struct lvfs_run_ctxt saved;
        ldlm_policy_data_t policy;
        struct dentry *new = NULL;
        struct dentry_params dp;
        int mealen, flags = 0, rc, size = sizeof(*repbody), cleanup_phase = 0;
        unsigned int tmpname;
        struct lvfs_ucred uc;
        struct mea *mea;
        void *handle = NULL;
        ENTRY;
       
        DEBUG_REQ(D_HA, req, "create remote object");

        parent_inode = mds->mds_objects_dir->d_inode;

        body = lustre_swab_reqbuf(req, 0, sizeof(*body), lustre_swab_ost_body);
        if (body == NULL)
                RETURN(-EFAULT);

        rc = lustre_pack_reply(req, 1, &size, NULL);
        if (rc)
                RETURN(rc);

        MDS_CHECK_RESENT(req, reconstruct_create(req));

        uc.luc_fsuid = body->oa.o_uid;
        uc.luc_fsgid = body->oa.o_gid;

        push_ctxt(&saved, &obd->obd_lvfs_ctxt, &uc);
        
        repbody = lustre_msg_buf(req->rq_repmsg, 0, sizeof(*repbody));

        if (body->oa.o_flags & OBD_FL_RECREATE_OBJS) {
                /* this is re-create request from MDS holding directory name.
                 * we have to lookup given ino/generation first. if it exists
                 * (good case) then there is nothing to do. if it does not
                 * then we have to recreate it */
                struct ll_fid fid;
                fid.id = body->oa.o_id;
                fid.generation = body->oa.o_generation;
                new = mds_fid2dentry(mds, &fid, NULL);
                if (!IS_ERR(new) && new->d_inode) {
                        CWARN("mkdir() repairing is on its way: %lu/%lu\n",
                              (unsigned long) fid.id,
                              (unsigned long) fid.generation);
                        obdo_from_inode(&repbody->oa, new->d_inode,
                                        FILTER_VALID_FLAGS);
                        repbody->oa.o_id = new->d_inode->i_ino;
                        repbody->oa.o_generation = new->d_inode->i_generation;
                        repbody->oa.o_valid |= OBD_MD_FLID | OBD_MD_FLGENER;
                        cleanup_phase = 1;
                        GOTO(cleanup, rc = 0);
                }
                CWARN("hmm. for some reason dir %lu/%lu (or reply) got lost\n",
                      (unsigned long) fid.id, (unsigned long) fid.generation);
                LASSERT(new->d_inode == NULL ||
                        new->d_inode->i_generation != fid.generation);
                l_dput(new); 
        }
        
        down(&parent_inode->i_sem);
        handle = fsfilt_start(obd, parent_inode, FSFILT_OP_MKDIR, NULL);
        LASSERT(!IS_ERR(handle));
        cleanup_phase = 1; /* transaction */

repeat:
        tmpname = ll_insecure_random_int();
        rc = sprintf(fidname, "%u", tmpname);
        new = lookup_one_len(fidname, mds->mds_objects_dir, rc);
        if (IS_ERR(new)) {
                CERROR("%s: can't lookup new inode (%s) for mkdir: %d\n",
                       obd->obd_name, fidname, (int) PTR_ERR(new));
                fsfilt_commit(obd, mds->mds_sb, new->d_inode, handle, 0);
                up(&parent_inode->i_sem);
                RETURN(PTR_ERR(new));
        } else if (new->d_inode) {
                CERROR("%s: name exists. repeat\n", obd->obd_name);
                goto repeat;
        }

        new->d_fsdata = (void *) &dp;
        dp.p_inum = 0;
        dp.p_ptr = req;

        if (lustre_msg_get_flags(req->rq_reqmsg) & MSG_REPLAY) {
                DEBUG_REQ(D_HA, req, "replay create obj %lu/%lu",
                          (unsigned long) body->oa.o_id,
                          (unsigned long) body->oa.o_generation);
                dp.p_inum = body->oa.o_id;
                dp.p_generation = body->oa.o_generation;
        }
        rc = vfs_mkdir(parent_inode, new, body->oa.o_mode);
        if (rc == 0) {
                if (lustre_msg_get_flags(req->rq_reqmsg) & MSG_REPLAY) {
                        LASSERTF(body->oa.o_id == new->d_inode->i_ino, 
                                 "BUG 3550: failed to recreate obj "
                                 LPU64" -> %lu\n",
                                 body->oa.o_id, new->d_inode->i_ino);
                        LASSERTF(body->oa.o_generation == 
                                 new->d_inode->i_generation,
                                 "BUG 3550: failed to recreate obj/gen "
                                 LPU64"/%u -> %lu/%u\n",
                                 body->oa.o_id, body->oa.o_generation,
                                 new->d_inode->i_ino, 
                                 new->d_inode->i_generation);
                }

                obdo_from_inode(&repbody->oa, new->d_inode, FILTER_VALID_FLAGS);
                repbody->oa.o_id = new->d_inode->i_ino;
                repbody->oa.o_generation = new->d_inode->i_generation;
                repbody->oa.o_valid |= OBD_MD_FLID | OBD_MD_FLGENER;

                rc = fsfilt_del_dir_entry(obd, new);
                up(&parent_inode->i_sem);

                if (rc) {
                        CERROR("can't remove name for object: %d\n", rc);
                        GOTO(cleanup, rc);
                }
                        
                /* this lock should be taken to serialize MDS modifications
                 * in failure case */
                res_id.name[0] = new->d_inode->i_ino;
                res_id.name[1] = new->d_inode->i_generation;
                policy.l_inodebits.bits = MDS_INODELOCK_UPDATE;
                rc = ldlm_cli_enqueue(NULL, NULL, obd->obd_namespace,
                                res_id, LDLM_IBITS, &policy,
                                LCK_EX, &flags, mds_blocking_ast,
                                ldlm_completion_ast, NULL, NULL,
                                NULL, 0, NULL, &lockh);
                if (rc != ELDLM_OK)
                        GOTO(cleanup, rc);
                cleanup_phase = 2; /* valid lockh */

                CDEBUG(D_OTHER, "created dirobj: %lu/%lu mode %o\n",
                                (unsigned long) new->d_inode->i_ino,
                                (unsigned long) new->d_inode->i_generation,
                                (unsigned) new->d_inode->i_mode);
        } else {
                up(&parent_inode->i_sem);
                CERROR("%s: can't create dirobj: %d\n", obd->obd_name, rc);
                GOTO(cleanup, rc);
        }

        if (body->oa.o_valid & OBD_MD_FLID) {
                /* this is new object for splitted dir. we have to
                 * prevent recursive splitting on it -bzzz */
                mealen = obd_size_diskmd(mds->mds_lmv_exp, NULL);
                OBD_ALLOC(mea, mealen);
                if (mea == NULL)
                        GOTO(cleanup, rc = -ENOMEM);
                mea->mea_magic = MEA_MAGIC_ALL_CHARS;
                mea->mea_master = 0;
                mea->mea_count = 0;
                down(&new->d_inode->i_sem);
                rc = fsfilt_set_md(obd, new->d_inode, handle, mea, mealen);
                up(&new->d_inode->i_sem);
                OBD_FREE(mea, mealen);
                CDEBUG(D_OTHER, "%s: mark non-splittable %lu/%u - %d\n",
                       obd->obd_name, new->d_inode->i_ino,
                       new->d_inode->i_generation, flags);
        } else if (body->oa.o_easize) {
                /* we pass LCK_EX to split routine to signal that we have
                 * exclusive access to the directory. simple because nobody
                 * knows it already exists -bzzz */
                mds_try_to_split_dir(obd, new, NULL, body->oa.o_easize, LCK_EX);
        }

cleanup:
        switch (cleanup_phase) {
        case 2: /* valid lockh */
                if (rc == 0)
                        ptlrpc_save_lock(req, &lockh, LCK_EX);
                else
                        ldlm_lock_decref(&lockh, LCK_EX);
        case 1: /* transaction */
                rc = mds_finish_transno(mds, parent_inode, handle, req, rc, 0);
        }

        l_dput(new);
        pop_ctxt(&saved, &obd->obd_lvfs_ctxt, &uc);
        RETURN(rc);
}

static int mdt_get_info(struct ptlrpc_request *req)
{
        char *key;
        struct obd_export *exp = req->rq_export;
        int keylen, rc = 0, size = sizeof(obd_id);
        obd_id *reply;
        ENTRY;

        key = lustre_msg_buf(req->rq_reqmsg, 0, 1);
        if (key == NULL) {
                DEBUG_REQ(D_HA, req, "no get_info key");
                RETURN(-EFAULT);
        }
        keylen = req->rq_reqmsg->buflens[0];

        if (keylen < strlen("mdsize") || memcmp(key, "mdsize", 6) != 0)
                RETURN(-EPROTO);

        rc = lustre_pack_reply(req, 1, &size, NULL);
        if (rc)
                RETURN(rc);

        reply = lustre_msg_buf(req->rq_repmsg, 0, sizeof(*reply));
        rc = obd_get_info(exp, keylen, key, &size, reply);
        req->rq_repmsg->status = 0;
        RETURN(rc);
}

static int mds_set_info(struct obd_export *exp, __u32 keylen,
                        void *key, __u32 vallen, void *val)
{
        struct obd_device *obd;
        struct mds_obd *mds;
        int    rc = 0;
        ENTRY;

        obd = class_exp2obd(exp);
        if (obd == NULL) {
                CDEBUG(D_IOCTL, "invalid client cookie "LPX64"\n",
                       exp->exp_handle.h_cookie);
                RETURN(-EINVAL);
        }

#define KEY_IS(str) \
        (keylen == strlen(str) && memcmp(key, str, keylen) == 0)

        mds = &obd->u.mds;
        if (KEY_IS("mds_num")) {
                int valsize;
                __u32 group;
                CDEBUG(D_IOCTL, "set mds num %d\n", *(int*)val);
                mds->mds_num = *(int*)val;
                group = FILTER_GROUP_FIRST_MDS + mds->mds_num;
                valsize = sizeof(group);
                /*mds number has been changed, so the corresponding obdfilter exp
                 *need to be changed too*/
                rc = obd_set_info(mds->mds_osc_exp, strlen("mds_conn"), "mds_conn",
                          valsize, &group);
                RETURN(rc);
        } else if (KEY_IS("client")) {
                if (!(exp->exp_flags & OBD_OPT_REAL_CLIENT)) {
                        atomic_inc(&mds->mds_real_clients);
                        CDEBUG(D_OTHER,"%s: peer from %s is real client (%d)\n",
                               obd->obd_name, exp->exp_client_uuid.uuid,
                               atomic_read(&mds->mds_real_clients));
                        exp->exp_flags |= OBD_OPT_REAL_CLIENT;
                }
                if (mds->mds_lmv_name) {
                        rc = mds_lmv_connect(obd, mds->mds_lmv_name);
                        LASSERT(rc == 0);
                }
                RETURN(0);
        }
        CDEBUG(D_IOCTL, "invalid key\n");
        RETURN(-EINVAL);
}

static int mdt_set_info(struct ptlrpc_request *req)
{
        char *key, *val;
        struct obd_export *exp = req->rq_export;
        int keylen, rc = 0, vallen;
        ENTRY;

        key = lustre_msg_buf(req->rq_reqmsg, 0, 1);
        if (key == NULL) {
                DEBUG_REQ(D_HA, req, "no set_info key");
                RETURN(-EFAULT);
        }
        keylen = req->rq_reqmsg->buflens[0];

        if (keylen == strlen("mds_num") &&
            memcmp(key, "mds_num", keylen) == 0) {
                rc = lustre_pack_reply(req, 0, NULL, NULL);
                if (rc)
                        RETURN(rc);
                val = lustre_msg_buf(req->rq_reqmsg, 1, 0);

                vallen = req->rq_reqmsg->buflens[1];

                rc = obd_set_info(exp, keylen, key, vallen, val);
                req->rq_repmsg->status = 0;
                RETURN(rc);
        } else if (keylen == strlen("client") &&
                   memcmp(key, "client", keylen) == 0) {
                rc = lustre_pack_reply(req, 0, NULL, NULL);
                if (rc)
                        RETURN(rc);
                rc = obd_set_info(exp, keylen, key, sizeof(obd_id), NULL);
                req->rq_repmsg->status = 0;
                RETURN(rc);
        } 
        CDEBUG(D_IOCTL, "invalid key\n");
        RETURN(-EINVAL);
}

int mds_handle(struct ptlrpc_request *req)
{
        int should_process, fail = OBD_FAIL_MDS_ALL_REPLY_NET;
        int rc = 0;
        struct mds_obd *mds = NULL; /* quell gcc overwarning */
        struct obd_device *obd = NULL;
        ENTRY;

        OBD_FAIL_RETURN(OBD_FAIL_MDS_ALL_REQUEST_NET | OBD_FAIL_ONCE, 0);

        LASSERT(current->journal_info == NULL);
        /* XXX identical to OST */
        if (req->rq_reqmsg->opc != MDS_CONNECT) {
                struct mds_export_data *med;
                int recovering;

                if (req->rq_export == NULL) {
                        CERROR("lustre_mds: operation %d on unconnected MDS\n",
                               req->rq_reqmsg->opc);
                        req->rq_status = -ENOTCONN;
                        GOTO(out, rc = -ENOTCONN);
                }

                med = &req->rq_export->exp_mds_data;
                obd = req->rq_export->exp_obd;
                mds = &obd->u.mds;

                /* sanity check: if the xid matches, the request must
                 * be marked as a resent or replayed */
                if (req->rq_xid == med->med_mcd->mcd_last_xid)
                        LASSERTF(lustre_msg_get_flags(req->rq_reqmsg) &
                                 (MSG_RESENT | MSG_REPLAY),
                                 "rq_xid "LPU64" matches last_xid, "
                                 "expected RESENT flag\n",
                                 req->rq_xid);
                /* else: note the opposite is not always true; a
                 * RESENT req after a failover will usually not match
                 * the last_xid, since it was likely never
                 * committed. A REPLAYed request will almost never
                 * match the last xid, however it could for a
                 * committed, but still retained, open. */

                spin_lock_bh(&obd->obd_processing_task_lock);
                recovering = obd->obd_recovering;
                spin_unlock_bh(&obd->obd_processing_task_lock);
                if (recovering) {
                        rc = mds_filter_recovery_request(req, obd,
                                                         &should_process);
                        if (rc || should_process == 0) {
                                RETURN(rc);
                        } else if (should_process < 0) {
                                req->rq_status = should_process;
                                rc = ptlrpc_error(req);
                                RETURN(rc);
                        }
                }
        }

        switch (req->rq_reqmsg->opc) {
        case MDS_CONNECT:
                DEBUG_REQ(D_INODE, req, "connect");
                OBD_FAIL_RETURN(OBD_FAIL_MDS_CONNECT_NET, 0);
                rc = target_handle_connect(req);
                if (!rc)
                        /* Now that we have an export, set mds. */
                        mds = mds_req2mds(req);
                break;

        case MDS_DISCONNECT:
                DEBUG_REQ(D_INODE, req, "disconnect");
                OBD_FAIL_RETURN(OBD_FAIL_MDS_DISCONNECT_NET, 0);
                rc = target_handle_disconnect(req);
                req->rq_status = rc;            /* superfluous? */
                break;

        case MDS_GETSTATUS:
                DEBUG_REQ(D_INODE, req, "getstatus");
                OBD_FAIL_RETURN(OBD_FAIL_MDS_GETSTATUS_NET, 0);
                rc = mds_getstatus(req);
                break;

        case MDS_GETATTR:
                DEBUG_REQ(D_INODE, req, "getattr");
                OBD_FAIL_RETURN(OBD_FAIL_MDS_GETATTR_NET, 0);
                rc = mds_getattr(0, req);
                break;

        case MDS_GETATTR_NAME: {
                struct lustre_handle lockh;
                DEBUG_REQ(D_INODE, req, "getattr_name");
                OBD_FAIL_RETURN(OBD_FAIL_MDS_GETATTR_NAME_NET, 0);

                /* If this request gets a reconstructed reply, we won't be
                 * acquiring any new locks in mds_getattr_name, so we don't
                 * want to cancel.
                 */
                lockh.cookie = 0;
                rc = mds_getattr_name(0, req, &lockh, MDS_INODELOCK_UPDATE);
                /* this non-intent call (from an ioctl) is special */
                req->rq_status = rc;
                if (rc == 0 && lockh.cookie)
                        ldlm_lock_decref(&lockh, LCK_PR);
                break;
        }
        case MDS_STATFS:
                DEBUG_REQ(D_INODE, req, "statfs");
                OBD_FAIL_RETURN(OBD_FAIL_MDS_STATFS_NET, 0);
                rc = mds_statfs(req);
                break;

        case MDS_READPAGE:
                DEBUG_REQ(D_INODE, req, "readpage");
                OBD_FAIL_RETURN(OBD_FAIL_MDS_READPAGE_NET, 0);
                rc = mds_readpage(req);

                if (OBD_FAIL_CHECK_ONCE(OBD_FAIL_MDS_SENDPAGE)) {
                        if (req->rq_reply_state) {
                                lustre_free_reply_state (req->rq_reply_state);
                                req->rq_reply_state = NULL;
                        }
                        RETURN(0);
                }

                break;

        case MDS_REINT: {
                __u32 *opcp = lustre_msg_buf(req->rq_reqmsg, 0, sizeof (*opcp));
                __u32  opc;
                int size[3] = {sizeof(struct mds_body), mds->mds_max_mdsize,
                               mds->mds_max_cookiesize};
                int bufcount;

                /* NB only peek inside req now; mds_reint() will swab it */
                if (opcp == NULL) {
                        CERROR ("Can't inspect opcode\n");
                        rc = -EINVAL;
                        break;
                }
                opc = *opcp;
                if (lustre_msg_swabbed (req->rq_reqmsg))
                        __swab32s(&opc);

                DEBUG_REQ(D_INODE, req, "reint %d (%s)", opc,
                          (opc < sizeof(reint_names) / sizeof(reint_names[0]) ||
                           reint_names[opc] == NULL) ? reint_names[opc] :
                                                       "unknown opcode");

                OBD_FAIL_RETURN(OBD_FAIL_MDS_REINT_NET, 0);

                if (opc == REINT_UNLINK || opc == REINT_RENAME)
                        bufcount = 3;
                else if (opc == REINT_OPEN)
                        bufcount = 2;
                else
                        bufcount = 1;

                rc = lustre_pack_reply(req, bufcount, size, NULL);
                if (rc)
                        break;

                rc = mds_reint(req, 0, NULL);
                fail = OBD_FAIL_MDS_REINT_NET_REP;
                break;
        }

        case MDS_CLOSE:
                DEBUG_REQ(D_INODE, req, "close");
                OBD_FAIL_RETURN(OBD_FAIL_MDS_CLOSE_NET, 0);
                rc = mds_close(req);
                break;

        case MDS_DONE_WRITING:
                DEBUG_REQ(D_INODE, req, "done_writing");
                OBD_FAIL_RETURN(OBD_FAIL_MDS_DONE_WRITING_NET, 0);
                rc = mds_done_writing(req);
                break;

        case MDS_PIN:
                DEBUG_REQ(D_INODE, req, "pin");
                OBD_FAIL_RETURN(OBD_FAIL_MDS_PIN_NET, 0);
                rc = mds_pin(req);
                break;

        case MDS_SYNC:
                DEBUG_REQ(D_INODE, req, "sync");
                OBD_FAIL_RETURN(OBD_FAIL_MDS_SYNC_NET, 0);
                rc = mds_sync(req);
                break;

        case OBD_PING:
                DEBUG_REQ(D_INODE, req, "ping");
                rc = target_handle_ping(req);
                break;

        case OBD_LOG_CANCEL:
                CDEBUG(D_INODE, "log cancel\n");
                OBD_FAIL_RETURN(OBD_FAIL_OBD_LOG_CANCEL_NET, 0);
                rc = -ENOTSUPP; /* la la la */
                break;

        case LDLM_ENQUEUE:
                DEBUG_REQ(D_INODE, req, "enqueue");
                OBD_FAIL_RETURN(OBD_FAIL_LDLM_ENQUEUE, 0);
                rc = ldlm_handle_enqueue(req, ldlm_server_completion_ast,
                                         ldlm_server_blocking_ast, NULL);
                break;
        case LDLM_CONVERT:
                DEBUG_REQ(D_INODE, req, "convert");
                OBD_FAIL_RETURN(OBD_FAIL_LDLM_CONVERT, 0);
                rc = ldlm_handle_convert(req);
                break;
        case LDLM_BL_CALLBACK:
        case LDLM_CP_CALLBACK:
                DEBUG_REQ(D_INODE, req, "callback");
                CERROR("callbacks should not happen on MDS\n");
                LBUG();
                OBD_FAIL_RETURN(OBD_FAIL_LDLM_BL_CALLBACK, 0);
                break;
        case LLOG_ORIGIN_HANDLE_OPEN:
                DEBUG_REQ(D_INODE, req, "llog_init");
                OBD_FAIL_RETURN(OBD_FAIL_OBD_LOGD_NET, 0);
                rc = llog_origin_handle_open(req);
                break;
        case LLOG_ORIGIN_HANDLE_NEXT_BLOCK:
                DEBUG_REQ(D_INODE, req, "llog next block");
                OBD_FAIL_RETURN(OBD_FAIL_OBD_LOGD_NET, 0);
                rc = llog_origin_handle_next_block(req);
                break;
        case LLOG_ORIGIN_HANDLE_PREV_BLOCK:
                DEBUG_REQ(D_INODE, req, "llog prev block");
                OBD_FAIL_RETURN(OBD_FAIL_OBD_LOGD_NET, 0);
                rc = llog_origin_handle_prev_block(req);
                break;
        case LLOG_ORIGIN_HANDLE_READ_HEADER:
                DEBUG_REQ(D_INODE, req, "llog read header");
                OBD_FAIL_RETURN(OBD_FAIL_OBD_LOGD_NET, 0);
                rc = llog_origin_handle_read_header(req);
                break;
        case LLOG_ORIGIN_HANDLE_CLOSE:
                DEBUG_REQ(D_INODE, req, "llog close");
                OBD_FAIL_RETURN(OBD_FAIL_OBD_LOGD_NET, 0);
                rc = llog_origin_handle_close(req);
                break;
        case OST_CREATE:
                DEBUG_REQ(D_INODE, req, "ost_create");
                rc = mdt_obj_create(req);
                break;
        case OST_GET_INFO:
                DEBUG_REQ(D_INODE, req, "get_info");
                rc = mdt_get_info(req);
                break;
        case OST_SET_INFO:
                DEBUG_REQ(D_INODE, req, "set_info");
                rc = mdt_set_info(req);
                break;
        case OST_WRITE:
                CDEBUG(D_INODE, "write\n");
                OBD_FAIL_RETURN(OBD_FAIL_OST_BRW_NET, 0);
                rc = ost_brw_write(req, NULL);
                LASSERT(current->journal_info == NULL);
                /* mdt_brw sends its own replies */
                RETURN(rc);
                break;
        case LLOG_CATINFO:
                DEBUG_REQ(D_INODE, req, "llog catinfo");
                OBD_FAIL_RETURN(OBD_FAIL_OBD_LOGD_NET, 0);
                rc = llog_catinfo(req);
                break;
        default:
                req->rq_status = -ENOTSUPP;
                rc = ptlrpc_error(req);
                RETURN(rc);
        }

        LASSERT(current->journal_info == NULL);

        EXIT;

        /* If we're DISCONNECTing, the mds_export_data is already freed */
        if (!rc && req->rq_reqmsg->opc != MDS_DISCONNECT) {
                struct mds_export_data *med = &req->rq_export->exp_mds_data;
                struct obd_device *obd = list_entry(mds, struct obd_device,
                                                    u.mds);
                req->rq_repmsg->last_xid =
                        le64_to_cpu(med->med_mcd->mcd_last_xid);

                if (!obd->obd_no_transno) {
                        req->rq_repmsg->last_committed =
                                obd->obd_last_committed;
                } else {
                        DEBUG_REQ(D_IOCTL, req,
                                  "not sending last_committed update");
                }
                CDEBUG(D_INFO, "last_transno "LPU64", last_committed "LPU64
                       ", xid "LPU64"\n",
                       mds->mds_last_transno, obd->obd_last_committed,
                       req->rq_xid);
        }
 out:

        if (lustre_msg_get_flags(req->rq_reqmsg) & MSG_LAST_REPLAY) {
                if (obd && obd->obd_recovering) {
                        DEBUG_REQ(D_HA, req, "LAST_REPLAY, queuing reply");
                        return target_queue_final_reply(req, rc);
                }
                /* Lost a race with recovery; let the error path DTRT. */
                rc = req->rq_status = -ENOTCONN;
        }

        target_send_reply(req, rc, fail);
        return 0;
}

/* Update the server data on disk.  This stores the new mount_count and
 * also the last_rcvd value to disk.  If we don't have a clean shutdown,
 * then the server last_rcvd value may be less than that of the clients.
 * This will alert us that we may need to do client recovery.
 *
 * Also assumes for mds_last_transno that we are not modifying it (no locking).
 */
int mds_update_server_data(struct obd_device *obd, int force_sync)
{
        struct mds_obd *mds = &obd->u.mds;
        struct mds_server_data *msd = mds->mds_server_data;
        struct file *filp = mds->mds_rcvd_filp;
        struct lvfs_run_ctxt saved;
        loff_t off = 0;
        int rc;
        ENTRY;

        push_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
        msd->msd_last_transno = cpu_to_le64(mds->mds_last_transno);

        CDEBUG(D_SUPER, "MDS mount_count is "LPU64", last_transno is "LPU64"\n",
               mds->mds_mount_count, mds->mds_last_transno);
        rc = fsfilt_write_record(obd, filp, msd, sizeof(*msd), &off,force_sync);
        if (rc)
                CERROR("error writing MDS server data: rc = %d\n", rc);
        pop_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);

        RETURN(rc);
}

/* mount the file system (secretly) */
static int mds_setup(struct obd_device *obd, obd_count len, void *buf)
{
        struct lustre_cfg* lcfg = buf;
        struct mds_obd *mds = &obd->u.mds;
        char *options = NULL;
        struct vfsmount *mnt;
        unsigned long page;
        int rc = 0;
        ENTRY;

        dev_clear_rdonly(2);

        if (!lcfg->lcfg_inlbuf1 || !lcfg->lcfg_inlbuf2)
                RETURN(rc = -EINVAL);

        obd->obd_fsops = fsfilt_get_ops(lcfg->lcfg_inlbuf2);
        if (IS_ERR(obd->obd_fsops))
                RETURN(rc = PTR_ERR(obd->obd_fsops));

        mds->mds_max_mdsize = sizeof(struct lov_mds_md);

        page = __get_free_page(GFP_KERNEL);
        if (!page)
                RETURN(-ENOMEM);

        options = (char *)page;
        memset(options, 0, PAGE_SIZE);

        /* here we use "iopen_nopriv" hardcoded, because it affects MDS utility
         * and the rest of options are passed by mount options. Probably this
         * should be moved to somewhere else like startup scripts or lconf. */
        sprintf(options, "iopen_nopriv");

        if (lcfg->lcfg_inllen4 > 0 && lcfg->lcfg_inlbuf4)
                sprintf(options + strlen(options), ",%s",
                        lcfg->lcfg_inlbuf4);

        /* we have to know mdsnum before touching underlying fs -bzzz */
        if (lcfg->lcfg_inllen5 > 0 && lcfg->lcfg_inlbuf5 && 
            strcmp(lcfg->lcfg_inlbuf5, "dumb")) {
                class_uuid_t uuid;

                CDEBUG(D_OTHER, "MDS: %s is master for %s\n",
                       obd->obd_name, lcfg->lcfg_inlbuf5);

                generate_random_uuid(uuid);
                class_uuid_unparse(uuid, &mds->mds_lmv_uuid);

                OBD_ALLOC(mds->mds_lmv_name, lcfg->lcfg_inllen5);
                if (mds->mds_lmv_name == NULL) 
                        RETURN(rc = -ENOMEM);

                memcpy(mds->mds_lmv_name, lcfg->lcfg_inlbuf5,
                       lcfg->lcfg_inllen5);
                
                rc = mds_lmv_connect(obd, mds->mds_lmv_name);
                if (rc) {
                        OBD_FREE(mds->mds_lmv_name, lcfg->lcfg_inllen5);
                        GOTO(err_ops, rc);
                }
        }
        
        /* FIXME-WANGDI: this should be reworked when we will use lmv along 
         * with cobd, because correct mdsnum is set in mds_lmv_connect(). */
        if (lcfg->lcfg_inllen6 > 0 && lcfg->lcfg_inlbuf6 && !mds->mds_lmv_obd &&
            strcmp(lcfg->lcfg_inlbuf6, "dumb")) {
                if (!memcmp(lcfg->lcfg_inlbuf6, "master", strlen("master")) &&
                    mds->mds_num == 0) {
                        mds->mds_num = REAL_MDS_NUMBER;
                } else if (!memcmp(lcfg->lcfg_inlbuf6, "cache", strlen("cache")) &&
                           mds->mds_num == 0) {
                        mds->mds_num = CACHE_MDS_NUMBER;
                }     
        }

        mnt = do_kern_mount(lcfg->lcfg_inlbuf2, 0, 
                            lcfg->lcfg_inlbuf1, options);

        free_page(page);

        if (IS_ERR(mnt)) {
                rc = PTR_ERR(mnt);
                CERROR("do_kern_mount failed: rc = %d\n", rc);
                GOTO(err_ops, rc);
        }

        CDEBUG(D_SUPER, "%s: mnt = %p\n", lcfg->lcfg_inlbuf1, mnt);

        sema_init(&mds->mds_orphan_recovery_sem, 1);
        sema_init(&mds->mds_epoch_sem, 1);
        spin_lock_init(&mds->mds_transno_lock);
        mds->mds_max_cookiesize = sizeof(struct llog_cookie);
        atomic_set(&mds->mds_open_count, 0);
        atomic_set(&mds->mds_real_clients, 0);

        obd->obd_namespace = ldlm_namespace_new(obd->obd_name,
                                                LDLM_NAMESPACE_SERVER);
        if (obd->obd_namespace == NULL) {
                mds_cleanup(obd, 0);
                GOTO(err_put, rc = -ENOMEM);
        }
        ldlm_register_intent(obd->obd_namespace, mds_intent_policy);

        rc = mds_fs_setup(obd, mnt);
        if (rc) {
                CERROR("MDS filesystem method init failed: rc = %d\n", rc);
                GOTO(err_ns, rc);
        }

        rc = llog_start_commit_thread();
        if (rc < 0)
                GOTO(err_fs, rc);

        /* this check for @dumb string is needed to handle mounting MDS with
         * smfs. Read lconf:MDSDEV.write_conf() for more details. */
        if (lcfg->lcfg_inllen3 > 0 && lcfg->lcfg_inlbuf3 &&
            strcmp(lcfg->lcfg_inlbuf3, "dumb")) {
                class_uuid_t uuid;

                generate_random_uuid(uuid);
                class_uuid_unparse(uuid, &mds->mds_lov_uuid);

                OBD_ALLOC(mds->mds_profile, lcfg->lcfg_inllen3);
                if (mds->mds_profile == NULL)
                        GOTO(err_fs, rc = -ENOMEM);

                memcpy(mds->mds_profile, lcfg->lcfg_inlbuf3,
                       lcfg->lcfg_inllen3);
        }

        ptlrpc_init_client(LDLM_CB_REQUEST_PORTAL, LDLM_CB_REPLY_PORTAL,
                           "mds_ldlm_client", &obd->obd_ldlm_client);
        obd->obd_replayable = 1;

        rc = mds_postsetup(obd);
        if (rc)
                GOTO(err_fs, rc);

        RETURN(0);

err_fs:
        /* No extra cleanup needed for llog_init_commit_thread() */
        mds_fs_cleanup(obd, 0);
err_ns:
        ldlm_namespace_free(obd->obd_namespace, 0);
        obd->obd_namespace = NULL;
err_put:
        unlock_kernel();
        mntput(mds->mds_vfsmnt);
        mds->mds_sb = 0;
        lock_kernel();
err_ops:
        fsfilt_put_ops(obd->obd_fsops);
        return rc;
}

static int mds_postsetup(struct obd_device *obd)
{
        struct mds_obd *mds = &obd->u.mds;
        int rc = 0;
        ENTRY;

        rc = obd_llog_setup(obd, &obd->obd_llogs, LLOG_CONFIG_ORIG_CTXT, 
                            obd, 0, NULL, &llog_lvfs_ops);
        if (rc)
                RETURN(rc);

        if (mds->mds_profile) {
                struct llog_ctxt *lgctxt;
                struct lvfs_run_ctxt saved;
                struct lustre_profile *lprof;
                struct config_llog_instance cfg;

                cfg.cfg_instance = NULL;
                cfg.cfg_uuid = mds->mds_lov_uuid;
                push_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);

                lgctxt = llog_get_context(&obd->obd_llogs, LLOG_CONFIG_ORIG_CTXT);
                if (!lgctxt) {
                        pop_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
                        GOTO(err_llog, rc = -EINVAL);
                }
                
                rc = class_config_process_llog(lgctxt, mds->mds_profile, &cfg);
                pop_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);

                if (rc)
                        GOTO(err_llog, rc);

                lprof = class_get_profile(mds->mds_profile);
                if (lprof == NULL) {
                        CERROR("No profile found: %s\n", mds->mds_profile);
                        GOTO(err_cleanup, rc = -ENOENT);
                }
                rc = mds_lov_connect(obd, lprof->lp_osc);
                if (rc)
                        GOTO(err_cleanup, rc);

                rc = mds_lmv_postsetup(obd);
                if (rc)
                        GOTO(err_cleanup, rc);
        }

        RETURN(rc);

err_cleanup:
        mds_lov_clean(obd);
err_llog:
        obd_llog_cleanup(llog_get_context(&obd->obd_llogs,
                                          LLOG_CONFIG_ORIG_CTXT));
        RETURN(rc);
}

int mds_postrecov(struct obd_device *obd)
{
        struct mds_obd *mds = &obd->u.mds;
        struct llog_ctxt *ctxt;
        int rc, item = 0;
        ENTRY;

        LASSERT(!obd->obd_recovering);
        ctxt = llog_get_context(&obd->obd_llogs, LLOG_UNLINK_ORIG_CTXT);
        LASSERT(ctxt != NULL);

        /* set nextid first, so we are sure it happens */
        rc = mds_lov_set_nextid(obd);
        if (rc) {
                CERROR("%s: mds_lov_set_nextid failed\n", obd->obd_name);
                GOTO(out, rc);
        }

        /* clean PENDING dir */
        rc = mds_cleanup_orphans(obd);
        if (rc < 0)
                GOTO(out, rc);
        item = rc;

        rc = llog_connect(ctxt, obd->u.mds.mds_lov_desc.ld_tgt_count,
                          NULL, NULL, NULL);
        if (rc) {
                CERROR("%s: failed at llog_origin_connect: %d\n", 
                       obd->obd_name, rc);
                GOTO(out, rc);
        }

        /* remove the orphaned precreated objects */
        rc = mds_lov_clearorphans(mds, NULL /* all OSTs */);
        if (rc)
                GOTO(err_llog, rc);

out:
        RETURN(rc < 0 ? rc : item);

err_llog:
        /* cleanup all llogging subsystems */
        rc = obd_llog_finish(obd, &obd->obd_llogs,
                             mds->mds_lov_desc.ld_tgt_count);
        if (rc)
                CERROR("%s: failed to cleanup llogging subsystems\n",
                        obd->obd_name);
        goto out;
}

int mds_lov_clean(struct obd_device *obd)
{
        struct mds_obd *mds = &obd->u.mds;

        if (mds->mds_profile) {
                char * cln_prof;
                struct config_llog_instance cfg;
                struct lvfs_run_ctxt saved;
                int len = strlen(mds->mds_profile) + sizeof("-clean") + 1;

                OBD_ALLOC(cln_prof, len);
                sprintf(cln_prof, "%s-clean", mds->mds_profile);

                cfg.cfg_instance = NULL;
                cfg.cfg_uuid = mds->mds_lov_uuid;

                push_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
                class_config_process_llog(llog_get_context(&obd->obd_llogs, LLOG_CONFIG_ORIG_CTXT),
                                          cln_prof, &cfg);
                pop_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);

                OBD_FREE(cln_prof, len);
                OBD_FREE(mds->mds_profile, strlen(mds->mds_profile) + 1);
                mds->mds_profile = NULL;
        }
        RETURN(0);
}

int mds_lmv_clean(struct obd_device *obd)
{
        struct mds_obd *mds = &obd->u.mds;

        if (mds->mds_lmv_name) {
                OBD_FREE(mds->mds_lmv_name, strlen(mds->mds_lmv_name) + 1);
                mds->mds_lmv_name = NULL;
        }
        RETURN(0);
}

static int mds_precleanup(struct obd_device *obd, int flags)
{
        int rc = 0;
        ENTRY;

        mds_lmv_clean(obd);
        mds_lov_disconnect(obd, flags);
        mds_lov_clean(obd);
        obd_llog_cleanup(llog_get_context(&obd->obd_llogs, LLOG_CONFIG_ORIG_CTXT));
        RETURN(rc);
}

static int mds_cleanup(struct obd_device *obd, int flags)
{
        struct mds_obd *mds = &obd->u.mds;
        ENTRY;

        if (mds->mds_sb == NULL)
                RETURN(0);

        mds_update_server_data(obd, 1);
        if (mds->mds_lov_objids != NULL) {
                OBD_FREE(mds->mds_lov_objids,
                         mds->mds_lov_desc.ld_tgt_count * sizeof(obd_id));
        }
        mds_fs_cleanup(obd, flags);

        unlock_kernel();

        /* 2 seems normal on mds, (may_umount() also expects 2
          fwiw), but we only see 1 at this point in obdfilter. */
        if (atomic_read(&obd->u.mds.mds_vfsmnt->mnt_count) > 2)
                CERROR("%s: mount busy, mnt_count %d != 2\n", obd->obd_name,
                       atomic_read(&obd->u.mds.mds_vfsmnt->mnt_count));

        mntput(mds->mds_vfsmnt);

        mds->mds_sb = 0;

        ldlm_namespace_free(obd->obd_namespace, flags & OBD_OPT_FORCE);

        spin_lock_bh(&obd->obd_processing_task_lock);
        if (obd->obd_recovering) {
                target_cancel_recovery_timer(obd);
                obd->obd_recovering = 0;
        }
        spin_unlock_bh(&obd->obd_processing_task_lock);

        lock_kernel();
        dev_clear_rdonly(2);
        fsfilt_put_ops(obd->obd_fsops);

        RETURN(0);
}

static void fixup_handle_for_resent_req(struct ptlrpc_request *req,
                                        struct ldlm_lock *new_lock,
                                        struct lustre_handle *lockh)
{
        struct obd_export *exp = req->rq_export;
        struct obd_device *obd = exp->exp_obd;
        struct ldlm_request *dlmreq =
                lustre_msg_buf(req->rq_reqmsg, 0, sizeof (*dlmreq));
        struct lustre_handle remote_hdl = dlmreq->lock_handle1;
        struct list_head *iter;

        if (!(lustre_msg_get_flags(req->rq_reqmsg) & MSG_RESENT))
                return;

        l_lock(&obd->obd_namespace->ns_lock);
        list_for_each(iter, &exp->exp_ldlm_data.led_held_locks) {
                struct ldlm_lock *lock;
                lock = list_entry(iter, struct ldlm_lock, l_export_chain);
                if (lock == new_lock)
                        continue;
                if (lock->l_remote_handle.cookie == remote_hdl.cookie) {
                        lockh->cookie = lock->l_handle.h_cookie;
                        DEBUG_REQ(D_HA, req, "restoring lock cookie "LPX64,
                                  lockh->cookie);
                        l_unlock(&obd->obd_namespace->ns_lock);
                        return;
                }
        }
        l_unlock(&obd->obd_namespace->ns_lock);

        /* If the xid matches, then we know this is a resent request,
         * and allow it. (It's probably an OPEN, for which we don't
         * send a lock */
        if (req->rq_xid == exp->exp_mds_data.med_mcd->mcd_last_xid)
                return;

        /* This remote handle isn't enqueued, so we never received or
         * processed this request.  Clear MSG_RESENT, because it can
         * be handled like any normal request now. */

        lustre_msg_clear_flags(req->rq_reqmsg, MSG_RESENT);

        DEBUG_REQ(D_HA, req, "no existing lock with rhandle "LPX64,
                  remote_hdl.cookie);
}

int intent_disposition(struct ldlm_reply *rep, int flag)
{
        if (!rep)
                return 0;
        return (rep->lock_policy_res1 & flag);
}

void intent_set_disposition(struct ldlm_reply *rep, int flag)
{
        if (!rep)
                return;
        rep->lock_policy_res1 |= flag;
}

static int mds_intent_policy(struct ldlm_namespace *ns,
                             struct ldlm_lock **lockp, void *req_cookie,
                             ldlm_mode_t mode, int flags, void *data)
{
        struct ptlrpc_request *req = req_cookie;
        struct ldlm_lock *lock = *lockp;
        struct ldlm_intent *it;
        struct mds_obd *mds = &req->rq_export->exp_obd->u.mds;
        struct ldlm_reply *rep;
        struct lustre_handle lockh = { 0 };
        struct ldlm_lock *new_lock;
        int getattr_part = MDS_INODELOCK_UPDATE;
        int rc, offset = 2, repsize[4] = {sizeof(struct ldlm_reply),
                                          sizeof(struct mds_body),
                                          mds->mds_max_mdsize,
                                          mds->mds_max_cookiesize};
        ENTRY;

        LASSERT(req != NULL);

        if (req->rq_reqmsg->bufcount <= 1) {
                /* No intent was provided */
                int size = sizeof(struct ldlm_reply);
                rc = lustre_pack_reply(req, 1, &size, NULL);
                LASSERT(rc == 0);
                RETURN(0);
        }

        it = lustre_swab_reqbuf(req, 1, sizeof(*it), lustre_swab_ldlm_intent);
        if (it == NULL) {
                CERROR("Intent missing\n");
                RETURN(req->rq_status = -EFAULT);
        }

        LDLM_DEBUG(lock, "intent policy, opc: %s", ldlm_it2str(it->opc));

        rc = lustre_pack_reply(req, 3, repsize, NULL);
        if (rc)
                RETURN(req->rq_status = rc);

        rep = lustre_msg_buf(req->rq_repmsg, 0, sizeof (*rep));
        intent_set_disposition(rep, DISP_IT_EXECD);

        fixup_handle_for_resent_req(req, lock, &lockh);

        /* execute policy */
        switch ((long)it->opc) {
        case IT_OPEN:
        case IT_CREAT|IT_OPEN:
                /* XXX swab here to assert that an mds_open reint
                 * packet is following */
                rep->lock_policy_res2 = mds_reint(req, offset, &lockh);
#if 0
                /* We abort the lock if the lookup was negative and
                 * we did not make it to the OPEN portion */
                if (!intent_disposition(rep, DISP_LOOKUP_EXECD))
                        RETURN(ELDLM_LOCK_ABORTED);
                if (intent_disposition(rep, DISP_LOOKUP_NEG) &&
                    !intent_disposition(rep, DISP_OPEN_OPEN))
#endif
                /* IT_OPEN may return lock on cross-node dentry
                 * that we want to hold during attr retrival -bzzz */
                if (rc != 0 || lockh.cookie == 0)
                        RETURN(ELDLM_LOCK_ABORTED);
                break;
        case IT_LOOKUP:
                getattr_part = MDS_INODELOCK_LOOKUP;
        case IT_CHDIR:
        case IT_GETATTR:
                getattr_part |= MDS_INODELOCK_LOOKUP;
        case IT_READDIR:
                rep->lock_policy_res2 = mds_getattr_name(offset, req, &lockh,
                                                         getattr_part);
                /* FIXME: LDLM can set req->rq_status. MDS sets
                   policy_res{1,2} with disposition and status.
                   - replay: returns 0 & req->status is old status
                   - otherwise: returns req->status */
                if (intent_disposition(rep, DISP_LOOKUP_NEG))
                        rep->lock_policy_res2 = 0;
                if (!intent_disposition(rep, DISP_LOOKUP_POS) ||
                    rep->lock_policy_res2)
                        RETURN(ELDLM_LOCK_ABORTED);
                if (req->rq_status != 0) {
                        LBUG();
                        rep->lock_policy_res2 = req->rq_status;
                        RETURN(ELDLM_LOCK_ABORTED);
                }
                break;
        case IT_UNLINK:
                rc = mds_lock_and_check_slave(offset, req, &lockh);
                if ((rep->lock_policy_res2 = rc)) {
                        if (rc == ENOLCK)
                                rep->lock_policy_res2 = 0;
                        RETURN(ELDLM_LOCK_ABORTED);
                }
                break;
        default:
                CERROR("Unhandled intent "LPD64"\n", it->opc);
                LBUG();
        }

        /* By this point, whatever function we called above must have either
         * filled in 'lockh', been an intent replay, or returned an error.  We
         * want to allow replayed RPCs to not get a lock, since we would just
         * drop it below anyways because lock replay is done separately by the
         * client afterwards.  For regular RPCs we want to give the new lock to
         * the client instead of whatever lock it was about to get. */
        new_lock = ldlm_handle2lock(&lockh);
        if (new_lock == NULL && (flags & LDLM_FL_INTENT_ONLY))
                RETURN(0);

        LASSERT(new_lock != NULL);

        /* If we've already given this lock to a client once, then we should
         * have no readers or writers.  Otherwise, we should have one reader
         * _or_ writer ref (which will be zeroed below) before returning the
         * lock to a client. */
        if (new_lock->l_export == req->rq_export) {
                LASSERT(new_lock->l_readers + new_lock->l_writers == 0);
        } else {
                LASSERT(new_lock->l_export == NULL);
                LASSERT(new_lock->l_readers + new_lock->l_writers == 1);
        }

        *lockp = new_lock;

        if (new_lock->l_export == req->rq_export) {
                /* Already gave this to the client, which means that we
                 * reconstructed a reply. */
                LASSERT(lustre_msg_get_flags(req->rq_reqmsg) &
                        MSG_RESENT);
                RETURN(ELDLM_LOCK_REPLACED);
        }

        /* Fixup the lock to be given to the client */
        l_lock(&new_lock->l_resource->lr_namespace->ns_lock);
        new_lock->l_readers = 0;
        new_lock->l_writers = 0;

        new_lock->l_export = class_export_get(req->rq_export);
        list_add(&new_lock->l_export_chain,
                 &new_lock->l_export->exp_ldlm_data.led_held_locks);

        new_lock->l_blocking_ast = lock->l_blocking_ast;
        new_lock->l_completion_ast = lock->l_completion_ast;

        memcpy(&new_lock->l_remote_handle, &lock->l_remote_handle,
               sizeof(lock->l_remote_handle));

        new_lock->l_flags &= ~LDLM_FL_LOCAL;

        LDLM_LOCK_PUT(new_lock);
        l_unlock(&new_lock->l_resource->lr_namespace->ns_lock);

        RETURN(ELDLM_LOCK_REPLACED);
}

int mds_attach(struct obd_device *dev, obd_count len, void *data)
{
        struct lprocfs_static_vars lvars;

        lprocfs_init_multi_vars(0, &lvars);
        return lprocfs_obd_attach(dev, lvars.obd_vars);
}

int mds_detach(struct obd_device *dev)
{
        return lprocfs_obd_detach(dev);
}

int mdt_attach(struct obd_device *dev, obd_count len, void *data)
{
        struct lprocfs_static_vars lvars;

        lprocfs_init_multi_vars(1, &lvars);
        return lprocfs_obd_attach(dev, lvars.obd_vars);
}

int mdt_detach(struct obd_device *dev)
{
        return lprocfs_obd_detach(dev);
}

static int mdt_setup(struct obd_device *obd, obd_count len, void *buf)
{
        struct mds_obd *mds = &obd->u.mds;
        int rc = 0;
        ENTRY;

        mds->mds_service =
                ptlrpc_init_svc(MDS_NBUFS, MDS_BUFSIZE, MDS_MAXREQSIZE,
                                MDS_REQUEST_PORTAL, MDC_REPLY_PORTAL,
                                mds_handle, "mds", obd->obd_proc_entry);

        if (!mds->mds_service) {
                CERROR("failed to start service\n");
                RETURN(-ENOMEM);
        }

        rc = ptlrpc_start_n_threads(obd, mds->mds_service, MDT_NUM_THREADS,
                                    "ll_mdt");
        if (rc)
                GOTO(err_thread, rc);

        mds->mds_setattr_service =
                ptlrpc_init_svc(MDS_NBUFS, MDS_BUFSIZE, MDS_MAXREQSIZE,
                                MDS_SETATTR_PORTAL, MDC_REPLY_PORTAL,
                                mds_handle, "mds_setattr",
                                obd->obd_proc_entry);
        if (!mds->mds_setattr_service) {
                CERROR("failed to start getattr service\n");
                GOTO(err_thread, rc = -ENOMEM);
        }

        rc = ptlrpc_start_n_threads(obd, mds->mds_setattr_service,
                                    MDT_NUM_THREADS, "ll_mdt_attr");
        if (rc)
                GOTO(err_thread2, rc);

        mds->mds_readpage_service =
                ptlrpc_init_svc(MDS_NBUFS, MDS_BUFSIZE, MDS_MAXREQSIZE,
                                MDS_READPAGE_PORTAL, MDC_REPLY_PORTAL,
                                mds_handle, "mds_readpage",
                                obd->obd_proc_entry);
        if (!mds->mds_readpage_service) {
                CERROR("failed to start readpage service\n");
                GOTO(err_thread2, rc = -ENOMEM);
        }

        rc = ptlrpc_start_n_threads(obd, mds->mds_readpage_service,
                                    MDT_NUM_THREADS, "ll_mdt_rdpg");

        if (rc)
                GOTO(err_thread3, rc);

        RETURN(0);

err_thread3:
        ptlrpc_unregister_service(mds->mds_readpage_service);
err_thread2:
        ptlrpc_unregister_service(mds->mds_setattr_service);
err_thread:
        ptlrpc_unregister_service(mds->mds_service);
        return rc;
}

static int mdt_cleanup(struct obd_device *obd, int flags)
{
        struct mds_obd *mds = &obd->u.mds;
        ENTRY;

        ptlrpc_stop_all_threads(mds->mds_readpage_service);
        ptlrpc_unregister_service(mds->mds_readpage_service);

        ptlrpc_stop_all_threads(mds->mds_setattr_service);
        ptlrpc_unregister_service(mds->mds_setattr_service);

        ptlrpc_stop_all_threads(mds->mds_service);
        ptlrpc_unregister_service(mds->mds_service);

        RETURN(0);
}

static struct dentry *mds_lvfs_fid2dentry(__u64 id, __u32 gen, __u64 gr,
                                          void *data)
{
        struct obd_device *obd = data;
        struct ll_fid fid;
        fid.id = id;
        fid.generation = gen;
        return mds_fid2dentry(&obd->u.mds, &fid, NULL);
}

static int mds_get_info(struct obd_export *exp, __u32 keylen,
                        void *key, __u32 *vallen, void *val)
{
        struct obd_device *obd;
        struct mds_obd *mds;
        ENTRY;

        obd = class_exp2obd(exp);
        if (obd == NULL) {
                CDEBUG(D_IOCTL, "invalid client cookie "LPX64"\n",
                       exp->exp_handle.h_cookie);
                RETURN(-EINVAL);
        }

        if (keylen >= strlen("reint_log") && memcmp(key, "reint_log", 9) == 0) {
                /*Get log_context handle*/
                unsigned long *llh_handle = val;
                *vallen = sizeof(unsigned long);
                *llh_handle = (unsigned long)obd->obd_llog_ctxt[LLOG_REINT_ORIG_CTXT];
                RETURN(0);
        }
        if (keylen >= strlen("cache_sb") && memcmp(key, "cache_sb", 8) == 0) {
                /*Get log_context handle*/
                unsigned long *sb = val;
                *vallen = sizeof(unsigned long);
                *sb = (unsigned long)obd->u.mds.mds_sb;
                RETURN(0);
        }

        mds = &obd->u.mds;
        keylen == strlen("mdsize");
        if (keylen && memcmp(key, "mdsize", keylen) == 0) {
                __u32 *mdsize = val;
                *vallen = sizeof(*mdsize);
                *mdsize = mds->mds_max_mdsize;
                RETURN(0);
        }

        CDEBUG(D_IOCTL, "invalid key\n");
        RETURN(-EINVAL);

}
struct lvfs_callback_ops mds_lvfs_ops = {
        l_fid2dentry:     mds_lvfs_fid2dentry,
};

int mds_preprw(int cmd, struct obd_export *exp, struct obdo *oa,
                int objcount, struct obd_ioobj *obj,
                int niocount, struct niobuf_remote *nb,
                struct niobuf_local *res,
                struct obd_trans_info *oti);
int mds_commitrw(int cmd, struct obd_export *exp, struct obdo *oa,
                 int objcount, struct obd_ioobj *obj, int niocount,
                 struct niobuf_local *res, struct obd_trans_info *oti,
                 int rc);

/* use obd ops to offer management infrastructure */
static struct obd_ops mds_obd_ops = {
        .o_owner           = THIS_MODULE,
        .o_attach          = mds_attach,
        .o_detach          = mds_detach,
        .o_connect         = mds_connect,
        .o_init_export     = mds_init_export,
        .o_destroy_export  = mds_destroy_export,
        .o_disconnect      = mds_disconnect,
        .o_setup           = mds_setup,
        .o_precleanup      = mds_precleanup,
        .o_cleanup         = mds_cleanup,
        .o_postrecov       = mds_postrecov,
        .o_statfs          = mds_obd_statfs,
        .o_iocontrol       = mds_iocontrol,
        .o_create          = mds_obd_create,
        .o_destroy         = mds_obd_destroy,
        .o_llog_init       = mds_llog_init,
        .o_llog_finish     = mds_llog_finish,
        .o_notify          = mds_notify,
        .o_get_info        = mds_get_info,
        .o_set_info        = mds_set_info,
        .o_preprw          = mds_preprw, 
        .o_commitrw        = mds_commitrw,
};

static struct obd_ops mdt_obd_ops = {
        .o_owner           = THIS_MODULE,
        .o_attach          = mdt_attach,
        .o_detach          = mdt_detach,
        .o_setup           = mdt_setup,
        .o_cleanup         = mdt_cleanup,
        .o_attach          = mdt_attach,
        .o_detach          = mdt_detach,
};

static int __init mds_init(void)
{
        struct lprocfs_static_vars lvars;

        lprocfs_init_multi_vars(0, &lvars);
        class_register_type(&mds_obd_ops, NULL, lvars.module_vars,
                            LUSTRE_MDS_NAME);
        lprocfs_init_multi_vars(1, &lvars);
        class_register_type(&mdt_obd_ops, NULL, lvars.module_vars,
                            LUSTRE_MDT_NAME);

        return 0;
}

static void /*__exit*/ mds_exit(void)
{
        class_unregister_type(LUSTRE_MDS_NAME);
        class_unregister_type(LUSTRE_MDT_NAME);
}

MODULE_AUTHOR("Cluster File Systems, Inc. <info@clusterfs.com>");
MODULE_DESCRIPTION("Lustre Metadata Server (MDS)");
MODULE_LICENSE("GPL");

module_init(mds_init);
module_exit(mds_exit);
