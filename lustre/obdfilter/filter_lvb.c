/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  linux/fs/obdfilter/filter_log.c
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

#define DEBUG_SUBSYSTEM S_FILTER

#include <linux/config.h>
#include <linux/module.h>
#include <linux/version.h>

#include <libcfs/list.h>
#include <linux/obd_class.h>
#include <linux/lustre_dlm.h>

#include "filter_internal.h"

/* Called with res->lr_lvb_sem held */
static int filter_lvbo_init(struct ldlm_resource *res)
{
        int rc = 0;
        struct ost_lvb *lvb = NULL;
        struct obd_device *obd;
        struct dentry *dentry;
        ENTRY;

        LASSERT(res);
        LASSERT(down_trylock(&res->lr_lvb_sem) != 0);

        /* we only want lvb's for object resources */
        /* check for internal locks: these have name[1] != 0 */
        if (res->lr_name.name[1])
                RETURN(0);

        if (res->lr_lvb_data)
                GOTO(out, rc = 0);

        OBD_ALLOC(lvb, sizeof(*lvb));
        if (lvb == NULL)
                GOTO(out, rc = -ENOMEM);

        res->lr_lvb_data = lvb;
        res->lr_lvb_len = sizeof(*lvb);

        obd = res->lr_namespace->ns_lvbp;
        LASSERT(obd != NULL);

        dentry = filter_fid2dentry(obd, NULL, 0, res->lr_name.name[0]);
        if (IS_ERR(dentry))
                GOTO(out, rc = PTR_ERR(dentry));

        if (dentry->d_inode == NULL)
                GOTO(out_dentry, rc = -ENOENT);

        lvb->lvb_size = dentry->d_inode->i_size;
        lvb->lvb_mtime = LTIME_S(dentry->d_inode->i_mtime);
        lvb->lvb_blocks = dentry->d_inode->i_blocks;

        CDEBUG(D_DLMTRACE, "res: "LPU64" initial lvb size: "LPU64", "
               "mtime: "LPU64", blocks: "LPU64"\n",
               res->lr_name.name[0], lvb->lvb_size,
               lvb->lvb_mtime, lvb->lvb_blocks);

 out_dentry:
        f_dput(dentry);
 out:
        /* Don't free lvb data on lookup error */
        return rc;
}

/* This will be called in two ways:
 *
 *   m != NULL : called by the DLM itself after a glimpse callback
 *   m == NULL : called by the filter after a disk write
 *
 *   If 'increase' is true, don't allow values to move backwards.
 */
