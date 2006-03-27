/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  lustre/mds/handler.c
 *  Lustre Metadata Server (mds) request handler
 *
 *  Copyright (c) 2001-2005 Cluster File Systems, Inc.
 *   Author: Peter Braam <braam@clusterfs.com>
 *   Author: Andreas Dilger <adilger@clusterfs.com>
 *   Author: Phil Schwan <phil@clusterfs.com>
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
#include <linux/lustre_mds.h>
#include <linux/lustre_fsfilt.h>
#include <linux/lprocfs_status.h>
#include <linux/lustre_commit_confd.h>
#include <linux/lustre_quota.h>
#include <linux/lustre_disk.h>
#include <linux/lustre_ver.h>

#include "mds_internal.h"

static int mds_intent_policy(struct ldlm_namespace *ns,
                             struct ldlm_lock **lockp, void *req_cookie,
                             ldlm_mode_t mode, int flags, void *data);
static int mds_postsetup(struct obd_device *obd);
static int mds_cleanup(struct obd_device *obd);

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

        class_fail_export(req->rq_export);

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

/* only valid locked dentries or errors should be returned */
struct dentry *mds_fid2locked_dentry(struct obd_device *obd, struct ll_fid *fid,
                                     struct vfsmount **mnt, int lock_mode,
                                     struct lustre_handle *lockh,
                                     char *name, int namelen, __u64 lockpart)
{
        struct mds_obd *mds = &obd->u.mds;
        struct dentry *de = mds_fid2dentry(mds, fid, mnt), *retval = de;
        struct ldlm_res_id res_id = { .name = {0} };
        int flags = 0, rc;
        ldlm_policy_data_t policy = { .l_inodebits = { lockpart} }; 
        ENTRY;

        if (IS_ERR(de))
                RETURN(de);

        res_id.name[0] = de->d_inode->i_ino;
        res_id.name[1] = de->d_inode->i_generation;
        rc = ldlm_cli_enqueue(NULL, NULL, obd->obd_namespace, res_id,
                              LDLM_IBITS, &policy, lock_mode, &flags,
                              ldlm_blocking_ast, ldlm_completion_ast,
                              NULL, NULL, NULL, 0, NULL, lockh);
        if (rc != ELDLM_OK) {
                l_dput(de);
                retval = ERR_PTR(-EIO); /* XXX translate ldlm code */
        }

        RETURN(retval);
}

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
               ino, generation, mds->mds_obt.obt_sb);

        /* under ext3 this is neither supposed to return bad inodes
           nor NULL inodes. */
        result = ll_lookup_one_len(fid_name, mds->mds_fid_de, strlen(fid_name));
        if (IS_ERR(result))
                RETURN(result);

        inode = result->d_inode;
        if (!inode)
                RETURN(ERR_PTR(-ENOENT));

        if (inode->i_generation == 0 || inode->i_nlink == 0) {
                LCONSOLE_WARN("Found inode with zero generation or link -- this"
                              " may indicate disk corruption (inode: %lu, link:"
                              " %lu, count: %d)\n", inode->i_ino,
                              (unsigned long)inode->i_nlink,
                              atomic_read(&inode->i_count));
                dput(result);
                RETURN(ERR_PTR(-ENOENT));
        }

        if (generation && inode->i_generation != generation) {
                /* we didn't find the right inode.. */
                CDEBUG(D_INODE, "found wrong generation: inode %lu, link: %lu, "
                       "count: %d, generation %u/%u\n", inode->i_ino,
                       (unsigned long)inode->i_nlink,
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

static int mds_connect_internal(struct obd_export *exp, 
                                struct obd_connect_data *data)
{
        struct obd_device *obd = exp->exp_obd;
        if (data != NULL) {
                data->ocd_connect_flags &= MDS_CONNECT_SUPPORTED;
                data->ocd_ibits_known &= MDS_INODELOCK_FULL;

                /* If no known bits (which should not happen, probably,
                   as everybody should support LOOKUP and UPDATE bits at least)
                   revert to compat mode with plain locks. */
                if (!data->ocd_ibits_known &&
                    data->ocd_connect_flags & OBD_CONNECT_IBITS)
                        data->ocd_connect_flags &= ~OBD_CONNECT_IBITS;

                if (!obd->u.mds.mds_fl_acl)
                        data->ocd_connect_flags &= ~OBD_CONNECT_ACL;

                if (!obd->u.mds.mds_fl_user_xattr)
                        data->ocd_connect_flags &= ~OBD_CONNECT_XATTR;

                exp->exp_connect_flags = data->ocd_connect_flags;
                data->ocd_version = LUSTRE_VERSION_CODE;
                exp->exp_mds_data.med_ibits_known = data->ocd_ibits_known;
        }

        if (obd->u.mds.mds_fl_acl &&
            ((exp->exp_connect_flags & OBD_CONNECT_ACL) == 0)) {
                CWARN("%s: MDS requires ACL support but client does not\n",
                      obd->obd_name);
                return -EBADE;
        }
        return 0;
}

static int mds_reconnect(struct obd_export *exp, struct obd_device *obd,
                         struct obd_uuid *cluuid,
                         struct obd_connect_data *data)
{
        int rc;
        ENTRY;

        if (exp == NULL || obd == NULL || cluuid == NULL)
                RETURN(-EINVAL);

        rc = mds_connect_internal(exp, data);

        RETURN(rc);
}

/* Establish a connection to the MDS.
 *
 * This will set up an export structure for the client to hold state data
 * about that client, like open files, the last operation number it did
 * on the server, etc.
 */
static int mds_connect(struct lustre_handle *conn, struct obd_device *obd,
                       struct obd_uuid *cluuid, struct obd_connect_data *data)
{
        struct obd_export *exp;
        struct mds_export_data *med;
        struct mds_client_data *mcd = NULL;
        int rc, abort_recovery;
        ENTRY;

        if (!conn || !obd || !cluuid)
                RETURN(-EINVAL);

        /* Check for aborted recovery. */
        spin_lock_bh(&obd->obd_processing_task_lock);
        abort_recovery = obd->obd_abort_recovery;
        spin_unlock_bh(&obd->obd_processing_task_lock);
        if (abort_recovery)
                target_abort_recovery(obd);

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

        rc = mds_connect_internal(exp, data);
        if (rc)
                GOTO(out, rc);

        OBD_ALLOC(mcd, sizeof(*mcd));
        if (!mcd)
                GOTO(out, rc = -ENOMEM);

        memcpy(mcd->mcd_uuid, cluuid, sizeof(mcd->mcd_uuid));
        med->med_mcd = mcd;

        rc = mds_client_add(obd, &obd->u.mds, med, -1);
        GOTO(out, rc);

out:
        if (rc) {
                if (mcd) {
                        OBD_FREE(mcd, sizeof(*mcd));
                        med->med_mcd = NULL;
                }
                class_disconnect(exp);
        } else {
                class_export_put(exp);
        }

        RETURN(rc);
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
                struct dentry *dentry = mfd->mfd_dentry;

                /* Remove mfd handle so it can't be found again.
                 * We are consuming the mfd_list reference here. */
                mds_mfd_unlink(mfd, 0);
                spin_unlock(&med->med_open_lock);

                /* If you change this message, be sure to update
                 * replay_single:test_46 */
                CDEBUG(D_INODE|D_IOCTL, "%s: force closing file handle for "
                       "%.*s (ino %lu)\n", obd->obd_name, dentry->d_name.len,
                       dentry->d_name.name, dentry->d_inode->i_ino);
                /* child orphan sem protects orphan_dec_test and
                 * is_orphan race, mds_mfd_close drops it */
                MDS_DOWN_WRITE_ORPHAN_SEM(dentry->d_inode);
                rc = mds_mfd_close(NULL, MDS_REQ_REC_OFF, obd, mfd,
                                   !(export->exp_flags & OBD_OPT_FAILOVER));

                if (rc)
                        CDEBUG(D_INODE|D_IOCTL, "Error closing file: %d\n", rc);
                spin_lock(&med->med_open_lock);
        }
        spin_unlock(&med->med_open_lock);
        pop_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
out:
        mds_client_free(export);

        RETURN(rc);
}

static int mds_disconnect(struct obd_export *exp)
{
        unsigned long irqflags;
        int rc;
        ENTRY;

        LASSERT(exp);
        class_export_get(exp);

        /* Disconnect early so that clients can't keep using export */
        rc = class_disconnect(exp);
        ldlm_cancel_locks_for_export(exp);

        /* complete all outstanding replies */
        spin_lock_irqsave(&exp->exp_lock, irqflags);
        while (!list_empty(&exp->exp_outstanding_replies)) {
                struct ptlrpc_reply_state *rs =
                        list_entry(exp->exp_outstanding_replies.next,
                                   struct ptlrpc_reply_state, rs_exp_list);
                struct ptlrpc_service *svc = rs->rs_service;

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

int mds_get_md(struct obd_device *obd, struct inode *inode, void *md,
               int *size, int lock)
{
        int rc = 0;
        int lmm_size;

        if (lock)
                down(&inode->i_sem);
        rc = fsfilt_get_md(obd, inode, md, *size, "lov");

        if (rc < 0) {
                CERROR("Error %d reading eadata for ino %lu\n",
                       rc, inode->i_ino);
        } else if (rc > 0) {
                lmm_size = rc;
                rc = mds_convert_lov_ea(obd, inode, md, lmm_size);

                if (rc == 0) {
                        *size = lmm_size;
                        rc = lmm_size;
                } else if (rc > 0) {
                        *size = rc;
                }
        } else {
                *size = 0;
        }
        if (lock)
                up(&inode->i_sem);

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

#ifdef CONFIG_FS_POSIX_ACL
static
int mds_pack_posix_acl(struct inode *inode, struct lustre_msg *repmsg,
                       struct mds_body *repbody, int repoff)
{
        struct dentry de = { .d_inode = inode };
        int buflen, rc;
        ENTRY;

        LASSERT(repbody->aclsize == 0);
        LASSERT(repmsg->bufcount > repoff);

        buflen = lustre_msg_buflen(repmsg, repoff);
        if (!buflen)
                GOTO(out, 0);

        if (!inode->i_op || !inode->i_op->getxattr)
                GOTO(out, 0);

        lock_24kernel();
        rc = inode->i_op->getxattr(&de, XATTR_NAME_ACL_ACCESS,
                                   lustre_msg_buf(repmsg, repoff, buflen),
                                   buflen);
        unlock_24kernel();

        if (rc >= 0)
                repbody->aclsize = rc;
        else if (rc != -ENODATA) {
                CERROR("buflen %d, get acl: %d\n", buflen, rc);
                RETURN(rc);
        }
        EXIT;
out:
        repbody->valid |= OBD_MD_FLACL;
        return 0;
}
#else
#define mds_pack_posix_acl(inode, repmsg, repbody, repoff) 0
#endif

int mds_pack_acl(struct mds_export_data *med, struct inode *inode,
                 struct lustre_msg *repmsg, struct mds_body *repbody,
                 int repoff)
{
        return mds_pack_posix_acl(inode, repmsg, repbody, repoff);
}

static int mds_getattr_internal(struct obd_device *obd, struct dentry *dentry,
                                struct ptlrpc_request *req,
                                struct mds_body *reqbody, int reply_off)
{
        struct mds_body *body;
        struct inode *inode = dentry->d_inode;
        int rc = 0;
        ENTRY;

        if (inode == NULL)
                RETURN(-ENOENT);

        body = lustre_msg_buf(req->rq_repmsg, reply_off, sizeof(*body));
        LASSERT(body != NULL);                 /* caller prepped reply */

        mds_pack_inode2fid(&body->fid1, inode);
        mds_pack_inode2body(body, inode);
        reply_off++;

        if ((S_ISREG(inode->i_mode) && (reqbody->valid & OBD_MD_FLEASIZE)) ||
            (S_ISDIR(inode->i_mode) && (reqbody->valid & OBD_MD_FLDIREA))) {
                rc = mds_pack_md(obd, req->rq_repmsg, reply_off, body,
                                 inode, 1);

                /* If we have LOV EA data, the OST holds size, atime, mtime */
                if (!(body->valid & OBD_MD_FLEASIZE) &&
                    !(body->valid & OBD_MD_FLDIREA))
                        body->valid |= (OBD_MD_FLSIZE | OBD_MD_FLBLOCKS |
                                        OBD_MD_FLATIME | OBD_MD_FLMTIME);

                lustre_shrink_reply(req, reply_off, body->eadatasize, 0);
                if (body->eadatasize)
                        reply_off++;
        } else if (S_ISLNK(inode->i_mode) &&
                   (reqbody->valid & OBD_MD_LINKNAME) != 0) {
                char *symname = lustre_msg_buf(req->rq_repmsg, reply_off, 0);
                int len;

                LASSERT (symname != NULL);       /* caller prepped reply */
                len = req->rq_repmsg->buflens[reply_off];

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
                reply_off++;
        }

        if (reqbody->valid & OBD_MD_FLMODEASIZE) {
                struct mds_obd *mds = mds_req2mds(req);
                body->max_cookiesize = mds->mds_max_cookiesize;
                body->max_mdsize = mds->mds_max_mdsize;
                body->valid |= OBD_MD_FLMODEASIZE;
        }

        if (rc)
                RETURN(rc);

#ifdef CONFIG_FS_POSIX_ACL
        if ((req->rq_export->exp_connect_flags & OBD_CONNECT_ACL) &&
            (reqbody->valid & OBD_MD_FLACL)) {
                rc = mds_pack_acl(&req->rq_export->exp_mds_data,
                                  inode, req->rq_repmsg,
                                  body, reply_off);

                lustre_shrink_reply(req, reply_off, body->aclsize, 0);
                if (body->aclsize)
                        reply_off++;
        }
#endif

        RETURN(rc);
}

static int mds_getattr_pack_msg(struct ptlrpc_request *req, struct inode *inode,
                                int offset)
{
        struct mds_obd *mds = mds_req2mds(req);
        struct mds_body *body;
        int rc, size[2] = {sizeof(*body)}, bufcount = 1;
        ENTRY;

        body = lustre_msg_buf(req->rq_reqmsg, offset, sizeof (*body));
        LASSERT(body != NULL);                 /* checked by caller */
        LASSERT_REQSWABBED(req, offset);       /* swabbed by caller */

        if ((S_ISREG(inode->i_mode) && (body->valid & OBD_MD_FLEASIZE)) ||
            (S_ISDIR(inode->i_mode) && (body->valid & OBD_MD_FLDIREA))) {
                down(&inode->i_sem);
                rc = fsfilt_get_md(req->rq_export->exp_obd, inode, NULL, 0,
                                   "lov");
                up(&inode->i_sem);
                CDEBUG(D_INODE, "got %d bytes MD data for inode %lu\n",
                       rc, inode->i_ino);
                if (rc < 0) {
                        if (rc != -ENODATA) {
                                CERROR("error getting inode %lu MD: rc = %d\n",
                                       inode->i_ino, rc);
                                RETURN(rc);
                        }
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

#ifdef CONFIG_FS_POSIX_ACL
        if ((req->rq_export->exp_connect_flags & OBD_CONNECT_ACL) &&
            (body->valid & OBD_MD_FLACL)) {
                struct dentry de = { .d_inode = inode };

                size[bufcount] = 0;
                if (inode->i_op && inode->i_op->getxattr) {
                        lock_24kernel();
                        rc = inode->i_op->getxattr(&de, XATTR_NAME_ACL_ACCESS,
                                                   NULL, 0);
                        unlock_24kernel();

                        if (rc < 0) {
                                if (rc != -ENODATA) {
                                        CERROR("got acl size: %d\n", rc);
                                        RETURN(rc);
                                }
                        } else
                                size[bufcount] = rc;
                }
                bufcount++;
        }
#endif

        if (OBD_FAIL_CHECK(OBD_FAIL_MDS_GETATTR_PACK)) {
                CERROR("failed MDS_GETATTR_PACK test\n");
                req->rq_status = -ENOMEM;
                RETURN(-ENOMEM);
        }

        rc = lustre_pack_reply(req, bufcount, size, NULL);
        if (rc) {
                CERROR("lustre_pack_reply failed: rc %d\n", rc);
                req->rq_status = rc;
                RETURN(rc);
        }

        RETURN(0);
}

static int mds_getattr_name(int offset, struct ptlrpc_request *req,
                            int child_part, struct lustre_handle *child_lockh)
{
        struct obd_device *obd = req->rq_export->exp_obd;
        struct mds_obd *mds = &obd->u.mds;
        struct ldlm_reply *rep = NULL;
        struct lvfs_run_ctxt saved;
        struct mds_body *body;
        struct dentry *dparent = NULL, *dchild = NULL;
        struct lvfs_ucred uc = {NULL,};
        struct lustre_handle parent_lockh;
        int namesize;
        int rc = 0, cleanup_phase = 0, resent_req = 0;
        char *name;
        ENTRY;

        LASSERT(!strcmp(obd->obd_type->typ_name, LUSTRE_MDS_NAME));

        /* Swab now, before anyone looks inside the request */

        body = lustre_swab_reqbuf(req, offset, sizeof(*body),
                                  lustre_swab_mds_body);
        if (body == NULL) {
                CERROR("Can't swab mds_body\n");
                RETURN(-EFAULT);
        }

        LASSERT_REQSWAB(req, offset + 1);
        name = lustre_msg_string(req->rq_reqmsg, offset + 1, 0);
        if (name == NULL) {
                CERROR("Can't unpack name\n");
                RETURN(-EFAULT);
        }
        namesize = lustre_msg_buflen(req->rq_reqmsg, offset + 1);

        rc = mds_init_ucred(&uc, req, offset);
        if (rc)
                GOTO(cleanup, rc);

        LASSERT (offset == MDS_REQ_REC_OFF || offset == MDS_REQ_INTENT_REC_OFF);
        /* if requests were at offset 2, the getattr reply goes back at 1 */
        if (offset == MDS_REQ_INTENT_REC_OFF) {
                rep = lustre_msg_buf(req->rq_repmsg, 0, sizeof (*rep));
                offset = 1;
        }

        push_ctxt(&saved, &obd->obd_lvfs_ctxt, &uc);
        cleanup_phase = 1; /* kernel context */
        intent_set_disposition(rep, DISP_LOOKUP_EXECD);

        /* FIXME: handle raw lookup */
#if 0
        if (body->valid == OBD_MD_FLID) {
                struct mds_body *mds_reply;
                int size = sizeof(*mds_reply);
                ino_t inum;
                // The user requested ONLY the inode number, so do a raw lookup
                rc = lustre_pack_reply(req, 1, &size, NULL);
                if (rc) {
                        CERROR("out of memory\n");
                        GOTO(cleanup, rc);
                }

                rc = dir->i_op->lookup_raw(dir, name, namesize - 1, &inum);

                mds_reply = lustre_msg_buf(req->rq_repmsg, offset,
                                           sizeof(*mds_reply));
                mds_reply->fid1.id = inum;
                mds_reply->valid = OBD_MD_FLID;
                GOTO(cleanup, rc);
        }
#endif

        if (lustre_handle_is_used(child_lockh)) {
                LASSERT(lustre_msg_get_flags(req->rq_reqmsg) & MSG_RESENT);
                resent_req = 1;
        }

        if (resent_req == 0) {
            if (name) {
                rc = mds_get_parent_child_locked(obd, &obd->u.mds, &body->fid1,
                                                 &parent_lockh, &dparent,
                                                 LCK_CR,
                                                 MDS_INODELOCK_UPDATE,
                                                 name, namesize,
                                                 child_lockh, &dchild, LCK_CR,
                                                 child_part);
            } else {
                        /* For revalidate by fid we always take UPDATE lock */
                        dchild = mds_fid2locked_dentry(obd, &body->fid2, NULL,
                                                       LCK_CR, child_lockh,
                                                       NULL, 0,
                                                       MDS_INODELOCK_UPDATE);
                        LASSERT(dchild);
                        if (IS_ERR(dchild))
                                rc = PTR_ERR(dchild);
            }
            if (rc)
                    GOTO(cleanup, rc);
        } else {
                struct ldlm_lock *granted_lock;
                struct ll_fid child_fid;
                struct ldlm_resource *res;
                DEBUG_REQ(D_DLMTRACE, req, "resent, not enqueuing new locks");
                granted_lock = ldlm_handle2lock(child_lockh);
                LASSERTF(granted_lock != NULL, LPU64"/%u lockh "LPX64"\n",
                         body->fid1.id, body->fid1.generation,
                         child_lockh->cookie);


                res = granted_lock->l_resource;
                child_fid.id = res->lr_name.name[0];
                child_fid.generation = res->lr_name.name[1];
                dchild = mds_fid2dentry(&obd->u.mds, &child_fid, NULL);
                LASSERT(!IS_ERR(dchild));
                LDLM_LOCK_PUT(granted_lock);
        }

        cleanup_phase = 2; /* dchild, dparent, locks */

        if (dchild->d_inode == NULL) {
                intent_set_disposition(rep, DISP_LOOKUP_NEG);
                /* in the intent case, the policy clears this error:
                   the disposition is enough */
                GOTO(cleanup, rc = -ENOENT);
        } else {
                intent_set_disposition(rep, DISP_LOOKUP_POS);
        }

        if (req->rq_repmsg == NULL) {
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
                        if (rc && dchild->d_inode)
                                ldlm_lock_decref(child_lockh, LCK_CR);
                        ldlm_lock_decref(&parent_lockh, LCK_CR);
                        l_dput(dparent);
                }
                l_dput(dchild);
        case 1:
                pop_ctxt(&saved, &obd->obd_lvfs_ctxt, &uc);
        default:
                mds_exit_ucred(&uc, mds);
                if (req->rq_reply_state == NULL) {
                        req->rq_status = rc;
                        lustre_pack_reply(req, 0, NULL, NULL);
                }
        }
        return rc;
}

static int mds_getattr(struct ptlrpc_request *req, int offset)
{
        struct mds_obd *mds = mds_req2mds(req);
        struct obd_device *obd = req->rq_export->exp_obd;
        struct lvfs_run_ctxt saved;
        struct dentry *de;
        struct mds_body *body;
        struct lvfs_ucred uc = {NULL,};
        int rc = 0;
        ENTRY;

        body = lustre_swab_reqbuf(req, offset, sizeof(*body),
                                  lustre_swab_mds_body);
        if (body == NULL)
                RETURN(-EFAULT);

        rc = mds_init_ucred(&uc, req, offset);
        if (rc)
                GOTO(out_ucred, rc);

        push_ctxt(&saved, &obd->obd_lvfs_ctxt, &uc);
        de = mds_fid2dentry(mds, &body->fid1, NULL);
        if (IS_ERR(de)) {
                rc = req->rq_status = PTR_ERR(de);
                GOTO(out_pop, rc);
        }

        rc = mds_getattr_pack_msg(req, de->d_inode, offset);
        if (rc != 0) {
                CERROR("mds_getattr_pack_msg: %d\n", rc);
                GOTO(out_pop, rc);
        }

        req->rq_status = mds_getattr_internal(obd, de, req, body, 0);

        l_dput(de);
        GOTO(out_pop, rc);
out_pop:
        pop_ctxt(&saved, &obd->obd_lvfs_ctxt, &uc);
out_ucred:
        if (req->rq_reply_state == NULL) {
                req->rq_status = rc;
                lustre_pack_reply(req, 0, NULL, NULL);
        }
        mds_exit_ucred(&uc, mds);
        return rc;
}

static int mds_obd_statfs(struct obd_device *obd, struct obd_statfs *osfs,
                          unsigned long max_age)
{
        int rc;

        spin_lock(&obd->obd_osfs_lock);
        rc = fsfilt_statfs(obd, obd->u.obt.obt_sb, max_age);
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

        /* This will trigger a watchdog timeout */
        OBD_FAIL_TIMEOUT(OBD_FAIL_MDS_STATFS_LCW_SLEEP,
                         (MDS_SERVICE_WATCHDOG_TIMEOUT / 1000) + 1);

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

static int mds_sync(struct ptlrpc_request *req, int offset)
{
        struct obd_device *obd = req->rq_export->exp_obd;
        struct mds_obd *mds = &obd->u.mds;
        struct mds_body *body;
        int rc, size = sizeof(*body);
        ENTRY;

        body = lustre_swab_reqbuf(req, 0, sizeof(*body), lustre_swab_mds_body);
        if (body == NULL)
                GOTO(out, rc = -EFAULT);

        rc = lustre_pack_reply(req, 1, &size, NULL);
        if (rc || OBD_FAIL_CHECK(OBD_FAIL_MDS_SYNC_PACK)) {
                CERROR("fsync lustre_pack_reply failed: rc = %d\n", rc);
                GOTO(out, rc);
        }

        if (body->fid1.id == 0) {
                /* a fid of zero is taken to mean "sync whole filesystem" */
                rc = fsfilt_sync(obd, obd->u.obt.obt_sb);
                GOTO(out, rc);
        } else {
                struct dentry *de;

                de = mds_fid2dentry(mds, &body->fid1, NULL);
                if (IS_ERR(de))
                        GOTO(out, rc = PTR_ERR(de));

                /* The file parameter isn't used for anything */
                if (de->d_inode->i_fop && de->d_inode->i_fop->fsync)
                        rc = de->d_inode->i_fop->fsync(NULL, de, 1);
                if (rc == 0) {
                        body = lustre_msg_buf(req->rq_repmsg, 0, sizeof(*body));
                        mds_pack_inode2fid(&body->fid1, de->d_inode);
                        mds_pack_inode2body(body, de->d_inode);
                }

                l_dput(de);
                GOTO(out, rc);
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
static int mds_readpage(struct ptlrpc_request *req, int offset)
{
        struct obd_device *obd = req->rq_export->exp_obd;
        struct mds_obd *mds = &obd->u.mds;
        struct vfsmount *mnt;
        struct dentry *de;
        struct file *file;
        struct mds_body *body, *repbody;
        struct lvfs_run_ctxt saved;
        int rc, size = sizeof(*repbody);
        struct lvfs_ucred uc = {NULL,};
        ENTRY;

        if (OBD_FAIL_CHECK(OBD_FAIL_MDS_READPAGE_PACK))
                RETURN(-ENOMEM);

        rc = lustre_pack_reply(req, 1, &size, NULL);
        if (rc) {
                CERROR("error packing readpage reply: rc %d\n", rc);
                GOTO(out, rc);
        }

        body = lustre_swab_reqbuf(req, offset, sizeof(*body),
                                  lustre_swab_mds_body);
        if (body == NULL)
                GOTO (out, rc = -EFAULT);

        rc = mds_init_ucred(&uc, req, 0);
        if (rc)
                GOTO(out, rc);

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
        mds_exit_ucred(&uc, mds);
        req->rq_status = rc;
        RETURN(0);
}

int mds_reint(struct ptlrpc_request *req, int offset,
              struct lustre_handle *lockh)
{
        struct mds_update_record *rec; /* 116 bytes on the stack?  no sir! */
        int rc;

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
        return rc;
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

static int mds_set_info_rpc(struct obd_export *exp, struct ptlrpc_request *req)
{
        char *key;
        __u32 *val;
        int keylen, rc = 0;
        ENTRY;

        key = lustre_msg_buf(req->rq_reqmsg, 0, 1);
        if (key == NULL) {
                DEBUG_REQ(D_HA, req, "no set_info key");
                RETURN(-EFAULT);
        }
        keylen = req->rq_reqmsg->buflens[0];

        val = lustre_msg_buf(req->rq_reqmsg, 1, sizeof(*val));
        if (val == NULL) {
                DEBUG_REQ(D_HA, req, "no set_info val");
                RETURN(-EFAULT);
        }

        rc = lustre_pack_reply(req, 0, NULL, NULL);
        if (rc)
                RETURN(rc);
        req->rq_repmsg->status = 0;

        if (keylen < strlen("read-only") ||
            memcmp(key, "read-only", keylen) != 0)
                RETURN(-EINVAL);

        if (*val)
                exp->exp_connect_flags |= OBD_CONNECT_RDONLY;
        else
                exp->exp_connect_flags &= ~OBD_CONNECT_RDONLY;

        RETURN(0);
}

static int mds_handle_quotacheck(struct ptlrpc_request *req)
{
        struct obd_quotactl *oqctl;
        int rc;
        ENTRY;

        oqctl = lustre_swab_reqbuf(req, 0, sizeof(*oqctl),
                                   lustre_swab_obd_quotactl);
        if (oqctl == NULL)
                RETURN(-EPROTO);

        rc = lustre_pack_reply(req, 0, NULL, NULL);
        if (rc) {
                CERROR("mds: out of memory while packing quotacheck reply\n");
                RETURN(rc);
        }

        req->rq_status = obd_quotacheck(req->rq_export, oqctl);
        RETURN(0);
}

static int mds_handle_quotactl(struct ptlrpc_request *req)
{
        struct obd_quotactl *oqctl, *repoqc;
        int rc, size = sizeof(*repoqc);
        ENTRY;

        oqctl = lustre_swab_reqbuf(req, 0, sizeof(*oqctl),
                                   lustre_swab_obd_quotactl);
        if (oqctl == NULL)
                RETURN(-EPROTO);

        rc = lustre_pack_reply(req, 1, &size, NULL);
        if (rc)
                RETURN(rc);

        repoqc = lustre_msg_buf(req->rq_repmsg, 0, sizeof(*repoqc));

        req->rq_status = obd_quotactl(req->rq_export, oqctl);
        *repoqc = *oqctl;
        RETURN(0);
}

static int mds_msg_check_version(struct lustre_msg *msg)
{
        int rc;

        /* TODO: enable the below check while really introducing msg version.
         * it's disabled because it will break compatibility with b1_4.
         */
        return (0);

        switch (msg->opc) {
        case MDS_CONNECT:
        case MDS_DISCONNECT:
        case OBD_PING:
                rc = lustre_msg_check_version(msg, LUSTRE_OBD_VERSION);
                if (rc)
                        CERROR("bad opc %u version %08x, expecting %08x\n",
                               msg->opc, msg->version, LUSTRE_OBD_VERSION);
                break;
        case MDS_GETSTATUS:
        case MDS_GETATTR:
        case MDS_GETATTR_NAME:
        case MDS_STATFS:
        case MDS_READPAGE:
        case MDS_REINT:
        case MDS_CLOSE:
        case MDS_DONE_WRITING:
        case MDS_PIN:
        case MDS_SYNC:
        case MDS_GETXATTR:
        case MDS_SETXATTR:
        case MDS_SET_INFO:
        case MDS_QUOTACHECK:
        case MDS_QUOTACTL:
        case QUOTA_DQACQ:
        case QUOTA_DQREL:
                rc = lustre_msg_check_version(msg, LUSTRE_MDS_VERSION);
                if (rc)
                        CERROR("bad opc %u version %08x, expecting %08x\n",
                               msg->opc, msg->version, LUSTRE_MDS_VERSION);
                break;
        case LDLM_ENQUEUE:
        case LDLM_CONVERT:
        case LDLM_BL_CALLBACK:
        case LDLM_CP_CALLBACK:
                rc = lustre_msg_check_version(msg, LUSTRE_DLM_VERSION);
                if (rc)
                        CERROR("bad opc %u version %08x, expecting %08x\n",
                               msg->opc, msg->version, LUSTRE_DLM_VERSION);
                break;
        case OBD_LOG_CANCEL:
        case LLOG_ORIGIN_HANDLE_CREATE:
        case LLOG_ORIGIN_HANDLE_NEXT_BLOCK:
        case LLOG_ORIGIN_HANDLE_PREV_BLOCK:
        case LLOG_ORIGIN_HANDLE_READ_HEADER:
        case LLOG_ORIGIN_HANDLE_CLOSE:
        case LLOG_CATINFO:
                rc = lustre_msg_check_version(msg, LUSTRE_LOG_VERSION);
                if (rc)
                        CERROR("bad opc %u version %08x, expecting %08x\n",
                               msg->opc, msg->version, LUSTRE_LOG_VERSION);
                break;
        default:
                CERROR("MDS unknown opcode %d\n", msg->opc);
                rc = -ENOTSUPP;
        }
        return rc;
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

        rc = mds_msg_check_version(req->rq_reqmsg);
        if (rc) {
                CERROR("MDS drop mal-formed request\n");
                RETURN(rc);
        }

        /* XXX identical to OST */
        if (req->rq_reqmsg->opc != MDS_CONNECT) {
                struct mds_export_data *med;
                int recovering, abort_recovery;

                if (req->rq_export == NULL) {
                        CERROR("operation %d on unconnected MDS from %s\n",
                               req->rq_reqmsg->opc,
                               libcfs_id2str(req->rq_peer));
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

                /* Check for aborted recovery. */
                spin_lock_bh(&obd->obd_processing_task_lock);
                abort_recovery = obd->obd_abort_recovery;
                recovering = obd->obd_recovering;
                spin_unlock_bh(&obd->obd_processing_task_lock);
                if (abort_recovery) {
                        target_abort_recovery(obd);
                } else if (recovering) {
                        rc = mds_filter_recovery_request(req, obd,
                                                         &should_process);
                        if (rc || !should_process)
                                RETURN(rc);
                }
        }

        switch (req->rq_reqmsg->opc) {
        case MDS_CONNECT:
                DEBUG_REQ(D_INODE, req, "connect");
                OBD_FAIL_RETURN(OBD_FAIL_MDS_CONNECT_NET, 0);
                rc = target_handle_connect(req, mds_handle);
                if (!rc) {
                        /* Now that we have an export, set mds. */
                        obd = req->rq_export->exp_obd;
                        mds = mds_req2mds(req);
                }
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
                rc = mds_getattr(req, MDS_REQ_REC_OFF);
                break;

        case MDS_SETXATTR:
                DEBUG_REQ(D_INODE, req, "setxattr");
                OBD_FAIL_RETURN(OBD_FAIL_MDS_SETXATTR_NET, 0);
                rc = mds_setxattr(req);
                break;

        case MDS_GETXATTR:
                DEBUG_REQ(D_INODE, req, "getxattr");
                OBD_FAIL_RETURN(OBD_FAIL_MDS_GETXATTR_NET, 0);
                rc = mds_getxattr(req);
                break;

        case MDS_GETATTR_NAME: {
                struct lustre_handle lockh = { 0 };
                DEBUG_REQ(D_INODE, req, "getattr_name");
                OBD_FAIL_RETURN(OBD_FAIL_MDS_GETATTR_NAME_NET, 0);

                /* If this request gets a reconstructed reply, we won't be
                 * acquiring any new locks in mds_getattr_name, so we don't
                 * want to cancel.
                 */
                rc = mds_getattr_name(MDS_REQ_REC_OFF, req,
                                      MDS_INODELOCK_UPDATE, &lockh);
                /* this non-intent call (from an ioctl) is special */
                req->rq_status = rc;
                if (rc == 0 && lustre_handle_is_used(&lockh))
                        ldlm_lock_decref(&lockh, LCK_CR);
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
                rc = mds_readpage(req, MDS_REQ_REC_OFF);

                if (OBD_FAIL_CHECK_ONCE(OBD_FAIL_MDS_SENDPAGE)) {
                        RETURN(0);
                }

                break;

        case MDS_REINT: {
                __u32 *opcp = lustre_msg_buf(req->rq_reqmsg, MDS_REQ_REC_OFF,
                                             sizeof (*opcp));
                __u32  opc;
                int size[] = { sizeof(struct mds_body), mds->mds_max_mdsize,
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

                rc = mds_reint(req, MDS_REQ_REC_OFF, NULL);
                fail = OBD_FAIL_MDS_REINT_NET_REP;
                break;
        }

        case MDS_CLOSE:
                DEBUG_REQ(D_INODE, req, "close");
                OBD_FAIL_RETURN(OBD_FAIL_MDS_CLOSE_NET, 0);
                rc = mds_close(req, MDS_REQ_REC_OFF);
                break;

        case MDS_DONE_WRITING:
                DEBUG_REQ(D_INODE, req, "done_writing");
                OBD_FAIL_RETURN(OBD_FAIL_MDS_DONE_WRITING_NET, 0);
                rc = mds_done_writing(req, MDS_REQ_REC_OFF);
                break;

        case MDS_PIN:
                DEBUG_REQ(D_INODE, req, "pin");
                OBD_FAIL_RETURN(OBD_FAIL_MDS_PIN_NET, 0);
                rc = mds_pin(req, MDS_REQ_REC_OFF);
                break;

        case MDS_SYNC:
                DEBUG_REQ(D_INODE, req, "sync");
                OBD_FAIL_RETURN(OBD_FAIL_MDS_SYNC_NET, 0);
                rc = mds_sync(req, MDS_REQ_REC_OFF);
                break;

        case MDS_SET_INFO:
                DEBUG_REQ(D_INODE, req, "set_info");
                rc = mds_set_info_rpc(req->rq_export, req);
                break;

        case MDS_QUOTACHECK:
                DEBUG_REQ(D_INODE, req, "quotacheck");
                OBD_FAIL_RETURN(OBD_FAIL_MDS_QUOTACHECK_NET, 0);
                rc = mds_handle_quotacheck(req);
                break;

        case MDS_QUOTACTL:
                DEBUG_REQ(D_INODE, req, "quotactl");
                OBD_FAIL_RETURN(OBD_FAIL_MDS_QUOTACTL_NET, 0);
                rc = mds_handle_quotactl(req);
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
                fail = OBD_FAIL_LDLM_REPLY;
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
        case LLOG_ORIGIN_HANDLE_CREATE:
                DEBUG_REQ(D_INODE, req, "llog_init");
                OBD_FAIL_RETURN(OBD_FAIL_OBD_LOGD_NET, 0);
                rc = llog_origin_handle_create(req);
                break;
        case LLOG_ORIGIN_HANDLE_DESTROY:
                DEBUG_REQ(D_INODE, req, "llog_init");
                OBD_FAIL_RETURN(OBD_FAIL_OBD_LOGD_NET, 0);
                rc = llog_origin_handle_destroy(req);
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

        /* If we're DISCONNECTing, the mds_export_data is already freed */
        if (!rc && req->rq_reqmsg->opc != MDS_DISCONNECT) {
                struct mds_export_data *med = &req->rq_export->exp_mds_data;
                req->rq_repmsg->last_xid =
                        le64_to_cpu(med->med_mcd->mcd_last_xid);

                target_committed_to_req(req);
        }

        EXIT;
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
        struct lr_server_data *lsd = mds->mds_server_data;
        struct lr_server_data *lsd_copy = NULL;
        struct file *filp = mds->mds_rcvd_filp;
        struct lvfs_run_ctxt saved;
        loff_t off = 0;
        int rc;
        ENTRY;

        CDEBUG(D_SUPER, "MDS mount_count is "LPU64", last_transno is "LPU64"\n",
               mds->mds_mount_count, mds->mds_last_transno);

        lsd->lsd_last_transno = cpu_to_le64(mds->mds_last_transno);

        if (!(lsd->lsd_feature_incompat & cpu_to_le32(OBD_INCOMPAT_COMMON_LR))){
                /* Swap to the old mds_server_data format, in case
                   someone wants to revert to a pre-1.6 lustre */
                CDEBUG(D_CONFIG, "writing old last_rcvd format\n");
                /* malloc new struct instead of swap in-place because 
                   we don't have a lock on the last_trasno or mount count -
                   someone may modify it while we're here, and we don't want
                   them to inc the wrong thing. */
                OBD_ALLOC(lsd_copy, sizeof(*lsd_copy));
                if (!lsd_copy) 
                        RETURN(-ENOMEM);
                *lsd_copy = *lsd;
                lsd_copy->lsd_unused = lsd->lsd_last_transno;
                lsd_copy->lsd_last_transno = lsd->lsd_mount_count;
                lsd = lsd_copy;
        }

        push_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
        rc = fsfilt_write_record(obd, filp, lsd, sizeof(*lsd), &off,force_sync);
        pop_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
        if (rc)
                CERROR("error writing MDS server data: rc = %d\n", rc);

        if (lsd_copy) 
                OBD_FREE(lsd_copy, sizeof(*lsd_copy));

        RETURN(rc);
}

static
void fsoptions_to_mds_flags(struct mds_obd *mds, char *options)
{
        char *p = options;

        while (*options) {
                int len;

                while (*p && *p != ',')
                        p++;

                len = p - options;
                if (len == sizeof("user_xattr") - 1 &&
                    memcmp(options, "user_xattr", len) == 0) {
                        mds->mds_fl_user_xattr = 1;
                } else if (len == sizeof("acl") - 1 &&
                         memcmp(options, "acl", len) == 0) {
#ifdef CONFIG_FS_POSIX_ACL
                        mds->mds_fl_acl = 1;
#else
                        CWARN("ignoring unsupported acl mount option\n");
                        memmove(options, p, strlen(p) + 1);
#endif
                }

                options = ++p;
        }
}

/* mount the file system (secretly).  lustre_cfg parameters are:
 * 1 = device
 * 2 = fstype
 * 3 = config name
 * 4 = mount options
 */
static int mds_setup(struct obd_device *obd, obd_count len, void *buf)
{
        struct lprocfs_static_vars lvars;
        struct lustre_cfg* lcfg = buf;
        struct mds_obd *mds = &obd->u.mds;
        struct lustre_mount_info *lmi;
        struct vfsmount *mnt;
        struct obd_uuid uuid;
        __u8 *uuid_ptr;
        char *options, *str, *label;
        char ns_name[48];
        unsigned long page;
        int rc = 0;
        ENTRY;

        /* setup 1:/dev/loop/0 2:ext3 3:mdsA 4:errors=remount-ro,iopen_nopriv */

        CLASSERT(offsetof(struct obd_device, u.obt) ==
                 offsetof(struct obd_device, u.mds.mds_obt));

        if (lcfg->lcfg_bufcount < 3)
                RETURN(rc = -EINVAL);

        if (LUSTRE_CFG_BUFLEN(lcfg, 1) == 0 || LUSTRE_CFG_BUFLEN(lcfg, 2) == 0)
                RETURN(rc = -EINVAL);

        lmi = server_get_mount(obd->obd_name);
        if (lmi) {
                /* We already mounted in lustre_fill_super.
                   lcfg bufs 1, 2, 4 (device, fstype, mount opts) are ignored.*/
                struct lustre_sb_info *lsi = s2lsi(lmi->lmi_sb);
                mnt = lmi->lmi_mnt;
                obd->obd_fsops = fsfilt_get_ops(MT_STR(lsi->lsi_ldd));
        } else {
                /* old path - used by lctl */
                CERROR("Using old MDS mount method\n");
                page = __get_free_page(GFP_KERNEL);
                if (!page)
                        RETURN(-ENOMEM);

                options = (char *)page;
                memset(options, 0, PAGE_SIZE);

                /* here we use "iopen_nopriv" hardcoded, because it affects
                 * MDS utility and the rest of options are passed by mount
                 * options. Probably this should be moved to somewhere else
                 * like startup scripts or lconf. */
                strcpy(options, "iopen_nopriv");

                if (LUSTRE_CFG_BUFLEN(lcfg, 4) > 0 && lustre_cfg_buf(lcfg, 4)) {
                        sprintf(options + strlen(options), ",%s",
                                lustre_cfg_string(lcfg, 4));
                        fsoptions_to_mds_flags(mds, options);
                }
                
                mnt = do_kern_mount(lustre_cfg_string(lcfg, 2), 0,
                                    lustre_cfg_string(lcfg, 1), 
                                    (void *)options);
                free_page(page);
                if (IS_ERR(mnt)) {
                        rc = PTR_ERR(mnt);
                        LCONSOLE_ERROR("Can't mount disk %s (%d)\n",
                                       lustre_cfg_string(lcfg, 1), rc);
                        RETURN(rc);
                }

                obd->obd_fsops = fsfilt_get_ops(lustre_cfg_string(lcfg, 2));
        }
        if (IS_ERR(obd->obd_fsops))
                GOTO(err_put, rc = PTR_ERR(obd->obd_fsops));

        CDEBUG(D_SUPER, "%s: mnt = %p\n", lustre_cfg_string(lcfg, 1), mnt);

        LASSERT(!lvfs_check_rdonly(lvfs_sbdev(mnt->mnt_sb)));

        sema_init(&mds->mds_orphan_recovery_sem, 1);
        sema_init(&mds->mds_epoch_sem, 1);
        spin_lock_init(&mds->mds_transno_lock);
        mds->mds_max_mdsize = sizeof(struct lov_mds_md);
        mds->mds_max_cookiesize = sizeof(struct llog_cookie);
        mds->mds_atime_diff = MAX_ATIME_DIFF;

        sprintf(ns_name, "mds-%s", obd->obd_uuid.uuid);
        obd->obd_namespace = ldlm_namespace_new(ns_name, LDLM_NAMESPACE_SERVER);
        if (obd->obd_namespace == NULL) {
                mds_cleanup(obd);
                GOTO(err_ops, rc = -ENOMEM);
        }
        ldlm_register_intent(obd->obd_namespace, mds_intent_policy);

        rc = mds_fs_setup(obd, mnt);
        if (rc) {
                CERROR("%s: MDS filesystem method init failed: rc = %d\n",
                       obd->obd_name, rc);
                GOTO(err_ns, rc);
        }

        rc = llog_start_commit_thread();
        if (rc < 0)
                GOTO(err_fs, rc);

        if (lcfg->lcfg_bufcount >= 4 && LUSTRE_CFG_BUFLEN(lcfg, 3) > 0) {
                class_uuid_t uuid;

                generate_random_uuid(uuid);
                class_uuid_unparse(uuid, &mds->mds_lov_uuid);

                OBD_ALLOC(mds->mds_profile, LUSTRE_CFG_BUFLEN(lcfg, 3));
                if (mds->mds_profile == NULL)
                        GOTO(err_fs, rc = -ENOMEM);

                strncpy(mds->mds_profile, lustre_cfg_string(lcfg, 3),
                        LUSTRE_CFG_BUFLEN(lcfg, 3));

        }

        ptlrpc_init_client(LDLM_CB_REQUEST_PORTAL, LDLM_CB_REPLY_PORTAL,
                           "mds_ldlm_client", &obd->obd_ldlm_client);
        obd->obd_replayable = 1;

        rc = lquota_setup(quota_interface, obd, lcfg);
        if (rc)
                GOTO(err_fs, rc);

        mds->mds_group_hash = upcall_cache_init(obd->obd_name);
        if (IS_ERR(mds->mds_group_hash)) {
                rc = PTR_ERR(mds->mds_group_hash);
                mds->mds_group_hash = NULL;
                GOTO(err_qctxt, rc);
        }

        /* Don't wait for mds_postrecov trying to clear orphans */
        obd->obd_async_recov = 1;
        rc = mds_postsetup(obd);
        obd->obd_async_recov = 0;
        if (rc)
                GOTO(err_qctxt, rc);

        lprocfs_init_vars(mds, &lvars);
        lprocfs_obd_setup(obd, lvars.obd_vars);

        uuid_ptr = fsfilt_uuid(obd, obd->u.obt.obt_sb);
        if (uuid_ptr != NULL) {
                class_uuid_unparse(uuid_ptr, &uuid);
                str = uuid.uuid;
        } else {
                str = "no UUID";
        }

        label = fsfilt_label(obd, obd->u.obt.obt_sb);
        if (obd->obd_recovering) {
                LCONSOLE_WARN("MDT %s now serving %s (%s%s%s), but will be in "
                              "recovery until %d %s reconnect, or if no clients"
                              " reconnect for %d:%.02d; during that time new "
                              "clients will not be allowed to connect. "
                              "Recovery progress can be monitored by watching "
                              "/proc/fs/lustre/mds/%s/recovery_status.\n",
                              obd->obd_name, lustre_cfg_string(lcfg, 1),
                              label ?: "", label ? "/" : "", str,
                              obd->obd_recoverable_clients,
                              (obd->obd_recoverable_clients == 1)
                              ? "client" : "clients",
                              (int)(OBD_RECOVERY_TIMEOUT / HZ) / 60,
                              (int)(OBD_RECOVERY_TIMEOUT / HZ) % 60,
                              obd->obd_name);
        } else {
                LCONSOLE_INFO("MDT %s now serving %s (%s%s%s) with recovery "
                              "%s\n", obd->obd_name, lustre_cfg_string(lcfg, 1),
                              label ?: "", label ? "/" : "", str,
                              obd->obd_replayable ? "enabled" : "disabled");
        }

        ldlm_timeout = 2;
        ping_evictor_start();

        RETURN(0);

err_qctxt:
        lquota_cleanup(quota_interface, obd);
err_fs:
        /* No extra cleanup needed for llog_init_commit_thread() */
        mds_fs_cleanup(obd);
        upcall_cache_cleanup(mds->mds_group_hash);
        mds->mds_group_hash = NULL;
err_ns:
        ldlm_namespace_free(obd->obd_namespace, 0);
        obd->obd_namespace = NULL;
err_ops:
        fsfilt_put_ops(obd->obd_fsops);
err_put:
        if (lmi) {
                server_put_mount(obd->obd_name, mds->mds_vfsmnt);
        } else {
                /* old method */
                unlock_kernel();
                mntput(mds->mds_vfsmnt);
                lock_kernel();
        }               
        obd->u.obt.obt_sb = NULL;
        return rc;
}

static int mds_lov_clean(struct obd_device *obd)
{
        struct mds_obd *mds = &obd->u.mds;
        struct obd_device *osc = mds->mds_osc_obd;
        ENTRY;

        if (mds->mds_profile) {
                class_del_profile(mds->mds_profile);
                OBD_FREE(mds->mds_profile, strlen(mds->mds_profile) + 1);
                mds->mds_profile = NULL;
        }

        /* There better be a lov */
        if (!osc)
                RETURN(0);
        if (IS_ERR(osc))
                RETURN(PTR_ERR(osc));

        obd_register_observer(osc, NULL);

        /* Give lov our same shutdown flags */
        osc->obd_force = obd->obd_force;
        osc->obd_fail = obd->obd_fail;

        /* Cleanup the lov */
        obd_disconnect(mds->mds_osc_exp);
        class_manual_cleanup(osc);
        mds->mds_osc_exp = NULL;

        RETURN(0);
}

static int mds_postsetup(struct obd_device *obd)
{
        struct mds_obd *mds = &obd->u.mds;
        int rc = 0;
        ENTRY;

        rc = llog_setup(obd, LLOG_CONFIG_ORIG_CTXT, obd, 0, NULL,
                        &llog_lvfs_ops);
        if (rc)
                RETURN(rc);

        rc = llog_setup(obd, LLOG_LOVEA_ORIG_CTXT, obd, 0, NULL,
                        &llog_lvfs_ops);
        if (rc)
                RETURN(rc);

        if (mds->mds_profile) {
                struct lustre_profile *lprof;
                /* The profile defines which osc and mdc to connect to, for a 
                   client.  We reuse that here to figure out the name of the
                   lov to use (and ignore lprof->lp_mdc).
                   The profile was set in the config log with 
                   LCFG_MOUNTOPT profilenm oscnm mdcnm */
                lprof = class_get_profile(mds->mds_profile);
                if (lprof == NULL) {
                        CERROR("No profile found: %s\n", mds->mds_profile);
                        GOTO(err_cleanup, rc = -ENOENT);
                }
                rc = mds_lov_connect(obd, lprof->lp_osc);
                if (rc)
                        GOTO(err_cleanup, rc);
        }

        RETURN(rc);

err_cleanup:
        mds_lov_clean(obd);
        llog_cleanup(llog_get_context(obd, LLOG_CONFIG_ORIG_CTXT));
        llog_cleanup(llog_get_context(obd, LLOG_LOVEA_ORIG_CTXT));
        RETURN(rc);
}

int mds_postrecov(struct obd_device *obd)
{
        int rc;
        ENTRY;

        if (obd->obd_fail)
                RETURN(0);

        LASSERT(!obd->obd_recovering);
        LASSERT(llog_get_context(obd, LLOG_MDS_OST_ORIG_CTXT) != NULL);

        /* FIXME why not put this in the synchronize? */
        /* set nextid first, so we are sure it happens */
        rc = mds_lov_set_nextid(obd);
        if (rc) {
                CERROR("%s: mds_lov_set_nextid failed %d\n",
                       obd->obd_name, rc);
                GOTO(out, rc);
        }

        /* clean PENDING dir */
        rc = mds_cleanup_pending(obd);
        if (rc < 0)
                GOTO(out, rc);

        /* FIXME Does target_finish_recovery really need this to block? */
        /* Notify the LOV, which will in turn call mds_notify for each tgt */
        /* This means that we have to hack obd_notify to think we're obd_set_up
           during mds_lov_connect. */
        obd_notify(obd->u.mds.mds_osc_obd, NULL, 
                   obd->obd_async_recov ? OBD_NOTIFY_SYNC_NONBLOCK :
                   OBD_NOTIFY_SYNC, NULL);

        /* quota recovery */
        lquota_recovery(quota_interface, obd);

out:
        RETURN(rc);
}

/* We need to be able to stop an mds_lov_synchronize */
static int mds_lov_early_clean(struct obd_device *obd)
{
        struct mds_obd *mds = &obd->u.mds;
        struct obd_device *osc = mds->mds_osc_obd;

        if (!osc || (!obd->obd_force && !obd->obd_fail))
                return(0);

        CDEBUG(D_HA, "abort inflight\n");
        return (obd_precleanup(osc, OBD_CLEANUP_EARLY));
}

static int mds_precleanup(struct obd_device *obd, int stage)
{
        int rc = 0;
        ENTRY;

        switch (stage) {
        case OBD_CLEANUP_EXPORTS:
                target_cleanup_recovery(obd);
                mds_lov_early_clean(obd);
                break;
        case OBD_CLEANUP_SELF_EXP:
                mds_lov_disconnect(obd);
                mds_lov_clean(obd);
                llog_cleanup(llog_get_context(obd, LLOG_CONFIG_ORIG_CTXT));
                llog_cleanup(llog_get_context(obd, LLOG_LOVEA_ORIG_CTXT));
                rc = obd_llog_finish(obd, 0);
        }
        RETURN(rc);
}

static int mds_cleanup(struct obd_device *obd)
{
        struct mds_obd *mds = &obd->u.mds;
        lvfs_sbdev_type save_dev;
        int must_put = 0;
        int must_relock = 0;
        ENTRY;

        ping_evictor_stop();

        if (obd->u.obt.obt_sb == NULL)
                RETURN(0);
        save_dev = lvfs_sbdev(obd->u.obt.obt_sb);

        if (mds->mds_osc_exp)
                /* lov export was disconnected by mds_lov_clean;
                   we just need to drop our ref */
                class_export_put(mds->mds_osc_exp);

        lprocfs_obd_cleanup(obd);

        lquota_cleanup(quota_interface, obd);

        mds_update_server_data(obd, 1);
        if (mds->mds_lov_objids != NULL) 
                OBD_FREE(mds->mds_lov_objids, mds->mds_lov_objids_size);
        mds_fs_cleanup(obd);

        upcall_cache_cleanup(mds->mds_group_hash);
        mds->mds_group_hash = NULL;

        must_put = server_put_mount(obd->obd_name, mds->mds_vfsmnt);
        /* must_put is for old method (l_p_m returns non-0 on err) */

        /* We can only unlock kernel if we are in the context of sys_ioctl,
           otherwise we never called lock_kernel */
        if (ll_kernel_locked()) {
                unlock_kernel();
                must_relock++;
        }
        
        if (must_put) 
                /* In case we didn't mount with lustre_get_mount -- old method*/
                mntput(mds->mds_vfsmnt);
        obd->u.obt.obt_sb = NULL;

        ldlm_namespace_free(obd->obd_namespace, obd->obd_force);

        spin_lock_bh(&obd->obd_processing_task_lock);
        if (obd->obd_recovering) {
                target_cancel_recovery_timer(obd);
                obd->obd_recovering = 0;
        }
        spin_unlock_bh(&obd->obd_processing_task_lock);

        lvfs_clear_rdonly(save_dev);

        if (must_relock)
                lock_kernel();

        fsfilt_put_ops(obd->obd_fsops);

        LCONSOLE_INFO("MDT %s has stopped.\n", obd->obd_name);

        RETURN(0);
}

static void fixup_handle_for_resent_req(struct ptlrpc_request *req, int offset,
                                        struct ldlm_lock *new_lock,
                                        struct ldlm_lock **old_lock,
                                        struct lustre_handle *lockh)
{
        struct obd_export *exp = req->rq_export;
        struct obd_device *obd = exp->exp_obd;
        struct ldlm_request *dlmreq =
                lustre_msg_buf(req->rq_reqmsg, offset, sizeof (*dlmreq));
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
                        LDLM_DEBUG(lock, "restoring lock cookie");
                        DEBUG_REQ(D_HA, req, "restoring lock cookie "LPX64,
                                  lockh->cookie);
                        if (old_lock)
                                *old_lock = LDLM_LOCK_GET(lock);
                        l_unlock(&obd->obd_namespace->ns_lock);
                        return;
                }
        }
        l_unlock(&obd->obd_namespace->ns_lock);

        /* If the xid matches, then we know this is a resent request,
         * and allow it. (It's probably an OPEN, for which we don't
         * send a lock */
        if (req->rq_xid ==
            le64_to_cpu(exp->exp_mds_data.med_mcd->mcd_last_xid))
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
        struct ldlm_lock *new_lock = NULL;
        int getattr_part = MDS_INODELOCK_UPDATE;
        int repsize[4] = {sizeof(*rep),
                          sizeof(struct mds_body),
                          mds->mds_max_mdsize};
        int repbufcnt = 3, offset = MDS_REQ_INTENT_REC_OFF;
        int rc;
        ENTRY;

        LASSERT(req != NULL);

        if (req->rq_reqmsg->bufcount <= MDS_REQ_INTENT_IT_OFF) {
                /* No intent was provided */
                int size = sizeof(struct ldlm_reply);
                rc = lustre_pack_reply(req, 1, &size, NULL);
                LASSERT(rc == 0);
                RETURN(0);
        }

        it = lustre_swab_reqbuf(req, MDS_REQ_INTENT_IT_OFF, sizeof(*it),
                                lustre_swab_ldlm_intent);
        if (it == NULL) {
                CERROR("Intent missing\n");
                RETURN(req->rq_status = -EFAULT);
        }

        LDLM_DEBUG(lock, "intent policy, opc: %s", ldlm_it2str(it->opc));

        if ((req->rq_export->exp_connect_flags & OBD_CONNECT_ACL) &&
            (it->opc & (IT_OPEN | IT_GETATTR | IT_LOOKUP)))
                /* we should never allow OBD_CONNECT_ACL if not configured */
                repsize[repbufcnt++] = LUSTRE_POSIX_ACL_MAX_SIZE;
        else if (it->opc & IT_UNLINK)
                repsize[repbufcnt++] = mds->mds_max_cookiesize;

        rc = lustre_pack_reply(req, repbufcnt, repsize, NULL);
        if (rc)
                RETURN(req->rq_status = rc);

        rep = lustre_msg_buf(req->rq_repmsg, 0, sizeof (*rep));
        intent_set_disposition(rep, DISP_IT_EXECD);


        /* execute policy */
        switch ((long)it->opc) {
        case IT_OPEN:
        case IT_CREAT|IT_OPEN:
                fixup_handle_for_resent_req(req, MDS_REQ_INTENT_LOCKREQ_OFF,
                                            lock, NULL, &lockh);
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
                        RETURN(ELDLM_LOCK_ABORTED);
                break;
        case IT_LOOKUP:
                        getattr_part = MDS_INODELOCK_LOOKUP;
        case IT_GETATTR:
                        getattr_part |= MDS_INODELOCK_LOOKUP;
        case IT_READDIR:
                fixup_handle_for_resent_req(req, MDS_REQ_INTENT_LOCKREQ_OFF,
                                            lock, &new_lock, &lockh);

                /* INODEBITS_INTEROP: if this lock was converted from a
                 * plain lock (client does not support inodebits), then
                 * child lock must be taken with both lookup and update
                 * bits set for all operations.
                 */
                if (!(req->rq_export->exp_connect_flags & OBD_CONNECT_IBITS))
                        getattr_part = MDS_INODELOCK_LOOKUP |
                                       MDS_INODELOCK_UPDATE;

                rep->lock_policy_res2 = mds_getattr_name(offset, req,
                                                         getattr_part, &lockh);
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
        if (new_lock == NULL)
                new_lock = ldlm_handle2lock(&lockh);
        if (new_lock == NULL && (flags & LDLM_FL_INTENT_ONLY))
                RETURN(0);

        LASSERTF(new_lock != NULL, "op "LPX64" lockh "LPX64"\n",
                 it->opc, lockh.cookie);

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

static int mdt_setup(struct obd_device *obd, obd_count len, void *buf)
{
        struct mds_obd *mds = &obd->u.mds;
        struct lprocfs_static_vars lvars;
        int rc = 0;
        ENTRY;

        lprocfs_init_vars(mdt, &lvars);
        lprocfs_obd_setup(obd, lvars.obd_vars);

        sema_init(&mds->mds_health_sem, 1);

        mds->mds_service =
                ptlrpc_init_svc(MDS_NBUFS, MDS_BUFSIZE, MDS_MAXREQSIZE,
                                MDS_MAXREPSIZE, MDS_REQUEST_PORTAL,
                                MDC_REPLY_PORTAL, MDS_SERVICE_WATCHDOG_TIMEOUT,
                                mds_handle, LUSTRE_MDS_NAME,
                                obd->obd_proc_entry, NULL, MDT_NUM_THREADS);

        if (!mds->mds_service) {
                CERROR("failed to start service\n");
                GOTO(err_lprocfs, rc = -ENOMEM);
        }

        rc = ptlrpc_start_threads(obd, mds->mds_service, "ll_mdt");
        if (rc)
                GOTO(err_thread, rc);

        mds->mds_setattr_service =
                ptlrpc_init_svc(MDS_NBUFS, MDS_BUFSIZE, MDS_MAXREQSIZE,
                                MDS_MAXREPSIZE, MDS_SETATTR_PORTAL,
                                MDC_REPLY_PORTAL, MDS_SERVICE_WATCHDOG_TIMEOUT,
                                mds_handle, "mds_setattr",
                                obd->obd_proc_entry, NULL, MDT_NUM_THREADS);
        if (!mds->mds_setattr_service) {
                CERROR("failed to start getattr service\n");
                GOTO(err_thread, rc = -ENOMEM);
        }

        rc = ptlrpc_start_threads(obd, mds->mds_setattr_service,
                                  "ll_mdt_attr");
        if (rc)
                GOTO(err_thread2, rc);

        mds->mds_readpage_service =
                ptlrpc_init_svc(MDS_NBUFS, MDS_BUFSIZE, MDS_MAXREQSIZE,
                                MDS_MAXREPSIZE, MDS_READPAGE_PORTAL,
                                MDC_REPLY_PORTAL, MDS_SERVICE_WATCHDOG_TIMEOUT,
                                mds_handle, "mds_readpage",
                                obd->obd_proc_entry, NULL, MDT_NUM_THREADS);
        if (!mds->mds_readpage_service) {
                CERROR("failed to start readpage service\n");
                GOTO(err_thread2, rc = -ENOMEM);
        }

        rc = ptlrpc_start_threads(obd, mds->mds_readpage_service,
                                  "ll_mdt_rdpg");

        if (rc)
                GOTO(err_thread3, rc);

        RETURN(0);

err_thread3:
        ptlrpc_unregister_service(mds->mds_readpage_service);
        mds->mds_readpage_service = NULL;
err_thread2:
        ptlrpc_unregister_service(mds->mds_setattr_service);
        mds->mds_setattr_service = NULL;
err_thread:
        ptlrpc_unregister_service(mds->mds_service);
        mds->mds_service = NULL;
err_lprocfs:
        lprocfs_obd_cleanup(obd);
        return rc;
}

static int mdt_cleanup(struct obd_device *obd)
{
        struct mds_obd *mds = &obd->u.mds;
        ENTRY;

        down(&mds->mds_health_sem);
        ptlrpc_unregister_service(mds->mds_readpage_service);
        ptlrpc_unregister_service(mds->mds_setattr_service);
        ptlrpc_unregister_service(mds->mds_service);
        mds->mds_readpage_service = NULL;
        mds->mds_setattr_service = NULL;
        mds->mds_service = NULL;
        up(&mds->mds_health_sem);

        lprocfs_obd_cleanup(obd);

        RETURN(0);
}

static int mdt_health_check(struct obd_device *obd)
{
        struct mds_obd *mds = &obd->u.mds;
        int rc = 0;

        down(&mds->mds_health_sem);
        rc |= ptlrpc_service_health_check(mds->mds_readpage_service);
        rc |= ptlrpc_service_health_check(mds->mds_setattr_service);
        rc |= ptlrpc_service_health_check(mds->mds_service);
        up(&mds->mds_health_sem);

        /*
         * health_check to return 0 on healthy
         * and 1 on unhealthy.
         */
        if(rc != 0)
                rc = 1;

        return rc;
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

static int mds_health_check(struct obd_device *obd)
{
        struct obd_device_target *odt = &obd->u.obt;
        struct mds_obd *mds = &obd->u.mds;
        int rc = 0;

        if (odt->obt_sb->s_flags & MS_RDONLY)
                rc = 1;

        LASSERT(mds->mds_health_check_filp != NULL);
        rc |= !!lvfs_check_io_health(obd, mds->mds_health_check_filp);

        return rc;
}

struct lvfs_callback_ops mds_lvfs_ops = {
        l_fid2dentry:     mds_lvfs_fid2dentry,
};

/* use obd ops to offer management infrastructure */
static struct obd_ops mds_obd_ops = {
        .o_owner           = THIS_MODULE,
        .o_connect         = mds_connect,
        .o_reconnect       = mds_reconnect,
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
        .o_health_check    = mds_health_check,
};

static struct obd_ops mdt_obd_ops = {
        .o_owner           = THIS_MODULE,
        .o_setup           = mdt_setup,
        .o_cleanup         = mdt_cleanup,
        .o_health_check    = mdt_health_check,
};

quota_interface_t *quota_interface;
quota_interface_t mds_quota_interface;

static int __init mds_init(void)
{
        int rc;
        struct lprocfs_static_vars lvars;

        quota_interface = PORTAL_SYMBOL_GET(mds_quota_interface);
        rc = lquota_init(quota_interface);
        if (rc) {
                if (quota_interface)
                        PORTAL_SYMBOL_PUT(mds_quota_interface);
                return rc;
        }
        init_obd_quota_ops(quota_interface, &mds_obd_ops);
        
        lprocfs_init_vars(mds, &lvars);
        class_register_type(&mds_obd_ops, lvars.module_vars, LUSTRE_MDS_NAME);
        lprocfs_init_vars(mdt, &lvars);
        class_register_type(&mdt_obd_ops, lvars.module_vars, LUSTRE_MDT_NAME);

        return 0;
}

static void /*__exit*/ mds_exit(void)
{
        lquota_exit(quota_interface);
        if (quota_interface)
                PORTAL_SYMBOL_PUT(mds_quota_interface);

        class_unregister_type(LUSTRE_MDS_NAME);
        class_unregister_type(LUSTRE_MDT_NAME);
}

MODULE_AUTHOR("Cluster File Systems, Inc. <info@clusterfs.com>");
MODULE_DESCRIPTION("Lustre Metadata Server (MDS)");
MODULE_LICENSE("GPL");

module_init(mds_init);
module_exit(mds_exit);
