/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  lustre/mds/mds_orphan.c
 *
 *  Copyright (c) 2001-2003 Cluster File Systems, Inc.
 *   Author: Peter Braam <braam@clusterfs.com>
 *   Author: Andreas Dilger <adilger@clusterfs.com>
 *   Author: Phil Schwan <phil@clusterfs.com>
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

/* code for handling open unlinked files */

#define DEBUG_SUBSYSTEM S_MDS

#include <linux/config.h>
#include <linux/module.h>
#include <linux/version.h>

#include <portals/list.h>
#include <linux/obd_class.h>
#include <linux/lustre_fsfilt.h>
#include <linux/lustre_commit_confd.h>
#include <linux/lvfs.h>

#include "mds_internal.h"


/* If we are unlinking an open file/dir (i.e. creating an orphan) then
 * we instead link the inode into the PENDING directory until it is
 * finally released.  We can't simply call mds_reint_rename() or some
 * part thereof, because we don't have the inode to check for link
 * count/open status until after it is locked.
 *
 * For lock ordering, we always get the PENDING, then pending_child lock
 * last to avoid deadlocks.
 */

int mds_open_unlink_rename(struct mds_update_record *rec,
                           struct obd_device *obd, struct dentry *dparent,
                           struct dentry *dchild, void **handle)
{
        struct mds_obd *mds = &obd->u.mds;
        struct inode *pending_dir = mds->mds_pending_dir->d_inode;
        struct dentry *pending_child;
        char fidname[LL_FID_NAMELEN];
        int fidlen = 0, rc;
        unsigned mode;
        ENTRY;

        LASSERT(!mds_inode_is_orphan(dchild->d_inode));

        down(&pending_dir->i_sem);
        fidlen = ll_fid2str(fidname, dchild->d_inode->i_ino,
                            dchild->d_inode->i_generation);

        CDEBUG(D_HA, "pending destroy of %dx open file %s = %s\n",
               mds_open_orphan_count(dchild->d_inode), rec->ur_name, fidname);

        pending_child = lookup_one_len(fidname, mds->mds_pending_dir, fidlen);
        if (IS_ERR(pending_child))
                GOTO(out_lock, rc = PTR_ERR(pending_child));

        if (pending_child->d_inode != NULL) {
                CERROR("re-destroying orphan file %s?\n", rec->ur_name);
                LASSERT(pending_child->d_inode == dchild->d_inode);
                GOTO(out_dput, rc = 0);
        }

        /* link() is semanticaly-wrong for S_IFDIR, so we set S_IFREG
         * for linking and return real mode back then -bzzz */
        mode = dchild->d_inode->i_mode;
        dchild->d_inode->i_mode = S_IFREG;
        rc = vfs_link(dchild, pending_dir, pending_child);
        if (rc)
                CERROR("error linking orphan %s to PENDING: rc = %d\n",
                       rec->ur_name, rc);
        else
                mds_inode_set_orphan(dchild->d_inode);

        /* return mode and correct i_nlink if inode is directory */
        LASSERT(dchild->d_inode->i_nlink == 1);
        dchild->d_inode->i_mode = mode;
        if ((mode & S_IFMT) == S_IFDIR) {
                dchild->d_inode->i_nlink++;
                pending_dir->i_nlink++;
        }
        mark_inode_dirty(dchild->d_inode);

out_dput:
        dput(pending_child);
out_lock:
        up(&pending_dir->i_sem);
        RETURN(rc);
}