static int filter_lvbo_update(struct ldlm_resource *res, struct lustre_msg *m,
                              int buf_idx, int increase)
{
        int rc = 0;
        struct ost_lvb *lvb = res->lr_lvb_data;
        struct obd_device *obd;
        struct dentry *dentry;
        ENTRY;

        LASSERT(res);

        /* we only want lvb's for object resources */
        /* check for internal locks: these have name[1] != 0 */
        if (res->lr_name.name[1])
                RETURN(0);

        down(&res->lr_lvb_sem);
        if (lvb == NULL) {
                CERROR("No lvb when running lvbo_update!\n");
                GOTO(out, rc = 0);
        }

        /* Update the LVB from the network message */
        if (m != NULL) {
                struct ost_lvb *new;

                new = lustre_swab_buf(m, buf_idx, sizeof(*new),
                                      lustre_swab_ost_lvb);
                if (new == NULL) {
                        CERROR("lustre_swab_buf failed\n");
                        //GOTO(out, rc = -EPROTO);
                        GOTO(out, rc = 0);
                }
                if (new->lvb_size > lvb->lvb_size || !increase) {
                        CDEBUG(D_DLMTRACE, "res: "LPU64" updating lvb size: "
                               LPU64" -> "LPU64"\n", res->lr_name.name[0],
                               lvb->lvb_size, new->lvb_size);
                        lvb->lvb_size = new->lvb_size;
                }
                if (new->lvb_mtime > lvb->lvb_mtime || !increase) {
                        CDEBUG(D_DLMTRACE, "res: "LPU64" updating lvb mtime: "
                               LPU64" -> "LPU64"\n", res->lr_name.name[0],
                               lvb->lvb_mtime, new->lvb_mtime);
                        lvb->lvb_mtime = new->lvb_mtime;
                }
                if (new->lvb_atime > lvb->lvb_atime || !increase) {
                        CDEBUG(D_DLMTRACE, "res: "LPU64" updating lvb atime: "
                               LPU64" -> "LPU64"\n", res->lr_name.name[0],
                               lvb->lvb_atime, new->lvb_atime);
                        lvb->lvb_atime = new->lvb_atime;
                }
                if (new->lvb_ctime > lvb->lvb_ctime || !increase) {
                        CDEBUG(D_DLMTRACE, "res: "LPU64" updating lvb ctime: "
                               LPU64" -> "LPU64"\n", res->lr_name.name[0],
                               lvb->lvb_ctime, new->lvb_ctime);
                        lvb->lvb_ctime = new->lvb_ctime;
                }
        }

        /* Update the LVB from the disk inode */
        obd = res->lr_namespace->ns_lvbp;
        LASSERT(obd);

        dentry = filter_fid2dentry(obd, NULL, 0, res->lr_name.name[0]);
        if (IS_ERR(dentry))
                GOTO(out, rc = PTR_ERR(dentry));

        if (dentry->d_inode == NULL)
                GOTO(out_dentry, rc = -ENOENT);

        if (dentry->d_inode->i_size > lvb->lvb_size || !increase) {
                CDEBUG(D_DLMTRACE, "res: "LPU64" updating lvb size from disk: "
                       LPU64" -> %llu\n", res->lr_name.name[0],
                       lvb->lvb_size, dentry->d_inode->i_size);
                lvb->lvb_size = dentry->d_inode->i_size;
        }

        if (LTIME_S(dentry->d_inode->i_mtime) > lvb->lvb_mtime || !increase) {
                CDEBUG(D_DLMTRACE, "res: "LPU64" updating lvb mtime from disk: "
                       LPU64" -> %lu\n", res->lr_name.name[0],
                       lvb->lvb_mtime, LTIME_S(dentry->d_inode->i_mtime));
                lvb->lvb_mtime = LTIME_S(dentry->d_inode->i_mtime);
        }
        if (LTIME_S(dentry->d_inode->i_atime) > lvb->lvb_atime || !increase) {
                CDEBUG(D_DLMTRACE, "res: "LPU64" updating lvb atime from disk: "
                       LPU64" -> %lu\n", res->lr_name.name[0],
                       lvb->lvb_atime, LTIME_S(dentry->d_inode->i_atime));
                lvb->lvb_atime = LTIME_S(dentry->d_inode->i_atime);
        }
        if (LTIME_S(dentry->d_inode->i_ctime) > lvb->lvb_ctime || !increase) {
                CDEBUG(D_DLMTRACE, "res: "LPU64" updating lvb ctime from disk: "
                       LPU64" -> %lu\n", res->lr_name.name[0],
                       lvb->lvb_ctime, LTIME_S(dentry->d_inode->i_ctime));
                lvb->lvb_ctime = LTIME_S(dentry->d_inode->i_ctime);
        }
        CDEBUG(D_DLMTRACE, "res: "LPU64" updating lvb blocks from disk: "
               LPU64" -> %lu\n", res->lr_name.name[0],
               lvb->lvb_blocks, dentry->d_inode->i_blocks);
        lvb->lvb_blocks = dentry->d_inode->i_blocks;

out_dentry:
        f_dput(dentry);

out:
        up(&res->lr_lvb_sem);
        return rc;
}

struct ldlm_valblock_ops filter_lvbo = {
        lvbo_init: filter_lvbo_init,
        lvbo_update: filter_lvbo_update
};
