/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  lustre/osd/osd_oi.c
 *  Object Index.
 *
 *  Copyright (c) 2006 Cluster File Systems, Inc.
 *   Author: Nikita Danilov <nikita@clusterfs.com>
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

/* LUSTRE_VERSION_CODE */
#include <linux/lustre_ver.h>
/*
 * struct OBD_{ALLOC,FREE}*()
 * OBD_FAIL_CHECK
 */
#include <linux/obd_support.h>

/* fid_is_local() */
#include <linux/lustre_fid.h>

#include "osd_oi.h"
/* osd_lookup(), struct osd_thread_info */
#include "osd_internal.h"

static struct lu_fid *oi_fid_key(struct osd_thread_info *info,
                                 const struct lu_fid *fid);

static const char osd_oi_dirname[] = "oi";

int osd_oi_init(struct osd_oi *oi, struct dentry *root, struct lu_site *site)
{
        int result;

        oi->oi_dir = osd_open(root, osd_oi_dirname, S_IFDIR);
        if (IS_ERR(oi->oi_dir)) {
                result = PTR_ERR(oi->oi_dir);
                oi->oi_dir = NULL;
        } else {
                result = 0;
                init_rwsem(&oi->oi_lock);
                oi->oi_site = site;
        }
        return result;
}

void osd_oi_fini(struct osd_oi *oi)
{
        if (oi->oi_dir != NULL) {
                dput(oi->oi_dir);
                oi->oi_dir = NULL;
        }
}

void osd_oi_read_lock(struct osd_oi *oi)
{
        down_read(&oi->oi_lock);
}

void osd_oi_read_unlock(struct osd_oi *oi)
{
        up_read(&oi->oi_lock);
}

void osd_oi_write_lock(struct osd_oi *oi)
{
        down_write(&oi->oi_lock);
}

void osd_oi_write_unlock(struct osd_oi *oi)
{
        up_write(&oi->oi_lock);
}

static struct lu_fid *oi_fid_key(struct osd_thread_info *info,
                                 const struct lu_fid *fid)
{
        fid_to_le(&info->oti_fid, fid);
        return &info->oti_fid;
}

/****************************************************************************
 * XXX prototype.
 ****************************************************************************/

/*
 * Locking: requires at least read lock on oi.
 */
int osd_oi_lookup(struct osd_thread_info *info, struct osd_oi *oi,
                  const struct lu_fid *fid, struct osd_inode_id *id)
{
        id->oii_ino = fid_seq(fid);
        id->oii_gen = fid_oid(fid);
        return 0;
}

/*
 * Locking: requires write lock on oi.
 */
int osd_oi_insert(struct osd_thread_info *info, struct osd_oi *oi,
                  const struct lu_fid *fid, const struct osd_inode_id *id)
{
        LASSERT(id->oii_ino == fid_seq(fid));
        LASSERT(id->oii_gen == fid_oid(fid));
        return 0;
}

/*
 * Locking: requires write lock on oi.
 */
int osd_oi_delete(struct osd_thread_info *info,
                  struct osd_oi *oi, const struct lu_fid *fid)
{
        return 0;
}