static int mds_osc_destroy_orphan(struct mds_obd *mds,
                                  struct inode *inode,
                                  struct lov_mds_md *lmm,
                                  int lmm_size,
                                  struct llog_cookie *logcookies,
                                  int log_unlink)
{
        struct lov_stripe_md *lsm = NULL;
        struct obd_trans_info oti = { 0 };
        struct obdo *oa;
        int rc;
        ENTRY;

        if (lmm_size == 0)
                RETURN(0);

        rc = obd_unpackmd(mds->mds_osc_exp, &lsm, lmm, lmm_size);
        if (rc < 0) {
                CERROR("Error unpack md %p\n", lmm);
                RETURN(rc);
        } else {
                LASSERT(rc >= sizeof(*lsm));
                rc = 0;
        }

        oa = obdo_alloc();
        if (oa == NULL)
                GOTO(out_free_memmd, rc = -ENOMEM);
        oa->o_id = lsm->lsm_object_id;
        oa->o_gr = FILTER_GROUP_FIRST_MDS + mds->mds_num;
        oa->o_mode = inode->i_mode & S_IFMT;
        oa->o_valid = OBD_MD_FLID | OBD_MD_FLTYPE | OBD_MD_FLGROUP;

        if (log_unlink && logcookies) {
                oa->o_valid |= OBD_MD_FLCOOKIE;
                oti.oti_logcookies = logcookies;
        }

        rc = obd_destroy(mds->mds_osc_exp, oa, lsm, &oti);
        obdo_free(oa);
        if (rc)
                CDEBUG(D_INODE, "destroy orphan objid 0x"LPX64" on ost error "
                       "%d\n", lsm->lsm_object_id, rc);
out_free_memmd:
        obd_free_memmd(mds->mds_osc_exp, &lsm);
        RETURN(rc);
}

static int mds_unlink_orphan(struct obd_device *obd, struct dentry *dchild,
                             struct inode *inode, struct inode *pending_dir)
{
        struct mds_obd *mds = &obd->u.mds;
        struct lov_mds_md *lmm = NULL;
        struct llog_cookie *logcookies = NULL;
        int lmm_size = 0, log_unlink = 0;
        void *handle = NULL;
        int rc, err;
        ENTRY;

        LASSERT(mds->mds_osc_obd != NULL);

        OBD_ALLOC(lmm, mds->mds_max_mdsize);
        if (lmm == NULL)
                RETURN(-ENOMEM);

        down(&inode->i_sem);
        rc = fsfilt_get_md(obd, inode, lmm, mds->mds_max_mdsize);
        up(&inode->i_sem);

        if (rc < 0) {
                CERROR("Error %d reading eadata for ino %lu\n",
                       rc, inode->i_ino);
                GOTO(out_free_lmm, rc);
        } else if (rc > 0) {
                lmm_size = rc;
                rc = mds_convert_lov_ea(obd, inode, lmm, lmm_size);
                if (rc > 0)
                        lmm_size = rc;
                rc = 0;
        }

        handle = fsfilt_start_log(obd, pending_dir, FSFILT_OP_UNLINK, NULL,
                                  le32_to_cpu(lmm->lmm_stripe_count));
        if (IS_ERR(handle)) {
                rc = PTR_ERR(handle);
                CERROR("error fsfilt_start: %d\n", rc);
                handle = NULL;
                GOTO(out_free_lmm, rc);
        }

        down(&inode->i_sem);
        rc = fsfilt_get_md(obd, inode, lmm, mds->mds_max_mdsize);
        up(&inode->i_sem);

        if (rc < 0) {
                CERROR("Error %d reading eadata for ino %lu\n",
                       rc, inode->i_ino);
                GOTO(out_free_lmm, rc);
        } else if (rc > 0) {
                lmm_size = rc;
                rc = 0;
        }

        if (S_ISDIR(inode->i_mode))
                rc = vfs_rmdir(pending_dir, dchild);
        else
                rc = vfs_unlink(pending_dir, dchild);

        if (rc)
                CERROR("error %d unlinking orphan %*s from PENDING directory\n",
                       rc, dchild->d_name.len, dchild->d_name.name);

        if (!rc && lmm_size) {
                OBD_ALLOC(logcookies, mds->mds_max_cookiesize);
                if (logcookies == NULL)
                        rc = -ENOMEM;
                else if (mds_log_op_unlink(obd, inode, lmm,lmm_size,logcookies,
                                           mds->mds_max_cookiesize) > 0)
                        log_unlink = 1;
        }
        err = fsfilt_commit(obd, mds->mds_sb, pending_dir, handle, 0);
        if (err) {
                CERROR("error committing orphan unlink: %d\n", err);
                if (!rc)
                        rc = err;
        }
        if (!rc) {
                rc = mds_osc_destroy_orphan(mds, inode, lmm, lmm_size,
                                            logcookies, log_unlink);
        }

