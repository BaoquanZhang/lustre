/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) 2002, 2003 Cluster File Systems, Inc.
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
#define DEBUG_SUBSYSTEM S_LOV

#ifdef __KERNEL__
#include <asm/div64.h>
#else
#include <liblustre.h>
#endif

#include <linux/obd_class.h>
#include <linux/obd_lov.h>

#include "lov_internal.h"

/* Merge the lock value block(&lvb) attributes from each of the stripes in a
 * file into a single lvb. It is expected that the caller initializes the
 * current atime, mtime, ctime to avoid regressing a more uptodate time on 
 * the local client.
 *
 * If @kms_only is set then we do not consider the recently seen size (rss)
 * when updating the known minimum size (kms).  Even when merging RSS, we will
 * take the KMS value if it's larger.  This prevents getattr from stomping on
 * dirty cached pages which extend the file size. */
int lov_merge_lvb(struct obd_export *exp, struct lov_stripe_md *lsm,
                  struct ost_lvb *lvb, int kms_only)
{
        struct lov_oinfo *loi;
        __u64 size = 0;
        __u64 blocks = 0;
        __u64 current_mtime = lvb->lvb_mtime;
        __u64 current_atime = lvb->lvb_atime;
        __u64 current_ctime = lvb->lvb_ctime;
        int i;

        LASSERT_SPIN_LOCKED(&lsm->lsm_lock);
#ifdef __KERNEL__
        LASSERT(lsm->lsm_lock_owner == current);
#endif

        for (i = 0, loi = lsm->lsm_oinfo; i < lsm->lsm_stripe_count;
             i++, loi++) {
                obd_size lov_size, tmpsize;

                tmpsize = loi->loi_kms;
                if (kms_only == 0 && loi->loi_lvb.lvb_size > tmpsize)
                        tmpsize = loi->loi_lvb.lvb_size;

                lov_size = lov_stripe_size(lsm, tmpsize, i);
                if (lov_size > size)
                        size = lov_size;
                /* merge blocks, mtime, atime */ 
                blocks += loi->loi_lvb.lvb_blocks;
                if (loi->loi_lvb.lvb_mtime > current_mtime)
                        current_mtime = loi->loi_lvb.lvb_mtime;
                if (loi->loi_lvb.lvb_atime > current_atime)
                        current_atime = loi->loi_lvb.lvb_atime;
                if (loi->loi_lvb.lvb_ctime > current_ctime)
                        current_ctime = loi->loi_lvb.lvb_ctime;
        }

        lvb->lvb_size = size;
        lvb->lvb_blocks = blocks;
        lvb->lvb_mtime = current_mtime; 
        lvb->lvb_atime = current_atime; 
        lvb->lvb_ctime = current_ctime; 
        RETURN(0);
}

/* Must be called under the lov_stripe_lock() */
int lov_adjust_kms(struct obd_export *exp, struct lov_stripe_md *lsm,
                   obd_off size, int shrink)
{
        struct lov_oinfo *loi;
        int stripe = 0;
        __u64 kms;
        ENTRY;

        LASSERT_SPIN_LOCKED(&lsm->lsm_lock);
#ifdef __KERNEL__
        LASSERT(lsm->lsm_lock_owner == current);
#endif

        if (shrink) {
                struct lov_oinfo *loi;
                for (loi = lsm->lsm_oinfo; stripe < lsm->lsm_stripe_count;
                     stripe++, loi++) {
                        kms = lov_size_to_stripe(lsm, size, stripe);
                        CDEBUG(D_INODE,
                               "stripe %d KMS %sing "LPU64"->"LPU64"\n",
                               stripe, kms > loi->loi_kms ? "increas":"shrink",
                               loi->loi_kms, kms);
                        loi->loi_kms = loi->loi_lvb.lvb_size = kms;
                }
                RETURN(0);
        }

        if (size > 0)
                stripe = lov_stripe_number(lsm, size - 1);
        kms = lov_size_to_stripe(lsm, size, stripe);
        loi = &(lsm->lsm_oinfo[stripe]);

        CDEBUG(D_INODE, "stripe %d KMS %sincreasing "LPU64"->"LPU64"\n",
               stripe, kms > loi->loi_kms ? "" : "not ", loi->loi_kms, kms);
        if (kms > loi->loi_kms)
                loi->loi_kms = kms;

        RETURN(0);
}

void lov_merge_attrs(struct obdo *tgt, struct obdo *src, obd_flag valid,
                     struct lov_stripe_md *lsm, int stripeno, int *set)
{
        valid &= src->o_valid;

        if (*set) {
                if (valid & OBD_MD_FLSIZE) {
                        /* this handles sparse files properly */
                        obd_size lov_size;

                        lov_size = lov_stripe_size(lsm, src->o_size, stripeno);
                        if (lov_size > tgt->o_size)
                                tgt->o_size = lov_size;
                }
                if (valid & OBD_MD_FLBLOCKS)
                        tgt->o_blocks += src->o_blocks;
                if (valid & OBD_MD_FLBLKSZ)
                        tgt->o_blksize += src->o_blksize;
                if (valid & OBD_MD_FLCTIME && tgt->o_ctime < src->o_ctime)
                        tgt->o_ctime = src->o_ctime;
                if (valid & OBD_MD_FLMTIME && tgt->o_mtime < src->o_mtime)
                        tgt->o_mtime = src->o_mtime;
        } else {
                memcpy(tgt, src, sizeof(*tgt));
                tgt->o_id = lsm->lsm_object_id;
                if (valid & OBD_MD_FLSIZE)
                        tgt->o_size = lov_stripe_size(lsm,src->o_size,stripeno);
                *set = 1;
        }
}