        if (logcookies != NULL)
                OBD_FREE(logcookies, mds->mds_max_cookiesize);
out_free_lmm:
        OBD_FREE(lmm, mds->mds_max_mdsize);
        RETURN(rc);
}

int mds_cleanup_orphans(struct obd_device *obd)
{
        struct mds_obd *mds = &obd->u.mds;
        struct lvfs_run_ctxt saved;
        struct file *file;
        struct dentry *dchild, *dentry;
        struct vfsmount *mnt;
        struct inode *child_inode, *pending_dir = mds->mds_pending_dir->d_inode;
        struct l_linux_dirent *dirent, *n;
        struct list_head dentry_list;
        char d_name[LL_FID_NAMELEN];
        __u64 i = 0;
        int rc = 0, item = 0, namlen;
        ENTRY;

        push_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
        dentry = dget(mds->mds_pending_dir);
        if (IS_ERR(dentry))
                GOTO(err_pop, rc = PTR_ERR(dentry));
        mnt = mntget(mds->mds_vfsmnt);
        if (IS_ERR(mnt))
                GOTO(err_mntget, rc = PTR_ERR(mnt));

        file = dentry_open(mds->mds_pending_dir, mds->mds_vfsmnt,
                           O_RDONLY | O_LARGEFILE);
        if (IS_ERR(file))
                GOTO(err_pop, rc = PTR_ERR(file));

        INIT_LIST_HEAD(&dentry_list);
        rc = l_readdir(file, &dentry_list);
        filp_close(file, 0);
        if (rc < 0)
                GOTO(err_out, rc);

        list_for_each_entry_safe(dirent, n, &dentry_list, lld_list) {
                i ++;
                list_del(&dirent->lld_list);

                namlen = strlen(dirent->lld_name);
                LASSERT(sizeof(d_name) >= namlen + 1);
                strcpy(d_name, dirent->lld_name);
                OBD_FREE(dirent, sizeof(*dirent));

                CDEBUG(D_INODE, "entry "LPU64" of PENDING DIR: %s\n",
                       i, d_name);

                if (((namlen == 1) && !strcmp(d_name, ".")) ||
                    ((namlen == 2) && !strcmp(d_name, ".."))) {
                        continue;
                }

                down(&pending_dir->i_sem);
                dchild = lookup_one_len(d_name, mds->mds_pending_dir, namlen);
                if (IS_ERR(dchild)) {
                        up(&pending_dir->i_sem);
                        GOTO(err_out, rc = PTR_ERR(dchild));
                }
                if (!dchild->d_inode) {
                        CERROR("orphan %s has been removed\n", d_name);
                        GOTO(next, rc = 0);
                }

                child_inode = dchild->d_inode;
                if (mds_inode_is_orphan(child_inode) &&
                    mds_open_orphan_count(child_inode)) {
                        CWARN("orphan %s was re-opened during recovery\n", d_name);
                        GOTO(next, rc = 0);
                }

                rc = mds_unlink_orphan(obd, dchild, child_inode, pending_dir);
                if (rc == 0) {
                        item ++;
                        CWARN("removed orphan %s from MDS and OST\n", d_name);
                } else {
                        CDEBUG(D_INODE, "removed orphan %s from MDS/OST failed,"
                               " rc = %d\n", d_name, rc);
                        rc = 0;
                }
next:
                l_dput(dchild);
                up(&pending_dir->i_sem);
        }
err_out:
        list_for_each_entry_safe(dirent, n, &dentry_list, lld_list) {
                list_del(&dirent->lld_list);
                OBD_FREE(dirent, sizeof(*dirent));
        }
err_pop:
        pop_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
        if (rc == 0)
                rc = item;
        RETURN(rc);

err_mntget:
        l_dput(mds->mds_pending_dir);
        goto err_pop;
}
